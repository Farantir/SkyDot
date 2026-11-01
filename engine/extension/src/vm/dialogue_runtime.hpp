// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dialogue domain of the Papyrus runtime: owns what was said (DLGS), the
// selection logic over it (dialogue/dialogue.hpp), the INFOs whose fragment
// scripts are attached, and the dice and names the logic asks for. No Godot:
// it reaches the engine through DialogueRuntime::Host and ActorHost.
#pragma once

#include "dialogue/dialogue.hpp"
#include "dialogue/dialogue_states.hpp"
#include "vm/actor_host.hpp"
#include "world_generated.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace skydot::vm {

class DialogueRuntime final : public dialogue::DialogueHost {
public:
    class Host {
    public:
        virtual ~Host() = default;
        /// The pack's world records; null without a world.
        [[nodiscard]] virtual const bethconv::pack::wfb::World* world_root() const = 0;
        /// Actor states and the condition world.
        [[nodiscard]] virtual ActorHost& actor_host() = 0;
        /// The NPC_ behind an actor reference; 0 if none.
        [[nodiscard]] virtual std::uint32_t base_of(std::uint32_t ref) const = 0;
    };

    /// Remember the host (SkydotPapyrus::bind_natives).
    void bind(Host& host) { host_ = &host; }
    /// Forget what was said, the selection logic and the attached INFOs (a new world). The dice go on.
    void reset();

    /// The selection logic, made on first use; null without a world or a host.
    dialogue::Dialogue* logic();
    /// The selection logic if it exists.
    [[nodiscard]] dialogue::Dialogue* existing() const { return dialogue_.get(); }

    /// Whether `ref` is a living actor that dialogue can address (not the player).
    [[nodiscard]] bool can_talk(std::uint32_t ref) const;
    /// The NPC_ of a reference: the player's 0x7, else the actor's base.
    [[nodiscard]] std::uint32_t npc_of(std::uint32_t ref) const;

    // What was said (DLGS).
    [[nodiscard]] std::vector<std::uint8_t> save() const { return states_.save(); }
    void adopt(dialogue::DialogueStates&& loaded) { states_.adopt(std::move(loaded)); }
    [[nodiscard]] bool talked_to_player(std::uint32_t ref) const { return states_.talked_to_player(ref); }
    [[nodiscard]] std::uint32_t speaker() const { return states_.speaker(); }
    void forget_talked(std::uint32_t ref) { states_.forget_talked(ref); }
    [[nodiscard]] std::size_t talked_count() const { return states_.talked_count(); }

    /// INFO -> the quest of its topic, for INFOs whose fragment scripts are attached.
    [[nodiscard]] bool info_attached(std::uint32_t info) const { return info_quests_.contains(info); }
    void set_info_quest(std::uint32_t info, std::uint32_t quest) { info_quests_[info] = quest; }
    [[nodiscard]] bool owning_quest(std::uint32_t info, std::uint32_t& quest) const;

    // dialogue::DialogueHost
    double random() override;
    std::string name_of(std::uint32_t ref) const override;

private:
    Host* host_ = nullptr;
    dialogue::DialogueStates states_;
    std::unique_ptr<dialogue::Dialogue> dialogue_;
    std::map<std::uint32_t, std::uint32_t> info_quests_;
    std::uint64_t dice_ = 0x2545F4914F6CDD1Dull;
};

} // namespace skydot::vm
