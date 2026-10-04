// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/vpath_index.hpp"

#include "bethconv/io/mapped_file.hpp"

#include <algorithm>
#include <cstddef>

namespace bethconv::pack {
namespace {

/// 64 lowercase hex characters. Validated because the hash becomes a path under
/// `assets/`; otherwise `../../etc/passwd` in the index would be a traversal.
[[nodiscard]] bool is_content_hex(std::string_view text) noexcept {
    if (text.size() != 64) {
        return false;
    }
    return std::ranges::all_of(text, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

/// Split off the next tab-separated field without allocating.
[[nodiscard]] std::string_view next_field(std::string_view& line) noexcept {
    const auto tab = line.find('\t');
    if (tab == std::string_view::npos) {
        const auto field = line;
        line = {};
        return field;
    }
    const auto field = line.substr(0, tab);
    line.remove_prefix(tab + 1);
    return field;
}

[[nodiscard]] std::unexpected<io::ParseError> bad_line(std::string_view origin, std::size_t line,
                                                       std::string detail) {
    return std::unexpected(io::ParseError{.origin = std::string(origin),
                                          .offset = line,
                                          .kind = io::ErrorKind::corrupt,
                                          .detail = std::move(detail)});
}

} // namespace

std::string index_header() {
    return "# bethconv vpath index v" + std::to_string(k_pack_format_version) + "\n"
           "# virtual path\tcontent hash\tkind\twinning source\n";
}

std::string format_index_line(std::string_view vpath, std::string_view hex, AssetKind kind,
                              std::string_view source) {
    std::string line;
    line.reserve(vpath.size() + hex.size() + source.size() + 16);
    line += vpath;
    line += '\t';
    line += hex;
    line += '\t';
    line += to_string(kind);
    line += '\t';
    line += source;
    line += '\n';
    return line;
}

io::ParseResult<VpathIndex> VpathIndex::read(const std::filesystem::path& path) {
    auto file = io::MappedFile::open(path);
    if (!file) {
        return std::unexpected(file.error());
    }
    // Via SpanReader, whose `chars` converts bytes to text with a bounds check.
    auto reader = file->reader();
    auto text = reader.chars(reader.remaining());
    if (!text) {
        return std::unexpected(text.error());
    }
    return parse(*text, path.string());
}

io::ParseResult<VpathIndex> VpathIndex::parse(std::string_view text, std::string_view origin) {
    VpathIndex index;
    std::size_t line_number = 0;

    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end == std::string_view::npos ? text.size() : end);
        text.remove_prefix(line.size() + (end == std::string_view::npos ? 0 : 1));
        ++line_number;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (line.front() == '#') {
            // Only the first comment line matters, for its version.
            static constexpr std::string_view k_marker = "# bethconv vpath index v";
            if (line.starts_with(k_marker)) {
                std::uint32_t version = 0;
                for (const char c : line.substr(k_marker.size())) {
                    if (c < '0' || c > '9' || version > 100000) {
                        version = 0;
                        break;
                    }
                    version = version * 10 + static_cast<std::uint32_t>(c - '0');
                }
                index.format_version_ = version;
            }
            continue;
        }

        std::string_view rest = line;
        const auto vpath = next_field(rest);
        const auto hex = next_field(rest);
        const auto kind_name = next_field(rest);
        const auto source = rest; // Everything after the third tab.

        if (vpath.empty()) {
            return bad_line(origin, line_number, "index line has no virtual path");
        }
        if (!is_content_hex(hex)) {
            return bad_line(origin, line_number,
                            "index line for '" + std::string(vpath) +
                                "' does not carry 64 lowercase hex characters where the "
                                "content hash belongs");
        }
        const auto kind = kind_from_string(kind_name);
        if (!kind) {
            return bad_line(origin, line_number,
                            "index line for '" + std::string(vpath) + "' names asset kind '" +
                                std::string(kind_name) + "', which this converter does not know");
        }

        index.entries_.push_back(VpathEntry{.vpath = std::string(vpath),
                                            .hex = std::string(hex),
                                            .kind = *kind,
                                            .source = std::string(source)});
    }

    // Our writer already sorts; this protects `find`'s binary search from
    // hand-edited files.
    std::ranges::stable_sort(index.entries_, [](const VpathEntry& a, const VpathEntry& b) {
        return a.vpath < b.vpath;
    });
    return index;
}

const VpathEntry* VpathIndex::find(std::string_view vpath) const noexcept {
    const auto found = std::ranges::lower_bound(
        entries_, vpath, std::ranges::less{}, [](const VpathEntry& e) -> std::string_view {
            return e.vpath;
        });
    if (found == entries_.end() || found->vpath != vpath) {
        return nullptr;
    }
    return &*found;
}

} // namespace bethconv::pack
