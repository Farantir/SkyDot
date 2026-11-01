// SPDX-License-Identifier: GPL-3.0-or-later
//
// The activation domain of the Papyrus runtime (vm/activation_runtime.hpp).
#include "vm/activation_runtime.hpp"

#include "data/fb_search.hpp"
#include "data/leveled.hpp"
#include "state/calendar.hpp"

namespace skydot::vm {

namespace wfb = bethconv::pack::wfb;

namespace {

/// Added to the seed's purpose: apart from the chance roll's.
constexpr std::uint64_t k_harvest_list_purpose = 0x4C495354ULL; // "LIST"

} // namespace

ActivationOutcome ActivationRuntime::activate(std::uint32_t ref, std::int64_t activator, bool default_only,
                                              ActivationSource source, bool force) {
    return activations_.activate(ref, activator, default_only, source, force);
}

std::optional<ActivationOutcome> ActivationRuntime::complete(std::uint32_t ref) { return activations_.complete(ref); }

const wfb::Base* ActivationRuntime::base_of(std::uint32_t base) const {
    const auto* root = host_ != nullptr ? host_->world_root() : nullptr;
    return root != nullptr ? lookup(root->bases(), base) : nullptr;
}

bool ActivationRuntime::target(std::uint32_t ref, ActivationTarget& out) const {
    RefInfo info;
    if (host_ == nullptr || !host_->world_access().ref_info(ref, info)) {
        // A placed actor is no reference of its cell: its base is the NPC_.
        const std::uint32_t npc = host_ != nullptr ? host_->placed_actor_base(ref) : 0;
        if (npc == 0) {
            return false;
        }
        out.base = npc;
        out.type = "NPC_";
        out.dead = host_->actor_states().is_dead(ref);
        return true;
    }
    out.base = info.base;
    out.type = info.type;
    out.editor_id = info.editor_id;
    out.parent_activate_only = info.parent_activate_only;
    out.load_door = info.load_door;
    // XCNT is not in the reference info yet: one of them.
    out.count = 1;
    out.dead = host_->actor_states().is_dead(ref);
    if (const auto* base = base_of(out.base)) {
        out.harvestable = (out.type == "FLOR" || out.type == "TREE") && base->harvest() != nullptr &&
                          base->harvest()->ingredient() != 0;
        out.bench_type = out.type == "FURN" ? static_cast<int>(base->bench_type()) : 0;
    }
    return true;
}

bool ActivationRuntime::harvested(std::uint32_t ref) const {
    return harvests_.harvested(ref, host_->world_access().game_days());
}

HarvestResult ActivationRuntime::harvest(std::uint32_t ref, std::int64_t activator) {
    HarvestResult out;
    const auto* root = host_->world_root();
    const auto* base = base_of(host_->world_access().base_form(ref));
    if (root == nullptr || base == nullptr || base->harvest() == nullptr) {
        return out;
    }
    const auto* h = base->harvest();
    const double now = host_->world_access().game_days();
    const auto season = season_of_month(Calendar{.days = now}.month());
    const int chance = harvest_chance(h->seasonal_yield(), season);
    const std::uint32_t times = harvests_.times(ref);
    const bool success = harvest_succeeds(chance, ref, times);
    harvests_.mark(ref, now);
    out.sound = h->sound();
    if (!success) {
        return out;
    }
    std::vector<leveled::Entry> given;
    if (leveled::is_list(*root, h->ingredient())) {
        const leveled::GlobalValue global = [this](std::uint32_t id) { return host_->global_value_of(id); };
        leveled::Rng rng(leveled::seed_for(ref, k_harvest_list_purpose + times));
        given = leveled::resolve(*root, h->ingredient(), leveled::player_level(), rng, global);
    } else {
        given.push_back({h->ingredient(), 1});
    }
    const auto who = static_cast<std::uint32_t>(activator);
    host_->items().resolve_owner(who);
    for (const auto& entry : given) {
        host_->inventory().add(who, entry.form, entry.count);
        if (out.form == 0) {
            out.form = entry.form;
            out.name = host_->items().item_name_of(entry.form);
        }
        if (entry.form == out.form) {
            out.count += entry.count;
        }
    }
    return out;
}

int ActivationRuntime::lock_level(std::uint32_t ref) const {
    return static_cast<int>(host_->references().lock_level(ref));
}

void ActivationRuntime::unlock(std::uint32_t ref) { host_->references().set_locked(ref, false); }

bool ActivationRuntime::graph_raises_activation(std::uint32_t ref) const { return host_->graph_raises_activation(ref); }

std::int64_t ActivationRuntime::send_activate(std::uint32_t ref, std::int64_t activator) {
    return host_->send_activate(ref, activator);
}

bool ActivationRuntime::activation_blocked(std::uint32_t ref) const {
    return host_->references().ref_states().blocked.contains(ref);
}

std::vector<ActivationHost::Child> ActivationRuntime::activate_children(std::uint32_t ref) const {
    std::vector<Child> out;
    for (const auto& child : host_->world_access().activate_children(ref)) {
        out.push_back({child.ref, child.delay});
    }
    return out;
}

void ActivationRuntime::schedule_child(double delay, std::uint32_t child, std::uint32_t parent) {
    host_->schedule_activation(delay, child, parent);
}

void ActivationRuntime::pick_up(std::uint32_t ref, std::int64_t activator, const ActivationTarget& item) {
    // A dropped item stands for the count it was dropped with, and is gone once taken.
    const std::int64_t dropped = host_->items().dropped_count(ref);
    host_->items().resolve_owner(static_cast<std::uint32_t>(activator));
    host_->inventory().add(static_cast<std::uint32_t>(activator), item.base, dropped > 0 ? dropped : item.count,
                           host_->items().dropped_extra(ref));
    if (dropped > 0) {
        host_->forget_ref(ref);
    } else {
        host_->references().set_disabled(ref, true);
    }
}

} // namespace skydot::vm
