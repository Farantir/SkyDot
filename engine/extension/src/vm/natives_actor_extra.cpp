// SPDX-License-Identifier: GPL-3.0-or-later
//
// What scripts do to an actor beyond its values (step 7b-7, the dragon's death): factions at runtime,
// ghosts, a visual effect or a sub graph animation (notices for the game layer), arrows in the body, and
// the worldspace a reference stands in.
#include "vm/activation.hpp"
#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

namespace skydot::vm {

void bind_actor_extra_natives(Vm& v, const WorldAccess& world, WorldEvents& events, ActorHost& host) {
    using namespace natives;
    const auto flag = [](bool x) { return NativeResult{.value = Value::boolean(x), .wait = -1.0}; };
    const auto faction_of = [](NativeCall& c) { return arg(c, 0).kind() == Kind::object ? arg(c, 0).form() : 0U; };

    // ---- factions: the NPC_'s, then what scripts did (ActorStates)
    const auto rank_of = [&host](std::uint32_t ref, std::uint32_t faction) {
        return host.conditions().faction_rank(ref, faction).value_or(-1);
    };
    v.bind("Actor", "IsInFaction", [faction_of, rank_of, flag](NativeCall& c) {
        const std::uint32_t faction = faction_of(c);
        return flag(faction != 0 && rank_of(c.self.form(), faction) >= 0);
    });
    v.bind("Actor", "GetFactionRank", [faction_of, rank_of](NativeCall& c) {
        const std::uint32_t faction = faction_of(c);
        return NativeResult{.value = Value::integer(faction != 0 ? rank_of(c.self.form(), faction) : -1), .wait = -1.0};
    });
    v.bind("Actor", "SetFactionRank", [&host, faction_of](NativeCall& c) {
        if (const std::uint32_t faction = faction_of(c); faction != 0) {
            host.actor_states().set_faction_rank(c.self.form(), faction, arg(c, 1).i());
        }
        return NativeResult{};
    });
    v.bind("Actor", "ModFactionRank", [&host, faction_of, rank_of](NativeCall& c) {
        const std::uint32_t faction = faction_of(c);
        const std::int32_t rank = faction != 0 ? rank_of(c.self.form(), faction) : -1;
        if (rank >= 0) {
            host.actor_states().set_faction_rank(c.self.form(), faction, rank + arg(c, 1).i());
        }
        return NativeResult{};
    });
    v.bind("Actor", "RemoveFromFaction", [&host, faction_of](NativeCall& c) {
        if (const std::uint32_t faction = faction_of(c); faction != 0) {
            host.actor_states().set_faction_rank(c.self.form(), faction, -1);
        }
        return NativeResult{};
    });
    v.bind("Actor", "RemoveFromAllFactions", [&host](NativeCall& c) {
        for (const std::uint32_t faction : host.conditions().factions_of(c.self.form())) {
            host.actor_states().set_faction_rank(c.self.form(), faction, -1);
        }
        return NativeResult{};
    });

    // ---- Kill: the same death as a blow that kills (flush_deaths announces it: OnDying, OnDeath), even for an
    // essential or invulnerable actor; the killer is who the script names (None: nobody).
    v.bind("Actor", "Kill", [&host](NativeCall& c) {
        const std::uint32_t ref = c.self.form();
        const std::uint32_t killer = arg(c, 0).kind() == Kind::object ? arg(c, 0).form() : 0U;
        if (ref == k_player_ref) {
            host.set_last_damager(killer);
        }
        host.actor_states().kill(ref, killer);
        return NativeResult{};
    });
    // EvaluatePackage: the AI chooses the package again at once.
    v.bind("Actor", "EvaluatePackage", [&host](NativeCall& c) {
        host.evaluate_package(c.self.form());
        return NativeResult{};
    });

    // ---- relationships: what a script set wins over the RELA records (GetRelationshipRank condition and the
    // combat AI read it too). Papyrus ranks: 4 lover ... 0 acquaintance ... -4 archnemesis.
    v.bind("Actor", "SetRelationshipRank", [&host](NativeCall& c) {
        if (arg(c, 0).kind() == Kind::object) {
            host.actor_states().set_relationship_rank(c.self.form(), arg(c, 0).form(), arg(c, 1).i());
        }
        return NativeResult{};
    });
    v.bind("Actor", "GetRelationshipRank", [&host](NativeCall& c) {
        if (arg(c, 0).kind() != Kind::object) {
            return NativeResult{.value = Value::integer(0), .wait = -1.0};
        }
        const std::uint32_t a = c.self.form();
        const std::uint32_t b = arg(c, 0).form();
        if (const auto set = host.actor_states().relationship_rank(a, b)) {
            return NativeResult{.value = Value::integer(*set), .wait = -1.0};
        }
        auto& cw = host.conditions();
        const auto raw = cw.relationship_rank(cw.npc_of(a), cw.npc_of(b));
        return NativeResult{.value = Value::integer(raw ? 4 - *raw : 0), .wait = -1.0};
    });

    // ---- flags on the base
    v.bind("ActorBase", "SetEssential", [&host](NativeCall& c) {
        host.actor_states().set_base_essential(c.self.form(), Vm::truthy(arg(c, 0)));
        return NativeResult{};
    });
    v.bind("ActorBase", "SetInvulnerable", [&host](NativeCall& c) {
        host.actor_states().set_base_invulnerable(c.self.form(), Vm::truthy(arg(c, 0)));
        return NativeResult{};
    });
    v.bind("ActorBase", "IsUnique", [&world, flag](NativeCall& c) { return flag(world.npc_unique(c.self.form())); });

    // ---- faction relations: how members of this faction see the other's (the AI reads them, they are saved).
    // SetEnemy(other, selfNeutral, otherNeutral); SetAlly(other, selfFriend, otherFriend).
    const auto relate = [&host, faction_of](NativeCall& c, std::uint8_t plain, std::uint8_t flagged) {
        if (const std::uint32_t other = faction_of(c); other != 0) {
            host.actor_states().set_faction_reaction(c.self.form(), other, Vm::truthy(arg(c, 1)) ? flagged : plain);
            host.actor_states().set_faction_reaction(other, c.self.form(), Vm::truthy(arg(c, 2)) ? flagged : plain);
        }
        return NativeResult{};
    };
    v.bind("Faction", "SetEnemy", [relate](NativeCall& c) { return relate(c, 1, 0); });
    v.bind("Faction", "SetAlly", [relate](NativeCall& c) { return relate(c, 2, 3); });

    // ---- locations (the tree is the data's; a cell is in the location its record names)
    v.bind("Location", "IsLoaded", [&host, flag](NativeCall& c) { return flag(host.location_loaded(c.self.form())); });
    // Whether this location lies below the other in the tree.
    v.bind("Location", "IsChild", [&host, flag](NativeCall& c) {
        if (arg(c, 0).kind() != Kind::object) {
            return flag(false);
        }
        auto& cw = host.conditions();
        std::uint32_t at = cw.location_parent(c.self.form());
        for (int depth = 0; at != 0 && depth < 32; ++depth) {
            if (at == arg(c, 0).form()) {
                return flag(true);
            }
            at = cw.location_parent(at);
        }
        return flag(false);
    });

    // The quest a package belongs to; None for none.
    v.bind("Package", "GetOwningQuest", [&world](NativeCall& c) {
        const std::uint32_t quest = world.package_owner_quest(c.self.form());
        return NativeResult{.value = quest != 0 ? c.vm.object(quest, "Quest") : Value{}, .wait = -1.0};
    });

    // ---- ghosts take no hits (hit_facts, magic landing)
    v.bind("Actor", "SetGhost", [&host](NativeCall& c) {
        host.actor_states().set_ghost(c.self.form(), !c.args.empty() && Vm::truthy(c.args[0]));
        return NativeResult{};
    });
    v.bind("Actor", "IsGhost", [&host, flag](NativeCall& c) { return flag(host.actor_states().is_ghost(c.self.form())); });

    // ---- effects and animations the game layer shows (placeholders there)
    v.bind("VisualEffect", "Play", [&events](NativeCall& c) {
        events.visual_effect(c.self.form(), arg(c, 0).form(), arg(c, 2).form(), number(arg(c, 1)), true);
        return NativeResult{};
    });
    v.bind("VisualEffect", "Stop", [&events](NativeCall& c) {
        events.visual_effect(c.self.form(), arg(c, 0).form(), 0, 0.0, false);
        return NativeResult{};
    });
    v.bind("Actor", "PlaySubGraphAnimation", [&events](NativeCall& c) {
        events.sub_graph_animation(c.self.form(), Vm::to_string(arg(c, 0)));
        return NativeResult{};
    });

    // Arrows stuck in a body are not simulated: nothing to clear.
    v.bind("Actor", "ClearExtraArrows", [](NativeCall&) { return NativeResult{}; });

    // ObjectReference.GetCurrentLocation: the location of the reference's cell; None without one.
    v.bind("ObjectReference", "GetCurrentLocation", [&host](NativeCall& c) {
        auto& cw = host.conditions();
        const std::uint32_t cell = cw.cell_of(c.self.form());
        const std::uint32_t location = cell != 0 ? cw.cell_location(cell) : 0;
        return NativeResult{.value = location != 0 ? c.vm.object(location, "Location") : Value{}, .wait = -1.0};
    });

    // ObjectReference.GetWorldSpace: the worldspace of the reference's cell; None in an interior.
    v.bind("ObjectReference", "GetWorldSpace", [&host](NativeCall& c) {
        auto& cw = host.conditions();
        const std::uint32_t cell = cw.cell_of(c.self.form());
        const std::uint32_t space = cell != 0 ? cw.cell_world(cell) : 0;
        return NativeResult{.value = space != 0 ? c.vm.object(space, "WorldSpace") : Value{}, .wait = -1.0};
    });
}

} // namespace skydot::vm
