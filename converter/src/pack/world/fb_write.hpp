// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the collectors share for world.fb's generated types. Private to
// pack/world/.
#pragma once

#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace bethconv::pack::detail {

/// Whether the pack's flag name and the record layer's name the same bit. The
/// pack's flag words are the plugin's, as stored, so the collectors only cast;
/// the asserts next to the casts keep the two sets of names from drifting.
template <typename Pack, typename Record>
[[nodiscard]] constexpr bool same_bit(Pack pack, Record record) noexcept {
    return std::to_underlying(pack) == std::to_underlying(record);
}

/// `tables` (keyed by id) moved into `out`, in key order.
template <typename T>
void move_into(std::vector<std::unique_ptr<T>>& out, std::map<std::uint32_t, T>& tables) {
    out.reserve(tables.size());
    for (auto& [id, table] : tables) {
        out.push_back(std::make_unique<T>(std::move(table)));
    }
}

[[nodiscard]] wfb::Vec3f to_fb(const record::Vec3& v);

} // namespace bethconv::pack::detail
