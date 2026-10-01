// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic .STRINGS/.DLSTRINGS/.ILSTRINGS writer, mainly for malformed tables:
// offsets past the data, overlong lengths, bad counts, missing terminators,
// invalid UTF-8.
//
// The layout is written by hand, not derived from record/strings.hpp, so
// fixture and parser cannot share a bug.
#pragma once

#include "bethconv/io/byte_writer.hpp"
#include "bethconv/record/strings.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bethconv::testing {

/// One directory entry and its text.
struct StringEntry {
    std::uint32_t id{};
    std::string text;

    /// Share an earlier entry's bytes instead of writing a copy (as Bethesda
    /// does). Index into the spec's entry list.
    int alias_of = -1;

    /// Write `offset` instead of the real one, e.g. past the end.
    bool override_offset = false;
    std::uint32_t offset = 0;

    /// Length-prefixed tables only: write `length` instead of `text.size() + 1`.
    bool override_length = false;
    std::uint32_t length = 0;

    /// `.strings` only: omit the NUL terminator.
    bool omit_terminator = false;
};

struct StringTableSpec {
    record::StringKind kind = record::StringKind::plain;
    std::vector<StringEntry> entries;

    /// Write `count` instead of `entries.size()`.
    bool override_count = false;
    std::uint32_t count = 0;

    /// Write `data_size` instead of the real section length.
    bool override_data_size = false;
    std::uint32_t data_size = 0;

    /// Truncate the file to this many bytes; 0 keeps it whole.
    std::size_t truncate_to = 0;
};

[[nodiscard]] inline std::vector<std::byte> build_string_table(const StringTableSpec& spec) {
    const bool prefixed = spec.kind != record::StringKind::plain;

    // Build the data section first so the directory can point into it.
    io::ByteWriter data;
    std::vector<std::uint32_t> offsets(spec.entries.size(), 0);
    for (std::size_t i = 0; i < spec.entries.size(); ++i) {
        const auto& entry = spec.entries[i];
        if (entry.alias_of >= 0) {
            offsets[i] = offsets[static_cast<std::size_t>(entry.alias_of)];
            continue;
        }
        offsets[i] = static_cast<std::uint32_t>(data.size());
        if (prefixed) {
            const auto length = entry.override_length
                                    ? entry.length
                                    : static_cast<std::uint32_t>(entry.text.size() + 1);
            data.put(length);
        }
        for (const char c : entry.text) {
            data.put(static_cast<std::uint8_t>(c));
        }
        if (!entry.omit_terminator) {
            data.put(static_cast<std::uint8_t>(0));
        }
    }
    const auto section = data.take();

    io::ByteWriter out;
    out.put(spec.override_count ? spec.count : static_cast<std::uint32_t>(spec.entries.size()));
    out.put(spec.override_data_size ? spec.data_size
                                    : static_cast<std::uint32_t>(section.size()));
    for (std::size_t i = 0; i < spec.entries.size(); ++i) {
        const auto& entry = spec.entries[i];
        out.put(entry.id);
        out.put(entry.override_offset ? entry.offset : offsets[i]);
    }
    out.put_bytes(section);

    auto bytes = out.take();
    if (spec.truncate_to != 0 && spec.truncate_to < bytes.size()) {
        bytes.resize(spec.truncate_to);
    }
    return bytes;
}

} // namespace bethconv::testing
