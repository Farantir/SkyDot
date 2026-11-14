// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bit-level reading for SWF records (RECT, MATRIX, CXFORM, shape records).
// Reads past the end return zero and clear ok(), so a record is decoded in one
// go and checked once. Private to ui/.
#pragma once

#include "bethconv/ui/swf.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace bethconv::ui {

class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }
    void align() noexcept {
        if (bit_ != 0) {
            bit_ = 0;
            ++pos_;
        }
    }
    [[nodiscard]] std::size_t byte_position() const noexcept { return pos_ + (bit_ != 0 ? 1 : 0); }
    [[nodiscard]] std::size_t remaining() const noexcept {
        const auto at = byte_position();
        return at < data_.size() ? data_.size() - at : 0;
    }
    bool seek(std::size_t offset) noexcept {
        if (offset > data_.size()) {
            ok_ = false;
            return false;
        }
        pos_ = offset;
        bit_ = 0;
        return true;
    }
    void skip(std::size_t n) noexcept {
        align();
        if (n > data_.size() - std::min(pos_, data_.size())) {
            ok_ = false;
            pos_ = data_.size();
            return;
        }
        pos_ += n;
    }

    std::uint32_t ub(std::uint32_t bits) noexcept {
        std::uint32_t v = 0;
        for (std::uint32_t i = 0; i < bits; ++i) {
            if (pos_ >= data_.size()) {
                ok_ = false;
                return 0;
            }
            const auto byte = std::to_integer<std::uint32_t>(data_[pos_]);
            v = (v << 1) | ((byte >> (7 - bit_)) & 1U);
            if (++bit_ == 8) {
                bit_ = 0;
                ++pos_;
            }
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
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    std::uint32_t bit_ = 0;
    bool ok_ = true;
};

inline Rect read_rect(BitReader& r) {
    r.align();
    const std::uint32_t bits = r.ub(5);
    Rect rect;
    rect.x_min = r.sb(bits);
    rect.x_max = r.sb(bits);
    rect.y_min = r.sb(bits);
    rect.y_max = r.sb(bits);
    r.align();
    return rect;
}

inline Matrix read_matrix(BitReader& r) {
    r.align();
    Matrix m;
    if (r.ub(1) != 0) {
        const std::uint32_t bits = r.ub(5);
        m.a = r.fb(bits);
        m.d = r.fb(bits);
    }
    if (r.ub(1) != 0) {
        const std::uint32_t bits = r.ub(5);
        m.b = r.fb(bits);
        m.c = r.fb(bits);
    }
    const std::uint32_t bits = r.ub(5);
    m.tx = r.sb(bits);
    m.ty = r.sb(bits);
    r.align();
    return m;
}

/// SHAPE records (after the style arrays) into `shape.edges`; `version` is the
/// DefineShape number (1 for glyphs). Appends any styles a record brings.
void read_shape_records(BitReader& r, int version, Shape& shape);

} // namespace bethconv::ui
