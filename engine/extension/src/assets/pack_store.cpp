// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets/pack_store.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// ---- MappedFile -----------------------------------------------------------

MappedFile::~MappedFile() {
#if defined(_WIN32)
    if (data_ != nullptr) {
        UnmapViewOfFile(data_);
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
    }
    if (file_ != nullptr && file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
    }
#else
    if (data_ != nullptr && size_ != 0) {
        munmap(const_cast<std::uint8_t*>(data_), size_);
    }
#endif
}

bool MappedFile::open(const std::string& path, std::string& error) {
#if defined(_WIN32)
    const int wide_size = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wide_size);
    // Shared for writing and deletion too: a converter may still hold the
    // blob open, and Windows otherwise refuses the open.
    file_ = CreateFileW(wide.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        error = "cannot open " + path;
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size)) {
        error = "cannot size " + path;
        return false;
    }
    size_ = static_cast<std::size_t>(size.QuadPart);
    if (size_ == 0) {
        return true;
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_ == nullptr) {
        error = "cannot map " + path;
        return false;
    }
    data_ = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (data_ == nullptr) {
        error = "cannot map " + path;
        return false;
    }
    return true;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path;
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        error = "cannot size " + path;
        return false;
    }
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ == 0) {
        ::close(fd);
        return true;
    }
    void* mapped = mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
        size_ = 0;
        error = "cannot map " + path;
        return false;
    }
    data_ = static_cast<const std::uint8_t*>(mapped);
    return true;
#endif
}

// ---- PackStore ------------------------------------------------------------

std::string_view PackStore::extension_of(std::string_view kind) {
    if (kind == "mesh") {
        return ".glb";
    }
    if (kind == "texture") {
        return ".dds";
    }
    if (kind == "script") {
        return ".pexfb";
    }
    if (kind == "lod") {
        return ".lodfb";
    }
    if (kind == "animation") {
        return ".animfb";
    }
    return {};
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
    const std::string_view ext = extension_of(entry.kind);
    if (blob_ != nullptr || ext.empty()) {
        return {};
    }
    return loose_dir_ + "/assets/" + entry.hash.substr(0, 2) + "/" + entry.hash + std::string(ext);
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
