// SPDX-License-Identifier: GPL-3.0-or-later
//
// mesh::Model -> GLB, using fastgltf.
//
// The writer decides:
//
//   * Coordinates: Z-up game units to Y-up metres via one root node, so buffer
//     values stay identical to the NIF.
//   * PBR vs. `extras`: Skyrim materials hold more than metallic-roughness can
//     express; the rest goes to `extras`.
//   * Collision: `extras` only, never geometry.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/mesh/mesh_ir.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace bethconv::mesh {

/// How texture references are emitted.
enum class TextureRefs : std::uint8_t {
    /// No images at all. The slot paths still appear in material extras.
    none,
    /// One glTF image per distinct slot path, normalized (separators, case) and
    /// URI-escaped (`textures\\Foo.dds` -> `textures/foo.dds`,
    /// `house crafting/nail01.dds` -> `house%20crafting/nail01.dds`). The URI is
    /// relative to the GLB's location, because glTF resolves against the
    /// document; `extras` keep the unescaped pack-relative path.
    source_paths,
};

struct WriteOptions {
    /// Rotate Z-up to Y-up at the root. Off keeps NIF space, for comparisons
    /// with the source.
    bool convert_to_y_up = true;

    /// Metres per game unit (64 units per yard = 0.0142875). Applied at the
    /// root.
    float unit_scale = 0.0142875f;

    TextureRefs texture_refs = TextureRefs::source_paths;

    /// Write Bethesda material fields and collision to `extras`. Off gives
    /// plain glTF for comparing with other tools.
    bool write_extras = true;
};

/// A texture path as a relative URI reference, escaped as the writer does
/// (`house crafting/nail01.dds` -> `house%20crafting/nail01.dds`).
[[nodiscard]] std::string escape_texture_uri(std::string_view path);

/// Serialize `model` as a self-contained GLB. Never throws.
[[nodiscard]] io::ParseResult<std::vector<std::byte>> write_glb(const Model& model,
                                                                const WriteOptions& options = {});

/// Serialize and write to `path`, creating parent directories.
[[nodiscard]] io::ParseResult<std::size_t> write_glb_file(const Model& model,
                                                          const std::filesystem::path& path,
                                                          const WriteOptions& options = {});

} // namespace bethconv::mesh
