// SPDX-License-Identifier: GPL-3.0-or-later
#include "perks/entry_points.hpp"

#include "conditions/functions.hpp"
#include "data/fb_search.hpp"
#include "magic/magic_rules.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace skydot::perks {
namespace {

namespace fn = conditions::fn;

constexpr std::array<const char*, k_entry_point_count> k_names = {
    "Calculate Weapon Damage", "Calculate My Critical Hit Chance", "Calculate My Critical Hit Damage",
    "Mine Explode Chance", "Adjust Limb Damage", "Adjust Book Skill Points", "Mod Recovered Health",
    "Get Should Attack", "Mod Buy Prices", "Add Leveled List On Death", "Get Max Carry Weight",
    "Addiction Chance", "Addiction Duration", "Positive Chem Duration", "Activate",
    "Ignore Running During Detection", "Ignore Broken Lock", "Mod Enemy Critical Hit Chance",
    "Mod Sneak Attack Mult", "Max Placeable Mines", "Bow Zoom", "Recover Arrow Chance", "Mod Skill Use",
    "TK Distance", "TK Damage Mult", "TK Damage", "Mod Bashing Damage", "Mod Power Attack Stamina",
    "Mod Power Attack Damage", "Mod Spell Magnitude", "Mod Spell Duration", "Secondary Value Weight",
    "Mod Armor Weight", "Mod Incoming Stagger", "Mod Target Stagger", "Mod Attack Damage",
    "Mod Incoming Damage", "Mod Target Damage Resistance", "Mod Spell Cost", "Mod Percent Blocked",
    "Shield Deflect Arrow", "Mod Incoming Spell Magnitude", "Mod Incoming Spell Duration", "Intimidation",
    "Reputation", "Favor Points", "Bribe Amount", "Mod Detection Light", "Mod Detection Movement",
    "Mod Soul Gem Recharge", "Set Sweep Attack", "Apply Combat Hit Spell", "Apply Bashing Spell",
    "Apply Reanimate Spell", "Set Boolean Graph Variable", "Spell Casting Sound Event",
    "Mod Pickpocket Chance", "Mod Detection Sneak Skill", "Falling Damage", "Mod Lockpick Sweet Spot",
    "Mod Sell Prices", "Can Pickpocket Equipped Item", "Lockpick Level Allowed", "Lockpick Starting Arc",
    "Set Progression Picking", "Lockpicks Unbreakable", "Mod Alchemy Effectiveness",
    "Apply Weapon Swing Spell", "Commanded Actor Limit", "Apply Sneaking Spell", "Player Magic Slowdown",
    "Ward Magicka Absorption", "Initial Ingredient Effects Learned", "Purify Alchemy Ingredients",
    "Filter Activation", "Can Dual Cast Spell", "Mod Tempering Health", "Mod Enchantment Power",
    "Soul Pct Captured to Weapon", "Mod Soul Gem Enchanting", "Mod # Applied Enchantments Allowed",
    "Set Activate Label", "Mod Shout OK", "Mod Poison Dose Count", "Should Apply Placed Item",
    "Mod Armor Rating", "Lockpicking Crime Chance", "Ingredients Harvested", "Spell Range (Target Loc.)",
    "Potions Created", "Lockpicking Key Reward Chance", "Allow Mount Actor",
};

/// Condition function ids of the EP* family and the others that need the entry point's context.
constexpr std::uint16_t k_fn_ep_alchemy_making_poison = 500;
constexpr std::uint16_t k_fn_ep_alchemy_effect_has_keyword = 501;
constexpr std::uint16_t k_fn_ep_tempering_item_has_keyword = 660;
constexpr std::uint16_t k_fn_ep_magic_is_advance_skill = 681;
constexpr std::uint16_t k_fn_worn_has_keyword = 682;
constexpr std::uint16_t k_fn_advance_object_has_keyword = 691;
constexpr std::uint16_t k_fn_is_advance_action = 692;
constexpr std::uint16_t k_fn_ep_magic_spell_has_keyword = 693;
constexpr std::uint16_t k_fn_ep_magic_spell_has_skill = 696;
constexpr std::uint16_t k_fn_is_attack_type = 697;
constexpr std::uint16_t k_fn_spell_has_casting_perk = 713;
constexpr std::uint16_t k_fn_worn_apparel_keyword_count = 722;

bool has(const flatbuffers::Vector<std::uint32_t>* list, std::uint32_t value) {
    return list != nullptr && std::ranges::find(*list, value) != list->end();
}

/// Answers the functions whose subject is a base form (a spell, a magic effect) or that read the
/// entry point's moment from the context.
class EntryOverrides final : public conditions::ConditionOverrides {
public:
    EntryOverrides(const wfb::World& world, conditions::ConditionWorld& cw, const EntryContext& ctx)
        : world_(world), cw_(cw), ctx_(ctx) {}

    std::optional<double> function_value(const wfb::Condition& c, std::uint32_t subject,
                                         const conditions::ConditionContext&) override {
        const std::uint32_t p1 = c.param1();
        switch (c.function()) {
        case fn::has_keyword:
            // A spell or a magic effect has no entry in the bases; its effects' keywords count.
            if (const auto* s = lookup(world_.spells(), subject)) {
                return flag(spell_has_keyword(*s, p1));
            }
            if (const auto* m = lookup(world_.magic_effects(), subject)) {
                return flag(has(m->keywords(), p1));
            }
            return std::nullopt;
        case k_fn_ep_magic_spell_has_keyword:
            if (const auto* s = lookup(world_.spells(), subject)) {
                return flag(spell_has_keyword(*s, p1));
            }
            return 0.0;
        case k_fn_ep_magic_spell_has_skill:
            if (const auto* s = lookup(world_.spells(), subject)) {
                return flag(magic::spell_school(world_, *s) == static_cast<int>(p1));
            }
            return 0.0;
        case k_fn_spell_has_casting_perk:
            if (const auto* s = lookup(world_.spells(), subject)) {
                return flag(s->half_cost_perk() != 0 && s->half_cost_perk() == p1);
            }
            return 0.0;
        case k_fn_ep_alchemy_effect_has_keyword: {
            const auto* m = lookup(world_.magic_effects(), subject);
            return flag(m != nullptr && has(m->keywords(), p1));
        }
        case k_fn_ep_alchemy_making_poison:
            return flag(ctx_.making_poison);
        case conditions::fn::ep_tempering_item_is_enchanted:
            return flag(ctx_.item_enchanted);
        case k_fn_ep_tempering_item_has_keyword:
            return flag(cw_.has_keyword(subject, p1));
        case k_fn_ep_magic_is_advance_skill:
            return flag(ctx_.advance_skill >= 0 && static_cast<std::uint32_t>(ctx_.advance_skill) == p1);
        case k_fn_is_advance_action:
            return flag(ctx_.advance_action >= 0 && static_cast<std::uint32_t>(ctx_.advance_action) == p1);
        case k_fn_advance_object_has_keyword:
            return flag(ctx_.advance_object != 0 && cw_.has_keyword(ctx_.advance_object, p1));
        case k_fn_worn_has_keyword:
            return flag(worn_count(p1) > 0);
        case k_fn_worn_apparel_keyword_count:
            return static_cast<double>(worn_count(p1));
        case k_fn_is_attack_type:
            return flag(std::ranges::find(ctx_.attack_keywords, p1) != ctx_.attack_keywords.end());
        default:
            return std::nullopt;
        }
    }

private:
    static double flag(bool v) { return v ? 1.0 : 0.0; }

    bool spell_has_keyword(const wfb::Spell& s, std::uint32_t keyword) const {
        if (s.effects() == nullptr) {
            return false;
        }
        for (const auto* e : *s.effects()) {
            const auto* m = lookup(world_.magic_effects(), e->effect());
            if (m != nullptr && has(m->keywords(), keyword)) {
                return true;
            }
        }
        return false;
    }
    int worn_count(std::uint32_t keyword) const {
        return static_cast<int>(std::ranges::count_if(
            ctx_.worn, [&](std::uint32_t item) { return cw_.has_keyword(item, keyword); }));
    }
    const wfb::World& world_;
    conditions::ConditionWorld& cw_;
    const EntryContext& ctx_;
};

} // namespace

const char* entry_point_name(EntryPoint ep) {
    const auto i = static_cast<std::size_t>(ep);
    return i < k_names.size() ? k_names[i] : "";
}

std::vector<Match> EntryPoints::matching(EntryPoint ep, const EntryContext& ctx) const {
    std::vector<Match> out;
    if (world_.perks() == nullptr) {
        return out;
    }
    EntryOverrides overrides(world_, cw_, ctx);
    for (const auto& [perk_id, rank] : perks_of_(ctx.owner)) {
        const auto* perk = lookup(world_.perks(), perk_id);
        if (perk == nullptr || perk->effects() == nullptr) {
            continue;
        }
        std::uint32_t order = 0;
        for (const auto* e : *perk->effects()) {
            ++order;
            if (e->kind() != wfb::PerkEffectKind::entry_point || e->entry_point() != static_cast<std::uint8_t>(ep) ||
                static_cast<unsigned>(e->rank()) + 1U != rank) {
                continue;
            }
            bool pass = true;
            if (e->tabs() != nullptr) {
                for (const auto* tab : *e->tabs()) {
                    std::uint32_t subject = tab->tab() < ctx.subjects.size() ? ctx.subjects[tab->tab()] : 0;
                    if (tab->tab() == 0 && subject == 0) {
                        subject = ctx.owner;
                    }
                    const conditions::ConditionContext cc{.subject = subject, .target = 0, .quest = 0};
                    if (!conditions::conditions_pass(tab->conditions(), cw_, cc, &overrides)) {
                        pass = false;
                        break;
                    }
                }
            }
            if (pass) {
                out.push_back({perk_id, rank, e->priority(), order, e});
            }
        }
    }
    std::ranges::sort(out, [](const Match& a, const Match& b) {
        return std::tuple(a.priority, a.perk, a.order) < std::tuple(b.priority, b.perk, b.order);
    });
    return out;
}

double EntryPoints::combine(Function f, double value, const std::vector<float>& data, double av) {
    const double x = data.empty() ? 0.0 : static_cast<double>(data[0]);
    switch (f) {
    case Function::SetValue: return x;
    case Function::AddValue: return value + x;
    case Function::MultiplyValue: return value * x;
    // ESTIMATE: the game rolls a number between the two; the mean is deterministic.
    case Function::AddRangeToValue: return value + (data.size() >= 2 ? (static_cast<double>(data[0]) + static_cast<double>(data[1])) / 2.0 : x);
    case Function::AddActorValueMult: return value + av * x;
    case Function::AbsoluteValue: return std::fabs(value);
    case Function::NegativeAbsoluteValue: return -std::fabs(value);
    case Function::SetToActorValueMult: return av * x;
    case Function::MultiplyActorValueMult: return value * av * x;
    case Function::MultiplyOnePlusActorValueMult: return value * (1.0 + av * x);
    default: return value;
    }
}

double EntryPoints::apply(EntryPoint ep, const EntryContext& ctx, double value) const {
    for (const auto& m : matching(ep, ctx)) {
        const auto f = static_cast<Function>(m.effect->function());
        double av = 0.0;
        if (f == Function::AddActorValueMult || f == Function::SetToActorValueMult ||
            f == Function::MultiplyActorValueMult || f == Function::MultiplyOnePlusActorValueMult) {
            av = m.effect->actor_value() >= 0
                     ? cw_.actor_value(ctx.owner, static_cast<std::uint32_t>(m.effect->actor_value()))
                     : 0.0;
        }
        std::vector<float> data;
        if (m.effect->values() != nullptr) {
            data.assign(m.effect->values()->begin(), m.effect->values()->end());
        }
        value = combine(f, value, data, av);
    }
    return value;
}

std::vector<std::uint32_t> EntryPoints::select_spells(EntryPoint ep, const EntryContext& ctx) const {
    std::vector<std::uint32_t> out;
    for (const auto& m : matching(ep, ctx)) {
        if (m.effect->function() == static_cast<std::uint8_t>(Function::SelectSpell) && m.effect->form() != 0) {
            out.push_back(m.effect->form());
        }
    }
    return out;
}

std::vector<std::string> EntryPoints::select_text(EntryPoint ep, const EntryContext& ctx) const {
    std::vector<std::string> out;
    for (const auto& m : matching(ep, ctx)) {
        const auto f = m.effect->function();
        if ((f == static_cast<std::uint8_t>(Function::SelectText) || f == static_cast<std::uint8_t>(Function::SetText)) &&
            m.effect->text() != nullptr) {
            out.push_back(m.effect->text()->str());
        }
    }
    return out;
}

namespace {

void each_effect(const wfb::World& world, std::uint32_t perk, wfb::PerkEffectKind kind,
                 const std::function<void(const wfb::PerkEffect&)>& fn) {
    const auto* p = lookup(world.perks(), perk);
    if (p == nullptr || p->effects() == nullptr) {
        return;
    }
    for (const auto* e : *p->effects()) {
        if (e->kind() == kind) {
            fn(*e);
        }
    }
}

} // namespace

void perk_added(const wfb::World& world, std::uint32_t actor, std::uint32_t perk, EffectSink& sink) {
    each_effect(world, perk, wfb::PerkEffectKind::ability, [&](const wfb::PerkEffect& e) {
        if (e.spell() != 0) {
            sink.add_ability(actor, e.spell());
        }
    });
    if (actor != 0x14) {
        return; // a quest stage is the player's perk's effect
    }
    each_effect(world, perk, wfb::PerkEffectKind::quest, [&](const wfb::PerkEffect& e) {
        if (e.quest() != 0) {
            sink.set_stage(e.quest(), e.stage());
        }
    });
}

void perk_removed(const wfb::World& world, std::uint32_t actor, std::uint32_t perk, EffectSink& sink) {
    each_effect(world, perk, wfb::PerkEffectKind::ability, [&](const wfb::PerkEffect& e) {
        if (e.spell() != 0) {
            sink.remove_ability(actor, e.spell());
        }
    });
}

} // namespace skydot::perks
