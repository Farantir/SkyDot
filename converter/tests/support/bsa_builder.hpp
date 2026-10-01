// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic BSA fixtures for the archive tests.
//
// The parser is rsm-bsa, so fixtures are written with rsm-bsa's own writer.
// The tests cover our wrapper (vpath normalization, precedence, conflicts, read
// dispatch, version-to-codec); malformed archives come from the fuzzer.
#pragma once

#include <bsa/tes4.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::test {

/// One file in a synthetic archive. `vpath` may use either separator; it is
/// split at the last one into rsm-bsa's directory and file keys.
struct BsaEntry {
    std::string vpath;
    std::string contents;
};

/// Write a TES4 BSA. `version` picks the format and codec: `tes5` (104) zlib,
/// `sse` (105) LZ4. With `compress`, every entry is compressed, so the codec
/// choice in `ArchiveSet::read()` is exercised.
inline void write_bsa(const std::filesystem::path& path,
                      std::span<const BsaEntry> entries,
                      bsa::tes4::version version = bsa::tes4::version::sse,
                      bool compress = false) {
    bsa::tes4::archive archive;

    for (const auto& entry : entries) {
        const auto cut = entry.vpath.find_last_of("/\\");
        const std::string dir =
            cut == std::string::npos ? std::string{} : entry.vpath.substr(0, cut);
        const std::string name =
            cut == std::string::npos ? entry.vpath : entry.vpath.substr(cut + 1);

        bsa::tes4::file file;
        file.read(std::as_bytes(std::span{entry.contents}), version,
                  bsa::tes4::compression_codec::normal,
                  bsa::compression_type::decompressed, bsa::copy_type::deep);
        if (compress) {
            file.compress(version);
        }

        const auto dir_it =
            archive.insert(bsa::tes4::directory::key{dir}, bsa::tes4::directory{}).first;
        dir_it->second.insert(bsa::tes4::file::key{name}, std::move(file));
    }

    archive.archive_flags(bsa::tes4::archive_flag::directory_strings |
                          bsa::tes4::archive_flag::file_strings |
                          (compress ? bsa::tes4::archive_flag::compressed
                                    : bsa::tes4::archive_flag::none));
    archive.archive_types(bsa::tes4::archive_type::misc);
    archive.write(path, version);
}

inline void write_bsa(const std::filesystem::path& path,
                      std::initializer_list<BsaEntry> entries,
                      bsa::tes4::version version = bsa::tes4::version::sse,
                      bool compress = false) {
    write_bsa(path, std::span<const BsaEntry>{entries.begin(), entries.size()}, version,
              compress);
}

} // namespace bethconv::test
