// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotPack`: a mounted bethconv pack.
//
// Packs are the only thing this engine reads. Written against
// formats/pack-format.md (v6) alone, without linking the converter's reader, so
// it also shows the document is sufficient.
//
// `open` checks, in order, and refuses on any failure:
//
//   manifest.json   `pack_format_version` must be 6
//   world.fb        exists (if the manifest's `world` key is present)
//   vpath.idx       header line, and a line count equal to the manifest's
//                   `assets.index_entries`
//   asset store     `assets.idx` and its blob, or `assets/` (the manifest's
//                   `store` key)
//
// A refusal leaves the pack closed; `get_error()` and the error log give one
// sentence with the numbers. Unknown versions are always refused. A `records`
// key or a `records.fb` from an earlier converter is ignored.
//
// Assets load straight from the pack (assets/asset_cache.hpp): `load_scene`
// for a mesh, `load_texture` for a DDS, `get_bytes` for anything; no import
// step. `open_world` reads world.fb (cells, references, bases) and shares the
// cache.
#pragma once

#include "assets/asset_cache.hpp"
#include "assets/pack_store.hpp"
#include "world/world.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace skydot {

class SkydotPack : public godot::RefCounted {
    GDCLASS(SkydotPack, godot::RefCounted)

public:
    /// The pack format version this engine reads (manifest and vpath.idx).
    static constexpr int PACK_FORMAT_VERSION = 6;

    /// Mount a pack directory. On refusal the pack stays closed, `get_error`
    /// explains, and the return code gives the category.
    godot::Error open(const godot::String& pack_dir);
    void close();
    bool is_open() const;

    godot::String get_path() const;
    godot::String get_error() const;

    /// From manifest.json, so available without opening the files.
    bool has_world() const;
    /// The pack's world.fb, opened and verified; null (with `get_error`) if
    /// the pack has none or it is damaged.
    godot::Ref<SkydotWorld> open_world();
    std::int64_t get_asset_count() const;
    std::int64_t get_index_count() const;
    /// vpath.idx lines with a kind this version does not know (from a newer
    /// converter). Counted, not dropped.
    std::int64_t get_unknown_kind_count() const;

    /// Whether the index has the path. Accepts any spelling (see
    /// `normalize_vpath`).
    bool has(const godot::String& vpath) const;
    /// The asset's bytes as stored (GLB, DDS, decoded script or LOD data);
    /// empty if absent.
    godot::PackedByteArray get_bytes(const godot::String& vpath) const;
    /// A mesh, built from its GLB; null if absent. Cached; `instantiate` it.
    godot::Ref<SkydotModel> load_scene(const godot::String& vpath) const;
    /// A texture (Cubemap for cube maps); null if absent. Cached.
    godot::Ref<godot::Texture> load_texture(const godot::String& vpath) const;
    /// "blob" or "loose".
    godot::String get_store_layout() const;

    /// "mesh", "texture", "script", "lod", the unknown word as written, or "" if
    /// absent.
    godot::String get_kind(const godot::String& vpath) const;
    godot::String get_hash(const godot::String& vpath) const;
    godot::String get_source(const godot::String& vpath) const;

    /// The vpath.idx spelling (assets/vpath.hpp): ASCII lowercase, forward
    /// slashes, no leading, repeated or trailing ones, for paths from record
    /// fields with backslashes and arbitrary case. Letters above ASCII keep
    /// their case, as in the index.
    static godot::String normalize_vpath(const godot::String& path);

    /// Virtual path for a MODL value. Model paths are relative to
    /// `Data\meshes\` (UESP, MODL), but some plugins include the prefix; such
    /// paths are left alone.
    static godot::String model_vpath(const godot::String& modl);

    /// The shared asset cache; null while closed.
    std::shared_ptr<AssetCache> assets() const { return assets_; }

protected:
    static void _bind_methods();

private:
    /// Record and log a failure; `refuse` additionally closes the pack.
    godot::Error report(godot::Error code, const godot::String& why);
    godot::Error refuse(godot::Error code, const godot::String& why);
    godot::Error read_manifest(const godot::String& path);
    godot::Error read_index(const godot::String& path);
    godot::Error open_store();

    const PackStore::Entry* find(const godot::String& vpath) const;

    godot::String path_;
    godot::String error_;
    bool open_{false};

    bool has_world_{false};
    std::int64_t asset_count_{0};
    std::int64_t index_count_{0};
    std::int64_t unknown_kind_count_{0};

    godot::String layout_;     ///< "blob" or "loose", from the manifest.
    godot::String blob_name_;  ///< For the blob layout.
    std::shared_ptr<PackStore> store_;
    std::shared_ptr<AssetCache> assets_;
};

} // namespace skydot
