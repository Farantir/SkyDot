// SPDX-License-Identifier: GPL-3.0-or-later
//
// The save container (docs/adr/0002-game-state-saves-and-session.md): magic
// "SKYS", u32 container version, sections (FourCC tag, u32 section version,
// u64 length, bytes), then a CRC-32 of everything before it. Little-endian.
// Each state class writes and reads its own section; a reader skips tags it
// does not know. Nothing here touches Godot.
#pragma once

#include "save/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::save {

inline constexpr std::uint32_t k_container_version = 1;

[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

class SaveWriter {
public:
    /// Add a section. `tag` is four characters. Sections keep their order.
    void add(std::string_view tag, std::uint32_t version, const std::vector<std::uint8_t>& bytes);
    /// The whole file: header, sections and the CRC trailer.
    [[nodiscard]] std::vector<std::uint8_t> finish() const;
    /// Write `bytes` to `<path>.tmp`, flush, then rename over `path`, so a
    /// crash while writing leaves the old file. False (with `error`) on failure.
    static bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes, std::string& error);

private:
    std::vector<std::uint8_t> body_;
};

struct SectionView {
    std::string tag;
    std::uint32_t version = 0;
    std::size_t offset = 0; ///< In the file's bytes.
    std::size_t size = 0;
};

class SaveReader {
public:
    /// Take the bytes of a file and check its magic, container version,
    /// section framing and CRC. Nothing is decoded.
    bool parse(std::vector<std::uint8_t> bytes, std::string& error);
    /// A reader over a section's bytes (valid while this reader is).
    [[nodiscard]] ByteReader open(const SectionView& section) const;
    /// The section with this tag, or null. Pointers stay valid until the
    /// reader is parsed again or destroyed.
    [[nodiscard]] const SectionView* find(std::string_view tag) const;
    [[nodiscard]] const std::vector<SectionView>& sections() const { return sections_; }
    static bool read_file(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error);

private:
    std::vector<std::uint8_t> bytes_;
    std::vector<SectionView> sections_;
};

} // namespace skydot::save
