// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/conditions.hpp"
#include "bethconv/record/forms_game.hpp"

#include <memory>
#include <utility>

namespace bethconv::pack::detail {
namespace {

using io::FourCC;
using record::FormId;

/// Conditions as world.fb's tables, with their FormID parameters made global.
std::vector<std::unique_ptr<wfb::ConditionT>> global_conditions(
    const CollectContext& shared, const record::MergedRecord& merged,
    const std::vector<record::Condition>& list, bool& failed) {
    std::vector<std::unique_ptr<wfb::ConditionT>> out;
    out.reserve(list.size());
    for (const auto& c : list) {
        auto& t = out.emplace_back(std::make_unique<wfb::ConditionT>());
        t->type = c.type;
        t->function = c.function;
        t->value = c.value;
        t->value_global = shared.global(merged, c.value_global, failed);
        t->param1 = record::condition_param_is_form(c, 1)
                        ? shared.global(merged, FormId{c.param1}, failed)
                        : c.param1;
        t->param2 = record::condition_param_is_form(c, 2)
                        ? shared.global(merged, FormId{c.param2}, failed)
                        : c.param2;
        t->run_on = c.run_on;
        t->reference = c.run_on == record::Condition::k_run_on_reference
                           ? shared.global(merged, c.reference, failed)
                           : c.reference.value;
        t->param3 = c.param3;
        t->string1 = c.string1;
        t->string2 = c.string2;
    }
    return out;
}

} // namespace

void AiCollector::collect(const record::MergedRecord& merged, io::SpanReader& data,
                          const record::FormContext& form_ctx) {
    switch (merged.type.value) {
    case FourCC{"PACK"}.value:
        on_package(merged, data, form_ctx);
        break;
    case FourCC{"FLST"}.value:
        on_form_list(merged, data, form_ctx);
        break;
    default:
        break;
    }
}

/// FLST: kept to expand NPCs' default package lists.
void AiCollector::on_form_list(const record::MergedRecord& merged, io::SpanReader& data,
                               const record::FormContext& form_ctx) {
    auto list = record::parse_form_list(data, form_ctx);
    if (!list) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    form_lists_[merged.form.value] = shared_.global_all(merged, list->forms, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
}

void AiCollector::on_package(const record::MergedRecord& merged, io::SpanReader& data,
                             const record::FormContext& form_ctx) {
    auto pack = record::parse_package(data, form_ctx);
    if (!pack) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::PackageT out;
    out.id = merged.form.value;
    out.editor_id = pack->editor_id;
    out.type = pack->type;
    out.flags = static_cast<wfb::PackageFlags>(pack->flags);
    out.interrupt_override = pack->interrupt_override;
    out.speed = pack->preferred_speed;
    out.interrupt_flags = pack->interrupt_flags;
    out.month = pack->schedule.month;
    out.day_of_week = pack->schedule.day_of_week;
    out.date = pack->schedule.date;
    out.hour = pack->schedule.hour;
    out.minute = pack->schedule.minute;
    out.duration = pack->schedule.duration;
    out.conditions = global_conditions(shared_, merged, pack->conditions, failed);
    out.template_ = shared_.global(merged, pack->template_package, failed);
    out.idle_flags = pack->idle_flags;
    out.idle_timer = pack->idle_timer;
    out.idles = shared_.global_all(merged, pack->idles, failed);
    out.owner_quest = shared_.global(merged, pack->owner_quest, failed);
    out.combat_style = shared_.global(merged, pack->combat_style, failed);
    out.on_begin_idle = shared_.global(merged, pack->on_begin.idle, failed);
    out.on_end_idle = shared_.global(merged, pack->on_end.idle, failed);
    out.on_change_idle = shared_.global(merged, pack->on_change.idle, failed);
    for (const auto& in : pack->inputs) {
        auto& w = out.inputs.emplace_back(std::make_unique<wfb::PackageInputT>());
        w->key = in.key;
        w->type = in.type;
        // CNAM: one byte for Bool, a word otherwise; Float and ObjectList
        // (a radius) hold floats, Int an integer.
        io::SpanReader v{in.value, "PACK CNAM"};
        if (in.value.size() == 1) {
            w->number = static_cast<float>(v.get<std::uint8_t>().value_or(0));
        } else if (in.value.size() == 4) {
            w->number = in.type == "Int" ? static_cast<float>(v.get<std::int32_t>().value_or(0))
                                         : v.get<float>().value_or(0.0F);
        }
        if (in.location) {
            const auto t = in.location->type;
            w->location_type = t;
            w->location_value = t == 0 || t == 1 || t == 4 || t == 6
                                    ? shared_.global(merged, FormId{in.location->value}, failed)
                                    : in.location->value;
            w->location_radius = in.location->radius;
        }
        if (in.target) {
            const auto t = in.target->type;
            w->target_type = t;
            w->target_value = t == 0 || t == 1 || t == 3
                                  ? shared_.global(merged, FormId{in.target->value}, failed)
                                  : in.target->value;
            w->target_count = in.target->count;
        }
        for (const auto& named : pack->public_inputs) {
            if (named.key == in.key) {
                w->name = named.name;
            }
        }
    }
    for (const auto& b : pack->branches) {
        auto& w = out.branches.emplace_back(std::make_unique<wfb::PackageBranchT>());
        w->type = b.type;
        w->conditions = global_conditions(shared_, merged, b.conditions, failed);
        w->children = b.branch_count;
        w->flags = static_cast<wfb::BranchFlags>(b.root_flags);
        w->procedure = b.procedure;
        w->success_completes = b.success_completes;
        w->inputs = b.input_keys;
        if (!b.flag_overrides.empty()) {
            const auto& o = b.flag_overrides.front();
            w->set_flags = o.set_flags;
            w->clear_flags = o.clear_flags;
            w->speed = static_cast<std::int8_t>(o.speed);
        }
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    packages_[out.id] = std::move(out);
}

std::vector<flatbuffers::Offset<wfb::Package>> AiCollector::write_packages(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::Package>> packages;
    packages.reserve(packages_.size());
    for (const auto& [id, p] : packages_) {
        packages.push_back(wfb::CreatePackage(builder, &p));
    }
    shared_.stats().packages += packages_.size();
    return packages;
}

} // namespace bethconv::pack::detail
