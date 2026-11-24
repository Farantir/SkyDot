// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/ui/ui_bundle.hpp"

#include "bethconv/io/json_text.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace bethconv::ui {
namespace {

using Json = nlohmann::ordered_json;

double round4(double v) { return std::round(v * 10000.0) / 10000.0; }
double to_px(double twips) { return round4(twips / 20.0); }

Json rect_px(const Rect& r) {
    return Json::array({to_px(r.x_min), to_px(r.y_min), to_px(r.x_max - r.x_min),
                        to_px(r.y_max - r.y_min)});
}

Json matrix_json(const Matrix& m) {
    return Json::array({round4(m.a), round4(m.b), round4(m.c), round4(m.d), to_px(m.tx),
                        to_px(m.ty)});
}

Json color_json(const ColorTransform& c) {
    return Json::array({round4(c.mr), round4(c.mg), round4(c.mb), round4(c.ma), round4(c.ar),
                        round4(c.ag), round4(c.ab), round4(c.aa)});
}

std::string rgba_hex(const Rgba& c) {
    char buf[12];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buf;
}

std::string text(std::string_view s) { return io::json_text(s); }

template <class Names> Json export_json(const Names& names) {
    Json n = Json::array();
    for (const auto& name : names) {
        n.push_back(text(name));
    }
    return n;
}

std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::ranges::transform(s, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

std::string font_file_name(const Font& f) {
    std::string base;
    for (const char c : f.name) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
            c == '-' || c == '_') {
            base.push_back(c);
        } else if (c == ' ') {
            base.push_back('_');
        }
    }
    if (base.empty()) {
        base = "font";
    }
    if (f.bold) {
        base += "-Bold";
    }
    if (f.italic) {
        base += "-Italic";
    }
    return base;
}

const char* type_of(const SwfMovie& m, std::uint16_t id) {
    if (m.shapes.contains(id)) {
        return "shape";
    }
    if (m.sprites.contains(id)) {
        return "sprite";
    }
    if (m.buttons.contains(id)) {
        return "button";
    }
    if (m.edit_texts.contains(id)) {
        return "edit_text";
    }
    if (m.bitmaps.contains(id)) {
        return "bitmap";
    }
    if (m.fonts.contains(id)) {
        return "font";
    }
    return "other";
}

/// Frames of a sprite drawn besides the first.
std::set<std::uint32_t> extra_frames(const Sprite& s, bool exported) {
    std::set<std::uint32_t> frames;
    for (const auto& l : s.timeline.labels) {
        if (l.frame != 0 && l.frame < s.timeline.frame_count) {
            frames.insert(l.frame);
        }
    }
    if (exported && s.timeline.frame_count <= k_all_frames_limit) {
        for (std::uint32_t f = 1; f < s.timeline.frame_count; ++f) {
            frames.insert(f);
        }
    }
    return frames;
}

Json display_json(const std::vector<DisplayObject>& list) {
    Json out = Json::array();
    for (const DisplayObject& o : list) {
        Json e;
        e["depth"] = o.depth;
        e["character"] = o.character;
        if (!o.name.empty()) {
            e["name"] = text(o.name);
        }
        if (!o.class_name.empty()) {
            e["class"] = text(o.class_name);
        }
        e["matrix"] = matrix_json(o.matrix);
        if (!o.color.identity()) {
            e["color"] = color_json(o.color);
        }
        if (o.clip_depth) {
            e["clip_depth"] = *o.clip_depth;
        }
        if (o.ratio) {
            e["ratio"] = *o.ratio;
        }
        if (o.blend_mode && *o.blend_mode > 1) {
            e["blend_mode"] = *o.blend_mode;
        }
        if (!o.filters.empty()) {
            e["filters"] = o.filters;
        }
        if (!o.visible) {
            e["visible"] = false;
        }
        out.push_back(std::move(e));
    }
    return out;
}

/// The events after frame 0, for a reader to step a sprite to any frame from
/// `display`: {"f": frame, "d": depth} plus "rm": 1 for a removal, else the
/// fields the PlaceObject set ("c" character, "m" matrix, "x" colour
/// transform, "n" name, "k" clip depth, "r" ratio, "mv": 1 for a move).
Json timeline_json(const Timeline& t) {
    Json out = Json::array();
    std::uint32_t frame = 0;
    for (const TimelineEvent& e : t.events) {
        if (e.kind == TimelineEvent::show_frame) {
            ++frame;
            continue;
        }
        if (frame == 0) {
            continue; // in `display`
        }
        const PlaceObject& p = e.placement;
        Json j;
        j["f"] = frame;
        j["d"] = p.depth;
        if (e.kind == TimelineEvent::remove) {
            j["rm"] = 1;
            out.push_back(std::move(j));
            continue;
        }
        if (p.move) {
            j["mv"] = 1;
        }
        if (p.character) {
            j["c"] = *p.character;
        }
        if (p.matrix) {
            j["m"] = matrix_json(*p.matrix);
        }
        if (p.color) {
            j["x"] = color_json(*p.color);
        }
        if (p.name) {
            j["n"] = text(*p.name);
        }
        if (p.clip_depth) {
            j["k"] = *p.clip_depth;
        }
        if (p.ratio) {
            j["r"] = *p.ratio;
        }
        out.push_back(std::move(j));
    }
    return out;
}

} // namespace

std::vector<BundleFile> bundle_files(const SwfMovie& m, std::string_view vpath) {
    std::vector<BundleFile> files;
    Json manifest;
    manifest["bundle_version"] = k_bundle_version;
    manifest["source"] = text(vpath);
    manifest["signature"] = m.signature;
    manifest["swf_version"] = m.version;
    manifest["stage"] = rect_px(m.frame_size);
    manifest["frame_rate"] = round4(m.frame_rate);
    manifest["frame_count"] = m.root.frame_count;

    // Names other movies or the game know characters by.
    std::map<std::uint16_t, std::vector<std::string>> export_names;
    for (const auto& [name, id] : m.exports) {
        export_names[id].push_back(name);
    }
    std::map<std::uint16_t, std::pair<std::string, std::string>> imported;
    Json imports = Json::array();
    for (const Import& im : m.imports) {
        Json j;
        j["url"] = text(im.url);
        Json assets = Json::object();
        for (const auto& a : im.assets) {
            assets[text(a.name)] = a.id;
            imported[a.id] = {im.url, a.name};
        }
        j["assets"] = std::move(assets);
        imports.push_back(std::move(j));
    }

    Json fonts = Json::array();
    std::set<std::string> font_names;
    std::vector<BundleFile> font_files;
    for (const auto& [id, f] : m.fonts) {
        Json j;
        j["id"] = id;
        j["name"] = text(f.name);
        if (!f.full_name.empty()) {
            j["full_name"] = text(f.full_name);
        }
        j["bold"] = f.bold;
        j["italic"] = f.italic;
        j["glyphs"] = f.glyphs.size();
        j["tag"] = tag_name(f.tag);
        if (f.has_layout) {
            // In ems: the engine scales by the text field's height.
            const double em = f.units_per_em;
            j["ascent_em"] = round4(f.ascent / em);
            j["descent_em"] = round4(f.descent / em);
            j["leading_em"] = round4(f.leading / em);
        }
        j["kerning_pairs"] = f.kerning.size();
        j["align_zones"] = f.has_align_zones;
        if (auto names = export_names.find(id); names != export_names.end()) {
            j["exports"] = export_json(names->second);
        }
        if (auto ttf = write_ttf(f)) {
            std::string name = font_file_name(f);
            if (font_names.contains(name)) {
                name += "_" + std::to_string(id);
            }
            font_names.insert(name);
            const std::string path = "fonts/" + name + ".ttf";
            j["file"] = path;
            font_files.push_back(BundleFile{.path = path, .bytes = std::move(*ttf)});
        }
        fonts.push_back(std::move(j));
    }
    manifest["fonts"] = std::move(fonts);

    Json bitmaps = Json::array();
    std::vector<BundleFile> image_files;
    for (const auto& [id, b] : m.bitmaps) {
        Json j;
        j["id"] = id;
        j["width"] = b.width;
        j["height"] = b.height;
        j["tag"] = tag_name(b.tag);
        std::string path;
        if (!b.rgba.empty()) {
            if (auto png = write_png(b.width, b.height, b.rgba)) {
                path = "png/" + std::to_string(id) + ".png";
                image_files.push_back(BundleFile{.path = path, .bytes = std::move(*png)});
            }
        } else if (!b.encoded.empty()) {
            const std::string ext = b.encoded_type == "jpeg" ? "jpg" : b.encoded_type;
            path = ext + "/" + std::to_string(id) + "." + ext;
            image_files.push_back(BundleFile{.path = path, .bytes = b.encoded});
        }
        if (!path.empty()) {
            j["file"] = path;
        }
        if (auto names = export_names.find(id); names != export_names.end()) {
            j["exports"] = export_json(names->second);
        }
        bitmaps.push_back(std::move(j));
    }
    manifest["bitmaps"] = std::move(bitmaps);

    std::set<std::uint16_t> ids;
    for (const auto& [id, s] : m.shapes) {
        ids.insert(id);
    }
    for (const auto& [id, s] : m.sprites) {
        ids.insert(id);
    }
    for (const auto& [id, s] : m.buttons) {
        ids.insert(id);
    }
    for (const auto& [id, s] : m.edit_texts) {
        ids.insert(id);
    }
    for (const auto& [id, s] : m.other_characters) {
        ids.insert(id);
    }

    Json characters = Json::array();
    std::vector<BundleFile> svg_files;
    for (const std::uint16_t id : ids) {
        Json j;
        j["id"] = id;
        j["type"] = type_of(m, id);
        if (auto it = m.other_characters.find(id); it != m.other_characters.end()) {
            j["tag"] = tag_name(it->second);
        }
        const auto names = export_names.find(id);
        const bool exported = names != export_names.end();
        if (exported) {
            j["exports"] = export_json(names->second);
        }
        const Rect bounds = character_bounds(m, id, 0);
        if (!bounds.empty()) {
            j["bounds"] = rect_px(bounds);
        }
        if (auto grid = m.scaling_grids.find(id); grid != m.scaling_grids.end()) {
            j["scaling_grid"] = rect_px(grid->second);
        }
        if (auto svg = character_svg(m, id, 0)) {
            const std::string path = "svg/" + std::to_string(id) + ".svg";
            j["svg"] = path;
            svg_files.push_back(BundleFile{.path = path, .bytes = bytes_of(*svg)});
        }
        if (auto s = m.sprites.find(id); s != m.sprites.end()) {
            const Timeline& t = s->second.timeline;
            j["frames"] = t.frame_count;
            if (!t.labels.empty()) {
                Json labels = Json::array();
                for (const auto& l : t.labels) {
                    labels.push_back(Json{{"name", text(l.name)}, {"frame", l.frame}});
                }
                j["labels"] = std::move(labels);
            }
            Json frame_svgs = Json::object();
            for (const std::uint32_t f : extra_frames(s->second, exported)) {
                if (auto svg = character_svg(m, id, f)) {
                    const std::string path =
                        "svg/" + std::to_string(id) + "_f" + std::to_string(f) + ".svg";
                    frame_svgs[std::to_string(f)] = path;
                    svg_files.push_back(BundleFile{.path = path, .bytes = bytes_of(*svg)});
                }
            }
            if (!frame_svgs.empty()) {
                j["frame_svgs"] = std::move(frame_svgs);
            }
            j["display"] = display_json(display_list(t, 0));
            if (t.frame_count > 1) {
                j["timeline"] = timeline_json(t);
            }
        }
        if (auto b = m.buttons.find(id); b != m.buttons.end()) {
            Json records = Json::array();
            for (const auto& r : b->second.records) {
                records.push_back(Json{{"states", r.states},
                                       {"character", r.character},
                                       {"depth", r.depth},
                                       {"matrix", matrix_json(r.matrix)}});
            }
            j["records"] = std::move(records);
        }
        if (auto t = m.edit_texts.find(id); t != m.edit_texts.end()) {
            const EditText& e = t->second;
            if (e.font_id) {
                j["font_id"] = *e.font_id;
                if (auto f = m.fonts.find(*e.font_id); f != m.fonts.end()) {
                    j["font"] = text(f->second.name);
                    j["font_bold"] = f->second.bold;
                    j["font_italic"] = f->second.italic;
                } else if (auto im = imported.find(*e.font_id); im != imported.end()) {
                    j["font"] = text(im->second.second);
                    j["font_from"] = text(im->second.first);
                }
            }
            if (!e.font_class.empty()) {
                j["font_class"] = text(e.font_class);
            }
            j["height"] = to_px(e.height);
            if (e.color) {
                j["color"] = rgba_hex(*e.color);
            }
            if (e.max_length) {
                j["max_length"] = *e.max_length;
            }
            if (e.align) {
                static constexpr const char* k_align[] = {"left", "right", "center", "justify"};
                j["align"] = *e.align < 4 ? k_align[*e.align] : "left";
                j["left_margin"] = to_px(e.left_margin);
                j["right_margin"] = to_px(e.right_margin);
                j["indent"] = to_px(e.indent);
                j["leading"] = to_px(e.leading);
            }
            if (!e.variable.empty()) {
                j["variable"] = text(e.variable);
            }
            if (!e.initial_text.empty()) {
                j["text"] = text(e.initial_text);
            }
            Json flags = Json::array();
            const std::pair<bool, const char*> named[] = {
                {e.word_wrap, "word_wrap"}, {e.multiline, "multiline"},
                {e.password, "password"},   {e.read_only, "read_only"},
                {e.auto_size, "auto_size"}, {e.no_select, "no_select"},
                {e.border, "border"},       {e.was_static, "was_static"},
                {e.html, "html"},           {e.use_outlines, "use_outlines"}};
            for (const auto& [on, name] : named) {
                if (on) {
                    flags.push_back(name);
                }
            }
            j["flags"] = std::move(flags);
        }
        characters.push_back(std::move(j));
    }
    manifest["characters"] = std::move(characters);

    Json exports = Json::object();
    for (const auto& [name, id] : m.exports) {
        exports[text(name)] = id;
    }
    manifest["exports"] = std::move(exports);
    manifest["imports"] = std::move(imports);
    manifest["root"] = Json{{"svg", "svg/root.svg"}, {"display", display_json(display_list(m.root, 0))}};
    if (!m.root.labels.empty()) {
        Json labels = Json::array();
        for (const auto& l : m.root.labels) {
            labels.push_back(Json{{"name", text(l.name)}, {"frame", l.frame}});
        }
        manifest["root"]["labels"] = std::move(labels);
    }
    Json tags = Json::object();
    for (const auto& [name, count] : m.tag_counts) {
        tags[name] = count;
    }
    manifest["tags"] = std::move(tags);
    Json notes = Json::array();
    for (const auto& n : m.notes) {
        notes.push_back(text(n));
    }
    manifest["notes"] = std::move(notes);

    files.push_back(BundleFile{.path = "manifest.json", .bytes = bytes_of(manifest.dump(1) + "\n")});
    for (auto& f : font_files) {
        files.push_back(std::move(f));
    }
    for (auto& f : image_files) {
        files.push_back(std::move(f));
    }
    files.push_back(BundleFile{.path = "svg/root.svg", .bytes = bytes_of(root_svg(m, 0))});
    for (auto& f : svg_files) {
        files.push_back(std::move(f));
    }
    return files;
}

io::ParseResult<std::vector<std::byte>> convert_swf(std::span<const std::byte> swf,
                                                    std::string_view vpath) {
    auto movie = read_swf(swf, vpath);
    if (!movie) {
        return std::unexpected(movie.error());
    }
    const auto files = bundle_files(*movie, vpath);
    return write_zip(files);
}

std::vector<std::byte> convert_ui_text(std::span<const std::byte> in, std::string_view vpath) {
    std::string name(vpath.substr(vpath.find_last_of('/') + 1));
    std::vector<std::byte> utf8;
    if (in.size() >= 2 && in[0] == std::byte{0xFF} && in[1] == std::byte{0xFE}) {
        const auto put = [&](std::uint32_t c) {
            const auto b = [&](std::uint32_t v) { utf8.push_back(static_cast<std::byte>(v)); };
            if (c < 0x80) {
                b(c);
            } else if (c < 0x800) {
                b(0xC0 | (c >> 6));
                b(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                b(0xE0 | (c >> 12));
                b(0x80 | ((c >> 6) & 0x3F));
                b(0x80 | (c & 0x3F));
            } else {
                b(0xF0 | (c >> 18));
                b(0x80 | ((c >> 12) & 0x3F));
                b(0x80 | ((c >> 6) & 0x3F));
                b(0x80 | (c & 0x3F));
            }
        };
        for (std::size_t i = 2; i + 1 < in.size(); i += 2) {
            std::uint32_t c = std::to_integer<std::uint32_t>(in[i]) |
                              (std::to_integer<std::uint32_t>(in[i + 1]) << 8);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < in.size()) {
                const std::uint32_t lo = std::to_integer<std::uint32_t>(in[i + 2]) |
                                         (std::to_integer<std::uint32_t>(in[i + 3]) << 8);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            put(c);
        }
    } else {
        utf8.assign(in.begin(), in.end());
    }
    const BundleFile file{.path = name, .bytes = std::move(utf8)};
    return write_zip(std::span<const BundleFile>(&file, 1));
}

} // namespace bethconv::ui
