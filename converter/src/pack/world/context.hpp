// SPDX-License-Identifier: GPL-3.0-or-later
//
// What every world.fb collector shares: the load order, the counters, and the
// resolver that makes the FormIDs inside a record's payload global. Private to
// pack/world/.
#pragma once

#include "bethconv/pack/world.hpp"
#include "bethconv/pack/world_generated.h"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"
#include "bethconv/record/types.hpp"
#include "bethconv/record/vmad.hpp"
#include "skydot_formats/vpath.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bethconv::pack::detail {

/// FLST id -> the FormIDs it lists, made global.
using FormLists = std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>;

class CollectContext {
public:
    explicit CollectContext(const record::LoadOrder& order) : order_(order) {}

    [[nodiscard]] const record::LoadOrder& order() const noexcept { return order_; }
    [[nodiscard]] WorldStats& stats() noexcept { return stats_; }

    /// A FormID from inside the winning record's payload, made global. 0 (and
    /// counted) if it cannot be resolved.
    [[nodiscard]] std::uint32_t global(const record::MergedRecord& merged, record::FormId local,
                                       bool& failed) const;

    /// Script data as world.fb's tables, with object properties made global.
    [[nodiscard]] std::vector<std::unique_ptr<wfb::ScriptT>> global_scripts(
        const record::MergedRecord& merged, const record::ScriptData& data, bool& failed) const;

    [[nodiscard]] std::vector<std::uint32_t> global_all(const record::MergedRecord& merged,
                                                        const std::vector<record::FormId>& forms,
                                                        bool& failed) const;

private:
    const record::LoadOrder& order_;
    WorldStats stats_;
};

/// MODL values are relative to `Data\meshes\`, though some plugins include the
/// prefix, and TXST paths to `Data\textures\`: normalized virtual paths, the
/// engine's rule (formats/include/skydot_formats/vpath.hpp).
using skydot::formats::model_vpath;
using skydot::formats::texture_vpath;

} // namespace bethconv::pack::detail
