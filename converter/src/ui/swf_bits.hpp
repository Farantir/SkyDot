// SPDX-License-Identifier: GPL-3.0-or-later
//
// SWF records read bit by bit (RECT, MATRIX, shape records) with
// io::BitReader. Private to ui/.
#pragma once

#include "bethconv/io/bit_reader.hpp"
#include "bethconv/ui/swf.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace bethconv::ui {

using io::BitReader;

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
