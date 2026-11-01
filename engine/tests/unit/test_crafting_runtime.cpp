// SPDX-License-Identifier: GPL-3.0-or-later
//
// The crafting domain module (vm/crafting_runtime.cpp) over a world built in
// memory and a fake host: benches and keywords through the shared world
// access, the player's conditions in a world of their own, the rules on the
// host's inventory, and reset.
#include "support/fake_condition_world.hpp"
#include "support/fake_world.hpp"
#include "support/world_builder.hpp"

#include "vm/crafting_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::CraftingRuntime;
namespace wfb = bethconv::pack::wfb;

namespace {

constexpr std::uint32_t k_forge_kw = 0x100;
constexpr std::uint32_t k_forge = 0x2000; // a bench base, its own reference too
constexpr std::uint32_t k_ingot = 0x10;
constexpr std::uint32_t k_dagger = 0x13;
constexpr std::uint32_t k_make_dagger = 0x500;

WorldSpec small_world() {
    WorldSpec w;
    ItemSpec misc;
    misc.value = 5;
    ItemSpec weapon;
    weapon.category = wfb::ItemCategory::weapon;
    weapon.value = 20;
    w.bases = {
        {.id = k_forge, .name = "Forge", .keywords = {k_forge_kw}, .bench_type = 1},
        {.id = k_ingot, .name = "Iron Ingot", .item = misc},
        {.id = k_dagger, .name = "Iron Dagger", .item = weapon},
    };
    w.keywords = {{.id = k_forge_kw, .editor_id = "CraftingSmelter"}};
    w.recipes = {{.id = k_make_dagger, .product = k_dagger, .bench_keyword = k_forge_kw, .components = {{k_ingot, 2}}}};
    return w;
}

struct BenchWorld final : FakeWorld {
    std::uint32_t base_form(std::uint32_t ref) const override { return ref; }
    bool has_keyword(std::uint32_t form, std::uint32_t keyword) const override {
        return form == k_forge && keyword == k_forge_kw;
    }
    std::uint32_t find_keyword(const std::string& id) const override { return id == "CraftingSmelter" ? k_forge_kw : 0; }
};

struct Host final : CraftingRuntime::Host {
    BuiltWorld data{small_world()};
    bool has_world = true;
    BenchWorld world;
    skydot::vm::Inventory inventory_;
    skydot::vm::ItemExtras extras_;
    skydot::vm::CreatedForms created_;
    int condition_worlds = 0;
    std::vector<std::string> lines;

    const wfb::World* world_root() const override { return has_world ? &*data : nullptr; }
    skydot::vm::WorldAccess& world_access() override { return world; }
    skydot::vm::Inventory& inventory() override { return inventory_; }
    skydot::vm::ItemExtras& item_extras() override { return extras_; }
    skydot::vm::CreatedForms& created_forms() override { return created_; }
    std::unique_ptr<skydot::conditions::ConditionWorld> new_condition_world() override {
        ++condition_worlds;
        return std::make_unique<FakeConditionWorld>();
    }
    void announce(const std::string& line) override { lines.push_back(line); }
    double player_actor_value(std::uint32_t) override { return 15.0; }
    double perk_entry(skydot::perks::EntryPoint, const skydot::perks::EntryContext&, double value) override {
        return value;
    }
};

} // namespace

TEST_CASE("the rules exist with a host and a world, and reset drops them", "[crafting][vm]") {
    Host host;
    CraftingRuntime c;
    CHECK(c.logic() == nullptr); // no host
    c.bind(host);
    REQUIRE(c.logic() != nullptr);
    CHECK(c.logic() == c.logic());
    c.reset();
    host.has_world = false;
    CHECK(c.logic() == nullptr);
}

TEST_CASE("benches, keywords and logging go through the host", "[crafting][vm]") {
    Host host;
    CraftingRuntime c;
    c.bind(host);
    CHECK(c.bench_type(k_forge) == 1);
    CHECK(c.bench_type(0x9999) == 0);
    CHECK(c.bench_has_keyword(k_forge, k_forge_kw));
    CHECK_FALSE(c.bench_has_keyword(k_ingot, k_forge_kw));
    CHECK(c.keyword("CraftingSmelter") == k_forge_kw);
    CHECK(c.keyword("Nothing") == 0);
    c.log("hello");
    CHECK(host.lines == std::vector<std::string>{"hello"});
}

TEST_CASE("recipes and crafting run on the host's inventory, conditions in a world of their own", "[crafting][vm]") {
    Host host;
    CraftingRuntime c;
    c.bind(host);
    host.inventory_.add(0x14, k_ingot, 3);
    auto* logic = c.logic();
    REQUIRE(logic != nullptr);
    const auto rows = logic->recipes(k_forge);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].product == k_dagger);
    CHECK(rows[0].craftable);
    CHECK(logic->craft(k_make_dagger, 1) == 1);
    CHECK(host.inventory_.count(0x14, k_dagger) == 1);
    CHECK(host.inventory_.count(0x14, k_ingot) == 1);
    CHECK(host.condition_worlds >= 1);
}
