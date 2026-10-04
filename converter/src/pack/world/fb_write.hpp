// SPDX-License-Identifier: GPL-3.0-or-later
//
// FlatBuffers pieces the collectors' write functions share. Private to
// pack/world/.
#pragma once

#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/vmad.hpp"

#include <vector>

namespace bethconv::pack::detail {

[[nodiscard]] flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<wfb::Script>>>
write_scripts(flatbuffers::FlatBufferBuilder& builder, const std::vector<record::Script>& scripts);

[[nodiscard]] wfb::Vec3f to_fb(const record::Vec3& v);

} // namespace bethconv::pack::detail
