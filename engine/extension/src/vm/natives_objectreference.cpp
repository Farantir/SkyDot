// SPDX-License-Identifier: GPL-3.0-or-later
//
// ObjectReference natives: enable state, links, placement, animation, physics, activation, doors and locks, triggers.
#include "vm/activation.hpp"
#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

namespace skydot::vm {

namespace {

/// A float's value as the double the interfaces carry.
double as_float(double x) { return static_cast<double>(static_cast<float>(x)); }

} // namespace

void bind_objectreference_natives(Vm& v, const WorldAccess& world, WorldEvents& events, ReferenceHost& host) {
    using namespace natives;
    v.bind("ObjectReference", "Enable", [&host](NativeCall& c) {
        host.set_disabled(c.self.form(), false);
        return NativeResult{};
    });
    v.bind("ObjectReference", "Disable", [&host](NativeCall& c) {
        host.set_disabled(c.self.form(), true);
        return NativeResult{};
    });
    // No fade exists, so these are Enable and Disable.
    v.bind("ObjectReference", "EnableNoWait", [&host](NativeCall& c) {
        host.set_disabled(c.self.form(), false);
        return NativeResult{};
    });
    v.bind("ObjectReference", "DisableNoWait", [&host](NativeCall& c) {
        host.set_disabled(c.self.form(), true);
        return NativeResult{};
    });
    // Back to the placed position and rotation (or to akTarget's), enabled as
    // placed. The move reaches the game layer as `ref_moved`; open and lock
    // state are left alone.
    v.bind("ObjectReference", "Reset", [&world, &events, &host](NativeCall& c) {
        const std::uint32_t ref = c.self.form();
        const std::uint32_t target = arg(c, 0).kind() == Kind::object ? arg(c, 0).form() : 0;
        Xyz position{};
        Xyz rotation{};
        auto& states = host.ref_states();
        if (target != 0) {
            position = world.ref_position(target);
            rotation = world.ref_rotation(target);
            MovedTo to;
            for (std::size_t i = 0; i < 3; ++i) {
                to.position[i] = position[i];
                to.rotation[i] = rotation[i];
            }
            states.moved[ref] = to;
        } else {
            states.moved.erase(ref);
            const RefPlacement placed = world.ref_placement(ref);
            position = placed.position;
            rotation = placed.rotation;
        }
        events.ref_moved(ref, position, rotation);
        states.disabled.erase(ref);
        host.clear_destruction(ref);
        host.set_disabled(ref, world.ref_placement(ref).disabled);
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsDisabled", [&world](NativeCall& c) {
        return result(Value::boolean(world.ref_disabled(c.self.form())));
    });
    v.bind("ObjectReference", "IsEnabled", [&world](NativeCall& c) {
        return result(Value::boolean(!world.ref_disabled(c.self.form())));
    });
    // Its model is built (the registry forgets nodes as they leave the tree)
    // and it is not disabled.
    v.bind("ObjectReference", "Is3DLoaded", [&world](NativeCall& c) {
        return result(Value::boolean(world.ref_3d_loaded(c.self.form())));
    });

    // ---- links, base, place
    // The link with this keyword (None: the link without one).
    v.bind("ObjectReference", "GetLinkedRef", [&world](NativeCall& c) {
        const std::uint32_t keyword = arg(c, 0).kind() == Kind::object ? arg(c, 0).form() : 0;
        const std::uint32_t target = world.linked_ref(c.self.form(), keyword);
        return target != 0 ? result(c.vm.object(target, "ObjectReference")) : NativeResult{};
    });
    v.bind("ObjectReference", "GetBaseObject", [&world](NativeCall& c) {
        return result(c.vm.object(world.ref_placement(c.self.form()).base, "Form"));
    });
    for (const char axis : {'X', 'Y', 'Z'}) {
        const auto index = static_cast<std::size_t>(axis - 'X');
        v.bind("ObjectReference", std::string("GetPosition") + axis, [&world, index](NativeCall& c) {
            return result(Value::floating(static_cast<float>(world.ref_position(c.self.form())[index])));
        });
        // Degrees, as the game reports them.
        v.bind("ObjectReference", std::string("GetAngle") + axis, [&world, index](NativeCall& c) {
            const Xyz r = world.ref_rotation(c.self.form());
            return result(Value::floating(static_cast<float>(r[index] * 180.0 / std::numbers::pi)));
        });
    }
    v.bind("ObjectReference", "GetScale", [&world](NativeCall& c) {
        return result(Value::floating(static_cast<float>(world.ref_placement(c.self.form()).scale)));
    });
    v.bind("ObjectReference", "GetDistance", [&world](NativeCall& c) {
        if (arg(c, 0).kind() != Kind::object) {
            return result(Value::floating(0.0F));
        }
        const Xyz a = world.ref_position(c.self.form());
        const Xyz b = world.ref_position(arg(c, 0).form());
        // In floats, as the positions are.
        const float dx = static_cast<float>(b[0]) - static_cast<float>(a[0]);
        const float dy = static_cast<float>(b[1]) - static_cast<float>(a[1]);
        const float dz = static_cast<float>(b[2]) - static_cast<float>(a[2]);
        return result(Value::floating(std::sqrt(dx * dx + dy * dy + dz * dz)));
    });

    // ---- animation
    v.bind("ObjectReference", "PlayAnimation", [&events](NativeCall& c) {
        events.play_animation(c.self.form(), Vm::to_string(arg(c, 0)));
        return result(Value::boolean(true));
    });
    // Waits for the named event (a text key of the animation) or a timeout.
    // The signal goes out after the wait is armed: a handler that reports the
    // event synchronously would otherwise be lost.
    v.bind("ObjectReference", "PlayAnimationAndWait", [&events](NativeCall& c) {
        const auto form = c.self.form();
        const std::string animation = Vm::to_string(arg(c, 0));
        return NativeResult{.value = Value::boolean(true),
                            .wait = k_animation_timeout,
                            .wait_for = "anim:" + std::to_string(c.self.form()) + ":" + Vm::to_string(arg(c, 1)),
                            .after = [&events, form, animation] { events.play_animation(form, animation); }};
    });
    // Physics dependencies between animated objects; nothing to do without physics.
    v.bind("ObjectReference", "AddDependentAnimatedObjectReference",
           [](NativeCall&) { return result(Value::boolean(true)); });
    v.bind("ObjectReference", "RemoveDependentAnimatedObjectReference",
           [](NativeCall&) { return result(Value::boolean(true)); });

    // ---- physics: the game layer owns the bodies
    // Direction in Skyrim's space, magnitude in Havok's (about kg m/s).
    v.bind("ObjectReference", "ApplyHavokImpulse", [&events](NativeCall& c) {
        const Xyz direction{as_float(number(arg(c, 0))), as_float(number(arg(c, 1))), as_float(number(arg(c, 2)))};
        events.havok_impulse(c.self.form(), direction, number(arg(c, 3)));
        return NativeResult{};
    });
    // PushActorAway(Actor, float force): the source knocks the actor over (a shout's push, a dragon's
    // blow); KnockAreaEffect(magnitude, radius): everything within the radius (game units) of the
    // source is knocked away from it. The game layer owns the bodies: it ragdolls and gets them up.
    v.bind("ObjectReference", "PushActorAway", [&events](NativeCall& c) {
        if (arg(c, 0).kind() != Kind::object) {
            return NativeResult{};
        }
        const double force = number(arg(c, 1));
        std::printf("papyrus: PushActorAway 0x%X pushes 0x%X (force %g)\n", c.self.form(), arg(c, 0).form(), force);
        events.push_actor(c.self.form(), arg(c, 0).form(), force);
        return NativeResult{};
    });
    v.bind("ObjectReference", "KnockAreaEffect", [&events](NativeCall& c) {
        const double magnitude = number(arg(c, 0));
        const double radius = number(arg(c, 1));
        std::printf("papyrus: KnockAreaEffect 0x%X (magnitude %g, radius %g)\n", c.self.form(), magnitude, radius);
        events.knock_area(c.self.form(), magnitude, radius);
        return NativeResult{};
    });
    // ObjectReference.psc: 1 dynamic, 2 sphere, 3 box, 6 thin box inertia
    // (all moving), 4 keyframed, 5 fixed, 7 character.
    v.bind("ObjectReference", "SetMotionType", [&events](NativeCall& c) {
        events.motion_type_changed(c.self.form(), arg(c, 0).i());
        return NativeResult{};
    });

    // ---- activation
    v.bind("ObjectReference", "Activate", [](NativeCall& c) {
        // Next VM update, on the VM's clock (and in its saves), not inside this call.
        const std::uint32_t activator = arg(c, 0).form() != 0 ? arg(c, 0).form() : k_player_ref;
        c.vm.schedule(0.0, "activate", c.self.form(), activator,
                      static_cast<std::uint32_t>(ActivationSource::script) | (Vm::truthy(arg(c, 1)) ? 0x100u : 0u));
        return result(Value::boolean(true));
    });
    v.bind("ObjectReference", "BlockActivation", [&host](NativeCall& c) {
        if (c.args.empty() || Vm::truthy(arg(c, 0))) {
            host.ref_states().blocked.insert(c.self.form());
        } else {
            host.ref_states().blocked.erase(c.self.form());
        }
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsActivationBlocked", [&host](NativeCall& c) {
        return result(Value::boolean(host.ref_states().blocked.contains(c.self.form())));
    });
    // No actors use furniture yet.
    v.bind("ObjectReference", "IsFurnitureInUse", [](NativeCall&) { return result(Value::boolean(false)); });
    // Followers are not simulated.
    v.bind("ObjectReference", "SetNoFavorAllowed", [](NativeCall&) { return NativeResult{}; });

    // ---- doors and locks
    v.bind("ObjectReference", "GetOpenState", [&host](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(host.open_state(c.self.form()))));
    });
    v.bind("ObjectReference", "SetOpen", [&host, &events](NativeCall& c) {
        const bool open = c.args.empty() || Vm::truthy(arg(c, 0));
        host.set_open_state(c.self.form(), open ? k_open : k_closed);
        events.open_changed(c.self.form(), open);
        return NativeResult{};
    });
    v.bind("ObjectReference", "Lock", [&host](NativeCall& c) {
        host.set_locked(c.self.form(), c.args.empty() || Vm::truthy(arg(c, 0)));
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsLocked", [&host](NativeCall& c) {
        return result(Value::boolean(host.is_locked(c.self.form())));
    });
    v.bind("ObjectReference", "GetLockLevel", [&host](NativeCall& c) {
        const auto level = host.lock_level(c.self.form());
        return result(Value::integer(static_cast<std::int32_t>(level < 0 ? 0 : level)));
    });
    // The key of the lock (from the data); None if the lock needs none or there is no lock.
    v.bind("ObjectReference", "GetKey", [&host](NativeCall& c) {
        const auto key = host.lock_key(c.self.form());
        return key != 0 ? result(c.vm.object(key, "Key")) : NativeResult{};
    });
    // MoveTo(target, x, y, z, matchRotation): to the target's place plus offsets in world axes. An actor moves in
    // the scene (relocate_ref); the player is not moved by scripts yet.
    v.bind("ObjectReference", "MoveTo", [&host](NativeCall& c) {
        if (arg(c, 0).kind() != Kind::object) {
            return NativeResult{};
        }
        const Xyz offset{as_float(number(arg(c, 1))), as_float(number(arg(c, 2))), as_float(number(arg(c, 3)))};
        host.move_ref_to(c.self.form(), arg(c, 0).form(), offset, c.args.size() < 5 || Vm::truthy(arg(c, 4)));
        return NativeResult{};
    });
    // There is no translation over time: TranslateTo and SplineTranslateTo put the reference at the end point
    // (position in units, angles in degrees) at once and report OnTranslationComplete, so a script that waits for
    // it goes on. Speed, rotation speed and the spline's tangent are ignored.
    for (const char* name : {"TranslateTo", "SplineTranslateTo"}) {
        v.bind("ObjectReference", name, [&host](NativeCall& c) {
            const auto at = [&](std::size_t i) { return as_float(number(arg(c, i))); };
            const auto angle = [&](std::size_t i) { return as_float(number(arg(c, i)) * (std::numbers::pi / 180.0)); };
            const std::uint32_t ref = c.self.form();
            host.relocate_ref(ref, Xyz{at(0), at(1), at(2)}, Xyz{angle(3), angle(4), angle(5)});
            host.send_event(ref, "OnTranslationComplete", {});
            return NativeResult{};
        });
    }

    // ---- inventory (bound here, reached from Actor through the parent class)
    // A FormList is not looked into (its forms are not reachable from here):
    // it counts as nothing and adds or removes nothing. The owner's start
    // inventory is resolved first (vm/papyrus_items.cpp); what runs out is
    // taken off.
    v.bind("ObjectReference", "GetItemCount", [&host](NativeCall& c) {
        if (arg(c, 0).kind() != Kind::object) {
            return result(Value::integer(0));
        }
        host.resolve_owner(c.self.form());
        return result(Value::integer(static_cast<std::int32_t>(c.vm.inventory().count(c.self.form(), arg(c, 0).form()))));
    });
    v.bind("ObjectReference", "AddItem", [&host](NativeCall& c) {
        if (arg(c, 0).kind() == Kind::object) {
            host.resolve_owner(c.self.form());
            c.vm.inventory().add(c.self.form(), arg(c, 0).form(), c.args.size() > 1 ? arg(c, 1).i() : 1);
        }
        return NativeResult{};
    });
    // Moves the items to `other` if given, else they are gone.
    v.bind("ObjectReference", "RemoveItem", [&host](NativeCall& c) {
        if (arg(c, 0).kind() == Kind::object) {
            // Plain units go first; extras (a tempered weapon) move with their stack.
            const auto taken = host.remove_item_split(c.self.form(), arg(c, 0).form(), c.args.size() > 1 ? arg(c, 1).i() : 1);
            if (arg(c, 3).kind() == Kind::object) {
                host.resolve_owner(arg(c, 3).form());
                for (const auto& part : taken) {
                    c.vm.inventory().add(arg(c, 3).form(), arg(c, 0).form(), part.count, part.extra);
                }
            }
        }
        return NativeResult{};
    });

    // ---- triggers and effects
    v.bind("ObjectReference", "GetTriggerObjectCount", [&host](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(host.trigger_count(c.self.form()))));
    });
}

} // namespace skydot::vm
