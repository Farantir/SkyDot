// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/papyrus.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <numbers>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;
using godot::Vector3;

namespace skydot {

namespace {

std::string utf8(const String& s) {
    const auto bytes = s.utf8();
    return {bytes.get_data(), static_cast<std::size_t>(bytes.length())};
}

String to_godot(const std::string& s) {
    return String::utf8(s.c_str(), static_cast<int>(s.size()));
}

bool scalar_type(std::string_view type) {
    return type == "int" || type == "float" || type == "bool" || type == "string";
}

// XPRM types (world.fbs).
constexpr std::uint32_t k_box = 1;
constexpr std::uint32_t k_sphere = 2;

// GetOpenState values. Source: the Creation Kit wiki's GetOpenState page.
constexpr std::int64_t k_open = 1;
constexpr std::int64_t k_closed = 3;

/// Game hours per real hour (the timescale GMST's default).
constexpr double k_timescale = 20.0;

/// 2 added quests, aliases and globals; 1 is still read.
constexpr std::int32_t k_save_format = 2;

} // namespace

void SkydotPapyrus::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("setup", "pack", "world"), &SkydotPapyrus::setup);
    godot::ClassDB::bind_method(D_METHOD("attach", "cell", "ref"), &SkydotPapyrus::attach);
    godot::ClassDB::bind_method(D_METHOD("attach_built", "root"), &SkydotPapyrus::attach_built);
    godot::ClassDB::bind_method(D_METHOD("attach_cell", "cell"), &SkydotPapyrus::attach_cell);
    godot::ClassDB::bind_method(D_METHOD("activate", "ref", "activator"), &SkydotPapyrus::activate,
                                DEFVAL(PLAYER_REF));
    godot::ClassDB::bind_method(D_METHOD("send_event", "ref", "event", "args"),
                                &SkydotPapyrus::send_event);
    godot::ClassDB::bind_method(D_METHOD("notify_animation_event", "ref", "event"),
                                &SkydotPapyrus::notify_animation_event);
    godot::ClassDB::bind_method(D_METHOD("update_actor", "actor", "position"),
                                &SkydotPapyrus::update_actor);
    godot::ClassDB::bind_method(D_METHOD("update", "seconds"), &SkydotPapyrus::update);
    godot::ClassDB::bind_method(D_METHOD("call_global", "cls", "fn", "args"),
                                &SkydotPapyrus::call_global);
    godot::ClassDB::bind_method(D_METHOD("create_instance", "cls"), &SkydotPapyrus::create_instance);
    godot::ClassDB::bind_method(D_METHOD("call_method", "form", "cls", "fn", "args"),
                                &SkydotPapyrus::call_method);
    godot::ClassDB::bind_method(D_METHOD("take_result", "thread"), &SkydotPapyrus::take_result);
    godot::ClassDB::bind_method(D_METHOD("get_variable", "form", "cls", "name"),
                                &SkydotPapyrus::get_variable);
    godot::ClassDB::bind_method(D_METHOD("get_scripts", "ref"), &SkydotPapyrus::get_scripts);
    godot::ClassDB::bind_method(D_METHOD("is_ref_disabled", "ref"), &SkydotPapyrus::is_ref_disabled);
    godot::ClassDB::bind_method(D_METHOD("get_disabled_changes"),
                                &SkydotPapyrus::get_disabled_changes);
    godot::ClassDB::bind_method(D_METHOD("is_activation_blocked", "ref"),
                                &SkydotPapyrus::is_activation_blocked);
    godot::ClassDB::bind_method(D_METHOD("get_lock_level", "ref"), &SkydotPapyrus::get_lock_level);
    godot::ClassDB::bind_method(D_METHOD("set_locked", "ref", "locked"), &SkydotPapyrus::set_locked);
    godot::ClassDB::bind_method(D_METHOD("get_open_state", "ref"), &SkydotPapyrus::get_open_state);
    godot::ClassDB::bind_method(D_METHOD("set_open_state", "ref", "state"),
                                &SkydotPapyrus::set_open_state);
    godot::ClassDB::bind_method(D_METHOD("get_trigger_count", "ref"),
                                &SkydotPapyrus::get_trigger_count);
    godot::ClassDB::bind_method(D_METHOD("get_triggers"), &SkydotPapyrus::get_triggers);
    godot::ClassDB::bind_method(D_METHOD("start_game_enabled_quests"),
                                &SkydotPapyrus::start_game_enabled_quests);
    godot::ClassDB::bind_method(D_METHOD("start_quest", "quest"), &SkydotPapyrus::start_quest);
    godot::ClassDB::bind_method(D_METHOD("stop_quest", "quest"), &SkydotPapyrus::stop_quest);
    godot::ClassDB::bind_method(D_METHOD("set_stage", "quest", "stage"), &SkydotPapyrus::set_stage);
    godot::ClassDB::bind_method(D_METHOD("get_quest_state", "quest"), &SkydotPapyrus::get_quest_state);
    godot::ClassDB::bind_method(D_METHOD("get_running_quests"), &SkydotPapyrus::get_running_quests);
    godot::ClassDB::bind_method(D_METHOD("get_alias_ref", "quest", "alias"),
                                &SkydotPapyrus::get_alias_ref);
    godot::ClassDB::bind_method(D_METHOD("get_global_value", "global"),
                                &SkydotPapyrus::get_global_value);
    godot::ClassDB::bind_method(D_METHOD("set_global_value", "global", "value"),
                                &SkydotPapyrus::set_global_value);
    godot::ClassDB::bind_method(D_METHOD("save_state"), &SkydotPapyrus::save_state);
    godot::ClassDB::bind_method(D_METHOD("load_state", "bytes"), &SkydotPapyrus::load_state);
    godot::ClassDB::bind_method(D_METHOD("get_last_error"), &SkydotPapyrus::get_last_error);
    godot::ClassDB::bind_method(D_METHOD("get_error_count"), &SkydotPapyrus::get_error_count);
    godot::ClassDB::bind_method(D_METHOD("get_instruction_count"),
                                &SkydotPapyrus::get_instruction_count);
    godot::ClassDB::bind_method(D_METHOD("get_thread_count"), &SkydotPapyrus::get_thread_count);
    godot::ClassDB::bind_method(D_METHOD("get_time"), &SkydotPapyrus::get_time);
    using godot::PropertyInfo;
    ADD_SIGNAL(godot::MethodInfo("trace", PropertyInfo(Variant::STRING, "text")));
    ADD_SIGNAL(godot::MethodInfo("message", PropertyInfo(Variant::STRING, "text"),
                                 PropertyInfo(Variant::BOOL, "box")));
    ADD_SIGNAL(godot::MethodInfo("error", PropertyInfo(Variant::STRING, "text")));
    ADD_SIGNAL(godot::MethodInfo("enable_changed", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::BOOL, "enabled")));
    ADD_SIGNAL(godot::MethodInfo("play_animation", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::STRING, "animation")));
    ADD_SIGNAL(godot::MethodInfo("activate_requested", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::INT, "activator"),
                                 PropertyInfo(Variant::BOOL, "default_only")));
    ADD_SIGNAL(godot::MethodInfo("open_changed", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::BOOL, "open")));
    ADD_SIGNAL(godot::MethodInfo("lock_changed", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::BOOL, "locked")));
    ADD_SIGNAL(godot::MethodInfo("effect_shader", PropertyInfo(Variant::INT, "shader"),
                                 PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::BOOL, "playing")));
    ADD_SIGNAL(godot::MethodInfo("trigger", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::INT, "actor"),
                                 PropertyInfo(Variant::BOOL, "entered")));
    ADD_SIGNAL(godot::MethodInfo("quest_started", PropertyInfo(Variant::INT, "quest")));
    ADD_SIGNAL(godot::MethodInfo("quest_stopped", PropertyInfo(Variant::INT, "quest")));
    ADD_SIGNAL(godot::MethodInfo("quest_stage", PropertyInfo(Variant::INT, "quest"),
                                 PropertyInfo(Variant::INT, "stage"),
                                 PropertyInfo(Variant::STRING, "text")));
    ADD_SIGNAL(godot::MethodInfo("objective_changed", PropertyInfo(Variant::INT, "quest"),
                                 PropertyInfo(Variant::INT, "index"),
                                 PropertyInfo(Variant::STRING, "state"),
                                 PropertyInfo(Variant::STRING, "text")));
    BIND_CONSTANT(PLAYER_REF);
    BIND_CONSTANT(ALIAS_HANDLE_BASE);
}

SkydotPapyrus::SkydotPapyrus() : vm_(std::make_unique<vm::Vm>()) {}

void SkydotPapyrus::reset() {
    attached_.clear();
    built_.clear();
    disabled_.clear();
    blocked_.clear();
    open_.clear();
    locked_.clear();
    triggers_.clear();
    actors_.clear();
    next_form_ = 0xFF000000u;
    quests_.clear();
    quest_scripts_.clear();
    alias_handles_.clear();
    handle_alias_.clear();
    alias_fill_.clear();
    globals_.clear();
    next_alias_handle_ = static_cast<std::uint32_t>(ALIAS_HANDLE_BASE);
    quest_info_.clear();
}

godot::Error SkydotPapyrus::setup(const godot::Ref<SkydotPack>& pack,
                                  const godot::Ref<SkydotWorld>& world) {
    if (pack.is_null() || world.is_null()) {
        return godot::ERR_INVALID_PARAMETER;
    }
    pack_ = pack;
    world_ = world;
    vm_ = std::make_unique<vm::Vm>();
    reset();
    // Scripts keep their virtual paths: class Foo is scripts/foo.pex.
    vm_->set_loader([this](const std::string& key) -> std::optional<std::vector<std::uint8_t>> {
        const auto bytes = pack_->get_bytes(to_godot("scripts/" + key + ".pex"));
        if (bytes.is_empty()) {
            return std::nullopt;
        }
        return std::vector<std::uint8_t>(bytes.ptr(), bytes.ptr() + bytes.size());
    });
    vm_->set_log([this](const std::string& message) {
        godot::UtilityFunctions::push_warning(String("Papyrus: ") + to_godot(message));
        emit_signal("error", to_godot(message));
    });
    bind_natives();
    bind_quest_natives();
    return godot::OK;
}

// ---- the world as scripts see it ----------------------------------------------

Dictionary SkydotPapyrus::ref_info(std::uint32_t ref) const {
    if (world_.is_null()) {
        return {};
    }
    return world_->get_ref_info(world_->get_ref_cell(ref), ref);
}

bool SkydotPapyrus::disabled(std::uint32_t ref, int depth) const {
    if (const auto it = disabled_.find(ref); it != disabled_.end()) {
        return it->second;
    }
    const Dictionary info = ref_info(ref);
    // Follow a script-changed enable parent; otherwise the world's initial state.
    const auto parent = static_cast<std::uint32_t>(static_cast<std::int64_t>(info.get("enable_parent", 0)));
    if (parent != 0 && depth < 16 && world_.is_valid() && world_->get_ref_cell(parent) != 0) {
        return disabled(parent, depth + 1) != static_cast<bool>(info.get("enable_opposite", false));
    }
    return static_cast<bool>(info.get("disabled", false));
}

void SkydotPapyrus::set_disabled(std::uint32_t ref, bool value) {
    // The reference and its enable children, whose state follows it.
    std::vector<std::pair<std::uint32_t, bool>> before{{ref, disabled(ref)}};
    for (std::size_t i = 0; i < before.size() && world_.is_valid() && before.size() < 4096; ++i) {
        for (const std::int64_t child : world_->get_enable_children(before[i].first)) {
            const auto id = static_cast<std::uint32_t>(child);
            if (!disabled_.contains(id)) {
                before.emplace_back(id, disabled(id));
            }
        }
    }
    disabled_[ref] = value;
    for (const auto& [id, was] : before) {
        if (const bool now = disabled(id); now != was) {
            emit_signal("enable_changed", static_cast<std::int64_t>(id), !now);
        }
    }
}

bool SkydotPapyrus::locked(std::uint32_t ref) const {
    if (const auto it = locked_.find(ref); it != locked_.end()) {
        return it->second;
    }
    // A reference with XLOC starts locked.
    return ref_info(ref).get("lock", Variant()).get_type() == Variant::DICTIONARY;
}

Vector3 SkydotPapyrus::position_of(std::uint32_t ref) const {
    if (const auto it = actors_.find(ref); it != actors_.end()) {
        return it->second;
    }
    const Dictionary info = ref_info(ref);
    if (info.is_empty() && world_.is_valid()) {
        return world_->get_actor(ref).get("position", Vector3()); // a placed actor
    }
    return info.get("position", Vector3());
}

void SkydotPapyrus::bind_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    auto& v = *vm_;
    const auto arg = [](NativeCall& c, std::size_t i) -> Value {
        return i < c.args.size() ? c.args[i] : Value{};
    };
    const auto result = [](Value value) { return NativeResult{.value = std::move(value), .wait = -1.0}; };
    const auto number = [](const Value& x) {
        return x.kind == vm::Kind::floating ? static_cast<double>(x.f)
                                            : (x.kind == vm::Kind::integer ? static_cast<double>(x.i) : 0.0);
    };

    // ---- Debug, Utility, Game
    v.bind("Debug", "Trace", [this, arg](NativeCall& c) {
        const String text = to_godot(vm::Vm::to_string(arg(c, 0)));
        godot::UtilityFunctions::print(String("[Papyrus] ") + text);
        emit_signal("trace", text);
        return NativeResult{};
    });
    for (const bool box : {false, true}) {
        v.bind("Debug", box ? "MessageBox" : "Notification", [this, arg, box](NativeCall& c) {
            const String text = to_godot(vm::Vm::to_string(arg(c, 0)));
            godot::UtilityFunctions::print(String(box ? "[Message] " : "[Notification] ") + text);
            emit_signal("message", text, box);
            return NativeResult{};
        });
    }
    v.bind("Utility", "Wait", [arg, number](NativeCall& c) {
        return NativeResult{.value = {}, .wait = std::max(0.0, number(arg(c, 0)))};
    });
    v.bind("Utility", "GetCurrentRealTime", [result](NativeCall& c) {
        return result(Value::floating(static_cast<float>(c.vm.time())));
    });
    // Days since the start, at the default timescale.
    v.bind("Utility", "GetCurrentGameTime", [result](NativeCall& c) {
        return result(Value::floating(static_cast<float>(c.vm.time() * k_timescale / 86400.0)));
    });
    // Both bounds inclusive, as documented for RandomInt.
    v.bind("Utility", "RandomInt", [arg, result](NativeCall& c) {
        auto lo = c.args.empty() ? 0 : arg(c, 0).i;
        auto hi = c.args.size() < 2 ? 100 : arg(c, 1).i;
        if (lo > hi) {
            std::swap(lo, hi);
        }
        const auto span = static_cast<std::uint64_t>(static_cast<std::int64_t>(hi) - lo) + 1;
        return result(Value::integer(static_cast<std::int32_t>(lo + static_cast<std::int64_t>(c.vm.random() % span))));
    });
    v.bind("Utility", "RandomFloat", [arg, result, number](NativeCall& c) {
        const double lo = c.args.empty() ? 0.0 : number(arg(c, 0));
        const double hi = c.args.size() < 2 ? 1.0 : number(arg(c, 1));
        const double unit = static_cast<double>(c.vm.random()) / 4294967295.0;
        return result(Value::floating(static_cast<float>(lo + (hi - lo) * unit)));
    });
    v.bind("Game", "GetPlayer", [result](NativeCall& c) {
        return result(c.vm.object(static_cast<std::uint32_t>(PLAYER_REF), "Actor"));
    });

    // ---- Form: identity and update timers
    v.bind("Form", "GetFormID", [result](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(c.self.form)));
    });
    const auto updates = [arg, number](NativeCall& c, bool repeat) {
        if (c.self.instance == nullptr) {
            c.vm.error("RegisterForUpdate needs a script to send OnUpdate to");
            return NativeResult{};
        }
        const double interval = std::max(0.0, number(arg(c, 0)));
        c.vm.register_update(c.self.instance, interval, repeat ? std::max(interval, 0.001) : 0.0);
        return NativeResult{};
    };
    v.bind("Form", "RegisterForSingleUpdate", [updates](NativeCall& c) { return updates(c, false); });
    v.bind("Form", "RegisterForUpdate", [updates](NativeCall& c) { return updates(c, true); });
    v.bind("Form", "UnregisterForUpdate", [](NativeCall& c) {
        if (c.self.instance != nullptr) {
            c.vm.unregister_update(c.self.instance);
        }
        return NativeResult{};
    });

    // ---- ObjectReference: enable state
    v.bind("ObjectReference", "Enable", [this](NativeCall& c) {
        set_disabled(c.self.form, false);
        return NativeResult{};
    });
    v.bind("ObjectReference", "Disable", [this](NativeCall& c) {
        set_disabled(c.self.form, true);
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsDisabled", [this, result](NativeCall& c) {
        return result(Value::boolean(disabled(c.self.form)));
    });
    v.bind("ObjectReference", "IsEnabled", [this, result](NativeCall& c) {
        return result(Value::boolean(!disabled(c.self.form)));
    });
    // Built by the game layer and not disabled; unloading is not tracked.
    v.bind("ObjectReference", "Is3DLoaded", [this, result](NativeCall& c) {
        return result(Value::boolean(built_.contains(c.self.form) && !disabled(c.self.form)));
    });

    // ---- links, base, place
    // The link with this keyword (None: the link without one).
    v.bind("ObjectReference", "GetLinkedRef", [this, arg, result](NativeCall& c) {
        const std::uint32_t keyword = arg(c, 0).kind == vm::Kind::object ? arg(c, 0).form : 0;
        const Array links = ref_info(c.self.form).get("links", Array());
        for (std::int64_t i = 0; i < links.size(); ++i) {
            const Dictionary link = links[i];
            if (static_cast<std::int64_t>(link["keyword"]) == keyword) {
                return result(c.vm.object(
                    static_cast<std::uint32_t>(static_cast<std::int64_t>(link["target"])), "ObjectReference"));
            }
        }
        return NativeResult{};
    });
    v.bind("ObjectReference", "GetBaseObject", [this, result](NativeCall& c) {
        const std::int64_t base = ref_info(c.self.form).get("base", 0);
        return result(c.vm.object(static_cast<std::uint32_t>(base), "Form"));
    });
    for (const char axis : {'X', 'Y', 'Z'}) {
        const auto index = static_cast<int>(axis - 'X');
        v.bind("ObjectReference", std::string("GetPosition") + axis, [this, index, result](NativeCall& c) {
            return result(Value::floating(static_cast<float>(position_of(c.self.form)[index])));
        });
        // Degrees, as the game reports them.
        v.bind("ObjectReference", std::string("GetAngle") + axis, [this, index, result](NativeCall& c) {
            const Vector3 r = ref_info(c.self.form).get("rotation", Vector3());
            return result(Value::floating(static_cast<float>(static_cast<double>(r[index]) * 180.0 / std::numbers::pi)));
        });
    }
    v.bind("ObjectReference", "GetScale", [this, result](NativeCall& c) {
        return result(Value::floating(static_cast<float>(static_cast<double>(ref_info(c.self.form).get("scale", 1.0)))));
    });
    v.bind("ObjectReference", "GetDistance", [this, arg, result](NativeCall& c) {
        if (arg(c, 0).kind != vm::Kind::object) {
            return result(Value::floating(0.0F));
        }
        return result(Value::floating(
            static_cast<float>(position_of(c.self.form).distance_to(position_of(arg(c, 0).form)))));
    });

    // ---- animation
    v.bind("ObjectReference", "PlayAnimation", [this, arg, result](NativeCall& c) {
        emit_signal("play_animation", static_cast<std::int64_t>(c.self.form),
                    to_godot(vm::Vm::to_string(arg(c, 0))));
        return result(Value::boolean(true));
    });
    // Waits for the named event (a text key of the animation) or a timeout.
    v.bind("ObjectReference", "PlayAnimationAndWait", [this, arg](NativeCall& c) {
        emit_signal("play_animation", static_cast<std::int64_t>(c.self.form),
                    to_godot(vm::Vm::to_string(arg(c, 0))));
        return NativeResult{.value = Value::boolean(true),
                            .wait = ANIMATION_TIMEOUT,
                            .wait_for = "anim:" + std::to_string(c.self.form) + ":" + vm::Vm::to_string(arg(c, 1))};
    });
    // Physics dependencies between animated objects; nothing to do without physics.
    v.bind("ObjectReference", "AddDependentAnimatedObjectReference",
           [result](NativeCall&) { return result(Value::boolean(true)); });
    v.bind("ObjectReference", "RemoveDependentAnimatedObjectReference",
           [result](NativeCall&) { return result(Value::boolean(true)); });

    // ---- activation
    v.bind("ObjectReference", "Activate", [this, arg, result](NativeCall& c) {
        emit_signal("activate_requested", static_cast<std::int64_t>(c.self.form),
                    static_cast<std::int64_t>(arg(c, 0).form), vm::Vm::truthy(arg(c, 1)));
        return result(Value::boolean(true));
    });
    v.bind("ObjectReference", "BlockActivation", [this, arg](NativeCall& c) {
        if (c.args.empty() || vm::Vm::truthy(arg(c, 0))) {
            blocked_.insert(c.self.form);
        } else {
            blocked_.erase(c.self.form);
        }
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsActivationBlocked", [this, result](NativeCall& c) {
        return result(Value::boolean(blocked_.contains(c.self.form)));
    });
    // No actors use furniture yet.
    v.bind("ObjectReference", "IsFurnitureInUse", [result](NativeCall&) { return result(Value::boolean(false)); });
    // Followers are not simulated.
    v.bind("ObjectReference", "SetNoFavorAllowed", [](NativeCall&) { return NativeResult{}; });

    // ---- doors and locks
    v.bind("ObjectReference", "GetOpenState", [this, result](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(get_open_state(c.self.form))));
    });
    v.bind("ObjectReference", "SetOpen", [this, arg](NativeCall& c) {
        const bool open = c.args.empty() || vm::Vm::truthy(arg(c, 0));
        set_open_state(c.self.form, open ? k_open : k_closed);
        emit_signal("open_changed", static_cast<std::int64_t>(c.self.form), open);
        return NativeResult{};
    });
    v.bind("ObjectReference", "Lock", [this, arg](NativeCall& c) {
        set_locked(c.self.form, c.args.empty() || vm::Vm::truthy(arg(c, 0)));
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsLocked", [this, result](NativeCall& c) {
        return result(Value::boolean(locked(c.self.form)));
    });
    v.bind("ObjectReference", "GetLockLevel", [this, result](NativeCall& c) {
        const auto level = get_lock_level(c.self.form);
        return result(Value::integer(static_cast<std::int32_t>(level < 0 ? 0 : level)));
    });

    // ---- triggers and effects
    v.bind("ObjectReference", "GetTriggerObjectCount", [this, result](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(get_trigger_count(c.self.form))));
    });
    // Effect shaders are not drawn yet; the game layer is told.
    v.bind("EffectShader", "Play", [this, arg](NativeCall& c) {
        emit_signal("effect_shader", static_cast<std::int64_t>(c.self.form),
                    static_cast<std::int64_t>(arg(c, 0).form), true);
        return NativeResult{};
    });
    v.bind("EffectShader", "Stop", [this, arg](NativeCall& c) {
        emit_signal("effect_shader", static_cast<std::int64_t>(c.self.form),
                    static_cast<std::int64_t>(arg(c, 0).form), false);
        return NativeResult{};
    });
}

// ---- values -------------------------------------------------------------------

vm::Value SkydotPapyrus::to_value(const Variant& v, std::string_view type, std::uint32_t owner_quest) {
    switch (v.get_type()) {
    case Variant::INT: {
        const auto n = static_cast<std::int64_t>(v);
        if (!type.empty() && !scalar_type(type) && !type.ends_with("[]")) {
            // What a property names has its scripts, loaded or not: in the
            // game such references are persistent, and quests exist before
            // they start.
            const auto form = static_cast<std::uint32_t>(n);
            attach_quest_scripts(form, true);
            if (!attached_.contains(form) && world_.is_valid()) {
                if (const auto cell = world_->get_ref_cell(form); cell != 0) {
                    attach(cell, form);
                }
            }
            return vm::Value::object(static_cast<std::uint32_t>(n), vm_->load_class(type));
        }
        return vm::Value::integer(static_cast<std::int32_t>(n));
    }
    case Variant::FLOAT: return vm::Value::floating(static_cast<float>(static_cast<double>(v)));
    case Variant::BOOL: return vm::Value::boolean(static_cast<bool>(v));
    case Variant::STRING:
    case Variant::STRING_NAME: return vm::Value::string(utf8(v));
    case Variant::ARRAY: {
        const Array list = v;
        const auto element = type.ends_with("[]") ? type.substr(0, type.size() - 2) : std::string_view{};
        auto out = std::make_shared<vm::Array>();
        for (std::int64_t i = 0; i < list.size(); ++i) {
            out->push_back(to_value(list[i], element, owner_quest));
        }
        return vm::Value::make_array(out);
    }
    case Variant::DICTIONARY: {
        // A quest alias: {form, alias}; form 0 is the quest whose script this is.
        const Dictionary d = v;
        auto quest = static_cast<std::uint32_t>(static_cast<std::int64_t>(d.get("form", 0)));
        if (quest == 0) {
            quest = owner_quest;
        }
        const auto alias = static_cast<std::int64_t>(d.get("alias", -1));
        if (quest == 0 || alias < 0) {
            return {};
        }
        attach_quest_scripts(quest, true);
        return vm::Value::object(alias_handle(quest, static_cast<std::uint32_t>(alias)),
                                 vm_->load_class(type.empty() ? std::string_view("Alias") : type));
    }
    default:
        return {};
    }
}

Variant SkydotPapyrus::to_variant(const vm::Value& v) const {
    switch (v.kind) {
    case vm::Kind::none: return {};
    case vm::Kind::integer: return v.i;
    case vm::Kind::floating: return v.f;
    case vm::Kind::boolean: return v.b;
    case vm::Kind::string: return to_godot(v.s);
    case vm::Kind::object: return static_cast<std::int64_t>(v.form);
    case vm::Kind::array: {
        Array out;
        for (const auto& e : *v.array) {
            out.push_back(to_variant(e));
        }
        return out;
    }
    }
    return {};
}

// ---- attaching and events -------------------------------------------------

std::int64_t SkydotPapyrus::attach(std::int64_t cell, std::int64_t ref) {
    const auto form = static_cast<std::uint32_t>(ref);
    if (world_.is_null() || !attached_.insert(form).second) {
        return 0;
    }
    const Dictionary info = world_->get_ref_info(cell, ref);
    const std::int64_t count = attach_scripts(form, info.get("scripts", Array()), 0);
    if (count != 0) {
        register_trigger(form, info);
        vm_->send_event(form, "OnInit", {});
    }
    return count;
}

std::int64_t SkydotPapyrus::attach_scripts(std::uint32_t form, const Array& scripts,
                                           std::uint32_t owner_quest) {
    // Base scripts first; a reference's entry of the same name overrides its
    // status and property values.
    std::vector<std::string> order;
    std::map<std::string, std::pair<std::int64_t, Dictionary>> merged;
    for (std::int64_t i = 0; i < scripts.size(); ++i) {
        const Dictionary s = scripts[i];
        const std::string name = utf8(s["name"]);
        const std::string key = vm::to_lower(name);
        auto [it, fresh] = merged.try_emplace(key, std::pair<std::int64_t, Dictionary>{0, Dictionary()});
        if (fresh) {
            order.push_back(name);
        }
        it->second.first = s["status"];
        it->second.second.merge(s["properties"], true);
    }
    std::int64_t count = 0;
    for (const auto& name : order) {
        const auto& [status, properties] = merged[vm::to_lower(name)];
        if ((status & 0x2) != 0) {
            continue; // removed
        }
        auto* instance = vm_->attach(form, name);
        if (instance == nullptr) {
            continue;
        }
        const Array keys = properties.keys();
        for (std::int64_t k = 0; k < keys.size(); ++k) {
            const std::string pname = vm::to_lower(utf8(keys[k]));
            std::string type;
            for (const auto* c = instance->cls; c != nullptr && type.empty(); c = c->parent()) {
                if (const auto* p = c->property(pname)) {
                    type = p->type;
                }
            }
            if (type.empty() ||
                !vm_->set_property(instance, pname, to_value(properties[keys[k]], type, owner_quest))) {
                vm_->error(name + ": no property " + pname);
            }
        }
        ++count;
    }
    return count;
}

std::int64_t SkydotPapyrus::attach_built(godot::Node* root) {
    if (root == nullptr) {
        return 0;
    }
    std::int64_t scripted = 0;
    std::vector<godot::Node*> pending{root};
    while (!pending.empty()) {
        godot::Node* node = pending.back();
        pending.pop_back();
        for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
            godot::Node* child = node->get_child(i);
            if (!child->has_meta("skydot_ref")) {
                pending.push_back(child);
                continue;
            }
            const std::int64_t ref = child->get_meta("skydot_ref");
            built_.insert(static_cast<std::uint32_t>(ref));
            attach(child->get_meta("skydot_cell"), ref);
            dispatch(static_cast<std::uint32_t>(ref), "OnLoad", {});
            if (!vm_->instances(static_cast<std::uint32_t>(ref)).empty()) {
                ++scripted;
            }
        }
    }
    return scripted;
}

std::int64_t SkydotPapyrus::attach_cell(std::int64_t cell) {
    if (world_.is_null()) {
        return 0;
    }
    std::int64_t count = 0;
    const auto refs = world_->get_scripted_refs(cell);
    for (std::int64_t i = 0; i < refs.size(); ++i) {
        count += attach(cell, refs[i]) > 0 ? 1 : 0;
    }
    return count;
}

std::int64_t SkydotPapyrus::activate(std::int64_t ref, std::int64_t activator) {
    return static_cast<std::int64_t>(dispatch(
        static_cast<std::uint32_t>(ref), "OnActivate",
        {vm_->object(static_cast<std::uint32_t>(activator), "ObjectReference")}));
}

std::int64_t SkydotPapyrus::send_event(std::int64_t ref, const String& event, const Array& args) {
    std::vector<vm::Value> values;
    for (std::int64_t i = 0; i < args.size(); ++i) {
        values.push_back(to_value(args[i], args[i].get_type() == Variant::INT ? "objectreference" : ""));
    }
    return static_cast<std::int64_t>(dispatch(static_cast<std::uint32_t>(ref), utf8(event), values));
}

void SkydotPapyrus::notify_animation_event(std::int64_t ref, const String& event) {
    vm_->notify("anim:" + std::to_string(static_cast<std::uint32_t>(ref)) + ":" + utf8(event));
}

// ---- triggers -------------------------------------------------------------

// A box's bounds are half extents: xEdit shows XPRM bounds doubled. A sphere's
// radius is its x. Neither is checked against the game yet.
void SkydotPapyrus::register_trigger(std::uint32_t ref, const Dictionary& info) {
    const Variant primitive = info.get("primitive", Variant());
    if (primitive.get_type() != Variant::DICTIONARY) {
        return;
    }
    const Dictionary p = primitive;
    const auto type = static_cast<std::uint32_t>(static_cast<std::int64_t>(p["type"]));
    if (type != k_box && type != k_sphere) {
        return;
    }
    const Vector3 r = info.get("rotation", Vector3());
    // As SkydotWorld::skyrim_transform: clockwise angles, Z first, then Y, X.
    const godot::Basis rx(Vector3(1, 0, 0), -r.x);
    const godot::Basis ry(Vector3(0, 1, 0), -r.y);
    const godot::Basis rz(Vector3(0, 0, 1), -r.z);
    const auto scale = static_cast<godot::real_t>(static_cast<double>(info.get("scale", 1.0)));
    Trigger t;
    t.origin = info.get("position", Vector3());
    t.to_local = (rx * ry * rz).transposed();
    t.half = Vector3(p["bounds"]) * scale;
    t.type = type;
    triggers_[ref] = std::move(t);
}

bool SkydotPapyrus::contains(const Trigger& t, const Vector3& p) {
    const Vector3 local = t.to_local.xform(p - t.origin);
    if (t.type == k_sphere) {
        return local.length() <= t.half.x;
    }
    return std::abs(local.x) <= t.half.x && std::abs(local.y) <= t.half.y &&
           std::abs(local.z) <= t.half.z;
}

void SkydotPapyrus::update_actor(std::int64_t actor, const Vector3& position) {
    const auto form = static_cast<std::uint32_t>(actor);
    actors_[form] = position;
    for (auto& [ref, t] : triggers_) {
        const bool in = contains(t, position);
        const bool was = t.inside.contains(form);
        if (in == was) {
            continue;
        }
        if (in) {
            t.inside.insert(form);
        } else {
            t.inside.erase(form);
        }
        emit_signal("trigger", static_cast<std::int64_t>(ref), actor, in);
        dispatch(ref, in ? "OnTriggerEnter" : "OnTriggerLeave", {vm_->object(form, "ObjectReference")});
    }
}

std::int64_t SkydotPapyrus::get_trigger_count(std::int64_t ref) const {
    const auto it = triggers_.find(static_cast<std::uint32_t>(ref));
    return it != triggers_.end() ? static_cast<std::int64_t>(it->second.inside.size()) : 0;
}

Array SkydotPapyrus::get_triggers() const {
    Array out;
    for (const auto& [ref, t] : triggers_) {
        Dictionary entry;
        entry["ref"] = static_cast<std::int64_t>(ref);
        entry["type"] = static_cast<std::int64_t>(t.type);
        entry["origin"] = t.origin;
        entry["half"] = t.half;
        entry["inside"] = static_cast<std::int64_t>(t.inside.size());
        out.push_back(entry);
    }
    return out;
}

// ---- state the game layer reads and sets ----------------------------------

std::int64_t SkydotPapyrus::get_lock_level(std::int64_t ref) const {
    const auto form = static_cast<std::uint32_t>(ref);
    if (!locked(form)) {
        return -1;
    }
    const Variant lock = ref_info(form).get("lock", Variant());
    return lock.get_type() == Variant::DICTIONARY ? static_cast<std::int64_t>(Dictionary(lock)["level"]) : 0;
}

void SkydotPapyrus::set_locked(std::int64_t ref, bool value) {
    const auto form = static_cast<std::uint32_t>(ref);
    const bool was = locked(form);
    locked_[form] = value;
    if (was != value) {
        emit_signal("lock_changed", ref, value);
    }
}

std::int64_t SkydotPapyrus::get_open_state(std::int64_t ref) const {
    const auto form = static_cast<std::uint32_t>(ref);
    if (const auto it = open_.find(form); it != open_.end()) {
        return it->second;
    }
    return String(ref_info(form).get("type", String())) == "DOOR" ? k_closed : 0;
}

void SkydotPapyrus::set_open_state(std::int64_t ref, std::int64_t state) {
    open_[static_cast<std::uint32_t>(ref)] = state;
}

bool SkydotPapyrus::is_activation_blocked(std::int64_t ref) const {
    return blocked_.contains(static_cast<std::uint32_t>(ref));
}

Dictionary SkydotPapyrus::get_disabled_changes() const {
    Dictionary out;
    for (const auto& [ref, value] : disabled_) {
        out[static_cast<std::int64_t>(ref)] = value;
    }
    return out;
}

bool SkydotPapyrus::is_ref_disabled(std::int64_t ref) const {
    return world_.is_valid() && disabled(static_cast<std::uint32_t>(ref));
}

// ---- saving ---------------------------------------------------------------

godot::PackedByteArray SkydotPapyrus::save_state() const {
    Dictionary state;
    state["format"] = k_save_format;
    const auto vm_bytes = vm_->save();
    godot::PackedByteArray vm_packed;
    vm_packed.resize(static_cast<std::int64_t>(vm_bytes.size()));
    std::copy(vm_bytes.begin(), vm_bytes.end(), vm_packed.ptrw());
    state["vm"] = vm_packed;
    Array attached;
    for (const auto ref : attached_) {
        attached.push_back(static_cast<std::int64_t>(ref));
    }
    state["attached"] = attached;
    Dictionary disabled;
    for (const auto& [ref, value] : disabled_) {
        disabled[static_cast<std::int64_t>(ref)] = value;
    }
    state["disabled"] = disabled;
    Array blocked;
    for (const auto ref : blocked_) {
        blocked.push_back(static_cast<std::int64_t>(ref));
    }
    state["blocked"] = blocked;
    Dictionary open;
    for (const auto& [ref, value] : open_) {
        open[static_cast<std::int64_t>(ref)] = value;
    }
    state["open"] = open;
    Dictionary locks;
    for (const auto& [ref, value] : locked_) {
        locks[static_cast<std::int64_t>(ref)] = value;
    }
    state["locked"] = locks;
    Dictionary inside;
    for (const auto& [ref, t] : triggers_) {
        Array actors;
        for (const auto a : t.inside) {
            actors.push_back(static_cast<std::int64_t>(a));
        }
        inside[static_cast<std::int64_t>(ref)] = actors;
    }
    state["inside"] = inside;
    Dictionary actors;
    for (const auto& [ref, position] : actors_) {
        actors[static_cast<std::int64_t>(ref)] = position;
    }
    state["actors"] = actors;
    state["next_form"] = static_cast<std::int64_t>(next_form_);
    Dictionary quests;
    for (const auto& [quest, q] : quests_) {
        Dictionary entry;
        entry["running"] = q.running;
        entry["started"] = q.started;
        entry["completed"] = q.completed;
        entry["failed"] = q.failed;
        entry["active"] = q.active;
        entry["stage"] = q.stage;
        Array done;
        for (const auto stage : q.done) {
            done.push_back(stage);
        }
        entry["done"] = done;
        Dictionary objectives;
        for (const auto& [index, bits] : q.objectives) {
            objectives[index] = bits;
        }
        entry["objectives"] = objectives;
        quests[static_cast<std::int64_t>(quest)] = entry;
    }
    state["quests"] = quests;
    Array quest_scripts;
    for (const auto quest : quest_scripts_) {
        quest_scripts.push_back(static_cast<std::int64_t>(quest));
    }
    state["quest_scripts"] = quest_scripts;
    Array handles; // [quest, alias, handle, fill] each
    for (const auto& [key, handle] : alias_handles_) {
        const auto fill = alias_fill_.find(handle);
        handles.push_back(Array::make(static_cast<std::int64_t>(key.first), static_cast<std::int64_t>(key.second),
                                      static_cast<std::int64_t>(handle),
                                      static_cast<std::int64_t>(fill != alias_fill_.end() ? fill->second : 0)));
    }
    state["aliases"] = handles;
    Dictionary globals;
    for (const auto& [global, value] : globals_) {
        globals[static_cast<std::int64_t>(global)] = value;
    }
    state["globals"] = globals;
    state["next_alias_handle"] = static_cast<std::int64_t>(next_alias_handle_);
    return godot::UtilityFunctions::var_to_bytes(state);
}

godot::Error SkydotPapyrus::load_state(const godot::PackedByteArray& bytes) {
    const auto fail = [this](const String& why) {
        last_error_ = why;
        godot::UtilityFunctions::push_error(String("SkydotPapyrus: ") + why);
        return godot::ERR_FILE_CORRUPT;
    };
    if (world_.is_null()) {
        return fail("setup has not run");
    }
    // Objects are not allowed in: a save is data.
    const Variant decoded = godot::UtilityFunctions::bytes_to_var(bytes);
    if (decoded.get_type() != Variant::DICTIONARY) {
        return fail("not a script save");
    }
    const Dictionary state = decoded;
    const auto format = static_cast<std::int64_t>(state.get("format", -1));
    if (format != k_save_format && format != 1) {
        return fail("script save format " + String(Variant(state.get("format", -1))) +
                    " is not one this engine reads");
    }
    const auto typed = [&](const char* key, Variant::Type type) {
        return state.get(key, Variant()).get_type() == type;
    };
    for (const auto& [key, type] :
         {std::pair{"vm", Variant::PACKED_BYTE_ARRAY}, {"attached", Variant::ARRAY},
          {"disabled", Variant::DICTIONARY}, {"blocked", Variant::ARRAY}, {"open", Variant::DICTIONARY},
          {"locked", Variant::DICTIONARY}, {"inside", Variant::DICTIONARY}, {"actors", Variant::DICTIONARY},
          {"next_form", Variant::INT}}) {
        if (!typed(key, type)) {
            return fail(String("script save lacks ") + key);
        }
    }
    if (format >= 2) {
        for (const auto& [key, type] :
             {std::pair{"quests", Variant::DICTIONARY}, {"quest_scripts", Variant::ARRAY},
              {"aliases", Variant::ARRAY}, {"globals", Variant::DICTIONARY},
              {"next_alias_handle", Variant::INT}}) {
            if (!typed(key, type)) {
                return fail(String("script save lacks ") + key);
            }
        }
    }
    const godot::PackedByteArray packed = state["vm"];
    const std::vector<std::uint8_t> vm_bytes(packed.ptr(), packed.ptr() + packed.size());
    std::string why;
    reset();
    if (!vm_->load(vm_bytes, why)) {
        return fail(to_godot(why));
    }
    const auto ref_of = [](const Variant& v) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(v)); };
    const Array attached = state["attached"];
    for (std::int64_t i = 0; i < attached.size(); ++i) {
        const auto ref = ref_of(attached[i]);
        attached_.insert(ref);
        if (!vm_->instances(ref).empty()) {
            register_trigger(ref, ref_info(ref));
        }
    }
    const auto each = [](const Dictionary& d, auto&& fn) {
        const Array keys = d.keys();
        for (std::int64_t i = 0; i < keys.size(); ++i) {
            fn(keys[i], d[keys[i]]);
        }
    };
    each(state["disabled"], [&](const Variant& k, const Variant& v) { disabled_[ref_of(k)] = v; });
    const Array blocked = state["blocked"];
    for (std::int64_t i = 0; i < blocked.size(); ++i) {
        blocked_.insert(ref_of(blocked[i]));
    }
    each(state["open"], [&](const Variant& k, const Variant& v) { open_[ref_of(k)] = v; });
    each(state["locked"], [&](const Variant& k, const Variant& v) { locked_[ref_of(k)] = v; });
    each(state["inside"], [&](const Variant& k, const Variant& v) {
        if (const auto it = triggers_.find(ref_of(k)); it != triggers_.end() && v.get_type() == Variant::ARRAY) {
            const Array actors = v;
            for (std::int64_t i = 0; i < actors.size(); ++i) {
                it->second.inside.insert(ref_of(actors[i]));
            }
        }
    });
    each(state["actors"], [&](const Variant& k, const Variant& v) {
        if (v.get_type() == Variant::VECTOR3) {
            actors_[ref_of(k)] = v;
        }
    });
    next_form_ = ref_of(state["next_form"]);
    if (format >= 2) {
        each(state["quests"], [&](const Variant& k, const Variant& v) {
            if (v.get_type() != Variant::DICTIONARY) {
                return;
            }
            const Dictionary d = v;
            QuestState q;
            q.running = d.get("running", false);
            q.started = d.get("started", false);
            q.completed = d.get("completed", false);
            q.failed = d.get("failed", false);
            q.active = d.get("active", false);
            q.stage = static_cast<std::int32_t>(static_cast<std::int64_t>(d.get("stage", 0)));
            const Variant done = d.get("done", Array());
            if (done.get_type() == Variant::ARRAY) {
                const Array list = done;
                for (std::int64_t i = 0; i < list.size(); ++i) {
                    q.done.insert(static_cast<std::int32_t>(static_cast<std::int64_t>(list[i])));
                }
            }
            const Variant objectives = d.get("objectives", Dictionary());
            if (objectives.get_type() == Variant::DICTIONARY) {
                each(objectives, [&](const Variant& index, const Variant& bits) {
                    q.objectives[static_cast<std::int32_t>(static_cast<std::int64_t>(index))] =
                        static_cast<std::uint8_t>(static_cast<std::int64_t>(bits));
                });
            }
            quests_[ref_of(k)] = std::move(q);
        });
        const Array quest_scripts = state["quest_scripts"];
        for (std::int64_t i = 0; i < quest_scripts.size(); ++i) {
            quest_scripts_.insert(ref_of(quest_scripts[i]));
        }
        const Array handles = state["aliases"];
        for (std::int64_t i = 0; i < handles.size(); ++i) {
            if (handles[i].get_type() != Variant::ARRAY || Array(handles[i]).size() != 4) {
                return fail("script save has a malformed alias");
            }
            const Array a = handles[i];
            const auto key = std::pair{ref_of(a[0]), ref_of(a[1])};
            alias_handles_[key] = ref_of(a[2]);
            handle_alias_[ref_of(a[2])] = key;
            alias_fill_[ref_of(a[2])] = ref_of(a[3]);
        }
        each(state["globals"], [&](const Variant& k, const Variant& v) { globals_[ref_of(k)] = v; });
        next_alias_handle_ = ref_of(state["next_alias_handle"]);
    }
    last_error_ = String();
    return godot::OK;
}

String SkydotPapyrus::get_last_error() const { return last_error_; }

// ---- tests and inspection -------------------------------------------------

void SkydotPapyrus::update(double seconds) { vm_->update(seconds); }

std::int64_t SkydotPapyrus::call_global(const String& cls, const String& fn, const Array& args) {
    std::vector<vm::Value> values;
    for (std::int64_t i = 0; i < args.size(); ++i) {
        values.push_back(to_value(args[i], ""));
    }
    return static_cast<std::int64_t>(vm_->call_global(utf8(cls), utf8(fn), std::move(values)));
}

std::int64_t SkydotPapyrus::create_instance(const String& cls) {
    const auto form = next_form_++;
    return vm_->attach(form, utf8(cls)) != nullptr ? form : 0;
}

std::int64_t SkydotPapyrus::call_method(std::int64_t form, const String& cls, const String& fn,
                                        const Array& args) {
    std::vector<vm::Value> values;
    for (std::int64_t i = 0; i < args.size(); ++i) {
        values.push_back(to_value(args[i], ""));
    }
    return static_cast<std::int64_t>(vm_->call_method(
        vm_->instance(static_cast<std::uint32_t>(form), utf8(cls)), utf8(fn), std::move(values)));
}

Dictionary SkydotPapyrus::take_result(std::int64_t thread) {
    Dictionary out;
    const auto result = vm_->take_result(static_cast<std::uint64_t>(thread));
    out["done"] = result.has_value();
    if (result) {
        out["value"] = to_variant(*result);
    }
    return out;
}

Variant SkydotPapyrus::get_variable(std::int64_t form, const String& cls, const String& name) const {
    auto* instance = vm_->instance(static_cast<std::uint32_t>(form), utf8(cls));
    return instance != nullptr ? to_variant(vm_->get_variable(instance, utf8(name))) : Variant();
}

Array SkydotPapyrus::get_scripts(std::int64_t ref) const {
    Array out;
    for (const auto* i : vm_->instances(static_cast<std::uint32_t>(ref))) {
        Dictionary entry;
        entry["name"] = to_godot(i->cls->name());
        entry["state"] = to_godot(i->state);
        out.push_back(entry);
    }
    return out;
}

std::int64_t SkydotPapyrus::get_error_count() const { return static_cast<std::int64_t>(vm_->errors()); }
std::int64_t SkydotPapyrus::get_instruction_count() const {
    return static_cast<std::int64_t>(vm_->instructions());
}
std::int64_t SkydotPapyrus::get_thread_count() const {
    return static_cast<std::int64_t>(vm_->thread_count());
}
double SkydotPapyrus::get_time() const { return vm_->time(); }

} // namespace skydot
