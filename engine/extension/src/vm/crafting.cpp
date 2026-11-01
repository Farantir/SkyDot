// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/crafting.hpp"

#include "data/fb_search.hpp"
#include "data/world_data.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>

namespace skydot::crafting {

namespace {

constexpr double k_cost_exponent = 1.1;
/// ItemCategory ingredient in world.fbs.
constexpr wfb::ItemCategory k_ingredient = wfb::ItemCategory::ingredient;

const wfb::ItemData* item_of(const wfb::World& world, std::uint32_t form) {
    const auto* base = lookup(world.bases(), form);
    return base != nullptr ? base->item() : nullptr;
}

const wfb::Recipe* recipe_of(const wfb::World& world, std::uint32_t id) {
    return lookup(world.recipes(), id);
}

const wfb::MagicEffect* effect_of(const wfb::World& world, std::uint32_t id) {
    return lookup(world.magic_effects(), id);
}

/// One ingredient's effect as it takes part in a mix.
struct Instance {
    std::uint32_t effect = 0;
    float magnitude = 0.0F;
    std::uint32_t duration = 0;
    double cost = 0.0;
    std::size_t index = 0; ///< Position in the ingredient's effects.
};

double power(const wfb::World& world, double skill) {
    const double init = static_cast<double>(WorldData::gmst_float(
        &world, "fAlchemyIngredientInitMult", static_cast<float>(k_alchemy_init_mult)));
    const double factor = static_cast<double>(
        WorldData::gmst_float(&world, "fAlchemySkillFactor", static_cast<float>(k_alchemy_skill_factor)));
    return init * (1.0 + (factor - 1.0) * skill / 100.0);
}

double effect_cost(float base_cost, float magnitude, std::uint32_t duration) {
    const double mag = std::max(static_cast<double>(magnitude), 1.0);
    const double dur = std::max(static_cast<double>(duration) / 10.0, 1.0);
    return static_cast<double>(base_cost) * std::pow(mag, k_cost_exponent) * std::pow(dur, k_cost_exponent);
}

} // namespace

int temper_level(double effective_skill) {
    return std::clamp(static_cast<int>(std::floor((effective_skill + 38.0) * 3.0 / 103.0)), 0, k_temper_max_level);
}

float temper_health(int level) { return 1.0F + 0.1F * static_cast<float>(std::clamp(level, 0, k_temper_max_level)); }

const char* skill_use_kind_name(SkillUse::Kind kind) {
    switch (kind) {
    case SkillUse::Kind::create: return "create";
    case SkillUse::Kind::improve: return "improve";
    case SkillUse::Kind::brew: return "brew";
    case SkillUse::Kind::enchant: return "enchant";
    case SkillUse::Kind::disenchant: return "disenchant";
    case SkillUse::Kind::recharge: return "recharge";
    }
    return "";
}

std::string Crafting::effect_name(std::uint32_t effect) const {
    const auto* e = effect_of(world_, effect);
    return e != nullptr && e->name() != nullptr ? e->name()->str() : std::string();
}

std::string Crafting::describe(std::uint32_t effect, float magnitude, std::uint32_t duration) const {
    const auto* e = effect_of(world_, effect);
    std::string text = e != nullptr && e->description() != nullptr ? e->description()->str() : std::string();
    const auto replace = [&text](const std::string& what, const std::string& with) {
        for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size())) {
            text.replace(at, what.size(), with);
        }
    };
    replace("<mag>", std::to_string(std::lround(static_cast<double>(magnitude))));
    replace("<dur>", std::to_string(duration));
    return text;
}

bool Crafting::listed(const wfb::Recipe& recipe, const ConditionItem* item) const {
    return host_.conditions_pass(recipe.conditions(), item);
}

std::vector<Component> Crafting::components_of(const wfb::Recipe& recipe) const {
    std::vector<Component> out;
    if (recipe.components() != nullptr) {
        for (const auto* c : *recipe.components()) {
            out.push_back({c->form(), c->count(), inventory_.count(player_, c->form())});
        }
    }
    return out;
}

bool Crafting::all_held(const std::vector<Component>& components) {
    return std::ranges::all_of(components, [](const Component& c) { return c.have >= c.need; });
}

float Crafting::health_of(std::uint32_t extra) const {
    const auto* e = extras_.find(extra);
    return e != nullptr ? e->health : 1.0F;
}

bool Crafting::enchanted(std::uint32_t form, std::uint32_t extra) const {
    const auto* item = item_of(world_, form);
    const auto* e = extras_.find(extra);
    return (item != nullptr && item->enchantment() != 0) || (e != nullptr && e->enchantment != 0);
}

void Crafting::take_components(const std::vector<Component>& components) {
    for (const auto& c : components) {
        (void)inventory_.remove(player_, c.form, c.need);
    }
}

std::int64_t Crafting::value_of(std::uint32_t form) const {
    if (const auto* potion = created_.find(form)) {
        return potion->value;
    }
    const auto* item = item_of(world_, form);
    return item != nullptr ? item->value() : 0;
}

bool Crafting::is_weapon(std::uint32_t form) const {
    const auto* item = item_of(world_, form);
    return item != nullptr && item->category() == wfb::ItemCategory::weapon;
}

std::string Crafting::tier_name(float health, bool weapon) const {
    const int level = vm::temper_quality(health);
    if (level <= 0) {
        return {};
    }
    const std::string name = std::string(weapon ? "sHealthDataPrefixWeap" : "sHealthDataPrefixArmo") + std::to_string(level);
    return host_.gmst_text(name, vm::k_temper_tiers[level - 1].name);
}

float Crafting::tempered_health(std::uint32_t form, bool is_enchanted) const {
    perks::EntryContext ctx;
    ctx.owner = player_;
    ctx.subjects = {player_, form, 0};
    ctx.item_enchanted = is_enchanted;
    // (skill - 13.29) * (1 + perk) + 13.29: the perk entries multiply the part above 13.29.
    const double above = host_.perk_entry(perks::EntryPoint::ModTemperingHealth, ctx,
                                          host_.skill(skill_av::smithing) - k_temper_skill_offset);
    return temper_health(temper_level(above + k_temper_skill_offset));
}

// ---- create object -----------------------------------------------------------------

std::vector<RecipeRow> Crafting::recipes(std::uint32_t bench_ref) const {
    std::vector<RecipeRow> out;
    const auto* all = world_.recipes();
    if (host_.bench_type(bench_ref) != bench::create_object || all == nullptr) {
        return out;
    }
    for (const auto* recipe : *all) {
        if (recipe->bench_keyword() == 0 || !host_.bench_has_keyword(bench_ref, recipe->bench_keyword()) ||
            !listed(*recipe, nullptr)) {
            continue;
        }
        RecipeRow row;
        row.recipe = recipe->id();
        row.product = recipe->product();
        row.count = recipe->product_count();
        row.components = components_of(*recipe);
        row.craftable = all_held(row.components);
        out.push_back(std::move(row));
    }
    return out;
}

std::int64_t Crafting::craft(std::uint32_t recipe_id, std::int64_t count) {
    const auto* recipe = recipe_of(world_, recipe_id);
    if (recipe == nullptr || count <= 0 || !listed(*recipe, nullptr)) {
        return 0;
    }
    std::int64_t made = 0;
    for (; made < count; ++made) {
        const auto components = components_of(*recipe);
        if (!all_held(components)) {
            break;
        }
        take_components(components);
        inventory_.add(player_, recipe->product(), std::max<std::int32_t>(recipe->product_count(), 1));
    }
    if (made > 0) {
        const auto* item = item_of(world_, recipe->product());
        const bool smithing = item != nullptr && (item->category() == wfb::ItemCategory::weapon ||
                                                  item->category() == wfb::ItemCategory::armor ||
                                                  item->category() == wfb::ItemCategory::jewelry);
        if (smithing) {
            SkillUse use;
            use.kind = SkillUse::Kind::create;
            use.skill = skill_av::smithing;
            use.count = static_cast<double>(made * std::max<std::int32_t>(recipe->product_count(), 1));
            use.value = static_cast<double>(value_of(recipe->product()));
            host_.skill_use(use);
        }
    }
    return made;
}

// ---- tempering ---------------------------------------------------------------------

std::vector<TemperRow> Crafting::temper_rows(std::uint32_t bench_ref) const {
    std::vector<TemperRow> out;
    const int type = host_.bench_type(bench_ref);
    const auto* all = world_.recipes();
    if ((type != bench::smithing_weapon && type != bench::smithing_armor) || all == nullptr) {
        return out;
    }
    const std::uint32_t keyword =
        host_.keyword(type == bench::smithing_weapon ? k_weapon_tempering_keyword : k_armor_tempering_keyword);
    if (keyword == 0) {
        return out;
    }
    for (const auto* recipe : *all) {
        if (recipe->bench_keyword() != keyword) {
            continue;
        }
        const std::uint32_t form = recipe->product();
        const auto& split = inventory_.extras_of(player_, form);
        std::vector<std::uint32_t> stacks;
        if (inventory_.count_extra(player_, form, 0) > 0) {
            stacks.push_back(0);
        }
        for (const auto& [extra, n] : split) {
            stacks.push_back(extra);
        }
        for (const std::uint32_t extra : stacks) {
            const ConditionItem item{form, enchanted(form, extra)};
            if (!listed(*recipe, &item)) {
                continue;
            }
            TemperRow row;
            row.recipe = recipe->id();
            row.form = form;
            row.extra = extra;
            row.count = inventory_.count_extra(player_, form, extra);
            row.health = health_of(extra);
            row.next_health = std::max(row.health, tempered_health(form, item.enchanted));
            row.components = components_of(*recipe);
            row.craftable = all_held(row.components) && row.next_health > row.health;
            out.push_back(std::move(row));
        }
    }
    return out;
}

bool Crafting::temper(std::uint32_t recipe_id, std::uint32_t form, std::uint32_t extra) {
    const auto* recipe = recipe_of(world_, recipe_id);
    if (recipe == nullptr || recipe->product() != form || inventory_.count_extra(player_, form, extra) <= 0) {
        return false;
    }
    const ConditionItem item{form, enchanted(form, extra)};
    if (!listed(*recipe, &item)) {
        return false;
    }
    const float now = health_of(extra);
    const float next = tempered_health(form, item.enchanted);
    if (next <= now) {
        return false;
    }
    const auto components = components_of(*recipe);
    if (!all_held(components)) {
        return false;
    }
    take_components(components);
    vm::ItemExtra changed;
    if (const auto* old = extras_.find(extra)) {
        changed = *old;
    }
    changed.health = next;
    (void)inventory_.remove_extra(player_, form, extra, 1);
    inventory_.add(player_, form, 1, extras_.intern(changed));
    SkillUse use;
    use.kind = SkillUse::Kind::improve;
    use.skill = skill_av::smithing;
    use.delta_quality = static_cast<double>(vm::temper_quality(next) - vm::temper_quality(now));
    // ESTIMATE: the item's value follows its health (no pack data says how a tempered item is priced).
    use.delta_value = static_cast<double>(value_of(form)) * static_cast<double>(next - now);
    host_.skill_use(use);
    return true;
}

// ---- alchemy -----------------------------------------------------------------------

std::vector<AlchemyRow> Crafting::alchemy_rows() const {
    std::vector<AlchemyRow> out;
    for (const auto& [form, count] : inventory_.items(player_)) {
        const auto* item = item_of(world_, form);
        const auto* effects = item != nullptr ? item->effects() : nullptr;
        if (effects == nullptr || item->category() != k_ingredient) {
            continue;
        }
        AlchemyRow row;
        row.form = form;
        row.count = count;
        for (std::size_t i = 0; i < effects->size() && i < 4; ++i) {
            row.effects.push_back({effects->Get(static_cast<flatbuffers::uoffset_t>(i))->effect(),
                                   created_.known(form, i)});
        }
        out.push_back(std::move(row));
    }
    return out;
}

namespace {

/// The effects of an ingredient with their costs at `skill`.
using EffectMult = std::function<double(std::uint32_t effect)>;
std::vector<Instance> instances_of(const wfb::World& world, std::uint32_t form, double skill,
                                   const EffectMult& perk = nullptr) {
    std::vector<Instance> out;
    const auto* item = item_of(world, form);
    const auto* effects = item != nullptr ? item->effects() : nullptr;
    if (effects == nullptr || item->category() != k_ingredient) {
        return out;
    }
    const double factor = power(world, skill);
    for (std::size_t i = 0; i < effects->size() && i < 4; ++i) {
        const auto* e = effects->Get(static_cast<flatbuffers::uoffset_t>(i));
        const auto* mgef = effect_of(world, e->effect());
        if (mgef == nullptr) {
            continue;
        }
        Instance inst;
        inst.effect = e->effect();
        inst.index = i;
        inst.magnitude = e->magnitude();
        inst.duration = e->duration();
        const double strength = factor * (perk ? perk(e->effect()) : 1.0);
        if ((mgef->flags() & k_mgef_power_affects_magnitude) != 0) {
            inst.magnitude = static_cast<float>(static_cast<double>(inst.magnitude) * strength);
        }
        if ((mgef->flags() & k_mgef_power_affects_duration) != 0) {
            inst.duration = static_cast<std::uint32_t>(std::lround(static_cast<double>(inst.duration) * strength));
        }
        inst.cost = effect_cost(mgef->base_cost(), inst.magnitude, inst.duration);
        out.push_back(inst);
    }
    return out;
}

} // namespace

AlchemyPreview Crafting::preview(std::span<const std::uint32_t> forms) const {
    AlchemyPreview out;
    if (forms.size() < 2 || forms.size() > 3) {
        return out;
    }
    for (std::size_t i = 0; i < forms.size(); ++i) {
        for (std::size_t j = i + 1; j < forms.size(); ++j) {
            if (forms[i] == forms[j]) {
                return out;
            }
        }
    }
    const double skill = host_.skill(skill_av::alchemy);
    // The effects the mix shares, the strongest instance of each, most costly first.
    const auto shared_effects = [&](const EffectMult& perk) {
        std::map<std::uint32_t, std::pair<int, Instance>> shared; // effect -> (ingredients with it, strongest)
        for (const std::uint32_t form : forms) {
            for (const auto& inst : instances_of(world_, form, skill, perk)) {
                auto [it, fresh] = shared.try_emplace(inst.effect, 0, inst);
                ++it->second.first;
                if (!fresh && inst.cost > it->second.second.cost) {
                    it->second.second = inst;
                }
            }
        }
        std::vector<Instance> found;
        for (const auto& [effect, entry] : shared) {
            if (entry.first >= 2) {
                found.push_back(entry.second);
            }
        }
        std::ranges::stable_sort(found, [](const Instance& a, const Instance& b) { return a.cost > b.cost; });
        return found;
    };
    const auto hostile = [&](std::uint32_t effect) {
        const auto* m = effect_of(world_, effect);
        return m != nullptr && (m->flags() & k_mgef_hostile) != 0;
    };
    // Whether it is a poison is the strongest effect's nature, before the perks scale anything.
    auto effects = shared_effects(nullptr);
    if (effects.empty()) {
        return out;
    }
    const bool poison = hostile(effects.front().effect);
    perks::EntryContext ctx;
    ctx.owner = player_;
    ctx.subjects = {player_, 0, 0};
    ctx.making_poison = poison;
    // Mod Alchemy Effectiveness (66): Alchemist, Physician, Benefactor, Poisoner multiply an effect's strength.
    effects = shared_effects([&](std::uint32_t effect) {
        perks::EntryContext c = ctx;
        c.alchemy_effect = effect;
        return host_.perk_entry(perks::EntryPoint::ModAlchemyEffectiveness, c, 1.0);
    });
    // Purify Alchemy Ingredients (73): Purity keeps only the effects that suit (a potion's good ones, a
    // poison's bad ones), unless that would leave nothing.
    if (host_.perk_entry(perks::EntryPoint::PurifyAlchemyIngredients, ctx, 0.0) > 0.5) {
        std::vector<Instance> kept;
        std::ranges::copy_if(effects, std::back_inserter(kept),
                             [&](const Instance& e) { return hostile(e.effect) == poison; });
        if (!kept.empty()) {
            effects = std::move(kept);
        }
    }
    double value = 0.0;
    for (const auto& e : effects) {
        out.effects.push_back({e.effect, e.magnitude, e.duration});
        value += e.cost;
    }
    out.value = static_cast<std::int32_t>(std::lround(value));
    out.poison = poison;
    out.name = std::string(out.poison ? "Poison of " : "Potion of ") + effect_name(effects.front().effect);
    out.mixable = true;
    return out;
}

std::uint32_t Crafting::mix(std::span<const std::uint32_t> forms) {
    const auto plan = preview(forms);
    if (!plan.mixable) {
        return 0;
    }
    for (const std::uint32_t form : forms) {
        if (inventory_.count(player_, form) < 1) {
            return 0;
        }
    }
    vm::CreatedPotion potion;
    potion.name = plan.name;
    potion.effects = plan.effects;
    potion.value = plan.value;
    potion.weight = k_potion_weight;
    potion.poison = plan.poison;
    potion.model = host_.potion_model();
    const std::uint32_t id = created_.create(std::move(potion));
    if (id == 0) {
        return 0;
    }
    for (const std::uint32_t form : forms) {
        (void)inventory_.remove(player_, form, 1);
    }
    inventory_.add(player_, id, 1);
    // Every shared effect becomes known on the ingredients that have it.
    for (const std::uint32_t form : forms) {
        for (const auto& inst : instances_of(world_, form, host_.skill(skill_av::alchemy))) {
            const bool shared = std::ranges::any_of(plan.effects, [&](const vm::CreatedEffect& e) {
                return e.effect == inst.effect;
            });
            if (shared) {
                created_.learn(form, inst.index);
            }
        }
    }
    SkillUse use;
    use.kind = SkillUse::Kind::brew;
    use.skill = skill_av::alchemy;
    use.value = static_cast<double>(plan.value);
    host_.skill_use(use);
    return id;
}

void Crafting::note_eaten(std::uint32_t ingredient) {
    const auto* item = item_of(world_, ingredient);
    const auto* effects = item != nullptr ? item->effects() : nullptr;
    if (effects != nullptr && item->category() == k_ingredient && effects->size() > 0) {
        created_.learn(ingredient, 0);
    }
}

} // namespace skydot::crafting
