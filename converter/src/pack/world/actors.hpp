// SPDX-License-Identifier: GPL-3.0-or-later
//
// What actors are built from: NPC_, RACE, ARMO, ARMA, OTFT, LVLI and LVLN.
// NPC_, ARMO and LVLN are placed and scripted too, so the dispatcher also
// offers them to BaseCollector. Private to pack/world/.
#pragma once

#include "context.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/merge.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace bethconv::pack::detail {

class ActorCollector {
public:
    explicit ActorCollector(CollectContext& shared) : shared_(shared) {}

    /// NPC_, RACE, ARMO, ARMA, OTFT, LVLI or LVLN; nothing for another type.
    void collect(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);

    /// The NPCs, in id order. An NPC's default package list is an FLST; its
    /// packages are expanded from `form_lists`.
    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Npc>> write_npcs(
        flatbuffers::FlatBufferBuilder& builder, const FormLists& form_lists);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Race>> write_races(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Armor>> write_armors(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::ArmorAddon>> write_armor_addons(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Outfit>> write_outfits(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::LeveledList>> write_leveled_lists(
        flatbuffers::FlatBufferBuilder& builder);

private:
    void on_npc(const record::MergedRecord& merged, io::SpanReader& data,
                const record::FormContext& form_ctx);
    void on_race(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);
    void on_armor(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx);
    void on_armor_addon(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx);
    void on_outfit(const record::MergedRecord& merged, io::SpanReader& data,
                   const record::FormContext& form_ctx);
    void on_leveled_item(const record::MergedRecord& merged, io::SpanReader& data,
                         const record::FormContext& form_ctx);
    void on_leveled_npc(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx);

    template <typename Entries>
    void add_leveled(const record::MergedRecord& merged, std::uint8_t flags,
                     std::uint8_t chance_none, const Entries& entries);

    CollectContext& shared_;
    std::map<std::uint32_t, WorldNpc> npcs_;
    std::map<std::uint32_t, WorldRace> races_;
    std::map<std::uint32_t, WorldArmor> armors_;
    std::map<std::uint32_t, WorldArmorAddon> armor_addons_;
    std::map<std::uint32_t, WorldOutfit> outfits_;
    std::map<std::uint32_t, WorldLeveledList> leveled_lists_;
};

} // namespace bethconv::pack::detail
