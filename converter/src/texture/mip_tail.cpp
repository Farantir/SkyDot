// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/texture/mip_tail.hpp"

#include "bethconv/io/byte_writer.hpp"
#include "bethconv/io/span_reader.hpp"

#include <algorithm>

namespace bethconv::texture {

std::string_view to_string(TailOutcome outcome) noexcept {
    switch (outcome) {
    case TailOutcome::already_complete: return "already-complete";
    case TailOutcome::single_level: return "single-level";
    case TailOutcome::completed: return "completed";
    case TailOutcome::unsupported: return "unsupported";
    }
    return "?";
}

io::ParseResult<TailFix> complete_mip_tail(std::span<const std::byte> file, const DdsInfo& info,
                                           std::string_view origin) {
    TailFix fix;
    fix.from_levels = info.stored_levels;
    fix.to_levels = info.stored_levels;

    if (info.kind == SurfaceKind::volume || info.array_size > 1) {
        fix.outcome = TailOutcome::unsupported;
        return fix;
    }
    if (info.single_level()) {
        fix.outcome = TailOutcome::single_level;
        return fix;
    }
    if (!info.short_chain()) {
        fix.outcome = TailOutcome::already_complete;
        return fix;
    }

    io::SpanReader reader(file, origin);
    io::ByteWriter out;
    out.reserve_more(info.full_chain_bytes);

    // Copy the header in pieces, patching dwMipMapCount, so every byte stays
    // within a bounds check.
    auto head = reader.bytes(k_mipmap_count_offset);
    if (!head) {
        return std::unexpected(std::move(head).error());
    }
    out.put_bytes(*head);
    if (auto old_count = reader.skip(sizeof(std::uint32_t)); !old_count) {
        return std::unexpected(std::move(old_count).error());
    }
    out.put(info.full_chain_levels);
    auto rest = reader.bytes(info.header_bytes - k_mipmap_count_offset - sizeof(std::uint32_t));
    if (!rest) {
        return std::unexpected(std::move(rest).error());
    }
    out.put_bytes(*rest);

    for (std::uint32_t face = 0; face < info.faces; ++face) {
        std::span<const std::byte> last;
        for (std::uint32_t level = 0; level < info.stored_levels; ++level) {
            const std::size_t want =
                level_bytes(info.layout, level_extent(info.width, level),
                            level_extent(info.height, level));
            auto data = reader.bytes(want);
            if (!data) {
                return std::unexpected(std::move(data).error());
            }
            out.put_bytes(*data);
            last = *data;
        }

        // Seed for the missing levels: the last block-format level (all smaller
        // levels are one block), or one pixel for uncompressed data. Levels are
        // always filled with whole units.
        std::span<const std::byte> seed = last;
        if (!info.layout.block_compressed) {
            seed = last.first(info.layout.unit_bytes);
        }
        if (seed.empty()) {
            return reader.fail(io::ErrorKind::corrupt,
                               "mip level " + std::to_string(info.stored_levels - 1) +
                                   " of face " + std::to_string(face) + " is empty");
        }

        for (std::uint32_t level = info.stored_levels; level < info.full_chain_levels; ++level) {
            std::size_t want = level_bytes(info.layout, level_extent(info.width, level),
                                           level_extent(info.height, level));
            fix.added_bytes += want;
            while (want != 0) {
                const std::size_t take = std::min(seed.size(), want);
                out.put_bytes(seed.first(take));
                want -= take;
            }
        }
    }

    fix.dropped_bytes = reader.remaining();
    fix.to_levels = info.full_chain_levels;
    fix.outcome = TailOutcome::completed;
    fix.data = out.take();
    return fix;
}

} // namespace bethconv::texture
