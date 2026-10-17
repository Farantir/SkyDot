// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who may activate what (vm/activation.hpp): parent-only references and locks
// by source.
#include "vm/activation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <tuple>

using namespace skydot;

TEST_CASE("a parent-only reference refuses the player, not a script or a parent", "[activation]") {
    ActivationRequest r;
    r.parent_activate_only = true;
    r.source = ActivationSource::player;
    CHECK(activation_gate(r) == ActivationGate::parent_only);
    r.force = true; // picking locks does not make it answer
    CHECK(activation_gate(r) == ActivationGate::parent_only);
    r.source = ActivationSource::script;
    CHECK(activation_gate(r) == ActivationGate::pass);
    r.source = ActivationSource::parent;
    CHECK(activation_gate(r) == ActivationGate::pass);
}

TEST_CASE("a lock refuses the player unless picked, and only the player's pick unlocks", "[activation]") {
    ActivationRequest r;
    r.lock_level = 25;
    CHECK(activation_gate(r) == ActivationGate::locked);
    CHECK_FALSE(activation_unlocks(r));
    r.force = true;
    CHECK(activation_gate(r) == ActivationGate::pass);
    CHECK(activation_unlocks(r));
    for (const auto source : {ActivationSource::script, ActivationSource::parent}) {
        r.source = source;
        r.force = false;
        CHECK(activation_gate(r) == ActivationGate::pass);
        CHECK_FALSE(activation_unlocks(r));
        r.force = true; // not a pick
        CHECK_FALSE(activation_unlocks(r));
    }
    r.source = ActivationSource::player;
    r.force = true;
    r.lock_level = -1;
    CHECK_FALSE(activation_unlocks(r)); // nothing to unlock
}

TEST_CASE("lock levels have the game's names", "[activation]") {
    CHECK(lock_level_name(0) == "Novice");
    CHECK(lock_level_name(25) == "Apprentice");
    CHECK(lock_level_name(74) == "Adept");
    CHECK(lock_level_name(100) == "Master");
    CHECK(lock_level_name(255) == "requires a key");
}

TEST_CASE("furniture waits for its graph's ObjectActivated, only for the player", "[activation]") {
    CHECK(activation_waits_for_graph("FURN", ActivationSource::player, true));
    CHECK_FALSE(activation_waits_for_graph("FURN", ActivationSource::player, false)); // no such event: at once
    CHECK_FALSE(activation_waits_for_graph("FURN", ActivationSource::script, true));
    CHECK_FALSE(activation_waits_for_graph("FURN", ActivationSource::parent, true));
    CHECK_FALSE(activation_waits_for_graph("ACTI", ActivationSource::player, true));
}

TEST_CASE("a pending activation is taken once, and a second use meanwhile is refused", "[activation]") {
    PendingActivations pending;
    CHECK(pending.begin(0xAA8C3, {0x14, false}));
    CHECK(pending.pending(0xAA8C3));
    CHECK_FALSE(pending.begin(0xAA8C3, {0x14, true}));
    CHECK_FALSE(pending.take(0x1234).has_value());
    const auto entry = pending.take(0xAA8C3);
    REQUIRE(entry.has_value());
    CHECK(entry->activator == 0x14);
    CHECK_FALSE(entry->default_only);
    CHECK_FALSE(pending.take(0xAA8C3).has_value());
    CHECK(pending.begin(0xAA8C3, {0x14, false})); // usable again
}

TEST_CASE("loose items are picked up, other records are not", "[activation]") {
    for (const char* type : {"ALCH", "AMMO", "ARMO", "BOOK", "INGR", "KEYM", "MISC", "SLGM", "SCRL", "WEAP"}) {
        CHECK(activation_picks_up(type));
    }
    for (const char* type : {"DOOR", "FURN", "CONT", "ACTI", "NPC_", "STAT", ""}) {
        CHECK_FALSE(activation_picks_up(type));
    }
}

namespace {

/// A world of a few references for Activations.
struct FakeHost final : ActivationHost {
    std::map<std::uint32_t, ActivationTarget> targets;
    std::map<std::uint32_t, int> locks;
    std::set<std::uint32_t> raises;
    std::set<std::uint32_t> blocked;
    std::map<std::uint32_t, std::vector<Child>> children;
    std::vector<std::pair<std::uint32_t, std::int64_t>> activated;
    std::vector<std::tuple<double, std::uint32_t, std::uint32_t>> scheduled;
    std::vector<std::pair<std::uint32_t, std::int64_t>> picked;
    std::vector<std::uint32_t> unlocked;
    std::set<std::uint32_t> harvested_refs;
    HarvestResult yield;
    std::vector<std::uint32_t> harvests;

    bool harvested(std::uint32_t ref) const override { return harvested_refs.contains(ref); }
    HarvestResult harvest(std::uint32_t ref, std::int64_t) override {
        harvests.push_back(ref);
        harvested_refs.insert(ref);
        return yield;
    }

    bool target(std::uint32_t ref, ActivationTarget& out) const override {
        const auto it = targets.find(ref);
        if (it == targets.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
    int lock_level(std::uint32_t ref) const override {
        const auto it = locks.find(ref);
        return it != locks.end() ? it->second : -1;
    }
    void unlock(std::uint32_t ref) override {
        unlocked.push_back(ref);
        locks.erase(ref);
    }
    bool graph_raises_activation(std::uint32_t ref) const override { return raises.contains(ref); }
    std::int64_t send_activate(std::uint32_t ref, std::int64_t activator) override {
        activated.emplace_back(ref, activator);
        return 1;
    }
    bool activation_blocked(std::uint32_t ref) const override { return blocked.contains(ref); }
    std::vector<Child> activate_children(std::uint32_t ref) const override {
        const auto it = children.find(ref);
        return it != children.end() ? it->second : std::vector<Child>{};
    }
    void schedule_child(double delay, std::uint32_t child, std::uint32_t parent) override {
        scheduled.emplace_back(delay, child, parent);
    }
    void pick_up(std::uint32_t ref, std::int64_t activator, const ActivationTarget&) override {
        picked.emplace_back(ref, activator);
    }
};

ActivationTarget target_of(const char* type, const char* editor_id) {
    ActivationTarget t;
    t.type = type;
    t.editor_id = editor_id;
    return t;
}

} // namespace

TEST_CASE("an activation of an unknown reference is missing", "[activation]") {
    FakeHost host;
    Activations activations(host);
    const auto out = activations.activate(5, 0, false, ActivationSource::player, false);
    CHECK(out.kind == "missing");
    CHECK_FALSE(out.label.has_value());
}

TEST_CASE("the player's activation runs the scripts as the player, then the children", "[activation]") {
    FakeHost host;
    host.targets[0x10] = target_of("ACTI", "Lever");
    host.children[0x10] = {{0x20, 1.5}, {0x21, 0.0}};
    Activations activations(host);
    const auto out = activations.activate(0x10, 0, false, ActivationSource::player, false);
    CHECK(out.kind == "none");
    CHECK(out.scripts == 1);
    CHECK(*out.label == "0x00000010 Lever (ACTI)");
    REQUIRE(host.activated.size() == 1);
    CHECK(host.activated[0].second == k_player_ref);
    REQUIRE(host.scheduled.size() == 2);
    CHECK(std::get<1>(host.scheduled[0]) == 0x20);
    CHECK(std::get<2>(host.scheduled[0]) == 0x10);
    CHECK(std::get<0>(host.scheduled[0]) == 1.5);
}

TEST_CASE("a script that blocks activation keeps the children quiet", "[activation]") {
    FakeHost host;
    host.targets[0x10] = target_of("ACTI", "Lever");
    host.children[0x10] = {{0x20, 0.0}};
    host.blocked.insert(0x10);
    Activations activations(host);
    const auto out = activations.activate(0x10, 0, false, ActivationSource::player, false);
    CHECK(out.kind == "blocked");
    CHECK(host.scheduled.empty());
    CHECK(host.activated.size() == 1);
}

TEST_CASE("default_only skips OnActivate", "[activation]") {
    FakeHost host;
    host.targets[0x10] = target_of("ACTI", "Lever");
    Activations activations(host);
    const auto out = activations.activate(0x10, 0, true, ActivationSource::script, false);
    CHECK(out.scripts == 0);
    CHECK(host.activated.empty());
}

TEST_CASE("a lock stops the player, and a pick unlocks as it goes", "[activation]") {
    FakeHost host;
    host.targets[0x10] = target_of("DOOR", "Door");
    host.locks[0x10] = 50;
    Activations activations(host);
    const auto refused = activations.activate(0x10, 0, false, ActivationSource::player, false);
    CHECK(refused.kind == "locked");
    CHECK(*refused.lock_level == 50);
    CHECK(refused.note == "0x00000010 Door (DOOR) is locked (level 50, Adept)");
    CHECK(host.activated.empty());
    const auto picked = activations.activate(0x10, 0, false, ActivationSource::player, true);
    CHECK(picked.kind == "toggle_open");
    CHECK(host.unlocked == std::vector<std::uint32_t>{0x10});
}

TEST_CASE("a lever with a graph waits for ObjectActivated, once", "[activation]") {
    FakeHost host;
    host.targets[0x10] = target_of("FURN", "Lever");
    host.raises.insert(0x10);
    host.children[0x10] = {{0x20, 0.0}};
    Activations activations(host);
    const auto first = activations.activate(0x10, 0x30, false, ActivationSource::player, false);
    CHECK(first.kind == "furniture");
    CHECK(first.deferred);
    CHECK(first.event == "LeverPushStart");
    CHECK(host.activated.empty());
    CHECK(activations.pending(0x10));
    CHECK(activations.activate(0x10, 0, false, ActivationSource::player, false).kind == "busy");
    const auto done = activations.complete(0x10);
    REQUIRE(done.has_value());
    CHECK(done->kind == "none");
    CHECK(done->scripts == 1);
    CHECK(host.activated[0].second == 0x30);
    CHECK(host.scheduled.size() == 1);
    CHECK_FALSE(activations.complete(0x10).has_value());
}

TEST_CASE("an actor that activates a loose item picks it up", "[activation]") {
    FakeHost host;
    host.targets[0x10] = target_of("MISC", "Gold");
    Activations activations(host);
    const auto out = activations.activate(0x10, 0, false, ActivationSource::player, false);
    CHECK(out.kind == "pickup");
    REQUIRE(host.picked.size() == 1);
    CHECK(host.picked[0].second == k_player_ref);
}

TEST_CASE("a container's activation asks the host to open it, after the lock gate", "[activation]") {
    FakeHost host;
    host.targets[0x10] = target_of("CONT", "Chest");
    host.targets[0x11] = target_of("CONT", "LockedChest");
    host.locks[0x11] = 25;
    Activations activations(host);
    const auto open = activations.activate(0x10, 0, false, ActivationSource::player, false);
    CHECK(open.kind == "container");
    CHECK(host.picked.empty());
    CHECK(activations.activate(0x11, 0, false, ActivationSource::player, false).kind == "locked");
    CHECK(activations.activate(0x11, 0, false, ActivationSource::player, true).kind == "container");
}

TEST_CASE("a load door and furniture hand their kind to the host", "[activation]") {
    FakeHost host;
    host.targets[0x10] = target_of("DOOR", "LoadDoor");
    host.targets[0x10].load_door = true;
    host.targets[0x11] = target_of("FURN", "Chair");
    Activations activations(host);
    const auto door = activations.activate(0x10, 0, false, ActivationSource::player, false);
    CHECK(door.kind == "door");
    CHECK(door.door);
    const auto chair = activations.activate(0x11, 0, false, ActivationSource::player, false);
    CHECK(chair.kind == "furniture");
    CHECK_FALSE(chair.deferred);
}

TEST_CASE("a bench is crafting, never a lever, also when its graph raises ObjectActivated", "[activation]") {
    CHECK_FALSE(activation_waits_for_graph("FURN", ActivationSource::player, true, 1));
    CHECK(activation_waits_for_graph("FURN", ActivationSource::player, true, 0));
    FakeHost host;
    auto bench = target_of("FURN", "BlacksmithForge");
    bench.bench_type = 1;
    host.targets[0x10] = bench;
    host.raises.insert(0x10);
    Activations activations(host);
    const auto out = activations.activate(0x10, 0, false, ActivationSource::player, false);
    CHECK(out.kind == "crafting");
    CHECK(out.bench_type == 1);
    CHECK_FALSE(out.deferred);
    CHECK(out.event.empty());
    CHECK(out.scripts == 1); // its scripts still run
}

TEST_CASE("a plant gives its ingredient once, then answers already harvested", "[activation][harvest]") {
    FakeHost host;
    auto plant = target_of("FLOR", "Garlic");
    plant.harvestable = true;
    host.targets[0x20] = plant;
    host.yield = {0x34D22, 2, "Garlic", 0x99};
    Activations activations(host);
    const auto first = activations.activate(0x20, 0, false, ActivationSource::player, false);
    CHECK(first.kind == "harvest");
    CHECK(first.harvest_form == 0x34D22);
    CHECK(first.harvest_count == 2);
    CHECK(first.harvest_sound == 0x99);
    CHECK(first.note == "Garlic added");
    const auto second = activations.activate(0x20, 0, false, ActivationSource::player, false);
    CHECK(second.kind == "none");
    CHECK(second.note == "already harvested");
    CHECK(host.harvests.size() == 1);
}

TEST_CASE("a zero chance still harvests the plant, and a blocking script wins", "[activation][harvest]") {
    FakeHost host;
    auto plant = target_of("TREE", "Tree");
    plant.harvestable = true;
    host.targets[0x20] = plant;
    host.targets[0x21] = plant;
    host.blocked.insert(0x21);
    Activations activations(host);
    const auto nothing = activations.activate(0x20, 0, false, ActivationSource::player, false);
    CHECK(nothing.kind == "harvest");
    CHECK(nothing.harvest_count == 0);
    CHECK(host.harvested_refs.contains(0x20));
    const auto blocked = activations.activate(0x21, 0, false, ActivationSource::player, false);
    CHECK(blocked.kind == "blocked");
    CHECK_FALSE(host.harvested_refs.contains(0x21));
}
