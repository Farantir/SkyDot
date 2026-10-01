// SPDX-License-Identifier: GPL-3.0-or-later
//
// VM saves. Little-endian, versioned:
//
//   "SKPV" u32 version, f64 time, u64 next thread id, u64 random state
//   instances: u32 count, then per instance form, class, state; then per
//              instance its variables per class level (class, count, values)
//   threads:   u32 count; id, keep_result, wake_at, wait_for, frames (function
//              as class/state/name/handler, instance, self, registers, pc,
//              result argument)
//   results:   u32 count; thread id, value
//   timers:    u32 count; instance, due, interval
//
// A value is its kind and payload; objects name their class and instance
// (index, or 0xFFFFFFFF), arrays an id that is followed by the elements the
// first time it appears, so shared arrays stay shared.
//
// Loading treats the bytes as untrusted: every read is bounds-checked, and a
// frame is only restored if its function exists, its registers match and its
// pc is inside it. Classes load from the pack as usual, so a save made with
// different scripts fails instead of running the wrong code.
#include "vm/vm.hpp"

#include <bit>
#include <cstring>
#include <map>

namespace skydot::vm {

namespace {

constexpr std::uint32_t k_version = 1;
constexpr std::uint32_t k_no_instance = 0xFFFFFFFFu;

class Writer {
public:
    void u8(std::uint8_t v) { out.push_back(v); }
    void u32(std::uint32_t v) {
        for (int s = 0; s < 32; s += 8) {
            out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFFu));
        }
    }
    void u64(std::uint64_t v) {
        for (int s = 0; s < 64; s += 8) {
            out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFFu));
        }
    }
    void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
    void text(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    std::vector<std::uint8_t> out;
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : in_(bytes) {}

    bool u8(std::uint8_t& v) {
        if (!need(1)) {
            return false;
        }
        v = in_[pos_++];
        return true;
    }
    bool u32(std::uint32_t& v) {
        if (!need(4)) {
            return false;
        }
        v = 0;
        for (int s = 0; s < 32; s += 8) {
            v |= static_cast<std::uint32_t>(in_[pos_++]) << s;
        }
        return true;
    }
    bool u64(std::uint64_t& v) {
        if (!need(8)) {
            return false;
        }
        v = 0;
        for (int s = 0; s < 64; s += 8) {
            v |= static_cast<std::uint64_t>(in_[pos_++]) << s;
        }
        return true;
    }
    bool f64(double& v) {
        std::uint64_t bits = 0;
        if (!u64(bits)) {
            return false;
        }
        v = std::bit_cast<double>(bits);
        return true;
    }
    bool text(std::string& s) {
        std::uint32_t n = 0;
        if (!u32(n) || !need(n)) {
            return false;
        }
        s.assign(in_.begin() + static_cast<std::ptrdiff_t>(pos_),
                 in_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
        return true;
    }
    /// A count of items each at least `min_bytes` long; refuses counts the
    /// remaining bytes cannot hold.
    bool count(std::uint32_t& n, std::size_t min_bytes) {
        return u32(n) && static_cast<std::size_t>(n) * min_bytes <= in_.size() - pos_;
    }
    [[nodiscard]] bool at_end() const { return pos_ == in_.size(); }

private:
    bool need(std::size_t n) const { return in_.size() - pos_ >= n; }
    const std::vector<std::uint8_t>& in_;
    std::size_t pos_ = 0;
};

struct SaveContext {
    std::map<const Instance*, std::uint32_t> instance_index;
    std::map<const Array*, std::uint32_t> array_ids;
};

void write_value(Writer& w, SaveContext& ctx, const Value& v) {
    w.u8(static_cast<std::uint8_t>(v.kind));
    switch (v.kind) {
    case Kind::none: break;
    case Kind::integer: w.u32(static_cast<std::uint32_t>(v.i)); break;
    case Kind::floating: w.u32(std::bit_cast<std::uint32_t>(v.f)); break;
    case Kind::boolean: w.u8(v.b ? 1 : 0); break;
    case Kind::string: w.text(v.s); break;
    case Kind::object: {
        w.u32(v.form);
        w.text(v.cls != nullptr ? v.cls->key() : std::string{});
        const auto it = ctx.instance_index.find(v.instance);
        w.u32(it != ctx.instance_index.end() ? it->second : k_no_instance);
        break;
    }
    case Kind::array: {
        const auto [it, fresh] = ctx.array_ids.try_emplace(
            v.array.get(), static_cast<std::uint32_t>(ctx.array_ids.size()));
        w.u32(it->second);
        if (fresh) {
            w.u32(static_cast<std::uint32_t>(v.array->size()));
            for (const auto& e : *v.array) {
                write_value(w, ctx, e);
            }
        }
        break;
    }
    }
}

} // namespace

std::vector<std::uint8_t> Vm::save() const {
    Writer w;
    w.u8('S');
    w.u8('K');
    w.u8('P');
    w.u8('V');
    w.u32(k_version);
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
        w.text(i->state);
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
    return w.out;
}

bool Vm::load(const std::vector<std::uint8_t>& bytes, std::string& error) {
    clear();
    Reader r(bytes);
    std::vector<Instance*> index;
    std::map<std::uint32_t, std::shared_ptr<Array>> arrays;
    const auto fail = [&](std::string why) {
        clear();
        error = std::move(why);
        return false;
    };

    std::function<bool(Value&)> read_value = [&](Value& v) -> bool {
        std::uint8_t kind = 0;
        if (!r.u8(kind) || kind > static_cast<std::uint8_t>(Kind::array)) {
            return false;
        }
        v = Value{};
        v.kind = static_cast<Kind>(kind);
        std::uint32_t n = 0;
        switch (v.kind) {
        case Kind::none: return true;
        case Kind::integer:
            if (!r.u32(n)) {
                return false;
            }
            v.i = static_cast<std::int32_t>(n);
            return true;
        case Kind::floating:
            if (!r.u32(n)) {
                return false;
            }
            v.f = std::bit_cast<float>(n);
            return true;
        case Kind::boolean: {
            std::uint8_t b = 0;
            if (!r.u8(b)) {
                return false;
            }
            v.b = b != 0;
            return true;
        }
        case Kind::string: return r.text(v.s);
        case Kind::object: {
            std::string cls;
            std::uint32_t instance = 0;
            if (!r.u32(v.form) || !r.text(cls) || !r.u32(instance)) {
                return false;
            }
            v.cls = cls.empty() ? nullptr : load_class(cls);
            if (instance != k_no_instance) {
                if (instance >= index.size()) {
                    return false;
                }
                v.instance = index[instance];
            }
            return true;
        }
        case Kind::array: {
            std::uint32_t id = 0;
            if (!r.u32(id)) {
                return false;
            }
            if (const auto it = arrays.find(id); it != arrays.end()) {
                v.array = it->second;
                return true;
            }
            if (id != arrays.size() || !r.count(n, 1)) {
                return false;
            }
            auto array = std::make_shared<Array>(n);
            arrays[id] = array;
            for (auto& e : *array) {
                if (!read_value(e)) {
                    return false;
                }
            }
            v.array = std::move(array);
            return true;
        }
        }
        return false;
    };

    std::uint8_t magic[4] = {};
    std::uint32_t version = 0;
    for (auto& m : magic) {
        if (!r.u8(m)) {
            return fail("not a VM save");
        }
    }
    if (std::memcmp(magic, "SKPV", 4) != 0 || !r.u32(version)) {
        return fail("not a VM save");
    }
    if (version != k_version) {
        return fail("VM save version " + std::to_string(version) + " is not one this engine reads (it reads " +
                    std::to_string(k_version) + ")");
    }
    double time = 0.0;
    std::uint64_t next_thread = 0;
    std::uint64_t rng = 0;
    if (!r.f64(time) || !r.u64(next_thread) || !r.u64(rng)) {
        return fail("truncated VM save");
    }

    std::uint32_t count = 0;
    if (!r.count(count, 12)) {
        return fail("truncated VM save");
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        std::uint32_t form = 0;
        std::string cls;
        std::string state;
        if (!r.u32(form) || !r.text(cls) || !r.text(state)) {
            return fail("truncated VM save");
        }
        auto* instance = attach(form, cls);
        if (instance == nullptr) {
            return fail("the save attaches script " + cls + ", which does not load");
        }
        instance->state = std::move(state);
        index.push_back(instance);
    }
    for (auto* instance : index) {
        std::uint32_t levels = 0;
        if (!r.count(levels, 8)) {
            return fail("truncated VM save");
        }
        for (std::uint32_t l = 0; l < levels; ++l) {
            std::string cls;
            std::uint32_t n = 0;
            if (!r.text(cls) || !r.count(n, 1)) {
                return fail("truncated VM save");
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
        return fail("truncated VM save");
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        auto thread = std::make_unique<Thread>();
        std::uint8_t keep = 0;
        std::uint32_t frames = 0;
        if (!r.u64(thread->id) || !r.u8(keep) || !r.f64(thread->wake_at) || !r.text(thread->wait_for) ||
            !r.count(frames, 16)) {
            return fail("truncated VM save");
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
                return fail("truncated VM save");
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
        threads_.push_back(std::move(thread));
    }

    if (!r.count(count, 9)) {
        return fail("truncated VM save");
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        std::uint64_t id = 0;
        Value v;
        if (!r.u64(id) || !read_value(v)) {
            return fail("truncated VM save");
        }
        results_[id] = std::move(v);
    }
    if (!r.count(count, 20)) {
        return fail("truncated VM save");
    }
    for (std::uint32_t n = 0; n < count; ++n) {
        std::uint32_t instance = 0;
        Timer t;
        if (!r.u32(instance) || !r.f64(t.due) || !r.f64(t.interval) || instance >= index.size()) {
            return fail("truncated VM save");
        }
        t.instance = index[instance];
        timers_.push_back(t);
    }
    if (!r.at_end()) {
        return fail("bytes after the end of the VM save");
    }
    time_ = time;
    next_thread_ = next_thread;
    for (const auto& t : threads_) {
        next_thread_ = std::max(next_thread_, t->id + 1);
    }
    for (const auto& [id, value] : results_) {
        next_thread_ = std::max(next_thread_, id + 1);
    }
    rng_ = rng;
    return true;
}

} // namespace skydot::vm
