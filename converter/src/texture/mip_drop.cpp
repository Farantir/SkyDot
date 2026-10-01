// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/texture/mip_drop.hpp"

#include "bethconv/io/byte_writer.hpp"
#include "bethconv/io/span_reader.hpp"

namespace bethconv::texture {

std::string_view to_string(DropOutcome outcome) noexcept {
    switch (outcome) {
    case DropOutcome::fits: return "fits";
    case DropOutcome::shrunk: return "shrunk";
    case DropOutcome::single_level: return "single-level";
    case DropOutcome::too_few_levels: return "too-few-levels";
    case DropOutcome::unsupported: return "unsupported";
    }
    return "?";
}

io::ParseResult<SizeLimit> limit_size(std::span<const std::byte> file, const DdsInfo& info,
                                      std::uint32_t max_extent, std::string_view origin) {
    SizeLimit limit;
    limit.width = info.width;
    limit.height = info.height;
    if (max_extent == 0 || (info.width <= max_extent && info.height <= max_extent)) {
        return limit;
    }
    if (info.kind == SurfaceKind::volume || info.array_size > 1) {
        limit.outcome = DropOutcome::unsupported;
        return limit;
    }
    if (info.single_level()) {
        limit.outcome = DropOutcome::single_level;
        return limit;
    }
    std::uint32_t drop = 0;
    while (level_extent(info.width, drop) > max_extent || level_extent(info.height, drop) > max_extent) {
        ++drop;
    }
    if (drop >= info.stored_levels) {
        limit.outcome = DropOutcome::too_few_levels;
        return limit;
    }

    const std::uint32_t width = level_extent(info.width, drop);
    const std::uint32_t height = level_extent(info.height, drop);
    const std::uint32_t levels = info.stored_levels - drop;
    const std::size_t skipped = chain_bytes(info.layout, info.width, info.height, drop);
    const std::size_t kept = chain_bytes(info.layout, width, height, levels);

    io::SpanReader reader(file, origin);
    io::ByteWriter out;
    out.reserve_more(info.header_bytes + kept * info.faces);

    // The header in pieces, patching height, width, pitch and mip count, so
    // every byte stays within a bounds check.
    auto head = reader.bytes(k_height_offset);
    if (!head) {
        return std::unexpected(std::move(head).error());
    }
    out.put_bytes(*head);
    for (std::size_t field = 0; field < 2; ++field) {
        if (auto old = reader.skip(sizeof(std::uint32_t)); !old) {
            return std::unexpected(std::move(old).error());
        }
    }
    out.put(height);
    out.put(width);
    auto pitch = reader.get<std::uint32_t>();
    if (!pitch) {
        return std::unexpected(std::move(pitch).error());
    }
    // Many writers leave it 0; only a set value is kept meaningful.
    std::uint32_t new_pitch = 0;
    if (*pitch != 0) {
        new_pitch = info.layout.block_compressed
                        ? static_cast<std::uint32_t>(level_bytes(info.layout, width, height))
                        : (width * info.layout.rgb_bit_count + 7) / 8;
    }
    out.put(new_pitch);
    auto depth = reader.bytes(k_mipmap_count_offset - k_pitch_offset - sizeof(std::uint32_t));
    if (!depth) {
        return std::unexpected(std::move(depth).error());
    }
    out.put_bytes(*depth);
    if (auto old = reader.skip(sizeof(std::uint32_t)); !old) {
        return std::unexpected(std::move(old).error());
    }
    out.put(levels);
    auto rest = reader.bytes(info.header_bytes - k_mipmap_count_offset - sizeof(std::uint32_t));
    if (!rest) {
        return std::unexpected(std::move(rest).error());
    }
    out.put_bytes(*rest);

    for (std::uint32_t face = 0; face < info.faces; ++face) {
        if (auto large = reader.skip(skipped); !large) {
            return std::unexpected(std::move(large).error());
        }
        auto small = reader.bytes(kept);
        if (!small) {
            return std::unexpected(std::move(small).error());
        }
        out.put_bytes(*small);
    }

    limit.outcome = DropOutcome::shrunk;
    limit.dropped_levels = drop;
    limit.width = width;
    limit.height = height;
    limit.data = out.take();
    limit.saved_bytes = file.size() > limit.data.size() ? file.size() - limit.data.size() : 0;
    return limit;
}

} // namespace bethconv::texture
