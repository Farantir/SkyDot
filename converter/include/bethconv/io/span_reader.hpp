// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bounds-checked reader. All untrusted input (archives, records, meshes,
// textures) is read through this type. There is no data() accessor and no way
// around the bounds checks; if something cannot be expressed, extend this class.
#pragma once

#include "bethconv/io/parse_error.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bethconv::io {

/// Readable as raw bytes and byte-order-normalizable.
template <typename T>
concept Readable = std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> &&
                   !std::is_member_pointer_v<T>;

/// A four-character tag ("TES4", "GRUP", "NIF "), stored as the raw
/// little-endian word so comparisons are a single integer compare.
struct FourCC {
    std::uint32_t value{};

    constexpr FourCC() = default;
    constexpr explicit FourCC(std::uint32_t v) noexcept : value(v) {}

    /// FourCC{"TES4"}. consteval, so a malformed literal fails to compile.
    consteval FourCC(const char (&s)[5]) noexcept
        : value(static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) |
                static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 8 |
                static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 16 |
                static_cast<std::uint32_t>(static_cast<unsigned char>(s[3])) << 24) {}

    friend constexpr bool operator==(FourCC, FourCC) noexcept = default;

    [[nodiscard]] std::string to_string() const;
};

/// Non-owning, bounds-checked cursor over a byte range.
///
/// Cheap to copy (three words). Error offsets are absolute within the original
/// file, even through subreaders.
class SpanReader {
public:
    SpanReader() = default;

    /// `origin` must outlive the reader; it is copied only into errors.
    explicit SpanReader(std::span<const std::byte> data, std::string_view origin) noexcept
        : data_(data), origin_(origin) {}

    // ---- cursor state -----------------------------------------------------

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= data_.size(); }
    [[nodiscard]] std::string_view origin() const noexcept { return origin_; }

    /// Absolute offset in the original file of the current cursor.
    [[nodiscard]] std::size_t absolute_position() const noexcept { return base_ + pos_; }

    // ---- reads ------------------------------------------------------------

    /// Read a trivially-copyable T in host byte order.
    ///
    /// Bethesda formats are little-endian. On big-endian hosts scalars are
    /// swapped and aggregates are rejected at compile time; read their fields
    /// individually.
    template <Readable T>
    [[nodiscard]] ParseResult<T> get() noexcept {
        if (remaining() < sizeof(T)) {
            return fail(ErrorKind::truncated, need_detail(sizeof(T)));
        }
        T value;
        std::memcpy(&value, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return normalize(value);
    }

    /// Read a T without advancing the cursor.
    template <Readable T>
    [[nodiscard]] ParseResult<T> peek() const noexcept {
        SpanReader copy = *this;
        return copy.get<T>();
    }

    /// Read a 4-byte tag. Unlike get<FourCC>(), never byte-swapped.
    [[nodiscard]] ParseResult<FourCC> tag() noexcept;

    /// Read a 4-byte tag without advancing, for formats that dispatch on it
    /// (GRUP vs. record). Not peek<uint32_t>(), which would swap.
    [[nodiscard]] ParseResult<FourCC> peek_tag() const noexcept {
        SpanReader copy = *this;
        return copy.tag();
    }

    /// Borrow `n` bytes and advance. The span points into the caller's buffer.
    [[nodiscard]] ParseResult<std::span<const std::byte>> bytes(std::size_t n) noexcept;

    /// Exactly `n` bytes as text, untrimmed. Length-prefixed entries are taken
    /// verbatim: a trailing space in dialogue is meaningful.
    [[nodiscard]] ParseResult<std::string_view> chars(std::size_t n) noexcept;

    /// Fixed-length char array, not NUL-terminated on disk. Trailing NULs and
    /// spaces are trimmed.
    [[nodiscard]] ParseResult<std::string_view> fixed_string(std::size_t n) noexcept;

    /// NUL-terminated string; the NUL is consumed and excluded. A missing
    /// terminator is an error.
    [[nodiscard]] ParseResult<std::string_view> zstring() noexcept;

    /// uint8 length including a trailing NUL, then the bytes ("bzstring").
    [[nodiscard]] ParseResult<std::string_view> bzstring() noexcept;

    /// uint16 length, then the bytes, no terminator ("wstring", BSA names).
    [[nodiscard]] ParseResult<std::string_view> wstring() noexcept;

    /// `count` contiguous elements with one bounds check. Aggregates are
    /// rejected on big-endian hosts, as for get<T>().
    template <Readable T>
    [[nodiscard]] ParseResult<std::vector<T>> array(std::size_t count) noexcept {
        static_assert(std::endian::native == std::endian::little ||
                          std::is_scalar_v<T> || std::is_enum_v<T>,
                      "array<T> of an aggregate is little-endian-host only");
        // count * sizeof(T) can overflow on a hostile count.
        if (sizeof(T) != 0 && count > remaining() / sizeof(T)) {
            return fail(ErrorKind::truncated,
                        "array of " + std::to_string(count) + " x " +
                            std::to_string(sizeof(T)) + " bytes, have " +
                            std::to_string(remaining()));
        }
        std::vector<T> out(count);
        if (count != 0) {
            std::memcpy(out.data(), data_.data() + pos_, count * sizeof(T));
            pos_ += count * sizeof(T);
            if constexpr (std::endian::native != std::endian::little) {
                for (T& v : out) {
                    v = *normalize(v);
                }
            }
        }
        return out;
    }

    // ---- navigation -------------------------------------------------------

    /// Seek to an offset relative to the start of this reader's range.
    [[nodiscard]] ParseResult<void> seek(std::size_t offset) noexcept;

    /// Advance by `n`; fails instead of clamping.
    [[nodiscard]] ParseResult<void> skip(std::size_t n) noexcept;

    /// Take the next `n` bytes as a child reader and advance past them. The
    /// child cannot read outside the parent's range, so nested structures need
    /// no size arithmetic in the parser.
    [[nodiscard]] ParseResult<SpanReader> subreader(std::size_t n) noexcept;

    /// A child reader at an offset in this reader's range, without moving the
    /// cursor. For offset tables (NIF strings, BSA name blocks).
    [[nodiscard]] ParseResult<SpanReader> subreader_at(std::size_t offset,
                                                       std::size_t n) const noexcept;

    // ---- diagnostics ------------------------------------------------------

    /// An error at the current absolute offset, for semantic errors the reader
    /// cannot detect itself.
    [[nodiscard]] std::unexpected<ParseError> fail(ErrorKind kind, std::string detail) const;

private:
    template <Readable T>
    [[nodiscard]] static ParseResult<T> normalize(T value) noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            return value;
        } else if constexpr (std::is_scalar_v<T> || std::is_enum_v<T>) {
            std::array<std::byte, sizeof(T)> raw;
            std::memcpy(raw.data(), &value, sizeof(T));
            std::ranges::reverse(raw);
            T swapped;
            std::memcpy(&swapped, raw.data(), sizeof(T));
            return swapped;
        } else {
            static_assert(sizeof(T) == 0,
                          "get<T>() of an aggregate is little-endian-host only; "
                          "read the fields individually");
        }
    }

    [[nodiscard]] std::string need_detail(std::size_t n) const;

    SpanReader(std::span<const std::byte> data, std::string_view origin,
               std::size_t base) noexcept
        : data_(data), origin_(origin), base_(base) {}

    std::span<const std::byte> data_{};
    std::string_view origin_{};
    std::size_t pos_{};
    std::size_t base_{}; ///< Absolute offset of data_[0] in the original file.
};

} // namespace bethconv::io
