// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotPack`: a mounted bethconv pack.
//
// Packs are the only thing this engine reads. Written against bethconv's
// formats/pack-format.md (v4) alone, without linking the converter's reader, so it
// also shows the document is sufficient.
//
// `open` checks, in order, and refuses on any failure:
//
//   manifest.json   `pack_format_version` must be 4
//   records.fb      64-byte header, magic and version (if the manifest's
//                   `records` key is present)
//   world.fb        exists (if the manifest's `world` key is present)
//   vpath.idx       header line, and a line count equal to the manifest's
//                   `assets.index_entries`
//
// A refusal leaves the pack closed; `get_error()` and the error log give one
// sentence with the numbers. Unknown versions are always refused.
//
// `resolve` maps a virtual path to its file via `vpath.idx`. Baked `.pck`
// scenes use the same virtual path (`meshes/clutter/apple01.nif` ->
// `res://meshes/clutter/apple01.scn`), so `scene_path_for` is computed, not
// looked up. `open_world` reads world.fb (cells, references, bases). Reading
// `records.fb` beyond its header is not implemented yet.
#pragma once

#include "world/world.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace skydot {

class SkydotPack : public godot::RefCounted {
    GDCLASS(SkydotPack, godot::RefCounted)

public:
    /// The pack format version this engine reads (manifest and vpath.idx).
    static constexpr int PACK_FORMAT_VERSION = 4;
    /// The records.fb format version this engine reads (its own number, not the
    /// pack's).
    static constexpr int RECORDS_FORMAT_VERSION = 1;

    /// Mount a pack directory. On refusal the pack stays closed, `get_error`
    /// explains, and the return code gives the category.
    godot::Error open(const godot::String& pack_dir);
    void close();
    bool is_open() const;

    godot::String get_path() const;
    godot::String get_error() const;

    /// From manifest.json, so available without opening the files.
    bool has_records() const;
    bool has_world() const;
    /// The pack's world.fb, opened and verified; null (with `get_error`) if
    /// the pack has none or it is damaged.
    godot::Ref<SkydotWorld> open_world();
    std::int64_t get_form_count() const;
    std::int64_t get_asset_count() const;
    std::int64_t get_index_count() const;
    /// vpath.idx lines with a kind this version does not know (from a newer
    /// converter). Counted, not dropped.
    std::int64_t get_unknown_kind_count() const;

    /// Path on disk of the asset for a virtual path, or "" if the index has no
    /// such line or its kind is unknown. Accepts any spelling (see
    /// `normalize_vpath`).
    godot::String resolve(const godot::String& vpath) const;
    /// "mesh", "texture", "script", "lod", the unknown word as written, or "" if
    /// absent.
    godot::String get_kind(const godot::String& vpath) const;
    godot::String get_hash(const godot::String& vpath) const;
    godot::String get_source(const godot::String& vpath) const;

    /// Lowercase, forward slashes, no leading slash (vpath.idx spelling), for
    /// paths from record fields with backslashes and arbitrary case.
    static godot::String normalize_vpath(const godot::String& path);

    /// Virtual path for a MODL value. Model paths are relative to
    /// `Data\meshes\` (UESP, MODL), but some plugins include the prefix; such
    /// paths are left alone.
    static godot::String model_vpath(const godot::String& modl);

    /// Scene path the bake uses for a mesh:
    /// `meshes/clutter/apple01.nif` -> `res://meshes/clutter/apple01.scn`.
    /// Computed; does not check that a bake exists.
    static godot::String scene_path_for(const godot::String& vpath);

    /// Mount a baked `.pck` over `res://`, so scripts mount pack and bake
    /// through one object.
    godot::Error mount_baked(const godot::String& pck_path);

protected:
    static void _bind_methods();

private:
    struct Entry {
        std::string hash;    ///< 64 lowercase hex characters.
        std::string kind;    ///< As written; never empty.
        std::string source;  ///< Archive or folder that won this path.
    };

    /// Record and log a failure; `refuse` additionally closes the pack.
    godot::Error report(godot::Error code, const godot::String& why);
    godot::Error refuse(godot::Error code, const godot::String& why);
    godot::Error read_manifest(const godot::String& path);
    godot::Error read_records_header(const godot::String& path);
    godot::Error read_index(const godot::String& path);

    const Entry* find(const godot::String& vpath) const;

    godot::String path_;
    godot::String error_;
    bool open_{false};

    bool has_records_{false};
    bool has_world_{false};
    std::int64_t form_count_{0};
    std::int64_t asset_count_{0};
    std::int64_t index_count_{0};
    std::int64_t unknown_kind_count_{0};

    std::unordered_map<std::string, Entry> index_;
};

} // namespace skydot
