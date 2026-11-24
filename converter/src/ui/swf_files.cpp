// SPDX-License-Identifier: GPL-3.0-or-later
//
// The file formats a movie's assets are written in: PNG, TrueType and ZIP.
// Each is the minimum a reader (Godot's image loader, FreeType, minizip) needs,
// with nothing that varies between runs.
#include "bethconv/ui/ui_bundle.hpp"

#include "bethconv/io/deflate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>

namespace bethconv::ui {
namespace {

/// Big-endian byte buffer (PNG and TrueType are big-endian).
class Be {
public:
    std::vector<std::byte> bytes;
    void u8(std::uint32_t v) { bytes.push_back(static_cast<std::byte>(v & 0xFFU)); }
    void u16(std::uint32_t v) {
        u8(v >> 8);
        u8(v);
    }
    void s16(std::int32_t v) { u16(static_cast<std::uint32_t>(v) & 0xFFFFU); }
    void u32(std::uint32_t v) {
        u16(v >> 16);
        u16(v & 0xFFFFU);
    }
    void tag(const char* t) {
        for (int i = 0; i < 4; ++i) {
            u8(static_cast<std::uint8_t>(t[i]));
        }
    }
    void append(std::span<const std::byte> b) { bytes.insert(bytes.end(), b.begin(), b.end()); }
    void pad4() {
        while (bytes.size() % 4 != 0) {
            u8(0);
        }
    }
    [[nodiscard]] std::size_t size() const { return bytes.size(); }
    void put_u32_at(std::size_t at, std::uint32_t v) {
        bytes[at] = static_cast<std::byte>(v >> 24);
        bytes[at + 1] = static_cast<std::byte>((v >> 16) & 0xFFU);
        bytes[at + 2] = static_cast<std::byte>((v >> 8) & 0xFFU);
        bytes[at + 3] = static_cast<std::byte>(v & 0xFFU);
    }
};

std::uint32_t crc(std::span<const std::byte> data) { return io::crc32_of(data); }

} // namespace

std::optional<std::vector<std::byte>> write_png(std::uint32_t width, std::uint32_t height,
                                                std::span<const std::uint8_t> rgba) {
    std::vector<std::byte> raw;
    raw.reserve(static_cast<std::size_t>(height) * (width * 4 + 1));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(std::byte{0});
        const std::size_t pitch = static_cast<std::size_t>(width) * 4;
        for (const std::uint8_t v : rgba.subspan(y * pitch, pitch)) {
            raw.push_back(static_cast<std::byte>(v));
        }
    }
    const auto idat = io::deflate_bytes(raw, false);
    if (!idat) {
        return std::nullopt;
    }
    Be png;
    for (const std::uint32_t b : {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU}) {
        png.u8(b);
    }
    const auto chunk = [&](const char* type, const std::vector<std::byte>& data) {
        png.u32(static_cast<std::uint32_t>(data.size()));
        const std::size_t start = png.size();
        png.tag(type);
        png.append(data);
        png.u32(crc(std::span<const std::byte>(png.bytes).subspan(start)));
    };
    Be ihdr;
    ihdr.u32(width);
    ihdr.u32(height);
    ihdr.u8(8);
    ihdr.u8(6);
    ihdr.u8(0);
    ihdr.u8(0);
    ihdr.u8(0);
    chunk("IHDR", ihdr.bytes);
    chunk("IDAT", *idat);
    chunk("IEND", {});
    return std::move(png.bytes);
}

std::vector<std::byte> write_zip(std::span<const BundleFile> files) {
    // ZIP is little-endian.
    std::vector<std::byte> out;
    std::vector<std::byte> central;
    const auto le16 = [](std::vector<std::byte>& v, std::uint32_t x) {
        v.push_back(static_cast<std::byte>(x & 0xFFU));
        v.push_back(static_cast<std::byte>((x >> 8) & 0xFFU));
    };
    const auto le32 = [&](std::vector<std::byte>& v, std::uint32_t x) {
        le16(v, x & 0xFFFFU);
        le16(v, x >> 16);
    };
    constexpr std::uint32_t k_dos_date = (0U << 9) | (1U << 5) | 1U; // 1980-01-01
    for (const BundleFile& f : files) {
        const std::uint32_t sum = crc(f.bytes);
        const auto deflated = io::deflate_bytes(f.bytes, true);
        const bool stored = !deflated || deflated->size() >= f.bytes.size();
        const std::span<const std::byte> data =
            stored ? std::span<const std::byte>(f.bytes) : std::span<const std::byte>(*deflated);
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto method = stored ? 0U : 8U;
        const auto header = [&](std::vector<std::byte>& v, bool is_central) {
            le32(v, is_central ? 0x02014b50U : 0x04034b50U);
            if (is_central) {
                le16(v, 20);
            }
            le16(v, 20);
            le16(v, 0x0800); // names are UTF-8
            le16(v, method);
            le16(v, 0);
            le16(v, k_dos_date);
            le32(v, sum);
            le32(v, static_cast<std::uint32_t>(data.size()));
            le32(v, static_cast<std::uint32_t>(f.bytes.size()));
            le16(v, static_cast<std::uint32_t>(f.path.size()));
            le16(v, 0);
            if (is_central) {
                le16(v, 0);
                le16(v, 0);
                le16(v, 0);
                le32(v, 0);
                le32(v, offset);
            }
            for (const char c : f.path) {
                v.push_back(static_cast<std::byte>(c));
            }
        };
        header(out, false);
        out.insert(out.end(), data.begin(), data.end());
        header(central, true);
    }
    const auto cd_offset = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    le32(out, 0x06054b50U);
    le16(out, 0);
    le16(out, 0);
    le16(out, static_cast<std::uint32_t>(files.size()));
    le16(out, static_cast<std::uint32_t>(files.size()));
    le32(out, static_cast<std::uint32_t>(central.size()));
    le32(out, cd_offset);
    le16(out, 0);
    return out;
}

// ---- TrueType -------------------------------------------------------------

namespace {

struct Point {
    std::int32_t x, y;
    bool on;
};
using Contour = std::vector<Point>;

double signed_area(const Contour& c) {
    double a = 0;
    for (std::size_t i = 0; i < c.size(); ++i) {
        const Point& p = c[i];
        const Point& q = c[(i + 1) % c.size()];
        a += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
    }
    return a / 2;
}

bool inside(const Contour& poly, double x, double y) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const double xi = poly[i].x, yi = poly[i].y, xj = poly[j].x, yj = poly[j].y;
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            in = !in;
        }
    }
    return in;
}

/// The glyph's outlines in font units, y up, each closed contour oriented so
/// that FreeType's non-zero fill gives Flash's even-odd result: outer
/// contours clockwise, each nesting level alternating.
std::vector<Contour> glyph_contours(const Shape& glyph, double scale) {
    std::vector<Contour> contours;
    const auto pt = [&](std::int32_t x, std::int32_t y, bool on) {
        return Point{static_cast<std::int32_t>(std::lround(x * scale)),
                     static_cast<std::int32_t>(std::lround(-y * scale)), on};
    };
    Contour cur;
    std::int32_t lx = 0, ly = 0;
    const auto flush = [&] {
        if (cur.size() >= 2 && cur.front().x == cur.back().x && cur.front().y == cur.back().y &&
            cur.back().on) {
            cur.pop_back();
        }
        if (cur.size() >= 3) {
            contours.push_back(std::move(cur));
        }
        cur.clear();
    };
    for (const Edge& e : glyph.edges) {
        if (e.move_before || cur.empty() || e.x0 != lx || e.y0 != ly) {
            flush();
            cur.push_back(pt(e.x0, e.y0, true));
        }
        if (e.curved) {
            cur.push_back(pt(e.cx, e.cy, false));
        }
        const Point end = pt(e.x1, e.y1, true);
        if (!(end.x == cur.back().x && end.y == cur.back().y && cur.back().on)) {
            cur.push_back(end);
        }
        lx = e.x1;
        ly = e.y1;
    }
    flush();
    for (std::size_t i = 0; i < contours.size(); ++i) {
        // A point just inside the contour's first edge is a better probe than a
        // vertex, which may lie on another contour.
        const Point& a = contours[i][0];
        const Point& b = contours[i][1];
        const double sx = (a.x + b.x) / 2.0;
        const double sy = (a.y + b.y) / 2.0;
        int depth = 0;
        for (std::size_t j = 0; j < contours.size(); ++j) {
            if (j != i && inside(contours[j], sx, sy)) {
                ++depth;
            }
        }
        const bool want_clockwise = depth % 2 == 0;
        const bool clockwise = signed_area(contours[i]) < 0;
        if (want_clockwise != clockwise) {
            std::ranges::reverse(contours[i]);
        }
    }
    return contours;
}

struct GlyphData {
    std::vector<std::byte> bytes;
    std::int32_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;
    std::uint32_t points = 0, contours = 0;
};

GlyphData encode_glyph(const std::vector<Contour>& contours) {
    GlyphData g;
    if (contours.empty()) {
        return g;
    }
    g.x_min = g.y_min = std::numeric_limits<std::int32_t>::max();
    g.x_max = g.y_max = std::numeric_limits<std::int32_t>::min();
    for (const auto& c : contours) {
        for (const auto& p : c) {
            g.x_min = std::min(g.x_min, p.x);
            g.y_min = std::min(g.y_min, p.y);
            g.x_max = std::max(g.x_max, p.x);
            g.y_max = std::max(g.y_max, p.y);
            ++g.points;
        }
    }
    g.contours = static_cast<std::uint32_t>(contours.size());
    Be b;
    b.s16(static_cast<std::int32_t>(contours.size()));
    b.s16(g.x_min);
    b.s16(g.y_min);
    b.s16(g.x_max);
    b.s16(g.y_max);
    std::uint32_t end = 0;
    for (const auto& c : contours) {
        end += static_cast<std::uint32_t>(c.size());
        b.u16(end - 1);
    }
    b.u16(0); // no instructions
    for (const auto& c : contours) {
        for (const auto& p : c) {
            b.u8(p.on ? 1U : 0U);
        }
    }
    std::int32_t last = 0;
    for (const auto& c : contours) {
        for (const auto& p : c) {
            b.s16(p.x - last);
            last = p.x;
        }
    }
    last = 0;
    for (const auto& c : contours) {
        for (const auto& p : c) {
            b.s16(p.y - last);
            last = p.y;
        }
    }
    b.pad4();
    g.bytes = std::move(b.bytes);
    return g;
}

std::u16string utf16(std::string_view s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        std::size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            cp = ((c & 7U) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 12) | ((static_cast<unsigned char>(s[i + 2]) & 0x3FU) << 6) |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3FU);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            cp = ((c & 15U) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3FU);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            cp = ((c & 31U) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3FU);
            n = 2;
        }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FFU)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::uint32_t table_checksum(std::span<const std::byte> t) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < t.size(); i += 4) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            v = (v << 8) | (i + k < t.size() ? std::to_integer<std::uint32_t>(t[i + k]) : 0U);
        }
        sum += v;
    }
    return sum;
}

} // namespace

std::optional<std::vector<std::byte>> write_ttf(const Font& font) {
    if (font.glyphs.empty()) {
        return std::nullopt;
    }
    constexpr std::uint32_t k_upem = 2048;
    const double scale = static_cast<double>(k_upem) / font.units_per_em;
    const auto scaled = [&](double v) { return static_cast<std::int32_t>(std::lround(v * scale)); };

    // Glyph 0 is .notdef, empty; SWF glyph i is glyph i + 1.
    const std::size_t n = font.glyphs.size() + 1;
    std::vector<GlyphData> glyphs(n);
    std::vector<std::int32_t> advance(n, 0);
    std::uint32_t max_points = 0, max_contours = 0;
    std::int32_t x_min = 0, y_min = 0, x_max = 0, y_max = 0;
    bool any = false;
    for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
        glyphs[i + 1] = encode_glyph(glyph_contours(font.glyphs[i], scale));
        const GlyphData& g = glyphs[i + 1];
        max_points = std::max(max_points, g.points);
        max_contours = std::max(max_contours, g.contours);
        if (g.contours != 0) {
            if (!any) {
                x_min = g.x_min;
                y_min = g.y_min;
                x_max = g.x_max;
                y_max = g.y_max;
                any = true;
            }
            x_min = std::min(x_min, g.x_min);
            y_min = std::min(y_min, g.y_min);
            x_max = std::max(x_max, g.x_max);
            y_max = std::max(y_max, g.y_max);
        }
        advance[i + 1] = i < font.advances.size() ? scaled(font.advances[i])
                                                  : std::max(0, g.x_max) + 102;
    }
    advance[0] = static_cast<std::int32_t>(k_upem / 2);
    const std::int32_t ascent = font.has_layout ? scaled(font.ascent) : std::max(y_max, 0);
    const std::int32_t descent = font.has_layout ? scaled(font.descent) : std::max(-y_min, 0);
    const std::int32_t line_gap = font.has_layout ? std::max(0, scaled(font.leading)) : 0;
    std::int32_t advance_max = 0;
    std::int64_t advance_sum = 0;
    std::int32_t min_lsb = 0, min_rsb = 0, max_extent = 0;
    for (std::size_t i = 0; i < n; ++i) {
        advance_max = std::max(advance_max, advance[i]);
        advance_sum += advance[i];
        if (glyphs[i].contours != 0) {
            min_lsb = std::min(min_lsb, glyphs[i].x_min);
            min_rsb = std::min(min_rsb, advance[i] - glyphs[i].x_max);
            max_extent = std::max(max_extent, glyphs[i].x_max);
        }
    }

    // Character map: code -> glyph, the first glyph for a repeated code.
    std::map<std::uint16_t, std::uint16_t> cmap;
    for (std::size_t i = 0; i < font.codes.size() && i + 1 < n; ++i) {
        if (font.codes[i] != 0xFFFF) {
            cmap.emplace(font.codes[i], static_cast<std::uint16_t>(i + 1));
        }
    }

    std::map<std::string, std::vector<std::byte>> tables;

    {   // glyf + loca
        Be glyf, loca;
        for (const auto& g : glyphs) {
            loca.u32(static_cast<std::uint32_t>(glyf.size()));
            glyf.append(g.bytes);
        }
        loca.u32(static_cast<std::uint32_t>(glyf.size()));
        tables["glyf"] = std::move(glyf.bytes);
        tables["loca"] = std::move(loca.bytes);
    }
    {
        Be head;
        head.u32(0x00010000);
        head.u32(0x00010000);
        head.u32(0); // checkSumAdjustment, set below
        head.u32(0x5F0F3CF5);
        head.u16(0x0003);
        head.u16(k_upem);
        head.u32(0);
        head.u32(0);
        head.u32(0);
        head.u32(0);
        head.s16(x_min);
        head.s16(y_min);
        head.s16(x_max);
        head.s16(y_max);
        head.u16((font.bold ? 1U : 0U) | (font.italic ? 2U : 0U));
        head.u16(8);
        head.s16(2);
        head.s16(1); // long loca
        head.s16(0);
        tables["head"] = std::move(head.bytes);
    }
    {
        Be hhea;
        hhea.u32(0x00010000);
        hhea.s16(ascent);
        hhea.s16(-descent);
        hhea.s16(line_gap);
        hhea.u16(static_cast<std::uint32_t>(advance_max));
        hhea.s16(min_lsb);
        hhea.s16(min_rsb);
        hhea.s16(max_extent);
        hhea.s16(1);
        hhea.s16(0);
        hhea.s16(0);
        for (int i = 0; i < 4; ++i) {
            hhea.s16(0);
        }
        hhea.s16(0);
        hhea.u16(static_cast<std::uint32_t>(n));
        tables["hhea"] = std::move(hhea.bytes);
    }
    {
        Be hmtx;
        for (std::size_t i = 0; i < n; ++i) {
            hmtx.u16(static_cast<std::uint32_t>(std::max(0, advance[i])));
            hmtx.s16(glyphs[i].contours != 0 ? glyphs[i].x_min : 0);
        }
        tables["hmtx"] = std::move(hmtx.bytes);
    }
    {
        Be maxp;
        maxp.u32(0x00010000);
        maxp.u16(static_cast<std::uint32_t>(n));
        maxp.u16(max_points);
        maxp.u16(max_contours);
        maxp.u16(0);
        maxp.u16(0);
        maxp.u16(2);
        for (int i = 0; i < 8; ++i) {
            maxp.u16(0);
        }
        tables["maxp"] = std::move(maxp.bytes);
    }
    {
        // Format 4: runs of consecutive codes with consecutive glyphs.
        struct Seg {
            std::uint32_t start, end;
            std::int32_t delta;
        };
        std::vector<Seg> segs;
        for (const auto& [code, gid] : cmap) {
            const std::int32_t delta = static_cast<std::int32_t>(gid) - code;
            if (!segs.empty() && segs.back().end + 1 == code && segs.back().delta == delta) {
                segs.back().end = code;
            } else {
                segs.push_back({code, code, delta});
            }
        }
        segs.push_back({0xFFFF, 0xFFFF, 1});
        const auto seg_count = static_cast<std::uint32_t>(segs.size());
        std::uint32_t search = 1, selector = 0;
        while (search * 2 <= seg_count) {
            search *= 2;
            ++selector;
        }
        Be sub;
        sub.u16(4);
        sub.u16(16 + seg_count * 8);
        sub.u16(0);
        sub.u16(seg_count * 2);
        sub.u16(search * 2);
        sub.u16(selector);
        sub.u16(seg_count * 2 - search * 2);
        for (const auto& s : segs) {
            sub.u16(s.end);
        }
        sub.u16(0);
        for (const auto& s : segs) {
            sub.u16(s.start);
        }
        for (const auto& s : segs) {
            sub.u16(static_cast<std::uint32_t>(s.delta) & 0xFFFFU);
        }
        for (std::uint32_t i = 0; i < seg_count; ++i) {
            sub.u16(0);
        }
        Be cm;
        cm.u16(0);
        cm.u16(2);
        cm.u16(0);
        cm.u16(3);
        cm.u32(20);
        cm.u16(3);
        cm.u16(1);
        cm.u32(20);
        cm.append(sub.bytes);
        tables["cmap"] = std::move(cm.bytes);
    }
    {
        std::vector<std::array<std::int32_t, 3>> pairs;
        for (const KerningPair& k : font.kerning) {
            auto l = cmap.find(k.left);
            auto r = cmap.find(k.right);
            const std::int32_t v = scaled(k.adjustment);
            if (l != cmap.end() && r != cmap.end() && v != 0) {
                pairs.push_back({static_cast<std::int32_t>(l->second),
                                 static_cast<std::int32_t>(r->second), v});
            }
        }
        std::ranges::sort(pairs);
        pairs.erase(std::unique(pairs.begin(), pairs.end(),
                                [](const auto& a, const auto& b) {
                                    return a[0] == b[0] && a[1] == b[1];
                                }),
                    pairs.end());
        constexpr std::size_t k_max_pairs = (0xFFFF - 14) / 6;
        if (pairs.size() > k_max_pairs) {
            pairs.resize(k_max_pairs);
        }
        if (!pairs.empty()) {
            const auto np = static_cast<std::uint32_t>(pairs.size());
            std::uint32_t search = 1, selector = 0;
            while (search * 2 <= np) {
                search *= 2;
                ++selector;
            }
            Be kern;
            kern.u16(0);
            kern.u16(1);
            kern.u16(0);
            kern.u16(14 + np * 6);
            kern.u16(0x0001);
            kern.u16(np);
            kern.u16(search * 6);
            kern.u16(selector);
            kern.u16(np * 6 - search * 6);
            for (const auto& p : pairs) {
                kern.u16(static_cast<std::uint32_t>(p[0]));
                kern.u16(static_cast<std::uint32_t>(p[1]));
                kern.s16(p[2]);
            }
            tables["kern"] = std::move(kern.bytes);
        }
    }
    const std::string style = font.bold && font.italic ? "Bold Italic"
                              : font.bold              ? "Bold"
                              : font.italic            ? "Italic"
                                                       : "Regular";
    {
        Be os2;
        os2.u16(4);
        os2.s16(static_cast<std::int32_t>(advance_sum / static_cast<std::int64_t>(n)));
        os2.u16(font.bold ? 700 : 400);
        os2.u16(5);
        os2.u16(0);
        const std::int32_t sub = static_cast<std::int32_t>(k_upem) * 65 / 100;
        os2.s16(sub);
        os2.s16(sub);
        os2.s16(0);
        os2.s16(static_cast<std::int32_t>(k_upem) * 14 / 100);
        os2.s16(sub);
        os2.s16(sub);
        os2.s16(0);
        os2.s16(static_cast<std::int32_t>(k_upem) * 48 / 100);
        os2.s16(static_cast<std::int32_t>(k_upem) * 5 / 100);
        os2.s16(static_cast<std::int32_t>(k_upem) * 26 / 100);
        os2.s16(0);
        for (int i = 0; i < 10; ++i) {
            os2.u8(0);
        }
        os2.u32(1); // Basic Latin
        os2.u32(0);
        os2.u32(0);
        os2.u32(0);
        os2.tag("SKYD");
        std::uint32_t selection = font.italic ? 0x01U : 0U;
        selection |= font.bold ? 0x20U : 0U;
        selection |= (!font.bold && !font.italic) ? 0x40U : 0U;
        os2.u16(selection | 0x80U);
        os2.u16(cmap.empty() ? 0 : cmap.begin()->first);
        os2.u16(cmap.empty() ? 0 : cmap.rbegin()->first);
        os2.s16(ascent);
        os2.s16(-descent);
        os2.s16(line_gap);
        os2.u16(static_cast<std::uint32_t>(std::max({ascent, y_max, 0})));
        os2.u16(static_cast<std::uint32_t>(std::max({descent, -y_min, 0})));
        os2.u32(1); // Latin 1
        os2.u32(0);
        os2.s16(0);
        os2.s16(0);
        os2.u16(0);
        os2.u16(0x20);
        os2.u16(2);
        tables["OS/2"] = std::move(os2.bytes);
    }
    {
        std::string family = font.name.empty() ? "SWF font " + std::to_string(font.id) : font.name;
        std::string postscript;
        for (const char c : family + "-" + style) {
            if (c > 32 && c < 127 && c != '[' && c != ']' && c != '(' && c != ')' && c != '{' &&
                c != '}' && c != '<' && c != '>' && c != '/' && c != '%') {
                postscript.push_back(c);
            }
        }
        const std::vector<std::pair<int, std::string>> names{
            {0, font.copyright},  {1, family}, {2, style}, {3, "bethconv: " + family + " " + style},
            {4, font.full_name.empty() ? family + (style == "Regular" ? "" : " " + style)
                                    : font.full_name},
            {6, postscript}};
        Be strings;
        Be records;
        std::uint32_t count = 0;
        for (const auto& [id, text] : names) {
            if (text.empty()) {
                continue;
            }
            const auto u = utf16(text);
            records.u16(3);
            records.u16(1);
            records.u16(0x0409);
            records.u16(static_cast<std::uint16_t>(id));
            records.u16(static_cast<std::uint32_t>(u.size() * 2));
            records.u16(static_cast<std::uint32_t>(strings.size()));
            for (const char16_t c : u) {
                strings.u16(c);
            }
            ++count;
        }
        Be name;
        name.u16(0);
        name.u16(count);
        name.u16(6 + count * 12);
        name.append(records.bytes);
        name.append(strings.bytes);
        tables["name"] = std::move(name.bytes);
    }
    {
        Be post;
        post.u32(0x00030000);
        post.u32(font.italic ? static_cast<std::uint32_t>(-12 << 16) : 0U);
        post.s16(-static_cast<std::int32_t>(k_upem) / 10);
        post.s16(static_cast<std::int32_t>(k_upem) / 20);
        for (int i = 0; i < 5; ++i) {
            post.u32(0);
        }
        tables["post"] = std::move(post.bytes);
    }

    // The table directory, tags sorted (std::map's order is byte order).
    const auto count = static_cast<std::uint32_t>(tables.size());
    std::uint32_t search = 1, selector = 0;
    while (search * 2 <= count) {
        search *= 2;
        ++selector;
    }
    Be out;
    out.u32(0x00010000);
    out.u16(count);
    out.u16(search * 16);
    out.u16(selector);
    out.u16(count * 16 - search * 16);
    std::uint32_t offset = 12 + count * 16;
    std::size_t head_offset = 0;
    for (const auto& [tag, data] : tables) {
        out.tag(tag.c_str());
        out.u32(table_checksum(data));
        out.u32(offset);
        out.u32(static_cast<std::uint32_t>(data.size()));
        if (tag == "head") {
            head_offset = offset;
        }
        offset += static_cast<std::uint32_t>((data.size() + 3) & ~std::size_t{3});
    }
    for (const auto& [tag, data] : tables) {
        out.append(data);
        out.pad4();
    }
    out.put_u32_at(head_offset + 8, 0xB1B0AFBAU - table_checksum(out.bytes));
    return std::move(out.bytes);
}

} // namespace bethconv::ui
