// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/strings.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace bethconv::record {
namespace {

/// Directory entries: two uint32 (after a count and dataSize header).
constexpr std::size_t k_directory_entry_size = 8;

/// Entry count limit, so a corrupt header cannot trigger a huge reservation.
/// The largest vanilla table has 34,427 entries.
constexpr std::uint32_t k_max_entries = 4u * 1024u * 1024u;

/// Length of the well-formed UTF-8 sequence at `i` (Unicode 15.0 table 3-7), or
/// 0. Rejects overlongs, surrogates and values above U+10FFFF.
[[nodiscard]] std::size_t utf8_sequence_length(std::string_view text, std::size_t i) noexcept {
    const auto byte_at = [&](std::size_t k) -> unsigned {
        return static_cast<unsigned char>(text[k]);
    };
    const auto have = text.size() - i;
    const unsigned b0 = byte_at(i);

    if (b0 <= 0x7F) {
        return 1;
    }
    const auto is_cont = [&](std::size_t k, unsigned lo, unsigned hi) {
        const unsigned b = byte_at(k);
        return b >= lo && b <= hi;
    };
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        return have >= 2 && is_cont(i + 1, 0x80, 0xBF) ? 2 : 0;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (have < 3) {
            return 0;
        }
        // E0 forbids an overlong second byte; ED forbids the surrogate range.
        const unsigned lo = b0 == 0xE0 ? 0xA0 : 0x80;
        const unsigned hi = b0 == 0xED ? 0x9F : 0xBF;
        return is_cont(i + 1, lo, hi) && is_cont(i + 2, 0x80, 0xBF) ? 3 : 0;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (have < 4) {
            return 0;
        }
        const unsigned lo = b0 == 0xF0 ? 0x90 : 0x80;
        const unsigned hi = b0 == 0xF4 ? 0x8F : 0xBF;
        return is_cont(i + 1, lo, hi) && is_cont(i + 2, 0x80, 0xBF) &&
                       is_cont(i + 3, 0x80, 0xBF)
                   ? 4
                   : 0;
    }
    // 0x80..0xC1 (continuation or overlong lead) and 0xF5..0xFF.
    return 0;
}

/// `text` with ill-formed bytes replaced by U+FFFD, and whether that happened.
/// The output feeds JSON and glTF, where one invalid byte would fail the whole
/// document. Zero repairs in vanilla.
[[nodiscard]] std::pair<std::string, bool> sanitize_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto len = utf8_sequence_length(text, i);
        if (len == 0) {
            break;
        }
        i += len;
    }
    if (i == text.size()) {
        return {std::string(text), false};
    }

    std::string out(text.substr(0, i));
    out.reserve(text.size());
    while (i < text.size()) {
        const auto len = utf8_sequence_length(text, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD"; // U+FFFD REPLACEMENT CHARACTER
            ++i;
        } else {
            out.append(text.substr(i, len));
            i += len;
        }
    }
    return {std::move(out), true};
}

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

std::string_view to_string(StringKind kind) noexcept {
    switch (kind) {
    case StringKind::plain:       return "STRINGS";
    case StringKind::description: return "DLSTRINGS";
    case StringKind::dialogue:    return "ILSTRINGS";
    }
    return "unknown";
}

std::string_view extension_of(StringKind kind) noexcept {
    switch (kind) {
    case StringKind::plain:       return ".strings";
    case StringKind::description: return ".dlstrings";
    case StringKind::dialogue:    return ".ilstrings";
    }
    return "";
}

std::string string_table_path(std::string_view plugin_name, std::string_view language,
                              StringKind kind) {
    std::string_view stem = plugin_name;
    if (const auto dot = stem.find_last_of('.'); dot != std::string_view::npos) {
        stem = stem.substr(0, dot);
    }
    std::string out = "strings/";
    out += lowercase(stem);
    out += '_';
    out += lowercase(language);
    out += extension_of(kind);
    return out;
}

io::ParseResult<StringTable> StringTable::parse(std::span<const std::byte> bytes,
                                                std::string_view origin, StringKind kind) {
    io::SpanReader reader(bytes, origin);

    auto count = reader.get<std::uint32_t>();
    if (!count) {
        return std::unexpected(std::move(count).error());
    }
    auto data_size = reader.get<std::uint32_t>();
    if (!data_size) {
        return std::unexpected(std::move(data_size).error());
    }
    if (*count > k_max_entries) {
        return reader.fail(io::ErrorKind::too_large,
                           "string table claims " + std::to_string(*count) +
                               " entries (cap " + std::to_string(k_max_entries) + ")");
    }

    // The directory follows the header, and the data section must fit in the
    // rest. Vanilla tables match 8 + count*8 + dataSize exactly; extra trailing
    // bytes are ignored, missing ones are an error.
    auto directory = reader.subreader(static_cast<std::size_t>(*count) * k_directory_entry_size);
    if (!directory) {
        return std::unexpected(std::move(directory).error());
    }
    auto data = reader.subreader(*data_size);
    if (!data) {
        return std::unexpected(std::move(data).error());
    }

    StringTable table;
    table.kind_ = kind;
    table.stats_.entries = *count;
    table.index_.reserve(*count);

    // Ids can share an offset, so decoding is keyed on the offset.
    std::unordered_map<std::uint32_t, std::uint32_t> by_offset;
    by_offset.reserve(*count);

    for (std::uint32_t i = 0; i < *count; ++i) {
        auto id = directory->get<std::uint32_t>();
        if (!id) {
            return std::unexpected(std::move(id).error());
        }
        auto offset = directory->get<std::uint32_t>();
        if (!offset) {
            return std::unexpected(std::move(offset).error());
        }

        if (const auto seen = by_offset.find(*offset); seen != by_offset.end()) {
            table.index_.insert_or_assign(*id, seen->second);
            continue;
        }

        auto entry = *data;
        if (auto sought = entry.seek(*offset); !sought) {
            return std::unexpected(std::move(sought).error());
        }

        io::ParseResult<std::string_view> raw = std::string_view{};
        if (kind == StringKind::plain) {
            raw = entry.zstring();
        } else {
            // uint32 length including the NUL (true for every vanilla entry).
            auto length = entry.get<std::uint32_t>();
            if (!length) {
                return std::unexpected(std::move(length).error());
            }
            if (*length == 0) {
                raw = std::string_view{};
            } else {
                raw = entry.chars(*length - 1);
            }
        }
        if (!raw) {
            return std::unexpected(std::move(raw).error());
        }

        auto [text, repaired] = sanitize_utf8(*raw);
        table.stats_.text_bytes += text.size();
        table.stats_.repaired += repaired ? 1 : 0;

        const auto slot = static_cast<std::uint32_t>(table.texts_.size());
        table.texts_.push_back(std::move(text));
        by_offset.emplace(*offset, slot);
        table.index_.insert_or_assign(*id, slot);
    }

    table.stats_.distinct_strings = table.texts_.size();
    return table;
}

const std::string* StringTable::find(std::uint32_t id) const noexcept {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return nullptr;
    }
    return &texts_[it->second];
}

void StringSource::set(StringKind kind, StringTable table) {
    tables_[static_cast<std::size_t>(kind)] = std::move(table);
}

const StringTable& StringSource::table(StringKind kind) const noexcept {
    return tables_[static_cast<std::size_t>(kind)];
}

const std::string* StringSource::find(std::uint32_t id, StringKind kind) const noexcept {
    return table(kind).find(id);
}

bool StringSource::empty() const noexcept {
    return std::ranges::all_of(tables_, [](const StringTable& t) { return t.empty(); });
}

std::size_t StringSource::size() const noexcept {
    std::size_t total = 0;
    for (const auto& table : tables_) {
        total += table.size();
    }
    return total;
}

std::uint64_t StringSource::repaired() const noexcept {
    std::uint64_t total = 0;
    for (const auto& table : tables_) {
        total += table.stats().repaired;
    }
    return total;
}

StringSource load_string_source(const StringFetch& fetch, std::string_view plugin_name,
                                std::string_view language,
                                std::vector<io::ParseError>* problems) {
    StringSource source;
    for (const StringKind kind : k_string_kinds) {
        const auto vpath = string_table_path(plugin_name, language, kind);
        auto bytes = fetch(vpath);
        if (!bytes) {
            // Missing is normal (no dialogue, no .ilstrings).
            continue;
        }
        auto table = StringTable::parse(*bytes, vpath, kind);
        if (!table) {
            if (problems != nullptr) {
                problems->push_back(std::move(table).error());
            }
            continue;
        }
        source.set(kind, std::move(*table));
    }
    return source;
}

} // namespace bethconv::record
