// SPDX-License-Identifier: GPL-3.0-or-later
//
// `PapyrusConditionWorld`: the game as condition functions ask about it
// (conditions/conditions.hpp), answered from the world data and from
// SkydotPapyrus's view of the world (quests, actor values, inventory, script
// variables, enabled state) and the game clock. Callers that know better
// about some of it, as the AI does for where its actors are, derive from it.
#pragma once

#include "conditions/conditions.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skydot {

class SkydotPapyrus;
class SkydotWorld;
class WorldData;
namespace vm {
struct QuestState;
}

class PapyrusConditionWorld : public conditions::ConditionWorld {
public:
    /// `owner` may be null (an AI set up without scripts): quests, actor
    /// values, inventory and script variables then answer nothing. `world`
    /// is used then for the data; with an owner it is the owner's.
    explicit PapyrusConditionWorld(const SkydotPapyrus* owner, const SkydotWorld* world = nullptr)
        : owner_(owner), world_(world) {}

    std::int32_t quest_stage(std::uint32_t quest) const override;
    bool stage_done(std::uint32_t quest, std::int32_t stage) const override;
    bool quest_running(std::uint32_t quest) const override;
    bool quest_completed(std::uint32_t quest) const override;
    std::uint32_t alias_fill(std::uint32_t quest, std::uint32_t alias) const override;
    double global_value(std::uint32_t global) const override;
    std::optional<double> script_variable(std::uint32_t form, const std::string& script,
                                          const std::string& name) const override;

    std::uint32_t npc_of(std::uint32_t ref) const override;
    std::optional<std::int32_t> faction_rank(std::uint32_t ref, std::uint32_t faction) const override;
    std::vector<std::uint32_t> factions_of(std::uint32_t ref) const override;
    std::uint32_t race_of(std::uint32_t ref) const override;
    bool is_female(std::uint32_t ref) const override;
    std::uint32_t voice_type_of(std::uint32_t ref) const override;
    std::pair<std::uint32_t, std::uint32_t> race_voice_types(std::uint32_t race) const override;
    bool form_list(std::uint32_t list, std::vector<std::uint32_t>& forms) const override;
    bool has_keyword(std::uint32_t ref, std::uint32_t keyword) const override;
    double actor_value(std::uint32_t ref, std::uint32_t av) const override;
    double base_actor_value(std::uint32_t ref, std::uint32_t av) const override;
    double actor_value_percent(std::uint32_t ref, std::uint32_t av) const override;
    bool is_dead(std::uint32_t ref) const override;
    std::int32_t dead_count(std::uint32_t npc) const override;
    bool is_disabled(std::uint32_t ref) const override;
    std::int64_t item_count(std::uint32_t ref, std::uint32_t item) const override;
    std::int32_t level(std::uint32_t ref) const override;

    std::uint32_t cell_of(std::uint32_t ref) const override;
    bool cell_interior(std::uint32_t cell) const override;
    std::uint32_t cell_world(std::uint32_t cell) const override;
    std::uint32_t cell_location(std::uint32_t cell) const override;
    std::uint32_t location_parent(std::uint32_t location) const override;
    bool location_has_keyword(std::uint32_t location, std::uint32_t keyword) const override;
    std::optional<double> distance(std::uint32_t a, std::uint32_t b) const override;
    std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const override;

    std::optional<std::int32_t> relationship_rank(std::uint32_t npc_a, std::uint32_t npc_b) const override;
    std::optional<std::pair<std::int32_t, std::int32_t>> vendor_hours(std::uint32_t faction) const override;

    double hour() const override;
    int day_of_week() const override;
    double game_days() const override;
    double random_percent() override;

    bool talked_to_player(std::uint32_t ref) const override;
    bool in_dialogue_with_player(std::uint32_t ref) const override;

private:
    /// The NPC_ standing for `ref` for what `flag` covers (templates followed).
    const bethconv::pack::wfb::Npc* resolved(std::uint32_t ref,
                                             bethconv::pack::wfb::NpcTemplateFlags flag) const;
    const bethconv::pack::wfb::World* root() const;
    const WorldData* data() const;
    const vm::QuestState* quests_of(std::uint32_t quest) const;
    std::uint32_t base_form(std::uint32_t ref) const;

    const SkydotPapyrus* owner_;
    const SkydotWorld* world_;
    std::uint64_t dice_ = 0x9E3779B97F4A7C15ull;
    /// RELA ranks by (a << 32 | b), both orders; built on first use.
    mutable std::map<std::uint64_t, std::int32_t> relationships_;
    mutable bool relationships_built_ = false;
};

} // namespace skydot
