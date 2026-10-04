// SPDX-License-Identifier: GPL-3.0-or-later
#include "asset_conversion.hpp"

#include "bethconv/animation/animation_data.hpp"
#include "bethconv/animation/hkx.hpp"
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/mesh/nif_reader.hpp"
#include "bethconv/pack/animation_asset.hpp"
#include "bethconv/pack/lod_asset.hpp"
#include "bethconv/pack/script_asset.hpp"
#include "bethconv/script/pex.hpp"
#include "bethconv/texture/dds.hpp"
#include "bethconv/texture/mip_drop.hpp"
#include "bethconv/texture/mip_tail.hpp"

#include <string>
#include <utility>

namespace bethconv::pack {

PackFailure failure_from(std::string_view vpath, std::string_view stage,
                         const io::ParseError& error) {
    return PackFailure{.vpath = std::string(vpath),
                       .stage = std::string(stage),
                       .kind = std::string(io::to_string(error.kind)),
                       .detail = error.to_string()};
}

AssetConversion convert_mesh(std::span<const std::byte> source, std::string_view vpath,
                             const ConvertOptions& options) {
    AssetConversion out;
    auto model = mesh::read_nif(source, vpath, options.mesh_read);
    if (!model) {
        out.failure = failure_from(vpath, "mesh", model.error());
        return out;
    }
    for (const auto& warning : model->warnings) {
        out.warnings.push_back(PackWarning{.vpath = std::string(vpath), .detail = warning});
    }
    auto glb = mesh::write_glb(*model, options.mesh_write);
    if (!glb) {
        out.failure = failure_from(vpath, "mesh", glb.error());
        return out;
    }
    out.bytes = std::move(*glb);
    return out;
}

AssetConversion convert_texture(std::span<const std::byte> source, std::string_view vpath,
                                const ConvertOptions& options) {
    AssetConversion out;
    const auto fail = [&](const io::ParseError& error) {
        out.failure = failure_from(vpath, "texture", error);
        return std::move(out);
    };

    auto info = texture::parse_dds(source, vpath);
    if (!info) {
        return fail(info.error());
    }
    std::span<const std::byte> payload = source;
    texture::SizeLimit limit;
    if (options.max_texture_size != 0) {
        auto limited = texture::limit_size(source, *info, options.max_texture_size, vpath);
        if (!limited) {
            return fail(limited.error());
        }
        limit = std::move(*limited);
        if (limit.outcome == texture::DropOutcome::shrunk) {
            // The tail fix below works on the smaller file.
            auto smaller = texture::parse_dds(limit.data, vpath);
            if (!smaller) {
                return fail(smaller.error());
            }
            info = std::move(smaller);
            payload = limit.data;
            ++out.textures.shrunk;
            out.textures.bytes_saved += limit.saved_bytes;
        } else if (limit.outcome != texture::DropOutcome::fits) {
            ++out.textures.kept_large;
            out.warnings.push_back(PackWarning{
                .vpath = std::string(vpath),
                .detail = "kept at " + std::to_string(info->width) + "x" +
                          std::to_string(info->height) + ", over the " +
                          std::to_string(options.max_texture_size) + " px limit (" +
                          std::string(texture::to_string(limit.outcome)) + ")"});
        }
    }
    texture::Encoded encoded;
    if (options.texture_encoding != texture::Encoding::keep && !info->layout.block_compressed) {
        // Normal maps by Skyrim's naming: tangent (_n) and model space (_msn).
        const bool normal_map = vpath.ends_with("_n.dds") || vpath.ends_with("_msn.dds");
        auto enc = texture::encode_uncompressed(payload, *info, options.texture_encoding,
                                                normal_map, vpath);
        if (!enc) {
            return fail(enc.error());
        }
        encoded = std::move(*enc);
        if (encoded.outcome == texture::EncodeOutcome::encoded) {
            auto compressed = texture::parse_dds(encoded.data, vpath);
            if (!compressed) {
                return fail(compressed.error());
            }
            out.textures.bytes_saved +=
                payload.size() > encoded.data.size() ? payload.size() - encoded.data.size() : 0;
            info = std::move(compressed);
            payload = encoded.data;
            ++out.textures.encoded;
        } else if (encoded.outcome == texture::EncodeOutcome::unsupported) {
            ++out.textures.not_encoded;
            out.warnings.push_back(PackWarning{.vpath = std::string(vpath),
                                               .detail = "left uncompressed: " + encoded.reason});
        }
    }
    texture::TailFix fix;
    if (options.fix_mip_tail) {
        auto completed = texture::complete_mip_tail(payload, *info, vpath);
        if (!completed) {
            return fail(completed.error());
        }
        fix = std::move(*completed);
        if (fix.outcome == texture::TailOutcome::completed) {
            payload = fix.data;
        }
        if (fix.dropped_bytes != 0) {
            out.warnings.push_back(PackWarning{
                .vpath = std::string(vpath),
                .detail = std::to_string(fix.dropped_bytes) +
                          " bytes past the declared surfaces were dropped"});
        }
    }
    // `payload` is the last edit that applied: the completed chain, else the
    // encoding, else the smaller file, else the source untouched.
    if (fix.outcome == texture::TailOutcome::completed) {
        out.bytes = std::move(fix.data);
    } else if (encoded.outcome == texture::EncodeOutcome::encoded) {
        out.bytes = std::move(encoded.data);
    } else if (limit.outcome == texture::DropOutcome::shrunk) {
        out.bytes = std::move(limit.data);
    } else {
        out.passthrough = true;
    }
    return out;
}

AssetConversion convert_script(std::span<const std::byte> source, std::string_view vpath) {
    AssetConversion out;
    auto info = script::parse_pex(source, vpath);
    if (!info) {
        out.failure = failure_from(vpath, "script", info.error());
        return out;
    }
    if (!info->convertible()) {
        // Valid PEX for another game: reported, not packed.
        out.failure = PackFailure{
            .vpath = std::string(vpath),
            .stage = "script",
            .kind = std::string(io::to_string(io::ErrorKind::unsupported)),
            .detail = "compiled for " + std::string(to_string(info->game)) + " (gameID " +
                      std::to_string(info->game_id) + "), not skyrim"};
        return out;
    }
    auto decoded = script::read_pex_script(source, vpath);
    if (!decoded) {
        out.failure = failure_from(vpath, "script", decoded.error());
        return out;
    }
    out.bytes = write_script_asset(*decoded);
    return out;
}

AssetConversion convert_lod(std::span<const std::byte> source, std::string_view vpath,
                            std::string_view extension) {
    AssetConversion out;
    auto decoded = read_lod_source(source, extension, vpath);
    if (!decoded) {
        out.failure = failure_from(vpath, "lod", decoded.error());
        return out;
    }
    if (decoded->trailing_bytes != 0) {
        out.warnings.push_back(PackWarning{.vpath = std::string(vpath),
                                           .detail = std::to_string(decoded->trailing_bytes) +
                                                     " bytes after the declared tree blocks skipped"});
    }
    out.bytes = write_lod_asset(*decoded);
    return out;
}

AssetConversion convert_animation(std::span<const std::byte> source, std::string_view vpath) {
    AssetConversion out;
    if (animation::is_animation_data(vpath)) {
        auto data = animation::read_animation_data(source, vpath);
        if (!data) {
            out.failure = failure_from(vpath, "animation", data.error());
        } else {
            out.bytes = write_animation_asset(*data);
        }
        return out;
    }
    auto decoded = animation::read_hkx(source, vpath);
    if (!decoded) {
        out.failure = failure_from(vpath, "animation", decoded.error());
        return out;
    }
    out.bytes = write_animation_asset(*decoded);
    return out;
}

} // namespace bethconv::pack
