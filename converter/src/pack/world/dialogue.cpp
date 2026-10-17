// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialogue.hpp"

#include "fb_write.hpp"

#include "bethconv/record/forms_dialogue.hpp"
#include "bethconv/record/forms_game.hpp"
#include "bethconv/record/forms_world.hpp"

#include <algorithm>
#include <utility>

namespace bethconv::pack::detail {
namespace {

using io::FourCC;

} // namespace

void DialogueCollector::collect(const record::MergedRecord& merged, io::SpanReader& data,
                                const record::FormContext& form_ctx) {
    switch (merged.type.value) {
    case FourCC{"DLBR"}.value:
        on_branch(merged, data, form_ctx);
        break;
    case FourCC{"DIAL"}.value:
        on_topic(merged, data, form_ctx);
        break;
    case FourCC{"INFO"}.value:
        on_info(merged, data, form_ctx);
        break;
    case FourCC{"VTYP"}.value:
        on_voice_type(merged, data, form_ctx);
        break;
    case FourCC{"LCTN"}.value:
        on_location(merged, data, form_ctx);
        break;
    case FourCC{"RELA"}.value:
        on_relationship(merged, data, form_ctx);
        break;
    case FourCC{"FACT"}.value:
        on_faction(merged, data, form_ctx);
        break;
    default:
        break;
    }
}

void DialogueCollector::on_branch(const record::MergedRecord& merged, io::SpanReader& data,
                                  const record::FormContext& form_ctx) {
    auto b = record::parse_dialogue_branch(data, form_ctx);
    if (!b) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::DialogueBranchT out;
    out.id = merged.form.value;
    out.editor_id = b->editor_id;
    out.quest = shared_.global(merged, b->quest, failed);
    out.start_topic = shared_.global(merged, b->start_topic, failed);
    out.flags = static_cast<wfb::DialogueBranchFlags>(b->flags);
    out.category = b->category;
    if (failed) {
        ++shared_.stats().unresolved;
    }
    branches_[out.id] = std::move(out);
}

void DialogueCollector::on_topic(const record::MergedRecord& merged, io::SpanReader& data,
                                 const record::FormContext& form_ctx) {
    auto t = record::parse_dialogue_topic(data, form_ctx);
    if (!t) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::DialogueTopicT out;
    out.id = merged.form.value;
    out.editor_id = t->editor_id;
    out.text = t->text.text;
    out.quest = shared_.global(merged, t->quest, failed);
    out.branch = shared_.global(merged, t->branch, failed);
    out.priority = t->priority;
    out.flags = static_cast<wfb::DialogueTopicFlags>(t->flags);
    out.category = t->category;
    out.subtype = t->subtype;
    out.subtype_code = t->subtype_code.value;
    if (failed) {
        ++shared_.stats().unresolved;
    }
    topics_[out.id] = std::move(out);
}

void DialogueCollector::on_info(const record::MergedRecord& merged, io::SpanReader& data,
                                const record::FormContext& form_ctx) {
    auto info = record::parse_dialogue_info(data, form_ctx);
    if (!info) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    PendingInfo pending;
    pending.topic = merged.parent.value;
    auto& out = pending.info;
    out.id = merged.form.value;
    out.editor_id = info->editor_id;
    out.flags = static_cast<wfb::DialogueInfoFlags>(info->flags);
    out.reset_hours = info->reset_hours;
    out.favor_level = info->favor_level;
    out.prompt = info->prompt.text;
    out.speaker = shared_.global(merged, info->speaker, failed);
    out.shared_info = shared_.global(merged, info->shared_info, failed);
    out.links = shared_.global_all(merged, info->links, failed);
    out.walk_away_topic = shared_.global(merged, info->walk_away_topic, failed);
    out.audio_output = shared_.global(merged, info->audio_output, failed);
    out.conditions = shared_.global_conditions(merged, info->conditions, failed);
    for (const auto& r : info->responses) {
        auto& t = out.responses.emplace_back(std::make_unique<wfb::DialogueResponseT>());
        t->emotion = r.emotion;
        t->emotion_value = r.emotion_value;
        t->number = r.number;
        t->sound = shared_.global(merged, r.sound, failed);
        t->flags = r.flags;
        t->text = r.text.text;
        t->notes = r.notes;
    }
    out.scripts = shared_.global_scripts(merged, info->scripts, failed);
    out.fragment_script = info->fragments.script;
    out.begin_fragment = info->fragments.begin_function;
    out.end_fragment = info->fragments.end_function;
    if (failed) {
        ++shared_.stats().unresolved;
    }
    const auto id = out.id;
    infos_[id] = std::move(pending);
}

void DialogueCollector::on_voice_type(const record::MergedRecord& merged, io::SpanReader& data,
                                      const record::FormContext& form_ctx) {
    auto v = record::parse_voice_type(data, form_ctx);
    if (!v) {
        ++shared_.stats().parse_errors;
        return;
    }
    wfb::VoiceTypeT out;
    out.id = merged.form.value;
    out.editor_id = v->editor_id;
    out.flags = v->flags;
    voice_types_[out.id] = std::move(out);
}

void DialogueCollector::on_location(const record::MergedRecord& merged, io::SpanReader& data,
                                    const record::FormContext& form_ctx) {
    auto l = record::parse_location(data, form_ctx);
    if (!l) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::LocationT out;
    out.id = merged.form.value;
    out.editor_id = l->editor_id;
    out.name = l->name.text;
    out.parent = shared_.global(merged, l->parent, failed);
    out.keywords = shared_.global_all(merged, l->keywords.keywords, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    locations_[out.id] = std::move(out);
}

void DialogueCollector::on_relationship(const record::MergedRecord& merged, io::SpanReader& data,
                                        const record::FormContext& form_ctx) {
    auto r = record::parse_relationship(data, form_ctx);
    if (!r) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::RelationshipT out;
    out.id = merged.form.value;
    out.editor_id = r->editor_id;
    out.parent = shared_.global(merged, r->parent, failed);
    out.child = shared_.global(merged, r->child, failed);
    out.rank = r->rank;
    out.association = shared_.global(merged, r->association, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    relationships_[out.id] = std::move(out);
}

void DialogueCollector::on_faction(const record::MergedRecord& merged, io::SpanReader& data,
                                   const record::FormContext& form_ctx) {
    auto f = record::parse_faction(data, form_ctx);
    if (!f) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::FactionT out;
    out.id = merged.form.value;
    out.editor_id = f->editor_id;
    out.flags = static_cast<wfb::FactionFlags>(f->flags);
    // VENV: start hour, end hour (u16 each), radius (u32), then flag bytes.
    if (f->vendor_values.size() >= 8) {
        io::SpanReader v(f->vendor_values, "FACT VENV");
        out.vendor_start_hour = v.get<std::uint16_t>().value_or(0);
        out.vendor_end_hour = v.get<std::uint16_t>().value_or(0);
        out.vendor_radius = v.get<std::uint32_t>().value_or(0);
    }
    out.vendor_chest = shared_.global(merged, f->vendor_chest, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    factions_[out.id] = std::move(out);
}

void DialogueCollector::finish(wfb::WorldT& world, const record::MergedWorld& merged) {
    auto& stats = shared_.stats();
    // Group the INFOs by topic in the order the merge first saw them.
    const auto* first = merged.records().data();
    std::vector<std::pair<std::ptrdiff_t, std::uint32_t>> order;
    order.reserve(infos_.size());
    for (const auto& [id, pending] : infos_) {
        const auto* record = merged.find(record::FormId{id});
        order.emplace_back(record != nullptr ? record - first : 0, id);
    }
    std::ranges::sort(order);
    for (const auto& [slot, id] : order) {
        auto& pending = infos_[id];
        const auto topic = topics_.find(pending.topic);
        if (topic == topics_.end()) {
            ++stats.orphan_infos;
            continue;
        }
        stats.dialogue_responses += pending.info.responses.size();
        topic->second.infos.push_back(std::make_unique<wfb::DialogueInfoT>(std::move(pending.info)));
        ++stats.dialogue_infos;
    }
    infos_.clear();
    stats.dialogue_branches += branches_.size();
    stats.dialogue_topics += topics_.size();
    stats.voice_types += voice_types_.size();
    stats.locations += locations_.size();
    stats.relationships += relationships_.size();
    stats.factions += factions_.size();
    move_into(world.dialogue_branches, branches_);
    move_into(world.dialogue_topics, topics_);
    move_into(world.voice_types, voice_types_);
    move_into(world.locations, locations_);
    move_into(world.relationships, relationships_);
    move_into(world.factions, factions_);
}

} // namespace bethconv::pack::detail
