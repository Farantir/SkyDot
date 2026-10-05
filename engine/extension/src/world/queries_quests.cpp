// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quests, globals and placed actors as world.fb carries them. The quest
// system itself is in SkydotPapyrus (vm/quests.cpp).
#include "world/queries.hpp"
#include "world/fb_search.hpp"
#include "world/refs.hpp"
#include "world/text.hpp"

#include "skydot_formats/flags.hpp"

#include <algorithm>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot::queries {

namespace {

String fourcc_text(std::uint32_t v) {
    if (v == 0) {
        return {};
    }
    const char chars[5] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                           static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF),
                           0};
    return String(chars);
}

Vector3 vec(const wfb::Vec3f& v) { return {v.x(), v.y(), v.z()}; }

} // namespace

std::int64_t get_quest_count(const WorldData& data) {
    return data.root() != nullptr && data.root()->quests() != nullptr ? data.root()->quests()->size() : 0;
}

bool has_quest(const WorldData& data, std::int64_t id) {
    return lookup(data.root() != nullptr ? data.root()->quests() : nullptr, static_cast<std::uint32_t>(id)) != nullptr;
}

Array list_quests(const WorldData& data, const String& filter) {
    Array out;
    const auto* quests = data.root() != nullptr ? data.root()->quests() : nullptr;
    if (quests == nullptr) {
        return out;
    }
    const String needle = filter.to_lower();
    for (const auto* q : *quests) {
        const String editor_id = to_godot(q->editor_id());
        if (!needle.is_empty() && !editor_id.to_lower().contains(needle)) {
            continue;
        }
        Dictionary entry;
        entry["id"] = static_cast<std::int64_t>(q->id());
        entry["editor_id"] = editor_id;
        entry["name"] = to_godot(q->name());
        entry["start_game_enabled"] =
            formats::has_flag(q->flags(), wfb::QuestFlags::start_game_enabled);
        out.push_back(entry);
    }
    return out;
}

std::int64_t find_quest(const WorldData& data, const String& editor_id) {
    const auto* quests = data.root() != nullptr ? data.root()->quests() : nullptr;
    if (quests == nullptr) {
        return 0;
    }
    const String wanted = editor_id.to_lower();
    for (const auto* q : *quests) {
        if (to_godot(q->editor_id()).to_lower() == wanted) {
            return q->id();
        }
    }
    return 0;
}

Dictionary get_quest(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* q = lookup(data.root() != nullptr ? data.root()->quests() : nullptr, static_cast<std::uint32_t>(id));
    if (q == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(q->id());
    out["editor_id"] = to_godot(q->editor_id());
    out["name"] = to_godot(q->name());
    out["flags"] = static_cast<std::int64_t>(q->flags());
    out["priority"] = static_cast<std::int64_t>(q->priority());
    out["type"] = static_cast<std::int64_t>(q->type());
    out["event"] = fourcc_text(q->event());
    out["start_game_enabled"] = formats::has_flag(q->flags(), wfb::QuestFlags::start_game_enabled);
    out["run_once"] = formats::has_flag(q->flags(), wfb::QuestFlags::run_once);
    out["allow_repeated_stages"] =
        formats::has_flag(q->flags(), wfb::QuestFlags::allow_repeated_stages);
    out["scripts"] = script_list(q->scripts(), false);
    out["fragment_script"] = to_godot(q->fragment_script());

    Array fragments;
    if (const auto* list = q->fragments()) {
        for (const auto* f : *list) {
            Dictionary entry;
            entry["stage"] = static_cast<std::int64_t>(f->stage());
            entry["log_entry"] = static_cast<std::int64_t>(f->log_entry());
            entry["function"] = to_godot(f->function());
            fragments.push_back(entry);
        }
    }
    out["fragments"] = fragments;

    Array stages;
    if (const auto* list = q->stages()) {
        for (const auto* st : *list) {
            Dictionary stage;
            stage["index"] = static_cast<std::int64_t>(st->index());
            stage["start_up"] = formats::has_flag(st->flags(), wfb::StageFlags::start_up);
            stage["shut_down"] = formats::has_flag(st->flags(), wfb::StageFlags::shut_down);
            Array log;
            if (const auto* entries = st->log()) {
                for (const auto* e : *entries) {
                    Dictionary entry;
                    entry["flags"] = static_cast<std::int64_t>(e->flags());
                    entry["completes_quest"] =
                        formats::has_flag(e->flags(), wfb::LogEntryFlags::completes_quest);
                    entry["fails_quest"] =
                        formats::has_flag(e->flags(), wfb::LogEntryFlags::fails_quest);
                    entry["text"] = to_godot(e->text());
                    entry["conditions"] = static_cast<std::int64_t>(e->conditions());
                    log.push_back(entry);
                }
            }
            stage["log"] = log;
            stages.push_back(stage);
        }
    }
    out["stages"] = stages;

    Array objectives;
    if (const auto* list = q->objectives()) {
        for (const auto* o : *list) {
            Dictionary objective;
            objective["index"] = static_cast<std::int64_t>(o->index());
            objective["flags"] = static_cast<std::int64_t>(o->flags());
            objective["text"] = to_godot(o->text());
            Array targets;
            if (const auto* t = o->targets()) {
                for (const auto alias : *t) {
                    targets.push_back(static_cast<std::int64_t>(alias));
                }
            }
            objective["targets"] = targets;
            objectives.push_back(objective);
        }
    }
    out["objectives"] = objectives;

    Array aliases;
    if (const auto* list = q->aliases()) {
        for (const auto* a : *list) {
            Dictionary alias;
            alias["id"] = static_cast<std::int64_t>(a->id());
            alias["name"] = to_godot(a->name());
            alias["location"] = a->location();
            alias["flags"] = static_cast<std::int64_t>(a->flags());
            alias["optional"] = formats::has_flag(a->flags(), wfb::AliasFlags::optional);
            alias["forced"] = static_cast<std::int64_t>(a->forced());
            alias["unique_actor"] = static_cast<std::int64_t>(a->unique_actor());
            alias["external_quest"] = static_cast<std::int64_t>(a->external_quest());
            alias["external_alias"] = static_cast<std::int64_t>(a->external_alias());
            alias["created_object"] = static_cast<std::int64_t>(a->created_object());
            alias["create_at"] = static_cast<std::int64_t>(a->create_at() & 0xFFFFu);
            alias["conditions"] = static_cast<std::int64_t>(a->conditions());
            alias["display_name"] = static_cast<std::int64_t>(a->display_name());
            alias["scripts"] = script_list(a->scripts(), false);
            aliases.push_back(alias);
        }
    }
    out["aliases"] = aliases;
    return out;
}

Dictionary get_global(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* g = lookup(data.root() != nullptr ? data.root()->globals() : nullptr, static_cast<std::uint32_t>(id));
    if (g == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(g->id());
    out["editor_id"] = to_godot(g->editor_id());
    const char kind[2] = {static_cast<char>(g->kind()), 0};
    out["kind"] = String(kind);
    out["value"] = static_cast<double>(g->value());
    return out;
}

Dictionary get_actor(const WorldData& data, std::int64_t ref) {
    Dictionary out;
    const auto* actors = data.root() != nullptr ? data.root()->actors() : nullptr;
    if (actors == nullptr) {
        return out;
    }
    const auto id = static_cast<std::uint32_t>(ref);
    const auto* it = lookup(actors, id);
    if (it == nullptr) {
        return out;
    }
    out["ref"] = static_cast<std::int64_t>(it->ref());
    out["base"] = static_cast<std::int64_t>(it->base());
    out["cell"] = static_cast<std::int64_t>(it->cell());
    out["position"] = vec(it->position());
    out["rotation"] = vec(it->rotation());
    out["disabled"] = formats::has_flag(it->flags(), wfb::RefFlags::initially_disabled);
    out["persistent"] = formats::has_flag(it->flags(), wfb::RefFlags::persistent);
    return out;
}

std::int64_t get_form_from_file(const WorldData& data, std::int64_t id, const String& plugin) {
    const auto* plugins = data.root() != nullptr ? data.root()->plugins() : nullptr;
    if (plugins == nullptr) {
        return 0;
    }
    const String wanted = plugin.to_lower();
    for (const auto* p : *plugins) {
        if (to_godot(p->name()).to_lower() == wanted) {
            const std::uint32_t mask = p->light() ? 0xFFFu : 0xFFFFFFu;
            return p->prefix() | (static_cast<std::uint32_t>(id) & mask);
        }
    }
    return 0;
}

std::int64_t find_actor_of(const WorldData& data, std::int64_t npc) {
    return data.actor_of(static_cast<std::uint32_t>(npc));
}

std::int64_t find_npc(const WorldData& data, const String& editor_id) {
    const auto* npcs = data.root() != nullptr ? data.root()->npcs() : nullptr;
    if (npcs == nullptr) {
        return 0;
    }
    const String wanted = editor_id.to_lower();
    for (const auto* n : *npcs) {
        if (n->editor_id() != nullptr && String::utf8(n->editor_id()->c_str()).to_lower() == wanted) {
            return n->id();
        }
    }
    return 0;
}

} // namespace skydot::queries
