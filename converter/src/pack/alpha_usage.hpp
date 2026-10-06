// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which diffuse textures are alpha-tested, from how the meshes use them.
//
// A texture file says nothing about this: a leaf and a window pane are both
// DXT5 with a varying alpha. The material does: NiAlphaProperty either tests
// alpha at a threshold (a cut-out; the coverage pass keeps that threshold's
// coverage through the mips), blends it, or is absent. So before the textures
// convert, every mesh in the mount is read once and each material's diffuse
// slot (0) is tallied. Textures also named in another slot (a normal map's alpha
// is gloss) are left alone. Private to pack/.
#pragma once

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/pack/convert.hpp"
#include "bethconv/pack/pack_writer.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

namespace bethconv::pack {

/// vpath of a diffuse texture -> the alpha-test threshold (1..255) its
/// coverage is kept at.
using AlphaThresholds = std::unordered_map<std::string, std::uint32_t>;

struct AlphaUsage {
    AlphaThresholds thresholds;
    AlphaCoverageRecord summary;
};

/// Read every `.nif`, `.btr` and `.bto` of the mount on `jobs` threads (0: every
/// core). The result does not depend on the thread count.
[[nodiscard]] AlphaUsage scan_alpha_usage(const archive::ArchiveSet& set, unsigned jobs);

/// The part of a texture's recipe that its threshold makes, hashed into its
/// asset name; `cov/1` is bumped when the pass's output changes.
[[nodiscard]] std::string coverage_recipe(std::uint32_t threshold, AlphaCoverage mode);

} // namespace bethconv::pack
