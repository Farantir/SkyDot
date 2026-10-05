// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small writer for world.fb (formats/schema/world.fbs): just the NPCs, races,
// armor, leveled lists and AI packages the engine's pure logic reads, built
// with the generated builder API from plain structs. Every vector the schema
// says is sorted by id is sorted here, so tests list records in any order.
//
// Fields keep the schema's meaning and defaults; the ones a test never needs
// are not here.
#pragma once

#include "world_generated.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skydot::testing {

namespace wfb = bethconv::pack::wfb;

// ---- AI packages ------------------------------------------------------------

/// CTDA's type byte: the comparison in the top three bits, flags below.
enum Compare : std::uint8_t { eq = 0 << 5, ne = 1 << 5, gt = 2 << 5, ge = 3 << 5, lt = 4 << 5, le = 5 << 5 };
inline constexpr auto k_or = static_cast<std::uint8_t>(wfb::ConditionFlags::or_next);
inline constexpr auto k_use_global = static_cast<std::uint8_t>(wfb::ConditionFlags::use_global);
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

/// "LVLN" as the record type a leveled list of NPCs carries.
inline constexpr std::uint32_t k_lvln = 0x4E4C564C;

struct LeveledListSpec {
    std::uint32_t id;
    std::uint32_t type = 0;
    wfb::LeveledListFlags flags = {};
    /// Entries as {level, form}. A struct, not a pair: pair's converting
    /// constructor narrows int literals inside the STL, which MSVC warns
    /// about there.
    struct Entry {
        std::uint16_t level;
        std::uint32_t form;
    };
    std::vector<Entry> entries = {};
};

// ---- the file ---------------------------------------------------------------

struct WorldSpec {
    std::vector<NpcSpec> npcs = {};
    std::vector<RaceSpec> races = {};
    std::vector<ArmorSpec> armors = {};
    std::vector<AddonSpec> addons = {};
    std::vector<OutfitSpec> outfits = {};
    std::vector<LeveledListSpec> leveled_lists = {};
    std::vector<PackageSpec> packages = {};
};

/// A built and verified world.fb, kept alive with the reader on it.
class BuiltWorld {
public:
    explicit BuiltWorld(const WorldSpec& spec);
    BuiltWorld(const BuiltWorld&) = delete;
    BuiltWorld& operator=(const BuiltWorld&) = delete;

    [[nodiscard]] const wfb::World& operator*() const { return *world_; }
    [[nodiscard]] const wfb::World* operator->() const { return world_; }

private:
    std::vector<std::uint8_t> bytes_;
    const wfb::World* world_ = nullptr;
};

} // namespace skydot::testing
