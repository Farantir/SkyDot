// SPDX-License-Identifier: GPL-3.0-or-later
#include "quests.hpp"

#include "fb_write.hpp"

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
    WorldQuest out{
        .id = merged.form.value,
        .editor_id = q->editor_id,
        .name = q->name.text,
        .flags = static_cast<wfb::QuestFlags>(q->flags),
        .priority = q->priority,
        .type = q->type,
        .event = q->event.value,
        .scripts = shared_.global_scripts(merged, q->scripts, failed),
        .fragment_script = q->fragments.script,
        .fragments = {},
        .stages = {},
        .objectives = {},
        .aliases = {},
    };
    for (const auto& f : q->fragments.fragments) {
        out.fragments.push_back(WorldQuestFragment{
            .stage = f.stage, .log_entry = f.log_entry, .function = f.function});
        if (out.fragment_script.empty()) {
            out.fragment_script = f.script;
        }
    }
    std::ranges::stable_sort(out.fragments, [](const auto& a, const auto& b) {
        return std::pair{a.stage, a.log_entry} < std::pair{b.stage, b.log_entry};
    });
    for (const auto& stage : q->stages) {
        auto& s = out.stages.emplace_back();
        s.index = stage.index;
        s.flags = static_cast<wfb::StageFlags>(stage.flags);
        for (const auto& entry : stage.log) {
            s.log.push_back(WorldQuestLogEntry{
                .flags = static_cast<wfb::LogEntryFlags>(entry.flags),
                .text = entry.text.text,
                .conditions = static_cast<std::uint16_t>(entry.conditions.raw.size())});
        }
    }
    std::ranges::stable_sort(out.stages, {}, &WorldQuestStage::index);
    for (const auto& objective : q->objectives) {
        auto& o = out.objectives.emplace_back();
        o.index = objective.index;
        o.flags = objective.flags;
        o.text = objective.text.text;
        for (const auto& target : objective.targets) {
            o.targets.push_back(target.alias);
        }
    }
    for (const auto& alias : q->aliases) {
        WorldQuestAlias a{
            .id = alias.id,
            .name = alias.name,
            .location = alias.location,
            .flags = static_cast<wfb::AliasFlags>(alias.flags),
            .forced = shared_.global(merged,
                                     alias.location ? alias.specific_location : alias.forced_ref,
                                     failed),
            .unique_actor = shared_.global(merged, alias.unique_actor, failed),
            .external_quest = shared_.global(merged, alias.external_quest, failed),
            .external_alias = alias.external_alias,
            .created_object = shared_.global(merged, alias.created_object, failed),
            .create_at = alias.create_at,
            .conditions = static_cast<std::uint16_t>(alias.conditions.raw.size()),
            .display_name = shared_.global(merged, alias.display_name, failed),
            .scripts = {},
        };
        for (auto& attached : q->fragments.aliases) {
            if (attached.alias.alias >= 0 &&
                static_cast<std::uint32_t>(attached.alias.alias) == alias.id) {
                record::ScriptData data_for_alias;
                data_for_alias.scripts = std::move(attached.scripts);
                a.scripts = shared_.global_scripts(merged, data_for_alias, failed);
            }
        }
        out.aliases.push_back(std::move(a));
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
    globals_[merged.form.value] = WorldGlobal{
        .id = merged.form.value, .editor_id = g->editor_id, .kind = g->kind, .value = g->value};
}

std::vector<flatbuffers::Offset<wfb::Quest>> QuestCollector::write_quests(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Quest>> quests;
    for (const auto& [id, q] : quests_) {
        std::vector<flatbuffers::Offset<wfb::QuestFragment>> fragments;
        for (const auto& f : q.fragments) {
            fragments.push_back(wfb::CreateQuestFragment(builder, f.stage, f.log_entry,
                                                         builder.CreateString(f.function)));
        }
        std::vector<flatbuffers::Offset<wfb::QuestStage>> stages;
        for (const auto& stage : q.stages) {
            std::vector<flatbuffers::Offset<wfb::QuestLogEntry>> log;
            for (const auto& e : stage.log) {
                log.push_back(wfb::CreateQuestLogEntry(builder, e.flags,
                                                       builder.CreateString(e.text),
                                                       e.conditions));
            }
            stages.push_back(wfb::CreateQuestStage(builder, stage.index, stage.flags,
                                                   builder.CreateVector(log)));
        }
        std::vector<flatbuffers::Offset<wfb::QuestObjective>> objectives;
        for (const auto& o : q.objectives) {
            objectives.push_back(wfb::CreateQuestObjective(builder, o.index, o.flags,
                                                           builder.CreateString(o.text),
                                                           builder.CreateVector(o.targets)));
        }
        std::vector<flatbuffers::Offset<wfb::QuestAlias>> aliases;
        for (const auto& a : q.aliases) {
            const auto name = builder.CreateString(a.name);
            const auto scripts = a.scripts.empty() ? 0 : write_scripts(builder, a.scripts);
            stats.scripts += a.scripts.size();
            wfb::QuestAliasBuilder ab(builder);
            ab.add_id(a.id);
            ab.add_name(name);
            ab.add_location(a.location);
            ab.add_flags(a.flags);
            ab.add_forced(a.forced);
            ab.add_unique_actor(a.unique_actor);
            ab.add_external_quest(a.external_quest);
            ab.add_external_alias(a.external_alias);
            ab.add_created_object(a.created_object);
            ab.add_create_at(a.create_at);
            ab.add_conditions(a.conditions);
            ab.add_display_name(a.display_name);
            if (!a.scripts.empty()) {
                ab.add_scripts(scripts);
            }
            aliases.push_back(ab.Finish());
        }
        stats.quest_aliases += q.aliases.size();
        stats.quest_fragments += q.fragments.size();
        stats.scripts += q.scripts.size();
        const auto editor_id = builder.CreateString(q.editor_id);
        const auto name = builder.CreateString(q.name);
        const auto scripts = q.scripts.empty() ? 0 : write_scripts(builder, q.scripts);
        const auto fragment_script = builder.CreateString(q.fragment_script);
        const auto fragments_off = builder.CreateVector(fragments);
        const auto stages_off = builder.CreateVector(stages);
        const auto objectives_off = builder.CreateVector(objectives);
        const auto aliases_off = builder.CreateVector(aliases);
        wfb::QuestBuilder qb(builder);
        qb.add_id(q.id);
        qb.add_editor_id(editor_id);
        qb.add_name(name);
        qb.add_flags(q.flags);
        qb.add_priority(q.priority);
        qb.add_type(q.type);
        qb.add_event(q.event);
        if (!q.scripts.empty()) {
            qb.add_scripts(scripts);
        }
        qb.add_fragment_script(fragment_script);
        qb.add_fragments(fragments_off);
        qb.add_stages(stages_off);
        qb.add_objectives(objectives_off);
        qb.add_aliases(aliases_off);
        quests.push_back(qb.Finish());
        ++stats.quests;
    }
    return quests;
}

std::vector<flatbuffers::Offset<wfb::Global>> QuestCollector::write_globals(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Global>> globals;
    for (const auto& [id, g] : globals_) {
        globals.push_back(wfb::CreateGlobal(builder, g.id, builder.CreateString(g.editor_id),
                                            static_cast<std::uint8_t>(g.kind), g.value));
        ++stats.globals;
    }
    return globals;
}

} // namespace bethconv::pack::detail
