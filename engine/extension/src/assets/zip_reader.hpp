// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads members of a ZIP held in memory: the pack's `ui` assets
// (formats/pack-format.md, "UI assets") are ZIPs of fonts, SVGs, PNGs and a
// manifest. Stored (0) and deflated (8) members, no ZIP64, no encryption,
// which is all the converter writes. Inflate is RFC 1951 written out here so
// the extension needs no zlib on any platform.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::assets {

struct ZipEntry {
    std::string name;
    std::uint16_t method{};
    std::uint32_t crc{};
    std::uint32_t compressed_size{};
    std::uint32_t size{};
    std::uint32_t local_offset{};
};

/// The central directory's entries, in order; empty if `zip` is not a ZIP.
[[nodiscard]] std::vector<ZipEntry> zip_entries(std::span<const std::uint8_t> zip);

/// One member's bytes, CRC checked; nullopt if absent or broken.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> zip_read(std::span<const std::uint8_t> zip,
                                                                std::string_view name);

/// Raw deflate (RFC 1951) of known output size; nullopt on a broken stream.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> inflate_raw(std::span<const std::uint8_t> in,
                                                                   std::size_t size);

} // namespace skydot::assets
