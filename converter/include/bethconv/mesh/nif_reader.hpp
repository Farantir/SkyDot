// SPDX-License-Identifier: GPL-3.0-or-later
//
// NIF -> mesh::Model, using nifly.
//
// nifly parses untrusted bytes itself. Two rules follow:
//
//   1. It fails by returning non-zero or by throwing (bad_alloc, range errors
//      on hostile sizes). Every call is wrapped; nothing escapes.
//   2. No nifly types in this header, so it stays replaceable.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/mesh/mesh_ir.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace bethconv::mesh {

/// Options that change what goes into the IR.
struct ReadOptions {
    /// Read `bhkCollisionObject` shapes into Model::collision (written to
    /// `extras`). Switchable because it costs noticeable time on full sweeps.
    bool read_collision = true;

    /// Read `NiSkinInstance`/`BSDismemberSkinInstance` into Model::skins.
    bool read_skinning = true;

    /// Read controllers, sequences and particle systems into
    /// Model::animations and Model::particles.
    bool read_animations = true;

    /// Drop shapes with no triangles instead of emitting empty primitives.
    bool skip_empty_shapes = true;

    /// Drop nodes named "EditorMarker" and their subtrees: Creation Kit-only
    /// geometry the game never renders.
    bool skip_editor_markers = true;
};

/// LE vs. SE. Not the version string: both are "Gamebryo File Format, Version
/// 20.2.0.7" with user version 12. User version 2 (nifly's stream version) is
/// 83 on LE and 100 on SE/VR.
enum class NifFlavor : std::uint8_t { unknown, le, se };

[[nodiscard]] constexpr NifFlavor flavor_of(std::uint32_t stream_version) noexcept {
    if (stream_version == 83) {
        return NifFlavor::le;
    }
    if (stream_version >= 100) {
        return NifFlavor::se;
    }
    return NifFlavor::unknown;
}

[[nodiscard]] std::string_view to_string(NifFlavor flavor) noexcept;

/// Parse a NIF. `origin` (the virtual path) is used in errors. Never throws:
/// files nifly rejects give a ParseError; broken individual shapes give
/// `warnings`.
[[nodiscard]] io::ParseResult<Model> read_nif(std::span<const std::byte> bytes,
                                              std::string_view origin,
                                              const ReadOptions& options = {});

} // namespace bethconv::mesh
