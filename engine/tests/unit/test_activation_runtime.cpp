// SPDX-License-Identifier: GPL-3.0-or-later
//
// The activation domain module (vm/activation_runtime.cpp) over a fake host:
// pickups, locks, activate children, blocked activations and harvests.
#include "support/fake_hosts.hpp"
#include "support/fake_world.hpp"
#include "support/world_builder.hpp"

#include "vm/activation_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <tuple>
#include <vector>

using namespace skydot::testing;
using skydot::ActivationSource;
using skydot::vm::ActivationRuntime;

namespace {

constexpr std::uint32_t k_misc_ref = 0x1000;
constexpr std::uint32_t k_misc_base = 0x20;
constexpr std::uint32_t k_chest_ref = 0x1001;
constexpr std::uint32_t k_plant_ref = 0x1002;
constexpr std::uint32_t k_plant_base = 0x30;
constexpr std::uint32_t k_ingredient = 0x40;
constexpr std::uint32_t k_lever_ref = 0x1003;

WorldSpec plant_world() {
    WorldSpec w;
    w.bases = {{.id = k_plant_base, .harvest = HarvestSpec{.ingredient = k_ingredient, .sound = 0x77, .seasonal_yield = 0x64646464}}};
    return w;
}

struct InfoWorld final : FakeWorld {
    std::uint32_t base_form(std::uint32_t ref) const override {
        const auto it = infos.find(ref);
        return it != infos.end() ? it->second.base : 0;
    }
};

struct FakeItems final : skydot::vm::ItemAccess {
    std::map<std::uint32_t, std::int64_t> dropped;
    mutable std::vector<std::uint32_t> resolved;
    void resolve_owner(std::uint32_t owner) const override { resolved.push_back(owner); }
    skydot::vm::ItemInfo item_info_of(std::uint32_t) const override { return {}; }
    std::string item_name_of(std::uint32_t form) const override { return "item " + std::to_string(form); }
    void refresh_equipment(std::uint32_t, std::uint32_t) override {}
    std::int64_t dropped_count(std::uint32_t ref) const override {
        const auto it = dropped.find(ref);
        return it != dropped.end() ? it->second : 0;
    }
    std::uint32_t dropped_extra(std::uint32_t) const override { return 0; }
};

struct Host final : ActivationRuntime::Host {
    BuiltWorld data{plant_world()};
    InfoWorld world;
    FakeRefs refs;
    skydot::vm::Inventory inventory_;
    skydot::vm::ActorStates actors_;
    FakeItems fake_items;
    std::vector<std::uint32_t> forgotten;
    std::vector<std::pair<std::uint32_t, std::int64_t>> activated;
    struct Scheduled {
        double delay;
        std::uint32_t child, parent;
    };
    std::vector<Scheduled> scheduled;

    const bethconv::pack::wfb::World* world_root() const override { return &*data; }
    skydot::vm::WorldAccess& world_access() override { return world; }
    skydot::vm::ReferenceHost& references() override { return refs; }
    skydot::vm::Inventory& inventory() override { return inventory_; }
    skydot::vm::ActorStates& actor_states() override { return actors_; }
    std::uint32_t placed_actor_base(std::uint32_t) const override { return 0; }
    bool graph_raises_activation(std::uint32_t) const override { return false; }
    std::int64_t send_activate(std::uint32_t ref, std::int64_t activator) override {
        activated.emplace_back(ref, activator);
        return 1;
    }
    void schedule_activation(double delay, std::uint32_t child, std::uint32_t parent) override {
        scheduled.push_back({delay, child, parent});
    }
    skydot::vm::ItemAccess& items() override { return fake_items; }
    std::optional<double> global_value_of(std::uint32_t) const override { return std::nullopt; }
    void forget_ref(std::uint32_t ref) override { forgotten.push_back(ref); }

    Host() {
        const auto add = [&](std::uint32_t ref, std::uint32_t base, const char* type) {
            skydot::vm::RefInfo info;
            info.base = base;
            info.type = type;
            world.infos[ref] = info;
        };
        add(k_misc_ref, k_misc_base, "MISC");
        add(k_chest_ref, 0x21, "ACTI");
        add(k_plant_ref, k_plant_base, "FLOR");
        add(k_lever_ref, 0x22, "ACTI");
    }
};

struct Rig {
    Host host;
    ActivationRuntime runtime;
    Rig() { runtime.bind(host); }
};

} // namespace

TEST_CASE("the player picks up a loose item and the reference goes", "[activation_runtime]") {
    Rig r;
    const auto out = r.runtime.activate(k_misc_ref, 0, false, ActivationSource::player, false);
    CHECK(out.kind == "pickup");
    CHECK(r.host.inventory_.count(skydot::k_player_ref, k_misc_base) == 1);
    REQUIRE(r.host.refs.disabled_calls.size() == 1);
    CHECK(r.host.refs.disabled_calls[0] == std::pair<std::uint32_t, bool>{k_misc_ref, true});
}

TEST_CASE("a dropped item gives its count and is forgotten", "[activation_runtime]") {
    Rig r;
    r.host.fake_items.dropped[k_misc_ref] = 5;
    r.runtime.activate(k_misc_ref, 0, false, ActivationSource::player, false);
    CHECK(r.host.inventory_.count(skydot::k_player_ref, k_misc_base) == 5);
    CHECK(r.host.forgotten == std::vector<std::uint32_t>{k_misc_ref});
    CHECK(r.host.refs.disabled_calls.empty());
}

TEST_CASE("activate children are scheduled with their delays", "[activation_runtime]") {
    Rig r;
    r.host.world.activate_child_lists[k_lever_ref] = {{0x2001, 0.0}, {0x2002, 1.5}};
    const auto out = r.runtime.activate(k_lever_ref, 0, false, ActivationSource::player, false);
    CHECK(out.scripts == 1);
    REQUIRE(r.host.scheduled.size() == 2);
    CHECK(r.host.scheduled[0].child == 0x2001);
    CHECK(r.host.scheduled[0].parent == k_lever_ref);
    CHECK(r.host.scheduled[1].child == 0x2002);
    CHECK(r.host.scheduled[1].delay == 1.5);
}

TEST_CASE("a blocked activation skips its children", "[activation_runtime]") {
    Rig r;
    r.host.world.activate_child_lists[k_lever_ref] = {{0x2001, 0.0}};
    r.host.refs.states.blocked.insert(k_lever_ref);
    const auto out = r.runtime.activate(k_lever_ref, 0, false, ActivationSource::player, false);
    CHECK(out.kind == "blocked");
    CHECK(r.host.scheduled.empty());
}

TEST_CASE("a locked reference refuses the player until the lock is picked", "[activation_runtime]") {
    Rig r;
    r.host.refs.lock_levels[k_chest_ref] = 50;
    r.host.refs.locked[k_chest_ref] = true;
    const auto refused = r.runtime.activate(k_chest_ref, 0, false, ActivationSource::player, false);
    CHECK(refused.kind == "locked");
    CHECK(refused.lock_level == 50);
    CHECK(r.host.refs.is_locked(k_chest_ref));
    const auto picked = r.runtime.activate(k_chest_ref, 0, false, ActivationSource::player, true);
    CHECK(picked.kind != "locked");
    CHECK_FALSE(r.host.refs.is_locked(k_chest_ref));
}

TEST_CASE("a plant is harvested once", "[activation_runtime]") {
    Rig r;
    const auto first = r.runtime.activate(k_plant_ref, 0, false, ActivationSource::player, false);
    CHECK(first.kind == "harvest");
    CHECK(first.harvest_form == k_ingredient);
    CHECK(first.harvest_count == 1);
    CHECK(first.harvest_sound == 0x77);
    CHECK(r.host.inventory_.count(skydot::k_player_ref, k_ingredient) == 1);
    CHECK(r.runtime.harvested(k_plant_ref));
    const auto second = r.runtime.activate(k_plant_ref, 0, false, ActivationSource::player, false);
    CHECK(second.kind != "harvest");
    CHECK(second.note.find("already harvested") != std::string::npos);
    CHECK(r.host.inventory_.count(skydot::k_player_ref, k_ingredient) == 1);
}

TEST_CASE("the harvest state is saved and adopted", "[activation_runtime]") {
    Rig r;
    r.runtime.activate(k_plant_ref, 0, false, ActivationSource::player, false);
    const auto bytes = r.runtime.save();
    skydot::vm::HarvestStates loaded;
    REQUIRE(skydot::vm::HarvestStates::load(skydot::save::ByteReader(bytes), loaded));
    Rig other;
    CHECK_FALSE(other.runtime.harvested(k_plant_ref));
    other.runtime.adopt(std::move(loaded));
    CHECK(other.runtime.harvested(k_plant_ref));
    other.runtime.reset();
    CHECK_FALSE(other.runtime.harvested(k_plant_ref));
}
