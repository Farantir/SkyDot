// SPDX-License-Identifier: GPL-3.0-or-later
#include "save/save_file.hpp"

#include "save/bytes.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace skydot::save {

namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}

constexpr auto k_table = make_table();
std::filesystem::path path_of(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

constexpr char k_magic[4] = {'S', 'K', 'Y', 'S'};

} // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t c = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        c = k_table[(c ^ data[i]) & 0xFFU] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

void SaveWriter::add(std::string_view tag, std::uint32_t version, const std::vector<std::uint8_t>& bytes) {
    ByteWriter w;
    for (std::size_t i = 0; i < 4; ++i) {
        w.u8(i < tag.size() ? static_cast<std::uint8_t>(tag[i]) : ' ');
    }
    w.u32(version);
    w.u64(bytes.size());
    body_.insert(body_.end(), w.out.begin(), w.out.end());
    body_.insert(body_.end(), bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> SaveWriter::finish() const {
    ByteWriter w;
    for (const char m : k_magic) {
        w.u8(static_cast<std::uint8_t>(m));
    }
    w.u32(k_container_version);
    w.out.insert(w.out.end(), body_.begin(), body_.end());
    w.u32(crc32(w.out.data(), w.out.size()));
    return w.out;
}

bool SaveWriter::write_file(const std::string& path, const std::vector<std::uint8_t>& bytes, std::string& error) {
    namespace fs = std::filesystem;
    const fs::path target = path_of(path);
    fs::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + temporary.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            error = "cannot write " + temporary.string();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, target, ec);
    if (ec) {
        error = "cannot move " + temporary.string() + " to " + target.string() + ": " + ec.message();
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool SaveReader::read_file(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error) {
    std::ifstream in(path_of(path), std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    // Sized read: GCC 14 at -O2 reports a null dereference inside streambuf
    // when istreambuf_iterator is inlined.
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(in.tellg());
    in.seekg(0, std::ios::beg);
    if (size < 0) {
        error = "cannot read " + path;
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        error = "cannot read " + path;
        return false;
    }
    return true;
}

bool SaveReader::parse(std::vector<std::uint8_t> bytes, std::string& error) {
    sections_.clear();
    bytes_.clear();
    constexpr std::size_t k_header = 8;
    constexpr std::size_t k_trailer = 4;
    if (bytes.size() < k_header + k_trailer || bytes[0] != 'S' || bytes[1] != 'K' || bytes[2] != 'Y' ||
        bytes[3] != 'S') {
        error = "not a save";
        return false;
    }
    ByteReader tail(bytes.data() + bytes.size() - k_trailer, k_trailer);
    std::uint32_t stored = 0;
    tail.u32(stored);
    if (stored != crc32(bytes.data(), bytes.size() - k_trailer)) {
        error = "the save is damaged (checksum)";
        return false;
    }
    ByteReader r(bytes.data() + 4, bytes.size() - 4 - k_trailer);
    std::uint32_t container = 0;
    r.u32(container);
    if (container != k_container_version) {
        error = "save container version " + std::to_string(container) + " is not one this engine reads";
        return false;
    }
    std::size_t pos = k_header;
    const std::size_t end = bytes.size() - k_trailer;
    std::vector<SectionView> sections;
    while (pos < end) {
        if (end - pos < 16) {
            error = "the save ends inside a section header";
            return false;
        }
        SectionView s;
        s.tag.assign(reinterpret_cast<const char*>(bytes.data() + pos), 4);
        ByteReader h(bytes.data() + pos + 4, 12);
        std::uint64_t length = 0;
        h.u32(s.version);
        h.u64(length);
        pos += 16;
        if (length > end - pos) {
            error = "section " + s.tag + " runs past the end of the save";
            return false;
        }
        s.size = static_cast<std::size_t>(length);
        s.offset = pos;
        for (const auto& other : sections) {
            if (other.tag == s.tag) {
                error = "section " + s.tag + " appears twice";
                return false;
            }
        }
        sections.push_back(std::move(s));
        pos += static_cast<std::size_t>(length);
    }
    bytes_ = std::move(bytes);
    sections_ = std::move(sections);
    return true;
}

ByteReader SaveReader::open(const SectionView& section) const {
    return {bytes_.data() + section.offset, section.size};
}

const SectionView* SaveReader::find(std::string_view tag) const {
    for (const auto& s : sections_) {
        if (s.tag == tag) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace skydot::save
