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
//         interval); scheduled host calls (due, kind, a, b, c)
//   INVT  u32 owner count; per owner: owner ref, u32 item count, then item
//         form, i64 count each
//   ANIM  u32 count; per reference: ref, u32 length, that many bytes (a
//         behaviour graph's snapshot; opaque to the VM)
//   FLST  u32 list count; per list: list form, u32 count, then the forms
//         added to it, in order
//   PLCD  u32 next id, u32 count; per kept reference: id, base, cell, f64 x3
//         position, f64 x3 rotation, f64 scale
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

namespace skydot::vm {

namespace {

using save::ByteReader;
using save::ByteWriter;

constexpr std::uint32_t k_section_version = 1;
constexpr std::uint32_t k_max_animation_state = 1U << 20; ///< Bytes per reference, against corrupt lengths.
constexpr std::uint32_t k_no_instance = 0xFFFFFFFFu;

struct SaveContext {
    std::map<const Instance*, std::uint32_t> instance_index;
    std::map<const Array*, std::uint32_t> array_ids;
};

void write_value(ByteWriter& w, SaveContext& ctx, const Value& v) {
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
}

/// The section `tag` of `file`, opened; sets `error` and returns false if it is
/// missing or newer than this engine reads.
bool open_section(const save::SaveReader& file, const char* tag, ByteReader& out, std::string& error) {
    const auto* s = file.find(tag);
    if (s == nullptr) {
        error = std::string("the save lacks its ") + tag + " section";
        return false;
    }
    if (s->version != k_section_version) {
        error = std::string("section ") + tag + " has version " + std::to_string(s->version) +
                ", which this engine does not read (it reads " + std::to_string(k_section_version) + ")";
        return false;
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
        w.u32(i->form);
        w.text(i->cls->key());
        w.text(i->state());
    }
    for (const auto* i : all) {
        w.u32(static_cast<std::uint32_t>(i->vars.size()));
        for (const auto& [owner, values] : i->vars) {
            w.text(owner->key());
            w.u32(static_cast<std::uint32_t>(values.size()));
            for (const auto& v : values) {
                write_value(w, ctx, v);
            }
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
        w.u64(t->id);
        w.u8(t->keep_result ? 1U : 0U);
        w.f64(t->wake_at);
        w.text(t->wait_for);
        w.u32(static_cast<std::uint32_t>(t->frames.size()));
        for (const auto& f : t->frames) {
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
        }
    }

    std::vector<std::uint64_t> ids;
    for (const auto& [id, value] : results_) {
        ids.push_back(id);
    }
    std::ranges::sort(ids);
    w.u32(static_cast<std::uint32_t>(ids.size()));
    for (const auto id : ids) {
        w.u64(id);
        write_value(w, ctx, results_.at(id));
    }

    w.u32(static_cast<std::uint32_t>(timers_.size()));
    for (const auto& t : timers_) {
        w.u32(ctx.instance_index.at(t.instance));
        w.f64(t.due);
        w.f64(t.interval);
    }
    w.u32(static_cast<std::uint32_t>(scheduled_.size()));
    for (const auto& d : scheduled_) {
        w.f64(d.due);
        w.text(d.kind);
        w.u32(d.a);
        w.u32(d.b);
        w.u32(d.c);
    }
    out.add("VMST", k_section_version, w.out);

    ByteWriter inventory;
    inventory.u32(static_cast<std::uint32_t>(inventory_.owners().size()));
    for (const auto& [owner, items] : inventory_.owners()) {
        inventory.u32(owner);
        inventory.u32(static_cast<std::uint32_t>(items.size()));
        for (const auto& [item, count] : items) {
            inventory.u32(item);
            inventory.u64(static_cast<std::uint64_t>(count));
        }
    }
    out.add("INVT", k_section_version, inventory.out);

    ByteWriter animation;
    animation.u32(static_cast<std::uint32_t>(animation_states_.size()));
    for (const auto& [ref, bytes] : animation_states_) {
        animation.u32(ref);
        animation.u32(static_cast<std::uint32_t>(bytes.size()));
        animation.bytes(bytes);
    }
    out.add("ANIM", k_section_version, animation.out);

    ByteWriter lists;
    lists.u32(static_cast<std::uint32_t>(form_lists_.lists().size()));
    for (const auto& [list, added] : form_lists_.lists()) {
        lists.u32(list);
        lists.u32(static_cast<std::uint32_t>(added.size()));
        for (const auto form : added) {
            lists.u32(form);
        }
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
    }
    out.add("PLCD", k_section_version, placed.out);

    ByteWriter destruction;
    destruction.u32(static_cast<std::uint32_t>(destruction_.states().size()));
    for (const auto& [ref, state] : destruction_.states()) {
        destruction.u32(ref);
        destruction.f64(state.damage);
        destruction.u32(static_cast<std::uint32_t>(state.stage));
        destruction.u8(state.destroyed ? 1 : 0);
    }
    out.add("DEST", k_section_version, destruction.out);
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
            if (id != arrays.size() || !r.count(n, 1)) {
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
    if (!r.count(count, 12)) {
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
        if (!r.count(levels, 8)) {
            return fail(truncated);
        }
        for (std::uint32_t l = 0; l < levels; ++l) {
            std::string cls;
            std::uint32_t n = 0;
            if (!r.text(cls) || !r.count(n, 1)) {
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

    if (!r.count(count, 32)) {
        return fail(truncated);
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        auto thread = std::make_unique<Thread>();
        std::uint8_t keep = 0;
        std::uint32_t frames = 0;
        if (!r.u64(thread->id) || !r.u8(keep) || !r.f64(thread->wake_at) || !r.text(thread->wait_for) ||
            !r.count(frames, 16)) {
            return fail(truncated);
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
            if (!r.count(regs, 1) || regs != frame.fn->reg_names.size()) {
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

    if (!r.count(count, 9)) {
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
    if (!r.count(count, 20)) {
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
    if (!r.count(count, 24)) {
        return fail(truncated);
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        Scheduled d;
        if (!r.f64(d.due) || !r.text(d.kind) || !r.u32(d.a) || !r.u32(d.b) || !r.u32(d.c)) {
            return fail(truncated);
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
    if (!r.count(owners, 8)) {
        return fail("truncated INVT section");
    }
    for (std::uint32_t n = 0; n < owners; ++n) {
        std::uint32_t owner = 0, items = 0;
        if (!r.u32(owner) || !r.count(items, 12)) {
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
    if (!r.count(refs, 8)) {
        return fail("truncated ANIM section");
    }
    for (std::uint32_t n = 0; n < refs; ++n) {
        std::uint32_t ref = 0, length = 0;
        if (!r.u32(ref) || !r.count(length, 1) || length > k_max_animation_state) {
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
    if (!r.count(lists, 8)) {
        return fail("truncated FLST section");
    }
    for (std::uint32_t n = 0; n < lists; ++n) {
        std::uint32_t list = 0, forms = 0;
        if (!r.u32(list) || !r.count(forms, 4)) {
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

    if (!open_section(file, "PLCD", r, error)) {
        return false;
    }
    std::uint32_t next = 0, kept = 0;
    if (!r.u32(next) || !r.count(kept, 12 + 7 * 8)) {
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
        out.placed.restore(id, ref);
    }
    out.placed.set_next(std::max(next, out.placed.next()));
    if (!r.at_end()) {
        return fail("bytes after the end of the PLCD section");
    }

    if (!open_section(file, "DEST", r, error)) {
        return false;
    }
    std::uint32_t states = 0;
    if (!r.count(states, 17)) {
        return fail("truncated DEST section");
    }
    for (std::uint32_t n = 0; n < states; ++n) {
        std::uint32_t ref = 0, stage = 0;
        std::uint8_t destroyed = 0;
        DestructionState state;
        if (!r.u32(ref) || !r.f64(state.damage) || !r.u32(stage) || !r.u8(destroyed)) {
            return fail("truncated DEST section");
        }
        state.stage = static_cast<std::int32_t>(stage);
        state.destroyed = destroyed != 0;
        out.destruction.set(ref, state);
    }
    if (!r.at_end()) {
        return fail("bytes after the end of the DEST section");
    }
    return true;
}

} // namespace skydot::vm
