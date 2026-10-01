// SPDX-License-Identifier: GPL-3.0-or-later
//
// Write-side counterpart of SpanReader.
//
// Writing floats and indices into a buffer needs reinterpret_cast, memcpy or
// bit_cast, which check-raw-access.sh rejects in the parser directories. This
// keeps that byte-level code in one place in src/io. It owns and grows its
// buffer; there is nothing to bounds-check because the output is ours.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace bethconv::io {

/// Writable as raw little-endian bytes.
template <typename T>
concept Writable = std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> &&
                   !std::is_member_pointer_v<T>;

/// Append-only little-endian byte buffer. glTF is little-endian, so scalars are
/// byte-swapped on big-endian hosts.
class ByteWriter {
public:
    ByteWriter() = default;

    /// Reserve capacity for `n` more bytes (allocation hint only).
    void reserve_more(std::size_t n) { buf_.reserve(buf_.size() + n); }

    /// Append one value in little-endian byte order.
    template <Writable T>
    void put(const T& value) {
        std::array<std::byte, sizeof(T)> raw{};
        std::memcpy(raw.data(), &value, sizeof(T));
        if constexpr (sizeof(T) > 1) {
            if constexpr (std::endian::native == std::endian::big) {
                std::ranges::reverse(raw);
            }
        }
        buf_.insert(buf_.end(), raw.begin(), raw.end());
    }

    /// Append raw bytes as is. The bulk path; put<std::byte>() per byte would be
    /// far too slow for texture data.
    void put_bytes(std::span<const std::byte> bytes) {
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    }

    /// Append every element of `values`, each little-endian.
    template <Writable T>
    void put_all(std::span<const T> values) {
        reserve_more(values.size() * sizeof(T));
        for (const T& v : values) {
            put(v);
        }
    }

    /// Append zero bytes until the length is a multiple of `alignment`. glTF
    /// requires bufferView offsets aligned to the component size.
    void align_to(std::size_t alignment);

    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] bool empty() const noexcept { return buf_.empty(); }

    /// Moves the accumulated bytes out; the writer is empty afterwards.
    [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(buf_); }

    [[nodiscard]] std::span<const std::byte> view() const noexcept { return buf_; }

private:
    std::vector<std::byte> buf_;
};

} // namespace bethconv::io
