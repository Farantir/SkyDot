// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dialogue domain of the Papyrus runtime (vm/dialogue_runtime.hpp).
#include "vm/dialogue_runtime.hpp"

#include "data/actors.hpp"
#include "vm/activation.hpp"

namespace skydot::vm {

namespace wfb = bethconv::pack::wfb;

void DialogueRuntime::reset() {
    states_.clear();
    dialogue_.reset();
    info_quests_.clear();
}

dialogue::Dialogue* DialogueRuntime::logic() {
    if (dialogue_ == nullptr && host_ != nullptr) {
        const auto* root = host_->world_root();
        if (root == nullptr) {
            return nullptr;
        }
        dialogue_ = std::make_unique<dialogue::Dialogue>(*root, states_, host_->actor_host().conditions(), *this);
    }
    return dialogue_.get();
}

bool DialogueRuntime::can_talk(std::uint32_t ref) const {
    if (ref == k_player_ref || host_ == nullptr || host_->world_root() == nullptr) {
        return false;
    }
    auto& actors = host_->actor_host().actor_states();
    return actors.is_actor(ref) && !actors.is_dead(ref);
}

std::uint32_t DialogueRuntime::npc_of(std::uint32_t ref) const {
    return ref == k_player_ref ? 0x7 : host_->base_of(ref);
}

bool DialogueRuntime::owning_quest(std::uint32_t info, std::uint32_t& quest) const {
    const auto it = info_quests_.find(info);
    if (it == info_quests_.end()) {
        return false;
    }
    quest = it->second;
    return true;
}

double DialogueRuntime::random() {
    dice_ ^= dice_ >> 12;
    dice_ ^= dice_ << 25;
    dice_ ^= dice_ >> 27;
    return static_cast<double>((dice_ * 0x2545F4914F6CDD1Dull) >> 11) / static_cast<double>(1ull << 53);
}

std::string DialogueRuntime::name_of(std::uint32_t ref) const {
    const auto* root = host_ != nullptr ? host_->world_root() : nullptr;
    const std::uint32_t npc = ref != 0 ? npc_of(ref) : 0;
    if (root == nullptr || npc == 0) {
        return {};
    }
    const auto* n = resolve_npc(*root, npc, wfb::NpcTemplateFlags::use_traits, ref);
    return n != nullptr && n->name() != nullptr ? n->name()->str() : std::string();
}

} // namespace skydot::vm
