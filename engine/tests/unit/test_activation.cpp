// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who may activate what (vm/activation.hpp): parent-only references and locks
// by source.
#include "vm/activation.hpp"

#include <catch2/catch_test_macros.hpp>

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
