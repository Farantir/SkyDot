// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets/pack_store.hpp"

#include "skydot_formats/asset_kind.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace skydot {

namespace {

// formats/pack-format.md, "Asset store": the index header and entry sizes.
constexpr std::size_t k_index_header = 24;
constexpr std::size_t k_index_entry = 56;
constexpr std::uint32_t k_index_version = 1;

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

std::uint64_t u64(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint64_t>(u32(b, at)) | static_cast<std::uint64_t>(u32(b, at + 4)) << 32;
}

std::string hex(std::span<const std::uint8_t> b) {
    static constexpr char k_digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (const std::uint8_t v : b) {
        out.push_back(k_digits[v >> 4]);
        out.push_back(k_digits[v & 0x0F]);
    }
    return out;
}

} // namespace

// ---- PackStore ------------------------------------------------------------

std::string_view PackStore::extension_of(std::string_view kind) {
    const auto known = formats::kind_from_string(kind);
    return known ? formats::extension_of(*known) : std::string_view{};
}

bool PackStore::read_index(std::string_view text, std::string_view header, std::string& error) {
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;

        if (line_no == 1) {
            if (line != header) {
                error = "vpath.idx does not begin with '" + std::string(header) + "'";
                return false;
            }
            continue;
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        // `virtual path \t content hash \t kind \t winning source`
        const std::size_t t1 = line.find('\t');
        const std::size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
        const std::size_t t3 = t2 == std::string_view::npos ? t2 : line.find('\t', t2 + 1);
        if (t3 == std::string_view::npos) {
            error = "vpath.idx line " + std::to_string(line_no) +
                    " does not have four tab-separated fields";
            return false;
        }
        Entry entry{std::string(line.substr(t1 + 1, t2 - t1 - 1)),
                    std::string(line.substr(t2 + 1, t3 - t2 - 1)), std::string(line.substr(t3 + 1))};
        if (entry.hash.size() != 64) {
            error = "vpath.idx line " + std::to_string(line_no) + " carries a content hash of " +
                    std::to_string(entry.hash.size()) + " characters, not 64";
            return false;
        }
        if (extension_of(entry.kind).empty()) {
            ++unknown_kinds_;
        }
        index_.emplace(std::string(line.substr(0, t1)), std::move(entry));
    }
    return true;
}

bool PackStore::open_blob(std::span<const std::uint8_t> index, const std::string& blob_path,
                          std::string& error) {
    if (index.size() < k_index_header || index[0] != 'B' || index[1] != 'C' || index[2] != 'A' ||
        index[3] != 'I') {
        error = "assets.idx is not an asset index";
        return false;
    }
    if (u32(index, 4) != k_index_version) {
        error = "assets.idx is version " + std::to_string(u32(index, 4)) + ", and this reads " +
                std::to_string(k_index_version);
        return false;
    }
    const std::uint32_t count = u32(index, 12);
    const std::uint64_t blob_bytes = u64(index, 16);
    if (index.size() != k_index_header + std::size_t{count} * k_index_entry) {
        error = "assets.idx is " + std::to_string(index.size()) + " bytes for " +
                std::to_string(count) + " entries";
        return false;
    }
    auto blob = std::make_unique<MappedFile>();
    if (!blob->open(blob_path, error)) {
        return false;
    }
    if (blob->bytes().size() < blob_bytes) {
        error = "the blob is " + std::to_string(blob->bytes().size()) +
                " bytes, and its index covers " + std::to_string(blob_bytes);
        return false;
    }
    blob_entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = k_index_header + std::size_t{i} * k_index_entry;
        const Span span{u64(index, at + 32), u64(index, at + 40)};
        if (span.offset > blob_bytes || span.size > blob_bytes - span.offset) {
            error = "assets.idx entry " + std::to_string(i) + " lies outside the blob";
            return false;
        }
        blob_entries_.emplace(hex(index.subspan(at, 32)), span);
    }
    blob_ = std::move(blob);
    return true;
}

void PackStore::open_loose(const std::string& pack_dir) {
    loose_dir_ = pack_dir;
}

const PackStore::Entry* PackStore::find(const std::string& normalized_vpath) const {
    const auto it = index_.find(normalized_vpath);
    return it == index_.end() ? nullptr : &it->second;
}

std::string PackStore::loose_path(const Entry& entry) const {
    const auto kind = formats::kind_from_string(entry.kind);
    if (blob_ != nullptr || !kind) {
        return {};
    }
    return loose_dir_ + "/" + formats::asset_relative_path(entry.hash, *kind);
}

std::optional<std::vector<std::uint8_t>> PackStore::read(const std::string& normalized_vpath) const {
    const Entry* entry = find(normalized_vpath);
    if (entry == nullptr || extension_of(entry->kind).empty()) {
        return std::nullopt;
    }
    if (blob_ != nullptr) {
        const auto it = blob_entries_.find(entry->hash);
        if (it == blob_entries_.end()) {
            return std::nullopt;
        }
        const auto bytes = blob_->bytes().subspan(it->second.offset, it->second.size);
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }
    const std::string path = loose_path(*entry);
    // UTF-8 to a native path, so Windows does not read it as the ANSI codepage.
    const std::u8string utf8(path.begin(), path.end());
    std::ifstream in(std::filesystem::path(utf8), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace skydot
