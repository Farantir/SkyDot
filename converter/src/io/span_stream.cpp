// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/io/span_stream.hpp"

#include <fstream>
#include <system_error>

namespace bethconv::io {

SpanStreamBuf::SpanStreamBuf(std::span<const std::byte> bytes) {
    // const_cast is safe: the get area is never written; streambuf's pointers
    // are just non-const by interface.
    begin_ = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
    end_ = begin_ + bytes.size();
    setg(begin_, begin_, end_);
}

std::streambuf::pos_type SpanStreamBuf::seekoff(off_type off, std::ios_base::seekdir dir,
                                                std::ios_base::openmode which) {
    if ((which & std::ios_base::in) == 0) {
        return pos_type(off_type(-1));
    }

    off_type base = 0;
    switch (dir) {
    case std::ios_base::beg:
        base = 0;
        break;
    case std::ios_base::cur:
        base = gptr() - begin_;
        break;
    case std::ios_base::end:
        base = end_ - begin_;
        break;
    default:
        return pos_type(off_type(-1));
    }

    const off_type target = base + off;
    if (target < 0 || target > end_ - begin_) {
        return pos_type(off_type(-1));
    }
    setg(begin_, begin_ + target, end_);
    return pos_type(target);
}

bool write_file(const std::filesystem::path& path, std::span<const std::byte> bytes,
                std::string& out_error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            out_error = "cannot create " + path.parent_path().string() + ": " + ec.message();
            return false;
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        out_error = "cannot open " + path.string();
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        out_error = "write failed: " + path.string();
        return false;
    }
    return true;
}

std::streambuf::pos_type SpanStreamBuf::seekpos(pos_type pos, std::ios_base::openmode which) {
    return seekoff(off_type(pos), std::ios_base::beg, which);
}

} // namespace bethconv::io
