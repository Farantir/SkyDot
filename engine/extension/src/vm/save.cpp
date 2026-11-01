// SPDX-License-Identifier: GPL-3.0-or-later
//
// VM state in a save (save/save_file.hpp): one section each, every one with
// its own version starting at 1 and no chain across classes.
//
//   VMST  f64 time, u64 next thread id, u64 random state; instances (u32
//         count, then per instance form, class, state, then per instance its
//         variables per class level: class, count, values); threads (u32
//         count; id, keep_result, wake_at, wait_for, frames: function as
//         class/state/name/handler, instance, self, registers, pc, result
//         argument); results (thread id, value); timers (instance, due,
//         interval); scheduled host calls (due, kind text,
//         three u32 slots; kind "activate": target, activator, source | 0x100 if default only)
//   INVT  u32 owner count; per owner: owner ref, u32 item count, then item
//         form, i64 count each
//   ANIM  u32 count; per reference: ref, u32 length, that many bytes (a
//         behaviour graph's snapshot; opaque to the VM)
//   FLST  u32 list count; per list: list form, u32 count, then the forms
//         added to it, in order
//   AEVT  u32 count; per RegisterForAnimationEvent: instance, sender ref, event
//         (an optional section: saves from before it have none)
//   PLCD  u32 next id, u32 count; per kept reference: id, base, cell, f64 x3
//         position, f64 x3 rotation, f64 scale; version 2 adds u32 count and
//         the released ids (forgotten, free for reuse; u32 each). Version 1
//         has none.
//   DEST  u32 count; per reference: ref, f64 damage, i32 stage (as u32), u8
//         destroyed
//
// A value is its kind and payload; objects name their class and instance
// (index, or 0xFFFFFFFF), arrays an id that is followed by the elements the
// first time it appears, so shared arrays stay shared. An array holds no
// arrays, as in Papyrus; a save that nests them is refused.
//
// Loading is two-phase: `decode` reads every section into a `Vm::Snapshot` of
// new, detached objects, treating the bytes as untrusted (every read is
// bounds-checked, a frame is only restored if its function exists, its
// registers match and its pc is inside it; classes load from the pack as
// usual, so a save made with different scripts fails instead of running the
// wrong code); `commit` swaps it in. A failed decode leaves the VM as it was.
#include "vm/vm.hpp"

#include "save/bytes.hpp"
#include "save/save_file.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string_view>

namespace skydot::vm {

namespace {

using save::ByteReader;
using save::ByteWriter;

constexpr std::uint32_t k_section_version = 1;
constexpr std::uint32_t k_max_animation_state = 1U << 20; ///< Bytes per reference, against corrupt lengths.
constexpr std::uint32_t k_no_instance = 0xFFFFFFFFu;

// Minimum sizes of the records below, which the readers hand to ByteReader::count.
/// A value: kind (u8); the payload of the kinds that have one comes on top.
constexpr std::size_t k_value_min_bytes = 1;
/// An attached script: form (u32), class (u32 length), state (u32 length).
constexpr std::size_t k_instance_min_bytes = 4 + 4 + 4;
/// One class's variables of an instance: class (u32 length), value count (u32).
constexpr std::size_t k_variable_level_min_bytes = 4 + 4;
/// A frame: class, state and function (u32 length each), handler (u8), instance (u32), self (a value),
/// register count (u32), pc (u32), result argument (u32).
constexpr std::size_t k_frame_min_bytes = 4 + 4 + 4 + 1 + 4 + k_value_min_bytes + 4 + 4 + 4;
/// A thread: id (u64), keep-result (u8), wake time (f64), wait-for (u32 length), frame count (u32).
constexpr std::size_t k_thread_min_bytes = 8 + 1 + 8 + 4 + 4;
/// A kept result: thread id (u64), value.
constexpr std::size_t k_result_min_bytes = 8 + k_value_min_bytes;
/// A timer: instance index (u32), due (f64), interval (f64).
constexpr std::size_t k_timer_min_bytes = 4 + 8 + 8;
/// The kind text of a scheduled activation, and the bit of its third slot for default processing only.
constexpr std::string_view k_scheduled_activate = "activate";
constexpr std::uint32_t k_scheduled_default_only = 0x100;
/// A scheduled call: due (f64), kind (u32 length), three arguments (u32 each).
constexpr std::size_t k_scheduled_min_bytes = 8 + 4 + 3 * 4;
/// An INVT owner: owner (u32), item count (u32).
constexpr std::size_t k_inventory_owner_min_bytes = 4 + 4;
/// An INVT item: form (u32), amount (u64).
constexpr std::size_t k_inventory_item_min_bytes = 4 + 8;
/// An ANIM reference: ref (u32), length (u32).
constexpr std::size_t k_animation_ref_min_bytes = 4 + 4;
/// One byte of an ANIM state.
constexpr std::size_t k_animation_byte_min_bytes = 1;
/// A form list: list (u32), form count (u32).
constexpr std::size_t k_form_list_min_bytes = 4 + 4;
/// A form in a list: form (u32).
constexpr std::size_t k_form_list_form_min_bytes = 4;
/// A placed reference: id, base, cell (u32 each), position and rotation (six f64), scale (f64).
constexpr std::size_t k_placed_ref_min_bytes = 3 * 4 + 7 * 8;
/// The version of the PLCD section: 2 adds the released ids.
constexpr std::uint32_t k_placed_section_version = 2;
/// A released reference id: id (u32).
constexpr std::size_t k_released_id_min_bytes = 4;
/// A destruction state: ref (u32), damage (f64), stage (u32), destroyed (u8).
constexpr std::size_t k_destruction_min_bytes = 4 + 8 + 4 + 1;
/// An animation event registration: instance index (u32), sender (u32), event (u32 length).
constexpr std::size_t k_animation_reg_min_bytes = 4 + 4 + 4;

struct SaveContext {
    std::map<const Instance*, std::uint32_t> instance_index;
    std::map<const Array*, std::uint32_t> array_ids;
};

void write_value(ByteWriter& w, SaveContext& ctx, const Value& v) {
    const auto start = w.out.size();
    w.u8(static_cast<std::uint8_t>(v.kind()));
    switch (v.kind()) {
    case Kind::none: break;
    case Kind::integer: w.u32(static_cast<std::uint32_t>(v.i())); break;
    case Kind::floating: w.u32(std::bit_cast<std::uint32_t>(v.f())); break;
    case Kind::boolean: w.u8(v.b() ? 1 : 0); break;
    case Kind::string: w.text(v.s()); break;
    case Kind::object: {
        w.u32(v.form());
        w.text(v.cls() != nullptr ? v.cls()->key() : std::string{});
        const auto it = ctx.instance_index.find(v.instance());
        w.u32(it != ctx.instance_index.end() ? it->second : k_no_instance);
        break;
    }
    case Kind::array: {
        const auto [it, fresh] = ctx.array_ids.try_emplace(
            v.array().get(), static_cast<std::uint32_t>(ctx.array_ids.size()));
        w.u32(it->second);
        if (fresh) {
            w.u32(static_cast<std::uint32_t>(v.array()->size()));
            for (const auto& e : *v.array()) {
                write_value(w, ctx, e);
            }
        }
        break;
    }
    }
    w.check_record(start, k_value_min_bytes);
}

/// The section `tag` of `file`, opened; sets `error` and returns false if it is
/// missing or newer than this engine reads. A section reads versions 1 to `max_version`; `version` gets the one found.
bool open_section(const save::SaveReader& file, const char* tag, ByteReader& out, std::string& error,
                  std::uint32_t max_version = 1, std::uint32_t* version = nullptr) {
    const auto* s = file.find(tag);
    if (s == nullptr) {
        error = std::string("the save lacks its ") + tag + " section";
        return false;
    }
    if (s->version < k_section_version || s->version > max_version) {
        error = std::string("section ") + tag + " has version " + std::to_string(s->version) +
                ", which this engine does not read (it reads " + std::to_string(max_version) + ")";
        return false;
    }
    if (version != nullptr) {
        *version = s->version;
    }
    out = file.open(*s);
    return true;
}

} // namespace

void Vm::save_to(save::SaveWriter& out) const {
    ByteWriter w;
    w.f64(time_);
    w.u64(next_thread_);
    w.u64(rng_);

    // Instances in form order, then attach order, so saves are reproducible.
    std::vector<std::uint32_t> forms;
    for (const auto& [form, list] : instances_) {
        forms.push_back(form);
    }
    std::ranges::sort(forms);
    std::vector<const Instance*> all;
    for (const auto form : forms) {
        for (const auto& i : instances_.at(form)) {
            all.push_back(i.get());
        }
    }
    SaveContext ctx;
    for (std::uint32_t n = 0; n < all.size(); ++n) {
        ctx.instance_index[all[n]] = n;
    }
    w.u32(static_cast<std::uint32_t>(all.size()));
    for (const auto* i : all) {
        const auto start = w.out.size();
        w.u32(i->form);
        w.text(i->cls->key());
        w.text(i->state());
        w.check_record(start, k_instance_min_bytes);
    }
    for (const auto* i : all) {
        w.u32(static_cast<std::uint32_t>(i->vars.size()));
        for (const auto& [owner, values] : i->vars) {
            const auto start = w.out.size();
            w.text(owner->key());
            w.u32(static_cast<std::uint32_t>(values.size()));
            for (const auto& v : values) {
                write_value(w, ctx, v);
            }
            w.check_record(start, k_variable_level_min_bytes);
        }
    }

    std::uint32_t live = 0;
    for (const auto& t : threads_) {
        live += t->done ? 0U : 1U;
    }
    w.u32(live);
    for (const auto& t : threads_) {
        if (t->done) {
            continue;
        }
        const auto thread_start = w.out.size();
        w.u64(t->id);
        w.u8(t->keep_result ? 1U : 0U);
        w.f64(t->wake_at);
        w.text(t->wait_for);
        w.u32(static_cast<std::uint32_t>(t->frames.size()));
        for (const auto& f : t->frames) {
            const auto frame_start = w.out.size();
            w.text(f.fn->owner->key());
            w.text(f.fn->state);
            w.text(f.fn->name);
            w.u8(f.fn->handler);
            const auto it = ctx.instance_index.find(f.instance);
            w.u32(it != ctx.instance_index.end() ? it->second : k_no_instance);
            write_value(w, ctx, f.self);
            w.u32(static_cast<std::uint32_t>(f.regs.size()));
            for (const auto& r : f.regs) {
                write_value(w, ctx, r);
            }
            w.u32(f.pc);
            w.u32(static_cast<std::uint32_t>(f.result_arg));
            w.check_record(frame_start, k_frame_min_bytes);
        }
        w.check_record(thread_start, k_thread_min_bytes);
    }

    std::vector<std::uint64_t> ids;
    for (const auto& [id, value] : results_) {
        ids.push_back(id);
    }
    std::ranges::sort(ids);
    w.u32(static_cast<std::uint32_t>(ids.size()));
    for (const auto id : ids) {
        const auto start = w.out.size();
        w.u64(id);
        write_value(w, ctx, results_.at(id));
        w.check_record(start, k_result_min_bytes);
    }

    // A timer whose instance is not in the save (detached) is dropped.
    w.u32(static_cast<std::uint32_t>(std::ranges::count_if(
        timers_, [&](const auto& t) { return ctx.instance_index.contains(t.instance); })));
    for (const auto& t : timers_) {
        const auto found = ctx.instance_index.find(t.instance);
        if (found == ctx.instance_index.end()) {
            continue;
        }
        const auto start = w.out.size();
        w.u32(found->second);
        w.f64(t.due);
        w.f64(t.interval);
        w.check_record(start, k_timer_min_bytes);
    }
    w.u32(static_cast<std::uint32_t>(scheduled_.size()));
    for (const auto& d : scheduled_) {
        const auto start = w.out.size();
        w.f64(d.due);
        // Each kind keeps its text and three slots.
        switch (kind_of(d.call)) {
        case ScheduledKind::activate: {
            const auto& a = std::get<ScheduledActivate>(d.call);
            w.text(k_scheduled_activate);
            w.u32(a.target);
            w.u32(a.activator);
            w.u32(a.source | (a.default_only ? k_scheduled_default_only : 0U));
            break;
        }
        }
        w.check_record(start, k_scheduled_min_bytes);
    }
    out.add("VMST", k_section_version, w.out);

    ByteWriter inventory;
    inventory.u32(static_cast<std::uint32_t>(inventory_.owners().size()));
    for (const auto& [owner, items] : inventory_.owners()) {
        const auto start = inventory.out.size();
        inventory.u32(owner);
        inventory.u32(static_cast<std::uint32_t>(items.size()));
        for (const auto& [item, count] : items) {
            const auto item_start = inventory.out.size();
            inventory.u32(item);
            inventory.u64(static_cast<std::uint64_t>(count));
            inventory.check_record(item_start, k_inventory_item_min_bytes);
        }
        inventory.check_record(start, k_inventory_owner_min_bytes);
    }
    out.add("INVT", k_section_version, inventory.out);

    ByteWriter animation;
    animation.u32(static_cast<std::uint32_t>(animation_states_.size()));
    for (const auto& [ref, bytes] : animation_states_) {
        const auto start = animation.out.size();
        animation.u32(ref);
        animation.u32(static_cast<std::uint32_t>(bytes.size()));
        animation.bytes(bytes);
        animation.check_record(start, k_animation_ref_min_bytes + bytes.size() * k_animation_byte_min_bytes);
    }
    out.add("ANIM", k_section_version, animation.out);

    ByteWriter lists;
    lists.u32(static_cast<std::uint32_t>(form_lists_.lists().size()));
    for (const auto& [list, added] : form_lists_.lists()) {
        const auto start = lists.out.size();
        lists.u32(list);
        lists.u32(static_cast<std::uint32_t>(added.size()));
        for (const auto form : added) {
            const auto form_start = lists.out.size();
            lists.u32(form);
            lists.check_record(form_start, k_form_list_form_min_bytes);
        }
        lists.check_record(start, k_form_list_min_bytes);
    }
    out.add("FLST", k_section_version, lists.out);

    ByteWriter placed;
    std::uint32_t kept = 0;
    for (const auto& [id, ref] : placed_.refs()) {
        kept += ref.transient ? 0U : 1U;
    }
    placed.u32(placed_.next());
    placed.u32(kept);
    for (const auto& [id, ref] : placed_.refs()) {
        if (ref.transient) {
            continue;
        }
        const auto start = placed.out.size();
        placed.u32(id);
        placed.u32(ref.base);
        placed.u32(ref.cell);
        for (const double v : ref.position) {
            placed.f64(v);
        }
        for (const double v : ref.rotation) {
            placed.f64(v);
        }
        placed.f64(ref.scale);
        placed.check_record(start, k_placed_ref_min_bytes);
    }
    placed.u32(static_cast<std::uint32_t>(placed_.released().size()));
    for (const auto id : placed_.released()) {
        const auto start = placed.out.size();
        placed.u32(id);
        placed.check_record(start, k_released_id_min_bytes);
    }
    out.add("PLCD", k_placed_section_version, placed.out);

    ByteWriter destruction;
    destruction.u32(static_cast<std::uint32_t>(destruction_.states().size()));
    for (const auto& [ref, state] : destruction_.states()) {
        const auto start = destruction.out.size();
        destruction.u32(ref);
        destruction.f64(state.damage);
        destruction.u32(static_cast<std::uint32_t>(state.stage));
        destruction.u8(state.destroyed ? 1 : 0);
        destruction.check_record(start, k_destruction_min_bytes);
    }
    out.add("DEST", k_section_version, destruction.out);

    ByteWriter animation_events;
    animation_events.u32(static_cast<std::uint32_t>(std::ranges::count_if(
        animation_regs_, [&](const auto& r) { return ctx.instance_index.contains(r.instance); })));
    for (const auto& r : animation_regs_) {
        const auto found = ctx.instance_index.find(r.instance);
        if (found == ctx.instance_index.end()) {
            continue;
        }
        const auto start = animation_events.out.size();
        animation_events.u32(found->second);
        animation_events.u32(r.sender);
        animation_events.text(r.event);
        animation_events.check_record(start, k_animation_reg_min_bytes);
    }
    out.add("AEVT", k_section_version, animation_events.out);
}

std::vector<std::uint8_t> Vm::save() const {
    save::SaveWriter w;
    save_to(w);
    return w.finish();
}

bool Vm::load(const std::vector<std::uint8_t>& bytes, std::string& error) {
    save::SaveReader file;
    Snapshot snapshot;
    if (!file.parse(bytes, error) || !decode(file, snapshot, error)) {
        return false;
    }
    commit(std::move(snapshot));
    return true;
}

void Vm::commit(Snapshot&& s) {
    clear();
    instances_ = std::move(s.instances);
    threads_ = std::move(s.threads);
    results_ = std::move(s.results);
    timers_ = std::move(s.timers);
    animation_regs_ = std::move(s.animation_regs);
    scheduled_ = std::move(s.scheduled);
    inventory_ = std::move(s.inventory);
    animation_states_ = std::move(s.animation_states);
    form_lists_ = std::move(s.form_lists);
    placed_ = std::move(s.placed);
    destruction_ = std::move(s.destruction);
    time_ = s.time;
    rng_ = s.rng;
    next_thread_ = s.next_thread;
    for (const auto& t : threads_) {
        next_thread_ = std::max(next_thread_, t->id + 1);
    }
    for (const auto& [id, value] : results_) {
        next_thread_ = std::max(next_thread_, id + 1);
    }
}

bool Vm::decode(const save::SaveReader& file, Snapshot& out, std::string& error) {
    ByteReader r({}, 0);
    if (!open_section(file, "VMST", r, error)) {
        return false;
    }
    std::vector<Instance*> index;
    std::map<std::uint32_t, std::shared_ptr<Array>> arrays;
    const auto fail = [&](std::string why) {
        error = std::move(why);
        return false;
    };
    static const std::string truncated = "truncated VM save";
    std::function<bool(Value&)> read_value = [&](Value& v) -> bool {
        std::uint8_t kind = 0;
        if (!r.u8(kind) || kind > static_cast<std::uint8_t>(Kind::array)) {
            return false;
        }
        std::uint32_t n = 0;
        switch (static_cast<Kind>(kind)) {
        case Kind::none: v = Value{}; return true;
        case Kind::integer:
            if (!r.u32(n)) {
                return false;
            }
            v = Value::integer(static_cast<std::int32_t>(n));
            return true;
        case Kind::floating:
            if (!r.u32(n)) {
                return false;
            }
            v = Value::floating(std::bit_cast<float>(n));
            return true;
        case Kind::boolean: {
            std::uint8_t b = 0;
            if (!r.u8(b)) {
                return false;
            }
            v = Value::boolean(b != 0);
            return true;
        }
        case Kind::string: {
            std::string text;
            if (!r.text(text)) {
                return false;
            }
            v = Value::string(std::move(text));
            return true;
        }
        case Kind::object: {
            std::uint32_t form = 0;
            std::string cls;
            std::uint32_t instance = 0;
            if (!r.u32(form) || !r.text(cls) || !r.u32(instance)) {
                return false;
            }
            const ScriptClass* seen_as = cls.empty() ? nullptr : load_class(cls);
            Instance* attached = nullptr;
            if (instance != k_no_instance) {
                if (instance >= index.size()) {
                    return false;
                }
                attached = index[instance];
            }
            v = Value::object(form, seen_as, attached);
            return true;
        }
        case Kind::array: {
            std::uint32_t id = 0;
            if (!r.u32(id)) {
                return false;
            }
            if (const auto it = arrays.find(id); it != arrays.end()) {
                v = Value::make_array(it->second);
                return true;
            }
            if (id != arrays.size() || !r.count(n, k_value_min_bytes)) {
                return false;
            }
            auto array = std::make_shared<Array>(n);
            arrays[id] = array;
            for (auto& e : *array) {
                // Papyrus arrays hold no arrays, so a save has no cycle to
                // follow; one that does (an array holding itself, or two
                // holding each other) would send to_string around forever.
                if (!read_value(e) || e.kind() == Kind::array) {
                    return false;
                }
            }
            v = Value::make_array(std::move(array));
            return true;
        }
        }
        return false;
    };

    double time = 0.0;
    std::uint64_t next_thread = 0;
    std::uint64_t rng = 0;
    if (!r.f64(time) || !r.u64(next_thread) || !r.u64(rng)) {
        return fail(truncated);
    }
    out.time = time;
    out.next_thread = next_thread;
    out.rng = rng;

    std::uint32_t count = 0;
    if (!r.count(count, k_instance_min_bytes)) {
        return fail(truncated);
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        std::uint32_t form = 0;
        std::string cls;
        std::string state;
        if (!r.u32(form) || !r.text(cls) || !r.text(state)) {
            return fail(truncated);
        }
        auto instance = make_instance(form, cls);
        if (instance == nullptr) {
            return fail("the save attaches script " + cls + ", which does not load");
        }
        instance->set_state(std::move(state));
        index.push_back(instance.get());
        out.instances[form].push_back(std::move(instance));
    }
    for (auto* instance : index) {
        std::uint32_t levels = 0;
        if (!r.count(levels, k_variable_level_min_bytes)) {
            return fail(truncated);
        }
        for (std::uint32_t l = 0; l < levels; ++l) {
            std::string cls;
            std::uint32_t n = 0;
            if (!r.text(cls) || !r.count(n, k_value_min_bytes)) {
                return fail(truncated);
            }
            std::vector<Value> values(n);
            for (auto& v : values) {
                if (!read_value(v)) {
                    return fail("bad value in the save");
                }
            }
            // Variables are matched by class; a class whose variables changed
            // keeps its fresh initial values.
            for (auto& [owner, vars] : instance->vars) {
                if (owner->key() == cls && vars.size() == values.size()) {
                    vars = std::move(values);
                    break;
                }
            }
        }
    }

    if (!r.count(count, k_thread_min_bytes)) {
        return fail(truncated);
    }
    std::set<std::uint64_t> seen_threads;
    for (std::uint32_t n = 0; n < count; ++n) {
        auto thread = std::make_unique<Thread>();
        std::uint8_t keep = 0;
        std::uint32_t frames = 0;
        if (!r.u64(thread->id) || !r.u8(keep) || !r.f64(thread->wake_at) || !r.text(thread->wait_for) ||
            !r.count(frames, k_frame_min_bytes)) {
            return fail(truncated);
        }
        if (!seen_threads.insert(thread->id).second) {
            return fail("the save lists a thread id twice");
        }
        thread->keep_result = keep != 0;
        for (std::uint32_t f = 0; f < frames; ++f) {
            std::string cls;
            std::string state;
            std::string name;
            std::uint8_t handler = 0;
            std::uint32_t instance = 0;
            Frame frame;
            if (!r.text(cls) || !r.text(state) || !r.text(name) || !r.u8(handler) || !r.u32(instance) ||
                !read_value(frame.self)) {
                return fail(truncated);
            }
            const auto* owner = load_class(cls);
            if (owner == nullptr) {
                return fail("the save runs script " + cls + ", which does not load");
            }
            if (handler == 0) {
                frame.fn = owner->find(state, name);
            } else if (const auto* p = owner->property(name)) {
                frame.fn = handler == 1 ? p->getter : p->setter;
            }
            if (frame.fn == nullptr || frame.fn->native) {
                return fail("the save runs " + cls + "." + name + ", which the script no longer has");
            }
            if (instance != k_no_instance) {
                if (instance >= index.size()) {
                    return fail("bad instance in the save");
                }
                frame.instance = index[instance];
            }
            std::uint32_t regs = 0;
            if (!r.count(regs, k_value_min_bytes) || regs != frame.fn->reg_names.size()) {
                return fail("the save's frame of " + cls + "." + name + " does not match the script");
            }
            frame.regs.resize(regs);
            for (auto& reg : frame.regs) {
                if (!read_value(reg)) {
                    return fail("bad value in the save");
                }
            }
            std::uint32_t result_arg = 0;
            if (!r.u32(frame.pc) || !r.u32(result_arg) || frame.pc > frame.fn->ops.size()) {
                return fail("the save's frame of " + cls + "." + name + " does not match the script");
            }
            frame.result_arg = static_cast<std::int32_t>(result_arg);
            thread->frames.push_back(std::move(frame));
        }
        // A caller waiting for a result must be at a call whose argument exists.
        for (std::size_t f = 1; f < thread->frames.size(); ++f) {
            const auto& caller = thread->frames[f - 1];
            const int arg = thread->frames[f].result_arg;
            if (caller.pc >= caller.fn->ops.size() ||
                (arg >= 0 && static_cast<std::uint32_t>(arg) >= caller.fn->offsets[caller.pc + 1])) {
                return fail("the save's call stack does not match the scripts");
            }
        }
        if (thread->frames.empty() || thread->frames.front().result_arg != -1) {
            return fail("the save's call stack does not match the scripts");
        }
        out.threads.push_back(std::move(thread));
    }

    if (!r.count(count, k_result_min_bytes)) {
        return fail(truncated);
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        std::uint64_t id = 0;
        Value v;
        if (!r.u64(id) || !read_value(v)) {
            return fail(truncated);
        }
        out.results[id] = std::move(v);
    }
    if (!r.count(count, k_timer_min_bytes)) {
        return fail(truncated);
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        std::uint32_t instance = 0;
        Timer t;
        if (!r.u32(instance) || !r.f64(t.due) || !r.f64(t.interval) || instance >= index.size()) {
            return fail(truncated);
        }
        t.instance = index[instance];
        out.timers.push_back(t);
    }
    if (!r.count(count, k_scheduled_min_bytes)) {
        return fail(truncated);
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        Scheduled d;
        std::string kind;
        std::uint32_t a = 0, b = 0, c = 0;
        if (!r.f64(d.due) || !r.text(kind) || !r.u32(a) || !r.u32(b) || !r.u32(c)) {
            return fail(truncated);
        }
        if (kind == k_scheduled_activate) {
            d.call = ScheduledActivate{a, b, static_cast<std::uint8_t>(c & 0xFFU), (c & k_scheduled_default_only) != 0};
        } else {
            return fail("the save has a scheduled call of unknown kind " + kind);
        }
        out.scheduled.push_back(std::move(d));
    }
    if (!r.at_end()) {
        return fail("bytes after the end of the VMST section");
    }

    if (!open_section(file, "INVT", r, error)) {
        return false;
    }
    std::uint32_t owners = 0;
    if (!r.count(owners, k_inventory_owner_min_bytes)) {
        return fail("truncated INVT section");
    }
    for (std::uint32_t n = 0; n < owners; ++n) {
        std::uint32_t owner = 0, items = 0;
        if (!r.u32(owner) || !r.count(items, k_inventory_item_min_bytes)) {
            return fail("truncated INVT section");
        }
        for (std::uint32_t k = 0; k < items; ++k) {
            std::uint32_t item = 0;
            std::uint64_t amount = 0;
            if (!r.u32(item) || !r.u64(amount)) {
                return fail("truncated INVT section");
            }
            out.inventory.add(owner, item, static_cast<std::int64_t>(amount));
        }
    }
    if (!r.at_end()) {
        return fail("bytes after the end of the INVT section");
    }

    if (!open_section(file, "ANIM", r, error)) {
        return false;
    }
    std::uint32_t refs = 0;
    if (!r.count(refs, k_animation_ref_min_bytes)) {
        return fail("truncated ANIM section");
    }
    for (std::uint32_t n = 0; n < refs; ++n) {
        std::uint32_t ref = 0, length = 0;
        if (!r.u32(ref) || !r.count(length, k_animation_byte_min_bytes) || length > k_max_animation_state) {
            return fail("truncated ANIM section");
        }
        std::vector<std::uint8_t> state(length);
        for (auto& b : state) {
            if (!r.u8(b)) {
                return fail("truncated ANIM section");
            }
        }
        if (!state.empty()) {
            out.animation_states[ref] = std::move(state);
        }
    }
    if (!r.at_end()) {
        return fail("bytes after the end of the ANIM section");
    }

    if (!open_section(file, "FLST", r, error)) {
        return false;
    }
    std::uint32_t lists = 0;
    if (!r.count(lists, k_form_list_min_bytes)) {
        return fail("truncated FLST section");
    }
    for (std::uint32_t n = 0; n < lists; ++n) {
        std::uint32_t list = 0, forms = 0;
        if (!r.u32(list) || !r.count(forms, k_form_list_form_min_bytes)) {
            return fail("truncated FLST section");
        }
        for (std::uint32_t k = 0; k < forms; ++k) {
            std::uint32_t form = 0;
            if (!r.u32(form)) {
                return fail("truncated FLST section");
            }
            out.form_lists.add(list, form);
        }
    }
    if (!r.at_end()) {
        return fail("bytes after the end of the FLST section");
    }

    std::uint32_t placed_version = 0;
    if (!open_section(file, "PLCD", r, error, k_placed_section_version, &placed_version)) {
        return false;
    }
    std::uint32_t next = 0, kept = 0;
    if (!r.u32(next) || !r.count(kept, k_placed_ref_min_bytes)) {
        return fail("truncated PLCD section");
    }
    for (std::uint32_t n = 0; n < kept; ++n) {
        std::uint32_t id = 0;
        PlacedRef ref;
        bool ok = r.u32(id) && r.u32(ref.base) && r.u32(ref.cell);
        for (double& v : ref.position) {
            ok = ok && r.f64(v);
        }
        for (double& v : ref.rotation) {
            ok = ok && r.f64(v);
        }
        ok = ok && r.f64(ref.scale);
        if (!ok) {
            return fail("truncated PLCD section");
        }
        if (!out.placed.restore(id, ref)) {
            return fail("a reference in the PLCD section lies outside the runtime reference range");
        }
    }
    if (!out.placed.set_next(next)) {
        return fail("the PLCD section's next id lies outside the runtime reference range");
    }
    if (placed_version >= 2) {
        std::uint32_t released = 0;
        if (!r.count(released, k_released_id_min_bytes)) {
            return fail("truncated PLCD section");
        }
        for (std::uint32_t n = 0; n < released; ++n) {
            std::uint32_t id = 0;
            if (!r.u32(id)) {
                return fail("truncated PLCD section");
            }
            // Inside the range, below next, not a kept reference, once.
            if (out.placed.find(id) != nullptr || !out.placed.restore_released(id)) {
                return fail("the PLCD section releases a reference id that is kept, repeated or outside the used range");
            }
        }
    }
    if (!r.at_end()) {
        return fail("bytes after the end of the PLCD section");
    }

    if (!open_section(file, "DEST", r, error)) {
        return false;
    }
    std::uint32_t states = 0;
    if (!r.count(states, k_destruction_min_bytes)) {
        return fail("truncated DEST section");
    }
    for (std::uint32_t n = 0; n < states; ++n) {
        std::uint32_t ref = 0, stage = 0;
        std::uint8_t destroyed = 0;
        DestructionState state;
        if (!r.u32(ref) || !r.f64(state.damage) || !r.u32(stage) || !r.u8(destroyed)) {
            return fail("truncated DEST section");
        }
        if (stage > 0x7FFFFFFFU) { // a negative stage would index the model list backwards
            return fail("a destruction stage in the DEST section is negative");
        }
        state.stage = static_cast<std::int32_t>(stage);
        state.destroyed = destroyed != 0;
        out.destruction.set(ref, state);
    }
    if (!r.at_end()) {
        return fail("bytes after the end of the DEST section");
    }

    // Saves from before animation events have no AEVT section.
    if (file.find("AEVT") != nullptr) {
        if (!open_section(file, "AEVT", r, error)) {
            return false;
        }
        std::uint32_t regs = 0;
        if (!r.count(regs, k_animation_reg_min_bytes)) {
            return fail("truncated AEVT section");
        }
        for (std::uint32_t n = 0; n < regs; ++n) {
            AnimationRegistration reg;
            std::uint32_t instance = 0;
            if (!r.u32(instance) || !r.u32(reg.sender) || !r.text(reg.event) || instance >= index.size()) {
                return fail("truncated AEVT section");
            }
            reg.instance = index[instance];
            out.animation_regs.push_back(std::move(reg));
        }
        if (!r.at_end()) {
            return fail("bytes after the end of the AEVT section");
        }
    }
    return true;
}

} // namespace skydot::vm
