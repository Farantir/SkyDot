// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/crafting.hpp"

#include "data/fb_search.hpp"
#include "data/world_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

void Crafting::log_skill(const char* skill, std::uint32_t form, std::int64_t value) {
    char id[16];
    std::snprintf(id, sizeof id, "0x%08X", form);
    host_.log(std::string("skill: ") + skill + " use (" + id + ", value " + std::to_string(value) +
              "): XP waits for P6");
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
                                                  item->category() == wfb::ItemCategory::armor);
        if (smithing) {
            log_skill("Smithing", recipe->product(), value_of(recipe->product()) * made);
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
            row.next_health = std::max(row.health, k_placeholder_temper_health);
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
    if (k_placeholder_temper_health <= now) {
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
    changed.health = k_placeholder_temper_health;
    (void)inventory_.remove_extra(player_, form, extra, 1);
    inventory_.add(player_, form, 1, extras_.intern(changed));
    log_skill("Smithing", form, value_of(form));
    return true;
}

std::vector<EnchantRow> Crafting::enchant_rows() const {
    std::vector<EnchantRow> out;
    for (const auto& [form, total] : inventory_.items(player_)) {
        const auto* item = item_of(world_, form);
        if (item == nullptr) {
            continue;
        }
        if (item->enchantment() != 0 && inventory_.count_extra(player_, form, 0) > 0) {
            out.push_back({form, 0, item->enchantment()});
        }
        for (const auto& [extra, n] : inventory_.extras_of(player_, form)) {
            const auto* e = extras_.find(extra);
            const std::uint32_t enchantment = e != nullptr && e->enchantment != 0 ? e->enchantment : item->enchantment();
            if (enchantment != 0) {
                out.push_back({form, extra, enchantment});
            }
        }
    }
    return out;
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
std::vector<Instance> instances_of(const wfb::World& world, std::uint32_t form, double skill) {
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
        if ((mgef->flags() & k_mgef_power_affects_magnitude) != 0) {
            inst.magnitude = static_cast<float>(static_cast<double>(inst.magnitude) * factor);
        }
        if ((mgef->flags() & k_mgef_power_affects_duration) != 0) {
            inst.duration = static_cast<std::uint32_t>(std::lround(static_cast<double>(inst.duration) * factor));
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
    std::map<std::uint32_t, std::pair<int, Instance>> shared; // effect -> (ingredients with it, strongest)
    for (const std::uint32_t form : forms) {
        for (const auto& inst : instances_of(world_, form, k_placeholder_skill)) {
            auto [it, fresh] = shared.try_emplace(inst.effect, 0, inst);
            ++it->second.first;
            if (!fresh && inst.cost > it->second.second.cost) {
                it->second.second = inst;
            }
        }
    }
    std::vector<Instance> effects;
    for (const auto& [effect, entry] : shared) {
        if (entry.first >= 2) {
            effects.push_back(entry.second);
        }
    }
    if (effects.empty()) {
        return out;
    }
    std::ranges::stable_sort(effects, [](const Instance& a, const Instance& b) { return a.cost > b.cost; });
    double value = 0.0;
    for (const auto& e : effects) {
        out.effects.push_back({e.effect, e.magnitude, e.duration});
        value += e.cost;
    }
    out.value = static_cast<std::int32_t>(std::lround(value));
    const auto* strongest = effect_of(world_, effects.front().effect);
    out.poison = strongest != nullptr && (strongest->flags() & k_mgef_hostile) != 0;
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
        for (const auto& inst : instances_of(world_, form, k_placeholder_skill)) {
            const bool shared = std::ranges::any_of(plan.effects, [&](const vm::CreatedEffect& e) {
                return e.effect == inst.effect;
            });
            if (shared) {
                created_.learn(form, inst.index);
            }
        }
    }
    log_skill("Alchemy", id, plan.value);
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
