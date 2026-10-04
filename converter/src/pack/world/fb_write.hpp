// SPDX-License-Identifier: GPL-3.0-or-later
//
// FlatBuffers pieces the collectors' write functions share. Private to
// pack/world/.
#pragma once

#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/vmad.hpp"

#include <cstdint>
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

[[nodiscard]] flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<wfb::Script>>>
write_scripts(flatbuffers::FlatBufferBuilder& builder, const std::vector<record::Script>& scripts);

[[nodiscard]] wfb::Vec3f to_fb(const record::Vec3& v);

} // namespace bethconv::pack::detail
