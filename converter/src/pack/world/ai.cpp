// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/conditions.hpp"
#include "bethconv/record/forms_game.hpp"

#include <utility>

namespace bethconv::pack::detail {
namespace {

using io::FourCC;
using record::FormId;

/// A condition with its FormID parameters made global.
record::Condition global_condition(const CollectContext& shared,
                                   const record::MergedRecord& merged, record::Condition c,
                                   bool& failed) {
    c.value_global = FormId{shared.global(merged, c.value_global, failed)};
    if (c.run_on == record::Condition::k_run_on_reference) {
        c.reference = FormId{shared.global(merged, c.reference, failed)};
    }
    if (record::condition_param_is_form(c, 1)) {
        c.param1 = shared.global(merged, FormId{c.param1}, failed);
    }
    if (record::condition_param_is_form(c, 2)) {
        c.param2 = shared.global(merged, FormId{c.param2}, failed);
    }
    return c;
}

std::vector<record::Condition> global_conditions(const CollectContext& shared,
                                                 const record::MergedRecord& merged,
                                                 std::vector<record::Condition> list,
                                                 bool& failed) {
    for (auto& c : list) {
        c = global_condition(shared, merged, std::move(c), failed);
    }
    return list;
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
    WorldPackage out{
        .id = merged.form.value,
        .editor_id = pack->editor_id,
        .type = pack->type,
        .flags = static_cast<wfb::PackageFlags>(pack->flags),
        .interrupt_override = pack->interrupt_override,
        .speed = pack->preferred_speed,
        .interrupt_flags = pack->interrupt_flags,
        .schedule = pack->schedule,
        .conditions = global_conditions(shared_, merged, std::move(pack->conditions), failed),
        .template_package = shared_.global(merged, pack->template_package, failed),
        .idle_flags = pack->idle_flags,
        .idle_timer = pack->idle_timer,
        .idles = shared_.global_all(merged, pack->idles, failed),
        .owner_quest = shared_.global(merged, pack->owner_quest, failed),
        .combat_style = shared_.global(merged, pack->combat_style, failed),
        .on_begin_idle = shared_.global(merged, pack->on_begin.idle, failed),
        .on_end_idle = shared_.global(merged, pack->on_end.idle, failed),
        .on_change_idle = shared_.global(merged, pack->on_change.idle, failed),
    };
    for (const auto& in : pack->inputs) {
        WorldPackage::Input w{.key = in.key, .type = in.type};
        // CNAM: one byte for Bool, a word otherwise; Float and ObjectList
        // (a radius) hold floats, Int an integer.
        io::SpanReader v{in.value, "PACK CNAM"};
        if (in.value.size() == 1) {
            w.number = static_cast<float>(v.get<std::uint8_t>().value_or(0));
        } else if (in.value.size() == 4) {
            w.number = in.type == "Int" ? static_cast<float>(v.get<std::int32_t>().value_or(0))
                                        : v.get<float>().value_or(0.0F);
        }
        if (in.location) {
            w.location = *in.location;
            const auto t = w.location.type;
            if (t == 0 || t == 1 || t == 4 || t == 6) {
                w.location.value = shared_.global(merged, FormId{w.location.value}, failed);
            }
        }
        if (in.target) {
            w.target = *in.target;
            const auto t = w.target.type;
            if (t == 0 || t == 1 || t == 3) {
                w.target.value = shared_.global(merged, FormId{w.target.value}, failed);
            }
        }
        for (const auto& named : pack->public_inputs) {
            if (named.key == in.key) {
                w.name = named.name;
            }
        }
        out.inputs.push_back(std::move(w));
    }
    for (auto& b : pack->branches) {
        WorldPackage::Branch w{
            .type = b.type,
            .conditions = global_conditions(shared_, merged, std::move(b.conditions), failed),
            .children = b.branch_count,
            .flags = static_cast<wfb::BranchFlags>(b.root_flags),
            .procedure = b.procedure,
            .success_completes = b.success_completes,
            .inputs = b.input_keys,
        };
        if (!b.flag_overrides.empty()) {
            const auto& o = b.flag_overrides.front();
            w.set_flags = o.set_flags;
            w.clear_flags = o.clear_flags;
            w.speed = static_cast<std::int8_t>(o.speed);
        }
        out.branches.push_back(std::move(w));
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    packages_[out.id] = std::move(out);
}

std::vector<flatbuffers::Offset<wfb::Package>> AiCollector::write_packages(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    const auto conditions = [&](const std::vector<record::Condition>& list) {
        std::vector<flatbuffers::Offset<wfb::Condition>> offsets;
        for (const auto& c : list) {
            offsets.push_back(wfb::CreateCondition(
                builder, c.type, c.function, c.value, c.value_global.value, c.param1, c.param2,
                c.run_on, c.reference.value, c.param3,
                c.string1.empty() ? 0 : builder.CreateString(c.string1),
                c.string2.empty() ? 0 : builder.CreateString(c.string2)));
        }
        return builder.CreateVector(offsets);
    };
    std::vector<flatbuffers::Offset<wfb::Package>> packages;
    for (const auto& [id, p] : packages_) {
        std::vector<flatbuffers::Offset<wfb::PackageInput>> inputs;
        for (const auto& in : p.inputs) {
            inputs.push_back(wfb::CreatePackageInput(
                builder, in.key, builder.CreateString(in.type),
                in.name.empty() ? 0 : builder.CreateString(in.name), in.number, in.location.type,
                in.location.value, in.location.radius, in.target.type, in.target.value,
                in.target.count));
        }
        std::vector<flatbuffers::Offset<wfb::PackageBranch>> branches;
        for (const auto& b : p.branches) {
            branches.push_back(wfb::CreatePackageBranch(
                builder, builder.CreateString(b.type), conditions(b.conditions), b.children,
                b.flags, b.procedure.empty() ? 0 : builder.CreateString(b.procedure),
                b.success_completes, builder.CreateVector(b.inputs), b.set_flags, b.clear_flags,
                b.speed));
        }
        const auto& sch = p.schedule;
        packages.push_back(wfb::CreatePackage(
            builder, p.id, builder.CreateString(p.editor_id), p.type, p.flags,
            p.interrupt_override, p.speed, p.interrupt_flags, sch.month, sch.day_of_week,
            sch.date, sch.hour, sch.minute, sch.duration, conditions(p.conditions),
            p.template_package, builder.CreateVector(inputs), builder.CreateVector(branches),
            p.idle_flags, p.idle_timer, builder.CreateVector(p.idles), p.owner_quest,
            p.combat_style, p.on_begin_idle, p.on_end_idle, p.on_change_idle));
        ++stats.packages;
    }
    return packages;
}

} // namespace bethconv::pack::detail
