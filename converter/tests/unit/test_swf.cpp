// SPDX-License-Identifier: GPL-3.0-or-later
//
// Menu movies (ui/swf.hpp, ui/ui_bundle.hpp). The SWFs are written here bit
// by bit from the SWF 19 specification; no game data is involved.
#include "bethconv/ui/swf.hpp"
#include "bethconv/ui/ui_bundle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;
using namespace bethconv::ui;

/// MSB-first bit writer for SWF records.
class Bits {
public:
    Bytes out;
    void ub(std::uint32_t v, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) {
            bit((v >> (n - 1 - i)) & 1U);
        }
    }
    void sb(std::int32_t v, std::uint32_t n) { ub(static_cast<std::uint32_t>(v) & mask(n), n); }
    void align() {
        if (used_ != 0) {
            used_ = 0;
        }
    }
    void u8(std::uint32_t v) {
        align();
        out.push_back(static_cast<std::byte>(v & 0xFFU));
    }
    void u16(std::uint32_t v) {
        u8(v);
        u8(v >> 8);
    }
    void u32(std::uint32_t v) {
        u16(v & 0xFFFFU);
        u16(v >> 16);
    }
    void str(std::string_view s) {
        for (char c : s) {
            u8(static_cast<unsigned char>(c));
        }
        u8(0);
    }
    void rect(std::int32_t x0, std::int32_t x1, std::int32_t y0, std::int32_t y1) {
        align();
        ub(16, 5);
        sb(x0, 16);
        sb(x1, 16);
        sb(y0, 16);
        sb(y1, 16);
        align();
    }
    /// A translate-only matrix.
    void matrix(std::int32_t tx, std::int32_t ty) {
        align();
        ub(0, 1);
        ub(0, 1);
        ub(16, 5);
        sb(tx, 16);
        sb(ty, 16);
        align();
    }

private:
    std::uint32_t used_ = 0;
    static std::uint32_t mask(std::uint32_t n) { return n >= 32 ? 0xFFFFFFFFU : (1U << n) - 1U; }
    void bit(std::uint32_t b) {
        if (used_ == 0) {
            out.push_back(std::byte{0});
        }
        if (b != 0) {
            out.back() |= static_cast<std::byte>(0x80U >> used_);
        }
        used_ = (used_ + 1) % 8;
    }
};

void tag(Bytes& stream, std::uint16_t code, const Bytes& data) {
    Bits h;
    h.u16(static_cast<std::uint32_t>(code << 6) | 0x3FU);
    h.u32(static_cast<std::uint32_t>(data.size()));
    stream.insert(stream.end(), h.out.begin(), h.out.end());
    stream.insert(stream.end(), data.begin(), data.end());
}

/// Shape records of a closed polygon (twips, absolute points), straight edges,
/// with style change (move, fill0/fill1) in front. 1-bit style indices.
void polygon(Bits& b, const std::vector<std::pair<int, int>>& pts, std::uint32_t fill0,
             std::uint32_t fill1) {
    b.ub(0, 1);
    b.ub(0x01U | (fill0 != 0 ? 0x02U : 0U) | (fill1 != 0 ? 0x04U : 0U), 5);
    b.ub(16, 5);
    b.sb(pts[0].first, 16);
    b.sb(pts[0].second, 16);
    if (fill0 != 0) {
        b.ub(fill0, 1);
    }
    if (fill1 != 0) {
        b.ub(fill1, 1);
    }
    for (std::size_t i = 1; i <= pts.size(); ++i) {
        const auto& from = pts[i - 1];
        const auto& to = pts[i % pts.size()];
        b.ub(1, 1); // edge
        b.ub(1, 1); // straight
        b.ub(14, 4); // 16 bits
        b.ub(1, 1); // general line
        b.sb(to.first - from.first, 16);
        b.sb(to.second - from.second, 16);
    }
}

/// DefineShape3 `id`: a 10x10 px square of `rgba`.
Bytes square_shape(std::uint16_t id, std::uint32_t rgba) {
    Bits b;
    b.u16(id);
    b.rect(0, 200, 0, 200);
    b.u8(1); // one fill
    b.u8(0x00);
    b.u8(rgba >> 24);
    b.u8(rgba >> 16);
    b.u8(rgba >> 8);
    b.u8(rgba);
    b.u8(0); // no lines
    b.ub(1, 4);
    b.ub(0, 4);
    polygon(b, {{0, 0}, {200, 0}, {200, 200}, {0, 200}}, 0, 1);
    b.ub(0, 6); // end
    b.align();
    return b.out;
}

Bytes place2(std::uint16_t depth, std::optional<int> character, bool move,
             std::optional<std::pair<int, int>> translate, std::string_view name = {}) {
    Bits b;
    std::uint32_t flags = move ? 1U : 0U;
    flags |= character ? 0x02U : 0U;
    flags |= translate ? 0x04U : 0U;
    flags |= name.empty() ? 0U : 0x20U;
    b.u8(flags);
    b.u16(depth);
    if (character) {
        b.u16(static_cast<std::uint16_t>(*character));
    }
    if (translate) {
        b.matrix(translate->first, translate->second);
    }
    if (!name.empty()) {
        b.str(name);
    }
    return b.out;
}

Bytes movie_body(const Bytes& tags) {
    Bits b;
    b.rect(0, 640 * 20, 0, 480 * 20);
    b.u16(30 << 8);
    b.u16(1);
    Bytes out = b.out;
    out.insert(out.end(), tags.begin(), tags.end());
    return out;
}

Bytes fws(const Bytes& tags, std::uint8_t version = 10) {
    const Bytes body = movie_body(tags);
    Bits h;
    for (char c : std::string_view("FWS")) {
        h.u8(static_cast<unsigned char>(c));
    }
    h.u8(version);
    h.u32(static_cast<std::uint32_t>(body.size() + 8));
    Bytes out = h.out;
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes cws(const Bytes& tags) {
    const Bytes body = movie_body(tags);
    uLongf size = compressBound(static_cast<uLong>(body.size()));
    Bytes packed(size);
    REQUIRE(compress(reinterpret_cast<Bytef*>(packed.data()), &size,
                     reinterpret_cast<const Bytef*>(body.data()),
                     static_cast<uLong>(body.size())) == Z_OK);
    packed.resize(size);
    Bits h;
    for (char c : std::string_view("CWS")) {
        h.u8(static_cast<unsigned char>(c));
    }
    h.u8(15);
    h.u32(static_cast<std::uint32_t>(body.size() + 8));
    Bytes out = h.out;
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
}

/// A red square shape, a sprite placing it twice over two frames, exports.
Bytes sample_tags() {
    Bytes t;
    tag(t, tag_define_shape3, square_shape(1, 0xFF0000FFU));
    Bytes sprite_tags;
    tag(sprite_tags, tag_place_object2, place2(1, 1, false, std::pair{0, 0}, "a"));
    tag(sprite_tags, tag_place_object2, place2(2, 1, false, std::pair{400, 0}));
    tag(sprite_tags, tag_show_frame, {});
    {
        Bits label;
        label.str("second");
        tag(sprite_tags, tag_frame_label, label.out);
    }
    tag(sprite_tags, tag_place_object2, place2(1, std::nullopt, true, std::pair{0, 400}));
    Bits remove;
    remove.u16(2);
    tag(sprite_tags, tag_remove_object2, remove.out);
    tag(sprite_tags, tag_show_frame, {});
    tag(sprite_tags, tag_end, {});
    Bits sprite;
    sprite.u16(2);
    sprite.u16(2);
    Bytes sprite_data = sprite.out;
    sprite_data.insert(sprite_data.end(), sprite_tags.begin(), sprite_tags.end());
    tag(t, tag_define_sprite, sprite_data);
    Bits exports;
    exports.u16(1);
    exports.u16(2);
    exports.str("Squares");
    tag(t, tag_export_assets, exports.out);
    tag(t, tag_place_object2, place2(1, 2, false, std::pair{0, 0}));
    tag(t, tag_show_frame, {});
    tag(t, tag_end, {});
    return t;
}

std::string as_string(const Bytes& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

std::uint32_t be16(const Bytes& b, std::size_t at) {
    return (std::to_integer<std::uint32_t>(b[at]) << 8) | std::to_integer<std::uint32_t>(b[at + 1]);
}
std::uint32_t be32(const Bytes& b, std::size_t at) { return (be16(b, at) << 16) | be16(b, at + 2); }

/// Offset of a TrueType table, or 0.
std::size_t ttf_table(const Bytes& f, std::string_view name) {
    const auto count = be16(f, 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = 12 + i * 16;
        if (std::memcmp(f.data() + at, name.data(), 4) == 0) {
            return be32(f, at + 8);
        }
    }
    return 0;
}

} // namespace

TEST_CASE("an uncompressed and a zlib movie read the same", "[swf]") {
    for (const Bytes& file : {fws(sample_tags()), cws(sample_tags())}) {
        auto movie = read_swf(file, "test.swf");
        REQUIRE(movie);
        CHECK(movie->frame_size.x_max == 640 * 20);
        CHECK(movie->frame_rate == 30.0);
        REQUIRE(movie->shapes.contains(1));
        const Shape& s = movie->shapes.at(1);
        CHECK(s.version == 3);
        REQUIRE(s.fills.size() == 1);
        CHECK(s.fills[0].color.r == 255);
        CHECK(s.fills[0].color.g == 0);
        REQUIRE(s.edges.size() == 4);
        CHECK(s.edges[0].fill1 == 1);
        CHECK(s.edges[0].move_before);
        CHECK(s.edges[2].x1 == 0);
        CHECK(s.edges[2].y1 == 200);
        REQUIRE(movie->sprites.contains(2));
        CHECK(movie->exports.at("Squares") == 2);
        CHECK(movie->tag_counts.at("PlaceObject2") == 4);
    }
}

TEST_CASE("an LZMA movie is refused as unsupported", "[swf]") {
    Bytes file = fws(sample_tags());
    file[0] = std::byte{'Z'};
    auto movie = read_swf(file, "test.swf");
    REQUIRE_FALSE(movie);
    CHECK(movie.error().kind == bethconv::io::ErrorKind::unsupported);
}

TEST_CASE("the display list follows places, moves and removes per frame", "[swf]") {
    auto movie = read_swf(fws(sample_tags()), "test.swf");
    REQUIRE(movie);
    const Sprite& sprite = movie->sprites.at(2);
    CHECK(sprite.timeline.frame_count == 2);
    REQUIRE(sprite.timeline.labels.size() == 1);
    CHECK(sprite.timeline.labels[0].frame == 1);

    const auto first = display_list(sprite.timeline, 0);
    REQUIRE(first.size() == 2);
    CHECK(first[0].name == "a");
    CHECK(first[1].matrix.tx == 400);

    const auto second = display_list(sprite.timeline, 1);
    REQUIRE(second.size() == 1);
    CHECK(second[0].character == 1);
    CHECK(second[0].name == "a"); // kept by the move
    CHECK(second[0].matrix.ty == 400);
}

TEST_CASE("a shape and a sprite frame draw as SVG with their bounds", "[swf]") {
    auto movie = read_swf(fws(sample_tags()), "test.swf");
    REQUIRE(movie);
    const auto shape = character_svg(*movie, 1);
    REQUIRE(shape);
    CHECK(shape->find("fill=\"#ff0000\"") != std::string::npos);
    CHECK(shape->find("d=\"M0 0L10 0L10 10L0 10Z\"") != std::string::npos);
    CHECK(shape->find("viewBox=\"0 0 10 10\"") != std::string::npos);

    const Rect both = character_bounds(*movie, 2, 0);
    CHECK(both.x_max == 600);
    CHECK(both.y_max == 200);
    const auto frame2 = character_svg(*movie, 2, 1);
    REQUIRE(frame2);
    CHECK(frame2->find("matrix(1 0 0 1 0 20)") != std::string::npos);
    CHECK_FALSE(character_svg(*movie, 99));
}

TEST_CASE("a DefineFont3 becomes a TrueType font with its outlines oriented", "[swf][ttf]") {
    // Glyph 'O': an outer square and a hole, both drawn the same way round;
    // Flash fills even-odd, so the hole must come out the other way round.
    Bits glyph;
    glyph.ub(1, 4);
    glyph.ub(0, 4);
    polygon(glyph, {{0, -20000}, {16000, -20000}, {16000, 0}, {0, 0}}, 0, 1);
    polygon(glyph, {{4000, -16000}, {12000, -16000}, {12000, -4000}, {4000, -4000}}, 0, 1);
    glyph.ub(0, 6);
    glyph.align();

    Bits f;
    f.u16(5);
    f.u8(0x84); // layout, wide codes
    f.u8(1);
    f.u8(4);
    for (char c : std::string_view("Test")) {
        f.u8(static_cast<unsigned char>(c));
    }
    f.u16(1);              // glyphs
    f.u16(4);              // offset of glyph 0 from the table start
    f.u16(4 + static_cast<std::uint32_t>(glyph.out.size())); // code table offset
    Bytes font = f.out;
    font.insert(font.end(), glyph.out.begin(), glyph.out.end());
    Bits rest;
    rest.u16('O');
    rest.u16(18000); // ascent
    rest.u16(4000);  // descent
    rest.u16(0);     // leading
    rest.u16(17000); // advance
    rest.rect(0, 16000, -20000, 0);
    rest.u16(0); // kerning
    font.insert(font.end(), rest.out.begin(), rest.out.end());

    Bytes t;
    tag(t, tag_define_font3, font);
    Bits name;
    name.u16(5);
    name.str("Test Full");
    name.str("(c) nobody");
    tag(t, tag_define_font_name, name.out);
    tag(t, tag_show_frame, {});
    tag(t, tag_end, {});
    auto movie = read_swf(fws(t), "fonts.swf");
    REQUIRE(movie);
    REQUIRE(movie->fonts.contains(5));
    const Font& parsed = movie->fonts.at(5);
    CHECK(parsed.name == "Test");
    CHECK(parsed.full_name == "Test Full");
    REQUIRE(parsed.glyphs.size() == 1);
    CHECK(parsed.codes[0] == 'O');

    const auto ttf = write_ttf(parsed);
    REQUIRE(ttf);
    const Bytes& file = *ttf;
    CHECK(be32(file, 0) == 0x00010000U);
    for (const char* name_tag : {"OS/2", "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp",
                                 "name", "post"}) {
        CHECK(ttf_table(file, name_tag) != 0);
    }
    CHECK(be16(file, ttf_table(file, "maxp") + 4) == 2); // .notdef and 'O'
    CHECK(be16(file, ttf_table(file, "head") + 18) == 2048);
    // Advance 17000 / 20480 em at 2048 units.
    CHECK(be16(file, ttf_table(file, "hmtx") + 4) == 1700);

    // Glyph 1: two contours of four points; outer clockwise (y up), hole not.
    const std::size_t loca = ttf_table(file, "loca");
    const std::size_t g = ttf_table(file, "glyf") + be32(file, loca + 4);
    REQUIRE(be16(file, g) == 2);
    const std::size_t flags_at = g + 10 + 4 + 2;
    std::vector<std::int32_t> xs, ys;
    std::int32_t x = 0, y = 0;
    const std::size_t xs_at = flags_at + 8;
    for (std::size_t i = 0; i < 8; ++i) {
        x += static_cast<std::int16_t>(be16(file, xs_at + i * 2));
        xs.push_back(x);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        y += static_cast<std::int16_t>(be16(file, xs_at + 16 + i * 2));
        ys.push_back(y);
    }
    const auto area = [&](std::size_t from) {
        double a = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t p = from + i, q = from + (i + 1) % 4;
            a += static_cast<double>(xs[p]) * ys[q] - static_cast<double>(xs[q]) * ys[p];
        }
        return a;
    };
    CHECK(area(0) < 0); // clockwise
    CHECK(area(4) > 0);
    CHECK(*std::max_element(ys.begin(), ys.end()) == 2000); // y flipped, scaled
}

TEST_CASE("a premultiplied lossless bitmap comes out straight as PNG", "[swf][png]") {
    Bytes px{std::byte{0x80}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
             std::byte{0xFF}, std::byte{0x00}, std::byte{0xFF}, std::byte{0x00}};
    uLongf size = compressBound(8);
    Bytes packed(size);
    REQUIRE(compress(reinterpret_cast<Bytef*>(packed.data()), &size,
                     reinterpret_cast<const Bytef*>(px.data()), 8) == Z_OK);
    packed.resize(size);
    Bits b;
    b.u16(7);
    b.u8(5);
    b.u16(2);
    b.u16(1);
    Bytes data = b.out;
    data.insert(data.end(), packed.begin(), packed.end());
    Bytes t;
    tag(t, tag_define_bits_lossless2, data);
    tag(t, tag_show_frame, {});
    tag(t, tag_end, {});
    auto movie = read_swf(fws(t), "bits.swf");
    REQUIRE(movie);
    const Bitmap& bm = movie->bitmaps.at(7);
    REQUIRE(bm.rgba.size() == 8);
    CHECK(bm.rgba[0] == 255); // 0x80 red at 0x80 alpha is full red
    CHECK(bm.rgba[3] == 0x80);
    CHECK(bm.rgba[5] == 255);
    CHECK(bm.rgba[7] == 255);
    const auto png = write_png(bm.width, bm.height, bm.rgba);
    CHECK(as_string(png).substr(1, 3) == "PNG");
    CHECK(as_string(png).substr(12, 4) == "IHDR");
}

TEST_CASE("a text field keeps its font, size, colour and text", "[swf]") {
    Bits b;
    b.u16(9);
    b.rect(0, 2000, 0, 400);
    b.u8(0x80 | 0x04 | 0x01); // text, colour, font
    b.u8(0x20);               // layout
    b.u16(3);
    b.u16(360);
    b.u8(0xFF);
    b.u8(0xFF);
    b.u8(0xFF);
    b.u8(0xFF);
    b.u8(2);
    b.u16(0);
    b.u16(0);
    b.u16(0);
    b.u16(40);
    b.str("Name_tf");
    b.str("$Inventory");
    Bytes t;
    tag(t, tag_define_edit_text, b.out);
    tag(t, tag_show_frame, {});
    tag(t, tag_end, {});
    auto movie = read_swf(fws(t), "text.swf");
    REQUIRE(movie);
    const EditText& e = movie->edit_texts.at(9);
    CHECK(e.font_id == 3);
    CHECK(e.height == 360);
    CHECK(e.align == 2);
    CHECK(e.leading == 40);
    CHECK(e.variable == "Name_tf");
    CHECK(e.initial_text == "$Inventory");
}

TEST_CASE("the bundle is a ZIP with the manifest first", "[swf][zip]") {
    auto zip = convert_swf(fws(sample_tags()), "interface/test.swf");
    REQUIRE(zip);
    const std::string s = as_string(*zip);
    CHECK(s.substr(0, 4) == "PK\x03\x04");
    CHECK(s.substr(30, 13) == "manifest.json");
    CHECK(s.find("svg/2_f1.svg") != std::string::npos); // the labelled frame
    // End of central directory: entries.
    const std::size_t eocd = s.rfind("PK\x05\x06");
    REQUIRE(eocd != std::string::npos);
    const auto entries = static_cast<unsigned char>(s[eocd + 10]);
    CHECK(entries == 5); // manifest, root, shape, sprite, frame 2
    // Deterministic.
    CHECK(*convert_swf(fws(sample_tags()), "interface/test.swf") == *zip);
}

TEST_CASE("a UTF-16 translation file becomes UTF-8", "[swf]") {
    Bytes in{std::byte{0xFF}, std::byte{0xFE}, std::byte{'$'}, std::byte{0},
             std::byte{0xE4}, std::byte{0},    std::byte{'\n'}, std::byte{0}};
    const auto zip = convert_ui_text(in, "interface/translate_german.txt");
    const std::string s = as_string(zip);
    CHECK(s.substr(30, 20) == "translate_german.txt");
    CHECK(s.substr(50, 4) == "$\xC3\xA4\n"); // stored: too small to deflate
}
