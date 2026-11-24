// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyrim's menus are Scaleform GFx movies: SWF files (version 10 and 15, zlib
// compressed "CWS", fonts_console.swf plain "FWS"). SkyDot never runs them; the
// converter takes their assets out (docs/adr/0010-menus-and-skyui-compatibility.md):
// fonts, vector shapes, bitmaps, the sprites' display lists, text fields and
// the export and import tables. This header is the reader; ui_bundle.hpp turns
// what it reads into files.
//
// Coordinates stay in twips (1/20 pixel), as in the file. The tag layouts are
// Adobe's SWF File Format Specification 19; GFx's own tags (1000 and up) are
// counted, not read (the vanilla .swf files have none; the console copies in
// interface/exported/*.gfx do).
#pragma once

#include "bethconv/io/parse_error.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::ui {

/// Tag codes read or counted by name.
enum TagCode : std::uint16_t {
    tag_end = 0,
    tag_show_frame = 1,
    tag_define_shape = 2,
    tag_place_object = 4,
    tag_remove_object = 5,
    tag_define_bits = 6,
    tag_define_button = 7,
    tag_jpeg_tables = 8,
    tag_define_font = 10,
    tag_define_text = 11,
    tag_define_sound = 14,
    tag_define_bits_lossless = 20,
    tag_define_bits_jpeg2 = 21,
    tag_define_shape2 = 22,
    tag_place_object2 = 26,
    tag_remove_object2 = 28,
    tag_define_shape3 = 32,
    tag_define_text2 = 33,
    tag_define_button2 = 34,
    tag_define_bits_jpeg3 = 35,
    tag_define_bits_lossless2 = 36,
    tag_define_edit_text = 37,
    tag_define_sprite = 39,
    tag_frame_label = 43,
    tag_define_morph_shape = 46,
    tag_define_font2 = 48,
    tag_export_assets = 56,
    tag_import_assets = 57,
    tag_define_video_stream = 60,
    tag_place_object3 = 70,
    tag_import_assets2 = 71,
    tag_define_font_align_zones = 73,
    tag_define_font3 = 75,
    tag_define_scaling_grid = 78,
    tag_define_shape4 = 83,
    tag_define_morph_shape2 = 84,
    tag_define_font_name = 88,
    tag_define_bits_jpeg4 = 90,
    tag_define_font4 = 91,
};

/// DefineSprites nested deeper than this are not read (a valid movie nests none).
inline constexpr int k_max_sprite_nesting = 8;

/// A short name for a tag code ("DefineShape3", "GFx_1009" ...).
[[nodiscard]] std::string tag_name(std::uint16_t code);

struct Rect {
    std::int32_t x_min{}, x_max{}, y_min{}, y_max{};
    [[nodiscard]] bool empty() const noexcept { return x_max <= x_min || y_max <= y_min; }
};

/// x' = a x + c y + tx, y' = b x + d y + ty (the spec's ScaleX, RotateSkew0,
/// RotateSkew1, ScaleY, TranslateX/Y). Translation in twips.
struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    [[nodiscard]] Matrix then(const Matrix& outer) const noexcept; ///< outer * this
    [[nodiscard]] bool identity() const noexcept {
        return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0;
    }
};

/// Colour transform: channel * mult + add, add in 0..255 units.
struct ColorTransform {
    double mr = 1, mg = 1, mb = 1, ma = 1;
    double ar = 0, ag = 0, ab = 0, aa = 0;
    [[nodiscard]] ColorTransform then(const ColorTransform& outer) const noexcept;
    [[nodiscard]] bool identity() const noexcept {
        return mr == 1 && mg == 1 && mb == 1 && ma == 1 && ar == 0 && ag == 0 && ab == 0 &&
               aa == 0;
    }
};

struct Rgba {
    std::uint8_t r{}, g{}, b{}, a{255};
};

struct GradientStop {
    std::uint8_t ratio{};
    Rgba color;
};

struct FillStyle {
    enum Type : std::uint8_t {
        solid = 0x00,
        linear = 0x10,
        radial = 0x12,
        focal = 0x13,
        bitmap_repeat = 0x40,
        bitmap_clipped = 0x41,
        bitmap_repeat_hard = 0x42,
        bitmap_clipped_hard = 0x43,
    };
    std::uint8_t type = solid;
    Rgba color;
    Matrix matrix; ///< Gradient square (+-16384 twips) or bitmap pixels to shape space.
    std::vector<GradientStop> stops;
    std::uint8_t spread{};        ///< 0 pad, 1 reflect, 2 repeat.
    std::uint8_t interpolation{}; ///< 0 RGB, 1 linear RGB.
    double focal_point{};
    std::uint16_t bitmap_id{};

    [[nodiscard]] bool is_gradient() const noexcept { return type >= 0x10 && type <= 0x13; }
    [[nodiscard]] bool is_bitmap() const noexcept { return type >= 0x40 && type <= 0x43; }
};

struct LineStyle {
    std::uint16_t width{}; ///< Twips; 0 is a hairline.
    Rgba color;
    std::optional<FillStyle> fill; ///< LINESTYLE2 with HasFillFlag.
    std::uint8_t start_cap{}, end_cap{}; ///< 0 round, 1 none, 2 square.
    std::uint8_t join{};                 ///< 0 round, 1 bevel, 2 miter.
    double miter_limit = 3;
    bool no_close = false;
};

/// One edge in twips. Style indices are into Shape::fills / Shape::lines,
/// 1-based, 0 for none, already offset for the style arrays a record added.
struct Edge {
    std::int32_t x0{}, y0{}, cx{}, cy{}, x1{}, y1{};
    bool curved = false;
    bool move_before = false; ///< A MoveTo (or a style change) started this edge's path.
    std::uint32_t fill0{}, fill1{}, line{};
    std::uint32_t layer{}; ///< Counts StateNewStyles records: later layers draw on top.
};

struct Shape {
    std::uint16_t id{};
    int version = 1; ///< 1..4 for DefineShape..DefineShape4; 0 for a glyph.
    Rect bounds;
    std::vector<FillStyle> fills;
    std::vector<LineStyle> lines;
    std::vector<Edge> edges;
    bool nonzero_winding = false; ///< DefineShape4 UsesFillWindingRule.
};

struct Bitmap {
    std::uint16_t id{};
    std::uint16_t tag{};
    std::uint32_t width{}, height{};
    /// Straight (not premultiplied) RGBA8, rows top-down. Empty for a JPEG.
    std::vector<std::uint8_t> rgba;
    /// For JPEG-coded bitmaps: a complete JPEG (tables merged), or a PNG/GIF
    /// stored in DefineBitsJPEG2/3.
    std::vector<std::byte> encoded;
    std::string encoded_type; ///< "jpeg", "png", "gif"; empty with `rgba`.
    /// DefineBitsJPEG3/4 alpha plane (width*height bytes), not merged.
    std::vector<std::uint8_t> jpeg_alpha;
};

struct KerningPair {
    std::uint16_t left{}, right{}; ///< Character codes.
    std::int16_t adjustment{};     ///< Font units (1024 * 20 per em for DefineFont3).
};

struct Font {
    std::uint16_t id{};
    std::uint16_t tag{};
    /// The name text fields and fontconfig.txt ask for ("Futura CondensedLight").
    std::string name;
    std::string full_name; ///< DefineFontName's ("Futura Condensed Light").
    std::string copyright; ///< DefineFontName.
    bool bold = false, italic = false, small_text = false;
    /// Units per em of the glyph outlines: 20480 for DefineFont3, 1024 else.
    std::uint32_t units_per_em = 1024;
    std::vector<Shape> glyphs;
    std::vector<std::uint16_t> codes; ///< One per glyph (UCS-2).
    bool has_layout = false;
    std::int32_t ascent{}, descent{}, leading{};
    std::vector<std::int16_t> advances;
    std::vector<Rect> glyph_bounds;
    std::vector<KerningPair> kerning;
    bool has_align_zones = false;
};

/// One PlaceObject/2/3 applied to a depth. Fields not set by a "move" keep
/// their earlier values when the display list is built.
struct PlaceObject {
    std::uint16_t depth{};
    bool move = false;
    std::optional<std::uint16_t> character;
    std::optional<Matrix> matrix;
    std::optional<ColorTransform> color;
    std::optional<std::uint16_t> ratio;
    std::optional<std::string> name;
    std::optional<std::uint16_t> clip_depth;
    std::optional<std::string> class_name;
    std::optional<std::uint8_t> blend_mode;
    std::vector<std::string> filters; ///< Filter names ("DropShadow", "Glow", ...).
    std::optional<bool> visible;
    bool cache_as_bitmap = false;
};

/// A timeline event: place, remove (depth) or end of frame.
struct TimelineEvent {
    enum Kind : std::uint8_t { place, remove, show_frame };
    Kind kind = show_frame;
    PlaceObject placement; ///< For `place`; `depth` also for `remove`.
};

struct FrameLabel {
    std::string name;
    std::uint32_t frame{}; ///< 0-based.
};

struct Timeline {
    std::uint32_t frame_count{};
    std::vector<TimelineEvent> events;
    std::vector<FrameLabel> labels;
};

struct Sprite {
    std::uint16_t id{};
    Timeline timeline;
};

struct ButtonRecord {
    std::uint8_t states{}; ///< 1 up, 2 over, 4 down, 8 hit test.
    std::uint16_t character{};
    std::uint16_t depth{};
    Matrix matrix;
    ColorTransform color;
};

struct Button {
    std::uint16_t id{};
    std::vector<ButtonRecord> records;
};

struct EditText {
    std::uint16_t id{};
    Rect bounds;
    std::optional<std::uint16_t> font_id;
    std::string font_class;
    std::uint16_t height{}; ///< Twips.
    std::optional<Rgba> color;
    std::optional<std::uint16_t> max_length;
    std::optional<std::uint8_t> align; ///< 0 left, 1 right, 2 center, 3 justify.
    std::uint16_t left_margin{}, right_margin{}, indent{};
    std::int16_t leading{};
    std::string variable;
    std::string initial_text;
    bool word_wrap = false, multiline = false, password = false, read_only = false,
         auto_size = false, no_select = false, border = false, was_static = false,
         html = false, use_outlines = false;
};

struct ImportedAsset {
    std::uint16_t id{};
    std::string name;
};

struct Import {
    std::string url;
    std::vector<ImportedAsset> assets;
};

/// Everything read from one movie.
struct SwfMovie {
    std::string signature; ///< "FWS", "CWS", "GFX", "CFX".
    std::uint8_t version{};
    Rect frame_size;
    double frame_rate{};
    Timeline root;

    std::map<std::uint16_t, Shape> shapes;
    std::map<std::uint16_t, Bitmap> bitmaps;
    std::map<std::uint16_t, Font> fonts;
    std::map<std::uint16_t, Sprite> sprites;
    std::map<std::uint16_t, Button> buttons;
    std::map<std::uint16_t, EditText> edit_texts;
    std::map<std::uint16_t, Rect> scaling_grids;
    /// Characters defined by tags this reader only counts (morph shapes,
    /// static text, DefineFont1 ...): id -> tag code.
    std::map<std::uint16_t, std::uint16_t> other_characters;

    std::map<std::string, std::uint16_t> exports; ///< Name -> character id.
    std::vector<Import> imports;

    std::map<std::string, std::uint32_t> tag_counts; ///< By tag_name, nested tags too.
    /// Parts not decoded or not understood, for the manifest.
    std::vector<std::string> notes;
};

/// What is on a depth of a timeline at one frame.
struct DisplayObject {
    std::uint16_t depth{};
    std::uint16_t character{};
    Matrix matrix;
    ColorTransform color;
    std::string name;
    std::string class_name;
    std::optional<std::uint16_t> clip_depth;
    std::optional<std::uint16_t> ratio;
    std::optional<std::uint8_t> blend_mode;
    std::vector<std::string> filters;
    bool visible = true;
};

/// The display list after `frame` (0-based) of `timeline` has been shown,
/// sorted by depth: places, moves and removes applied in order.
[[nodiscard]] std::vector<DisplayObject> display_list(const Timeline& timeline,
                                                      std::uint32_t frame);

/// The movie's tag stream after the header: decompressed body and where its
/// tags start. FWS/GFX as is, CWS/CFX zlib; ZWS (LZMA) is unsupported.
struct SwfBody {
    std::string signature;
    std::uint8_t version{};
    std::vector<std::byte> body; ///< Everything after the 8-byte header.
};

[[nodiscard]] io::ParseResult<SwfBody> decompress_swf(std::span<const std::byte> file,
                                                      std::string_view origin);

/// One tag as stored: code and payload.
struct RawTag {
    std::uint16_t code{};
    std::span<const std::byte> data;
    std::size_t offset{}; ///< Of the tag header within the body.
};

/// The tags of a tag stream (the movie's or a sprite's), up to End.
[[nodiscard]] io::ParseResult<std::vector<RawTag>> split_tags(std::span<const std::byte> stream,
                                                              std::size_t base_offset,
                                                              std::string_view origin);

/// Read a whole movie. A tag that does not decode is noted in `notes` and
/// skipped; only a broken container or tag stream fails.
[[nodiscard]] io::ParseResult<SwfMovie> read_swf(std::span<const std::byte> file,
                                                 std::string_view origin);

} // namespace bethconv::ui
