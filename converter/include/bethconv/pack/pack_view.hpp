// SPDX-License-Identifier: GPL-3.0-or-later
//
// Materializes a pack as a directory tree that mirrors the virtual filesystem,
// so any glTF consumer (Godot, Blender) can open it.
//
// A pack's meshes have no glTF images: the engine resolves the texture paths
// in their material extras through `vpath.idx`. The view adds images whose
// URIs reach the view's own copy of each texture:
//
//     view/
//       meshes/clutter/apple01.glb      <- images added, URIs start with ../..
//       textures/clutter/apple01.dds    <- hard link (loose pack) or copy (blob)
//       scripts/wispwallscript.pexfb
//
// One file per index entry, with the converted extension. glTF resolves image
// URIs against the document, so each mesh gets the `../` prefix for its depth;
// everything else, including `extras`, is unchanged.
//
// A view is derived and disposable; it is not part of the pack format. From a
// blob pack every file is a copy, so view a filtered subset.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/pack/pack_writer.hpp"
#include "bethconv/pack/vpath_index.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace bethconv::pack {

struct ViewOptions {
    std::filesystem::path out;

    /// Copy assets instead of hard-linking. Off by default (a full SE view would
    /// be 19 GiB of textures). Linking already falls back to copying where hard
    /// links fail; this is for views that must outlive the pack.
    bool copy_assets = false;

    /// Substring filter on the virtual path; empty means everything. Textures
    /// referenced by selected meshes are always included.
    std::string filter;

    /// Stop after this many index entries; 0 means no limit. The index is sorted,
    /// so limited views are reproducible.
    std::size_t limit = 0;

    /// If non-empty, only these virtual paths (normalized) are materialized,
    /// plus the textures their meshes reference. Combined with `filter`.
    std::vector<std::string> only;

    std::function<void(const std::string& phase, std::uint64_t done, std::uint64_t total)>
        progress;
    std::uint64_t progress_interval = 2000;
};

/// View statistics.
struct ViewStats {
    std::uint64_t index_entries{}; ///< Lines in vpath.idx.
    std::uint64_t considered{};    ///< Entries passing the filter and limit.

    std::uint64_t meshes{};
    std::uint64_t textures{};
    std::uint64_t scripts{};
    std::uint64_t lod{};
    std::uint64_t animations{};

    std::uint64_t linked{};  ///< Hard links to pack bytes.
    std::uint64_t copied{};  ///< Copies (requested or as fallback).
    std::uint64_t written{}; ///< Meshes written after rewriting.

    /// Meshes identical to one already written (same hash and depth), linked
    /// instead of rewritten.
    std::uint64_t mesh_links{};

    /// Textures included only because a selected mesh references them.
    std::uint64_t pulled_in{};

    /// Image URIs across rewritten meshes, and how many resolve to a path in
    /// the pack. `dangling` includes the 67 FaceGen slots whose "path" is a
    /// control byte.
    std::uint64_t image_refs{};
    std::uint64_t image_refs_resolved{};
    std::uint64_t image_refs_dangling{};

    std::uint64_t failed{};
    std::uint64_t bytes{}; ///< Bytes written or copied; links count zero.
};

struct ViewResult {
    ViewStats stats;
    /// Entries that failed, in index order.
    std::vector<PackFailure> failures;
};

/// Materialize `pack`'s `vpath.idx` as a directory under `options.out`.
[[nodiscard]] io::ParseResult<ViewResult> materialize_view(const std::filesystem::path& pack,
                                                            const ViewOptions& options);

/// The `../` prefix for a document at `vpath` to reach the view root: one per
/// directory level. Same formula as `ascent_to_root` in mesh/gltf_writer.cpp,
/// kept separate because the inputs differ (source path there, index entry
/// here).
[[nodiscard]] std::string ascent_prefix(std::string_view vpath);

/// Result of `view_glb`.
struct ViewGlb {
    std::vector<std::byte> bytes;
    /// Virtual paths named by the images, percent-decoded to the `vpath.idx`
    /// form, deduplicated, in document order.
    std::vector<std::string> image_vpaths;
    /// Texture paths named in the materials' `extras` that are not images (e.g.
    /// effect palettes, environment maps), which an engine shader may load.
    std::vector<std::string> slot_vpaths;
    std::uint64_t uris{}; ///< Image URIs rebased or added.
};

/// A pack GLB made loadable from the view: images are added from the
/// materials' extras with `prefix` in front of each URI. A GLB that already has
/// images (packs before v5) gets `prefix` on every relative `images[].uri`
/// instead; absolute, `../` and `data:` URIs are left alone and not reported.
/// Everything else, including `extras` and the BIN chunk, is unchanged.
[[nodiscard]] io::ParseResult<ViewGlb> view_glb(std::span<const std::byte> glb,
                                                std::string_view prefix,
                                                std::string_view origin);

} // namespace bethconv::pack
