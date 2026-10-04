// SPDX-License-Identifier: GPL-3.0-or-later
//
// The merge pass's sink for world.fb: it owns the collectors and the context
// they share, and hands each winning record to the collector that owns its
// type. Private to pack/world/.
#pragma once

#include "actors.hpp"
#include "ai.hpp"
#include "bases.hpp"
#include "context.hpp"
#include "environment.hpp"
#include "places.hpp"
#include "quests.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"
#include "bethconv/record/plugin.hpp"

namespace bethconv::pack::detail {

class WorldSink final : public record::MergedRecordSink {
public:
    explicit WorldSink(const record::LoadOrder& order)
        : shared_(order),
          places_(shared_),
          bases_(shared_),
          environment_(shared_),
          quests_(shared_),
          actors_(shared_),
          ai_(shared_) {}

    /// Deleted records are skipped, and so are INFO records. NPC_, ARMO and
    /// LVLN are collected twice, by ActorCollector and as generic bases; any
    /// type no collector names is a base if it has a model or scripts.
    void on_record(const record::MergedRecord& merged, const record::RecordContext& ctx,
                   io::SpanReader& data, const record::FormContext& form_ctx) override;

    [[nodiscard]] WorldStats& stats() noexcept { return shared_.stats(); }
    [[nodiscard]] PlaceCollector& places() noexcept { return places_; }
    [[nodiscard]] BaseCollector& bases() noexcept { return bases_; }
    [[nodiscard]] EnvironmentCollector& environment() noexcept { return environment_; }
    [[nodiscard]] QuestCollector& quests() noexcept { return quests_; }
    [[nodiscard]] ActorCollector& actors() noexcept { return actors_; }
    [[nodiscard]] AiCollector& ai() noexcept { return ai_; }

private:
    CollectContext shared_;
    PlaceCollector places_;
    BaseCollector bases_;
    EnvironmentCollector environment_;
    QuestCollector quests_;
    ActorCollector actors_;
    AiCollector ai_;
};

} // namespace bethconv::pack::detail
