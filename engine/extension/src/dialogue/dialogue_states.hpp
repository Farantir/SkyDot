// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a game has said: the INFOs and topics spoken, the Say Once INFOs, the
// speakers who talked to the player (GetTalkedToPC) and who the player talks
// to now (IsInDialogueWithPlayer, not saved). Saved as the DLGS section
// (ADR 0004). Nothing here touches Godot.
#pragma once

#include "save/bytes.hpp"

#include <cstdint>
#include <set>
#include <vector>

namespace skydot::dialogue {

class DialogueStates {
public:
    static constexpr std::uint32_t k_section_version = 1;

    [[nodiscard]] bool info_said(std::uint32_t info) const { return infos_.contains(info); }
    [[nodiscard]] bool topic_said(std::uint32_t topic) const { return topics_.contains(topic); }
    /// A Say Once INFO that was spoken.
    [[nodiscard]] bool said_once(std::uint32_t info) const { return once_.contains(info); }
    [[nodiscard]] bool talked_to_player(std::uint32_t speaker) const { return talked_.contains(speaker); }
    [[nodiscard]] std::uint32_t speaker() const { return speaker_; }

    void mark_info(std::uint32_t info, bool say_once) {
        infos_.insert(info);
        if (say_once) {
            once_.insert(info);
        }
    }
    void mark_topic(std::uint32_t topic) { topics_.insert(topic); }
    void mark_talked(std::uint32_t speaker) { talked_.insert(speaker); }
    /// 0 when the dialogue is over.
    void set_speaker(std::uint32_t speaker) { speaker_ = speaker; }

    void clear() { *this = DialogueStates{}; }
    /// Take the records of `other` (a loaded section); the current speaker
    /// is not part of a save, so nobody is talking afterwards.
    void adopt(DialogueStates&& other) {
        infos_ = std::move(other.infos_);
        topics_ = std::move(other.topics_);
        once_ = std::move(other.once_);
        talked_ = std::move(other.talked_);
        speaker_ = 0;
    }

    [[nodiscard]] std::vector<std::uint8_t> save() const;
    /// Decode a section into a fresh object; false if it is cut short or has
    /// bytes left over.
    [[nodiscard]] static bool load(save::ByteReader r, DialogueStates& out);

private:
    std::set<std::uint32_t> infos_;
    std::set<std::uint32_t> topics_;
    std::set<std::uint32_t> once_;
    std::set<std::uint32_t> talked_;
    std::uint32_t speaker_ = 0;
};

} // namespace skydot::dialogue
