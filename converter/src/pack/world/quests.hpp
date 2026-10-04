// SPDX-License-Identifier: GPL-3.0-or-later
//
// QUST and GLOB: quests with their stages, objectives, aliases and fragment
// scripts, and the global variables conditions read. Private to pack/world/.
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

class QuestCollector {
public:
    explicit QuestCollector(CollectContext& shared) : shared_(shared) {}

    /// QUST or GLOB; nothing for another type.
    void collect(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Quest>> write_quests(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Global>> write_globals(
        flatbuffers::FlatBufferBuilder& builder);

private:
    void on_quest(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx);
    void on_global(const record::MergedRecord& merged, io::SpanReader& data,
                   const record::FormContext& form_ctx);

    CollectContext& shared_;
    std::map<std::uint32_t, WorldQuest> quests_;
    std::map<std::uint32_t, WorldGlobal> globals_;
};

} // namespace bethconv::pack::detail
