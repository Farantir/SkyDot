// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/pack_view.hpp"

#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/byte_writer.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_reader.hpp"
#include "bethconv/io/span_stream.hpp"
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/pack/asset_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bethconv::pack {
namespace {

using nlohmann::ordered_json;

/// glTF 2.0 §3.2: magic/version/length header, JSON chunk first.
constexpr io::FourCC k_glb_magic{"glTF"};
constexpr io::FourCC k_chunk_json{"JSON"};
constexpr std::uint32_t k_glb_version = 2;

[[nodiscard]] std::unexpected<io::ParseError> view_error(std::string_view origin,
                                                         io::ErrorKind kind, std::string detail) {
    return std::unexpected(io::ParseError{
        .origin = std::string(origin), .offset = 0, .kind = kind, .detail = std::move(detail)});
}

[[nodiscard]] int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// Inverse of `uri_escape` in the glTF writer. Invalid escapes are kept as
/// literal text: the result is only used for lookup, and a failed lookup is
/// already counted as dangling.
[[nodiscard]] std::string uri_unescape(std::string_view uri) {
    std::string out;
    out.reserve(uri.size());
    for (std::size_t i = 0; i < uri.size(); ++i) {
        const char c = uri[i];
        if (c != '%' || i + 2 >= uri.size()) {
            out.push_back(c);
            continue;
        }
        const int hi = hex_value(uri[i + 1]);
        const int lo = hex_value(uri[i + 2]);
        if (hi < 0 || lo < 0) {
            out.push_back(c);
            continue;
        }
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return out;
}

/// True for URIs the view must not rewrite: `data:` (no file), rooted `/`,
/// absolute `scheme:`, and `../` (already corrected by
/// `TextureRefs::source_paths`).
[[nodiscard]] bool leave_alone(std::string_view uri) noexcept {
    if (uri.empty() || uri.front() == '/' || uri.starts_with("../") || uri.starts_with("./")) {
        return true;
    }
    // A scheme cannot contain `/`, so a colon after the first slash is part of
    // a path (uri_escape never emits one, but hand-made packs might).
    const auto colon = uri.find(':');
    return colon != std::string_view::npos && colon < uri.find('/');
}

/// The view path for an entry: its virtual path with the converted extension.
[[nodiscard]] std::string view_relative_path(const VpathEntry& entry) {
    std::string out = entry.vpath;
    const auto slash = out.find_last_of('/');
    const auto dot = out.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        out.resize(dot);
    }
    out += extension_of(entry.kind);
    return out;
}

} // namespace

std::string ascent_prefix(std::string_view vpath) {
    std::string out;
    for (const char c : vpath) {
        if (c == '/' || c == '\\') {
            out += "../";
        }
    }
    return out;
}

namespace {

/// Give each material the glTF textures its extras name, as the writer does
/// for `TextureRefs::source_paths`: slot 0 as base colour, 1 as normal map
/// (unless model-space), 2 as emissive when it is a glow map.
void add_images(ordered_json& doc, std::string_view prefix, ViewGlb& out) {
    const auto materials = doc.find("materials");
    if (materials == doc.end() || !materials->is_array()) {
        return;
    }
    std::unordered_map<std::string, std::size_t> texture_of;
    auto images = ordered_json::array();
    auto textures = ordered_json::array();
    const auto texture_for = [&](const std::string& vpath) {
        if (const auto found = texture_of.find(vpath); found != texture_of.end()) {
            return found->second;
        }
        images.push_back(ordered_json{{"uri", std::string(prefix) + mesh::escape_texture_uri(vpath)},
                                      {"name", vpath}});
        textures.push_back(ordered_json{{"source", images.size() - 1}, {"name", vpath}});
        out.image_vpaths.push_back(vpath);
        ++out.uris;
        texture_of.emplace(vpath, textures.size() - 1);
        return textures.size() - 1;
    };

    for (auto& material : *materials) {
        if (!material.is_object() || !material.contains("extras") ||
            !material["extras"].is_object() || !material["extras"].contains("bethconv")) {
            continue;
        }
        // Read everything first: adding keys to the material may move its
        // extras (ordered_json keeps members in a vector).
        std::string base;
        std::string normal;
        std::string glow;
        {
            const auto& bethconv = material["extras"]["bethconv"];
            if (!bethconv.is_object() || !bethconv.contains("texture_slots") ||
                !bethconv["texture_slots"].is_object()) {
                continue;
            }
            const auto& slots = bethconv["texture_slots"];
            const auto slot = [&](const char* key) -> std::string {
                if (!slots.contains(key) || !slots[key].is_object() ||
                    !slots[key].contains("path") || !slots[key]["path"].is_string()) {
                    return {};
                }
                return slots[key]["path"].get<std::string>();
            };
            const auto flag = [&](const char* key) {
                return bethconv.contains(key) && bethconv[key].is_boolean() &&
                       bethconv[key].get<bool>();
            };
            base = slot("0");
            if (!flag("model_space_normals")) {
                normal = slot("1");
            }
            if (flag("has_glowmap")) {
                glow = slot("2");
            }
        }
        if (!base.empty()) {
            material["pbrMetallicRoughness"]["baseColorTexture"] =
                ordered_json{{"index", texture_for(base)}};
        }
        if (!normal.empty()) {
            material["normalTexture"] = ordered_json{{"index", texture_for(normal)}};
        }
        if (!glow.empty()) {
            material["emissiveTexture"] = ordered_json{{"index", texture_for(glow)}};
        }
    }
    if (!images.empty()) {
        doc["images"] = std::move(images);
        doc["textures"] = std::move(textures);
    }
}

} // namespace

io::ParseResult<ViewGlb> view_glb(std::span<const std::byte> glb, std::string_view prefix,
                                  std::string_view origin) {
    io::SpanReader reader(glb, origin);

    const auto magic = reader.tag();
    if (!magic) {
        return std::unexpected(magic.error());
    }
    if (*magic != k_glb_magic) {
        return view_error(origin, io::ErrorKind::bad_magic,
                          "not a GLB: leading tag is '" + magic->to_string() + "'");
    }
    const auto version = reader.get<std::uint32_t>();
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != k_glb_version) {
        return view_error(origin, io::ErrorKind::unsupported,
                          "GLB container version " + std::to_string(*version) +
                              ", and glTF 2.0 defines only 2");
    }
    if (const auto total = reader.get<std::uint32_t>(); !total) {
        return std::unexpected(total.error());
    }

    // glTF 2.0 §3.3: the JSON chunk comes first and is mandatory. Later chunks
    // (BIN, extensions) are copied unchanged.
    const auto json_length = reader.get<std::uint32_t>();
    if (!json_length) {
        return std::unexpected(json_length.error());
    }
    const auto json_type = reader.tag();
    if (!json_type) {
        return std::unexpected(json_type.error());
    }
    if (*json_type != k_chunk_json) {
        return view_error(origin, io::ErrorKind::bad_value,
                          "first GLB chunk is '" + json_type->to_string() + "', not JSON");
    }
    const auto json_text = reader.chars(*json_length);
    if (!json_text) {
        return std::unexpected(json_text.error());
    }
    const auto tail = reader.bytes(reader.remaining());
    if (!tail) {
        return std::unexpected(tail.error());
    }

    auto doc = ordered_json::parse(*json_text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        return view_error(origin, io::ErrorKind::corrupt,
                          "the GLB's JSON chunk does not parse as a JSON object");
    }

    ViewGlb out;
    std::unordered_set<std::string> seen;
    const auto images = doc.find("images");
    const bool has_images = images != doc.end() && images->is_array() && !images->empty();
    if (!has_images) {
        add_images(doc, prefix, out);
        seen.insert(out.image_vpaths.begin(), out.image_vpaths.end());
    } else {
        for (auto& image : *images) {
            if (!image.is_object()) {
                continue;
            }
            const auto uri = image.find("uri");
            if (uri == image.end() || !uri->is_string()) {
                continue;
            }
            const auto text = uri->get<std::string>();
            if (leave_alone(text)) {
                continue;
            }
            std::string vpath = uri_unescape(text);
            if (seen.insert(vpath).second) {
                out.image_vpaths.push_back(std::move(vpath));
            }
            if (!prefix.empty()) {
                *uri = std::string(prefix) + text;
                ++out.uris;
            }
        }
    }

    // Every texture slot in the materials' extras.
    if (const auto materials = doc.find("materials");
        materials != doc.end() && materials->is_array()) {
        std::unordered_set<std::string> slot_seen;
        for (const auto& material : *materials) {
            const auto* slots = material.contains("extras") && material["extras"].contains("bethconv")
                                    ? &material["extras"]["bethconv"]
                                    : nullptr;
            if (slots == nullptr || !slots->contains("texture_slots") ||
                !(*slots)["texture_slots"].is_object()) {
                continue;
            }
            for (const auto& [slot, value] : (*slots)["texture_slots"].items()) {
                if (!value.is_object() || !value.contains("path") || !value["path"].is_string()) {
                    continue;
                }
                auto vpath = value["path"].get<std::string>();
                if (!vpath.empty() && !seen.contains(vpath) && slot_seen.insert(vpath).second) {
                    out.slot_vpaths.push_back(std::move(vpath));
                }
            }
        }
    }

    std::string rewritten;
    try {
        rewritten = doc.dump();
    } catch (const std::exception& e) {
        // dump() throws on invalid UTF-8. Our own output cannot contain it, but a
        // pack directory may have been edited, so convert to a ParseError.
        return view_error(origin, io::ErrorKind::corrupt,
                          std::string("cannot re-serialize the GLB's JSON chunk: ") + e.what());
    }
    // §3.3: chunks are 4-byte aligned; JSON pads with spaces.
    while (rewritten.size() % 4 != 0) {
        rewritten.push_back(' ');
    }

    io::ByteWriter writer;
    const std::uint32_t chunk_bytes = static_cast<std::uint32_t>(rewritten.size());
    const std::size_t total = 12 + 8 + rewritten.size() + tail->size();
    writer.reserve_more(total);
    writer.put(k_glb_magic.value);
    writer.put(k_glb_version);
    writer.put(static_cast<std::uint32_t>(total));
    writer.put(chunk_bytes);
    writer.put(k_chunk_json.value);
    writer.put_bytes(std::as_bytes(std::span<const char>(rewritten)));
    writer.put_bytes(*tail);
    out.bytes = writer.take();
    return out;
}

io::ParseResult<ViewResult> materialize_view(const std::filesystem::path& pack,
                                             const ViewOptions& options) {
    auto index = VpathIndex::read(pack / "vpath.idx");
    if (!index) {
        return std::unexpected(index.error());
    }
    if (index->format_version() > k_pack_format_version) {
        return view_error((pack / "vpath.idx").string(), io::ErrorKind::unsupported,
                          "pack format v" + std::to_string(index->format_version()) +
                              ", and this converter reads v" +
                              std::to_string(k_pack_format_version));
    }

    auto assets = AssetReader::open(pack);
    if (!assets) {
        return std::unexpected(assets.error());
    }

    std::error_code ec;
    std::filesystem::create_directories(options.out, ec);
    if (ec) {
        return view_error(options.out.string(), io::ErrorKind::corrupt,
                          "cannot create the view directory: " + ec.message());
    }

    ViewResult result;
    result.stats.index_entries = index->entries().size();

    // Entries to materialize. The index is sorted, so --limit is stable.
    std::vector<const VpathEntry*> work;
    std::unordered_set<std::string> only;
    for (const auto& vpath : options.only) {
        only.insert(archive::normalize_vpath(vpath));
    }
    for (const auto& entry : index->entries()) {
        if (!options.filter.empty() && entry.vpath.find(options.filter) == std::string::npos) {
            continue;
        }
        if (!only.empty() && !only.contains(entry.vpath)) {
            continue;
        }
        if (options.limit != 0 && work.size() >= options.limit) {
            break;
        }
        work.push_back(&entry);
    }
    result.stats.considered = work.size();

    const auto fail = [&result](const VpathEntry& entry, std::string stage, io::ErrorKind kind,
                                std::string detail) {
        ++result.stats.failed;
        result.failures.push_back(PackFailure{.vpath = entry.vpath,
                                              .stage = std::move(stage),
                                              .kind = std::string(io::to_string(kind)),
                                              .detail = std::move(detail)});
    };

    /// Write `bytes` at `relative` in the view.
    const auto write = [&](const VpathEntry& entry, std::span<const std::byte> bytes,
                           const std::string& relative) -> bool {
        const auto to = options.out / std::filesystem::path(relative);
        std::string error;
        if (!io::write_file(to, bytes, error)) {
            fail(entry, "write", io::ErrorKind::corrupt, std::move(error));
            return false;
        }
        ++result.stats.copied;
        result.stats.bytes += bytes.size();
        return true;
    };

    /// Place one file at `relative` in the view, by link or copy.
    const auto place = [&](const VpathEntry& entry, const std::filesystem::path& from,
                           const std::string& relative) -> bool {
        const auto to = options.out / std::filesystem::path(relative);
        std::error_code inner;
        std::filesystem::create_directories(to.parent_path(), inner);
        if (inner) {
            fail(entry, "write", io::ErrorKind::corrupt,
                 "cannot create " + to.parent_path().string() + ": " + inner.message());
            return false;
        }
        if (!options.copy_assets) {
            // Already the same file (common when a view is rebuilt). One stat
            // is much cheaper than unlink + relink, especially on FUSE.
            std::error_code same;
            if (std::filesystem::equivalent(from, to, same) && !same) {
                ++result.stats.linked;
                return true;
            }
        }

        // create_hard_link does not overwrite, and copy_file would follow a
        // stale link.
        std::filesystem::remove(to, inner);

        if (!options.copy_assets) {
            inner.clear();
            std::filesystem::create_hard_link(from, to, inner);
            if (!inner) {
                ++result.stats.linked;
                return true;
            }
            // Fall back to copying (different device, no hard-link support).
        }
        inner.clear();
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing,
                                   inner);
        if (inner) {
            fail(entry, "write", io::ErrorKind::corrupt,
                 "cannot place " + to.string() + ": " + inner.message());
            return false;
        }
        ++result.stats.copied;
        result.stats.bytes += std::filesystem::file_size(to, inner);
        return true;
    };

    /// A stored asset into the view: linked from a loose pack, written from a
    /// blob.
    const auto place_asset = [&](const VpathEntry& entry, const std::string& relative) -> bool {
        if (const auto path = assets->path_of(entry)) {
            return place(entry, *path, relative);
        }
        auto stored = assets->read(entry);
        if (!stored) {
            fail(entry, "read", stored.error().kind, stored.error().detail);
            return false;
        }
        return write(entry, stored->data, relative);
    };

    const auto report = [&options](const char* phase, std::uint64_t done, std::uint64_t total) {
        if (options.progress) {
            options.progress(phase, done, total);
        }
    };
    // Treat 0 as "report every item" instead of dividing by zero.
    const std::uint64_t interval = options.progress_interval == 0 ? 1 : options.progress_interval;

    // ---- meshes -------------------------------------------------------
    // First, because they determine which textures the view needs.
    std::unordered_set<std::string> wanted;   ///< Texture vpaths meshes referenced.
    std::unordered_set<std::string> placed;   ///< View paths already written.
    /// content hash + depth -> first view path written for it. Same hash at the
    /// same depth gives identical bytes, so later ones are linked.
    std::unordered_map<std::string, std::string> rewritten_by;

    std::uint64_t done = 0;
    for (const auto* entry : work) {
        ++done;
        if (done % interval == 0) {
            report("meshes", done, work.size());
        }
        if (entry->kind != AssetKind::mesh) {
            continue;
        }
        if (!archive::is_safe_relative(entry->vpath)) {
            fail(*entry, "read", io::ErrorKind::bad_value,
                 "virtual path is not a safe relative path");
            continue;
        }
        const auto relative = view_relative_path(*entry);
        if (!placed.insert(relative).second) {
            continue;
        }
        const auto prefix = ascent_prefix(entry->vpath);

        const std::string key = entry->hex + ":" + std::to_string(prefix.size());
        if (const auto seen = rewritten_by.find(key); seen != rewritten_by.end()) {
            if (place(*entry, options.out / std::filesystem::path(seen->second), relative)) {
                ++result.stats.meshes;
                ++result.stats.mesh_links;
            }
            continue;
        }

        auto stored = assets->read(*entry);
        if (!stored) {
            fail(*entry, "read", stored.error().kind, stored.error().detail);
            continue;
        }
        auto rebased = view_glb(stored->data, prefix, entry->vpath);
        if (!rebased) {
            fail(*entry, "rebase", rebased.error().kind, rebased.error().detail);
            continue;
        }

        const auto to = options.out / std::filesystem::path(relative);
        std::string error;
        if (!io::write_file(to, rebased->bytes, error)) {
            fail(*entry, "write", io::ErrorKind::corrupt, std::move(error));
            continue;
        }
        ++result.stats.meshes;
        ++result.stats.written;
        result.stats.bytes += rebased->bytes.size();
        rewritten_by.emplace(key, relative);

        for (const auto& referenced : rebased->image_vpaths) {
            ++result.stats.image_refs;
            if (index->find(referenced) != nullptr) {
                ++result.stats.image_refs_resolved;
                wanted.insert(referenced);
            } else {
                ++result.stats.image_refs_dangling;
            }
        }
        // Textures only named in extras are included when the pack has them;
        // they are not counted as image references.
        for (const auto& referenced : rebased->slot_vpaths) {
            if (index->find(referenced) != nullptr) {
                wanted.insert(referenced);
            }
        }
    }
    report("meshes", work.size(), work.size());

    // ---- textures, scripts and LOD data -------------------------------
    done = 0;
    for (const auto* entry : work) {
        ++done;
        if (done % interval == 0) {
            report("assets", done, work.size());
        }
        if (entry->kind == AssetKind::mesh) {
            continue;
        }
        if (!archive::is_safe_relative(entry->vpath)) {
            fail(*entry, "read", io::ErrorKind::bad_value,
                 "virtual path is not a safe relative path");
            continue;
        }
        const auto relative = view_relative_path(*entry);
        if (!placed.insert(relative).second) {
            continue;
        }
        if (!place_asset(*entry, relative)) {
            continue;
        }
        if (entry->kind == AssetKind::texture) {
            ++result.stats.textures;
            wanted.erase(entry->vpath);
        } else if (entry->kind == AssetKind::lod) {
            ++result.stats.lod;
        } else if (entry->kind == AssetKind::animation) {
            ++result.stats.animations;
        } else {
            ++result.stats.scripts;
        }
    }
    report("assets", work.size(), work.size());

    // ---- textures the filter excluded ----------------------------------
    // Textures meshes asked for that were not placed above; empty without a
    // filter.
    std::vector<std::string> pull(wanted.begin(), wanted.end());
    std::ranges::sort(pull);
    done = 0;
    for (const auto& vpath : pull) {
        ++done;
        if (done % interval == 0) {
            report("textures", done, pull.size());
        }
        const auto* entry = index->find(vpath);
        if (entry == nullptr || !archive::is_safe_relative(entry->vpath)) {
            continue;
        }
        const auto relative = view_relative_path(*entry);
        if (!placed.insert(relative).second) {
            continue;
        }
        if (!place_asset(*entry, relative)) {
            continue;
        }
        ++result.stats.textures;
        ++result.stats.pulled_in;
    }
    report("textures", pull.size(), pull.size());

    std::ranges::sort(result.failures, [](const PackFailure& a, const PackFailure& b) {
        return a.vpath != b.vpath ? a.vpath < b.vpath : a.stage < b.stage;
    });
    return result;
}

} // namespace bethconv::pack
