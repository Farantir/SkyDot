// SPDX-License-Identifier: GPL-3.0-or-later
//
// ObjectReference natives: enable state, links, placement, animation, physics, activation, doors and locks, triggers.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"
#include "vm/activation.hpp"

#include <numbers>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Vector3;

namespace skydot {

void SkydotPapyrus::bind_objectreference_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    [[maybe_unused]] auto& v = *vm_;
    [[maybe_unused]] const auto arg = natives::arg;
    [[maybe_unused]] const auto result = natives::result;
    [[maybe_unused]] const auto number = natives::number;
    v.bind("ObjectReference", "Enable", [this](NativeCall& c) {
        set_disabled(c.self.form(), false);
        return NativeResult{};
    });
    v.bind("ObjectReference", "Disable", [this](NativeCall& c) {
        set_disabled(c.self.form(), true);
        return NativeResult{};
    });
    // No fade exists, so these are Enable and Disable.
    v.bind("ObjectReference", "EnableNoWait", [this](NativeCall& c) {
        set_disabled(c.self.form(), false);
        return NativeResult{};
    });
    v.bind("ObjectReference", "DisableNoWait", [this](NativeCall& c) {
        set_disabled(c.self.form(), true);
        return NativeResult{};
    });
    // Back to the placed position and rotation (or to akTarget's), enabled as
    // placed. The move reaches the game layer as `ref_moved`; open and lock
    // state are left alone.
    v.bind("ObjectReference", "Reset", [this, arg](NativeCall& c) {
        const std::uint32_t ref = c.self.form();
        const std::uint32_t target = arg(c, 0).kind() == vm::Kind::object ? arg(c, 0).form() : 0;
        Vector3 position;
        Vector3 rotation;
        if (target != 0) {
            position = position_of(target);
            rotation = rotation_of(target);
            vm::MovedTo to;
            for (int i = 0; i < 3; ++i) {
                to.position[i] = static_cast<double>(position[i]);
                to.rotation[i] = static_cast<double>(rotation[i]);
            }
            states_.moved[ref] = to;
        } else {
            states_.moved.erase(ref);
            const Dictionary info = ref_info(ref);
            position = info.get("position", Vector3());
            rotation = info.get("rotation", Vector3());
        }
        emit_signal("ref_moved", static_cast<std::int64_t>(ref), position, rotation);
        states_.disabled.erase(ref);
        clear_destruction(ref);
        set_disabled(ref, static_cast<bool>(ref_info(ref).get("disabled", false)));
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsDisabled", [this, result](NativeCall& c) {
        return result(Value::boolean(disabled(c.self.form())));
    });
    v.bind("ObjectReference", "IsEnabled", [this, result](NativeCall& c) {
        return result(Value::boolean(!disabled(c.self.form())));
    });
    // Its model is built (the registry forgets nodes as they leave the tree)
    // and it is not disabled.
    v.bind("ObjectReference", "Is3DLoaded", [this, result](NativeCall& c) {
        return result(Value::boolean(is_3d_loaded(c.self.form())));
    });

    // ---- links, base, place
    // The link with this keyword (None: the link without one).
    v.bind("ObjectReference", "GetLinkedRef", [this, arg, result](NativeCall& c) {
        const std::uint32_t keyword = arg(c, 0).kind() == vm::Kind::object ? arg(c, 0).form() : 0;
        const Array links = ref_info(c.self.form()).get("links", Array());
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
        const std::int64_t base = ref_info(c.self.form()).get("base", 0);
        return result(c.vm.object(static_cast<std::uint32_t>(base), "Form"));
    });
    for (const char axis : {'X', 'Y', 'Z'}) {
        const auto index = static_cast<int>(axis - 'X');
        v.bind("ObjectReference", std::string("GetPosition") + axis, [this, index, result](NativeCall& c) {
            return result(Value::floating(static_cast<float>(position_of(c.self.form())[index])));
        });
        // Degrees, as the game reports them.
        v.bind("ObjectReference", std::string("GetAngle") + axis, [this, index, result](NativeCall& c) {
            const Vector3 r = rotation_of(c.self.form());
            return result(Value::floating(static_cast<float>(static_cast<double>(r[index]) * 180.0 / std::numbers::pi)));
        });
    }
    v.bind("ObjectReference", "GetScale", [this, result](NativeCall& c) {
        return result(Value::floating(static_cast<float>(static_cast<double>(ref_info(c.self.form()).get("scale", 1.0)))));
    });
    v.bind("ObjectReference", "GetDistance", [this, arg, result](NativeCall& c) {
        if (arg(c, 0).kind() != vm::Kind::object) {
            return result(Value::floating(0.0F));
        }
        return result(Value::floating(
            static_cast<float>(position_of(c.self.form()).distance_to(position_of(arg(c, 0).form())))));
    });

    // ---- animation
    v.bind("ObjectReference", "PlayAnimation", [this, arg, result](NativeCall& c) {
        emit_signal("play_animation", static_cast<std::int64_t>(c.self.form()),
                    to_godot(vm::Vm::to_string(arg(c, 0))));
        return result(Value::boolean(true));
    });
    // Waits for the named event (a text key of the animation) or a timeout.
    // The signal goes out after the wait is armed: a handler that reports the
    // event synchronously would otherwise be lost.
    v.bind("ObjectReference", "PlayAnimationAndWait", [this, arg](NativeCall& c) {
        const auto form = static_cast<std::int64_t>(c.self.form());
        const String animation = to_godot(vm::Vm::to_string(arg(c, 0)));
        return NativeResult{.value = Value::boolean(true),
                            .wait = ANIMATION_TIMEOUT,
                            .wait_for = "anim:" + std::to_string(c.self.form()) + ":" + vm::Vm::to_string(arg(c, 1)),
                            .after = [this, form, animation] { emit_signal("play_animation", form, animation); }};
    });
    // Physics dependencies between animated objects; nothing to do without physics.
    v.bind("ObjectReference", "AddDependentAnimatedObjectReference",
           [result](NativeCall&) { return result(Value::boolean(true)); });
    v.bind("ObjectReference", "RemoveDependentAnimatedObjectReference",
           [result](NativeCall&) { return result(Value::boolean(true)); });

    // ---- physics: the game layer owns the bodies
    // Direction in Skyrim's space, magnitude in Havok's (about kg m/s).
    v.bind("ObjectReference", "ApplyHavokImpulse", [this, arg, number](NativeCall& c) {
        const Vector3 direction(static_cast<float>(number(arg(c, 0))), static_cast<float>(number(arg(c, 1))),
                                static_cast<float>(number(arg(c, 2))));
        emit_signal("havok_impulse", static_cast<std::int64_t>(c.self.form()), direction, number(arg(c, 3)));
        return NativeResult{};
    });
    // ObjectReference.psc: 1 dynamic, 2 sphere, 3 box, 6 thin box inertia
    // (all moving), 4 keyframed, 5 fixed, 7 character.
    v.bind("ObjectReference", "SetMotionType", [this, arg](NativeCall& c) {
        emit_signal("motion_type_changed", static_cast<std::int64_t>(c.self.form()),
                    static_cast<std::int64_t>(arg(c, 0).i()));
        return NativeResult{};
    });

    // ---- activation
    v.bind("ObjectReference", "Activate", [this, arg, result](NativeCall& c) {
        // Next VM update, on the VM's clock (and in its saves), not inside this call.
        const std::uint32_t activator = arg(c, 0).form() != 0 ? arg(c, 0).form() : static_cast<std::uint32_t>(PLAYER_REF);
        vm_->schedule(0.0, "activate", c.self.form(), activator,
                      static_cast<std::uint32_t>(ActivationSource::script) | (vm::Vm::truthy(arg(c, 1)) ? 0x100u : 0u));
        return result(Value::boolean(true));
    });
    v.bind("ObjectReference", "BlockActivation", [this, arg](NativeCall& c) {
        if (c.args.empty() || vm::Vm::truthy(arg(c, 0))) {
            states_.blocked.insert(c.self.form());
        } else {
            states_.blocked.erase(c.self.form());
        }
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsActivationBlocked", [this, result](NativeCall& c) {
        return result(Value::boolean(states_.blocked.contains(c.self.form())));
    });
    // No actors use furniture yet.
    v.bind("ObjectReference", "IsFurnitureInUse", [result](NativeCall&) { return result(Value::boolean(false)); });
    // Followers are not simulated.
    v.bind("ObjectReference", "SetNoFavorAllowed", [](NativeCall&) { return NativeResult{}; });

    // ---- doors and locks
    v.bind("ObjectReference", "GetOpenState", [this, result](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(get_open_state(c.self.form()))));
    });
    v.bind("ObjectReference", "SetOpen", [this, arg](NativeCall& c) {
        const bool open = c.args.empty() || vm::Vm::truthy(arg(c, 0));
        set_open_state(c.self.form(), open ? k_open : k_closed);
        emit_signal("open_changed", static_cast<std::int64_t>(c.self.form()), open);
        return NativeResult{};
    });
    v.bind("ObjectReference", "Lock", [this, arg](NativeCall& c) {
        set_locked(c.self.form(), c.args.empty() || vm::Vm::truthy(arg(c, 0)));
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsLocked", [this, result](NativeCall& c) {
        return result(Value::boolean(locked(c.self.form())));
    });
    v.bind("ObjectReference", "GetLockLevel", [this, result](NativeCall& c) {
        const auto level = get_lock_level(c.self.form());
        return result(Value::integer(static_cast<std::int32_t>(level < 0 ? 0 : level)));
    });

    // ---- inventory (bound here, reached from Actor through the parent class)
    // A FormList is not looked into (its forms are not reachable from here):
    // it counts as nothing and adds or removes nothing.
    v.bind("ObjectReference", "GetItemCount", [this, arg, result](NativeCall& c) {
        if (arg(c, 0).kind() != vm::Kind::object) {
            return result(Value::integer(0));
        }
        return result(Value::integer(static_cast<std::int32_t>(vm_->inventory().count(c.self.form(), arg(c, 0).form()))));
    });
    v.bind("ObjectReference", "AddItem", [this, arg](NativeCall& c) {
        if (arg(c, 0).kind() == vm::Kind::object) {
            vm_->inventory().add(c.self.form(), arg(c, 0).form(), c.args.size() > 1 ? arg(c, 1).i() : 1);
        }
        return NativeResult{};
    });
    // Moves the items to `other` if given, else they are gone.
    v.bind("ObjectReference", "RemoveItem", [this, arg](NativeCall& c) {
        if (arg(c, 0).kind() == vm::Kind::object) {
            const std::int64_t taken =
                vm_->inventory().remove(c.self.form(), arg(c, 0).form(), c.args.size() > 1 ? arg(c, 1).i() : 1);
            if (arg(c, 3).kind() == vm::Kind::object) {
                vm_->inventory().add(arg(c, 3).form(), arg(c, 0).form(), taken);
            }
        }
        return NativeResult{};
    });

    // ---- triggers and effects
    v.bind("ObjectReference", "GetTriggerObjectCount", [this, result](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(get_trigger_count(c.self.form()))));
    });
}

} // namespace skydot
