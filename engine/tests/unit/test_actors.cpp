// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a placed actor is built from (data/actors.cpp), decided from world.fb
// alone, as docs/actors.md describes: template chains, leveled lists that
// pick the same entry every time for a reference, the skeleton, the outfit
// and the skin worn over it, and the models and idle that come with them.
// The world is built in memory (support/world_builder.hpp); `exists` stands
// for the pack.
#include "support/world_builder.hpp"

#include "data/actors.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace skydot::testing;
using Catch::Matchers::ContainsSubstring;

namespace {

using Paths = std::set<std::string>;
using List = std::vector<std::string>;

constexpr std::uint32_t k_human = 0x100;
constexpr std::uint32_t k_skin = 0x200;
/// Body slots as ARMO and ARMA have them: bit n is slot 30 + n.
constexpr std::uint32_t k_hair = 0x2;
constexpr std::uint32_t k_body = 0x4;
constexpr std::uint32_t k_hands = 0x8;
constexpr std::uint32_t k_feet = 0x80;

const std::string k_skeleton = "meshes/actors/character/character assets/skeleton";
const std::string k_skeleton_female = "meshes/actors/character/character assets female/skeleton_female";

/// What the test pack holds unless a test says otherwise: the human skeletons.
Paths pack_with(Paths more = {}) {
    more.insert(k_skeleton + ".hkx");
    more.insert(k_skeleton_female + ".hkx");
    return more;
}

/// A human race with the skin armor 0x200, and a male NPC 1 of it.
WorldSpec humans() {
    return {.npcs = {{.id = 1, .race = k_human}},
            .races = {{.id = k_human,
                       .skeletons = {k_skeleton + ".nif", k_skeleton_female + ".nif"},
                       .behaviours = {"meshes/actors/character/defaultmale.hkx",
                                      "meshes/actors/character/defaultfemale.hkx"},
                       .skin = k_skin,
                       .heights = {1.0F, 0.9F}}}};
}

skydot::ActorPlan plan(const WorldSpec& spec, std::uint32_t npc, const Paths& pack = pack_with(),
                       std::uint32_t ref = 0x800) {
    const BuiltWorld world(spec);
    return skydot::plan_actor(*world, npc, ref, [&](const std::string& path) { return pack.contains(path); });
}

NpcSpec& npc_of(WorldSpec& spec, std::uint32_t id) {
    return *std::ranges::find(spec.npcs, id, &NpcSpec::id);
}

} // namespace

// ---- race, sex and size -----------------------------------------------------

TEST_CASE("a plan names the race, the skeleton for the sex and the size", "[actors]") {
    auto spec = humans();
    spec.npcs.push_back({.id = 2, .flags = wfb::NpcFlags::female, .race = k_human, .height = 1.1F}); // female
    spec.npcs.push_back({.id = 3, .race = k_human, .height = 1.1F});

    const auto male = plan(spec, 1);
    CHECK(male.missing.empty());
    CHECK(male.npc == 1);
    CHECK(male.race == k_human);
    CHECK_FALSE(male.female);
    // The skeleton NIF's animation skeleton is the .hkx of the same name.
    CHECK(male.skeleton == k_skeleton + ".hkx");
    CHECK(male.behaviour == "meshes/actors/character/defaultmale.hkx");
    // NPC height times the race's for the sex; an NPC without one is 1.
    CHECK(male.scale == 1.0F);

    const auto female = plan(spec, 2);
    CHECK(female.female);
    CHECK(female.skeleton == k_skeleton_female + ".hkx");
    CHECK(female.behaviour == "meshes/actors/character/defaultfemale.hkx");
    CHECK(female.scale == 1.1F * 0.9F);
    CHECK(plan(spec, 3).scale == 1.1F);
}

TEST_CASE("a race with one skeleton gives it to both sexes", "[actors]") {
    auto spec = humans();
    spec.races[0].skeletons = {k_skeleton + ".nif"};
    spec.races[0].heights = {};
    spec.npcs.push_back({.id = 2, .flags = wfb::NpcFlags::female, .race = k_human, .height = 2.0F});
    const auto female = plan(spec, 2);
    CHECK(female.female);
    CHECK(female.skeleton == k_skeleton + ".hkx");
    // Without heights in the race, its factor is 1.
    CHECK(female.scale == 2.0F);
}

TEST_CASE("the skin tone is the traits NPC's QNAM", "[actors]") {
    auto spec = humans();
    npc_of(spec, 1).skin_tone = {0.5F, 0.25F, 1.0F};
    spec.npcs.push_back({.id = 2, .race = k_human, .skin_tone = {0.5F}}); // not three: ignored
    const auto tinted = plan(spec, 1);
    CHECK(tinted.skin_tone[0] == 0.5F);
    CHECK(tinted.skin_tone[1] == 0.25F);
    CHECK(tinted.skin_tone[2] == 1.0F);
    const auto plain = plan(spec, 2);
    CHECK(plain.skin_tone[0] == 1.0F);
    CHECK(plain.skin_tone[2] == 1.0F);
}

TEST_CASE("an actor that cannot be built says why", "[actors]") {
    auto spec = humans();
    spec.npcs.push_back({.id = 2, .race = 0x999});
    CHECK(plan(spec, 77).missing == "no NPC_");
    CHECK(plan(spec, 2).missing == "no RACE");

    // The pack lacks the skeleton's .hkx.
    const auto bare = plan(spec, 1, Paths{});
    CHECK_THAT(bare.missing, ContainsSubstring("no animation skeleton"));
    CHECK(bare.skeleton.empty());
    CHECK(bare.parts.empty());
}

// ---- templates --------------------------------------------------------------

namespace {

/// Three humans: 1 takes its traits from 2, which takes them from 3 (a female,
/// 1.5 tall).
WorldSpec chain() {
    auto spec = humans();
    spec.npcs = {{.id = 1, .race = k_human, .template_id = 2, .template_flags = wfb::NpcTemplateFlags::use_traits},
                 {.id = 2, .race = k_human, .template_id = 3, .template_flags = wfb::NpcTemplateFlags::use_traits},
                 {.id = 3, .flags = wfb::NpcFlags::female, .race = k_human, .height = 1.5F}};
    return spec;
}

} // namespace

TEST_CASE("an NPC takes its traits down the template chain while the flag is set", "[actors][templates]") {
    const auto spec = chain();
    const auto from_the_end = plan(spec, 1);
    CHECK(from_the_end.npc == 3);
    CHECK(from_the_end.female);
    CHECK(from_the_end.scale == 1.5F * 0.9F);

    // Where a link does not take traits, the chain stops there.
    auto stops = chain();
    npc_of(stops, 2).template_flags = wfb::NpcTemplateFlags::NONE;
    CHECK(plan(stops, 1).npc == 2);
    CHECK_FALSE(plan(stops, 1).female);

    // And an NPC without the flag is itself, whatever its template.
    auto own = chain();
    npc_of(own, 1).template_flags = wfb::NpcTemplateFlags::NONE;
    CHECK(plan(own, 1).npc == 1);
}

TEST_CASE("template chains that loop or lead nowhere end", "[actors][templates]") {
    auto spec = chain();
    npc_of(spec, 3).template_id = 1;
    npc_of(spec, 3).template_flags = wfb::NpcTemplateFlags::use_traits;
    CHECK(plan(spec, 1).missing.empty());

    auto itself = chain();
    npc_of(itself, 1).template_id = 1;
    CHECK(plan(itself, 1).npc == 1);

    auto missing = chain();
    npc_of(missing, 1).template_id = 0x7777;
    CHECK(plan(missing, 1).npc == 1);
    npc_of(missing, 1).template_id = 0;
    CHECK(plan(missing, 1).npc == 1);
}

TEST_CASE("traits and the outfit come from templates independently", "[actors][templates]") {
    auto spec = humans();
    spec.armors = {{.id = k_skin, .slots = 0, .addons = {}},
                   {.id = 0x210, .slots = k_body, .addons = {0x300}}};
    spec.addons = {{.id = 0x300, .slots = k_body, .race = k_human, .male_model = "armor/cuirass.nif"}};
    spec.outfits = {{.id = 0x400, .items = {0x210}}};
    // 1 is its own person, in the outfit of 2; 3 is in 2's person, in its own clothes.
    spec.npcs = {{.id = 1, .race = k_human, .template_id = 2, .template_flags = wfb::NpcTemplateFlags::use_inventory},
                 {.id = 2, .flags = wfb::NpcFlags::female, .race = k_human, .default_outfit = 0x400},
                 {.id = 3, .race = k_human, .template_id = 2, .template_flags = wfb::NpcTemplateFlags::use_traits}};
    const auto pack = pack_with({"armor/cuirass.nif"});

    const auto dressed = plan(spec, 1, pack);
    CHECK(dressed.npc == 1);
    CHECK_FALSE(dressed.female);
    CHECK(dressed.parts == List{"armor/cuirass.nif"});

    const auto naked = plan(spec, 3, pack);
    CHECK(naked.npc == 2);
    CHECK(naked.female);
    CHECK(naked.parts.empty()); // 3 has no outfit of its own, and takes none
}

TEST_CASE("a leveled list picks one entry, the same every time for a reference", "[actors][templates][leveled]") {
    // 1 takes its traits from the NPC list 0x500: 2, 3 or 4, different people.
    auto spec = humans();
    spec.npcs = {{.id = 1, .race = k_human, .template_id = 0x500, .template_flags = wfb::NpcTemplateFlags::use_traits},
                 {.id = 2, .race = k_human},
                 {.id = 3, .race = k_human},
                 {.id = 4, .race = k_human}};
    spec.leveled_lists = {{.id = 0x500, .type = k_lvln, .entries = {{1, 2}, {1, 3}, {1, 4}}}};
    const BuiltWorld world(spec);
    const auto exists = [](const std::string& path) { return pack_with().contains(path); };

    std::set<std::uint32_t> picked;
    for (std::uint32_t ref = 0xA00; ref < 0xA40; ++ref) {
        const auto first = skydot::plan_actor(*world, 1, ref, exists);
        REQUIRE(first.missing.empty());
        CHECK((first.npc == 2 || first.npc == 3 || first.npc == 4));
        // Again, as after a reload: the same one.
        CHECK(skydot::plan_actor(*world, 1, ref, exists).npc == first.npc);
        CHECK(skydot::resolve_npc(*world, 1, wfb::NpcTemplateFlags::use_traits, ref)->id() == first.npc);
        picked.insert(first.npc);
    }
    // Different references get different people, not one for them all.
    CHECK(picked.size() >= 2);

    // Without the flag nothing is taken from the list.
    CHECK(skydot::resolve_npc(*world, 1, wfb::NpcTemplateFlags::use_inventory, 0xA00)->id() == 1);
}

TEST_CASE("a placed base may be a leveled list, and lists may nest", "[actors][templates][leveled]") {
    auto spec = humans();
    spec.npcs = {{.id = 2, .race = k_human}, {.id = 3, .race = k_human}};
    // 0x501 holds NPCs; 0x500, which is what is placed, holds 0x501 alone.
    spec.leveled_lists = {{.id = 0x501, .type = k_lvln, .entries = {{1, 2}, {1, 3}}},
                          {.id = 0x500, .type = k_lvln, .entries = {{1, 0x501}}}};
    for (std::uint32_t ref = 0xA00; ref < 0xA10; ++ref) {
        const auto p = plan(spec, 0x500, pack_with(), ref);
        CHECK(p.missing.empty());
        CHECK((p.npc == 2 || p.npc == 3));
    }
    // A list with nothing in it places nobody.
    spec.leveled_lists.push_back({.id = 0x502, .type = k_lvln});
    CHECK(plan(spec, 0x502).missing == "no NPC_");
}

TEST_CASE("resolve_npc gives the NPC a flag leads to, or null for something that is none", "[actors][templates]") {
    const BuiltWorld world(chain());
    REQUIRE(skydot::resolve_npc(*world, 1, wfb::NpcTemplateFlags::use_traits, 0x800) != nullptr);
    CHECK(skydot::resolve_npc(*world, 1, wfb::NpcTemplateFlags::use_traits, 0x800)->id() == 3);
    CHECK(skydot::resolve_npc(*world, 3, wfb::NpcTemplateFlags::use_traits, 0x800)->id() == 3);
    CHECK(skydot::resolve_npc(*world, 1, wfb::NpcTemplateFlags::use_inventory, 0x800)->id() == 1);
    CHECK(skydot::resolve_npc(*world, 99, wfb::NpcTemplateFlags::use_traits, 0x800) == nullptr);
}

// ---- what is worn -----------------------------------------------------------

namespace {

constexpr std::uint32_t k_cuirass = 0x210;
constexpr std::uint32_t k_boots = 0x211;
constexpr std::uint32_t k_helmet = 0x212;

/// Humans with a skin of body, hands and feet, and the armor 0x210 cuirass,
/// 0x211 boots and 0x212 helmet, each with one addon model; NPC 1 has the
/// outfit 0x400 of `items`.
WorldSpec dressed(std::vector<std::uint32_t> items) {
    auto spec = humans();
    spec.armors = {{.id = k_skin, .slots = 0, .addons = {0x301, 0x302, 0x303}},
                   {.id = k_cuirass, .slots = k_body, .addons = {0x311}},
                   {.id = k_boots, .slots = k_feet, .addons = {0x312}},
                   {.id = k_helmet, .slots = k_hair | 0x1, .addons = {0x313}}};
    spec.addons = {{.id = 0x301, .slots = k_body, .race = k_human, .male_model = "skin/body.nif"},
                   {.id = 0x302, .slots = k_hands, .race = k_human, .male_model = "skin/hands.nif"},
                   {.id = 0x303, .slots = k_feet, .race = k_human, .male_model = "skin/feet.nif"},
                   {.id = 0x311, .slots = k_body, .race = k_human, .male_model = "armor/cuirass.nif"},
                   {.id = 0x312, .slots = k_feet, .race = k_human, .male_model = "armor/boots.nif"},
                   {.id = 0x313, .slots = k_hair, .race = k_human, .male_model = "armor/helmet.nif"}};
    spec.outfits = {{.id = 0x400, .items = std::move(items)}};
    npc_of(spec, 1).default_outfit = 0x400;
    return spec;
}

const Paths k_models{"skin/body.nif",   "skin/hands.nif",   "skin/feet.nif",
                     "armor/cuirass.nif", "armor/boots.nif", "armor/helmet.nif"};

} // namespace

TEST_CASE("an actor wears its outfit, and its skin shows where nothing covers it", "[actors][worn]") {
    const auto pack = pack_with(k_models);

    // Naked: the skin's body, hands and feet.
    CHECK(plan(dressed({}), 1, pack).parts == List{"skin/body.nif", "skin/hands.nif", "skin/feet.nif"});

    // A cuirass takes the body's place; hands and feet stay.
    const auto cuirass = plan(dressed({k_cuirass}), 1, pack);
    CHECK(cuirass.parts == List{"armor/cuirass.nif", "skin/hands.nif", "skin/feet.nif"});

    // Boots take the feet too, and what is worn comes first.
    const auto both = plan(dressed({k_cuirass, k_boots}), 1, pack);
    CHECK(both.parts == List{"armor/cuirass.nif", "armor/boots.nif", "skin/hands.nif"});
    CHECK_FALSE(both.hide_hair);
}

TEST_CASE("the first item on a slot keeps it", "[actors][worn]") {
    // Armor 0x213 covers the body and the hands.
    const auto with_other = [](std::vector<std::uint32_t> items) {
        auto spec = dressed(std::move(items));
        spec.armors.push_back({.id = 0x213, .slots = k_body | k_hands, .addons = {0x314}});
        spec.addons.push_back({.id = 0x314, .slots = k_body, .race = k_human, .male_model = "armor/other.nif"});
        return spec;
    };
    Paths pack = pack_with(k_models);
    pack.insert("armor/other.nif");

    CHECK(plan(with_other({k_cuirass, 0x213}), 1, pack).parts ==
          List{"armor/cuirass.nif", "skin/hands.nif", "skin/feet.nif"});
    // The other way round, the other is worn, and covers the hands as well.
    CHECK(plan(with_other({0x213, k_cuirass}), 1, pack).parts == List{"armor/other.nif", "skin/feet.nif"});
}

TEST_CASE("an item over the hair hides the FaceGen head's hair", "[actors][worn]") {
    const auto pack = pack_with(k_models);
    CHECK(plan(dressed({k_helmet}), 1, pack).hide_hair);
    CHECK_FALSE(plan(dressed({k_cuirass}), 1, pack).hide_hair);
    CHECK_FALSE(plan(dressed({}), 1, pack).hide_hair);
}

TEST_CASE("the NPC's own skin replaces the race's", "[actors][worn]") {
    auto spec = dressed({});
    spec.armors.push_back({.id = 0x220, .slots = 0, .addons = {0x320}});
    spec.addons.push_back({.id = 0x320, .slots = k_body, .race = k_human, .male_model = "skin/scaly.nif"});
    npc_of(spec, 1).skin = 0x220;
    CHECK(plan(spec, 1, pack_with({"skin/scaly.nif", "skin/body.nif"})).parts == List{"skin/scaly.nif"});
}

TEST_CASE("an addon fits the race it names, its additional races, or the race's armor race", "[actors][worn]") {
    constexpr std::uint32_t k_elf = 0x101;
    constexpr std::uint32_t k_beast = 0x102;
    const auto pack = pack_with(k_models);
    const auto worn_by = [&](std::uint32_t race_of_npc, std::uint32_t addon_race, std::vector<std::uint32_t> more,
                             std::uint32_t armor_race = 0) {
        auto spec = dressed({k_cuirass});
        spec.races.push_back({.id = k_elf, .skeletons = {k_skeleton + ".nif"}});
        spec.races.push_back({.id = k_beast, .skeletons = {k_skeleton + ".nif"}, .armor_race = armor_race});
        auto& addon = *std::ranges::find(spec.addons, 0x311U, &AddonSpec::id);
        addon.race = addon_race;
        addon.additional_races = std::move(more);
        npc_of(spec, 1).race = race_of_npc;
        const auto parts = plan(spec, 1, pack).parts;
        return std::ranges::count(parts, "armor/cuirass.nif") > 0;
    };

    CHECK(worn_by(k_human, k_human, {}));
    CHECK_FALSE(worn_by(k_elf, k_human, {}));
    CHECK(worn_by(k_elf, k_human, {k_elf}));
    CHECK_FALSE(worn_by(k_elf, k_human, {k_beast}));
    // The beast race wears what its armor race, the human, does.
    CHECK(worn_by(k_beast, k_human, {}, k_human));
    CHECK(worn_by(k_beast, 0x999, {k_human}, k_human));
    CHECK_FALSE(worn_by(k_beast, k_human, {}, 0));
}

TEST_CASE("an addon's model is the sex's, else the other's", "[actors][worn]") {
    auto spec = dressed({k_cuirass});
    auto& addon = *std::ranges::find(spec.addons, 0x311U, &AddonSpec::id);
    npc_of(spec, 1).flags = wfb::NpcFlags::female;
    const Paths pack = pack_with({"armor/cuirass.nif", "armor/cuirass_f.nif"});

    addon.male_model = "armor/cuirass.nif";
    addon.female_model = "armor/cuirass_f.nif";
    CHECK(plan(spec, 1, pack).parts == List{"armor/cuirass_f.nif"});
    addon.female_model = "";
    CHECK(plan(spec, 1, pack).parts == List{"armor/cuirass.nif"});

    npc_of(spec, 1).flags = wfb::NpcFlags::NONE;
    addon.male_model = "";
    addon.female_model = "armor/cuirass_f.nif";
    CHECK(plan(spec, 1, pack).parts == List{"armor/cuirass_f.nif"});
}

TEST_CASE("a weight slider picks the thin model below weight 50 and the heavy one from it", "[actors][worn]") {
    auto spec = dressed({k_cuirass});
    auto& addon = *std::ranges::find(spec.addons, 0x311U, &AddonSpec::id);
    addon.male_model = "armor/body_1.nif"; // the path names the heavy one
    addon.male_weight_slider = 2;
    const Paths both = pack_with({"armor/body_0.nif", "armor/body_1.nif"});
    const auto worn = [&](float weight, const Paths& pack) {
        npc_of(spec, 1).weight = weight;
        const auto parts = plan(spec, 1, pack).parts;
        return parts.empty() ? std::string() : parts.front();
    };

    CHECK(worn(0.0F, both) == "armor/body_0.nif");
    CHECK(worn(49.0F, both) == "armor/body_0.nif");
    CHECK(worn(50.0F, both) == "armor/body_1.nif");
    CHECK(worn(100.0F, both) == "armor/body_1.nif");
    // Only if the pack has the thin one.
    CHECK(worn(10.0F, pack_with({"armor/body_1.nif"})) == "armor/body_1.nif");
    // Without the slider the path is as it is.
    addon.male_weight_slider = 0;
    CHECK(worn(10.0F, both) == "armor/body_1.nif");
}

TEST_CASE("models the pack lacks are left out, and a model is listed once", "[actors][worn]") {
    const auto spec = dressed({k_cuirass, k_boots});
    CHECK(plan(spec, 1, pack_with({"armor/cuirass.nif", "skin/hands.nif"})).parts ==
          List{"armor/cuirass.nif", "skin/hands.nif"});

    // Two items whose addons are the one model.
    auto twice = dressed({k_cuirass, k_boots});
    twice.addons[4].male_model = "armor/cuirass.nif"; // boots' addon
    CHECK(plan(twice, 1, pack_with(k_models)).parts ==
          List{"armor/cuirass.nif", "skin/hands.nif"});
}

TEST_CASE("the FaceGen head is the traits NPC's, when the pack has it", "[actors][worn]") {
    auto spec = dressed({k_cuirass});
    spec.npcs.push_back({.id = 2, .race = k_human, .template_id = 3, .template_flags = wfb::NpcTemplateFlags::use_traits});
    spec.npcs.push_back({.id = 3, .race = k_human, .face_model = "facegeom/three.nif"});
    npc_of(spec, 1).face_model = "facegeom/one.nif";
    const auto pack = pack_with({"facegeom/one.nif", "facegeom/three.nif"});

    const auto one = plan(spec, 1, pack_with(k_models)).parts; // its head is not in this pack
    CHECK(std::ranges::count(one, "facegeom/one.nif") == 0);

    Paths with_heads = pack_with(k_models);
    with_heads.insert(pack.begin(), pack.end());
    const auto own = plan(spec, 1, with_heads).parts;
    REQUIRE_FALSE(own.empty());
    CHECK(own.back() == "facegeom/one.nif");
    // 2 has the face of 3, whose traits it takes.
    const auto borrowed = plan(spec, 2, with_heads).parts;
    REQUIRE_FALSE(borrowed.empty());
    CHECK(borrowed.back() == "facegeom/three.nif");
}

TEST_CASE("an outfit entry that is a leveled list is resolved by the actor's level, or worn whole",
          "[actors][worn][leveled]") {
    const auto pack = pack_with(k_models);
    const auto worn_at = [&](wfb::LeveledListFlags flags, std::uint16_t level,
                             std::uint32_t ref = 0x800) {
        auto spec = dressed({0x600});
        spec.leveled_lists = {{.id = 0x600, .flags = flags, .entries = {{1, k_cuirass}, {20, k_boots}}}};
        npc_of(spec, 1).level = level;
        return plan(spec, 1, pack, ref).parts;
    };
    const auto has = [](const List& parts, const char* model) { return std::ranges::count(parts, model) > 0; };

    // Only entries at or below the level are candidates: at 5, the cuirass.
    for (std::uint32_t ref = 0x800; ref < 0x810; ++ref) {
        const auto parts = worn_at(wfb::LeveledListFlags::NONE, 5, ref);
        CHECK(has(parts, "armor/cuirass.nif"));
        CHECK_FALSE(has(parts, "armor/boots.nif"));
    }
    // At 25 either, the same one for a reference every time.
    std::set<bool> cuirass_seen;
    for (std::uint32_t ref = 0x800; ref < 0x840; ++ref) {
        const auto parts = worn_at(wfb::LeveledListFlags::NONE, 25, ref);
        CHECK(has(parts, "armor/cuirass.nif") != has(parts, "armor/boots.nif"));
        CHECK(worn_at(wfb::LeveledListFlags::NONE, 25, ref) == parts);
        cuirass_seen.insert(has(parts, "armor/cuirass.nif"));
    }
    CHECK(cuirass_seen.size() == 2);
    // With "use all" the list is every entry.
    const auto all = worn_at(wfb::LeveledListFlags::use_all, 5);
    CHECK(has(all, "armor/cuirass.nif"));
    CHECK(has(all, "armor/boots.nif"));
}

// ---- the idle ---------------------------------------------------------------

TEST_CASE("the idle is the first clip the race's behaviour folder has", "[actors][idle]") {
    const std::string animations = "meshes/actors/character/animations/";
    const auto idle_of = [&](std::vector<std::string> clips, bool female = false) {
        auto spec = humans();
        npc_of(spec, 1).flags = female ? wfb::NpcFlags::female : wfb::NpcFlags::NONE;
        Paths pack = pack_with();
        pack.insert(clips.begin(), clips.end());
        return plan(spec, 1, pack).idle;
    };

    CHECK(idle_of({}).empty());
    CHECK(idle_of({animations + "idlestand.hkx"}) == animations + "idlestand.hkx");
    // The order: sex's mt_idle, mt_idle, idle, idle1, idlestand.
    CHECK(idle_of({animations + "idlestand.hkx", animations + "idle1.hkx"}) == animations + "idle1.hkx");
    CHECK(idle_of({animations + "idle1.hkx", animations + "idle.hkx"}) == animations + "idle.hkx");
    CHECK(idle_of({animations + "idle.hkx", animations + "mt_idle.hkx"}) == animations + "mt_idle.hkx");
    CHECK(idle_of({animations + "mt_idle.hkx", animations + "male/mt_idle.hkx"}) == animations + "male/mt_idle.hkx");
    // A female's folder is her own; the male's is not hers.
    CHECK(idle_of({animations + "male/mt_idle.hkx"}, true).empty());
    CHECK(idle_of({animations + "male/mt_idle.hkx", animations + "female/mt_idle.hkx"}, true) ==
          animations + "female/mt_idle.hkx");
}
