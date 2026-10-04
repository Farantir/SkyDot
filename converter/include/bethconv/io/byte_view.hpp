// SPDX-License-Identifier: GPL-3.0-or-later
//
// A byte buffer seen as another one-byte type: unsigned chars for the
// FlatBuffers verifier, chars for iostreams.
//
// Each view is one reinterpret_cast, which check-raw-access.sh rejects in the
// parser directories. Keeping the casts here, next to ByteWriter and
// SpanReader, leaves one place to read; the length never changes and the view
// borrows the buffer it is given.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bethconv::io {

/// `bytes` as unsigned chars, for APIs that take `const uint8_t*`.
[[nodiscard]] inline std::span<const std::uint8_t> as_u8(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

/// `bytes` as chars, for `std::ostream::write`.
[[nodiscard]] inline std::span<const char> as_chars(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// The same for a buffer that is already unsigned chars (a FlatBuffers builder).
[[nodiscard]] inline std::span<const char> as_chars(std::span<const std::uint8_t> bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace bethconv::io
