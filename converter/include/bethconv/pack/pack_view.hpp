// SPDX-License-Identifier: GPL-3.0-or-later
//
// Materializes a pack as a directory tree that mirrors the virtual filesystem,
// so any glTF consumer can open it.
//
// In a pack, a GLB lives at `assets/3f/3f9c....glb` and names its textures by
// virtual path (`textures/clutter/apple01.dds`), because a relative path to the
// texture's hash would go stale when the texture changes (see
// `mesh::TextureRefs::pack_vpaths`). Without `vpath.idx`, Godot and Blender load
// such meshes with no textures.
//
//     view/
//       meshes/clutter/apple01.glb      <- rewritten: URIs rebased to ../..
//       textures/clutter/apple01.dds    <- hard link into the pack
//       scripts/wispwallscript.pex
//
// One file per index entry, with the converted extension. Textures and scripts
// are hard links, so a full view costs almost nothing.
//
// Meshes are rewritten because glTF resolves image URIs against the document:
// `textures/foo.dds` inside `meshes/clutter/apple01.glb` means
// `meshes/clutter/textures/foo.dds`. Each mesh's `images[].uri` gets the `../`
// prefix for its depth; everything else, including `extras`, is unchanged.
//
// A view is derived and disposable; it is not part of the pack format, and the
// bake could read `vpath.idx` directly.
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

/// What `rebase_glb_uris` produced.
/// Result of `rebase_glb_uris`.
struct RebasedGlb {
    std::vector<std::byte> bytes;

    /// Virtual paths named by the images, percent-decoded to the `vpath.idx`
    /// form, deduplicated, in document order.
    std::vector<std::string> image_vpaths;

    /// Texture paths named in the materials' `extras` (all texture slots,
    /// e.g. effect palettes, glow and environment maps), which an engine
    /// shader may load even though no glTF image references them.
    std::vector<std::string> slot_vpaths;

    std::uint64_t rebased{}; ///< URIs that got the prefix.
};

/// Prefix every relative `images[].uri` in `glb` with `prefix`. Absolute URIs,
/// URIs starting with `../` and `data:` URIs are left alone and not reported.
[[nodiscard]] io::ParseResult<RebasedGlb> rebase_glb_uris(std::span<const std::byte> glb,
                                                          std::string_view prefix,
                                                          std::string_view origin);

} // namespace bethconv::pack
