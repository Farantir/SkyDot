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
//   Find siblings. Branch conditions are evaluated then, not again.
//
// Source for the record layout: xEdit's wbDefinitionsTES5.pas (PACK); for
// what procedures do, how the vanilla templates use them (docs/ai.md).
#pragma once

#include "world_generated.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skydot::ai {

namespace wfb = bethconv::pack::wfb;

/// Game time.
struct Clock {
    /// Days passed since the game started (GameDaysPassed), with the time of
    /// day as the fraction.
    double days = 0.0;

    double hour() const;
    /// 0 Sundas (Sunday) … 6 Loredas (Saturday).
    int day_of_week() const;
    /// 0-based month and 1-based day of the month.
    int month() const;
    int date() const;
};

/// Whether `p`'s schedule (PSDT) covers `clock`. A start hour opens a window
/// of `duration` minutes (an hour when 0), which may run past midnight.
bool schedule_active(const wfb::Package& p, const Clock& clock);

/// What conditions ask about the world. Functions the host does not know
/// count as 0, which is what most "is it so?" functions are by default.
class ConditionHost {
public:
    virtual ~ConditionHost() = default;
    /// The value of `c`'s function for `subject` (a reference, or 0 when the
    /// condition's subject could not be found).
    virtual double function_value(const wfb::Condition& c, std::uint32_t subject) = 0;
    /// A GLOB's value, for conditions comparing with one.
    virtual double global_value(std::uint32_t global) = 0;
    /// `actor`'s linked reference with `keyword` (0: the one without), or 0.
    virtual std::uint32_t linked_ref(std::uint32_t actor, std::uint32_t keyword) = 0;
};

/// Conditions in order, ORed runs ANDed together (CTDA flag 0x01 ORs a
/// condition with the next). Empty or null passes. `actor` is the subject
/// unless a condition runs on a reference or the actor's linked reference.
bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list,
                     ConditionHost& host, std::uint32_t actor);

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
/// (Follow, Escort, UseWeapon, …) are `other`: the actor stays put.
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
    other,
};

Procedure procedure_of(std::string_view name);
const char* procedure_name(Procedure p);

/// One thing to do, from one procedure of the tree.
struct Step {
    Procedure procedure = Procedure::other;
    std::string name; ///< The procedure as the record names it.
    /// The procedure's first Location input, and its first SingleRef or
    /// TargetSelector input; null if it has none.
    const wfb::PackageInput* location = nullptr;
    const wfb::PackageInput* target = nullptr;
    /// Its first ObjectList input's key (what Find fills and Sleep, Sit and
    /// Eat use), or -1.
    std::int32_t list_key = -1;
    /// Seconds after which the step is done (a Wait, or a Wait running
    /// alongside); 0: done when the procedure is, or never.
    double seconds = 0.0;
};

/// Seeded random numbers for Random nodes and GetRandomPercent.
using Dice = std::function<double()>; ///< Uniform in [0, 1).

/// The steps `pkg` runs for `actor` now. Conditions on branches see `host`;
/// GetNumericPackageData reads `pkg`'s inputs.
std::vector<Step> plan_steps(const Package& pkg, ConditionHost& host, std::uint32_t actor,
                             const Dice& dice);
/// Whether the tree starts over when its steps are done (PRCB bit 0 on the
/// root).
bool repeats(const Package& pkg);

/// The package `actor` (whose NPC_ is `npc`) runs at `clock`: its NPC_'s
/// packages, then its default package list, leaving out those `skip` names
/// (packages it has completed). 0 if none applies.
std::uint32_t choose_package(const wfb::World& world, std::uint32_t npc, std::uint32_t actor,
                             const Clock& clock, ConditionHost& host,
                             const std::function<bool(std::uint32_t)>& skip = {});
/// The same over a package list already made.
std::uint32_t choose_package(const wfb::World& world, const std::vector<std::uint32_t>& list,
                             std::uint32_t actor, const Clock& clock, ConditionHost& host,
                             const std::function<bool(std::uint32_t)>& skip = {});
/// Minutes a package stays done once completed: its schedule window, or an
/// hour for one without a start time.
double done_minutes(const wfb::Package& p);
/// The packages `choose_package` tries, in order.
std::vector<std::uint32_t> package_list(const wfb::World& world, std::uint32_t npc,
                                        std::uint32_t actor);

/// GetNumericPackageData (612), which conditions inside a tree use to read
/// the package's own inputs.
inline constexpr std::uint16_t k_get_numeric_package_data = 612;

} // namespace skydot::ai
