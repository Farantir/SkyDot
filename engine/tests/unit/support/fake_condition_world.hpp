// SPDX-License-Identifier: GPL-3.0-or-later
//
// A ConditionWorld a test fills in with plain maps. Whatever it was not given
// is 0 / false / nothing.
#pragma once

#include "conditions/conditions.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace skydot::testing {

class FakeConditionWorld : public conditions::ConditionWorld {
public:
    struct Quest {
        std::int32_t stage = 0;
        std::set<std::int32_t> done;
        bool running = false;
        bool completed = false;
    };
    std::map<std::uint32_t, Quest> quests;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> aliases;
    std::map<std::uint32_t, double> globals;
    std::map<std::string, double> variables; ///< by variable name
    std::map<std::uint32_t, std::uint32_t> npcs;
    std::map<std::uint32_t, std::map<std::uint32_t, std::int32_t>> factions;
    std::map<std::uint32_t, std::uint32_t> races;
    std::set<std::uint32_t> females;
    std::map<std::uint32_t, std::uint32_t> voices;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> race_voices;
    std::map<std::uint32_t, std::vector<std::uint32_t>> lists;
    std::set<std::pair<std::uint32_t, std::uint32_t>> keywords;
    std::map<std::pair<std::uint32_t, std::uint32_t>, double> values;
    std::set<std::uint32_t> dead;
    std::set<std::uint32_t> children;
    std::map<std::uint32_t, std::int32_t> dead_counts;
    std::set<std::uint32_t> disabled;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::int64_t> items;
    std::map<std::uint32_t, std::int32_t> levels;
    std::map<std::uint32_t, std::uint32_t> cells;
    std::set<std::uint32_t> interiors;
    std::map<std::uint32_t, std::uint32_t> cell_worlds;
    std::map<std::uint32_t, std::uint32_t> cell_locations;
    std::map<std::uint32_t, std::uint32_t> location_parents;
    std::set<std::pair<std::uint32_t, std::uint32_t>> location_keywords;
    std::map<std::pair<std::uint32_t, std::uint32_t>, double> distances;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> links; ///< (ref, keyword)
    std::map<std::uint32_t, std::uint32_t> combat;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::int32_t> relationships;
    std::map<std::uint32_t, std::pair<std::int32_t, std::int32_t>> vendors;
    double now_hour = 12.0;
    int now_day = 0;
    double days = 0.0;
    double percent = 0.0;
    std::set<std::uint32_t> talked;
    std::set<std::uint32_t> in_dialogue;
    std::map<std::uint32_t, std::uint32_t> shouts; ///< ref -> the shout in its voice slot
    /// The subject of each reference-asking call is not recorded; tests that
    /// need it use ConditionOverrides.

    std::int32_t quest_stage(std::uint32_t q) const override { return find(q).stage; }
    bool stage_done(std::uint32_t q, std::int32_t s) const override { return find(q).done.contains(s); }
    bool quest_running(std::uint32_t q) const override { return find(q).running; }
    bool quest_completed(std::uint32_t q) const override { return find(q).completed; }
    std::uint32_t alias_fill(std::uint32_t q, std::uint32_t a) const override {
        const auto it = aliases.find({q, a});
        return it != aliases.end() ? it->second : 0;
    }
    double global_value(std::uint32_t g) const override { return get(globals, g, 0.0); }
    std::optional<double> script_variable(std::uint32_t, const std::string&, const std::string& name) const override {
        const auto it = variables.find(name);
        return it != variables.end() ? std::optional<double>(it->second) : std::nullopt;
    }
    std::uint32_t npc_of(std::uint32_t r) const override { return get(npcs, r, 0U); }
    std::optional<std::int32_t> faction_rank(std::uint32_t r, std::uint32_t f) const override {
        const auto it = factions.find(r);
        if (it == factions.end() || !it->second.contains(f)) {
            return std::nullopt;
        }
        return it->second.at(f);
    }
    std::vector<std::uint32_t> factions_of(std::uint32_t r) const override {
        std::vector<std::uint32_t> out;
        if (const auto it = factions.find(r); it != factions.end()) {
            for (const auto& [f, rank] : it->second) {
                out.push_back(f);
            }
        }
        return out;
    }
    std::uint32_t race_of(std::uint32_t r) const override { return get(races, r, 0U); }
    bool is_female(std::uint32_t r) const override { return females.contains(r); }
    std::uint32_t voice_type_of(std::uint32_t r) const override { return get(voices, r, 0U); }
    std::pair<std::uint32_t, std::uint32_t> race_voice_types(std::uint32_t race) const override {
        const auto it = race_voices.find(race);
        return it != race_voices.end() ? it->second : std::pair<std::uint32_t, std::uint32_t>{0, 0};
    }
    bool form_list(std::uint32_t l, std::vector<std::uint32_t>& out) const override {
        const auto it = lists.find(l);
        if (it == lists.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
    bool has_keyword(std::uint32_t r, std::uint32_t k) const override { return keywords.contains({r, k}); }
    double actor_value(std::uint32_t r, std::uint32_t av) const override { return get(values, {r, av}, 0.0); }
    double base_actor_value(std::uint32_t r, std::uint32_t av) const override { return get(values, {r, av}, 0.0); }
    double actor_value_percent(std::uint32_t, std::uint32_t) const override { return 1.0; }
    bool is_child(std::uint32_t r) const override { return children.contains(r); }
    bool is_dead(std::uint32_t r) const override { return dead.contains(r); }
    std::int32_t dead_count(std::uint32_t npc) const override { return get(dead_counts, npc, 0); }
    bool is_disabled(std::uint32_t r) const override { return disabled.contains(r); }
    std::int64_t item_count(std::uint32_t r, std::uint32_t i) const override {
        return get(items, {r, i}, std::int64_t{0});
    }
    std::int32_t level(std::uint32_t r) const override { return get(levels, r, 0); }
    std::uint32_t combat_target(std::uint32_t r) const override { return get(combat, r, 0U); }

    std::uint32_t cell_of(std::uint32_t r) const override { return get(cells, r, 0U); }
    bool cell_interior(std::uint32_t c) const override { return interiors.contains(c); }
    std::uint32_t cell_world(std::uint32_t c) const override { return get(cell_worlds, c, 0U); }
    std::uint32_t cell_location(std::uint32_t c) const override { return get(cell_locations, c, 0U); }
    std::uint32_t location_parent(std::uint32_t l) const override { return get(location_parents, l, 0U); }
    bool location_has_keyword(std::uint32_t l, std::uint32_t k) const override {
        return location_keywords.contains({l, k});
    }
    std::optional<double> distance(std::uint32_t a, std::uint32_t b) const override {
        const auto it = distances.find({a, b});
        return it != distances.end() ? std::optional<double>(it->second) : std::nullopt;
    }
    std::uint32_t linked_ref(std::uint32_t r, std::uint32_t k) const override { return get(links, {r, k}, 0U); }

    std::optional<std::int32_t> relationship_rank(std::uint32_t a, std::uint32_t b) const override {
        for (const auto& key : {std::pair{a, b}, std::pair{b, a}}) {
            if (const auto it = relationships.find(key); it != relationships.end()) {
                return it->second;
            }
        }
        return std::nullopt;
    }
    std::optional<std::pair<std::int32_t, std::int32_t>> vendor_hours(std::uint32_t f) const override {
        const auto it = vendors.find(f);
        return it != vendors.end() ? std::optional(it->second) : std::nullopt;
    }

    double hour() const override { return now_hour; }
    int day_of_week() const override { return now_day; }
    double game_days() const override { return days; }
    double random_percent() override { return percent; }
    bool talked_to_player(std::uint32_t r) const override { return talked.contains(r); }
    bool in_dialogue_with_player(std::uint32_t r) const override { return in_dialogue.contains(r); }
    std::uint32_t equipped_shout(std::uint32_t r) const override {
        const auto it = shouts.find(r);
        return it != shouts.end() ? it->second : 0;
    }

private:
    template <typename Map, typename Value>
    static Value get(const Map& map, const typename Map::key_type& key, Value fallback) {
        const auto it = map.find(key);
        return it != map.end() ? it->second : fallback;
    }
    const Quest& find(std::uint32_t q) const {
        static const Quest none;
        const auto it = quests.find(q);
        return it != quests.end() ? it->second : none;
    }
};

} // namespace skydot::testing
