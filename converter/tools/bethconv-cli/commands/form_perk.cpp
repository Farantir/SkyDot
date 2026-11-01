// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/form_perk.hpp"

#include "bethconv/record/conditions.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string_view>

namespace bethconv::cli {
namespace {

namespace wfb = bethconv::pack::wfb;

const char* comparison_name(std::uint8_t type) {
    static constexpr const char* k_names[] = {"==", "!=", ">", ">=", "<", "<=", "?", "?"};
    return k_names[(type >> 5) & 7];
}

void print_condition(const wfb::ConditionT& c, const char* indent) {
    const auto* fn = record::condition_function(c.function);
    std::string name = fn != nullptr ? std::string(fn->name) : "fn" + std::to_string(c.function);
    char value[64];
    if ((c.type & 0x4) != 0) {
        std::snprintf(value, sizeof value, "global 0x%X", c.value_global);
    } else {
        std::snprintf(value, sizeof value, "%g", static_cast<double>(c.value));
    }
    std::printf("%s%s(0x%X, 0x%X) %s %s%s run_on %u%s\n", indent, name.c_str(), c.param1,
                c.param2, comparison_name(c.type), value, (c.type & 0x1) != 0 ? " OR" : "",
                c.run_on, (c.type & 0x10) != 0 ? " swapped" : "");
}

const char* kind_name(wfb::PerkEffectKind kind) {
    switch (kind) {
    case wfb::PerkEffectKind::quest: return "quest";
    case wfb::PerkEffectKind::ability: return "ability";
    default: return "entry point";
    }
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

void print_perk(const pack::WorldFile& world, std::uint32_t id) {
    const auto perk = world.perk(id);
    if (!perk) {
        std::printf("perk 0x%X: none\n", id);
        return;
    }
    std::printf("perk 0x%X %s '%s': level %u ranks %u trait %d playable %d hidden %d next 0x%X, "
                "%zu conditions, %zu effects\n  %s\n",
                id, perk->editor_id.c_str(), perk->name.c_str(), perk->level, perk->ranks,
                perk->trait, perk->playable, perk->hidden, perk->next_perk,
                perk->conditions.size(), perk->effects.size(), perk->description.c_str());
    for (const auto& c : perk->conditions) {
        print_condition(*c, "  prerequisite ");
    }
    int n = 0;
    for (const auto& e : perk->effects) {
        std::printf("  effect %d: %s rank %u priority %u", n++, kind_name(e->kind), e->rank,
                    e->priority);
        if (e->kind == wfb::PerkEffectKind::quest) {
            std::printf(" quest 0x%X stage %u\n", e->quest, e->stage);
        } else if (e->kind == wfb::PerkEffectKind::ability) {
            std::printf(" spell 0x%X\n", e->spell);
        } else {
            std::printf(" entry point %u function %u tabs %u data type %u", e->entry_point,
                        e->function, e->tab_count, e->data_type);
            if (e->actor_value >= 0) {
                std::printf(" actor value %d", e->actor_value);
            }
            for (const float v : e->values) {
                std::printf(" %g", static_cast<double>(v));
            }
            if (e->form != 0) {
                std::printf(" form 0x%X", e->form);
            }
            if (!e->text.empty()) {
                std::printf(" text '%s'", e->text.c_str());
            }
            if (e->script_flags != 0) {
                std::printf(" script flags 0x%X", e->script_flags);
            }
            std::printf("\n");
            for (const auto& t : e->tabs) {
                std::printf("    tab %u: %zu conditions\n", t->tab, t->conditions.size());
                for (const auto& c : t->conditions) {
                    print_condition(*c, "      ");
                }
            }
        }
    }
}

void print_avif(const pack::WorldFile& world, const std::string& what) {
    std::optional<wfb::ActorValueInfoT> info;
    if (!what.empty() && std::isdigit(static_cast<unsigned char>(what[0])) != 0) {
        info = world.actor_value_info(static_cast<std::uint32_t>(std::strtoul(what.c_str(), nullptr, 0)));
    } else {
        const std::string want = lower(what.rfind("AV", 0) == 0 ? what.substr(2) : what);
        for (auto& candidate : world.actor_value_infos()) {
            if (lower(candidate.editor_id) == "av" + want) {
                info = std::move(candidate);
                break;
            }
        }
    }
    if (!info) {
        std::printf("avif %s: none\n", what.c_str());
        return;
    }
    std::printf("avif 0x%X %s '%s' [%s]: actor value %d category %u skill %d use %g + %g, improve "
                "%g, offset %g, %zu tree nodes\n",
                info->id, info->editor_id.c_str(), info->name.c_str(),
                info->abbreviation.c_str(), info->actor_value, info->category, info->has_skill,
                static_cast<double>(info->use_mult), static_cast<double>(info->use_offset),
                static_cast<double>(info->improve_mult), static_cast<double>(info->improve_offset),
                info->perk_tree.size());
    for (const auto& n : info->perk_tree) {
        const auto perk = world.perk(n->perk);
        std::printf("  node %u perk 0x%X %s flags 0x%X grid (%d, %d) pos (%g, %g) skill 0x%X ->",
                    n->index, n->perk, perk ? perk->editor_id.c_str() : "-", n->flags, n->grid_x,
                    n->grid_y, static_cast<double>(n->x), static_cast<double>(n->y), n->skill);
        for (const auto link : n->links) {
            std::printf(" %u", link);
        }
        std::printf("\n");
    }
}

void print_class(const pack::WorldFile& world, std::uint32_t id) {
    const auto c = world.actor_class(id);
    if (!c) {
        std::printf("class 0x%X: none\n", id);
        return;
    }
    std::printf("class 0x%X %s '%s': teaches %d up to %u, health/magicka/stamina weights "
                "%u/%u/%u, bleedout %g, voice points %u, skill weights",
                id, c->editor_id.c_str(), c->name.c_str(), c->teaches, c->max_training_level,
                c->health_weight, c->magicka_weight, c->stamina_weight,
                static_cast<double>(c->bleedout_default), c->voice_points);
    for (const auto w : c->skill_weights) {
        std::printf(" %u", w);
    }
    std::printf("\n");
}

void print_perk_census(const pack::WorldFile& world) {
    const auto perks = world.perks();
    std::size_t kinds[3]{};
    // (entry point, function) -> the tab counts seen.
    std::map<std::pair<unsigned, unsigned>, std::set<unsigned>> entries;
    std::map<unsigned, std::size_t> data_types;
    std::size_t with_tree = 0;
    for (const auto& p : perks) {
        for (const auto& e : p.effects) {
            ++kinds[static_cast<unsigned>(e->kind) % 3];
            if (e->kind == wfb::PerkEffectKind::entry_point) {
                entries[{e->entry_point, e->function}].insert(e->tab_count);
                ++data_types[e->data_type];
            }
        }
    }
    for (const auto& a : world.actor_value_infos()) {
        if (!a.perk_tree.empty()) {
            ++with_tree;
        }
    }
    std::printf("%zu PERK (%zu AVIF with a tree); effects: %zu quest, %zu ability, %zu entry point\n",
                perks.size(), with_tree, kinds[0], kinds[1], kinds[2]);
    std::map<unsigned, std::set<unsigned>> by_entry;
    for (const auto& [key, tabs] : entries) {
        by_entry[key.first].insert(tabs.begin(), tabs.end());
    }
    std::printf("entry points (id: tab counts), %zu ids:", by_entry.size());
    for (const auto& [ep, tabs] : by_entry) {
        std::printf(" %u:", ep);
        const char* sep = "";
        for (const auto t : tabs) {
            std::printf("%s%u", sep, t);
            sep = "/";
        }
    }
    std::printf("\nentry point effects by data type:");
    for (const auto& [t, n] : data_types) {
        std::printf(" %u=%zu", t, n);
    }
    std::printf("\n");
}

} // namespace bethconv::cli
