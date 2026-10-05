// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Papyrus VM: runs script assets. Written from the PEX format and the
// measurements in bethconv docs/spikes/papyrus.md; no other VM's code was read.
//
// - Scripts are attached to forms as instances, each with its variables and
//   current state. Classes load on first use through a loader callback.
// - Every call runs on a thread with an explicit frame stack, never native
//   recursion, so a latent native (Utility.Wait) suspends the thread and the
//   scheduler resumes it later. Threads run cooperatively in creation order,
//   each until it finishes, waits or uses its instruction budget, so a run is
//   deterministic.
// - Events start a thread per attached script that handles them.
// - Natives are bound by class and function name; an unbound one logs once
//   and returns None, as does any runtime error, and the script continues.
//   A native can also suspend its thread until a named event (`notify`), with
//   a timeout: how PlayAnimationAndWait waits for an animation's text key.
// - RegisterForSingleUpdate/RegisterForUpdate timers send OnUpdate to the one
//   script that registered.
// - `save` writes instances, threads with their frames, timers and results;
//   `load` restores them into an empty VM (vm/save.cpp).
//
// Dispatch: an object value carries the class it is seen as. A method is
// looked up in the attached script whose class derives from that one (so a
// script's override wins), in its current state and then its default state,
// up the class chain.
#pragma once

#include "vm/script_class.hpp"
#include "vm/value.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skydot::vm {

struct Instance {
    const ScriptClass* cls = nullptr;
    std::uint32_t form = 0;
    /// Variables per class in the chain, most derived first.
    std::vector<std::pair<const ScriptClass*, std::vector<Value>>> vars;

    std::vector<Value>* vars_of(const ScriptClass* c);
    /// Variable `index` of class `c`, or null if the instance has no list for
    /// `c` or the list is shorter. Every class in the instance's chain has
    /// one (attach), so for those it is null only for a bad index.
    Value* variable(const ScriptClass* c, std::size_t index);

    /// The current state as a script last named it ("" is the default state),
    /// which `GetState` gives back, and in lowercase, which lookups use.
    [[nodiscard]] const std::string& state() const noexcept { return state_.s(); }
    [[nodiscard]] Name state_key() const noexcept { return {state_key_, state_hash_}; }
    [[nodiscard]] const Value& state_value() const noexcept { return state_; }
    void set_state(std::string name);

private:
    Value state_ = default_of(TypeKind::string);
    std::string state_key_;
    std::size_t state_hash_ = name_hash("");
};

struct Frame {
    const Function* fn = nullptr;
    Instance* instance = nullptr; ///< Null for globals and engine types.
    Value self;
    std::vector<Value> regs;
    std::uint32_t pc = 0;
    /// Argument index of the caller's instruction that receives the result,
    /// or -1.
    int result_arg = -1;
};

struct Thread {
    std::uint64_t id = 0;
    std::vector<Frame> frames;
    double wake_at = 0.0;
    /// A named event the thread waits for (lowercase); `wake_at` is the timeout.
    std::string wait_for;
    bool done = false;
    bool keep_result = true; ///< False for events and setters.
    Value result;
};

class Vm;

struct NativeCall {
    Vm& vm;
    Thread& thread;
    Value self;
    std::vector<Value>& args;
};

struct NativeResult {
    Value value;
    /// Seconds to suspend the calling thread for; negative for none.
    double wait = -1.0;
    /// With `wait` as the timeout: suspend until `Vm::notify` names this.
    std::string wait_for = {};
};

using Native = std::function<NativeResult(NativeCall&)>;

class Vm {
public:
    using Loader = std::function<std::optional<std::vector<std::uint8_t>>(const std::string& key)>;
    using Log = std::function<void(const std::string&)>;

    void set_loader(Loader loader) { loader_ = std::move(loader); }
    void set_log(Log log) { log_ = std::move(log); }

    /// Bind `cls.fn` (any case). Replaces an earlier binding.
    void bind(std::string_view cls, std::string_view fn, Native native);

    /// The class named `name` with its parents, loading them if needed; null
    /// if it or a parent is missing or invalid (logged once).
    const ScriptClass* load_class(std::string_view name);

    /// Attach a new instance of `class_name` to `form` with its variables at
    /// their initial values and its auto state. Null if the class does not
    /// load or is an engine type.
    Instance* attach(std::uint32_t form, std::string_view class_name);
    [[nodiscard]] std::vector<Instance*> instances(std::uint32_t form) const;
    /// The attached instance of exactly this class, or null.
    [[nodiscard]] Instance* instance(std::uint32_t form, std::string_view class_name) const;

    /// Set a property the way VMAD does: an auto property's variable
    /// directly, otherwise through its setter (on a new thread). False if
    /// there is no such property.
    bool set_property(Instance* instance, std::string_view name, Value value);
    [[nodiscard]] Value get_variable(Instance* instance, std::string_view name) const;

    /// Start a thread per attached script that handles `event`. Returns how
    /// many started.
    std::size_t send_event(std::uint32_t form, std::string_view event, std::vector<Value> args);
    /// Start a thread calling a global function, or a method on an instance.
    /// Returns its id, or 0 if there is no such function.
    std::uint64_t call_global(std::string_view cls, std::string_view fn, std::vector<Value> args);
    std::uint64_t call_method(Instance* instance, std::string_view fn, std::vector<Value> args);

    /// Advance the clock by `seconds` and run every thread that is ready.
    void update(double seconds);
    [[nodiscard]] double time() const noexcept { return time_; }
    [[nodiscard]] std::size_t thread_count() const noexcept { return threads_.size(); }
    /// A finished thread's return value (kept until taken).
    [[nodiscard]] std::optional<Value> take_result(std::uint64_t thread);
    [[nodiscard]] std::uint64_t instructions() const noexcept { return instructions_; }
    [[nodiscard]] std::uint64_t errors() const noexcept { return errors_; }

    /// Instructions a thread may run per update before yielding.
    void set_budget(std::uint64_t budget) { budget_ = budget; }

    /// Wake the threads waiting for `key` (any case).
    void notify(std::string_view key);
    /// Send OnUpdate to `instance` after `delay` seconds, and every `interval`
    /// seconds after that if positive. Replaces its earlier registration.
    void register_update(Instance* instance, double delay, double interval);
    void unregister_update(Instance* instance);
    /// The VM's random numbers; seeded, so runs repeat.
    std::uint32_t random();

    /// Everything the scripts would need to carry on, as bytes.
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    /// Replace all state with a save. On failure the VM is left empty and
    /// `error` says why.
    bool load(const std::vector<std::uint8_t>& bytes, std::string& error);
    /// Drop all instances, threads, timers and results; keep classes and natives.
    void clear();

    /// Papyrus conversions.
    [[nodiscard]] static Value default_for(std::string_view type);
    [[nodiscard]] Value cast(const Value& v, std::string_view type);
    [[nodiscard]] static std::string to_string(const Value& v);
    /// `to_string(v)` appended to `out`.
    static void append_text(std::string& out, const Value& v);
    [[nodiscard]] static bool truthy(const Value& v);
    /// An object value for `form` seen as class `cls` (any case).
    [[nodiscard]] Value object(std::uint32_t form, std::string_view cls);

    void error(const std::string& message);

private:
    struct Found {
        const Function* fn = nullptr;
        Instance* instance = nullptr;
    };
    /// `name` for an object seen as `cls`: in an attached script derived from
    /// `cls`, else in `cls`'s own chain.
    Found find_method(const Value& object, const Name& name);
    /// The class `name` (any case), loading it if needed.
    const ScriptClass* class_named(std::string_view name);
    /// The instances attached to `form`, in attach order.
    const std::vector<std::unique_ptr<Instance>>& attached(std::uint32_t form) const;
    /// `v` as a value of the declared type `type`, whose kind is `kind`.
    Value convert(const Value& v, TypeKind kind, std::string_view type);
    /// Look `name` up from class `start` upward, in `state` (lowercase) then the default.
    static const Function* find_in_chain(const ScriptClass* start, const Name& state, const Name& name);

    std::uint64_t start(Found found, Value self, std::vector<Value> args, bool keep_result);
    void fire_timers();
    /// Push a frame for `fn`; returns false (logged) if the call cannot run.
    bool push(Thread& thread, const Function* fn, Instance* instance, Value self,
              std::vector<Value> args, int result_arg);
    /// The same, its arguments being the top frame's current instruction's
    /// from argument `first` on.
    bool push_call(Thread& thread, const Function* fn, Instance* instance, Value self, std::uint32_t first,
                   int result_arg);
    /// The binding of native `fn`, or null if there is none yet.
    const Native* native_of(const Function* fn);
    void run(Thread& thread);
    /// Execute one instruction of the top frame. False once the thread must
    /// stop for this update (it finished or waits).
    bool step(Thread& thread);
    void finish_frame(Thread& thread, Value result);
    bool call_native(Thread& thread, const Function* fn, Value self, std::vector<Value> args,
                     int result_arg);

    /// The value of argument `arg` of the frame's instruction, where it lives:
    /// valid until the frame or the value it reads changes.
    const Value& peek(Frame& frame, std::uint32_t arg);
    const Value& peek_other(Frame& frame, std::uint32_t arg);
    void write(Frame& frame, std::uint32_t arg, Value value);
    void write_other(Frame& frame, std::uint32_t arg, Value value);
    [[nodiscard]] std::string_view type_of(const Frame& frame, std::uint32_t arg) const;
    [[nodiscard]] std::string where(const Frame& frame) const;

    Loader loader_;
    Log log_;
    std::map<std::string, std::unique_ptr<ScriptClass>, std::less<>> classes_;
    std::set<std::string, std::less<>> missing_;
    std::unordered_map<std::string, Native> natives_;
    std::unordered_map<const Function*, const Native*> resolved_natives_;
    std::set<std::string, std::less<>> unbound_logged_;
    std::unordered_map<std::uint32_t, std::vector<std::unique_ptr<Instance>>> instances_;
    std::vector<std::unique_ptr<Thread>> threads_;
    std::unordered_map<std::uint64_t, Value> results_;
    struct Timer {
        Instance* instance = nullptr;
        double due = 0.0;
        double interval = 0.0;
    };
    std::vector<Timer> timers_;
    std::uint64_t rng_ = 0x9E3779B97F4A7C15ULL;
    std::uint64_t next_thread_ = 1;
    std::uint64_t budget_ = 100000;
    std::uint64_t instructions_ = 0;
    std::uint64_t errors_ = 0;
    double time_ = 0.0;
};

} // namespace skydot::vm
