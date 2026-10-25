// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one CTDA evaluator (ADR 0004): AI packages, quests and dialogue ask it
// whether a condition list passes. Nothing here touches Godot.
//
// - A list is ORed runs ANDed: CTDA flag 0x01 ORs a condition with the next;
//   a trailing OR closes its run at the end of the list. An empty or null
//   list passes.
// - A condition runs its function on a reference chosen by `run_on`: 0 the
//   subject, 1 the target, 2 the condition's own reference, 3 the subject's
//   combat target, 4 the subject's linked reference, 5 the fill of alias
//   `param3` of the context's quest, 6 package data and 7 event data (no
//   reference: 0). Flag 0x10 swaps subject and target first. A subject that
//   does not resolve is 0, and the function is still asked.
// - The comparison value is the condition's, or a GLOB's with flag 0x04.
// - Functions are bound by index in functions.cpp and read the game through
//   `ConditionWorld`. A function without a binding is unknown: the condition
//   is false (`UnknownPolicy::fails`) or its value is 0 (`zero`, the rule AI
//   packages keep), and the function index is counted, with one warning per
//   index through the counter's sink.
// - A caller can answer some functions first with `ConditionOverrides` (the
//   AI's GetIsCurrentPackage, its own dice for GetRandomPercent, package
//   data).
//
// Source for the functions: the Creation Kit wiki and UESP; the parameter
// kinds are in bethconv/record/conditions.hpp.
#pragma once

#include "world_generated.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skydot::ai {
struct Package;
}

namespace skydot::conditions {

namespace wfb = bethconv::pack::wfb;

/// What an unbound function means.
enum class UnknownPolicy : std::uint8_t {
    fails, ///< The condition is false (dialogue, quests).
    zero,  ///< The function's value is 0 (AI packages).
};

struct ConditionContext {
    std::uint32_t subject = 0;
    std::uint32_t target = 0;
    /// The quest whose aliases `run_on` 5 and GetIsAliasRef read.
    std::uint32_t quest = 0;
    /// The package whose data inputs `run_on` 6 reads (answered by overrides).
    const ai::Package* package = nullptr;
    UnknownPolicy unknown = UnknownPolicy::fails;
};

/// Counts the functions nothing is bound to, per index.
class UnknownCounter {
public:
    using Sink = std::function<void(std::uint16_t function)>;
    /// Called once for each function index, the first time it is met.
    void set_sink(Sink sink) { sink_ = std::move(sink); }
    void note(std::uint16_t function) {
        if (++counts_[function] == 1 && sink_) {
            sink_(function);
        }
    }
    [[nodiscard]] const std::map<std::uint16_t, std::uint64_t>& counts() const { return counts_; }
    void clear() { counts_.clear(); }

private:
    std::map<std::uint16_t, std::uint64_t> counts_;
    Sink sink_;
};

/// What condition functions ask of the game. FormIDs are global. A reference
/// that is no actor (or unknown) answers 0, false or nothing.
class ConditionWorld {
public:
    virtual ~ConditionWorld() = default;

    // ---- quests and variables
    [[nodiscard]] virtual std::int32_t quest_stage(std::uint32_t quest) const = 0;
    [[nodiscard]] virtual bool stage_done(std::uint32_t quest, std::int32_t stage) const = 0;
    [[nodiscard]] virtual bool quest_running(std::uint32_t quest) const = 0;
    [[nodiscard]] virtual bool quest_completed(std::uint32_t quest) const = 0;
    /// What fills alias `alias` of `quest`; 0 if empty.
    [[nodiscard]] virtual std::uint32_t alias_fill(std::uint32_t quest, std::uint32_t alias) const = 0;
    [[nodiscard]] virtual double global_value(std::uint32_t global) const = 0;
    /// A script variable of `form`; `script` empty: any script on it. Nothing
    /// if there is none.
    [[nodiscard]] virtual std::optional<double> script_variable(std::uint32_t form,
                                                                const std::string& script,
                                                                const std::string& name) const = 0;

    // ---- actors
    [[nodiscard]] virtual std::uint32_t player_ref() const { return 0x14; }
    /// The NPC_ an actor reference is (the player: 0x7); 0 if it is none.
    [[nodiscard]] virtual std::uint32_t npc_of(std::uint32_t ref) const = 0;
    /// The rank of `ref` in `faction`, templates resolved; nothing if not a member.
    [[nodiscard]] virtual std::optional<std::int32_t> faction_rank(std::uint32_t ref,
                                                                  std::uint32_t faction) const = 0;
    [[nodiscard]] virtual std::vector<std::uint32_t> factions_of(std::uint32_t ref) const = 0;
    [[nodiscard]] virtual std::uint32_t race_of(std::uint32_t ref) const = 0;
    [[nodiscard]] virtual bool is_female(std::uint32_t ref) const = 0;
    /// The NPC_'s own voice type (VTCK); 0 if it has none.
    [[nodiscard]] virtual std::uint32_t voice_type_of(std::uint32_t ref) const = 0;
    /// A race's default voice types: male, female.
    [[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> race_voice_types(
        std::uint32_t race) const = 0;
    [[nodiscard]] virtual bool form_list(std::uint32_t list, std::vector<std::uint32_t>& forms) const = 0;
    /// Whether `ref` (through its base) has `keyword`.
    /// HasKeyword on an actor sees its base's and its race's keywords too.
    [[nodiscard]] virtual bool has_keyword(std::uint32_t ref, std::uint32_t keyword) const = 0;
    /// The actor's race has the child flag (RACE DATA 0x4).
    [[nodiscard]] virtual bool is_child(std::uint32_t) const { return false; }
    [[nodiscard]] virtual double actor_value(std::uint32_t ref, std::uint32_t av) const = 0;
    [[nodiscard]] virtual double base_actor_value(std::uint32_t ref, std::uint32_t av) const = 0;
    [[nodiscard]] virtual double actor_value_percent(std::uint32_t ref, std::uint32_t av) const = 0;
    [[nodiscard]] virtual bool is_dead(std::uint32_t ref) const = 0;
    /// How many actors of NPC_ `npc` are dead.
    [[nodiscard]] virtual std::int32_t dead_count(std::uint32_t npc) const = 0;
    [[nodiscard]] virtual bool is_disabled(std::uint32_t ref) const = 0;
    [[nodiscard]] virtual std::int64_t item_count(std::uint32_t ref, std::uint32_t item) const = 0;
    [[nodiscard]] virtual std::int32_t level(std::uint32_t ref) const = 0;
    /// The actor `ref` is fighting; 0 until combat exists.
    [[nodiscard]] virtual std::uint32_t combat_target(std::uint32_t) const { return 0; }

    // ---- places
    [[nodiscard]] virtual std::uint32_t cell_of(std::uint32_t ref) const = 0;
    [[nodiscard]] virtual bool cell_interior(std::uint32_t cell) const = 0;
    /// The worldspace of an exterior cell; 0 for an interior.
    [[nodiscard]] virtual std::uint32_t cell_world(std::uint32_t cell) const = 0;
    /// The location a cell is in (XLCN); 0 if none.
    [[nodiscard]] virtual std::uint32_t cell_location(std::uint32_t cell) const = 0;
    [[nodiscard]] virtual std::uint32_t location_parent(std::uint32_t location) const = 0;
    [[nodiscard]] virtual bool location_has_keyword(std::uint32_t location, std::uint32_t keyword) const = 0;
    /// Game units between two references; nothing if either is unknown or
    /// they are in different spaces.
    [[nodiscard]] virtual std::optional<double> distance(std::uint32_t a, std::uint32_t b) const = 0;
    /// `ref`'s linked reference with `keyword` (0: the one without); 0 if none.
    [[nodiscard]] virtual std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const = 0;

    // ---- relationships and vendors
    /// RELA rank between two NPC_s as stored (0 lover ... 8 archnemesis), in
    /// either order; nothing if there is no relationship.
    [[nodiscard]] virtual std::optional<std::int32_t> relationship_rank(std::uint32_t npc_a,
                                                                       std::uint32_t npc_b) const = 0;
    /// A vendor faction's trading hours (start, end); nothing for a faction
    /// that is not a vendor.
    [[nodiscard]] virtual std::optional<std::pair<std::int32_t, std::int32_t>> vendor_hours(
        std::uint32_t faction) const = 0;

    // ---- clock and chance
    [[nodiscard]] virtual double hour() const = 0;
    [[nodiscard]] virtual int day_of_week() const = 0;
    [[nodiscard]] virtual double game_days() const = 0;
    /// A whole number 0-99.
    [[nodiscard]] virtual double random_percent() = 0;

    // ---- dialogue (answered from D1 on)
    [[nodiscard]] virtual bool talked_to_player(std::uint32_t) const { return false; }
    [[nodiscard]] virtual bool in_dialogue_with_player(std::uint32_t) const { return false; }
    /// The shout in the voice slot of `ref` (a SHOU), 0 if none.
    [[nodiscard]] virtual std::uint32_t equipped_shout(std::uint32_t) const { return 0; }

    [[nodiscard]] UnknownCounter& unknowns() { return unknowns_; }
    [[nodiscard]] const UnknownCounter& unknowns() const { return unknowns_; }

private:
    UnknownCounter unknowns_;
};

/// Answers some functions before the shared bindings do.
class ConditionOverrides {
public:
    virtual ~ConditionOverrides() = default;
    /// The value of `c` for `subject`, or nothing to leave it to the bindings.
    virtual std::optional<double> function_value(const wfb::Condition& c, std::uint32_t subject,
                                                 const ConditionContext& ctx) = 0;
};

/// The reference condition `c` runs on.
[[nodiscard]] std::uint32_t resolve_subject(const wfb::Condition& c, const ConditionContext& ctx,
                                            const ConditionWorld& world);

/// The binding table, by function index: `c`'s function for `subject`, or
/// nothing if the function is unknown.
[[nodiscard]] std::optional<double> function_value(const wfb::Condition& c, std::uint32_t subject,
                                                   const ConditionContext& ctx, ConditionWorld& world);

/// Whether one condition holds.
[[nodiscard]] bool condition_passes(const wfb::Condition& c, ConditionWorld& world,
                                    const ConditionContext& ctx,
                                    ConditionOverrides* overrides = nullptr);

[[nodiscard]] bool conditions_pass(
    const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, ConditionWorld& world,
    const ConditionContext& ctx, ConditionOverrides* overrides = nullptr);

/// The comparison of CTDA type byte `type` (top three bits: == != > >= < <=).
[[nodiscard]] bool compare(double value, std::uint8_t type, double with);

} // namespace skydot::conditions
