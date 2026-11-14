// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shapes and sprite frames as SVG. Every shape is written with its full
// transform (no transformed groups around it), so a clip layer's shapes can
// sit in a <clipPath> and the group it clips has no transform of its own.
// Colour transforms are applied to the colours (solid fills, gradient stops,
// strokes); a bitmap only takes the alpha multiplier, as opacity.
//
// Fills: per style layer, each fill style's edges (fill1 forwards, fill0
// reversed) are chained into closed contours and drawn even-odd (non-zero for
// DefineShape4 with UsesFillWindingRule), then the layer's strokes.
#include "bethconv/ui/ui_bundle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

namespace bethconv::ui {
namespace {

constexpr int k_max_nesting = 24;

std::string num(double v) {
    if (std::abs(v) < 5e-7) {
        return "0";
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

/// Twips to pixels.
std::string px(double twips) { return num(twips / 20.0); }

std::string matrix_attr(const Matrix& m) {
    if (m.identity()) {
        return {};
    }
    return " transform=\"matrix(" + num(m.a) + " " + num(m.b) + " " + num(m.c) + " " + num(m.d) +
           " " + px(m.tx) + " " + px(m.ty) + ")\"";
}

Rgba apply(const ColorTransform& t, Rgba c) {
    const auto ch = [](double v) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    };
    return Rgba{.r = ch(c.r * t.mr + t.ar),
                .g = ch(c.g * t.mg + t.ag),
                .b = ch(c.b * t.mb + t.ab),
                .a = ch(c.a * t.ma + t.aa)};
}

std::string hex(Rgba c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string base64(std::span<const std::byte> data) {
    static constexpr char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const auto v = (std::to_integer<std::uint32_t>(data[i]) << 16) |
                       (std::to_integer<std::uint32_t>(data[i + 1]) << 8) |
                       std::to_integer<std::uint32_t>(data[i + 2]);
        out += k[(v >> 18) & 63U];
        out += k[(v >> 12) & 63U];
        out += k[(v >> 6) & 63U];
        out += k[v & 63U];
    }
    if (i < data.size()) {
        std::uint32_t v = std::to_integer<std::uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) {
            v |= std::to_integer<std::uint32_t>(data[i + 1]) << 8;
        }
        out += k[(v >> 18) & 63U];
        out += k[(v >> 12) & 63U];
        out += i + 1 < data.size() ? k[(v >> 6) & 63U] : '=';
        out += '=';
    }
    return out;
}

struct DEdge {
    std::int32_t x0, y0, cx, cy, x1, y1;
    bool curved;
};

DEdge forward(const Edge& e) { return {e.x0, e.y0, e.cx, e.cy, e.x1, e.y1, e.curved}; }
DEdge reversed(const Edge& e) { return {e.x1, e.y1, e.cx, e.cy, e.x0, e.y0, e.curved}; }

std::uint64_t key(std::int32_t x, std::int32_t y) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
           static_cast<std::uint32_t>(y);
}

void append_edge(std::string& d, const DEdge& e) {
    if (e.curved) {
        d += "Q" + px(e.cx) + " " + px(e.cy) + " " + px(e.x1) + " " + px(e.y1);
    } else {
        d += "L" + px(e.x1) + " " + px(e.y1);
    }
}

/// Closed contours from directed edges, chained end to start.
std::string chain_fill(const std::vector<DEdge>& edges) {
    std::unordered_multimap<std::uint64_t, std::size_t> by_start;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        by_start.emplace(key(edges[i].x0, edges[i].y0), i);
    }
    std::vector<bool> used(edges.size(), false);
    std::string d;
    for (std::size_t first = 0; first < edges.size(); ++first) {
        if (used[first]) {
            continue;
        }
        const DEdge& s = edges[first];
        d += "M" + px(s.x0) + " " + px(s.y0);
        std::size_t cur = first;
        while (true) {
            used[cur] = true;
            const DEdge& e = edges[cur];
            const bool closes = e.x1 == s.x0 && e.y1 == s.y0;
            if (!closes || e.curved) {
                append_edge(d, e); // Z draws a closing straight edge
            }
            if (closes) {
                break;
            }
            // The next unused edge from here, preferring the one after `cur`.
            std::size_t next = edges.size();
            if (cur + 1 < edges.size() && !used[cur + 1] && edges[cur + 1].x0 == e.x1 &&
                edges[cur + 1].y0 == e.y1) {
                next = cur + 1;
            } else {
                auto [lo, hi] = by_start.equal_range(key(e.x1, e.y1));
                for (auto it = lo; it != hi; ++it) {
                    if (!used[it->second] && it->second < next) {
                        next = it->second;
                    }
                }
            }
            if (next == edges.size()) {
                break;
            }
            cur = next;
        }
        d += "Z";
    }
    return d;
}

/// Open polylines in edge order.
std::string chain_line(const std::vector<DEdge>& edges) {
    std::string d;
    bool have = false;
    std::int32_t x = 0, y = 0;
    for (const DEdge& e : edges) {
        if (!have || e.x0 != x || e.y0 != y) {
            d += "M" + px(e.x0) + " " + px(e.y0);
        }
        append_edge(d, e);
        x = e.x1;
        y = e.y1;
        have = true;
    }
    return d;
}

struct FRect {
    double x0 = std::numeric_limits<double>::infinity();
    double y0 = std::numeric_limits<double>::infinity();
    double x1 = -std::numeric_limits<double>::infinity();
    double y1 = -std::numeric_limits<double>::infinity();
    [[nodiscard]] bool empty() const { return !(x1 >= x0 && y1 >= y0); }
    void add(double x, double y) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    void add(const FRect& r) {
        if (!r.empty()) {
            add(r.x0, r.y0);
            add(r.x1, r.y1);
        }
    }
    void add(const Rect& r, const Matrix& m) {
        if (r.empty()) {
            return;
        }
        const std::array<std::array<double, 2>, 4> c{{{double(r.x_min), double(r.y_min)},
                                                      {double(r.x_max), double(r.y_min)},
                                                      {double(r.x_min), double(r.y_max)},
                                                      {double(r.x_max), double(r.y_max)}}};
        for (const auto& p : c) {
            add(m.a * p[0] + m.c * p[1] + m.tx, m.b * p[0] + m.d * p[1] + m.ty);
        }
    }
    [[nodiscard]] FRect intersect(const FRect& o) const {
        FRect r;
        r.x0 = std::max(x0, o.x0);
        r.y0 = std::max(y0, o.y0);
        r.x1 = std::min(x1, o.x1);
        r.y1 = std::min(y1, o.y1);
        return r;
    }
};

class Renderer {
public:
    explicit Renderer(const SwfMovie& movie) : m_(movie) {}

    std::string defs;
    std::string body;

    /// Draw `id` at `frame` with the full transform `mat`. In clip mode only
    /// geometry is written (paths with a transform), for a <clipPath>.
    void character(std::uint16_t id, std::uint32_t frame, const Matrix& mat,
                   const ColorTransform& cx, int depth, std::string& out, bool clip) {
        if (depth > k_max_nesting) {
            return;
        }
        if (auto s = m_.shapes.find(id); s != m_.shapes.end()) {
            shape(s->second, mat, cx, out, clip);
        } else if (auto sp = m_.sprites.find(id); sp != m_.sprites.end()) {
            timeline(sp->second.timeline, frame, mat, cx, depth + 1, out, clip);
        } else if (auto b = m_.buttons.find(id); b != m_.buttons.end()) {
            std::vector<ButtonRecord> up;
            for (const auto& r : b->second.records) {
                if ((r.states & 1U) != 0) {
                    up.push_back(r);
                }
            }
            std::ranges::stable_sort(up, {}, &ButtonRecord::depth);
            for (const auto& r : up) {
                character(r.character, 0, r.matrix.then(mat), r.color.then(cx), depth + 1, out,
                          clip);
            }
        }
    }

    void timeline(const Timeline& t, std::uint32_t frame, const Matrix& mat,
                  const ColorTransform& cx, int depth, std::string& out, bool clip) {
        const auto list = display_list(t, frame);
        for (std::size_t i = 0; i < list.size(); ++i) {
            const DisplayObject& o = list[i];
            if (!o.visible) {
                continue;
            }
            const Matrix m = o.matrix.then(mat);
            const ColorTransform c = o.color.then(cx);
            if (!o.clip_depth) {
                character(o.character, 0, m, c, depth, out, clip);
                continue;
            }
            // A mask: the depths above it up to clip_depth are drawn through it.
            std::size_t end = i + 1;
            while (end < list.size() && list[end].depth <= *o.clip_depth) {
                ++end;
            }
            if (clip) {
                // Nested masks inside a clip path: the masked content's shape.
                for (std::size_t j = i + 1; j < end; ++j) {
                    character(list[j].character, 0, list[j].matrix.then(mat),
                              list[j].color.then(cx), depth, out, true);
                }
            } else {
                const std::string id = "k" + std::to_string(next_id_++);
                std::string geometry;
                character(o.character, 0, m, c, depth, geometry, true);
                defs += "<clipPath id=\"" + id + "\">" + geometry + "</clipPath>\n";
                out += "<g clip-path=\"url(#" + id + ")\">\n";
                for (std::size_t j = i + 1; j < end; ++j) {
                    if (list[j].visible) {
                        character(list[j].character, 0, list[j].matrix.then(mat),
                                  list[j].color.then(cx), depth, out, false);
                    }
                }
                out += "</g>\n";
            }
            i = end - 1;
        }
    }

    FRect bounds(std::uint16_t id, std::uint32_t frame, const Matrix& mat, int depth) {
        FRect r;
        if (depth > k_max_nesting) {
            return r;
        }
        if (auto s = m_.shapes.find(id); s != m_.shapes.end()) {
            r.add(s->second.bounds, mat);
        } else if (auto t = m_.edit_texts.find(id); t != m_.edit_texts.end()) {
            r.add(t->second.bounds, mat);
        } else if (auto sp = m_.sprites.find(id); sp != m_.sprites.end()) {
            const auto list = display_list(sp->second.timeline, frame);
            for (std::size_t i = 0; i < list.size(); ++i) {
                const auto& o = list[i];
                const Matrix m = o.matrix.then(mat);
                if (!o.clip_depth) {
                    r.add(bounds(o.character, 0, m, depth + 1));
                    continue;
                }
                const FRect mask = bounds(o.character, 0, m, depth + 1);
                std::size_t end = i + 1;
                FRect inner;
                while (end < list.size() && list[end].depth <= *o.clip_depth) {
                    inner.add(bounds(list[end].character, 0, list[end].matrix.then(mat),
                                     depth + 1));
                    ++end;
                }
                r.add(inner.intersect(mask));
                i = end - 1;
            }
        } else if (auto b = m_.buttons.find(id); b != m_.buttons.end()) {
            for (const auto& rec : b->second.records) {
                if ((rec.states & 1U) != 0) {
                    r.add(bounds(rec.character, 0, rec.matrix.then(mat), depth + 1));
                }
            }
        }
        return r;
    }

private:
    const SwfMovie& m_;
    int next_id_ = 0;
    std::map<std::uint16_t, std::string> image_uri_;

    const std::string& image_uri(const Bitmap& b) {
        auto it = image_uri_.find(b.id);
        if (it != image_uri_.end()) {
            return it->second;
        }
        std::string uri;
        if (!b.rgba.empty()) {
            uri = "data:image/png;base64," + base64(write_png(b.width, b.height, b.rgba));
        } else if (!b.encoded.empty()) {
            uri = "data:image/" + b.encoded_type + ";base64," + base64(b.encoded);
        }
        return image_uri_.emplace(b.id, std::move(uri)).first->second;
    }

    std::string gradient(const FillStyle& f, const ColorTransform& cx) {
        const std::string id = "g" + std::to_string(next_id_++);
        const char* spread = f.spread == 1 ? "reflect" : f.spread == 2 ? "repeat" : "pad";
        std::string g;
        if (f.type == FillStyle::linear) {
            g = "<linearGradient id=\"" + id +
                "\" gradientUnits=\"userSpaceOnUse\" x1=\"-819.2\" y1=\"0\" x2=\"819.2\" y2=\"0\"";
        } else {
            g = "<radialGradient id=\"" + id +
                "\" gradientUnits=\"userSpaceOnUse\" cx=\"0\" cy=\"0\" r=\"819.2\"";
            if (f.type == FillStyle::focal) {
                g += " fx=\"" + num(f.focal_point * 819.2) + "\" fy=\"0\"";
            }
        }
        g += " spreadMethod=\"" + std::string(spread) + "\"";
        if (f.interpolation == 1) {
            g += " color-interpolation=\"linearRGB\"";
        }
        if (std::string t = matrix_attr(f.matrix); !t.empty()) {
            g += t.replace(1, 9, "gradientTransform");
        }
        g += ">";
        for (const auto& s : f.stops) {
            const Rgba c = apply(cx, s.color);
            g += "<stop offset=\"" + num(s.ratio / 255.0) + "\" stop-color=\"" + hex(c) + "\"";
            if (c.a != 255) {
                g += " stop-opacity=\"" + num(c.a / 255.0) + "\"";
            }
            g += "/>";
        }
        g += f.type == FillStyle::linear ? "</linearGradient>\n" : "</radialGradient>\n";
        defs += g;
        return "url(#" + id + ")";
    }

    /// The fill attributes for a style ("fill=... fill-opacity=...").
    std::string paint(const FillStyle& f, const ColorTransform& cx, const char* prefix) {
        const std::string p(prefix);
        if (f.is_gradient()) {
            return " " + p + "=\"" + gradient(f, cx) + "\"";
        }
        const Rgba c = apply(cx, f.color);
        std::string s = " " + p + "=\"" + hex(c) + "\"";
        if (c.a != 255) {
            s += " " + p + "-opacity=\"" + num(c.a / 255.0) + "\"";
        }
        return s;
    }

    void shape(const Shape& s, const Matrix& mat, const ColorTransform& cx, std::string& out,
               bool clip) {
        const std::string transform = matrix_attr(mat);
        const std::string rule = s.nonzero_winding ? "nonzero" : "evenodd";
        std::uint32_t layers = 0;
        for (const Edge& e : s.edges) {
            layers = std::max(layers, e.layer + 1);
        }
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            std::set<std::uint32_t> fills;
            std::set<std::uint32_t> lines;
            for (const Edge& e : s.edges) {
                if (e.layer != layer) {
                    continue;
                }
                if (e.fill0 != 0) {
                    fills.insert(e.fill0);
                }
                if (e.fill1 != 0) {
                    fills.insert(e.fill1);
                }
                if (e.line != 0) {
                    lines.insert(e.line);
                }
            }
            for (const std::uint32_t f : fills) {
                if (f > s.fills.size()) {
                    continue;
                }
                std::vector<DEdge> edges;
                for (const Edge& e : s.edges) {
                    if (e.layer != layer) {
                        continue;
                    }
                    if (e.fill1 == f) {
                        edges.push_back(forward(e));
                    }
                    if (e.fill0 == f) {
                        edges.push_back(reversed(e));
                    }
                }
                const std::string d = chain_fill(edges);
                if (d.empty()) {
                    continue;
                }
                const FillStyle& style = s.fills[f - 1];
                if (clip) {
                    out += "<path" + transform + " clip-rule=\"" + rule + "\" d=\"" + d + "\"/>\n";
                    continue;
                }
                if (style.is_bitmap()) {
                    bitmap_fill(style, d, rule, transform, cx, out);
                    continue;
                }
                out += "<path" + transform + paint(style, cx, "fill") + " fill-rule=\"" + rule +
                       "\" d=\"" + d + "\"/>\n";
            }
            if (clip) {
                continue;
            }
            for (const std::uint32_t l : lines) {
                if (l > s.lines.size()) {
                    continue;
                }
                std::vector<DEdge> edges;
                for (const Edge& e : s.edges) {
                    if (e.layer == layer && e.line == l) {
                        edges.push_back(forward(e));
                    }
                }
                const LineStyle& ls = s.lines[l - 1];
                std::string stroke;
                if (ls.fill && ls.fill->is_gradient()) {
                    stroke = paint(*ls.fill, cx, "stroke");
                } else {
                    FillStyle solid;
                    solid.color = ls.color;
                    stroke = paint(solid, cx, "stroke");
                }
                std::string attrs = " fill=\"none\"" + stroke;
                if (ls.width == 0) {
                    attrs += " stroke-width=\"1\" vector-effect=\"non-scaling-stroke\"";
                } else {
                    attrs += " stroke-width=\"" + px(ls.width) + "\"";
                }
                const auto cap = ls.start_cap == 1 ? "butt" : ls.start_cap == 2 ? "square" : "round";
                const auto join = ls.join == 1 ? "bevel" : ls.join == 2 ? "miter" : "round";
                attrs += " stroke-linecap=\"" + std::string(cap) + "\" stroke-linejoin=\"" +
                         std::string(join) + "\"";
                if (ls.join == 2) {
                    attrs += " stroke-miterlimit=\"" + num(std::max(1.0, ls.miter_limit)) + "\"";
                }
                out += "<path" + transform + attrs + " d=\"" + chain_line(edges) + "\"/>\n";
            }
        }
    }

    void bitmap_fill(const FillStyle& style, const std::string& d, const std::string& rule,
                     const std::string& transform, const ColorTransform& cx, std::string& out) {
        auto b = m_.bitmaps.find(style.bitmap_id);
        if (b == m_.bitmaps.end() || b->second.width == 0) {
            // A missing bitmap (65535 is Flash's "none"): nothing is drawn.
            return;
        }
        const std::string& uri = image_uri(b->second);
        if (uri.empty()) {
            return;
        }
        const std::string id = "k" + std::to_string(next_id_++);
        defs += "<clipPath id=\"" + id + "\"><path clip-rule=\"" + rule + "\" d=\"" + d +
                "\"/></clipPath>\n";
        const bool hard = style.type == FillStyle::bitmap_clipped_hard ||
                          style.type == FillStyle::bitmap_repeat_hard;
        out += "<g" + transform + "><g clip-path=\"url(#" + id + ")\"><image" +
               matrix_attr(scale_twips(style.matrix)) + " width=\"" + std::to_string(b->second.width) +
               "\" height=\"" + std::to_string(b->second.height) + "\"" +
               (hard ? " image-rendering=\"pixelated\"" : "") +
               (cx.ma < 1.0 ? " opacity=\"" + num(std::max(0.0, cx.ma)) + "\"" : "") +
               " preserveAspectRatio=\"none\" xlink:href=\"" + uri + "\"/></g></g>\n";
    }

    /// A bitmap matrix maps bitmap pixels to twips: as an SVG transform from
    /// image pixels to user pixels the linear part is divided by 20.
    static Matrix scale_twips(Matrix m) {
        m.a /= 20.0;
        m.b /= 20.0;
        m.c /= 20.0;
        m.d /= 20.0;
        return m;
    }
};

std::string document(const FRect& box, const std::string& defs, const std::string& body) {
    FRect b = box;
    if (b.empty()) {
        b.x0 = b.y0 = 0;
        b.x1 = b.y1 = 20;
    }
    const double w = std::max(b.x1 - b.x0, 20.0);
    const double h = std::max(b.y1 - b.y0, 20.0);
    std::string s = "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                    "xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" +
                    px(w) + "\" height=\"" + px(h) + "\" viewBox=\"" + px(b.x0) + " " + px(b.y0) +
                    " " + px(w) + " " + px(h) + "\">\n";
    if (!defs.empty()) {
        s += "<defs>\n" + defs + "</defs>\n";
    }
    s += body;
    s += "</svg>\n";
    return s;
}

} // namespace

Rect character_bounds(const SwfMovie& movie, std::uint16_t id, std::uint32_t frame) {
    Renderer r(movie);
    const FRect b = r.bounds(id, frame, Matrix{}, 0);
    if (b.empty()) {
        return {};
    }
    return Rect{.x_min = static_cast<std::int32_t>(std::floor(b.x0)),
                .x_max = static_cast<std::int32_t>(std::ceil(b.x1)),
                .y_min = static_cast<std::int32_t>(std::floor(b.y0)),
                .y_max = static_cast<std::int32_t>(std::ceil(b.y1))};
}

std::optional<std::string> character_svg(const SwfMovie& movie, std::uint16_t id,
                                         std::uint32_t frame) {
    if (!movie.shapes.contains(id) && !movie.sprites.contains(id) && !movie.buttons.contains(id)) {
        return std::nullopt;
    }
    Renderer r(movie);
    std::string body;
    r.character(id, frame, Matrix{}, ColorTransform{}, 0, body, false);
    return document(r.bounds(id, frame, Matrix{}, 0), r.defs, body);
}

std::string root_svg(const SwfMovie& movie, std::uint32_t frame) {
    Renderer r(movie);
    std::string body;
    r.timeline(movie.root, frame, Matrix{}, ColorTransform{}, 0, body, false);
    FRect stage;
    stage.add(movie.frame_size, Matrix{});
    return document(stage, r.defs, body);
}

} // namespace bethconv::ui
