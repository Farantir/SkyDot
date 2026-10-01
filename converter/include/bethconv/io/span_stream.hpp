// SPDX-License-Identifier: GPL-3.0-or-later
//
// std::istream over a byte span, for nifly, which reads from streams.
//
// Bridging needs a `char*` cast for std::streambuf::setg, which the raw-access
// check rejects in src/mesh. It lives here in src/io, like MappedFile. The
// alternative, a temp file per mesh, would be much slower.
#pragma once

#include <cstddef>
#include <filesystem>
#include <istream>
#include <span>
#include <streambuf>
#include <string>

namespace bethconv::io {

/// Read-only streambuf over borrowed bytes. Does not copy and does not own.
class SpanStreamBuf final : public std::streambuf {
public:
    explicit SpanStreamBuf(std::span<const std::byte> bytes);

protected:
    /// The default streambuf cannot seek a user-provided get area; nifly seeks
    /// when following block references.
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode which) override;
    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override;

private:
    char* begin_{};
    char* end_{};
};

/// Write `bytes` to `path`, creating parent directories. Here because
/// std::ofstream needs a `const char*` cast.
[[nodiscard]] bool write_file(const std::filesystem::path& path,
                              std::span<const std::byte> bytes, std::string& out_error);

/// An istream reading directly out of `bytes`, which must outlive the stream.
class SpanStream final : public std::istream {
public:
    explicit SpanStream(std::span<const std::byte> bytes)
        : std::istream(&buf_), buf_(bytes) {}

private:
    SpanStreamBuf buf_;
};

} // namespace bethconv::io
