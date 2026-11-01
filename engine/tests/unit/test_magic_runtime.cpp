// SPDX-License-Identifier: GPL-3.0-or-later
//
// The magic domain module (vm/magic_runtime.cpp) over a world built in memory and a fake host:
// a self cast starts and ends an effect (with its events), forgetting an actor drops its effects, the
// MAGC section round trips, and a summon is placed in front of its caster.
#include "support/fake_condition_world.hpp"
#include "support/fake_world.hpp"
#include "support/vm_rig.hpp"
#include "support/world_builder.hpp"

#include "vm/magic_runtime.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace skydot::testing;
using skydot::vm::MagicRuntime;
namespace wfb = bethconv::pack::wfb;
namespace magic = skydot::magic;
using Catch::Approx;

namespace {

constexpr std::uint32_t k_player = 0x14;
constexpr std::uint32_t k_fortify = 0x1004;
constexpr std::uint32_t k_summon_effect = 0x1005;
constexpr std::uint32_t k_buff = 0x2001;
constexpr std::uint32_t k_summon_spell = 0x2002;
constexpr std::uint32_t k_wolf_npc = 0x700;
constexpr std::uint32_t k_wolf_ref = 0xFF000001;

WorldSpec small_world() {
    WorldSpec s;
    s.format_version = 36;
    MagicEffectSpec fortify{.id = k_fortify};
    fortify.archetype = 34;
    fortify.actor_value = static_cast<std::int32_t>(skydot::vm::av::k_health);
    fortify.flags = magic::mgef_flag::recover;
    fortify.casting_type = 0;
    MagicEffectSpec summon{.id = k_summon_effect};
    summon.archetype = 18;
    summon.associated_item = k_wolf_npc;
    s.magic_effects = {fortify, summon};
    SpellSpec buff{.id = k_buff};
    buff.casting_type = 1;
    buff.effects = {{.effect = k_fortify, .magnitude = 25.0F, .duration = 5}};
    SpellSpec call{.id = k_summon_spell};
    call.casting_type = 1;
    call.effects = {{.effect = k_summon_effect, .magnitude = 1.0F, .duration = 30}};
    s.spells = {buff, call};
    return s;
}

struct ItemsHost final : skydot::vm::ItemRuntime::Host {
    skydot::vm::WorldAccess* access = nullptr;
    skydot::vm::Inventory* inv = nullptr;
    skydot::vm::CreatedForms created;
    skydot::vm::Vm* the_vm = nullptr;
    const wfb::World* root = nullptr;
    const wfb::World* world_root() const override { return root; }
    skydot::vm::WorldAccess& world_access() override { return *access; }
    skydot::vm::Inventory& inventory() override { return *inv; }
    skydot::vm::CreatedForms& created_forms() override { return created; }
    skydot::vm::Vm& vm() override { return *the_vm; }
    void dispatch_event(std::uint32_t, std::string_view, const std::vector<skydot::vm::Value>&) override {}
    std::uint32_t placed_actor_base(std::uint32_t) const override { return 0; }
    std::optional<double> global_value_of(std::uint32_t) const override { return std::nullopt; }
    void sync_worn_magic(std::uint32_t) override {}
    void worn_changed(std::uint32_t) override {}
    std::optional<bool> apply_consumed(std::uint32_t, std::uint32_t, std::uint32_t) override { return std::nullopt; }
    void note_eaten(std::uint32_t) override {}
    void log(const std::string&) override {}
    void item_trace(const std::string&) override {}
};

struct Host final : MagicRuntime::Host {
    BuiltWorld data{small_world()};
    FakeWorld world;
    skydot::vm::Inventory inventory_;
    skydot::vm::ActorStates actors;
    skydot::vm::CreatedForms created_;
    Rig rig;
    FakeConditionWorld condition_world;
    std::vector<std::pair<std::uint32_t, bool>> events; // (mgef, started)
    std::vector<std::string> traces;
    std::vector<skydot::vm::PlacedRef> placed;
    std::vector<std::uint32_t> forgotten;

    Host() {
        actors.set_source([](std::uint32_t ref, skydot::vm::ActorBase& out) {
            if (ref != k_player && ref != k_wolf_ref) {
                return false;
            }
            out = skydot::vm::ActorBase{};
            out.base[skydot::vm::av::k_health] = 100;
            return true;
        });
        world.positions[k_player] = {1000.0, 2000.0, 30.0};
        world.rotations[k_player] = {0.0, 0.0, 1.5707963267948966}; // facing +x
    }
    skydot::vm::WorldAccess& world_access() override { return world; }
    skydot::vm::Inventory& inventory() override { return inventory_; }
    skydot::vm::ActorStates& actor_states() override { return actors; }
    skydot::vm::CreatedForms& created_forms() override { return created_; }
    skydot::vm::Vm& vm() override { return rig.vm; }
    std::unique_ptr<skydot::conditions::ConditionWorld> new_condition_world() override {
        return std::make_unique<FakeConditionWorld>();
    }
    void forget_ref(std::uint32_t ref) override { forgotten.push_back(ref); }
    std::uint32_t dialogue_npc(std::uint32_t) const override { return 0; }
    std::uint32_t place_summon_ref(const skydot::vm::PlacedRef& p) override {
        placed.push_back(p);
        return k_wolf_ref;
    }
    std::vector<std::uint32_t> triggered_actors() const override { return {}; }
    void magic_trace(const std::string& line) override { traces.push_back(line); }
    void magic_notification(const std::string&) override {}
    void effect_event(const magic::EffectView& view, bool started) override { events.emplace_back(view.mgef, started); }
    void attach_effect_scripts(std::uint32_t, std::uint32_t) override {}
};

struct Fixture {
    Host host;
    ItemsHost items_host;
    skydot::vm::ItemRuntime items;
    skydot::vm::CombatRuntime combat;
    MagicRuntime magic;
    Fixture() : magic(host, *host.data, items, combat) {
        items_host.access = &host.world;
        items_host.inv = &host.inventory_;
        items_host.the_vm = &host.rig.vm;
        items_host.root = &*host.data;
        items.bind(items_host);
    }
};

} // namespace

TEST_CASE("a self cast starts an effect, the tick ends it, each told once", "[magic_runtime]") {
    Fixture f;
    CHECK(f.magic.cast(k_buff, k_player, 0, 0));
    f.magic.flush();
    REQUIRE(f.host.events.size() == 1);
    CHECK(f.host.events[0] == std::pair<std::uint32_t, bool>{k_fortify, true});
    CHECK(f.magic.effects.has_effect(k_player, k_fortify));
    f.magic.flush(); // nothing new
    CHECK(f.host.events.size() == 1);
    f.magic.tick(6.0);
    REQUIRE(f.host.events.size() == 2);
    CHECK(f.host.events[1] == std::pair<std::uint32_t, bool>{k_fortify, false});
    CHECK_FALSE(f.magic.effects.has_effect(k_player, k_fortify));
}

TEST_CASE("forgetting an actor drops its effects without telling", "[magic_runtime]") {
    Fixture f;
    REQUIRE(f.magic.cast(k_buff, k_player, 0, 0));
    f.magic.flush();
    REQUIRE(f.magic.effects.has_effect(k_player, k_fortify));
    f.host.rig.vm.forgets().forget(k_player);
    CHECK_FALSE(f.magic.effects.has_effect(k_player, k_fortify));
    CHECK(f.magic.effects.entries() == 0);
}

TEST_CASE("the MAGC section round trips through decode and adopt", "[magic_runtime]") {
    Fixture f;
    REQUIRE(f.magic.cast(k_buff, k_player, 0, 0));
    f.magic.flush();
    const auto bytes = f.magic.save();
    std::vector<std::uint8_t> effects, book, shouts;
    REQUIRE(MagicRuntime::decode(*f.host.data, skydot::save::ByteReader(bytes), effects, book, shouts));
    Fixture other;
    other.magic.adopt(true, effects, book, shouts);
    CHECK(other.magic.effects.has_effect(k_player, k_fortify));
    CHECK(other.magic.save() == bytes);
    other.magic.adopt(false, {}, {}, {});
    CHECK_FALSE(other.magic.effects.has_effect(k_player, k_fortify));
}

TEST_CASE("a summon is placed in front of its caster", "[magic_runtime]") {
    Fixture f;
    REQUIRE(f.magic.cast(k_summon_spell, k_player, 0, 0));
    f.magic.flush();
    REQUIRE(f.host.placed.size() == 1);
    const auto& p = f.host.placed[0];
    CHECK(p.base == k_wolf_npc);
    CHECK(p.position[0] == Approx(1000.0 + 150.0));
    CHECK(p.position[1] == Approx(2000.0).margin(1e-6));
    CHECK(p.position[2] == Approx(30.0));
    CHECK(p.rotation[2] == Approx(1.5707963267948966));
    CHECK(p.scale == 1.0);
}
