// SPDX-License-Identifier: GPL-3.0-or-later
//
// Trigger volumes and quest states (vm/triggers.hpp, vm/quest_states.hpp):
// crossings, and the save round trips.
#include "vm/quest_states.hpp"
#include "vm/triggers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

using namespace skydot;
using skydot::vm::Affine;
using skydot::vm::QuestStates;
using skydot::vm::Trigger;
using skydot::vm::Triggers;
using skydot::vm::Vec3;
using skydot::vm::Volume;

namespace {

Trigger box_at(Vec3 origin, Vec3 half) {
    Trigger t;
    t.origin = origin;
    t.half = half;
    t.type = Trigger::k_box;
    return t;
}

struct Crossings {
    std::vector<std::pair<std::uint32_t, bool>> events;
    void move(Triggers& triggers, std::uint32_t actor, Vec3 position,
              std::function<bool(std::uint32_t)> notices = [](std::uint32_t) { return true; }) {
        triggers.move_actor(actor, position, notices,
                            [this](std::uint32_t trigger, bool entered) { events.emplace_back(trigger, entered); });
    }
};

} // namespace

TEST_CASE("an actor entering and leaving a box is reported once each", "[triggers]") {
    Triggers triggers;
    triggers.set_primitive(7, box_at({100, 0, 0}, {10, 10, 10}));
    Crossings log;
    log.move(triggers, 0x14, {0, 0, 0});
    CHECK(log.events.empty());
    log.move(triggers, 0x14, {105, 5, -5});
    log.move(triggers, 0x14, {106, 5, -5}); // still inside
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0] == std::pair<std::uint32_t, bool>{7, true});
    CHECK(triggers.inside_count(7) == 1);
    log.move(triggers, 0x14, {0, 0, 0});
    REQUIRE(log.events.size() == 2);
    CHECK_FALSE(log.events[1].second);
    CHECK(triggers.inside_count(7) == 0);
    REQUIRE(triggers.actor_position(0x14) != nullptr);
}

TEST_CASE("a sphere's radius is its x half extent", "[triggers]") {
    Triggers triggers;
    Trigger t = box_at({0, 0, 0}, {10, 0, 0});
    t.type = Trigger::k_sphere;
    triggers.set_primitive(1, t);
    Crossings log;
    log.move(triggers, 5, {8, 5, 0}); // 9.4 from the centre
    CHECK(triggers.inside_count(1) == 1);
    log.move(triggers, 5, {8, 8, 0}); // 11.3
    CHECK(triggers.inside_count(1) == 0);
}

TEST_CASE("a trigger that does not notice is skipped on entering, never on leaving", "[triggers]") {
    Triggers triggers;
    triggers.set_primitive(7, box_at({0, 0, 0}, {10, 10, 10}));
    Crossings log;
    log.move(triggers, 1, {0, 0, 0}, [](std::uint32_t) { return false; });
    CHECK(log.events.empty());
    CHECK(triggers.inside_count(7) == 0);
    log.move(triggers, 1, {0, 0, 0});
    CHECK(triggers.inside_count(7) == 1);
    log.move(triggers, 1, {50, 0, 0}, [](std::uint32_t) { return false; });
    CHECK(triggers.inside_count(7) == 0);
}

TEST_CASE("phantom volumes are asked in their own frame and survive a new primitive", "[triggers]") {
    Triggers triggers;
    Volume v;
    v.to_local.origin = {-100, 0, 0}; // the volume stands at x = 100
    v.half = {5, 5, 5};
    triggers.set_volumes(3, {v});
    CHECK_FALSE(triggers.triggers().at(3).primitive);
    Crossings log;
    log.move(triggers, 1, {102, 0, 0});
    CHECK(triggers.inside_count(3) == 1);
    triggers.set_primitive(3, box_at({0, 0, 0}, {1, 1, 1}));
    CHECK(triggers.triggers().at(3).volumes.size() == 1);
    triggers.set_volumes(3, {}); // nothing to set: unchanged
    CHECK(triggers.triggers().at(3).volumes.size() == 1);
}

TEST_CASE("forgetting a reference drops it as a trigger and as an actor", "[triggers]") {
    Triggers triggers;
    triggers.set_primitive(7, box_at({0, 0, 0}, {1, 1, 1}));
    Crossings log;
    log.move(triggers, 7, {50, 0, 0});
    triggers.forget(7);
    CHECK_FALSE(triggers.has(7));
    CHECK(triggers.actor_position(7) == nullptr);
}

TEST_CASE("the inside sets and actors round trip; unknown triggers are dropped on restore", "[triggers][save]") {
    Triggers triggers;
    triggers.set_primitive(7, box_at({0, 0, 0}, {10, 10, 10}));
    Crossings log;
    log.move(triggers, 0x14, {1, 2, 3});
    save::ByteWriter w;
    triggers.write(w);
    save::ByteReader r(w.out.data(), w.out.size());
    Triggers::Saved saved;
    REQUIRE(Triggers::read(r, saved));
    CHECK(r.at_end());
    Triggers fresh;
    fresh.set_primitive(7, box_at({0, 0, 0}, {10, 10, 10}));
    fresh.restore(std::move(saved));
    CHECK(fresh.inside_count(7) == 1);
    REQUIRE(fresh.actor_position(0x14) != nullptr);
    CHECK(fresh.actor_position(0x14)->z == 3.0F);
    // A cut-short section is refused.
    save::ByteReader cut(w.out.data(), w.out.size() - 1);
    Triggers::Saved ignored;
    CHECK_FALSE(Triggers::read(cut, ignored));
}

TEST_CASE("quest states round trip with aliases, fills and globals", "[quests][save]") {
    QuestStates quests;
    auto& q = quests.state(0x100);
    q.running = true;
    q.stage = 20;
    q.done = {10, 20};
    q.objectives[1] = 3;
    quests.mark_scripts_attached(0x100);
    quests.add_handle(0x100, 4, quests.next_handle++);
    quests.add_handle(0x100, 2, quests.next_handle++);
    quests.set_fill(quests.find_handle(0x100, 4), 0x55);
    quests.set_global(0x9, 2.5);

    QuestStates loaded;
    const auto bytes = quests.save();
    REQUIRE(QuestStates::load(save::ByteReader(bytes), loaded));
    REQUIRE(loaded.find(0x100) != nullptr);
    CHECK(loaded.find(0x100)->stage == 20);
    CHECK(loaded.find(0x100)->done.size() == 2);
    CHECK(loaded.scripts_attached(0x100));
    CHECK(loaded.fill(loaded.find_handle(0x100, 4)) == 0x55);
    CHECK(loaded.fill(loaded.find_handle(0x100, 2)) == 0);
    CHECK(*loaded.global(0x9) == 2.5);
    CHECK(loaded.next_handle == quests.next_handle);
    CHECK(loaded.owner(loaded.find_handle(0x100, 2))->second == 2);
    // The handles come back in the order they were made, not by alias id.
    const auto handles = loaded.handles_of(0x100);
    REQUIRE(handles.size() == 2);
    CHECK(handles[0] < handles[1]);
}

TEST_CASE("clearing a quest's fills leaves the other quests'", "[quests]") {
    QuestStates quests;
    quests.add_handle(1, 0, 0xA);
    quests.add_handle(2, 0, 0xB);
    quests.set_fill(0xA, 5);
    quests.set_fill(0xB, 6);
    quests.clear_fills(1);
    CHECK(quests.fill(0xA) == 0);
    CHECK(quests.fill(0xB) == 6);
}

TEST_CASE("a damaged QUST section is refused", "[quests][save]") {
    QuestStates quests;
    quests.state(1).running = true;
    auto bytes = quests.save();
    bytes.push_back(0); // left over
    QuestStates loaded;
    CHECK_FALSE(QuestStates::load(save::ByteReader(bytes), loaded));
    bytes.resize(bytes.size() - 5);
    CHECK_FALSE(QuestStates::load(save::ByteReader(bytes), loaded));
}
