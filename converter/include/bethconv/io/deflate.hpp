// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decompression for compressed records and archive entries: zlib for ESM4
// records and v104 archives, LZ4 frames for v105 archives.
#pragma once

#include "bethconv/io/parse_error.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace bethconv::io {

/// Upper bound for one decompressed input. Larger declared sizes are treated as
/// corrupt, skipping the record instead of exhausting memory.
inline constexpr std::size_t k_max_inflate_size = 256u * 1024u * 1024u;

/// Inflate a zlib stream of known uncompressed size (ESM4 records store it in
/// the first four bytes of the payload). Fails if the output is shorter or
/// longer than declared.
[[nodiscard]] ParseResult<std::vector<std::byte>> inflate_exact(
    std::span<const std::byte> compressed, std::size_t expected_size,
    std::string_view origin, std::size_t origin_offset);

/// Decompress an LZ4 frame of known size (BSA v105 blocks). Same contract as
/// inflate_exact.
///
/// Used instead of rsm-bsa 4.1.0's `file::decompress_into_lz4`, which loops
/// forever on a frame truncated mid-block: with both buffers exhausted,
/// `LZ4F_decompress` returns a nonzero hint without progress and the loop never
/// exits. Found by fuzz_bsa with a 321-byte archive. Verified byte-identical on
/// 33,685 LZ4 entries (SE meshes) and 132,907 zlib entries (LE).
[[nodiscard]] ParseResult<std::vector<std::byte>> lz4_decompress_exact(
    std::span<const std::byte> compressed, std::size_t expected_size,
    std::string_view origin, std::size_t origin_offset);

} // namespace bethconv::io
