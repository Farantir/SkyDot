// SPDX-License-Identifier: GPL-3.0-or-later
#include "conditions/conditions.hpp"

#include "skydot_formats/flags.hpp"

#include <cmath>

namespace skydot::conditions {
namespace {

constexpr std::uint32_t k_subject = 0;
constexpr std::uint32_t k_target = 1;
constexpr std::uint32_t k_reference = 2;
constexpr std::uint32_t k_combat_target = 3;
constexpr std::uint32_t k_linked = 4;
constexpr std::uint32_t k_alias = 5;

bool has(const wfb::Condition& c, wfb::ConditionFlags flag) {
    return formats::has_flag(static_cast<wfb::ConditionFlags>(c.type()), flag);
}

} // namespace

bool compare(double value, std::uint8_t type, double with) {
    constexpr double k_eps = 1e-4;
    switch (type >> 5) {
    case 0:
        return std::abs(value - with) < k_eps;
    case 1:
        return std::abs(value - with) >= k_eps;
    case 2:
        return value > with + k_eps;
    case 3:
        return value > with - k_eps;
    case 4:
        return value < with - k_eps;
    case 5:
        return value < with + k_eps;
    default:
        return false;
    }
}

std::uint32_t resolve_subject(const wfb::Condition& c, const ConditionContext& ctx,
                              const ConditionWorld& world) {
    std::uint32_t subject = ctx.subject;
    std::uint32_t target = ctx.target;
    if (has(c, wfb::ConditionFlags::swap_subject_target)) {
        std::swap(subject, target);
    }
    switch (c.run_on()) {
    case k_subject:
        return subject;
    case k_target:
        return target;
    case k_reference:
        return c.reference();
    case k_combat_target:
        return world.combat_target(subject);
    case k_linked:
        return world.linked_ref(subject, 0);
    case k_alias:
        return c.param3() >= 0 ? world.alias_fill(ctx.quest, static_cast<std::uint32_t>(c.param3())) : 0;
    default: // Package data, event data.
        return 0;
    }
}

bool condition_passes(const wfb::Condition& c, ConditionWorld& world, const ConditionContext& ctx,
                      ConditionOverrides* overrides) {
    const std::uint32_t subject = resolve_subject(c, ctx, world);
    std::optional<double> value;
    if (overrides != nullptr) {
        value = overrides->function_value(c, subject, ctx);
    }
    if (!value) {
        value = function_value(c, subject, ctx, world);
    }
    if (!value) {
        world.unknowns().note(c.function());
        if (ctx.unknown == UnknownPolicy::fails) {
            return false;
        }
        value = 0.0;
    }
    const double with = has(c, wfb::ConditionFlags::use_global)
                            ? world.global_value(c.value_global())
                            : static_cast<double>(c.value());
    return compare(*value, c.type(), with);
}

bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list,
                     ConditionWorld& world, const ConditionContext& ctx,
                     ConditionOverrides* overrides) {
    if (list == nullptr) {
        return true;
    }
    bool all = true;
    bool any = false;
    bool open = false;
    for (const auto* c : *list) {
        any = condition_passes(*c, world, ctx, overrides) || any;
        open = true;
        if (!has(*c, wfb::ConditionFlags::or_next)) {
            all = all && any;
            any = false;
            open = false;
        }
    }
    if (open) { // A trailing OR closes its run at the end.
        all = all && any;
    }
    return all;
}

} // namespace skydot::conditions
