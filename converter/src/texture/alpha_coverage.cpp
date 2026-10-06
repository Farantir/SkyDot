// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/texture/alpha_coverage.hpp"

#include "bethconv/io/byte_writer.hpp"
#include "bethconv/io/span_reader.hpp"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <mutex>

namespace bethconv::texture {
namespace {

enum class Codec : std::uint8_t { none, dxt1, bc2, bc3, bc7, rgba32 };

constexpr std::uint32_t k_ddpf_alphapixels = 0x1;

void init_codecs() {
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

[[nodiscard]] Codec codec_of(const DdsInfo& info) noexcept {
    const PixelLayout& layout = info.layout;
    if (layout.block_compressed) {
        const std::string_view name = layout.name;
        if (name == "DXT1" || name == "BC1") {
            return Codec::dxt1;
        }
        if (name == "DXT3" || name == "BC2") {
            return Codec::bc2;
        }
        if (name == "DXT5" || name == "BC3") {
            return Codec::bc3;
        }
        if (name == "BC7") {
            return Codec::bc7;
        }
        return Codec::none;
    }
    // Legacy header with an 8-bit alpha channel in a 32-bit pixel.
    if (layout.unit_bytes == 4 && (layout.pf_flags & k_ddpf_alphapixels) != 0 &&
        std::popcount(layout.masks[3]) == 8 && std::countr_zero(layout.masks[3]) % 8 == 0) {
        return Codec::rgba32;
    }
    return Codec::none;
}

/// One level's alpha, padded to whole blocks for block formats. Texels
/// outside the level (`width` x `height`) are present but not counted.
struct Plane {
    std::uint32_t width{};  ///< Level size.
    std::uint32_t height{};
    std::uint32_t stride{}; ///< Row length of `alpha`.
    std::vector<std::uint8_t> alpha;

    [[nodiscard]] std::array<std::uint32_t, 256> histogram() const {
        std::array<std::uint32_t, 256> hist{};
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                ++hist[alpha[std::size_t{y} * stride + x]];
            }
        }
        return hist;
    }
};

[[nodiscard]] std::uint8_t byte_u8(std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); }

/// Alpha of one level from its raw bytes (`data` is exactly the level).
[[nodiscard]] io::ParseResult<Plane> decode_plane(Codec codec, const DdsInfo& info,
                                                  std::span<const std::byte> data,
                                                  std::uint32_t width, std::uint32_t height,
                                                  std::string_view origin) {
    Plane plane;
    plane.width = width;
    plane.height = height;
    io::SpanReader reader(data, origin);

    if (codec == Codec::rgba32) {
        plane.stride = width;
        plane.alpha.resize(std::size_t{width} * height);
        const std::uint32_t shift = static_cast<std::uint32_t>(std::countr_zero(info.layout.masks[3])) / 8;
        for (std::size_t i = 0; i < plane.alpha.size(); ++i) {
            auto pixel = reader.bytes(4);
            if (!pixel) {
                return std::unexpected(std::move(pixel).error());
            }
            plane.alpha[i] = byte_u8((*pixel)[shift]);
        }
        return plane;
    }

    init_codecs();
    const std::uint32_t bw = (width + 3) / 4;
    const std::uint32_t bh = (height + 3) / 4;
    plane.stride = bw * 4;
    plane.alpha.resize(std::size_t{plane.stride} * bh * 4);
    const std::size_t unit = info.layout.unit_bytes;
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            auto block = reader.bytes(unit);
            if (!block) {
                return std::unexpected(std::move(block).error());
            }
            std::array<std::uint8_t, 16> a{};
            std::array<std::uint8_t, 16> raw{};
            for (std::size_t i = 0; i < unit; ++i) {
                raw[i] = byte_u8((*block)[i]);
            }
            switch (codec) {
            case Codec::bc3:
                rgbcx::unpack_bc4(raw.data(), a.data(), 1);
                break;
            case Codec::bc2:
                for (std::size_t i = 0; i < 16; ++i) {
                    const std::uint8_t nibble = (raw[i / 2] >> (4 * (i % 2))) & 0xF;
                    a[i] = static_cast<std::uint8_t>(nibble * 17);
                }
                break;
            case Codec::bc7: {
                std::array<bc7decomp::color_rgba, 16> pixels{};
                if (bc7decomp::unpack_bc7(raw.data(), pixels.data())) {
                    for (std::size_t i = 0; i < 16; ++i) {
                        a[i] = pixels[i][3];
                    }
                } else {
                    a.fill(255);
                }
                break;
            }
            case Codec::dxt1: {
                // 4-colour mode is opaque; in 3-colour mode (c0 <= c1) index 3
                // is transparent.
                const std::uint16_t c0 = static_cast<std::uint16_t>(raw[0] | (raw[1] << 8));
                const std::uint16_t c1 = static_cast<std::uint16_t>(raw[2] | (raw[3] << 8));
                for (std::size_t i = 0; i < 16; ++i) {
                    const std::uint8_t index = (raw[4 + i / 4] >> (2 * (i % 4))) & 3;
                    a[i] = (c0 <= c1 && index == 3) ? 0 : 255;
                }
                break;
            }
            default:
                a.fill(255);
                break;
            }
            for (std::uint32_t y = 0; y < 4; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    plane.alpha[std::size_t{by * 4 + y} * plane.stride + bx * 4 + x] = a[y * 4 + x];
                }
            }
        }
    }
    return plane;
}

/// The level's bytes with `plane`'s alpha in place of the old one; `data` is
/// the old level.
[[nodiscard]] io::ParseResult<std::vector<std::byte>> encode_plane(
    Codec codec, const DdsInfo& info, std::span<const std::byte> data, const Plane& plane,
    std::string_view origin) {
    io::SpanReader reader(data, origin);
    io::ByteWriter out;
    out.reserve_more(data.size());

    if (codec == Codec::rgba32) {
        const std::uint32_t shift = static_cast<std::uint32_t>(std::countr_zero(info.layout.masks[3])) / 8;
        for (std::size_t i = 0; i < plane.alpha.size(); ++i) {
            auto pixel = reader.bytes(4);
            if (!pixel) {
                return std::unexpected(std::move(pixel).error());
            }
            std::array<std::byte, 4> px{(*pixel)[0], (*pixel)[1], (*pixel)[2], (*pixel)[3]};
            px[shift] = static_cast<std::byte>(plane.alpha[i]);
            out.put_bytes(px);
        }
        return out.take();
    }

    init_codecs();
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    const std::uint32_t bw = (plane.width + 3) / 4;
    const std::uint32_t bh = (plane.height + 3) / 4;
    const std::size_t unit = info.layout.unit_bytes;
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            auto block = reader.bytes(unit);
            if (!block) {
                return std::unexpected(std::move(block).error());
            }
            std::array<std::uint8_t, 16> raw{};
            for (std::size_t i = 0; i < unit; ++i) {
                raw[i] = byte_u8((*block)[i]);
            }
            std::array<std::uint8_t, 16> a{};
            for (std::uint32_t y = 0; y < 4; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    a[y * 4 + x] = plane.alpha[std::size_t{by * 4 + y} * plane.stride + bx * 4 + x];
                }
            }
            switch (codec) {
            case Codec::bc3:
                // Colour half (bytes 8..15) stays as it was.
                (void)rgbcx::encode_bc4_hq(raw.data(), a.data(), 1);
                break;
            case Codec::bc2:
                for (std::size_t i = 0; i < 16; i += 2) {
                    const auto q0 = static_cast<std::uint8_t>((a[i] * 15 + 127) / 255);
                    const auto q1 = static_cast<std::uint8_t>((a[i + 1] * 15 + 127) / 255);
                    raw[i / 2] = static_cast<std::uint8_t>(q0 | (q1 << 4));
                }
                break;
            case Codec::bc7: {
                std::array<bc7decomp::color_rgba, 16> pixels{};
                std::array<std::uint8_t, 64> rgba{};
                if (bc7decomp::unpack_bc7(raw.data(), pixels.data())) {
                    for (std::size_t i = 0; i < 16; ++i) {
                        for (std::uint32_t c = 0; c < 3; ++c) {
                            rgba[i * 4 + c] = pixels[i][c];
                        }
                        rgba[i * 4 + 3] = a[i];
                    }
                    (void)bc7enc_compress_block(raw.data(), rgba.data(), &params);
                }
                break;
            }
            default:
                break;
            }
            for (std::size_t i = 0; i < unit; ++i) {
                out.put(static_cast<std::byte>(raw[i]));
            }
        }
    }
    return out.take();
}

[[nodiscard]] double coverage_of(const std::array<std::uint32_t, 256>& hist, std::uint32_t threshold,
                                 std::uint64_t total) {
    if (total == 0) {
        return 0.0;
    }
    std::uint64_t covered = 0;
    for (std::uint32_t a = std::min<std::uint32_t>(threshold, 255); a < 256; ++a) {
        covered += hist[a];
    }
    return static_cast<double>(covered) / static_cast<double>(total);
}

/// The alpha value `cut` (1..255) that, scaled to the threshold, gives the
/// coverage closest to `target`: every alpha >= cut counts as covered. A
/// binary search over the scale would land on the same step; the histogram
/// has only 255 of them.
[[nodiscard]] std::uint32_t pick_cut(const std::array<std::uint32_t, 256>& hist,
                                     std::uint32_t threshold, double target, std::uint64_t total) {
    std::uint32_t best = threshold;
    double best_error = std::abs(coverage_of(hist, threshold, total) - target);
    // Rounding in the file's own encoding moves coverage by about this much.
    if (best_error <= 0.002) {
        return threshold;
    }
    for (std::uint32_t cut = 1; cut < 256; ++cut) {
        const double error = std::abs(coverage_of(hist, cut, total) - target);
        const auto distance = [&](std::uint32_t c) {
            return c > threshold ? c - threshold : threshold - c;
        };
        if (error < best_error || (error == best_error && distance(cut) < distance(best))) {
            best = cut;
            best_error = error;
        }
    }
    return best;
}

struct Levels {
    std::vector<std::span<const std::byte>> data;
    std::vector<Plane> planes;
};

[[nodiscard]] io::ParseResult<Levels> read_levels(Codec codec, const DdsInfo& info,
                                                  std::span<const std::byte> file,
                                                  std::string_view origin) {
    io::SpanReader reader(file, origin);
    if (auto head = reader.skip(info.header_bytes); !head) {
        return std::unexpected(std::move(head).error());
    }
    Levels levels;
    for (std::uint32_t l = 0; l < info.stored_levels; ++l) {
        const std::uint32_t w = level_extent(info.width, l);
        const std::uint32_t h = level_extent(info.height, l);
        auto bytes = reader.bytes(level_bytes(info.layout, w, h));
        if (!bytes) {
            return std::unexpected(std::move(bytes).error());
        }
        auto plane = decode_plane(codec, info, *bytes, w, h, origin);
        if (!plane) {
            return std::unexpected(std::move(plane).error());
        }
        levels.data.push_back(*bytes);
        levels.planes.push_back(std::move(*plane));
    }
    return levels;
}

[[nodiscard]] bool plain_surface(const DdsInfo& info) noexcept {
    return info.kind == SurfaceKind::texture_2d && info.array_size <= 1 && info.faces == 1;
}

} // namespace

std::string_view to_string(CoverageOutcome outcome) noexcept {
    switch (outcome) {
    case CoverageOutcome::adjusted: return "adjusted";
    case CoverageOutcome::unchanged: return "unchanged";
    case CoverageOutcome::single_level: return "single-level";
    case CoverageOutcome::unsupported: return "unsupported";
    }
    return "?";
}

io::ParseResult<std::vector<double>> measure_alpha_coverage(std::span<const std::byte> file,
                                                            const DdsInfo& info,
                                                            std::uint32_t threshold,
                                                            std::string_view origin) {
    const Codec codec = codec_of(info);
    if (codec == Codec::none || !plain_surface(info)) {
        return std::vector<double>{};
    }
    auto levels = read_levels(codec, info, file, origin);
    if (!levels) {
        return std::unexpected(std::move(levels).error());
    }
    std::vector<double> out;
    for (const Plane& plane : levels->planes) {
        out.push_back(coverage_of(plane.histogram(), threshold,
                                  std::uint64_t{plane.width} * plane.height));
    }
    return out;
}

io::ParseResult<CoverageFix> preserve_alpha_coverage(std::span<const std::byte> file,
                                                     const DdsInfo& info, std::uint32_t threshold,
                                                     std::string_view origin) {
    CoverageFix fix;
    const auto unsupported = [&](std::string why) {
        fix.outcome = CoverageOutcome::unsupported;
        fix.reason = std::move(why);
        return fix;
    };
    if (!plain_surface(info)) {
        return unsupported(std::string(to_string(info.kind)) + " surfaces are left alone");
    }
    if (threshold == 0 || threshold > 255) {
        return unsupported("no alpha test threshold in 1..255");
    }
    const Codec codec = codec_of(info);
    if (codec == Codec::none) {
        return unsupported("no alpha channel to scale in " + info.layout.name);
    }
    if (info.stored_levels < 2) {
        fix.outcome = CoverageOutcome::single_level;
        return fix;
    }
    auto read = read_levels(codec, info, file, origin);
    if (!read) {
        return std::unexpected(std::move(read).error());
    }
    Levels& levels = *read;

    const auto total = [](const Plane& p) { return std::uint64_t{p.width} * p.height; };
    std::vector<std::array<std::uint32_t, 256>> hists;
    for (const Plane& plane : levels.planes) {
        hists.push_back(plane.histogram());
        fix.before.push_back(coverage_of(hists.back(), threshold, total(plane)));
    }
    fix.after = fix.before;
    fix.scale.assign(levels.planes.size(), 1.0F);
    const double target = fix.before.front();
    if (codec == Codec::dxt1) {
        return unsupported("one-bit alpha (DXT1) cannot be scaled");
    }
    if (target <= 0.0 || target >= 1.0) {
        // Nothing is cut out, or everything is: no coverage to keep.
        fix.outcome = CoverageOutcome::unchanged;
        return fix;
    }

    std::vector<std::vector<std::byte>> rewritten(levels.planes.size());
    bool changed = false;
    for (std::size_t l = 1; l < levels.planes.size(); ++l) {
        Plane& plane = levels.planes[l];
        const std::uint32_t cut = pick_cut(hists[l], threshold, target, total(plane));
        if (cut == threshold) {
            continue;
        }
        fix.scale[l] = static_cast<float>(threshold) / static_cast<float>(cut);
        for (auto& a : plane.alpha) {
            const std::uint32_t scaled = (std::uint32_t{a} * threshold + cut / 2) / cut;
            a = static_cast<std::uint8_t>(std::min<std::uint32_t>(scaled, 255));
        }
        auto bytes = encode_plane(codec, info, levels.data[l], plane, origin);
        if (!bytes) {
            return std::unexpected(std::move(bytes).error());
        }
        // What the encoding actually kept.
        auto check = decode_plane(codec, info, *bytes, plane.width, plane.height, origin);
        if (!check) {
            return std::unexpected(std::move(check).error());
        }
        fix.after[l] = coverage_of(check->histogram(), threshold, total(plane));
        rewritten[l] = std::move(*bytes);
        changed = true;
    }
    if (!changed) {
        fix.outcome = CoverageOutcome::unchanged;
        return fix;
    }

    io::SpanReader reader(file, origin);
    io::ByteWriter out;
    out.reserve_more(file.size());
    auto head = reader.bytes(info.header_bytes);
    if (!head) {
        return std::unexpected(std::move(head).error());
    }
    out.put_bytes(*head);
    for (std::size_t l = 0; l < levels.data.size(); ++l) {
        if (auto skipped = reader.skip(levels.data[l].size()); !skipped) {
            return std::unexpected(std::move(skipped).error());
        }
        out.put_bytes(rewritten[l].empty() ? levels.data[l]
                                           : std::span<const std::byte>(rewritten[l]));
    }
    fix.data = out.take();
    fix.outcome = CoverageOutcome::adjusted;
    return fix;
}

} // namespace bethconv::texture
