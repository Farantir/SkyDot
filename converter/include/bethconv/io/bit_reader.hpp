// SPDX-License-Identifier: GPL-3.0-or-later
//
// MSB-first bit fields over a SpanReader (SWF records). Bytes come from the
// SpanReader one at a time; reads past the end return zero and clear ok(), so
// a record is decoded in one go and checked once.
#pragma once

#include "bethconv/io/span_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bethconv::io {

class BitReader {
public:
    /// `origin` must outlive the reader.
    explicit BitReader(std::span<const std::byte> data, std::string_view origin = "bits") noexcept
        : bytes_(data, origin) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }
    /// Drop the rest of a partly read byte.
    void align() noexcept { bit_ = 0; }
    /// The next whole byte; a partly read byte counts as read.
    [[nodiscard]] std::size_t byte_position() const noexcept { return bytes_.position(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.remaining(); }
    bool seek(std::size_t offset) noexcept {
        bit_ = 0;
        if (!bytes_.seek(offset)) {
            ok_ = false;
            return false;
        }
        return true;
    }
    /// Past the end: clears ok() and stops at the end.
    void skip(std::size_t n) noexcept {
        align();
        if (!bytes_.skip(n)) {
            ok_ = false;
            (void)bytes_.seek(bytes_.size());
        }
    }

    std::uint32_t ub(std::uint32_t bits) noexcept {
        std::uint32_t v = 0;
        for (std::uint32_t i = 0; i < bits; ++i) {
            if (bit_ == 0) {
                const auto byte = bytes_.get<std::uint8_t>();
                if (!byte) {
                    ok_ = false;
                    return 0;
                }
                current_ = *byte;
            }
            v = (v << 1) | ((current_ >> (7 - bit_)) & 1U);
            bit_ = (bit_ + 1) % 8;
        }
        return v;
    }
    std::int32_t sb(std::uint32_t bits) noexcept {
        if (bits == 0) {
            return 0;
        }
        const std::uint32_t v = ub(bits);
        if (bits < 32 && (v & (1U << (bits - 1))) != 0) {
            return static_cast<std::int32_t>(v | ~((1U << bits) - 1U));
        }
        return static_cast<std::int32_t>(v);
    }
    /// 16.16 fixed point.
    double fb(std::uint32_t bits) noexcept { return static_cast<double>(sb(bits)) / 65536.0; }

    // Little-endian, byte aligned.
    std::uint8_t u8() noexcept {
        align();
        return static_cast<std::uint8_t>(ub(8));
    }
    std::uint16_t u16() noexcept {
        const std::uint32_t lo = u8();
        return static_cast<std::uint16_t>(lo | (static_cast<std::uint32_t>(u8()) << 8));
    }
    std::int16_t s16() noexcept { return static_cast<std::int16_t>(u16()); }
    std::uint32_t u32() noexcept {
        const std::uint32_t lo = u16();
        return lo | (static_cast<std::uint32_t>(u16()) << 16);
    }

private:
    SpanReader bytes_;
    std::uint32_t current_ = 0;
    std::uint32_t bit_ = 0;
    bool ok_ = true;
};

} // namespace bethconv::io
