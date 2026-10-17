// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's crafting and harvesting: the glue between the rules of
// vm/crafting.hpp and vm/harvest.hpp, the VM's inventory, the pack's data and
// Godot. Activating a bench or a plant is vm/papyrus_activation.cpp.
#include "vm/papyrus.hpp"

#include "conditions/functions.hpp"
#include "data/fb_search.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <span>

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace skydot {

namespace {

namespace wfb = bethconv::pack::wfb;

String to_godot(const std::string& s) {
    return String::utf8(s.c_str(), static_cast<int>(s.size()));
}

/// EPTemperingItemIsEnchanted from the item being tempered.
class TemperOverrides final : public conditions::ConditionOverrides {
public:
    explicit TemperOverrides(const crafting::ConditionItem* item) : item_(item) {}
    std::optional<double> function_value(const wfb::Condition& c, std::uint32_t, const conditions::ConditionContext&) override {
        if (c.function() == conditions::fn::ep_tempering_item_is_enchanted && item_ != nullptr) {
            return item_->enchanted ? 1.0 : 0.0;
        }
        return std::nullopt;
    }

private:
    const crafting::ConditionItem* item_;
};

} // namespace

struct SkydotPapyrus::CraftingAdapter final : crafting::CraftingHost {
    explicit CraftingAdapter(SkydotPapyrus& owner) : owner_(owner) {}

    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list,
                         const crafting::ConditionItem* item) override {
        PapyrusConditionWorld world(&owner_);
        conditions::ConditionContext ctx;
        ctx.subject = k_player_ref;
        ctx.unknown = conditions::UnknownPolicy::fails;
        TemperOverrides overrides(item);
        return conditions::conditions_pass(list, world, ctx, &overrides);
    }
    int bench_type(std::uint32_t bench_ref) const override {
        const auto* root = owner_.world_->data().root();
        const auto* base = root != nullptr ? lookup(root->bases(), owner_.base_form_of(bench_ref)) : nullptr;
        return base != nullptr ? static_cast<int>(base->bench_type()) : 0;
    }
    bool bench_has_keyword(std::uint32_t bench_ref, std::uint32_t keyword) const override {
        return owner_.world_->data().base_has_keyword(owner_.base_form_of(bench_ref), keyword);
    }
    std::uint32_t keyword(std::string_view editor_id) const override {
        return owner_.world_->data().keyword_by_editor_id(editor_id);
    }
    void log(const std::string& line) override {
        owner_.emit_signal("trace", to_godot(line));
        godot::UtilityFunctions::print(to_godot(line));
    }

private:
    SkydotPapyrus& owner_;
};

crafting::Crafting* SkydotPapyrus::crafting_logic() {
    if (crafting_) {
        return crafting_.get();
    }
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (root == nullptr || !vm_) {
        return nullptr;
    }
    crafting_host_ = std::make_unique<CraftingAdapter>(*this);
    crafting_ = std::make_unique<crafting::Crafting>(*root, *crafting_host_, vm_->inventory(), items_.extras(),
                                                     created_, k_player_ref);
    return crafting_.get();
}

double SkydotPapyrus::game_days() const {
    return clock_.is_valid() ? clock_->get_game_days() : 0.0;
}

bool SkydotPapyrus::harvested(std::int64_t ref) const {
    return harvests_.harvested(static_cast<std::uint32_t>(ref), game_days());
}

godot::PackedInt64Array SkydotPapyrus::get_harvested_refs() const {
    godot::PackedInt64Array out;
    for (const std::uint32_t ref : harvests_.harvested_refs(game_days())) {
        out.push_back(static_cast<std::int64_t>(ref));
    }
    return out;
}

namespace {

Array components_array(const std::vector<crafting::Component>& components,
                       const std::function<std::string(std::uint32_t)>& name_of) {
    Array out;
    for (const auto& c : components) {
        Dictionary entry;
        entry["form"] = static_cast<std::int64_t>(c.form);
        entry["name"] = to_godot(name_of(c.form));
        entry["need"] = c.need;
        entry["have"] = c.have;
        out.push_back(entry);
    }
    return out;
}

} // namespace

Array SkydotPapyrus::recipes(std::int64_t bench_ref) {
    Array out;
    auto* logic = crafting_logic();
    if (logic == nullptr) {
        return out;
    }
    resolve_owner(k_player_ref);
    const auto name_of = [this](std::uint32_t form) { return item_name_of(form); };
    for (const auto& row : logic->recipes(static_cast<std::uint32_t>(bench_ref))) {
        Dictionary entry;
        entry["recipe"] = static_cast<std::int64_t>(row.recipe);
        entry["product"] = static_cast<std::int64_t>(row.product);
        entry["name"] = to_godot(item_name_of(row.product));
        entry["count"] = static_cast<std::int64_t>(row.count);
        entry["components"] = components_array(row.components, name_of);
        entry["craftable"] = row.craftable;
        out.push_back(entry);
    }
    return out;
}

std::int64_t SkydotPapyrus::craft(std::int64_t recipe, std::int64_t count) {
    auto* logic = crafting_logic();
    if (logic == nullptr) {
        return 0;
    }
    resolve_owner(k_player_ref);
    return logic->craft(static_cast<std::uint32_t>(recipe), count);
}

Array SkydotPapyrus::temper_rows(std::int64_t bench_ref) {
    Array out;
    auto* logic = crafting_logic();
    if (logic == nullptr) {
        return out;
    }
    resolve_owner(k_player_ref);
    const auto name_of = [this](std::uint32_t form) { return item_name_of(form); };
    for (const auto& row : logic->temper_rows(static_cast<std::uint32_t>(bench_ref))) {
        Dictionary entry;
        std::string name = item_name_of(row.form);
        const char* tier = crafting::Crafting::tier_name(row.health);
        if (tier[0] != '\0') {
            name += std::string(" (") + tier + ")";
        }
        entry["recipe"] = static_cast<std::int64_t>(row.recipe);
        entry["form"] = static_cast<std::int64_t>(row.form);
        entry["extra"] = static_cast<std::int64_t>(row.extra);
        entry["name"] = to_godot(name);
        entry["count"] = row.count;
        entry["tier"] = to_godot(tier);
        entry["next_tier"] = to_godot(row.next_health > row.health ? crafting::Crafting::tier_name(row.next_health) : "");
        entry["components"] = components_array(row.components, name_of);
        entry["craftable"] = row.craftable;
        out.push_back(entry);
    }
    return out;
}

bool SkydotPapyrus::temper(std::int64_t recipe, std::int64_t form, std::int64_t extra) {
    auto* logic = crafting_logic();
    if (logic == nullptr) {
        return false;
    }
    resolve_owner(k_player_ref);
    const auto base = static_cast<std::uint32_t>(form);
    const auto old_extra = static_cast<std::uint32_t>(extra);
    const bool was_equipped = items_.find(k_player_ref) != nullptr && items_.find(k_player_ref)->holds(base, old_extra);
    if (!logic->temper(static_cast<std::uint32_t>(recipe), base, old_extra)) {
        return false;
    }
    // The unit that was equipped is the one that got better.
    if (was_equipped && vm_->inventory().count_extra(k_player_ref, base, old_extra) <= 0) {
        refresh_equipment(k_player_ref, base);
    }
    return true;
}

Array SkydotPapyrus::enchant_rows() {
    Array out;
    auto* logic = crafting_logic();
    if (logic == nullptr) {
        return out;
    }
    resolve_owner(k_player_ref);
    for (const auto& row : logic->enchant_rows()) {
        Dictionary entry;
        entry["form"] = static_cast<std::int64_t>(row.form);
        entry["extra"] = static_cast<std::int64_t>(row.extra);
        entry["name"] = to_godot(item_name_of(row.form));
        entry["enchantment"] = static_cast<std::int64_t>(row.enchantment);
        out.push_back(entry);
    }
    return out;
}

Array SkydotPapyrus::alchemy_rows() {
    Array out;
    auto* logic = crafting_logic();
    if (logic == nullptr) {
        return out;
    }
    resolve_owner(k_player_ref);
    for (const auto& row : logic->alchemy_rows()) {
        Dictionary entry;
        entry["form"] = static_cast<std::int64_t>(row.form);
        entry["name"] = to_godot(item_name_of(row.form));
        entry["count"] = row.count;
        Array effects;
        for (const auto& e : row.effects) {
            Dictionary effect;
            // An effect the player has not learned is not named.
            effect["name"] = to_godot(e.known ? logic->effect_name(e.effect) : std::string("?"));
            effect["known"] = e.known;
            effects.push_back(effect);
        }
        entry["effects"] = effects;
        out.push_back(entry);
    }
    return out;
}

Dictionary SkydotPapyrus::alchemy_preview(const godot::PackedInt64Array& forms) {
    Dictionary out;
    out["name"] = String();
    out["poison"] = false;
    out["value"] = 0;
    out["effects"] = Array();
    out["mixable"] = false;
    auto* logic = crafting_logic();
    if (logic == nullptr) {
        return out;
    }
    std::vector<std::uint32_t> list;
    for (const auto form : forms) {
        list.push_back(static_cast<std::uint32_t>(form));
    }
    const auto preview = logic->preview(list);
    out["name"] = to_godot(preview.name);
    out["poison"] = preview.poison;
    out["value"] = static_cast<std::int64_t>(preview.value);
    out["mixable"] = preview.mixable;
    Array effects;
    for (const auto& e : preview.effects) {
        Dictionary effect;
        effect["name"] = to_godot(logic->effect_name(e.effect));
        effect["magnitude"] = static_cast<double>(e.magnitude);
        effect["duration"] = static_cast<std::int64_t>(e.duration);
        effect["description"] = to_godot(logic->describe(e.effect, e.magnitude, e.duration));
        effects.push_back(effect);
    }
    out["effects"] = effects;
    return out;
}

std::int64_t SkydotPapyrus::alchemy_mix(const godot::PackedInt64Array& forms) {
    auto* logic = crafting_logic();
    if (logic == nullptr) {
        return 0;
    }
    resolve_owner(k_player_ref);
    std::vector<std::uint32_t> list;
    for (const auto form : forms) {
        list.push_back(static_cast<std::uint32_t>(form));
    }
    return static_cast<std::int64_t>(logic->mix(list));
}

} // namespace skydot
