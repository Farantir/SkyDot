// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dialogue domain module (vm/dialogue_runtime.cpp) over fake hosts: who
// can talk, the NPC behind a reference, the INFO bookkeeping, the dice, what
// was said (save, adopt, reset) and the selection logic over an empty world.
#include "support/fake_hosts.hpp"

#include "vm/dialogue_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace skydot::testing;
using skydot::vm::DialogueRuntime;
namespace wfb = bethconv::pack::wfb;

namespace {

constexpr std::uint32_t k_guard = 0x30;

struct Host final : DialogueRuntime::Host {
    std::vector<std::uint8_t> bytes;
    bool has_world = true;
    FakeActors actors;
    Host() {
        flatbuffers::FlatBufferBuilder fbb;
        wfb::WorldBuilder world(fbb);
        wfb::FinishWorldBuffer(fbb, world.Finish());
        bytes.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
        actors.actors = {k_guard};
    }
    const wfb::World* world_root() const override { return has_world ? wfb::GetWorld(bytes.data()) : nullptr; }
    skydot::vm::ActorHost& actor_host() override { return actors; }
    std::uint32_t base_of(std::uint32_t ref) const override { return ref + 0x1000; }
};

} // namespace

TEST_CASE("only a living actor that is not the player can talk", "[dialogue][vm]") {
    Host host;
    DialogueRuntime d;
    d.bind(host);
    CHECK(d.can_talk(k_guard));
    CHECK_FALSE(d.can_talk(0x14)); // the player
    CHECK_FALSE(d.can_talk(0x99)); // not an actor
    host.actors.states.damage(k_guard, skydot::vm::av::k_health, 1000);
    CHECK_FALSE(d.can_talk(k_guard));
    host.actors.states.restore(k_guard, skydot::vm::av::k_health, 100);
    host.has_world = false;
    CHECK_FALSE(d.can_talk(k_guard));
}

TEST_CASE("the NPC of the player is 0x7, of anything else the host's base", "[dialogue][vm]") {
    Host host;
    DialogueRuntime d;
    d.bind(host);
    CHECK(d.npc_of(0x14) == 0x7);
    CHECK(d.npc_of(k_guard) == k_guard + 0x1000);
    CHECK(d.name_of(0).empty());
    CHECK(d.name_of(k_guard).empty()); // no such NPC_ record
}

TEST_CASE("the logic exists once there is a world and a host, and reset drops it", "[dialogue][vm]") {
    Host host;
    DialogueRuntime d;
    CHECK(d.logic() == nullptr); // no host yet
    d.bind(host);
    CHECK(d.existing() == nullptr);
    REQUIRE(d.logic() != nullptr);
    CHECK(d.existing() == d.logic());
    CHECK_FALSE(d.logic()->is_open());
    d.reset();
    CHECK(d.existing() == nullptr);
    host.has_world = false;
    CHECK(d.logic() == nullptr);
}

TEST_CASE("INFOs with attached fragments remember their quest", "[dialogue][vm]") {
    DialogueRuntime d;
    std::uint32_t quest = 0;
    CHECK_FALSE(d.info_attached(0x500));
    CHECK_FALSE(d.owning_quest(0x500, quest));
    d.set_info_quest(0x500, 0x100);
    CHECK(d.info_attached(0x500));
    REQUIRE(d.owning_quest(0x500, quest));
    CHECK(quest == 0x100);
    Host host;
    d.bind(host);
    d.reset();
    CHECK_FALSE(d.info_attached(0x500));
}

TEST_CASE("the dice are in [0, 1), deterministic, and go on across a reset", "[dialogue][vm]") {
    DialogueRuntime a;
    DialogueRuntime b;
    std::vector<double> first;
    for (int i = 0; i < 8; ++i) {
        first.push_back(a.random());
        CHECK(first.back() >= 0.0);
        CHECK(first.back() < 1.0);
        CHECK(b.random() == first.back());
    }
    a.reset();
    CHECK(a.random() != first.front()); // not rewound
}

TEST_CASE("what was said survives save and adopt; the speaker does not", "[dialogue][vm]") {
    Host host;
    DialogueRuntime d;
    d.bind(host);
    skydot::dialogue::DialogueStates said;
    said.mark_talked(k_guard);
    said.set_speaker(k_guard);
    REQUIRE(skydot::dialogue::DialogueStates::load(skydot::save::ByteReader(said.save()), said));
    d.adopt(std::move(said));
    CHECK(d.talked_to_player(k_guard));
    CHECK(d.speaker() == 0);
    CHECK(d.talked_count() == 1);
    const auto bytes = d.save();
    d.forget_talked(k_guard);
    CHECK_FALSE(d.talked_to_player(k_guard));
    skydot::dialogue::DialogueStates again;
    REQUIRE(skydot::dialogue::DialogueStates::load(skydot::save::ByteReader(bytes), again));
    d.adopt(std::move(again));
    CHECK(d.talked_to_player(k_guard));
    d.reset();
    CHECK(d.talked_count() == 0);
}
