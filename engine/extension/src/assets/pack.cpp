// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets/pack.hpp"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string_view>

using godot::Dictionary;
using godot::Error;
using godot::FileAccess;
using godot::PackedByteArray;
using godot::PackedStringArray;
using godot::String;
using godot::Variant;

namespace skydot {

namespace {

// formats/pack-format.md, "The directory".
constexpr const char* k_manifest_name = "manifest.json";
constexpr const char* k_index_name = "vpath.idx";

// formats/pack-format.md, "vpath.idx": two comment lines, the first naming format
// and version.
constexpr const char* k_index_header = "# bethconv vpath index v4";

// formats/pack-format.md, "records.fb" > "The header": little-endian, 64 bytes,
// magic `BETHSNAP`, then `format_version` (uint32). Only enough is read to
// check the version.
constexpr std::string_view k_snapshot_magic = "BETHSNAP";
constexpr std::int64_t k_snapshot_header_size = 64;
constexpr std::int64_t k_snapshot_version_offset = 8;

// formats/pack-format.md, "assets/ and the content hash":
// `assets/<first two hex>/<hex><ext>`, one extension per kind.
std::string_view extension_of(std::string_view kind) {
    if (kind == "mesh") {
        return ".glb";
    }
    if (kind == "texture") {
        return ".dds";
    }
    if (kind == "script") {
        return ".pexfb";
    }
    if (kind == "lod") {
        return ".lodfb";
    }
    return {};
}

std::string to_std(const String& s) {
    return std::string(s.utf8().get_data());
}

String to_godot(std::string_view s) {
    return String::utf8(s.data(), static_cast<int>(s.size()));
}

// A size as a String; Godot's num_uint64 takes a signed argument.
String count(std::size_t n) {
    return String::num_int64(static_cast<std::int64_t>(n));
}

// A JSON number as an integer. Godot parses all JSON numbers as floats, which
// is exact for manifest counts.
bool read_count(const Dictionary& object, const String& key, std::int64_t& out) {
    if (!object.has(key)) {
        return false;
    }
    const Variant value = object[key];
    if (value.get_type() != Variant::FLOAT && value.get_type() != Variant::INT) {
        return false;
    }
    out = static_cast<std::int64_t>(value);
    return true;
}

} // namespace

// ---- binding --------------------------------------------------------------

void SkydotPack::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;

    BIND_CONSTANT(PACK_FORMAT_VERSION);
    BIND_CONSTANT(RECORDS_FORMAT_VERSION);

    ClassDB::bind_method(D_METHOD("open", "pack_dir"), &SkydotPack::open);
    ClassDB::bind_method(D_METHOD("close"), &SkydotPack::close);
    ClassDB::bind_method(D_METHOD("is_open"), &SkydotPack::is_open);
    ClassDB::bind_method(D_METHOD("get_path"), &SkydotPack::get_path);
    ClassDB::bind_method(D_METHOD("get_error"), &SkydotPack::get_error);

    ClassDB::bind_method(D_METHOD("has_records"), &SkydotPack::has_records);
    ClassDB::bind_method(D_METHOD("has_world"), &SkydotPack::has_world);
    ClassDB::bind_method(D_METHOD("open_world"), &SkydotPack::open_world);
    ClassDB::bind_method(D_METHOD("get_form_count"), &SkydotPack::get_form_count);
    ClassDB::bind_method(D_METHOD("get_asset_count"), &SkydotPack::get_asset_count);
    ClassDB::bind_method(D_METHOD("get_index_count"), &SkydotPack::get_index_count);
    ClassDB::bind_method(D_METHOD("get_unknown_kind_count"), &SkydotPack::get_unknown_kind_count);

    ClassDB::bind_method(D_METHOD("resolve", "vpath"), &SkydotPack::resolve);
    ClassDB::bind_method(D_METHOD("get_kind", "vpath"), &SkydotPack::get_kind);
    ClassDB::bind_method(D_METHOD("get_hash", "vpath"), &SkydotPack::get_hash);
    ClassDB::bind_method(D_METHOD("get_source", "vpath"), &SkydotPack::get_source);

    ClassDB::bind_static_method("SkydotPack", D_METHOD("normalize_vpath", "path"),
                                &SkydotPack::normalize_vpath);
    ClassDB::bind_static_method("SkydotPack", D_METHOD("model_vpath", "modl"),
                                &SkydotPack::model_vpath);
    ClassDB::bind_static_method("SkydotPack", D_METHOD("scene_path_for", "vpath"),
                                &SkydotPack::scene_path_for);

    ClassDB::bind_method(D_METHOD("mount_baked", "pck_path"), &SkydotPack::mount_baked);
}

// ---- mounting -------------------------------------------------------------

Error SkydotPack::report(Error code, const String& why) {
    error_ = why;
    godot::UtilityFunctions::push_error("SkydotPack: ", why);
    return code;
}

Error SkydotPack::refuse(Error code, const String& why) {
    close();
    return report(code, why);
}

Error SkydotPack::open(const String& pack_dir) {
    close();
    path_ = pack_dir.simplify_path();

    if (!godot::DirAccess::dir_exists_absolute(path_)) {
        return refuse(Error::ERR_FILE_NOT_FOUND, "no directory at " + path_);
    }

    if (const Error e = read_manifest(path_.path_join(k_manifest_name)); e != Error::OK) {
        return e;
    }
    if (has_records_) {
        if (const Error e = read_records_header(path_.path_join("records.fb")); e != Error::OK) {
            return e;
        }
    }
    if (has_world_ && !FileAccess::file_exists(path_.path_join("world.fb"))) {
        return refuse(Error::ERR_FILE_NOT_FOUND, "manifest.json names world.fb, which is missing");
    }
    if (const Error e = read_index(path_.path_join(k_index_name)); e != Error::OK) {
        return e;
    }

    open_ = true;
    error_ = String();
    return Error::OK;
}

void SkydotPack::close() {
    open_ = false;
    has_records_ = false;
    has_world_ = false;
    form_count_ = 0;
    asset_count_ = 0;
    index_count_ = 0;
    unknown_kind_count_ = 0;
    index_.clear();
    // path_ and error_ stay, so the refusal can still be queried.
}

Error SkydotPack::read_manifest(const String& path) {
    if (!FileAccess::file_exists(path)) {
        return refuse(Error::ERR_FILE_NOT_FOUND, "not a pack: no manifest.json in " + path_);
    }

    const Variant parsed = godot::JSON::parse_string(FileAccess::get_file_as_string(path));
    if (parsed.get_type() != Variant::DICTIONARY) {
        return refuse(Error::ERR_FILE_CORRUPT, "manifest.json is not a JSON object: " + path);
    }
    const Dictionary manifest = parsed;

    // formats/pack-format.md, "manifest.json": `pack_format_version` first.
    std::int64_t version = 0;
    if (!read_count(manifest, "pack_format_version", version)) {
        return refuse(Error::ERR_FILE_UNRECOGNIZED,
                      "manifest.json carries no pack_format_version; this engine reads v"
                          + String::num_int64(PACK_FORMAT_VERSION));
    }
    if (version != PACK_FORMAT_VERSION) {
        return refuse(Error::ERR_UNAVAILABLE,
                      "pack format version " + String::num_int64(version)
                          + " is not one this engine reads (it reads v"
                          + String::num_int64(PACK_FORMAT_VERSION) + "): " + path_);
    }

    // formats/pack-format.md: the `records` key decides; if present, the file it
    // names must exist.
    has_records_ = manifest.has("records");
    if (has_records_) {
        const Variant records = manifest["records"];
        if (records.get_type() != Variant::DICTIONARY
            || !read_count(Dictionary(records), "forms", form_count_)) {
            return refuse(Error::ERR_FILE_CORRUPT,
                          "manifest.json names records but not how many forms they hold");
        }
    }

    // The `world` key, like `records`, decides whether world.fb exists.
    has_world_ = manifest.has("world");

    if (!manifest.has("assets") || Variant(manifest["assets"]).get_type() != Variant::DICTIONARY) {
        return refuse(Error::ERR_FILE_CORRUPT, "manifest.json has no assets object");
    }
    const Dictionary assets = manifest["assets"];
    if (!read_count(assets, "distinct", asset_count_)
        || !read_count(assets, "index_entries", index_count_)) {
        return refuse(Error::ERR_FILE_CORRUPT,
                      "manifest.json's assets object lacks distinct or index_entries");
    }
    return Error::OK;
}

Error SkydotPack::read_records_header(const String& path) {
    const godot::Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (file.is_null()) {
        return refuse(Error::ERR_FILE_NOT_FOUND,
                      "manifest.json names records.fb but it is not there: " + path);
    }
    const PackedByteArray header = file->get_buffer(k_snapshot_header_size);
    if (header.size() < k_snapshot_header_size) {
        return refuse(Error::ERR_FILE_CORRUPT,
                      "records.fb is " + String::num_int64(header.size())
                          + " bytes, shorter than its own header");
    }

    for (std::int64_t i = 0; i < static_cast<std::int64_t>(k_snapshot_magic.size()); ++i) {
        if (header[i] != static_cast<std::uint8_t>(k_snapshot_magic[static_cast<std::size_t>(i)])) {
            return refuse(Error::ERR_FILE_UNRECOGNIZED, "records.fb does not start with BETHSNAP");
        }
    }
    const std::int64_t version = header.decode_u32(k_snapshot_version_offset);
    if (version != RECORDS_FORMAT_VERSION) {
        return refuse(Error::ERR_UNAVAILABLE,
                      "records.fb format version " + String::num_int64(version)
                          + " is not one this engine reads (it reads v"
                          + String::num_int64(RECORDS_FORMAT_VERSION) + "): " + path);
    }
    return Error::OK;
}

Error SkydotPack::read_index(const String& path) {
    if (!FileAccess::file_exists(path)) {
        return refuse(Error::ERR_FILE_NOT_FOUND, "not a pack: no vpath.idx in " + path_);
    }

    // Read at once; about 20 MB for vanilla.
    const std::string text = to_std(FileAccess::get_file_as_string(path));

    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string_view line(text.data() + pos, end - pos);
        pos = end + 1;
        ++line_no;

        if (line_no == 1) {
            if (line != k_index_header) {
                return refuse(Error::ERR_FILE_UNRECOGNIZED,
                              "vpath.idx does not begin with '" + String(k_index_header)
                                  + "': " + path);
            }
            continue;
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        // `virtual path \t content hash \t kind \t winning source`
        const std::size_t t1 = line.find('\t');
        const std::size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
        const std::size_t t3 = t2 == std::string_view::npos ? t2 : line.find('\t', t2 + 1);
        if (t3 == std::string_view::npos) {
            return refuse(Error::ERR_FILE_CORRUPT,
                          "vpath.idx line " + count(line_no)
                              + " does not have four tab-separated fields");
        }
        Entry entry{
            std::string(line.substr(t1 + 1, t2 - t1 - 1)),
            std::string(line.substr(t2 + 1, t3 - t2 - 1)),
            std::string(line.substr(t3 + 1)),
        };
        if (entry.hash.size() != 64) {
            return refuse(Error::ERR_FILE_CORRUPT,
                          "vpath.idx line " + count(line_no)
                              + " carries a content hash of " + count(entry.hash.size())
                              + " characters, not 64");
        }
        if (extension_of(entry.kind).empty()) {
            ++unknown_kind_count_;
        }
        index_.emplace(std::string(line.substr(0, t1)), std::move(entry));
    }

    // Must match the manifest's count; otherwise the pack was edited or
    // truncated.
    if (static_cast<std::int64_t>(index_.size()) != index_count_) {
        return refuse(Error::ERR_FILE_CORRUPT,
                      "vpath.idx has " + count(index_.size())
                          + " entries but manifest.json says " + String::num_int64(index_count_));
    }
    if (unknown_kind_count_ > 0) {
        godot::UtilityFunctions::push_warning(
            "SkydotPack: ", String::num_int64(unknown_kind_count_),
            " vpath.idx line(s) name an asset kind this engine does not know; they were "
            "written by a newer converter and will not resolve: ",
            path_);
    }
    return Error::OK;
}

// ---- queries --------------------------------------------------------------

bool SkydotPack::is_open() const {
    return open_;
}

String SkydotPack::get_path() const {
    return path_;
}

String SkydotPack::get_error() const {
    return error_;
}

bool SkydotPack::has_records() const {
    return has_records_;
}

bool SkydotPack::has_world() const {
    return has_world_;
}

godot::Ref<SkydotWorld> SkydotPack::open_world() {
    if (!open_ || !has_world_) {
        report(Error::ERR_UNAVAILABLE, "this pack has no world.fb");
        return {};
    }
    godot::Ref<SkydotWorld> world;
    world.instantiate();
    if (world->open(path_.path_join("world.fb")) != Error::OK) {
        report(Error::ERR_FILE_CORRUPT, world->get_error());
        return {};
    }
    return world;
}

std::int64_t SkydotPack::get_form_count() const {
    return form_count_;
}

std::int64_t SkydotPack::get_asset_count() const {
    return asset_count_;
}

std::int64_t SkydotPack::get_index_count() const {
    return index_count_;
}

std::int64_t SkydotPack::get_unknown_kind_count() const {
    return unknown_kind_count_;
}

const SkydotPack::Entry* SkydotPack::find(const String& vpath) const {
    if (!open_) {
        return nullptr;
    }
    const auto it = index_.find(to_std(normalize_vpath(vpath)));
    return it == index_.end() ? nullptr : &it->second;
}

String SkydotPack::resolve(const String& vpath) const {
    const Entry* entry = find(vpath);
    if (entry == nullptr) {
        return String();
    }
    const std::string_view ext = extension_of(entry->kind);
    if (ext.empty()) {
        return String();
    }
    const std::string relative =
        "assets/" + entry->hash.substr(0, 2) + "/" + entry->hash + std::string(ext);
    return path_.path_join(to_godot(relative));
}

String SkydotPack::get_kind(const String& vpath) const {
    const Entry* entry = find(vpath);
    return entry == nullptr ? String() : to_godot(entry->kind);
}

String SkydotPack::get_hash(const String& vpath) const {
    const Entry* entry = find(vpath);
    return entry == nullptr ? String() : to_godot(entry->hash);
}

String SkydotPack::get_source(const String& vpath) const {
    const Entry* entry = find(vpath);
    return entry == nullptr ? String() : to_godot(entry->source);
}

// ---- path arithmetic ------------------------------------------------------

String SkydotPack::normalize_vpath(const String& path) {
    String out = path.replace("\\", "/").to_lower();
    while (out.begins_with("/")) {
        out = out.substr(1);
    }
    return out;
}

String SkydotPack::model_vpath(const String& modl) {
    const String normalized = normalize_vpath(modl);
    if (normalized.is_empty() || normalized.begins_with("meshes/")) {
        return normalized;
    }
    return "meshes/" + normalized;
}

String SkydotPack::scene_path_for(const String& vpath) {
    const String normalized = normalize_vpath(vpath);
    if (normalized.is_empty()) {
        return String();
    }
    return "res://" + normalized.get_basename() + ".scn";
}

// ---- the bake -------------------------------------------------------------

Error SkydotPack::mount_baked(const String& pck_path) {
    // A missing bake does not close the pack; packs are complete without one.
    if (!FileAccess::file_exists(pck_path)) {
        return report(Error::ERR_FILE_NOT_FOUND, "no baked pack at " + pck_path);
    }
    if (!godot::ProjectSettings::get_singleton()->load_resource_pack(pck_path, true, 0)) {
        return report(Error::ERR_CANT_OPEN, "Godot could not mount the baked pack at " + pck_path);
    }
    return Error::OK;
}

} // namespace skydot
