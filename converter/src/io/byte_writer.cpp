// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/io/byte_writer.hpp"

namespace bethconv::io {

void ByteWriter::align_to(std::size_t alignment) {
    if (alignment <= 1) {
        return;
    }
    const std::size_t rem = buf_.size() % alignment;
    if (rem != 0) {
        buf_.insert(buf_.end(), alignment - rem, std::byte{0});
    }
}

} // namespace bethconv::io
