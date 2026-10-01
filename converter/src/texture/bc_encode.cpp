// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/texture/bc_encode.hpp"

#include "bethconv/io/byte_writer.hpp"
#include "bethconv/io/span_reader.hpp"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <bit>
#include <mutex>
#include <thread>

namespace bethconv::texture {
namespace {

// DDS_HEADER / DDS_PIXELFORMAT / DDS_HEADER_DXT10 values, from Microsoft's DDS
// programming guide (see dds.hpp).
constexpr std::uint32_t k_ddsd_caps = 0x1;
constexpr std::uint32_t k_ddsd_height = 0x2;
constexpr std::uint32_t k_ddsd_width = 0x4;
constexpr std::uint32_t k_ddsd_pixelformat = 0x1000;
constexpr std::uint32_t k_ddsd_mipmapcount = 0x20000;
constexpr std::uint32_t k_ddsd_linearsize = 0x80000;
constexpr std::uint32_t k_ddpf_alphapixels = 0x1;
constexpr std::uint32_t k_ddpf_alpha = 0x2;
constexpr std::uint32_t k_ddpf_fourcc = 0x4;
constexpr std::uint32_t k_ddpf_luminance = 0x20000;
constexpr std::uint32_t k_ddscaps_complex = 0x8;
constexpr std::uint32_t k_ddscaps_texture = 0x1000;
constexpr std::uint32_t k_ddscaps_mipmap = 0x400000;
constexpr std::uint32_t k_dxgi_bc7_unorm = 98;
constexpr std::uint32_t k_dimension_texture2d = 3;

using Rgba = std::array<std::uint8_t, 4>;

void init_encoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

/// One channel of a pixel through its mask, widened or narrowed to 8 bits.
[[nodiscard]] std::uint8_t channel(std::uint32_t pixel, std::uint32_t mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    const auto shift = static_cast<std::uint32_t>(std::countr_zero(mask));
    const auto bits = static_cast<std::uint32_t>(std::popcount(mask));
    const std::uint32_t value = (pixel & mask) >> shift;
    if (bits == 8) {
        return static_cast<std::uint8_t>(value);
    }
    const std::uint64_t max = (std::uint64_t{1} << bits) - 1;
    return static_cast<std::uint8_t>((std::uint64_t{value} * 255 + max / 2) / max);
}

/// Pixels of one level as RGBA, row by row.
class Level {
public:
    Level(std::uint32_t width, std::uint32_t height)
        : width_(width), height_(height), rgba_(std::size_t{width} * height) {}

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    Rgba& at(std::uint32_t x, std::uint32_t y) { return rgba_[std::size_t{y} * width_ + x]; }

    /// The 4x4 block at block coordinates (bx, by); outside pixels repeat the
    /// nearest edge pixel.
    void block(std::uint32_t bx, std::uint32_t by, std::array<std::uint8_t, 64>& out) const {
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const auto px = std::min(bx * 4 + x, width_ - 1);
                const auto py = std::min(by * 4 + y, height_ - 1);
                const auto& p = rgba_[std::size_t{py} * width_ + px];
                std::copy(p.begin(), p.end(), out.begin() + (y * 4 + x) * 4);
            }
        }
    }

    [[nodiscard]] bool opaque() const noexcept {
        return std::ranges::all_of(rgba_, [](const Rgba& p) { return p[3] == 255; });
    }

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<Rgba> rgba_;
};

[[nodiscard]] io::ParseResult<Level> read_level(io::SpanReader& reader, const PixelLayout& layout,
                                                std::uint32_t width, std::uint32_t height) {
    auto bytes = reader.bytes(level_bytes(layout, width, height));
    if (!bytes) {
        return std::unexpected(std::move(bytes).error());
    }
    Level level(width, height);
    const std::uint32_t unit = layout.unit_bytes;
    const bool luminance = (layout.pf_flags & k_ddpf_luminance) != 0;
    const bool has_alpha = (layout.pf_flags & (k_ddpf_alphapixels | k_ddpf_alpha)) != 0 &&
                           layout.masks[3] != 0;
    std::size_t at = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t pixel = 0;
            for (std::uint32_t b = 0; b < unit; ++b) {
                pixel |= std::uint32_t{std::to_integer<std::uint8_t>((*bytes)[at + b])} << (8 * b);
            }
            at += unit;
            auto& out = level.at(x, y);
            if (luminance) {
                const auto grey = channel(pixel, layout.masks[0]);
                out = {grey, grey, grey, 255};
            } else {
                out = {channel(pixel, layout.masks[0]), channel(pixel, layout.masks[1]),
                       channel(pixel, layout.masks[2]), 255};
            }
            if (has_alpha) {
                out[3] = channel(pixel, layout.masks[3]);
            }
        }
    }
    return level;
}

/// Encode a level into `out`, rows of blocks split across threads; each
/// thread writes its own range.
void encode_level(const Level& level, bool bc7, bool perceptual, unsigned threads,
                  std::vector<std::byte>& out) {
    const std::uint32_t bw = (level.width() + 3) / 4;
    const std::uint32_t bh = (level.height() + 3) / 4;
    const std::size_t block_bytes = bc7 ? 16 : 8;
    const std::size_t start = out.size();
    out.resize(start + std::size_t{bw} * bh * block_bytes);

    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    if (!perceptual) {
        bc7enc_compress_block_params_init_linear_weights(&params);
    }
    // uber level 0 with all partitions: bc7enc's default speed/quality point.

    const auto run = [&](std::uint32_t first_row, std::uint32_t end_row) {
        std::array<std::uint8_t, 64> pixels{};
        std::array<std::uint8_t, 16> block{};
        for (std::uint32_t by = first_row; by < end_row; ++by) {
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                level.block(bx, by, pixels);
                if (bc7) {
                    (void)bc7enc_compress_block(block.data(), pixels.data(), &params);
                } else {
                    rgbcx::encode_bc1(10, block.data(), pixels.data(), false, false);
                }
                const std::size_t offset = start + (std::size_t{by} * bw + bx) * block_bytes;
                std::transform(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(block_bytes),
                               out.begin() + static_cast<std::ptrdiff_t>(offset),
                               [](std::uint8_t b) { return std::byte{b}; });
            }
        }
    };

    const unsigned workers = std::min<unsigned>(threads, bh);
    if (workers <= 1) {
        run(0, bh);
        return;
    }
    std::vector<std::jthread> pool;
    pool.reserve(workers);
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back(run, bh * w / workers, bh * (w + 1) / workers);
    }
}

} // namespace

std::string_view to_string(Encoding encoding) noexcept {
    switch (encoding) {
    case Encoding::keep: return "keep";
    case Encoding::bc7: return "bc7";
    case Encoding::compact: return "compact";
    }
    return "?";
}

io::ParseResult<Encoded> encode_uncompressed(std::span<const std::byte> file, const DdsInfo& info,
                                             Encoding encoding, bool normal_map,
                                             std::string_view origin, unsigned threads) {
    Encoded result;
    if (encoding == Encoding::keep || info.layout.block_compressed) {
        return result;
    }
    const auto unsupported = [&](std::string why) {
        result.outcome = EncodeOutcome::unsupported;
        result.reason = std::move(why);
        return result;
    };
    if (info.kind != SurfaceKind::texture_2d || info.array_size > 1 || info.faces != 1) {
        return unsupported(std::string(to_string(info.kind)) + " surfaces are left uncompressed");
    }
    if (info.layout.unit_bytes == 0 || info.layout.unit_bytes > 4 ||
        (info.layout.masks[0] | info.layout.masks[1] | info.layout.masks[2] | info.layout.masks[3]) == 0) {
        return unsupported("no usable channel masks in " + info.layout.name);
    }
    if (threads == 0) {
        threads = std::max(1U, std::thread::hardware_concurrency());
    }
    init_encoders();

    io::SpanReader reader(file, origin);
    if (auto header = reader.skip(info.header_bytes); !header) {
        return std::unexpected(std::move(header).error());
    }
    std::vector<Level> levels;
    for (std::uint32_t l = 0; l < info.stored_levels; ++l) {
        auto level = read_level(reader, info.layout, level_extent(info.width, l),
                                level_extent(info.height, l));
        if (!level) {
            return std::unexpected(std::move(level).error());
        }
        levels.push_back(std::move(*level));
    }

    if (levels.empty()) {  // parse_dds guarantees one level; stated for the optimizer
        return unsupported("no levels");
    }
    const bool bc7 = encoding == Encoding::bc7 || normal_map || !levels.front().opaque();
    std::vector<std::byte> data;
    for (const auto& level : levels) {
        // Normal maps are vectors, not colours: weigh the channels equally.
        encode_level(level, bc7, !normal_map, threads, data);
    }

    PixelLayout blocks;
    blocks.unit_bytes = bc7 ? 16U : 8U;
    blocks.block_compressed = true;
    const std::size_t top = level_bytes(blocks, info.width, info.height);
    io::ByteWriter out;
    out.reserve_more(k_dds_header_size + k_dx10_header_size + data.size());
    for (const char c : std::string_view("DDS ")) {
        out.put(static_cast<std::uint8_t>(c));
    }
    out.put(std::uint32_t{124});
    out.put(k_ddsd_caps | k_ddsd_height | k_ddsd_width | k_ddsd_pixelformat | k_ddsd_linearsize |
            (info.declared_mips > 1 ? k_ddsd_mipmapcount : 0U));
    out.put(info.height);
    out.put(info.width);
    out.put(static_cast<std::uint32_t>(top));
    out.put(std::uint32_t{0});          // dwDepth
    out.put(info.declared_mips);
    for (int i = 0; i < 11; ++i) {
        out.put(std::uint32_t{0});      // dwReserved1
    }
    out.put(std::uint32_t{32});         // DDS_PIXELFORMAT.dwSize
    out.put(k_ddpf_fourcc);
    out.put(bc7 ? io::FourCC{"DX10"}.value : io::FourCC{"DXT1"}.value);
    for (int i = 0; i < 5; ++i) {
        out.put(std::uint32_t{0});      // bit count and masks
    }
    out.put(k_ddscaps_texture | (info.declared_mips > 1 ? k_ddscaps_mipmap | k_ddscaps_complex : 0U));
    out.put(std::uint32_t{0});          // dwCaps2: a 2D texture
    out.put(std::uint32_t{0});
    out.put(std::uint32_t{0});
    out.put(std::uint32_t{0});
    if (bc7) {
        out.put(k_dxgi_bc7_unorm);
        out.put(k_dimension_texture2d);
        out.put(std::uint32_t{0});      // miscFlag
        out.put(std::uint32_t{1});      // arraySize
        out.put(std::uint32_t{0});      // miscFlags2: alpha mode unknown
    }
    out.put_bytes(data);

    result.outcome = EncodeOutcome::encoded;
    result.format = bc7 ? "BC7" : "BC1";
    result.data = out.take();
    return result;
}

bool decode_block(std::string_view format, std::span<const std::byte> block,
                  std::span<std::uint8_t, 64> rgba) {
    init_encoders();
    std::array<std::uint8_t, 16> bits{};
    const std::size_t size = format == "BC7" ? 16 : 8;
    if (block.size() < size) {
        return false;
    }
    std::transform(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(size), bits.begin(),
                   [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
    if (format == "BC7") {
        std::array<bc7decomp::color_rgba, 16> pixels{};
        if (!bc7decomp::unpack_bc7(bits.data(), pixels.data())) {
            return false;
        }
        for (std::size_t i = 0; i < 16; ++i) {
            for (std::size_t c = 0; c < 4; ++c) {
                rgba[i * 4 + c] = pixels[i][static_cast<std::uint32_t>(c)];
            }
        }
        return true;
    }
    if (format == "BC1") {
        // Returns whether the block uses 3-colour mode, not success.
        (void)rgbcx::unpack_bc1(bits.data(), rgba.data());
        return true;
    }
    return false;
}

} // namespace bethconv::texture
