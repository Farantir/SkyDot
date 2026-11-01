// SPDX-License-Identifier: GPL-3.0-or-later
//
// vm::ProgressionRuntime (vm/progression_runtime.hpp): the player's progress, perks as an NPC_'s PRKR
// changed by overrides, the PRGS section, the player level, and the HasPerk and GetLevel conditions.
#include "support/fake_condition_world.hpp"
#include "support/world_builder.hpp"

#include "ai/packages.hpp"
#include "conditions/functions.hpp"
#include "data/leveled.hpp"
#include "vm/progression_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
namespace vm = skydot::vm;
namespace leveled = skydot::leveled;
namespace conditions = skydot::conditions;

namespace {

constexpr std::uint32_t k_player = 0x14;
constexpr std::uint32_t k_guard = 0x1000;     // a placed actor of NPC_ k_guard_base
constexpr std::uint32_t k_guard_base = 0x2000;
constexpr std::uint32_t k_clone = 0x1001;     // a placed actor of a NPC_ that takes the guard's perks
constexpr std::uint32_t k_clone_base = 0x2001;
constexpr std::uint32_t k_perk_a = 0x500;
constexpr std::uint32_t k_perk_b = 0x501;
constexpr std::uint32_t k_perk_c = 0x502;

class Rig final : public vm::ProgressionRuntime::Host {
public:
    Rig()
        : world(WorldSpec{
              .npcs = {{.id = 0x7, .level = 4, .perks = {{k_perk_a, 1}}},
                       {.id = k_guard_base, .perks = {{k_perk_a, 2}, {k_perk_b, 1}}},
                       {.id = k_clone_base,
                        .template_id = k_guard_base,
                        .template_flags = static_cast<wfb::NpcTemplateFlags>(0x8),
                        .perks = {{k_perk_c, 1}}}}}) {
        runtime.bind(*this);
        runtime.clear();
    }
    const wfb::World* world_root() const override { return &*world; }
    std::uint32_t base_of(std::uint32_t ref) const override {
        switch (ref) {
        case k_player: return 0x7;
        case k_guard: return k_guard_base;
        case k_clone: return k_clone_base;
        default: return 0;
        }
    }
    BuiltWorld world;
    vm::ProgressionRuntime runtime;
};

} // namespace

TEST_CASE("the player starts at the level of the Player NPC_", "[progression]") {
    Rig rig;
    CHECK(rig.runtime.player().level == 4);
    CHECK(leveled::player_level() == 4);
    CHECK(rig.runtime.player().perk_points == 0);
    CHECK(rig.runtime.player().xp == 0.0);
}

TEST_CASE("player_level follows set_level, load and clear", "[progression]") {
    Rig rig;
    rig.runtime.set_level(12);
    CHECK(leveled::player_level() == 12);
    const auto bytes = rig.runtime.save();
    rig.runtime.clear();
    CHECK(leveled::player_level() == 4);
    vm::ProgressionState state;
    REQUIRE(vm::ProgressionRuntime::load(skydot::save::ByteReader(bytes), state, 1));
    rig.runtime.adopt(std::move(state));
    CHECK(leveled::player_level() == 12);
    rig.runtime.adopt(std::nullopt); // an older save
    CHECK(leveled::player_level() == 4);
}

TEST_CASE("PRGS round-trips every field and the perk overrides", "[progression]") {
    Rig rig;
    auto& rt = rig.runtime;
    rt.set_skill_xp(0, 12.5);
    rt.set_skill_xp(17, 99.25);
    rt.set_xp(7.5);
    rt.set_level(9);
    rt.set_perk_points(3);
    rt.set_trainings(2);
    rt.set_legendary(5, 2);
    rt.mark_book_read(0x700);
    rt.mark_book_read(0x701);
    rt.add_perk(k_guard, k_perk_c, 3);
    rt.remove_perk(k_guard, k_perk_b);
    const auto expected = rt.player();
    const auto bytes = rt.save();

    Rig other;
    vm::ProgressionState state;
    REQUIRE(vm::ProgressionRuntime::load(skydot::save::ByteReader(bytes), state, 1));
    other.runtime.adopt(std::move(state));
    CHECK(other.runtime.player() == expected);
    CHECK(other.runtime.perk_rank(k_guard, k_perk_c) == 3);
    CHECK(other.runtime.perk_rank(k_guard, k_perk_b) == 0);
    CHECK(other.runtime.perk_rank(k_guard, k_perk_a) == 2);
    CHECK(other.runtime.is_book_read(0x701));
    CHECK_FALSE(other.runtime.is_book_read(0x702));

    // A damaged or newer section is refused.
    vm::ProgressionState refused;
    CHECK_FALSE(vm::ProgressionRuntime::load(skydot::save::ByteReader(bytes), refused, 2));
    auto shortened = bytes;
    shortened.pop_back();
    CHECK_FALSE(vm::ProgressionRuntime::load(skydot::save::ByteReader(shortened), refused, 1));
}

TEST_CASE("base PRKR, add and remove give the effective ranks", "[progression]") {
    Rig rig;
    auto& rt = rig.runtime;
    CHECK(rt.perk_rank(k_player, k_perk_a) == 1); // the Player NPC_'s own
    CHECK(rt.perk_rank(k_guard, k_perk_a) == 2);
    CHECK(rt.has_perk(k_guard, k_perk_b));
    CHECK_FALSE(rt.has_perk(k_guard, k_perk_c));
    rt.add_perk(k_guard, k_perk_c);
    rt.add_perk(k_guard, k_perk_a, 3);
    rt.remove_perk(k_guard, k_perk_b);
    CHECK(rt.perk_rank(k_guard, k_perk_c) == 1);
    CHECK(rt.perk_rank(k_guard, k_perk_a) == 3);
    CHECK_FALSE(rt.has_perk(k_guard, k_perk_b));
    const auto all = rt.perks_of(k_guard);
    REQUIRE(all.size() == 2);
    CHECK(all[0] == std::make_pair(k_perk_a, std::uint8_t{3}));
    CHECK(all[1] == std::make_pair(k_perk_c, std::uint8_t{1}));
    CHECK(rt.perks_of(0x9999).empty());
}

TEST_CASE("perks come from the template with the spell list flag", "[progression]") {
    Rig rig;
    CHECK(rig.runtime.perk_rank(k_clone, k_perk_a) == 2);
    CHECK(rig.runtime.perk_rank(k_clone, k_perk_b) == 1);
    CHECK(rig.runtime.perk_rank(k_clone, k_perk_c) == 0); // its own are replaced
}

TEST_CASE("forget drops an actor's overrides", "[progression]") {
    Rig rig;
    rig.runtime.add_perk(k_guard, k_perk_c);
    rig.runtime.remove_perk(k_guard, k_perk_a);
    CHECK(rig.runtime.override_count() == 1);
    rig.runtime.forget(k_guard);
    CHECK(rig.runtime.override_count() == 0);
    CHECK(rig.runtime.perk_rank(k_guard, k_perk_a) == 2);
    CHECK(rig.runtime.perk_rank(k_guard, k_perk_c) == 0);
}

TEST_CASE("HasPerk and GetLevel answer from the world", "[progression][conditions]") {
    FakeConditionWorld w;
    w.perk_ranks[{k_guard, k_perk_a}] = 2;
    w.levels[k_player] = 8;
    const auto pass = [&](ConditionSpec c, std::uint32_t subject) {
        const BuiltWorld built(WorldSpec{.packages = {{.id = 1, .conditions = {c}}}});
        const auto* package = skydot::ai::find_package(*built, 1);
        conditions::ConditionContext ctx;
        ctx.subject = subject;
        return conditions::conditions_pass(package->conditions(), w, ctx, nullptr);
    };
    CHECK(pass(condition(conditions::fn::has_perk, eq, 1.0F, 0, k_perk_a), k_guard));
    CHECK_FALSE(pass(condition(conditions::fn::has_perk, eq, 1.0F, 0, k_perk_b), k_guard));
    CHECK_FALSE(pass(condition(conditions::fn::has_perk, eq, 1.0F, 0, k_perk_a), k_player));
    CHECK(pass(condition(conditions::fn::get_level, ge, 8.0F), k_player));
    CHECK_FALSE(pass(condition(conditions::fn::get_level, gt, 8.0F), k_player));
}
