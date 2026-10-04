// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/packages.hpp"

#include "world/actors.hpp"
#include "world/fb_search.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>

namespace skydot::ai {
namespace {

/// The game starts on the 17th of Last Seed (month 7, 0-based).
constexpr int k_start_month = 7;
constexpr int k_start_date = 17;
/// The day of the week the game starts on, 0 Sundas … 6 Loredas. Morndas,
/// as the opening's date reads; not yet compared with GetDayOfWeek in game.
constexpr int k_start_day_of_week = 1;
constexpr std::array<int, 12> k_month_days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr int k_minutes_per_day = 24 * 60;
/// Template chains are short in vanilla (a package, its template); this only
/// stops loops.
constexpr int k_max_chain = 8;
constexpr std::int32_t k_no_key = 255;

/// CTDA type byte (see world.fbs, Condition).
constexpr std::uint8_t k_or = 0x01;
constexpr std::uint8_t k_use_global = 0x04;
constexpr std::uint32_t k_run_on_subject = 0;
constexpr std::uint32_t k_run_on_target = 1;
constexpr std::uint32_t k_run_on_reference = 2;
constexpr std::uint32_t k_run_on_linked = 4;

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

bool evaluate(const wfb::Condition& c, ConditionHost& host, std::uint32_t actor) {
    std::uint32_t subject = 0;
    switch (c.run_on()) {
    case k_run_on_subject:
    case k_run_on_target: // A package has no target of its own: the actor.
        subject = actor;
        break;
    case k_run_on_reference:
        subject = c.reference();
        break;
    case k_run_on_linked:
        subject = host.linked_ref(actor, 0);
        break;
    default: // Combat target, quest alias, package data, event data.
        break;
    }
    const double value = host.function_value(c, subject);
    const double with = (c.type() & k_use_global) != 0 ? host.global_value(c.value_global())
                                                         : static_cast<double>(c.value());
    return compare(value, c.type(), with);
}

/// A host that answers GetNumericPackageData from a package's inputs.
class PackageDataHost final : public ConditionHost {
public:
    PackageDataHost(const Package& pkg, ConditionHost& inner) : pkg_(pkg), inner_(inner) {}
    double function_value(const wfb::Condition& c, std::uint32_t subject) override {
        if (c.function() == k_get_numeric_package_data) {
            return pkg_.number(static_cast<std::int32_t>(c.param1()));
        }
        return inner_.function_value(c, subject);
    }
    double global_value(std::uint32_t global) override { return inner_.global_value(global); }
    std::uint32_t linked_ref(std::uint32_t actor, std::uint32_t keyword) override {
        return inner_.linked_ref(actor, keyword);
    }

private:
    const Package& pkg_;
    ConditionHost& inner_;
};

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
    Planner(const Package& pkg, ConditionHost& host, std::uint32_t actor, const Dice& dice)
        : pkg_(pkg), host_(pkg, host), actor_(actor), dice_(dice) {}

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
                if (key == k_no_key) {
                    continue;
                }
                const auto* in = pkg_.input(key);
                if (in == nullptr) {
                    continue;
                }
                const std::string_view type = sv(in->type());
                if (type == "Location" && s.location == nullptr) {
                    s.location = in;
                } else if ((type == "SingleRef" || type == "TargetSelector") && s.target == nullptr) {
                    s.target = in;
                } else if (type == "ObjectList" && s.list_key < 0) {
                    s.list_key = key;
                } else if (type == "Float" && s.procedure == Procedure::wait && s.seconds == 0.0) {
                    s.seconds = static_cast<double>(in->number());
                }
            }
        }
        return s;
    }

    const Package& pkg_;
    PackageDataHost host_;
    std::uint32_t actor_;
    const Dice& dice_;
    std::vector<Node> nodes_;
};

/// Days since the start to (month, date).
std::pair<int, int> calendar(double days) {
    int month = k_start_month;
    int date = k_start_date;
    auto left = static_cast<long long>(std::floor(std::max(0.0, days)));
    while (left > 0) {
        const int rest = k_month_days[static_cast<std::size_t>(month)] - date;
        if (left <= rest) {
            date += static_cast<int>(left);
            break;
        }
        left -= rest + 1;
        month = (month + 1) % 12;
        date = 1;
    }
    return {month, date};
}

} // namespace

double Clock::hour() const {
    const double frac = days - std::floor(days);
    return frac * 24.0;
}

int Clock::day_of_week() const {
    const auto d = static_cast<long long>(std::floor(days)) + k_start_day_of_week;
    return static_cast<int>(((d % 7) + 7) % 7);
}

int Clock::month() const {
    return calendar(days).first;
}

int Clock::date() const {
    return calendar(days).second;
}

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
                     ConditionHost& host, std::uint32_t actor) {
    if (list == nullptr) {
        return true;
    }
    bool all = true;
    bool any = false;
    bool open = false;
    for (const auto* c : *list) {
        any = evaluate(*c, host, actor) || any;
        open = true;
        if ((c->type() & k_or) == 0) {
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
    return find_sorted(world.packages(), id, [](const wfb::Package* p) { return p->id(); });
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
    case Procedure::other: return "other";
    }
    return "other";
}

std::vector<Step> plan_steps(const Package& pkg, ConditionHost& host, std::uint32_t actor,
                             const Dice& dice) {
    return Planner(pkg, host, actor, dice).run();
}

bool repeats(const Package& pkg) {
    const auto* branches = pkg.tree != nullptr ? pkg.tree->branches() : nullptr;
    return branches != nullptr && branches->size() != 0 && (branches->Get(0)->flags() & 0x1u) != 0;
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
    if (const auto* n = resolve_npc(world, npc, k_template_ai_packages, actor)) {
        add(n->packages());
    }
    if (const auto* n = resolve_npc(world, npc, k_template_package_list, actor)) {
        add(n->default_packages());
    }
    return out;
}

double done_minutes(const wfb::Package& p) {
    return p.hour() >= 0 && p.duration() != 0 ? static_cast<double>(p.duration()) : 60.0;
}

std::uint32_t choose_package(const wfb::World& world, std::uint32_t npc, std::uint32_t actor,
                             const Clock& clock, ConditionHost& host,
                             const std::function<bool(std::uint32_t)>& skip) {
    return choose_package(world, package_list(world, npc, actor), actor, clock, host, skip);
}

std::uint32_t choose_package(const wfb::World& world, const std::vector<std::uint32_t>& list,
                             std::uint32_t actor, const Clock& clock, ConditionHost& host,
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
        PackageDataHost data(pkg, host);
        if (conditions_pass(pkg.own->conditions(), data, actor)) {
            return id;
        }
    }
    return 0;
}

} // namespace skydot::ai
