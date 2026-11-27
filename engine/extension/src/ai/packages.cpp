// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/packages.hpp"

#include "data/actors.hpp"
#include "data/fb_search.hpp"

#include "skydot_formats/flags.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>

namespace skydot::ai {
namespace {

constexpr int k_minutes_per_day = 24 * 60;
/// Template chains are short in vanilla (a package, its template); this only
/// stops loops.
constexpr int k_max_chain = 8;
constexpr std::int32_t k_no_key = 255;

/// Answers GetNumericPackageData from a package's inputs, the rest from the
/// caller's overrides; a condition run on package data runs on the reference
/// of that input.
class PackageDataOverrides final : public conditions::ConditionOverrides {
public:
    PackageDataOverrides(const Package& pkg, const ConditionSource& source, std::uint32_t actor)
        : pkg_(pkg), inner_(source.overrides), world_(source.world), actor_(actor) {}
    std::optional<double> function_value(const wfb::Condition& c, std::uint32_t subject,
                                         const conditions::ConditionContext& ctx) override {
        if (c.function() == k_get_numeric_package_data) {
            return pkg_.number(static_cast<std::int32_t>(c.param1()));
        }
        conditions::ConditionContext with = ctx;
        with.package = &pkg_;
        if (c.run_on() == k_run_on_package_data && subject == 0 && world_ != nullptr) {
            const auto* in = pkg_.input(c.param3());
            subject = in != nullptr ? input_ref(*in, actor_) : 0;
            if (auto v = inner_ != nullptr ? inner_->function_value(c, subject, with) : std::nullopt) {
                return v;
            }
            return conditions::function_value(c, subject, with, *world_);
        }
        return inner_ != nullptr ? inner_->function_value(c, subject, with) : std::nullopt;
    }

private:
    const Package& pkg_;
    conditions::ConditionOverrides* inner_;
    conditions::ConditionWorld* world_;
    std::uint32_t actor_;
};

conditions::ConditionContext context_of(std::uint32_t actor) {
    return {.subject = actor,
            .target = actor,
            .unknown = conditions::UnknownPolicy::zero};
}

struct Node {
    const wfb::PackageBranch* branch = nullptr;
    std::vector<std::size_t> children;
};

/// Pre-order branches into a tree. Returns the index after `at`'s subtree.
std::size_t parse(const flatbuffers::Vector<flatbuffers::Offset<wfb::PackageBranch>>& list,
                  std::size_t at, std::vector<Node>& nodes, int depth) {
    const std::size_t self = nodes.size();
    nodes.push_back(Node{list.Get(static_cast<flatbuffers::uoffset_t>(at)), {}});
    std::size_t next = at + 1;
    const std::uint32_t children = nodes[self].branch->children();
    for (std::uint32_t i = 0; i < children && next < list.size() && depth < 32; ++i) {
        nodes[self].children.push_back(nodes.size());
        next = parse(list, next, nodes, depth + 1);
    }
    return next;
}

std::string_view sv(const flatbuffers::String* s) {
    return s == nullptr ? std::string_view() : s->string_view();
}

class Planner {
public:
    Planner(const Package& pkg, const ConditionSource& source, std::uint32_t actor, const Dice& dice)
        : pkg_(pkg), data_(pkg, source, actor), host_(*source.world, &data_), actor_(actor),
          dice_(dice) {}

    std::vector<Step> run() {
        const auto* branches = pkg_.tree != nullptr ? pkg_.tree->branches() : nullptr;
        if (branches == nullptr || branches->size() == 0) {
            return {};
        }
        parse(*branches, 0, nodes_, 0);
        return flatten(0).value_or(std::vector<Step>{});
    }

private:
    /// The node's steps, or nothing if its conditions fail.
    std::optional<std::vector<Step>> flatten(std::size_t index) {
        if (!conditions_pass(nodes_[index].branch->conditions(), host_, actor_)) {
            return std::nullopt;
        }
        return expand(index);
    }

    /// The steps of a node whose conditions have passed. A Random node asks
    /// its children's to find the open ones, and the picked one must not be
    /// asked again: GetRandomPercent can answer differently the second time.
    std::vector<Step> expand(std::size_t index) {
        const Node& node = nodes_[index];
        const std::string_view type = sv(node.branch->type());
        std::vector<Step> out;
        if (type == "Procedure" || node.children.empty()) {
            if (node.branch->procedure() != nullptr) {
                out.push_back(step(*node.branch));
            }
            return out;
        }
        if (type == "Stacked") {
            for (const auto child : node.children) {
                if (auto steps = flatten(child)) {
                    return std::move(*steps);
                }
            }
            return out;
        }
        if (type == "Random") {
            std::vector<std::size_t> open;
            for (const auto child : node.children) {
                if (conditions_pass(nodes_[child].branch->conditions(), host_, actor_)) {
                    open.push_back(child);
                }
            }
            if (open.empty()) {
                return out;
            }
            const auto pick = std::min(open.size() - 1,
                                       static_cast<std::size_t>(dice_() * static_cast<double>(open.size())));
            return expand(open[pick]);
        }
        if (type == "Simultaneous") {
            std::vector<Step> all;
            for (const auto child : node.children) {
                if (auto steps = flatten(child)) {
                    all.insert(all.end(), steps->begin(), steps->end());
                }
            }
            return merge_simultaneous(std::move(all));
        }
        // Sequence, and anything else, in order.
        for (const auto child : node.children) {
            if (auto steps = flatten(child)) {
                out.insert(out.end(), steps->begin(), steps->end());
            }
        }
        return out;
    }

    /// The first moving or staying step runs for as long as the Waits beside
    /// it; Finds come first, so it can use what they find.
    static std::vector<Step> merge_simultaneous(std::vector<Step> all) {
        std::vector<Step> out;
        double seconds = 0.0;
        const Step* main = nullptr;
        for (const auto& s : all) {
            if (s.procedure == Procedure::find || s.procedure == Procedure::unlock_doors ||
                s.procedure == Procedure::lock_doors) {
                out.push_back(s);
            } else if (s.procedure == Procedure::wait) {
                seconds = std::max(seconds, s.seconds);
            } else if (main == nullptr) {
                main = &s;
            }
        }
        if (main != nullptr) {
            Step m = *main;
            if (seconds > 0.0) {
                m.seconds = m.seconds > 0.0 ? std::min(m.seconds, seconds) : seconds;
            }
            out.push_back(std::move(m));
        } else if (const auto wait = std::ranges::find(all, Procedure::wait, &Step::procedure);
                   wait != all.end()) {
            out.push_back(*wait);
        }
        return out;
    }

    Step step(const wfb::PackageBranch& b) const {
        Step s;
        s.name = std::string(sv(b.procedure()));
        s.procedure = procedure_of(s.name);
        if (const auto* keys = b.inputs()) {
            for (const auto key : *keys) {
                const auto* in = key != k_no_key ? pkg_.input(key) : nullptr;
                s.params.push_back(in);
                if (in != nullptr && sv(in->type()) == "ObjectList" && s.list_key < 0) {
                    s.list_key = key;
                } else if (in != nullptr && sv(in->type()) == "Float" && s.procedure == Procedure::wait &&
                           s.seconds == 0.0) {
                    s.seconds = static_cast<double>(in->number());
                }
            }
        }
        const auto at = [&](int i) -> const wfb::PackageInput* {
            return i >= 0 && static_cast<std::size_t>(i) < s.params.size() ? s.params[static_cast<std::size_t>(i)]
                                                                          : nullptr;
        };
        const auto is_ref = [](const wfb::PackageInput* in) {
            const std::string_view type = in != nullptr ? sv(in->type()) : std::string_view();
            return type == "SingleRef" || type == "TargetSelector";
        };
        if (const auto slots = slots_of(s.name)) {
            // The place may be a Location or a reference (a Travel to "Player"); the target is a reference.
            const auto* place = at(slots->place);
            s.location = place != nullptr && (sv(place->type()) == "Location" || is_ref(place)) ? place : nullptr;
            s.target = is_ref(at(slots->target)) ? at(slots->target) : nullptr;
            return s;
        }
        for (const auto* in : s.params) {
            if (in != nullptr && sv(in->type()) == "Location" && s.location == nullptr) {
                s.location = in;
            } else if (is_ref(in) && s.target == nullptr) {
                s.target = in;
            }
        }
        return s;
    }

    /// Which parameter of a procedure says where it goes and which whom it is about, from the vanilla
    /// templates' parameter lists (docs/ai.md); -1 none.
    struct Slots {
        int place;
        int target;
    };
    static std::optional<Slots> slots_of(std::string_view procedure) {
        static constexpr std::pair<std::string_view, Slots> k_slots[] = {
            {"Travel", {0, -1}},       {"Sandbox", {0, -1}},       {"Wander", {0, -1}},
            {"HoldPosition", {0, -1}}, {"UnlockDoors", {0, -1}},   {"LockDoors", {0, -1}},
            {"Guard", {0, 1}},         {"Find", {0, 1}},           {"Sit", {-1, 0}},
            {"Sleep", {-1, 0}},        {"Eat", {-1, 0}},           {"UseIdleMarker", {-1, 0}},
            {"Activate", {-1, 0}},     {"Patrol", {-1, 0}},        {"Follow", {-1, 0}},
            {"ForceGreet", {1, -1}},   {"Escort", {2, 0}},         {"FollowTo", {1, 0}},
            {"KeepAnEyeOn", {1, 0}},   {"Flee", {1, 0}},           {"DialogueActivate", {1, 0}},
            {"Shout", {0, 2}},         {"UseWeapon", {0, 2}},      {"UseMagic", {0, 2}},
            {"Hover", {0, -1}},        {"Orbit", {0, -1}},
        };
        for (const auto& [name, slots] : k_slots) {
            if (name == procedure) {
                return slots;
            }
        }
        return std::nullopt;
    }

    const Package& pkg_;
    PackageDataOverrides data_;
    ConditionSource host_;
    std::uint32_t actor_;
    const Dice& dice_;
    std::vector<Node> nodes_;
};

} // namespace

bool schedule_active(const wfb::Package& p, const Clock& clock) {
    if (p.month() >= 0 && p.month() != clock.month()) {
        return false;
    }
    if (p.date() > 0 && p.date() != clock.date()) {
        return false;
    }
    const int dow = clock.day_of_week();
    switch (p.day_of_week()) {
    case -1:
        break;
    case 7: // Weekdays
        if (dow == 0 || dow == 6) {
            return false;
        }
        break;
    case 8: // Weekends
        if (dow != 0 && dow != 6) {
            return false;
        }
        break;
    case 9: // Monday, Wednesday, Friday
        if (dow != 1 && dow != 3 && dow != 5) {
            return false;
        }
        break;
    case 10: // Tuesday, Thursday
        if (dow != 2 && dow != 4) {
            return false;
        }
        break;
    default:
        if (p.day_of_week() != dow) {
            return false;
        }
    }
    if (p.hour() < 0) {
        return true;
    }
    const int start = p.hour() * 60 + std::max<int>(p.minute(), 0);
    const int length = p.duration() != 0 ? static_cast<int>(p.duration()) : 60;
    if (length >= k_minutes_per_day) {
        return true;
    }
    const int now = static_cast<int>(std::floor(clock.hour() * 60.0));
    const int since = ((now - start) % k_minutes_per_day + k_minutes_per_day) % k_minutes_per_day;
    return since < length;
}

bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list,
                     const ConditionSource& source, std::uint32_t actor) {
    return conditions::conditions_pass(list, *source.world, context_of(actor), source.overrides);
}

const wfb::PackageInput* Package::input(std::int32_t key) const {
    for (const auto* p : chain) {
        if (const auto* inputs = p->inputs()) {
            for (const auto* in : *inputs) {
                if (in->key() == key) {
                    return in;
                }
            }
        }
    }
    return nullptr;
}

double Package::number(std::int32_t key, double fallback) const {
    const auto* in = input(key);
    return in != nullptr ? static_cast<double>(in->number()) : fallback;
}

std::string Package::editor_id() const {
    return own != nullptr && own->editor_id() != nullptr ? own->editor_id()->str() : std::string();
}

const wfb::Package* find_package(const wfb::World& world, std::uint32_t id) {
    return lookup(world.packages(), id);
}

Package resolve_package(const wfb::World& world, std::uint32_t id) {
    Package out;
    const auto* p = find_package(world, id);
    out.own = p;
    for (int depth = 0; p != nullptr && depth < k_max_chain; ++depth) {
        out.chain.push_back(p);
        if (p->branches() != nullptr && p->branches()->size() != 0) {
            out.tree = p;
            break;
        }
        p = p->template_() != 0 ? find_package(world, p->template_()) : nullptr;
    }
    return out;
}

Procedure procedure_of(std::string_view name) {
    static constexpr std::pair<std::string_view, Procedure> k_names[] = {
        {"Travel", Procedure::travel},
        {"Sandbox", Procedure::sandbox},
        {"Wander", Procedure::wander},
        {"Sleep", Procedure::sleep},
        {"Sit", Procedure::sit},
        {"Eat", Procedure::eat},
        {"UseIdleMarker", Procedure::use_idle_marker},
        {"Activate", Procedure::activate},
        {"HoldPosition", Procedure::hold_position},
        {"Guard", Procedure::guard},
        {"Patrol", Procedure::patrol},
        {"Find", Procedure::find},
        {"Wait", Procedure::wait},
        {"UnlockDoors", Procedure::unlock_doors},
        {"LockDoors", Procedure::lock_doors},
        {"ForceGreet", Procedure::force_greet},
        {"Follow", Procedure::follow},
        {"Escort", Procedure::escort},
    };
    for (const auto& [n, p] : k_names) {
        if (n == name) {
            return p;
        }
    }
    return Procedure::other;
}

const char* procedure_name(Procedure p) {
    switch (p) {
    case Procedure::travel: return "travel";
    case Procedure::sandbox: return "sandbox";
    case Procedure::wander: return "wander";
    case Procedure::sleep: return "sleep";
    case Procedure::sit: return "sit";
    case Procedure::eat: return "eat";
    case Procedure::use_idle_marker: return "use_idle_marker";
    case Procedure::activate: return "activate";
    case Procedure::hold_position: return "hold_position";
    case Procedure::guard: return "guard";
    case Procedure::patrol: return "patrol";
    case Procedure::find: return "find";
    case Procedure::wait: return "wait";
    case Procedure::unlock_doors: return "unlock_doors";
    case Procedure::lock_doors: return "lock_doors";
    case Procedure::force_greet: return "force_greet";
    case Procedure::follow: return "follow";
    case Procedure::escort: return "escort";
    case Procedure::other: return "other";
    }
    return "other";
}

std::vector<Step> plan_steps(const Package& pkg, const ConditionSource& host, std::uint32_t actor,
                             const Dice& dice) {
    return Planner(pkg, host, actor, dice).run();
}

bool repeats(const Package& pkg) {
    const auto* branches = pkg.tree != nullptr ? pkg.tree->branches() : nullptr;
    return branches != nullptr && branches->size() != 0 &&
           formats::has_flag(branches->Get(0)->flags(), wfb::BranchFlags::repeat_when_complete);
}

bool replans(const Package& pkg) {
    const auto* branches = pkg.tree != nullptr ? pkg.tree->branches() : nullptr;
    if (branches == nullptr) {
        return false;
    }
    for (const auto* b : *branches) {
        if (const auto* list = b->conditions()) {
            for (const auto* c : *list) {
                if (c->function() == k_get_within_package_location || c->function() == k_get_detected) {
                    return true;
                }
            }
        }
    }
    return false;
}

double Step::number(std::size_t i, double fallback) const {
    return i < params.size() && params[i] != nullptr ? static_cast<double>(params[i]->number()) : fallback;
}

bool Step::same_as(const Step& other) const {
    return procedure == other.procedure && name == other.name && location == other.location &&
           target == other.target && list_key == other.list_key && seconds == other.seconds &&
           params == other.params;
}

std::uint32_t input_ref(const wfb::PackageInput& in, std::uint32_t actor) {
    constexpr std::int32_t k_specific_reference = 0;
    constexpr std::int32_t k_self = 6;
    constexpr std::int32_t k_near_reference = 0;
    constexpr std::int32_t k_near_self = 12;
    const std::string_view type = sv(in.type());
    if (type == "SingleRef" || type == "TargetSelector") {
        return in.target_type() == k_specific_reference ? in.target_value()
               : in.target_type() == k_self             ? actor
                                                        : 0;
    }
    if (type == "Location") {
        return in.location_type() == k_near_reference ? in.location_value()
               : in.location_type() == k_near_self    ? actor
                                                      : 0;
    }
    return 0;
}

std::vector<std::uint32_t> package_list(const wfb::World& world, std::uint32_t npc, std::uint32_t actor) {
    std::vector<std::uint32_t> out;
    const auto add = [&](const flatbuffers::Vector<std::uint32_t>* list) {
        if (list == nullptr) {
            return;
        }
        for (const auto id : *list) {
            if (std::ranges::find(out, id) == out.end()) {
                out.push_back(id);
            }
        }
    };
    if (const auto* n = resolve_npc(world, npc, wfb::NpcTemplateFlags::use_ai_packages, actor)) {
        add(n->packages());
    }
    if (const auto* n = resolve_npc(world, npc, wfb::NpcTemplateFlags::use_package_list, actor)) {
        add(n->default_packages());
    }
    return out;
}

std::vector<std::uint32_t> alias_packages(const wfb::World& world,
                                          std::vector<std::pair<std::uint32_t, std::uint32_t>> filled) {
    std::vector<std::uint32_t> out;
    if (filled.empty() || world.quests() == nullptr) {
        return out;
    }
    std::vector<const wfb::Quest*> quests;
    for (const auto& [quest, alias] : filled) {
        const auto* q = world.quests()->LookupByKey(quest);
        if (q != nullptr && std::ranges::find(quests, q) == quests.end()) {
            quests.push_back(q);
        }
    }
    std::ranges::sort(quests, [](const wfb::Quest* a, const wfb::Quest* b) {
        return a->priority() != b->priority() ? a->priority() > b->priority() : a->id() < b->id();
    });
    for (const auto* q : quests) {
        if (q->aliases() == nullptr) {
            continue;
        }
        for (const auto* a : *q->aliases()) {
            const bool fills = std::ranges::find(filled, std::pair{q->id(), a->id()}) != filled.end();
            if (!fills || a->location() || a->packages() == nullptr) {
                continue;
            }
            for (const auto id : *a->packages()) {
                if (id != 0 && std::ranges::find(out, id) == out.end()) {
                    out.push_back(id);
                }
            }
        }
    }
    return out;
}

double done_minutes(const wfb::Package& p) {
    return p.hour() >= 0 && p.duration() != 0 ? static_cast<double>(p.duration()) : 60.0;
}

std::uint32_t choose_package(const wfb::World& world, std::uint32_t npc, std::uint32_t actor,
                             const Clock& clock, const ConditionSource& host,
                             const std::function<bool(std::uint32_t)>& skip) {
    return choose_package(world, package_list(world, npc, actor), actor, clock, host, skip);
}

std::uint32_t choose_package(const wfb::World& world, const std::vector<std::uint32_t>& list,
                             std::uint32_t actor, const Clock& clock, const ConditionSource& host,
                             const std::function<bool(std::uint32_t)>& skip) {
    for (const auto id : list) {
        if (skip && skip(id)) {
            continue;
        }
        const auto* own = find_package(world, id);
        if (own == nullptr || !schedule_active(*own, clock)) {
            continue;
        }
        if (own->conditions() == nullptr || own->conditions()->size() == 0) {
            return id;
        }
        const auto pkg = resolve_package(world, id);
        PackageDataOverrides data(pkg, host, actor);
        if (conditions_pass(pkg.own->conditions(), ConditionSource(*host.world, &data), actor)) {
            return id;
        }
    }
    return 0;
}

} // namespace skydot::ai
