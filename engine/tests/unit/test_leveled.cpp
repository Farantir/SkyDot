// SPDX-License-Identifier: GPL-3.0-or-later
//
// Leveled lists (data/leveled.cpp): every rule of the resolver with fixed
// seeds on worlds built in memory, and the generator itself.
#include "support/world_builder.hpp"

#include "data/leveled.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace skydot::testing;
namespace lv = skydot::leveled;

namespace {

constexpr std::uint32_t k_lvli = 0x494C564C; // "LVLI"
constexpr std::uint32_t k_list = 0x500;
constexpr std::uint32_t k_inner = 0x501;
constexpr std::uint32_t k_a = 0x10;
constexpr std::uint32_t k_b = 0x11;
constexpr std::uint32_t k_c = 0x12;
constexpr std::uint32_t k_global = 0x900;

wfb::LeveledListFlags flags_of(bool all_levels, bool each, bool use_all) {
    wfb::LeveledListFlags f{};
    if (all_levels) {
        f = f | wfb::LeveledListFlags::from_all_levels;
    }
    if (each) {
        f = f | wfb::LeveledListFlags::each_entry;
    }
    if (use_all) {
        f = f | wfb::LeveledListFlags::use_all;
    }
    return f;
}

std::vector<lv::Entry> run(const WorldSpec& spec, std::uint32_t list, std::uint16_t level, std::uint64_t seed,
                           const lv::GlobalValue& global = {}) {
    const BuiltWorld world(spec);
    lv::Rng rng(seed);
    return lv::resolve(*world, list, level, rng, global);
}

std::set<std::uint32_t> forms(const std::vector<lv::Entry>& entries) {
    std::set<std::uint32_t> out;
    for (const auto& e : entries) {
        out.insert(e.form);
    }
    return out;
}

/// Every form that comes up over seeds 1..n.
std::set<std::uint32_t> seen(const WorldSpec& spec, std::uint32_t list, std::uint16_t level, int n = 64) {
    std::set<std::uint32_t> out;
    for (int seed = 1; seed <= n; ++seed) {
        const auto got = forms(run(spec, list, level, static_cast<std::uint64_t>(seed)));
        out.insert(got.begin(), got.end());
    }
    return out;
}

WorldSpec with_list(wfb::LeveledListFlags flags, std::vector<LeveledListSpec::Entry> entries,
                    std::uint8_t chance_none = 0) {
    WorldSpec spec;
    spec.leveled_lists = {{.id = k_list, .type = k_lvli, .flags = flags, .chance_none = chance_none,
                           .entries = std::move(entries)}};
    return spec;
}

} // namespace

TEST_CASE("the generator is splitmix64", "[leveled][rng]") {
    // The reference outputs of splitmix64 seeded with 0.
    lv::Rng rng(0);
    CHECK(rng.next() == 0xE220A8397B1DCDAFULL);
    CHECK(rng.next() == 0x6E789E6AA1B965F4ULL);
    CHECK(rng.next() == 0x06C45D188009454FULL);
}

TEST_CASE("below stays in range, and a seed is the same every time", "[leveled][rng]") {
    lv::Rng rng(7);
    for (int i = 0; i < 1000; ++i) {
        CHECK(rng.below(6) < 6);
    }
    CHECK(rng.below(0) == 0);
    CHECK(rng.below(1) == 0);

    CHECK(lv::seed_for(0x800) == lv::seed_for(0x800));
    CHECK(lv::seed_for(0x800) != lv::seed_for(0x801));
    CHECK(lv::seed_for(0x800, lv::k_purpose_inventory) != lv::seed_for(0x800, lv::k_purpose_actor));
}

TEST_CASE("one entry is picked with one draw of the generator", "[leveled]") {
    const auto spec = with_list({}, {{1, k_a}, {1, k_b}, {1, k_c}});
    for (const std::uint64_t seed : {1ULL, 2ULL, 99ULL}) {
        lv::Rng probe(seed);
        const std::uint32_t index = probe.below(3);
        const auto got = run(spec, k_list, 1, seed);
        REQUIRE(got.size() == 1);
        CHECK(got[0].form == std::vector<std::uint32_t>{k_a, k_b, k_c}[index]);
        CHECK(got[0].count == 1);
    }
    // Every entry comes up over enough seeds.
    CHECK(seen(spec, k_list, 1) == std::set<std::uint32_t>{k_a, k_b, k_c});
    // And the result is the same for the same seed.
    CHECK(run(spec, k_list, 1, 5)[0].form == run(spec, k_list, 1, 5)[0].form);
}

TEST_CASE("without 'from all levels' only the highest eligible level is used", "[leveled]") {
    const auto spec = with_list({}, {{1, k_a}, {5, k_b}, {10, k_c}});
    CHECK(seen(spec, k_list, 1) == std::set<std::uint32_t>{k_a});
    CHECK(seen(spec, k_list, 7) == std::set<std::uint32_t>{k_b});
    CHECK(seen(spec, k_list, 10) == std::set<std::uint32_t>{k_c});
    CHECK(seen(spec, k_list, 50) == std::set<std::uint32_t>{k_c});
}

TEST_CASE("with 'from all levels' every eligible entry can come up", "[leveled]") {
    const auto spec = with_list(flags_of(true, false, false), {{1, k_a}, {5, k_b}, {10, k_c}});
    CHECK(seen(spec, k_list, 1) == std::set<std::uint32_t>{k_a});
    CHECK(seen(spec, k_list, 7) == std::set<std::uint32_t>{k_a, k_b});
    CHECK(seen(spec, k_list, 10) == std::set<std::uint32_t>{k_a, k_b, k_c});
}

TEST_CASE("a list whose entries are all above the level uses the lowest ones", "[leveled]") {
    const auto spec = with_list({}, {{5, k_a}, {5, k_b}, {9, k_c}});
    CHECK(seen(spec, k_list, 1) == std::set<std::uint32_t>{k_a, k_b});
}

TEST_CASE("a list without entries, or no list, gives nothing", "[leveled]") {
    const auto spec = with_list({}, {});
    CHECK(run(spec, k_list, 1, 1).empty());
    CHECK(run(spec, 0x777, 1, 1).empty());
}

TEST_CASE("'use all' takes every candidate", "[leveled]") {
    // At the top eligible level only...
    auto spec = with_list(flags_of(false, false, true), {{1, k_a}, {1, k_b}, {5, k_c}});
    CHECK(forms(run(spec, k_list, 3, 1)) == std::set<std::uint32_t>{k_a, k_b});
    CHECK(forms(run(spec, k_list, 5, 1)) == std::set<std::uint32_t>{k_c});
    // ... or from all levels.
    spec = with_list(flags_of(true, false, true), {{1, k_a}, {1, k_b}, {5, k_c}});
    CHECK(forms(run(spec, k_list, 5, 1)) == std::set<std::uint32_t>{k_a, k_b, k_c});
    CHECK(forms(run(spec, k_list, 1, 1)) == std::set<std::uint32_t>{k_a, k_b});
}

TEST_CASE("an entry's count is how many of the item", "[leveled]") {
    const auto spec = with_list({}, {{1, k_a, 4}});
    const auto got = run(spec, k_list, 1, 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0].count == 4);
}

TEST_CASE("chance none rolls once per evaluation", "[leveled]") {
    CHECK(run(with_list({}, {{1, k_a}}, 100), k_list, 1, 1).empty());
    CHECK(run(with_list({}, {{1, k_a}}, 0), k_list, 1, 1).size() == 1);

    // About half, with a fixed seed range: the generator decides, so the
    // count is exact and stable.
    const auto half = with_list({}, {{1, k_a}}, 50);
    int given = 0;
    for (int seed = 1; seed <= 200; ++seed) {
        given += run(half, k_list, 1, static_cast<std::uint64_t>(seed)).empty() ? 0 : 1;
    }
    CHECK(given > 70);
    CHECK(given < 130);
    // The roll is the first draw: below(100) < chance means nothing.
    for (int seed = 1; seed <= 20; ++seed) {
        lv::Rng probe(static_cast<std::uint64_t>(seed));
        const bool nothing = probe.below(100) < 50;
        CHECK(run(half, k_list, 1, static_cast<std::uint64_t>(seed)).empty() == nothing);
    }
}

TEST_CASE("a chance global replaces the list's chance none", "[leveled]") {
    auto spec = with_list({}, {{1, k_a}}, 0);
    spec.leveled_lists[0].chance_global = k_global;
    CHECK(run(spec, k_list, 1, 1, [](std::uint32_t) { return std::optional<double>{100.0}; }).empty());

    spec = with_list({}, {{1, k_a}}, 100);
    spec.leveled_lists[0].chance_global = k_global;
    CHECK(run(spec, k_list, 1, 1, [](std::uint32_t) { return std::optional<double>{0.0}; }).size() == 1);
    // A global that is unknown leaves the list's own chance.
    CHECK(run(spec, k_list, 1, 1, [](std::uint32_t) { return std::optional<double>{}; }).empty());
    CHECK(run(spec, k_list, 1, 1).empty());
}

TEST_CASE("a nested list is evaluated once with the counts multiplied", "[leveled]") {
    WorldSpec spec;
    spec.leveled_lists = {{.id = k_list, .type = k_lvli, .entries = {{1, k_inner, 3}}},
                          {.id = k_inner, .type = k_lvli, .flags = flags_of(false, false, true),
                           .entries = {{1, k_a, 1}, {1, k_b, 2}}}};
    const auto got = run(spec, k_list, 1, 1);
    REQUIRE(got.size() == 2);
    CHECK(got[0].form == k_a);
    CHECK(got[0].count == 3);
    CHECK(got[1].form == k_b);
    CHECK(got[1].count == 6);
}

TEST_CASE("with 'for each item in count' a nested list is evaluated count times", "[leveled]") {
    WorldSpec spec;
    spec.leveled_lists = {{.id = k_list, .type = k_lvli, .flags = flags_of(false, true, false),
                           .entries = {{1, k_inner, 3}}},
                          // One pick of two: three evaluations spread over both.
                          {.id = k_inner, .type = k_lvli, .entries = {{1, k_a}, {1, k_b}}}};
    std::set<std::uint32_t> kinds;
    for (int seed = 1; seed <= 64; ++seed) {
        const auto got = run(spec, k_list, 1, static_cast<std::uint64_t>(seed));
        std::int64_t total = 0;
        for (const auto& e : got) {
            total += e.count;
            kinds.insert(e.form);
        }
        CHECK(total == 3);
    }
    CHECK(kinds == std::set<std::uint32_t>{k_a, k_b});
}

TEST_CASE("nesting is capped", "[leveled]") {
    WorldSpec spec;
    spec.leveled_lists = {{.id = k_list, .type = k_lvli, .entries = {{1, k_list}}}};
    CHECK(run(spec, k_list, 1, 1).size() <= 1);
    // The deepest list is not followed; its own id is then the "item".
    const auto got = run(spec, k_list, 1, 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0].form == k_list);
}

TEST_CASE("pick_one follows lists to one form", "[leveled]") {
    WorldSpec spec;
    spec.leveled_lists = {{.id = k_list, .type = k_lvli, .entries = {{1, k_inner}}},
                          {.id = k_inner, .type = k_lvli, .entries = {{1, k_a}, {1, k_b}}}};
    const BuiltWorld world(spec);
    std::set<std::uint32_t> got;
    for (std::uint64_t seed = 1; seed <= 64; ++seed) {
        lv::Rng rng(seed);
        got.insert(lv::pick_one(*world, k_list, 1, rng));
    }
    CHECK(got == std::set<std::uint32_t>{k_a, k_b});
    lv::Rng rng(1);
    CHECK(lv::pick_one(*world, k_c, 1, rng) == k_c);
    CHECK(lv::pick_one(*world, 0, 1, rng) == 0);
}

TEST_CASE("the player's level is one until the player levels", "[leveled]") {
    CHECK(lv::player_level() == 1);
}
