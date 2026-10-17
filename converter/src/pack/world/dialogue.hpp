// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dialogue (DLBR, DIAL, INFO) and the records its conditions read (VTYP, LCTN,
// RELA, FACT). Private to pack/world/.
#pragma once

#include "context.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/merge.hpp"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bethconv::pack::detail {

class DialogueCollector {
public:
    explicit DialogueCollector(CollectContext& shared) : shared_(shared) {}

    /// DLBR, DIAL, INFO, VTYP, LCTN, RELA or FACT; nothing for another type.
    void collect(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);

    /// The tables, each in id order, into `world`. A topic's INFOs are placed
    /// in the order the merge first saw them, which is the base plugin's group
    /// order with later plugins' new INFOs after it.
    void finish(wfb::WorldT& world, const record::MergedWorld& merged);

private:
    void on_branch(const record::MergedRecord& merged, io::SpanReader& data,
                   const record::FormContext& form_ctx);
    void on_topic(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx);
    void on_info(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);
    void on_voice_type(const record::MergedRecord& merged, io::SpanReader& data,
                       const record::FormContext& form_ctx);
    void on_location(const record::MergedRecord& merged, io::SpanReader& data,
                     const record::FormContext& form_ctx);
    void on_relationship(const record::MergedRecord& merged, io::SpanReader& data,
                         const record::FormContext& form_ctx);
    void on_faction(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx);

    struct PendingInfo {
        std::uint32_t topic{};
        wfb::DialogueInfoT info;
    };

    CollectContext& shared_;
    std::map<std::uint32_t, wfb::DialogueBranchT> branches_;
    std::map<std::uint32_t, wfb::DialogueTopicT> topics_;
    std::map<std::uint32_t, PendingInfo> infos_; ///< By INFO id.
    std::map<std::uint32_t, wfb::VoiceTypeT> voice_types_;
    std::map<std::uint32_t, wfb::LocationT> locations_;
    std::map<std::uint32_t, wfb::RelationshipT> relationships_;
    std::map<std::uint32_t, wfb::FactionT> factions_;
};

} // namespace bethconv::pack::detail
