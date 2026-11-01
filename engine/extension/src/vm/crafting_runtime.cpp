// SPDX-License-Identifier: GPL-3.0-or-later
//
// The crafting domain of the Papyrus runtime (vm/crafting_runtime.hpp).
#include "vm/crafting_runtime.hpp"

#include "conditions/functions.hpp"
#include "data/fb_search.hpp"
#include "data/world_data.hpp"
#include "vm/activation.hpp"

namespace skydot::vm {

namespace wfb = bethconv::pack::wfb;

namespace {

/// EPTemperingItemIsEnchanted from the item being tempered.
class TemperOverrides final : public conditions::ConditionOverrides {
public:
    explicit TemperOverrides(const crafting::ConditionItem* item) : item_(item) {}
    std::optional<double> function_value(const wfb::Condition& c, std::uint32_t,
                                         const conditions::ConditionContext&) override {
        if (c.function() == conditions::fn::ep_tempering_item_is_enchanted && item_ != nullptr) {
            return item_->enchanted ? 1.0 : 0.0;
        }
        return std::nullopt;
    }

private:
    const crafting::ConditionItem* item_;
};

} // namespace

crafting::Crafting* CraftingRuntime::logic() {
    if (crafting_ == nullptr && host_ != nullptr) {
        const auto* root = host_->world_root();
        if (root == nullptr) {
            return nullptr;
        }
        crafting_ = std::make_unique<crafting::Crafting>(*root, *this, host_->inventory(), host_->item_extras(),
                                                         host_->created_forms(), k_player_ref);
    }
    return crafting_.get();
}

bool CraftingRuntime::conditions_pass(
    const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, const crafting::ConditionItem* item) {
    const auto world = host_->new_condition_world();
    conditions::ConditionContext ctx;
    ctx.subject = k_player_ref;
    ctx.unknown = conditions::UnknownPolicy::fails;
    TemperOverrides overrides(item);
    return conditions::conditions_pass(list, *world, ctx, &overrides);
}

int CraftingRuntime::bench_type(std::uint32_t bench_ref) const {
    const auto* root = host_->world_root();
    const auto* base = root != nullptr ? lookup(root->bases(), host_->world_access().base_form(bench_ref)) : nullptr;
    return base != nullptr ? static_cast<int>(base->bench_type()) : 0;
}

bool CraftingRuntime::bench_has_keyword(std::uint32_t bench_ref, std::uint32_t keyword) const {
    return host_->world_access().has_keyword(bench_ref, keyword);
}

std::uint32_t CraftingRuntime::keyword(std::string_view editor_id) const {
    return host_->world_access().find_keyword(std::string(editor_id));
}

double CraftingRuntime::skill(int av) const {
    return host_->player_actor_value(static_cast<std::uint32_t>(av));
}

double CraftingRuntime::perk_entry(perks::EntryPoint ep, const perks::EntryContext& ctx, double value) const {
    return host_->perk_entry(ep, ctx, value);
}

std::string CraftingRuntime::gmst_text(std::string_view name, std::string_view fallback) const {
    const auto* g = WorldData::find_gmst(host_->world_root(), name);
    return g != nullptr && g->text_value() != nullptr && g->text_value()->size() > 0 ? g->text_value()->str()
                                                                                      : std::string(fallback);
}

void CraftingRuntime::log(const std::string& line) { host_->announce(line); }

} // namespace skydot::vm
