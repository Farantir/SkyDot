// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/script/pex.hpp"

#include <bit>

namespace bethconv::script {
namespace {

/// Big-endian read. Kept here rather than in SpanReader, which would need a
/// byte-order mode just for this format.
template <typename T>
[[nodiscard]] io::ParseResult<T> read_be(io::SpanReader& reader, bool big_endian) {
    auto value = reader.get<T>();
    if (!value) {
        return std::unexpected(value.error());
    }
    if (big_endian) {
        return std::byteswap(*value);
    }
    return *value;
}

/// Papyrus `wstring`: 16-bit length, then the bytes, no terminator. Unlike
/// SpanReader::wstring() it is big-endian.
[[nodiscard]] io::ParseResult<std::string_view> read_pex_string(io::SpanReader& reader,
                                                                bool big_endian) {
    auto length = read_be<std::uint16_t>(reader, big_endian);
    if (!length) {
        return std::unexpected(length.error());
    }
    return reader.chars(*length);
}

} // namespace

std::string_view to_string(PexGame game) noexcept {
    switch (game) {
    case PexGame::skyrim: return "skyrim";
    case PexGame::fallout4: return "fallout4";
    case PexGame::unknown: break;
    }
    return "unknown";
}

io::ParseResult<PexInfo> parse_pex(std::span<const std::byte> bytes, std::string_view origin) {
    io::SpanReader reader(bytes, origin);

    PexInfo info;
    info.file_bytes = bytes.size();

    // The magic decides the byte order; a Fallout 4 script is reported as such.
    auto raw_magic = reader.get<std::uint32_t>();
    if (!raw_magic) {
        return std::unexpected(raw_magic.error());
    }
    if (std::byteswap(*raw_magic) == k_pex_magic) {
        info.big_endian = true;
    } else if (*raw_magic == k_pex_magic) {
        info.big_endian = false;
    } else {
        return std::unexpected(io::ParseError{
            .origin = std::string(origin),
            .offset = 0,
            .kind = io::ErrorKind::bad_magic,
            .detail = "not a compiled Papyrus script: expected FA57C0DE in either byte order"});
    }

    auto major = reader.get<std::uint8_t>();
    if (!major) {
        return std::unexpected(major.error());
    }
    auto minor = reader.get<std::uint8_t>();
    if (!minor) {
        return std::unexpected(minor.error());
    }
    info.major_version = *major;
    info.minor_version = *minor;

    auto game_id = read_be<std::uint16_t>(reader, info.big_endian);
    if (!game_id) {
        return std::unexpected(game_id.error());
    }
    info.game_id = *game_id;
    // gameID 1 is Skyrim, 2 is Fallout 4 (UESP; all 896 corpus files are 1).
    switch (info.game_id) {
    case 1: info.game = PexGame::skyrim; break;
    case 2: info.game = PexGame::fallout4; break;
    default: info.game = PexGame::unknown; break;
    }

    auto compiled_at = read_be<std::uint64_t>(reader, info.big_endian);
    if (!compiled_at) {
        return std::unexpected(compiled_at.error());
    }
    info.compilation_time = *compiled_at;

    auto source = read_pex_string(reader, info.big_endian);
    if (!source) {
        return std::unexpected(source.error());
    }
    info.source_file = *source;

    auto user = read_pex_string(reader, info.big_endian);
    if (!user) {
        return std::unexpected(user.error());
    }
    info.username = *user;

    auto machine = read_pex_string(reader, info.big_endian);
    if (!machine) {
        return std::unexpected(machine.error());
    }
    info.machine = *machine;

    // Walking the whole string table is what catches truncated scripts.
    auto count = read_be<std::uint16_t>(reader, info.big_endian);
    if (!count) {
        return std::unexpected(count.error());
    }
    info.string_count = *count;
    for (std::uint16_t i = 0; i < info.string_count; ++i) {
        auto entry = read_pex_string(reader, info.big_endian);
        if (!entry) {
            return std::unexpected(entry.error());
        }
    }

    info.parsed_bytes = reader.position();
    return info;
}

} // namespace bethconv::script
