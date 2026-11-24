// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/ui/swf.hpp"

#include "bethconv/io/byte_view.hpp"
#include "bethconv/io/deflate.hpp"
#include "bethconv/io/span_reader.hpp"
#include "swf_bits.hpp"


#include <algorithm>
#include <string>
#include <utility>

namespace bethconv::ui {

using io::ErrorKind;
using io::ParseError;

Matrix Matrix::then(const Matrix& o) const noexcept {
    // p' = o(this(p))
    Matrix m;
    m.a = o.a * a + o.c * b;
    m.b = o.b * a + o.d * b;
    m.c = o.a * c + o.c * d;
    m.d = o.b * c + o.d * d;
    m.tx = o.a * tx + o.c * ty + o.tx;
    m.ty = o.b * tx + o.d * ty + o.ty;
    return m;
}

ColorTransform ColorTransform::then(const ColorTransform& o) const noexcept {
    ColorTransform t;
    t.mr = mr * o.mr;
    t.mg = mg * o.mg;
    t.mb = mb * o.mb;
    t.ma = ma * o.ma;
    t.ar = ar * o.mr + o.ar;
    t.ag = ag * o.mg + o.ag;
    t.ab = ab * o.mb + o.ab;
    t.aa = aa * o.ma + o.aa;
    return t;
}

std::string tag_name(std::uint16_t code) {
    switch (code) {
    case 0: return "End";
    case 1: return "ShowFrame";
    case 2: return "DefineShape";
    case 4: return "PlaceObject";
    case 5: return "RemoveObject";
    case 6: return "DefineBits";
    case 7: return "DefineButton";
    case 8: return "JPEGTables";
    case 9: return "SetBackgroundColor";
    case 10: return "DefineFont";
    case 11: return "DefineText";
    case 12: return "DoAction";
    case 13: return "DefineFontInfo";
    case 14: return "DefineSound";
    case 18: return "SoundStreamHead";
    case 20: return "DefineBitsLossless";
    case 21: return "DefineBitsJPEG2";
    case 22: return "DefineShape2";
    case 24: return "Protect";
    case 26: return "PlaceObject2";
    case 28: return "RemoveObject2";
    case 32: return "DefineShape3";
    case 33: return "DefineText2";
    case 34: return "DefineButton2";
    case 35: return "DefineBitsJPEG3";
    case 36: return "DefineBitsLossless2";
    case 37: return "DefineEditText";
    case 39: return "DefineSprite";
    case 43: return "FrameLabel";
    case 46: return "DefineMorphShape";
    case 48: return "DefineFont2";
    case 56: return "ExportAssets";
    case 57: return "ImportAssets";
    case 59: return "DoInitAction";
    case 62: return "DefineFontInfo2";
    case 69: return "FileAttributes";
    case 70: return "PlaceObject3";
    case 71: return "ImportAssets2";
    case 73: return "DefineFontAlignZones";
    case 74: return "CSMTextSettings";
    case 75: return "DefineFont3";
    case 76: return "SymbolClass";
    case 77: return "Metadata";
    case 78: return "DefineScalingGrid";
    case 82: return "DoABC";
    case 83: return "DefineShape4";
    case 84: return "DefineMorphShape2";
    case 86: return "DefineSceneAndFrameLabelData";
    case 88: return "DefineFontName";
    case 90: return "DefineBitsJPEG4";
    case 91: return "DefineFont4";
    default: break;
    }
    if (code >= 1000) {
        return "GFx_" + std::to_string(code);
    }
    return "Tag_" + std::to_string(code);
}

io::ParseResult<SwfBody> decompress_swf(std::span<const std::byte> file, std::string_view origin) {
    const auto fail = [&](ErrorKind kind, std::size_t offset, std::string detail) {
        return std::unexpected(ParseError{.origin = std::string(origin),
                                          .offset = offset,
                                          .kind = kind,
                                          .detail = std::move(detail)});
    };
    SwfBody out;
    io::SpanReader header(file, origin);
    const auto signature = header.chars(3);
    const auto version = header.get<std::uint8_t>();
    const auto declared = header.get<std::uint32_t>();
    if (!signature || !version || !declared) {
        return fail(ErrorKind::truncated, 0, "shorter than the 8-byte SWF header");
    }
    out.signature = *signature;
    out.version = *version;
    const std::uint32_t length = *declared;
    if (length < 8) {
        return fail(ErrorKind::bad_value, 4, "file length " + std::to_string(length));
    }
    const std::string& sig = out.signature;
    if (sig == "FWS" || sig == "GFX") {
        const auto body = header.bytes(std::min<std::size_t>(file.size(), length) - 8);
        if (!body) {
            return std::unexpected(body.error());
        }
        out.body.assign(body->begin(), body->end());
        return out;
    }
    if (sig == "CWS" || sig == "CFX") {
        // Not inflate_exact: some movies (fonts_pl.swf) end their zlib stream
        // short of its end marker once the declared length is out. The
        // declared length is what counts, as for the player.
        const std::size_t want = length - 8;
        if (want > io::k_max_inflate_size) {
            return fail(ErrorKind::too_large, 4, "declares " + std::to_string(want) + " bytes");
        }
        auto body = io::inflate_prefix(*header.bytes(header.remaining()), want, origin, 8);
        if (!body) {
            return std::unexpected(std::move(body).error());
        }
        out.body = std::move(*body);
        return out;
    }
    if (sig == "ZWS") {
        return fail(ErrorKind::unsupported, 0, "LZMA-compressed SWF (ZWS)");
    }
    return fail(ErrorKind::bad_magic, 0, "not a SWF (signature '" + sig + "')");
}

io::ParseResult<std::vector<RawTag>> split_tags(std::span<const std::byte> stream,
                                                std::size_t base_offset, std::string_view origin) {
    std::vector<RawTag> tags;
    io::SpanReader r(stream, origin);
    while (r.remaining() >= 2) {
        const std::size_t start = r.position();
        const std::uint32_t header = *r.get<std::uint16_t>();
        const auto code = static_cast<std::uint16_t>(header >> 6);
        std::size_t length = header & 0x3FU;
        if (length == 0x3F) {
            const auto long_length = r.get<std::uint32_t>();
            if (!long_length) {
                break;
            }
            length = *long_length;
        }
        const auto data = r.bytes(length);
        if (!data) {
            return std::unexpected(ParseError{.origin = std::string(origin),
                                              .offset = base_offset + start,
                                              .kind = ErrorKind::truncated,
                                              .detail = tag_name(code) + " tag of " +
                                                        std::to_string(length) +
                                                        " bytes runs past the stream"});
        }
        tags.push_back(RawTag{.code = code, .data = *data, .offset = base_offset + start});
        if (code == tag_end) {
            break;
        }
    }
    return tags;
}

namespace {

Rgba read_rgb(BitReader& r) {
    Rgba c;
    c.r = r.u8();
    c.g = r.u8();
    c.b = r.u8();
    return c;
}

Rgba read_rgba(BitReader& r) {
    Rgba c = read_rgb(r);
    c.a = r.u8();
    return c;
}

/// Lossless2 colours are premultiplied; ARGB order in format 5.
std::uint8_t unpremultiply(std::uint8_t c, std::uint8_t a) {
    if (a == 0) {
        return 0;
    }
    const unsigned v = (static_cast<unsigned>(c) * 255U + a / 2U) / a;
    return static_cast<std::uint8_t>(std::min(v, 255U));
}

FillStyle read_fill(BitReader& r, int shape_version) {
    FillStyle f;
    f.type = r.u8();
    if (f.type == FillStyle::solid) {
        f.color = shape_version >= 3 ? read_rgba(r) : read_rgb(r);
    } else if (f.is_gradient()) {
        f.matrix = read_matrix(r);
        r.align();
        f.spread = static_cast<std::uint8_t>(r.ub(2));
        f.interpolation = static_cast<std::uint8_t>(r.ub(2));
        const auto count = r.ub(4);
        for (std::uint32_t i = 0; i < count; ++i) {
            GradientStop s;
            s.ratio = r.u8();
            s.color = shape_version >= 3 ? read_rgba(r) : read_rgb(r);
            f.stops.push_back(s);
        }
        if (f.type == FillStyle::focal) {
            f.focal_point = static_cast<double>(r.s16()) / 256.0;
        }
    } else if (f.is_bitmap()) {
        f.bitmap_id = r.u16();
        f.matrix = read_matrix(r);
    } else {
        r.fail();
    }
    return f;
}

void read_styles(BitReader& r, int version, Shape& shape) {
    std::uint32_t fills = r.u8();
    if (fills == 0xFF && version >= 2) {
        fills = r.u16();
    }
    for (std::uint32_t i = 0; i < fills && r.ok(); ++i) {
        shape.fills.push_back(read_fill(r, version));
    }
    std::uint32_t lines = r.u8();
    if (lines == 0xFF) {
        lines = r.u16();
    }
    for (std::uint32_t i = 0; i < lines && r.ok(); ++i) {
        LineStyle l;
        l.width = r.u16();
        if (version >= 4) {
            l.start_cap = static_cast<std::uint8_t>(r.ub(2));
            l.join = static_cast<std::uint8_t>(r.ub(2));
            const bool has_fill = r.ub(1) != 0;
            r.ub(3); // NoHScale, NoVScale, PixelHinting
            r.ub(5);
            l.no_close = r.ub(1) != 0;
            l.end_cap = static_cast<std::uint8_t>(r.ub(2));
            if (l.join == 2) {
                l.miter_limit = static_cast<double>(r.u16()) / 256.0;
            }
            if (has_fill) {
                l.fill = read_fill(r, version);
                if (l.fill->type == FillStyle::solid) {
                    l.color = l.fill->color;
                } else if (!l.fill->stops.empty()) {
                    l.color = l.fill->stops.front().color;
                }
            } else {
                l.color = read_rgba(r);
            }
        } else {
            l.color = version >= 3 ? read_rgba(r) : read_rgb(r);
        }
        shape.lines.push_back(std::move(l));
    }
}

} // namespace

void read_shape_records(BitReader& r, int version, Shape& shape) {
    std::uint32_t fill_bits = r.ub(4);
    std::uint32_t line_bits = r.ub(4);
    std::uint32_t fill_base = 0;
    std::uint32_t line_base = 0;
    std::uint32_t layer = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t fill0 = 0, fill1 = 0, line = 0;
    bool move = true;
    const auto offset = [](std::uint32_t index, std::uint32_t base) {
        return index == 0 ? 0U : index + base;
    };
    while (r.ok()) {
        if (r.ub(1) == 0) {
            const std::uint32_t flags = r.ub(5);
            if (flags == 0) {
                break;
            }
            const bool new_styles = (flags & 0x10U) != 0;
            if ((flags & 0x01U) != 0) {
                const std::uint32_t bits = r.ub(5);
                x = r.sb(bits);
                y = r.sb(bits);
            }
            move = true;
            // The style indices of this record are into the arrays it brings
            // when it brings some.
            std::uint32_t f0 = 0, f1 = 0, ln = 0;
            const bool has_f0 = (flags & 0x02U) != 0;
            const bool has_f1 = (flags & 0x04U) != 0;
            const bool has_ln = (flags & 0x08U) != 0;
            if (has_f0) {
                f0 = r.ub(fill_bits);
            }
            if (has_f1) {
                f1 = r.ub(fill_bits);
            }
            if (has_ln) {
                ln = r.ub(line_bits);
            }
            if (new_styles && version >= 2) {
                r.align();
                fill_base = static_cast<std::uint32_t>(shape.fills.size());
                line_base = static_cast<std::uint32_t>(shape.lines.size());
                read_styles(r, version, shape);
                fill_bits = r.ub(4);
                line_bits = r.ub(4);
                ++layer;
                // New styles reset the current ones unless set here.
                fill0 = fill1 = line = 0;
            }
            if (has_f0) {
                fill0 = offset(f0, fill_base);
            }
            if (has_f1) {
                fill1 = offset(f1, fill_base);
            }
            if (has_ln) {
                line = offset(ln, line_base);
            }
            continue;
        }
        Edge e;
        e.x0 = x;
        e.y0 = y;
        e.fill0 = fill0;
        e.fill1 = fill1;
        e.line = line;
        e.layer = layer;
        e.move_before = move;
        move = false;
        const bool straight = r.ub(1) != 0;
        const std::uint32_t bits = r.ub(4) + 2;
        if (straight) {
            std::int32_t dx = 0, dy = 0;
            if (r.ub(1) != 0) {
                dx = r.sb(bits);
                dy = r.sb(bits);
            } else if (r.ub(1) != 0) {
                dy = r.sb(bits);
            } else {
                dx = r.sb(bits);
            }
            e.x1 = x + dx;
            e.y1 = y + dy;
            e.cx = (e.x0 + e.x1) / 2;
            e.cy = (e.y0 + e.y1) / 2;
        } else {
            e.curved = true;
            e.cx = x + r.sb(bits);
            e.cy = y + r.sb(bits);
            e.x1 = e.cx + r.sb(bits);
            e.y1 = e.cy + r.sb(bits);
        }
        x = e.x1;
        y = e.y1;
        shape.edges.push_back(e);
    }
}

namespace {

std::optional<Shape> read_define_shape(std::span<const std::byte> data, std::uint16_t code) {
    BitReader r(data);
    Shape s;
    s.version = code == tag_define_shape    ? 1
                : code == tag_define_shape2 ? 2
                : code == tag_define_shape3 ? 3
                                            : 4;
    s.id = r.u16();
    s.bounds = read_rect(r);
    if (s.version == 4) {
        (void)read_rect(r); // edge bounds
        r.align();
        r.ub(5);
        s.nonzero_winding = r.ub(1) != 0;
        r.ub(2);
    }
    r.align();
    read_styles(r, s.version, s);
    read_shape_records(r, s.version, s);
    if (!r.ok()) {
        return std::nullopt;
    }
    return s;
}

std::vector<std::uint8_t> inflate_bytes(std::span<const std::byte> data, std::size_t size,
                                        bool& ok) {
    auto out = io::inflate_exact(data, size, "bitmap", 0);
    if (!out) {
        ok = false;
        return {};
    }
    const auto bytes = io::as_u8(*out);
    return {bytes.begin(), bytes.end()};
}

std::optional<Bitmap> read_lossless(std::span<const std::byte> data, std::uint16_t code) {
    BitReader r(data);
    Bitmap b;
    b.tag = code;
    b.id = r.u16();
    const std::uint8_t format = r.u8();
    b.width = r.u16();
    b.height = r.u16();
    const bool alpha = code == tag_define_bits_lossless2;
    std::uint32_t table = 0;
    if (format == 3) {
        table = static_cast<std::uint32_t>(r.u8()) + 1;
    }
    if (!r.ok() || b.width == 0 || b.height == 0) {
        return std::nullopt;
    }
    const std::size_t w = b.width;
    const std::size_t h = b.height;
    const auto rest = data.subspan(r.byte_position());
    bool ok = true;
    b.rgba.resize(w * h * 4);
    if (format == 3) {
        const std::size_t entry = alpha ? 4 : 3;
        const std::size_t pitch = (w + 3) & ~std::size_t{3};
        const auto px = inflate_bytes(rest, table * entry + pitch * h, ok);
        if (!ok) {
            return std::nullopt;
        }
        for (std::size_t yy = 0; yy < h; ++yy) {
            for (std::size_t xx = 0; xx < w; ++xx) {
                const std::size_t index = px[table * entry + yy * pitch + xx];
                std::uint8_t* o = &b.rgba[(yy * w + xx) * 4];
                if (index >= table) {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }
                const std::uint8_t* c = &px[index * entry];
                const std::uint8_t a = alpha ? c[3] : 255;
                o[0] = alpha ? unpremultiply(c[0], a) : c[0];
                o[1] = alpha ? unpremultiply(c[1], a) : c[1];
                o[2] = alpha ? unpremultiply(c[2], a) : c[2];
                o[3] = a;
            }
        }
    } else if (format == 4 && !alpha) {
        const std::size_t pitch = (w * 2 + 3) & ~std::size_t{3};
        const auto px = inflate_bytes(rest, pitch * h, ok);
        if (!ok) {
            return std::nullopt;
        }
        for (std::size_t yy = 0; yy < h; ++yy) {
            for (std::size_t xx = 0; xx < w; ++xx) {
                const unsigned v = (static_cast<unsigned>(px[yy * pitch + xx * 2]) << 8) |
                                   px[yy * pitch + xx * 2 + 1];
                std::uint8_t* o = &b.rgba[(yy * w + xx) * 4];
                o[0] = static_cast<std::uint8_t>(((v >> 10) & 31U) * 255U / 31U);
                o[1] = static_cast<std::uint8_t>(((v >> 5) & 31U) * 255U / 31U);
                o[2] = static_cast<std::uint8_t>((v & 31U) * 255U / 31U);
                o[3] = 255;
            }
        }
    } else if (format == 5) {
        const auto px = inflate_bytes(rest, w * h * 4, ok);
        if (!ok) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < w * h; ++i) {
            const std::uint8_t a = alpha ? px[i * 4] : 255;
            std::uint8_t* o = &b.rgba[i * 4];
            o[0] = alpha ? unpremultiply(px[i * 4 + 1], a) : px[i * 4 + 1];
            o[1] = alpha ? unpremultiply(px[i * 4 + 2], a) : px[i * 4 + 2];
            o[2] = alpha ? unpremultiply(px[i * 4 + 3], a) : px[i * 4 + 3];
            o[3] = a;
        }
    } else {
        return std::nullopt;
    }
    return b;
}

/// JPEG data in SWF may start with an extra EOI/SOI pair (FF D9 FF D8) and
/// DefineBits leaves its tables to JPEGTables; the result is one JPEG.
std::vector<std::byte> clean_jpeg(std::span<const std::byte> tables,
                                  std::span<const std::byte> image) {
    const auto strip_bad_header = [](std::span<const std::byte> s) {
        if (s.size() >= 4 && s[0] == std::byte{0xFF} && s[1] == std::byte{0xD9} &&
            s[2] == std::byte{0xFF} && s[3] == std::byte{0xD8}) {
            return s.subspan(4);
        }
        return s;
    };
    tables = strip_bad_header(tables);
    image = strip_bad_header(image);
    std::vector<std::byte> out;
    if (tables.size() >= 4) {
        // Tables minus their EOI, image minus its SOI.
        out.assign(tables.begin(), tables.end() - 2);
        if (image.size() >= 2 && image[0] == std::byte{0xFF} && image[1] == std::byte{0xD8}) {
            image = image.subspan(2);
        }
    }
    out.insert(out.end(), image.begin(), image.end());
    return out;
}

std::string sniff_image(std::span<const std::byte> data) {
    if (data.size() >= 8 && data[0] == std::byte{0x89} && data[1] == std::byte{'P'}) {
        return "png";
    }
    if (data.size() >= 3 && data[0] == std::byte{'G'} && data[1] == std::byte{'I'}) {
        return "gif";
    }
    return "jpeg";
}

std::optional<Font> read_font23(std::span<const std::byte> data, std::uint16_t code) {
    BitReader r(data);
    Font f;
    f.tag = code;
    f.units_per_em = code == tag_define_font3 ? 20480 : 1024;
    f.id = r.u16();
    const std::uint8_t flags = r.u8();
    f.has_layout = (flags & 0x80U) != 0;
    f.small_text = (flags & 0x20U) != 0;
    const bool wide_offsets = (flags & 0x08U) != 0;
    const bool wide_codes = (flags & 0x04U) != 0;
    f.italic = (flags & 0x02U) != 0;
    f.bold = (flags & 0x01U) != 0;
    (void)r.u8(); // language
    const std::uint8_t name_len = r.u8();
    for (std::uint32_t i = 0; i < name_len; ++i) {
        const char c = static_cast<char>(r.u8());
        if (c != '\0') {
            f.name.push_back(c);
        }
    }
    const std::uint16_t count = r.u16();
    const std::size_t table_start = r.byte_position();
    std::vector<std::uint32_t> offsets(count);
    for (auto& o : offsets) {
        o = wide_offsets ? r.u32() : r.u16();
    }
    std::uint32_t code_table = 0;
    if (count > 0) {
        code_table = wide_offsets ? r.u32() : r.u16();
    } else if (r.remaining() >= (wide_offsets ? 4U : 2U)) {
        code_table = wide_offsets ? r.u32() : r.u16();
    }
    if (!r.ok()) {
        return std::nullopt;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t begin = table_start + offsets[i];
        const std::size_t end =
            table_start + (i + 1 < count ? offsets[i + 1] : code_table);
        if (begin > end || end > data.size()) {
            return std::nullopt;
        }
        BitReader g(data.subspan(begin, end - begin));
        Shape glyph;
        glyph.version = 0;
        glyph.fills.push_back(FillStyle{});
        read_shape_records(g, 1, glyph);
        f.glyphs.push_back(std::move(glyph));
    }
    if (count > 0) {
        if (!r.seek(table_start + code_table)) {
            return std::nullopt;
        }
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        f.codes.push_back(wide_codes ? r.u16() : r.u8());
    }
    if (f.has_layout && r.ok()) {
        f.ascent = r.u16();
        f.descent = r.u16();
        f.leading = r.s16();
        for (std::uint16_t i = 0; i < count; ++i) {
            f.advances.push_back(r.s16());
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            f.glyph_bounds.push_back(read_rect(r));
        }
        r.align();
        const std::uint16_t kerns = r.remaining() >= 2 ? r.u16() : 0;
        for (std::uint16_t i = 0; i < kerns && r.ok(); ++i) {
            KerningPair k;
            k.left = wide_codes ? r.u16() : r.u8();
            k.right = wide_codes ? r.u16() : r.u8();
            k.adjustment = r.s16();
            if (r.ok()) {
                f.kerning.push_back(k);
            }
        }
    }
    if (!r.ok()) {
        return std::nullopt;
    }
    return f;
}

std::string read_string(BitReader& r) {
    std::string s;
    while (r.ok()) {
        const char c = static_cast<char>(r.u8());
        if (c == '\0') {
            break;
        }
        s.push_back(c);
    }
    return s;
}

ColorTransform read_cxform(BitReader& r, bool with_alpha) {
    r.align();
    ColorTransform t;
    const bool add = r.ub(1) != 0;
    const bool mult = r.ub(1) != 0;
    const std::uint32_t bits = r.ub(4);
    if (mult) {
        t.mr = r.sb(bits) / 256.0;
        t.mg = r.sb(bits) / 256.0;
        t.mb = r.sb(bits) / 256.0;
        if (with_alpha) {
            t.ma = r.sb(bits) / 256.0;
        }
    }
    if (add) {
        t.ar = r.sb(bits);
        t.ag = r.sb(bits);
        t.ab = r.sb(bits);
        if (with_alpha) {
            t.aa = r.sb(bits);
        }
    }
    r.align();
    return t;
}

void read_filters(BitReader& r, std::vector<std::string>& names) {
    const std::uint8_t count = r.u8();
    for (std::uint8_t i = 0; i < count && r.ok(); ++i) {
        const std::uint8_t id = r.u8();
        switch (id) {
        case 0: names.emplace_back("DropShadow"); r.skip(23); break;
        case 1: names.emplace_back("Blur"); r.skip(9); break;
        case 2: names.emplace_back("Glow"); r.skip(15); break;
        case 3: names.emplace_back("Bevel"); r.skip(27); break;
        case 4:
        case 7: {
            names.emplace_back(id == 4 ? "GradientGlow" : "GradientBevel");
            const std::uint8_t n = r.u8();
            r.skip(static_cast<std::size_t>(n) * 5 + 19);
            break;
        }
        case 5: {
            names.emplace_back("Convolution");
            const std::uint8_t x = r.u8();
            const std::uint8_t y = r.u8();
            r.skip(8 + static_cast<std::size_t>(x) * y * 4 + 4 + 1);
            break;
        }
        case 6: names.emplace_back("ColorMatrix"); r.skip(80); break;
        default: r.fail(); break;
        }
    }
}

std::optional<PlaceObject> read_place(std::span<const std::byte> data, std::uint16_t code) {
    BitReader r(data);
    PlaceObject p;
    if (code == tag_place_object) {
        p.character = r.u16();
        p.depth = r.u16();
        p.matrix = read_matrix(r);
        if (r.remaining() > 0) {
            p.color = read_cxform(r, false);
        }
        if (!r.ok()) {
            return std::nullopt;
        }
        return p;
    }
    const std::uint8_t flags = r.u8();
    const std::uint8_t flags2 = code == tag_place_object3 ? r.u8() : 0;
    p.depth = r.u16();
    p.move = (flags & 0x01U) != 0;
    if ((flags2 & 0x08U) != 0 || ((flags2 & 0x10U) != 0 && (flags & 0x02U) != 0)) {
        p.class_name = read_string(r);
    }
    if ((flags & 0x02U) != 0) {
        p.character = r.u16();
    }
    if ((flags & 0x04U) != 0) {
        p.matrix = read_matrix(r);
    }
    if ((flags & 0x08U) != 0) {
        p.color = read_cxform(r, true);
    }
    if ((flags & 0x10U) != 0) {
        p.ratio = r.u16();
    }
    if ((flags & 0x20U) != 0) {
        p.name = read_string(r);
    }
    if ((flags & 0x40U) != 0) {
        p.clip_depth = r.u16();
    }
    if ((flags2 & 0x01U) != 0) {
        read_filters(r, p.filters);
    }
    if ((flags2 & 0x02U) != 0) {
        p.blend_mode = r.u8();
    }
    if ((flags2 & 0x04U) != 0) {
        p.cache_as_bitmap = r.u8() != 0;
    }
    if ((flags2 & 0x20U) != 0) {
        p.visible = r.u8() != 0;
    }
    if (!r.ok()) {
        return std::nullopt;
    }
    return p;
}

std::optional<EditText> read_edit_text(std::span<const std::byte> data) {
    BitReader r(data);
    EditText t;
    t.id = r.u16();
    t.bounds = read_rect(r);
    r.align();
    const std::uint8_t f1 = r.u8();
    const std::uint8_t f2 = r.u8();
    const bool has_text = (f1 & 0x80U) != 0;
    t.word_wrap = (f1 & 0x40U) != 0;
    t.multiline = (f1 & 0x20U) != 0;
    t.password = (f1 & 0x10U) != 0;
    t.read_only = (f1 & 0x08U) != 0;
    const bool has_color = (f1 & 0x04U) != 0;
    const bool has_max = (f1 & 0x02U) != 0;
    const bool has_font = (f1 & 0x01U) != 0;
    const bool has_font_class = (f2 & 0x80U) != 0;
    t.auto_size = (f2 & 0x40U) != 0;
    const bool has_layout = (f2 & 0x20U) != 0;
    t.no_select = (f2 & 0x10U) != 0;
    t.border = (f2 & 0x08U) != 0;
    t.was_static = (f2 & 0x04U) != 0;
    t.html = (f2 & 0x02U) != 0;
    t.use_outlines = (f2 & 0x01U) != 0;
    if (has_font) {
        t.font_id = r.u16();
    }
    if (has_font_class) {
        t.font_class = read_string(r);
    }
    if (has_font || has_font_class) {
        t.height = r.u16();
    }
    if (has_color) {
        t.color = read_rgba(r);
    }
    if (has_max) {
        t.max_length = r.u16();
    }
    if (has_layout) {
        t.align = r.u8();
        t.left_margin = r.u16();
        t.right_margin = r.u16();
        t.indent = r.u16();
        t.leading = r.s16();
    }
    t.variable = read_string(r);
    if (has_text) {
        t.initial_text = read_string(r);
    }
    if (!r.ok()) {
        return std::nullopt;
    }
    return t;
}

std::optional<Button> read_button2(std::span<const std::byte> data) {
    BitReader r(data);
    Button b;
    b.id = r.u16();
    (void)r.u8();  // TrackAsMenu
    (void)r.u16(); // ActionOffset
    while (r.ok() && r.remaining() > 0) {
        const std::uint8_t flags = r.u8();
        if (flags == 0) {
            break;
        }
        ButtonRecord rec;
        rec.states = flags & 0x0FU;
        rec.character = r.u16();
        rec.depth = r.u16();
        rec.matrix = read_matrix(r);
        rec.color = read_cxform(r, true);
        if ((flags & 0x10U) != 0) {
            std::vector<std::string> ignored;
            read_filters(r, ignored);
        }
        if ((flags & 0x20U) != 0) {
            (void)r.u8();
        }
        b.records.push_back(rec);
    }
    if (!r.ok()) {
        return std::nullopt;
    }
    return b;
}

struct MovieReader {
    SwfMovie& movie;
    std::string_view origin;
    std::span<const std::byte> jpeg_tables;

    void note(std::string text) { movie.notes.push_back(std::move(text)); }

    void count(std::uint16_t code) { ++movie.tag_counts[tag_name(code)]; }

    void bad(const RawTag& tag, const char* what) {
        note(std::string(what) + " at body offset " + std::to_string(tag.offset) + " did not decode");
    }

    /// One timeline (root or sprite); definition tags only at the root, but a
    /// sprite holding one is read the same way.
    void read_timeline(const std::vector<RawTag>& tags, Timeline& timeline, int depth) {
        std::uint32_t frame = 0;
        for (const RawTag& tag : tags) {
            count(tag.code);
            const auto data = tag.data;
            switch (tag.code) {
            case tag_show_frame:
                timeline.events.push_back(TimelineEvent{.kind = TimelineEvent::show_frame, .placement = {}});
                ++frame;
                break;
            case tag_place_object:
            case tag_place_object2:
            case tag_place_object3:
                if (auto p = read_place(data, tag.code)) {
                    timeline.events.push_back(
                        TimelineEvent{.kind = TimelineEvent::place, .placement = std::move(*p)});
                } else {
                    bad(tag, "place");
                }
                break;
            case tag_remove_object:
            case tag_remove_object2: {
                BitReader r(data);
                if (tag.code == tag_remove_object) {
                    (void)r.u16();
                }
                TimelineEvent e{.kind = TimelineEvent::remove, .placement = {}};
                e.placement.depth = r.u16();
                if (!r.ok()) {
                    bad(tag, "remove");
                    break;
                }
                timeline.events.push_back(std::move(e));
                break;
            }
            case tag_frame_label: {
                BitReader r(data);
                std::string name = read_string(r);
                if (!r.ok()) {
                    bad(tag, "frame label");
                    break;
                }
                timeline.labels.push_back(FrameLabel{.name = std::move(name), .frame = frame});
                break;
            }
            default: read_definition(tag, depth); break;
            }
        }
    }

    void read_definition(const RawTag& tag, int depth) {
        const auto data = tag.data;
        const auto bad = [&](const char* what) { this->bad(tag, what); };
        switch (tag.code) {
        case tag_define_shape:
        case tag_define_shape2:
        case tag_define_shape3:
        case tag_define_shape4: {
            if (auto s = read_define_shape(data, tag.code)) {
                const auto id = s->id;
                movie.shapes[id] = std::move(*s);
            } else {
                bad("shape");
            }
            break;
        }
        case tag_define_bits_lossless:
        case tag_define_bits_lossless2:
            if (auto b = read_lossless(data, tag.code)) {
                const auto id = b->id;
                movie.bitmaps[id] = std::move(*b);
            } else {
                bad("lossless bitmap");
            }
            break;
        case tag_jpeg_tables: jpeg_tables = data; break;
        case tag_define_bits:
        case tag_define_bits_jpeg2:
        case tag_define_bits_jpeg3:
        case tag_define_bits_jpeg4: {
            BitReader r(data);
            Bitmap b;
            b.tag = tag.code;
            b.id = r.u16();
            std::uint32_t image_size = static_cast<std::uint32_t>(data.size()) - 2;
            std::size_t start = 2;
            if (tag.code == tag_define_bits_jpeg3 || tag.code == tag_define_bits_jpeg4) {
                image_size = r.u32();
                start = 6;
                if (tag.code == tag_define_bits_jpeg4) {
                    (void)r.u16();
                    start = 8;
                }
            }
            if (!r.ok() || start + image_size > data.size()) {
                bad("JPEG bitmap");
                break;
            }
            const auto image = data.subspan(start, image_size);
            b.encoded_type = sniff_image(image);
            b.encoded = b.encoded_type == "jpeg"
                            ? clean_jpeg(tag.code == tag_define_bits ? jpeg_tables
                                                                     : std::span<const std::byte>{},
                                         image)
                            : std::vector<std::byte>(image.begin(), image.end());
            if (start + image_size < data.size()) {
                // The plane's size is the JPEG's, unknown without decoding it.
                note("DefineBitsJPEG3/4 " + std::to_string(b.id) +
                     ": alpha plane not merged (JPEG not decoded)");
            }
            movie.bitmaps[b.id] = std::move(b);
            break;
        }
        case tag_define_font2:
        case tag_define_font3:
            if (auto f = read_font23(data, tag.code)) {
                const auto id = f->id;
                movie.fonts[id] = std::move(*f);
            } else {
                bad("font");
            }
            break;
        case tag_define_font_name: {
            BitReader r(data);
            const auto id = r.u16();
            std::string name = read_string(r);
            std::string copyright = read_string(r);
            if (!r.ok()) {
                bad("font name");
                break;
            }
            if (auto it = movie.fonts.find(id); it != movie.fonts.end()) {
                it->second.full_name = name;
                it->second.copyright = copyright;
            }
            break;
        }
        case tag_define_font_align_zones: {
            BitReader r(data);
            if (auto it = movie.fonts.find(r.u16()); it != movie.fonts.end()) {
                it->second.has_align_zones = true;
            }
            break;
        }
        case tag_define_sprite: {
            BitReader r(data);
            Sprite s;
            s.id = r.u16();
            s.timeline.frame_count = r.u16();
            auto inner = split_tags(data.subspan(std::min<std::size_t>(4, data.size())),
                                    tag.offset + 4, origin);
            if (!r.ok() || !inner || depth > k_max_sprite_nesting) {
                bad("sprite");
                break;
            }
            read_timeline(*inner, s.timeline, depth + 1);
            movie.sprites[s.id] = std::move(s);
            break;
        }
        case tag_define_button2:
            if (auto b = read_button2(data)) {
                const auto id = b->id;
                movie.buttons[id] = std::move(*b);
            } else {
                bad("button");
            }
            break;
        case tag_define_edit_text:
            if (auto t = read_edit_text(data)) {
                const auto id = t->id;
                movie.edit_texts[id] = std::move(*t);
            } else {
                bad("edit text");
            }
            break;
        case tag_define_scaling_grid: {
            BitReader r(data);
            const auto id = r.u16();
            const Rect grid = read_rect(r);
            if (r.ok()) {
                movie.scaling_grids[id] = grid;
            }
            break;
        }
        case tag_export_assets: {
            BitReader r(data);
            const auto n = r.u16();
            for (std::uint16_t i = 0; i < n && r.ok(); ++i) {
                const auto id = r.u16();
                std::string name = read_string(r);
                if (r.ok()) {
                    movie.exports[name] = id;
                }
            }
            break;
        }
        case tag_import_assets:
        case tag_import_assets2: {
            BitReader r(data);
            Import im;
            im.url = read_string(r);
            if (tag.code == tag_import_assets2) {
                (void)r.u8();
                (void)r.u8();
            }
            const auto n = r.u16();
            for (std::uint16_t i = 0; i < n && r.ok(); ++i) {
                ImportedAsset a;
                a.id = r.u16();
                a.name = read_string(r);
                im.assets.push_back(std::move(a));
            }
            movie.imports.push_back(std::move(im));
            break;
        }
        case tag_define_font:
        case tag_define_text:
        case tag_define_text2:
        case tag_define_morph_shape:
        case tag_define_morph_shape2:
        case tag_define_button:
        case tag_define_sound:
        case tag_define_font4:
        case tag_define_video_stream: {
            BitReader r(data);
            const auto id = r.u16();
            if (r.ok()) {
                movie.other_characters[id] = tag.code;
            }
            break;
        }
        default: break;
        }
    }
};

} // namespace

io::ParseResult<SwfMovie> read_swf(std::span<const std::byte> file, std::string_view origin) {
    auto body = decompress_swf(file, origin);
    if (!body) {
        return std::unexpected(body.error());
    }
    SwfMovie movie;
    movie.signature = body->signature;
    movie.version = body->version;
    BitReader header(body->body);
    movie.frame_size = read_rect(header);
    header.align();
    movie.frame_rate = static_cast<double>(header.u16()) / 256.0;
    movie.root.frame_count = header.u16();
    if (!header.ok()) {
        return std::unexpected(ParseError{.origin = std::string(origin),
                                          .offset = 8,
                                          .kind = ErrorKind::truncated,
                                          .detail = "SWF header"});
    }
    const std::size_t start = header.byte_position();
    auto tags = split_tags(std::span<const std::byte>(body->body).subspan(start), 8 + start,
                           origin);
    if (!tags) {
        return std::unexpected(tags.error());
    }
    MovieReader reader{.movie = movie, .origin = origin, .jpeg_tables = {}};
    reader.read_timeline(*tags, movie.root, 0);
    return movie;
}

} // namespace bethconv::ui

namespace bethconv::ui {

std::vector<DisplayObject> display_list(const Timeline& timeline, std::uint32_t frame) {
    std::map<std::uint16_t, DisplayObject> list;
    std::uint32_t shown = 0;
    for (const TimelineEvent& e : timeline.events) {
        if (e.kind == TimelineEvent::show_frame) {
            if (shown++ == frame) {
                break;
            }
            continue;
        }
        const PlaceObject& p = e.placement;
        if (e.kind == TimelineEvent::remove) {
            list.erase(p.depth);
            continue;
        }
        auto it = list.find(p.depth);
        if (!p.move || it == list.end()) {
            if (!p.character) {
                continue; // a move of nothing
            }
            DisplayObject fresh;
            fresh.depth = p.depth;
            it = list.insert_or_assign(p.depth, fresh).first;
        }
        DisplayObject& o = it->second;
        // A move with a character replaces the character and keeps the rest.
        if (p.character) {
            o.character = *p.character;
        }
        if (p.matrix) {
            o.matrix = *p.matrix;
        }
        if (p.color) {
            o.color = *p.color;
        }
        if (p.name) {
            o.name = *p.name;
        }
        if (p.class_name) {
            o.class_name = *p.class_name;
        }
        if (p.clip_depth) {
            o.clip_depth = p.clip_depth;
        }
        if (p.ratio) {
            o.ratio = p.ratio;
        }
        if (p.blend_mode) {
            o.blend_mode = p.blend_mode;
        }
        if (!p.filters.empty()) {
            o.filters = p.filters;
        }
        if (p.visible) {
            o.visible = *p.visible;
        }
    }
    std::vector<DisplayObject> out;
    out.reserve(list.size());
    for (auto& [depth, o] : list) {
        out.push_back(std::move(o));
    }
    return out;
}

} // namespace bethconv::ui
