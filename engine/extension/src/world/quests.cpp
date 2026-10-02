// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotWorld: quests, globals and placed actors as world.fb carries them.
// The quest system itself is in SkydotPapyrus (vm/quests.cpp).
#include "world/refs.hpp"
#include "world/fb_search.hpp"
#include "world/world.hpp"

#include <algorithm>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

// DNAM and stage flags (world.fbs).
constexpr std::uint32_t k_start_game_enabled = 0x0001;
constexpr std::uint32_t k_allow_repeated_stages = 0x0008;
constexpr std::uint32_t k_run_once = 0x0100;
constexpr std::uint32_t k_stage_start_up = 0x02;
constexpr std::uint32_t k_stage_shut_down = 0x04;
constexpr std::uint32_t k_alias_optional = 0x0002;

String text(const flatbuffers::String* s) {
    return s == nullptr ? String() : String::utf8(s->c_str(), static_cast<int>(s->size()));
}

template <typename T>
const T* by_id(const flatbuffers::Vector<flatbuffers::Offset<T>>* list, std::uint32_t id) {
    if (list == nullptr) {
        return nullptr;
    }
    return find_sorted(list, id, [](const T* e) { return e->id(); });
}

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

std::int64_t SkydotWorld::get_quest_count() const {
    return root_ != nullptr && root_->quests() != nullptr ? root_->quests()->size() : 0;
}

bool SkydotWorld::has_quest(std::int64_t id) const {
    return by_id(root_ != nullptr ? root_->quests() : nullptr, static_cast<std::uint32_t>(id)) != nullptr;
}

Array SkydotWorld::list_quests(const String& filter) const {
    Array out;
    const auto* quests = root_ != nullptr ? root_->quests() : nullptr;
    if (quests == nullptr) {
        return out;
    }
    const String needle = filter.to_lower();
    for (const auto* q : *quests) {
        const String editor_id = text(q->editor_id());
        if (!needle.is_empty() && !editor_id.to_lower().contains(needle)) {
            continue;
        }
        Dictionary entry;
        entry["id"] = static_cast<std::int64_t>(q->id());
        entry["editor_id"] = editor_id;
        entry["name"] = text(q->name());
        entry["start_game_enabled"] = (q->flags() & k_start_game_enabled) != 0;
        out.push_back(entry);
    }
    return out;
}

std::int64_t SkydotWorld::find_quest(const String& editor_id) const {
    const auto* quests = root_ != nullptr ? root_->quests() : nullptr;
    if (quests == nullptr) {
        return 0;
    }
    const String wanted = editor_id.to_lower();
    for (const auto* q : *quests) {
        if (text(q->editor_id()).to_lower() == wanted) {
            return q->id();
        }
    }
    return 0;
}

Dictionary SkydotWorld::get_quest(std::int64_t id) const {
    Dictionary out;
    const auto* q = by_id(root_ != nullptr ? root_->quests() : nullptr, static_cast<std::uint32_t>(id));
    if (q == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(q->id());
    out["editor_id"] = text(q->editor_id());
    out["name"] = text(q->name());
    out["flags"] = static_cast<std::int64_t>(q->flags());
    out["priority"] = static_cast<std::int64_t>(q->priority());
    out["type"] = static_cast<std::int64_t>(q->type());
    out["event"] = fourcc_text(q->event());
    out["start_game_enabled"] = (q->flags() & k_start_game_enabled) != 0;
    out["run_once"] = (q->flags() & k_run_once) != 0;
    out["allow_repeated_stages"] = (q->flags() & k_allow_repeated_stages) != 0;
    out["scripts"] = script_list(q->scripts(), false);
    out["fragment_script"] = text(q->fragment_script());

    Array fragments;
    if (const auto* list = q->fragments()) {
        for (const auto* f : *list) {
            Dictionary entry;
            entry["stage"] = static_cast<std::int64_t>(f->stage());
            entry["log_entry"] = static_cast<std::int64_t>(f->log_entry());
            entry["function"] = text(f->function());
            fragments.push_back(entry);
        }
    }
    out["fragments"] = fragments;

    Array stages;
    if (const auto* list = q->stages()) {
        for (const auto* st : *list) {
            Dictionary stage;
            stage["index"] = static_cast<std::int64_t>(st->index());
            stage["start_up"] = (st->flags() & k_stage_start_up) != 0;
            stage["shut_down"] = (st->flags() & k_stage_shut_down) != 0;
            Array log;
            if (const auto* entries = st->log()) {
                for (const auto* e : *entries) {
                    Dictionary entry;
                    entry["flags"] = static_cast<std::int64_t>(e->flags());
                    entry["text"] = text(e->text());
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
            objective["text"] = text(o->text());
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
            alias["name"] = text(a->name());
            alias["location"] = a->location();
            alias["flags"] = static_cast<std::int64_t>(a->flags());
            alias["optional"] = (a->flags() & k_alias_optional) != 0;
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

Dictionary SkydotWorld::get_global(std::int64_t id) const {
    Dictionary out;
    const auto* g = by_id(root_ != nullptr ? root_->globals() : nullptr, static_cast<std::uint32_t>(id));
    if (g == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(g->id());
    out["editor_id"] = text(g->editor_id());
    const char kind[2] = {static_cast<char>(g->kind()), 0};
    out["kind"] = String(kind);
    out["value"] = static_cast<double>(g->value());
    return out;
}

Dictionary SkydotWorld::get_actor(std::int64_t ref) const {
    Dictionary out;
    const auto* actors = root_ != nullptr ? root_->actors() : nullptr;
    if (actors == nullptr) {
        return out;
    }
    const auto id = static_cast<std::uint32_t>(ref);
    const auto* it = find_sorted(actors, id, [](const wfb::ActorRef* a) { return a->ref(); });
    if (it == nullptr) {
        return out;
    }
    out["ref"] = static_cast<std::int64_t>(it->ref());
    out["base"] = static_cast<std::int64_t>(it->base());
    out["cell"] = static_cast<std::int64_t>(it->cell());
    out["position"] = vec(it->position());
    out["rotation"] = vec(it->rotation());
    out["disabled"] = (it->flags() & 0x1u) != 0;
    out["persistent"] = (it->flags() & 0x2u) != 0;
    return out;
}

std::int64_t SkydotWorld::get_form_from_file(std::int64_t id, const String& plugin) const {
    const auto* plugins = root_ != nullptr ? root_->plugins() : nullptr;
    if (plugins == nullptr) {
        return 0;
    }
    const String wanted = plugin.to_lower();
    for (const auto* p : *plugins) {
        if (text(p->name()).to_lower() == wanted) {
            const std::uint32_t mask = p->light() ? 0xFFFu : 0xFFFFFFu;
            return p->prefix() | (static_cast<std::uint32_t>(id) & mask);
        }
    }
    return 0;
}

std::int64_t SkydotWorld::find_actor_of(std::int64_t npc) const {
    const auto* actors = root_ != nullptr ? root_->actors() : nullptr;
    if (actors == nullptr) {
        return 0;
    }
    if (actor_of_.empty()) {
        // Sorted by ref, so the first seen is the lowest.
        for (const auto* a : *actors) {
            actor_of_.try_emplace(a->base(), a->ref());
        }
    }
    const auto it = actor_of_.find(static_cast<std::uint32_t>(npc));
    return it != actor_of_.end() ? it->second : 0;
}

std::int64_t SkydotWorld::find_npc(const String& editor_id) const {
    const auto* npcs = root_ != nullptr ? root_->npcs() : nullptr;
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

} // namespace skydot
