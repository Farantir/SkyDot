// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small writer for world.fb (formats/schema/world.fbs): just the NPCs, races,
// armor, leveled lists, AI packages and the cells' references the engine's
// pure logic reads, built
// with the generated builder API from plain structs. Every vector the schema
// says is sorted by id is sorted here, so tests list records in any order.
//
// Fields keep the schema's meaning and defaults; the ones a test never needs
// are not here.
#pragma once

#include "world_generated.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace skydot::testing {

namespace wfb = bethconv::pack::wfb;

// ---- AI packages ------------------------------------------------------------

/// CTDA's type byte: the comparison in the top three bits, flags below.
enum Compare : std::uint8_t { eq = 0 << 5, ne = 1 << 5, gt = 2 << 5, ge = 3 << 5, lt = 4 << 5, le = 5 << 5 };
inline constexpr auto k_or = static_cast<std::uint8_t>(wfb::ConditionFlags::or_next);
inline constexpr auto k_use_global = static_cast<std::uint8_t>(wfb::ConditionFlags::use_global);
inline constexpr auto k_use_aliases = static_cast<std::uint8_t>(wfb::ConditionFlags::use_aliases);
inline constexpr auto k_swap = static_cast<std::uint8_t>(wfb::ConditionFlags::swap_subject_target);
/// CTDA run-on values.
inline constexpr std::uint32_t k_on_subject = 0;
inline constexpr std::uint32_t k_on_target = 1;
inline constexpr std::uint32_t k_on_reference = 2;
inline constexpr std::uint32_t k_on_combat_target = 3;
inline constexpr std::uint32_t k_on_linked_ref = 4;

struct ConditionSpec {
    std::uint8_t type = 0;
    std::uint16_t function = 0;
    float value = 0.0F;
    std::uint32_t value_global = 0;
    std::uint32_t param1 = 0;
    std::uint32_t param2 = 0;
    std::uint32_t run_on = k_on_subject;
    std::uint32_t reference = 0;
    std::int32_t param3 = -1;
    std::string string2 = {};
};

/// `function` compared with `value` by `compare`, plus flags.
[[nodiscard]] inline ConditionSpec condition(std::uint16_t function, Compare compare, float value,
                                             std::uint8_t flags = 0, std::uint32_t param1 = 0) {
    return {.type = static_cast<std::uint8_t>(compare | flags),
            .function = function,
            .value = value,
            .value_global = 0,
            .param1 = param1};
}

struct InputSpec {
    std::int8_t key = 0;
    /// Bool, Int, Float, Location, SingleRef, TargetSelector, ObjectList...
    std::string type;
    float number = 0.0F;
};

[[nodiscard]] inline InputSpec input(std::int8_t key, std::string type, float number = 0.0F) {
    return {.key = key, .type = std::move(type), .number = number};
}

/// A node of a procedure tree: a leaf running `procedure`, or `type` (Sequence,
/// Stacked, Random, Simultaneous...) over `children`.
struct BranchSpec {
    std::string type;
    std::string procedure = {};
    std::vector<ConditionSpec> conditions = {};
    /// The data input keys a procedure reads.
    std::vector<std::uint8_t> inputs = {};
    /// PRCB flags: `repeat_when_complete` repeats the tree when it is done.
    wfb::BranchFlags flags = {};
    std::vector<BranchSpec> children = {};
};

[[nodiscard]] inline BranchSpec procedure(std::string name, std::vector<std::uint8_t> inputs = {},
                                          std::vector<ConditionSpec> conditions = {}) {
    return {.type = "Procedure",
            .procedure = std::move(name),
            .conditions = std::move(conditions),
            .inputs = std::move(inputs)};
}
[[nodiscard]] inline BranchSpec node(std::string type, std::vector<BranchSpec> children,
                                     std::vector<ConditionSpec> conditions = {}) {
    return {.type = std::move(type), .conditions = std::move(conditions), .children = std::move(children)};
}
[[nodiscard]] inline BranchSpec sequence(std::vector<BranchSpec> children, std::vector<ConditionSpec> conditions = {}) {
    return node("Sequence", std::move(children), std::move(conditions));
}
[[nodiscard]] inline BranchSpec stacked(std::vector<BranchSpec> children, std::vector<ConditionSpec> conditions = {}) {
    return node("Stacked", std::move(children), std::move(conditions));
}
[[nodiscard]] inline BranchSpec random_of(std::vector<BranchSpec> children) {
    return node("Random", std::move(children));
}
[[nodiscard]] inline BranchSpec simultaneous(std::vector<BranchSpec> children) {
    return node("Simultaneous", std::move(children));
}

/// A package (type 18) or a template (19), with a procedure tree in `tree`.
struct PackageSpec {
    std::uint32_t id;
    std::string editor_id = {};
    std::uint8_t type = 18;
    /// PSDT: -1 any month, day of week or hour (`date` 0 any). Day of week 0
    /// Sundas ... 6 Loredas, 7 weekdays, 8 weekends, 9 Morndas/Middas/Fredas,
    /// 10 Tirdas/Turdas. `duration` in minutes.
    std::int8_t month = -1;
    std::int8_t day_of_week = -1;
    std::int8_t date = 0;
    std::int8_t hour = -1;
    std::int8_t minute = -1;
    std::uint32_t duration = 0;
    std::vector<ConditionSpec> conditions = {};
    /// The template package whose tree and inputs this one runs over.
    std::uint32_t template_id = 0;
    std::vector<InputSpec> inputs = {};
    std::optional<BranchSpec> tree = {};
};

// ---- actors -----------------------------------------------------------------

/// An inventory entry (CNTO).
struct ItemEntry {
    std::uint32_t form;
    std::int32_t count = 1;
};

struct NpcSpec {
    std::uint32_t id;
    wfb::NpcFlags flags = {};
    std::uint16_t level = 0;
    std::uint32_t race = 0;
    std::uint32_t template_id = 0;
    wfb::NpcTemplateFlags template_flags = {};
    std::uint32_t skin = 0;
    std::uint32_t default_outfit = 0;
    float height = 0.0F;
    float weight = 0.0F;
    std::string face_model = {};
    std::vector<float> skin_tone = {};
    std::vector<std::uint32_t> packages = {};
    std::vector<std::uint32_t> default_packages = {};
    std::vector<ItemEntry> items = {};
};

struct RaceSpec {
    std::uint32_t id;
    /// The skeleton NIF, male then female.
    std::vector<std::string> skeletons = {};
    /// The behaviour graph, male then female.
    std::vector<std::string> behaviours = {};
    std::uint32_t skin = 0;
    /// Male then female.
    std::vector<float> heights = {};
    std::uint32_t armor_race = 0;
};

struct ArmorSpec {
    std::uint32_t id;
    /// Bit n is body slot 30 + n.
    std::uint32_t slots = 0;
    std::vector<std::uint32_t> addons = {};
};

struct AddonSpec {
    std::uint32_t id;
    std::uint32_t slots = 0;
    std::uint32_t race = 0;
    std::vector<std::uint32_t> additional_races = {};
    std::string male_model = {};
    std::string female_model = {};
    /// Bit 1 (2): the model has `_0` and `_1` weight variants.
    std::uint8_t male_weight_slider = 0;
    std::uint8_t female_weight_slider = 0;
};

struct OutfitSpec {
    std::uint32_t id;
    std::vector<std::uint32_t> items = {};
};

/// ItemData of a carriable base.
struct ItemSpec {
    wfb::ItemCategory category = wfb::ItemCategory::misc;
    float weight = 0.0F;
    std::int32_t value = 0;
    bool playable = true;
    std::uint32_t equip_slot = 0;
    std::uint8_t weapon_type = 0;
    float damage = 0.0F;
    float armor_rating = 0.0F;
    std::uint8_t armor_type = 0;
    std::uint32_t biped_slots = 0;
    bool food = false;
    /// EFID/EFIT of an ingredient or potion.
    struct Effect {
        std::uint32_t effect;
        float magnitude = 0.0F;
        std::uint32_t duration = 0;
    };
    std::vector<Effect> effects = {};
    std::uint32_t enchantment = 0;
};

/// PFIG, SNAM and PFPC of a plant.
struct HarvestSpec {
    std::uint32_t ingredient = 0;
    std::uint32_t sound = 0;
    std::uint32_t seasonal_yield = 0;
};

/// A base record: an item (with `item`) or a container (with `items`).
struct BaseSpec {
    std::uint32_t id;
    std::string name = {};
    std::optional<ItemSpec> item = {};
    std::vector<ItemEntry> items = {};
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> keywords = {};
    std::optional<HarvestSpec> harvest = {};
    std::uint8_t bench_type = 0;
};

/// COBJ.
struct RecipeSpec {
    std::uint32_t id;
    std::uint32_t product = 0;
    std::int32_t product_count = 1;
    std::uint32_t bench_keyword = 0;
    std::vector<ItemEntry> components = {};
    std::vector<ConditionSpec> conditions = {};
};

/// MGEF.
struct MagicEffectSpec {
    std::uint32_t id;
    std::string name = {};
    std::string description = {};
    std::uint32_t flags = 0;
    float base_cost = 0.0F;
    std::uint32_t archetype = 0;
    std::int32_t actor_value = -1;
    std::int32_t second_actor_value = -1;
    float second_av_weight = 0.0F;
    std::int32_t resist_value = -1;
    std::int32_t magic_skill = -1;
    std::uint32_t casting_type = 1;
    std::uint32_t delivery = 0;
    std::vector<std::uint32_t> keywords = {};
    std::vector<ConditionSpec> conditions = {};
};

/// One effect entry of a spell (or an ench.: EFID, EFIT and its CTDAs).
struct SpellEffectSpec {
    std::uint32_t effect;
    float magnitude = 0.0F;
    std::uint32_t area = 0;
    std::uint32_t duration = 0;
    std::vector<ConditionSpec> conditions = {};
};

/// SPEL.
struct SpellSpec {
    std::uint32_t id;
    std::string name = {};
    std::int32_t cost = 0;
    std::uint32_t flags = 0;
    std::uint32_t type = 0;
    std::uint32_t casting_type = 1;
    std::uint32_t delivery = 0;
    std::vector<SpellEffectSpec> effects = {};
};

/// "LVLN" as the record type a leveled list of NPCs carries.
inline constexpr std::uint32_t k_lvln = 0x4E4C564C;

struct LeveledListSpec {
    std::uint32_t id;
    std::uint32_t type = 0;
    wfb::LeveledListFlags flags = {};
    std::uint8_t chance_none = 0;
    /// LVLG: a global replacing `chance_none`.
    std::uint32_t chance_global = 0;
    /// Entries as {level, form, count}. A struct, not a pair: pair's converting
    /// constructor narrows int literals inside the STL, which MSVC warns
    /// about there.
    struct Entry {
        std::uint16_t level;
        std::uint32_t form;
        std::uint16_t count = 1;
    };
    std::vector<Entry> entries = {};
};

// ---- the file ---------------------------------------------------------------

// ---- cells ------------------------------------------------------------------

struct CellSpec {
    std::uint32_t id;
    /// The references placed in it, in any order.
    std::vector<std::uint32_t> refs = {};
    /// More references, flagged initially disabled.
    std::vector<std::uint32_t> disabled_refs = {};
};

/// A large reference (format 11) standing in grid square (x, y), whose bounds
/// also reach into the squares of `reaches`.
struct LargeRefSpec {
    std::uint32_t id;
    std::uint32_t base = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::vector<std::pair<std::int16_t, std::int16_t>> reaches = {};
    std::uint32_t enable_parent = 0;
    wfb::RefFlags flags = {};
};

/// A worldspace with large references; the builder derives the per-square
/// lists from the references' own squares and `reaches`.
struct WorldspaceSpec {
    std::uint32_t id;
    std::vector<LargeRefSpec> large_refs = {};
};

struct WorldSpec {
    /// The file's format version; 11 when worldspaces carry large references.
    int format_version = 10;
    std::vector<WorldspaceSpec> worlds = {};
    std::vector<CellSpec> cells = {};
    std::vector<BaseSpec> bases = {};
    std::vector<NpcSpec> npcs = {};
    std::vector<RaceSpec> races = {};
    std::vector<ArmorSpec> armors = {};
    std::vector<AddonSpec> addons = {};
    std::vector<OutfitSpec> outfits = {};
    std::vector<LeveledListSpec> leveled_lists = {};
    std::vector<PackageSpec> packages = {};
    std::vector<RecipeSpec> recipes = {};
    std::vector<MagicEffectSpec> magic_effects = {};
    std::vector<SpellSpec> spells = {};
};

/// A built and verified world.fb, kept alive with the reader on it.
class BuiltWorld {
public:
    explicit BuiltWorld(const WorldSpec& spec);
    BuiltWorld(const BuiltWorld&) = delete;
    BuiltWorld& operator=(const BuiltWorld&) = delete;

    [[nodiscard]] const wfb::World& operator*() const { return *world_; }
    [[nodiscard]] const wfb::World* operator->() const { return world_; }
    /// The file's bytes, to write out for something that opens a path.
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    const wfb::World* world_ = nullptr;
};

} // namespace skydot::testing
