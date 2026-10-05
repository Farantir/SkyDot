// SPDX-License-Identifier: GPL-3.0-or-later
#include "quests.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/forms_game.hpp"

#include <algorithm>
#include <utility>

namespace bethconv::pack::detail {
namespace {

using io::FourCC;

} // namespace

void QuestCollector::collect(const record::MergedRecord& merged, io::SpanReader& data,
                             const record::FormContext& form_ctx) {
    switch (merged.type.value) {
    case FourCC{"QUST"}.value:
        on_quest(merged, data, form_ctx);
        break;
    case FourCC{"GLOB"}.value:
        on_global(merged, data, form_ctx);
        break;
    default:
        break;
    }
}

void QuestCollector::on_quest(const record::MergedRecord& merged, io::SpanReader& data,
                              const record::FormContext& form_ctx) {
    auto q = record::parse_quest(data, form_ctx);
    if (!q) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::QuestT out;
    out.id = merged.form.value;
    out.editor_id = q->editor_id;
    out.name = q->name.text;
    out.flags = static_cast<wfb::QuestFlags>(q->flags);
    out.priority = q->priority;
    out.type = q->type;
    out.event = q->event.value;
    out.scripts = shared_.global_scripts(merged, q->scripts, failed);
    out.fragment_script = q->fragments.script;
    for (const auto& f : q->fragments.fragments) {
        auto& fragment = out.fragments.emplace_back(std::make_unique<wfb::QuestFragmentT>());
        fragment->stage = f.stage;
        fragment->log_entry = f.log_entry;
        fragment->function = f.function;
        if (out.fragment_script.empty()) {
            out.fragment_script = f.script;
        }
    }
    std::ranges::stable_sort(out.fragments, [](const auto& a, const auto& b) {
        return std::pair{a->stage, a->log_entry} < std::pair{b->stage, b->log_entry};
    });
    for (const auto& stage : q->stages) {
        auto& s = out.stages.emplace_back(std::make_unique<wfb::QuestStageT>());
        s->index = stage.index;
        s->flags = static_cast<wfb::StageFlags>(stage.flags);
        for (const auto& entry : stage.log) {
            auto& e = s->log.emplace_back(std::make_unique<wfb::QuestLogEntryT>());
            e->flags = static_cast<wfb::LogEntryFlags>(entry.flags);
            e->text = entry.text.text;
            e->conditions = static_cast<std::uint16_t>(entry.conditions.raw.size());
        }
    }
    std::ranges::stable_sort(out.stages, {}, [](const auto& s) { return s->index; });
    for (const auto& objective : q->objectives) {
        auto& o = out.objectives.emplace_back(std::make_unique<wfb::QuestObjectiveT>());
        o->index = objective.index;
        o->flags = objective.flags;
        o->text = objective.text.text;
        for (const auto& target : objective.targets) {
            o->targets.push_back(target.alias);
        }
    }
    for (const auto& alias : q->aliases) {
        auto& a = out.aliases.emplace_back(std::make_unique<wfb::QuestAliasT>());
        a->id = alias.id;
        a->name = alias.name;
        a->location = alias.location;
        a->flags = static_cast<wfb::AliasFlags>(alias.flags);
        a->forced = shared_.global(merged, alias.location ? alias.specific_location : alias.forced_ref,
                                   failed);
        a->unique_actor = shared_.global(merged, alias.unique_actor, failed);
        a->external_quest = shared_.global(merged, alias.external_quest, failed);
        a->external_alias = alias.external_alias;
        a->created_object = shared_.global(merged, alias.created_object, failed);
        a->create_at = alias.create_at;
        a->conditions = static_cast<std::uint16_t>(alias.conditions.raw.size());
        a->display_name = shared_.global(merged, alias.display_name, failed);
        for (auto& attached : q->fragments.aliases) {
            if (attached.alias.alias >= 0 &&
                static_cast<std::uint32_t>(attached.alias.alias) == alias.id) {
                record::ScriptData data_for_alias;
                data_for_alias.scripts = std::move(attached.scripts);
                a->scripts = shared_.global_scripts(merged, data_for_alias, failed);
            }
        }
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    quests_[out.id] = std::move(out);
}

void QuestCollector::on_global(const record::MergedRecord& merged, io::SpanReader& data,
                               const record::FormContext& form_ctx) {
    auto g = record::parse_global(data, form_ctx);
    if (!g) {
        ++shared_.stats().parse_errors;
        return;
    }
    wfb::GlobalT out;
    out.id = merged.form.value;
    out.editor_id = g->editor_id;
    out.kind = static_cast<std::uint8_t>(g->kind);
    out.value = g->value;
    globals_[out.id] = std::move(out);
}

std::vector<flatbuffers::Offset<wfb::Quest>> QuestCollector::write_quests(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Quest>> quests;
    quests.reserve(quests_.size());
    for (const auto& [id, q] : quests_) {
        quests.push_back(wfb::CreateQuest(builder, &q));
        stats.scripts += q.scripts.size();
        for (const auto& a : q.aliases) {
            stats.scripts += a->scripts.size();
        }
        stats.quest_aliases += q.aliases.size();
        stats.quest_fragments += q.fragments.size();
        ++stats.quests;
    }
    return quests;
}

std::vector<flatbuffers::Offset<wfb::Global>> QuestCollector::write_globals(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::Global>> globals;
    globals.reserve(globals_.size());
    for (const auto& [id, g] : globals_) {
        globals.push_back(wfb::CreateGlobal(builder, &g));
    }
    shared_.stats().globals += globals_.size();
    return globals;
}

} // namespace bethconv::pack::detail
