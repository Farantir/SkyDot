// SPDX-License-Identifier: GPL-3.0-or-later
//
// Actor natives beyond values (vm/natives_actor_extra.cpp), over FakeWorld,
// FakeEvents and FakeActors.
#include "support/fake_events.hpp"
#include "support/fake_hosts.hpp"
#include "support/fake_world.hpp"
#include "support/native_calls.hpp"
#include "support/vm_rig.hpp"

#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Kind;
using skydot::vm::Value;

namespace {

constexpr std::uint32_t k_actor = 0x100;
constexpr std::uint32_t k_other = 0x101;
constexpr std::uint32_t k_faction = 0x900;
constexpr std::uint32_t k_faction2 = 0x901;

void add_classes(Rig& rig) {
    rig.add({.name = "Form"});
    rig.add({.name = "Quest"});
    rig.add({.name = "WorldSpace"});
    rig.add({.name = "ObjectReference",
             .functions = {native("GetCurrentLocation", {}, "Location"), native("GetWorldSpace", {}, "WorldSpace")}});
    rig.add({.name = "Actor",
             .parent = "ObjectReference",
             .functions = {native("IsInFaction", {{"f", "Faction"}}, "Bool"),
                           native("GetFactionRank", {{"f", "Faction"}}, "Int"),
                           native("SetFactionRank", {{"f", "Faction"}, {"r", "Int"}}),
                           native("ModFactionRank", {{"f", "Faction"}, {"r", "Int"}}),
                           native("RemoveFromFaction", {{"f", "Faction"}}), native("RemoveFromAllFactions"),
                           native("Kill", {{"k", "Actor"}}), native("EvaluatePackage"),
                           native("SetRelationshipRank", {{"x", "Actor"}, {"r", "Int"}}),
                           native("GetRelationshipRank", {{"x", "Actor"}}, "Int"),
                           native("SetGhost", {{"g", "Bool"}}), native("IsGhost", {}, "Bool"),
                           native("PlaySubGraphAnimation", {{"a", "String"}})}});
    rig.add({.name = "ActorBase",
             .functions = {native("SetEssential", {{"e", "Bool"}}), native("IsUnique", {}, "Bool")}});
    rig.add({.name = "Faction", .functions = {native("SetEnemy", {{"x", "Faction"}, {"a", "Bool"}, {"b", "Bool"}}),
                                               native("SetAlly", {{"x", "Faction"}, {"a", "Bool"}, {"b", "Bool"}})}});
    rig.add({.name = "Location", .functions = {native("IsLoaded", {}, "Bool"), native("IsChild", {{"x", "Location"}}, "Bool")}});
    rig.add({.name = "Package", .functions = {native("GetOwningQuest", {}, "Quest")}});
    rig.add({.name = "VisualEffect", .functions = {native("Play", {{"t", "ObjectReference"}, {"d", "Float"}, {"x", "ObjectReference"}}),
                                                    native("Stop", {{"t", "ObjectReference"}})}});

    const auto a = [](const char* name, const char* fn, std::vector<Param> extra = {}, const char* ret = "None") {
        return wrap(name, "Actor", fn, std::move(extra), ret);
    };
    const auto fac = [](const char* name, const char* fn, std::vector<Param> extra = {}, const char* ret = "None") {
        return wrap(name, "Faction", fn, std::move(extra), ret);
    };
    auto kill_nobody = global_function("killnobody", {{"x", "Actor"}});
    kill_nobody.add(skydot::vm::Op::callmethod, {ident("Kill"), ident("o"), ident("::NoneVar"), ident("::NoneVar")});
    rig.add(script(
        "Caller",
        {a("infaction", "IsInFaction", {{"f", "Faction"}}, "Bool"), a("rank", "GetFactionRank", {{"f", "Faction"}}, "Int"),
         a("setrank", "SetFactionRank", {{"f", "Faction"}, {"r", "Int"}}),
         a("modrank", "ModFactionRank", {{"f", "Faction"}, {"r", "Int"}}),
         a("removefaction", "RemoveFromFaction", {{"f", "Faction"}}), a("removeall", "RemoveFromAllFactions"),
         a("kill", "Kill", {{"k", "Actor"}}), a("evaluate", "EvaluatePackage"),
         a("setrel", "SetRelationshipRank", {{"x", "Actor"}, {"r", "Int"}}),
         a("getrel", "GetRelationshipRank", {{"x", "Actor"}}, "Int"), a("setghost", "SetGhost", {{"g", "Bool"}}),
         a("isghost", "IsGhost", {}, "Bool"), a("subgraph", "PlaySubGraphAnimation", {{"a", "String"}}),
         wrap("location", "ObjectReference", "GetCurrentLocation", {}, "Location"),
         wrap("space", "ObjectReference", "GetWorldSpace", {}, "WorldSpace"),
         wrap("unique", "ActorBase", "IsUnique", {}, "Bool"),
         wrap("essential", "ActorBase", "SetEssential", {{"e", "Bool"}}),
         fac("enemy", "SetEnemy", {{"x", "Faction"}, {"a", "Bool"}, {"b", "Bool"}}),
         fac("ally", "SetAlly", {{"x", "Faction"}, {"a", "Bool"}, {"b", "Bool"}}),
         wrap("loaded", "Location", "IsLoaded", {}, "Bool"),
         wrap("ischild", "Location", "IsChild", {{"x", "Location"}}, "Bool"),
         wrap("owner", "Package", "GetOwningQuest", {}, "Quest"),
         wrap("fxplay", "VisualEffect", "Play", {{"t", "ObjectReference"}, {"d", "Float"}, {"x", "ObjectReference"}}),
         wrap("fxstop", "VisualEffect", "Stop", {{"t", "ObjectReference"}}), kill_nobody}));
}

struct Fixture {
    FakeWorld world;
    FakeEvents events;
    FakeActors host;
    Rig rig;
    Fixture() {
        add_classes(rig);
        host.actors = {k_actor, k_other, 0x14};
        skydot::vm::bind_actor_extra_natives(rig.vm, world, events, host);
    }
    Value obj(std::uint32_t form, const char* cls = "Actor") { return rig.vm.object(form, cls); }
    Value call(const char* fn, std::vector<Value> args) { return rig.call("caller", fn, std::move(args)); }
};

} // namespace

TEST_CASE("faction ranks: the NPC_'s, then what scripts set, then removal", "[vm][natives]") {
    Fixture f;
    f.host.world.factions[k_actor][k_faction] = 2;
    // A faction the script never names (None) answers not-a-member.
    CHECK(f.call("infaction", {f.obj(k_actor), Value()}).b() == false);
    CHECK(f.call("rank", {f.obj(k_actor), Value()}).i() == -1);

    // The fake condition world only knows the NPC_'s ranks; scripts write to the actor states.
    CHECK(f.call("rank", {f.obj(k_actor), f.obj(k_faction, "Faction")}).i() == 2);
    CHECK(f.call("infaction", {f.obj(k_actor), f.obj(k_faction, "Faction")}).b());
    CHECK_FALSE(f.call("infaction", {f.obj(k_actor), f.obj(k_faction2, "Faction")}).b());
    CHECK(f.call("rank", {f.obj(k_actor), f.obj(k_faction2, "Faction")}).i() == -1);

    f.call("setrank", {f.obj(k_actor), f.obj(k_faction2, "Faction"), Int(3)});
    CHECK(f.host.states.faction_override(k_actor, k_faction2) == 3);
    f.call("setrank", {f.obj(k_actor), Value(), Int(4)}); // None: nothing
    CHECK(f.host.states.added_factions(k_actor).size() == 1);

    // ModFactionRank adds to the current rank; only for a member.
    f.call("modrank", {f.obj(k_actor), f.obj(k_faction, "Faction"), Int(5)});
    CHECK(f.host.states.faction_override(k_actor, k_faction) == 7);
    f.call("modrank", {f.obj(k_actor), f.obj(k_faction2, "Faction"), Int(1)}); // fake world: not a member
    CHECK(f.host.states.faction_override(k_actor, k_faction2) == 3);

    f.call("removefaction", {f.obj(k_actor), f.obj(k_faction, "Faction")});
    CHECK(f.host.states.faction_override(k_actor, k_faction) == -1);
}

TEST_CASE("RemoveFromAllFactions removes every faction the actor is in", "[vm][natives]") {
    Fixture f;
    f.host.world.factions[k_actor] = {{k_faction, 1}, {k_faction2, 0}};
    f.call("removeall", {f.obj(k_actor)});
    CHECK(f.host.states.faction_override(k_actor, k_faction) == -1);
    CHECK(f.host.states.faction_override(k_actor, k_faction2) == -1);
    CHECK_FALSE(f.host.states.faction_override(k_other, k_faction).has_value());
}

TEST_CASE("SetEnemy and SetAlly write both directions with the plain or flagged reaction", "[vm][natives]") {
    Fixture f;
    f.call("enemy", {f.obj(k_faction, "Faction"), f.obj(k_faction2, "Faction"), Bool(false), Bool(true)});
    CHECK(f.host.states.faction_reaction(k_faction, k_faction2) == 1);
    CHECK(f.host.states.faction_reaction(k_faction2, k_faction) == 0);
    f.call("ally", {f.obj(k_faction, "Faction"), f.obj(k_faction2, "Faction"), Bool(true), Bool(false)});
    CHECK(f.host.states.faction_reaction(k_faction, k_faction2) == 3);
    CHECK(f.host.states.faction_reaction(k_faction2, k_faction) == 2);
    // None: nothing changes.
    f.call("enemy", {f.obj(k_faction, "Faction"), Value(), Bool(true), Bool(true)});
    CHECK(f.host.states.faction_reaction(k_faction, k_faction2) == 3);
}

TEST_CASE("relationship ranks: a script's wins, else the RELA record on the Papyrus scale, None is 0", "[vm][natives]") {
    Fixture f;
    CHECK(f.call("getrel", {f.obj(k_actor), Value()}).i() == 0);
    CHECK(f.call("getrel", {f.obj(k_actor), f.obj(k_other)}).i() == 0); // nothing known

    f.host.world.npcs = {{k_actor, 0x5000}, {k_other, 0x5001}};
    f.host.world.relationships[{0x5000, 0x5001}] = 1; // the record's scale: 4 - 1
    CHECK(f.call("getrel", {f.obj(k_actor), f.obj(k_other)}).i() == 3);

    f.call("setrel", {f.obj(k_actor), f.obj(k_other), Int(-2)});
    CHECK(f.call("getrel", {f.obj(k_actor), f.obj(k_other)}).i() == -2);
    CHECK(f.call("getrel", {f.obj(k_other), f.obj(k_actor)}).i() == -2); // both ways round
    f.call("setrel", {f.obj(k_actor), Value(), Int(4)});                  // None: nothing
}

TEST_CASE("ghosts and the base's essential and unique flags", "[vm][natives]") {
    Fixture f;
    CHECK_FALSE(f.call("isghost", {f.obj(k_actor)}).b());
    f.call("setghost", {f.obj(k_actor), Bool(true)});
    CHECK(f.call("isghost", {f.obj(k_actor)}).b());
    f.call("setghost", {f.obj(k_actor), Bool(false)});
    CHECK_FALSE(f.call("isghost", {f.obj(k_actor)}).b());

    f.call("essential", {f.obj(k_actor + 0x1000, "ActorBase"), Bool(true)}); // the NPC_ of k_actor
    CHECK(f.host.states.is_essential(k_actor));
    CHECK_FALSE(f.host.states.is_essential(k_other));
    f.world.unique_npcs = {0x5000};
    CHECK(f.call("unique", {f.obj(0x5000, "ActorBase")}).b());
    CHECK_FALSE(f.call("unique", {f.obj(0x5001, "ActorBase")}).b());
}

TEST_CASE("Kill kills with the killer the script names; the player's killer is told to the host", "[vm][natives]") {
    Fixture f;
    f.call("kill", {f.obj(k_actor), f.obj(k_other)});
    CHECK(f.host.states.is_dead(k_actor));
    CHECK(f.host.states.killer_of(k_actor) == k_other);
    CHECK(f.host.last_damagers.empty());

    f.call("killnobody", {f.obj(k_other)});
    CHECK(f.host.states.killer_of(k_other) == 0);

    f.call("kill", {f.obj(0x14), f.obj(k_actor)});
    REQUIRE(f.host.last_damagers.size() == 1);
    CHECK(f.host.last_damagers[0] == k_actor);
}

TEST_CASE("EvaluatePackage and Location.IsLoaded ask the host", "[vm][natives]") {
    Fixture f;
    f.call("evaluate", {f.obj(k_actor)});
    REQUIRE(f.host.evaluated.size() == 1);
    CHECK(f.host.evaluated[0] == k_actor);

    f.host.loaded_locations = {0x800};
    CHECK(f.call("loaded", {f.obj(0x800, "Location")}).b());
    CHECK_FALSE(f.call("loaded", {f.obj(0x801, "Location")}).b());
}

TEST_CASE("Location.IsChild walks the tree upwards, and stops on a loop", "[vm][natives]") {
    Fixture f;
    f.host.world.location_parents = {{0x10, 0x20}, {0x20, 0x30}, {0x40, 0x41}, {0x41, 0x40}};
    const auto loc = [&f](std::uint32_t id) { return f.obj(id, "Location"); };
    CHECK(f.call("ischild", {loc(0x10), loc(0x30)}).b());
    CHECK_FALSE(f.call("ischild", {loc(0x30), loc(0x10)}).b());
    CHECK_FALSE(f.call("ischild", {loc(0x10), Value()}).b());
    CHECK_FALSE(f.call("ischild", {loc(0x40), loc(0x99)}).b()); // a loop ends after 32 steps
}

TEST_CASE("GetCurrentLocation and GetWorldSpace go through the reference's cell; None without one", "[vm][natives]") {
    Fixture f;
    f.host.world.cells[k_actor] = 0x300;
    f.host.world.cell_locations[0x300] = 0x800;
    f.host.world.cell_worlds[0x300] = 0x3C;
    CHECK(f.call("location", {f.obj(k_actor)}).form() == 0x800);
    CHECK(f.call("space", {f.obj(k_actor)}).form() == 0x3C);
    CHECK(f.call("location", {f.obj(k_other)}).kind() == Kind::none);
    CHECK(f.call("space", {f.obj(k_other)}).kind() == Kind::none);
    f.host.world.cells[k_other] = 0x301; // an interior: no worldspace, no location
    CHECK(f.call("location", {f.obj(k_other)}).kind() == Kind::none);
    CHECK(f.call("space", {f.obj(k_other)}).kind() == Kind::none);
}

TEST_CASE("Package.GetOwningQuest reads the record; effects and sub graph animations go out as notices", "[vm][natives]") {
    Fixture f;
    f.world.package_quests[0xA00] = 0xB00;
    CHECK(f.call("owner", {f.obj(0xA00, "Package")}).form() == 0xB00);
    CHECK(f.call("owner", {f.obj(0xA01, "Package")}).kind() == Kind::none);

    f.call("fxplay", {f.obj(0xC00, "VisualEffect"), f.obj(0xD00, "ObjectReference"), Float(2.5F), f.obj(0xE00, "ObjectReference")});
    f.call("fxstop", {f.obj(0xC00, "VisualEffect"), f.obj(0xD00, "ObjectReference")});
    REQUIRE(f.events.effects.size() == 2);
    CHECK(f.events.effects[0].ref == 0xC00);
    CHECK(f.events.effects[0].art == 0xD00);
    CHECK(f.events.effects[0].target == 0xE00);
    CHECK(f.events.effects[0].duration == 2.5);
    CHECK(f.events.effects[0].play);
    CHECK_FALSE(f.events.effects[1].play);
    CHECK(f.events.effects[1].target == 0);

    f.call("subgraph", {f.obj(k_actor), String("Idle")});
    REQUIRE(f.events.sub_graphs.size() == 1);
    CHECK(f.events.sub_graphs[0] == std::pair<std::uint32_t, std::string>{k_actor, "Idle"});
}
