// SPDX-License-Identifier: GPL-3.0-or-later
//
// AI packages (PACK) read from world.fb: which one an actor runs now, and
// what doing it means, as a list of steps. Nothing here moves an actor;
// `SkydotAi` (ai.hpp) does, using this.
//
// - An actor's packages are its NPC_'s PKID list, then its default package
//   list (DPLT), both from the NPC_ its "AI packages" and "default package
//   list" template flags lead to. The first whose schedule covers the clock
//   and whose conditions pass runs.
// - A package runs its template's procedure tree with its own data inputs
//   over the template's, matched by key.
// - The tree is flattened when the package starts: a Sequence runs its
//   children in order, a Stacked node the first child whose conditions
//   pass, a Random node one at random, a Simultaneous node its first moving
//   (or staying) procedure for as long as its Wait siblings say, after its
//   Find siblings. Branch conditions are evaluated again while it runs
//   (SkydotAi plans again with the same dice and keeps the step it is in
//   when that is unchanged).
//
// Source for the record layout: xEdit's wbDefinitionsTES5.pas (PACK); for
// what procedures do, how the vanilla templates use them (docs/ai.md).
#pragma once

#include "conditions/conditions.hpp"
#include "state/calendar.hpp"
#include "world_generated.h"

#include <concepts>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace skydot::ai {

namespace wfb = bethconv::pack::wfb;

/// Game time; the arithmetic is state/calendar.hpp's.
using Clock = vm::Calendar;

/// Whether `p`'s schedule (PSDT) covers `clock`. A start hour opens a window
/// of `duration` minutes (an hour when 0), which may run past midnight.
bool schedule_active(const wfb::Package& p, const Clock& clock);

/// What a package's conditions are evaluated against: the shared world
/// (conditions/conditions.hpp) and the answers only the AI knows (the
/// current package, the mind's dice). Functions neither knows count as 0.
struct ConditionSource {
    conditions::ConditionWorld* world = nullptr;
    conditions::ConditionOverrides* overrides = nullptr;

    ConditionSource() = default;
    ConditionSource(conditions::ConditionWorld& w, conditions::ConditionOverrides* o = nullptr)
        : world(&w), overrides(o) {}
    /// One object that is both, as the tests' fake is.
    template <typename Both>
        requires std::derived_from<Both, conditions::ConditionWorld> &&
                 std::derived_from<Both, conditions::ConditionOverrides>
    ConditionSource(Both& both) : world(&both), overrides(&both) {}
};

/// Conditions in order, ORed runs ANDed together (CTDA flag 0x01 ORs a
/// condition with the next). Empty or null passes. `actor` is the subject and
/// the target (a package has no target of its own); unknown functions count
/// as 0.
bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list,
                     const ConditionSource& source, std::uint32_t actor);

/// A package with its template: inputs by key, its own first.
struct Package {
    const wfb::Package* own = nullptr;
    /// The package whose procedure tree runs: the first in the template chain
    /// with branches (the package itself if it has some).
    const wfb::Package* tree = nullptr;
    /// The chain from `own` to `tree`, for inputs.
    std::vector<const wfb::Package*> chain;

    explicit operator bool() const { return own != nullptr; }
    const wfb::PackageInput* input(std::int32_t key) const;
    /// A Bool, Int or Float input's value, or `fallback`.
    double number(std::int32_t key, double fallback = 0.0) const;
    std::string editor_id() const;
};

const wfb::Package* find_package(const wfb::World& world, std::uint32_t id);
/// `id` with its template chain; empty if `id` is no package.
Package resolve_package(const wfb::World& world, std::uint32_t id);

/// What a step of a package does. Procedures without their own handling
/// (UseWeapon, KeepAnEyeOn, …) are `other`: the actor stays put.
enum class Procedure : std::uint8_t {
    travel,
    sandbox,
    wander,
    sleep,
    sit,
    eat,
    use_idle_marker,
    activate,
    hold_position,
    guard,
    patrol,
    find,
    wait,
    unlock_doors,
    lock_doors,
    force_greet,
    follow,
    escort,
    other,
};

Procedure procedure_of(std::string_view name);
const char* procedure_name(Procedure p);

/// One thing to do, from one procedure of the tree.
struct Step {
    Procedure procedure = Procedure::other;
    std::string name; ///< The procedure as the record names it.
    /// The input that says where (a Location, or a SingleRef as Travel's
    /// "Player"), and the reference it is about (SingleRef or
    /// TargetSelector), each from the parameter the procedure keeps it in
    /// (Escort's place is its third, Shout's target its third); null if it
    /// has none. Procedures not known take the first of each type.
    const wfb::PackageInput* location = nullptr;
    const wfb::PackageInput* target = nullptr;
    /// Its first ObjectList input's key (what Find fills and Sleep, Sit and
    /// Eat use), or -1.
    std::int32_t list_key = -1;
    /// Seconds after which the step is done (a Wait, or a Wait running
    /// alongside); 0: done when the procedure is, or never.
    double seconds = 0.0;
    /// The inputs the procedure names, in its own parameter order (null for
    /// an unset key or one the package lacks): Follow, Escort and ForceGreet
    /// read their distances and topic by position.
    std::vector<const wfb::PackageInput*> params;

    /// The number of `params[i]` (a Bool, Int or Float), or `fallback`.
    [[nodiscard]] double number(std::size_t i, double fallback = 0.0) const;
    /// Whether two plans run the same procedures with the same inputs.
    [[nodiscard]] bool same_as(const Step& other) const;
};

/// Seeded random numbers for Random nodes and GetRandomPercent.
using Dice = std::function<double()>; ///< Uniform in [0, 1).

/// The steps `pkg` runs for `actor` now. Conditions on branches see `source`;
/// GetNumericPackageData reads `pkg`'s inputs.
std::vector<Step> plan_steps(const Package& pkg, const ConditionSource& source, std::uint32_t actor,
                             const Dice& dice);
/// Whether the tree starts over when its steps are done (PRCB bit 0 on the
/// root).
bool repeats(const Package& pkg);
/// Whether a branch condition of the tree reads where actors are
/// (GetWithinPackageLocation, GetDetected): its plan is made again while it
/// runs, as the ForceGreet template waits for the player to come close.
bool replans(const Package& pkg);

/// The package `actor` (whose NPC_ is `npc`) runs at `clock`: its NPC_'s
/// packages, then its default package list, leaving out those `skip` names
/// (packages it has completed). 0 if none applies.
std::uint32_t choose_package(const wfb::World& world, std::uint32_t npc, std::uint32_t actor,
                             const Clock& clock, const ConditionSource& source,
                             const std::function<bool(std::uint32_t)>& skip = {});
/// The same over a package list already made.
std::uint32_t choose_package(const wfb::World& world, const std::vector<std::uint32_t>& list,
                             std::uint32_t actor, const Clock& clock, const ConditionSource& source,
                             const std::function<bool(std::uint32_t)>& skip = {});
/// Minutes a package stays done once completed: its schedule window, or an
/// hour for one without a start time.
double done_minutes(const wfb::Package& p);
/// The packages `choose_package` tries, in order.
std::vector<std::uint32_t> package_list(const wfb::World& world, std::uint32_t npc,
                                        std::uint32_t actor);

/// The packages quest aliases give an actor filling `filled` (quest, alias id):
/// they come before its own. The highest quest priority (DNAM) first, a tie by
/// form id; within a quest its aliases' order, within an alias ALPC's.
std::vector<std::uint32_t> alias_packages(const wfb::World& world,
                                          std::vector<std::pair<std::uint32_t, std::uint32_t>> filled);

/// GetNumericPackageData (612), which conditions inside a tree use to read
/// the package's own inputs.
inline constexpr std::uint16_t k_get_numeric_package_data = 612;
inline constexpr std::uint16_t k_get_detected = 45;
inline constexpr std::uint16_t k_get_within_package_location = 325;
/// CTDA run on 6: the condition's subject is the reference of package data
/// input `param3`.
inline constexpr std::uint32_t k_run_on_package_data = 6;

/// The reference a SingleRef, TargetSelector or near-reference Location
/// input names when it names one directly (a specific reference, or `actor`
/// itself); 0 otherwise.
std::uint32_t input_ref(const wfb::PackageInput& in, std::uint32_t actor);

} // namespace skydot::ai
