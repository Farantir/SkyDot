// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/animation/animation_data.hpp"

#include "bethconv/io/span_reader.hpp"

#include <charconv>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace bethconv::animation {
namespace {

constexpr std::string_view k_folder = "meshes/animationdata/";
constexpr std::string_view k_bound = "meshes/animationdata/boundanims/";
constexpr std::string_view k_single = "meshes/animationdatasinglefile.txt";
/// Lines in one block of the single file; vanilla's largest has 21,000.
constexpr std::uint32_t k_max_block_lines = 1u << 24;

/// The text as lines (CRLF or LF), with a position for errors.
class Lines {
public:
    Lines(std::string_view text, std::string_view origin, std::span<const std::byte> bytes,
          std::size_t first_line = 0)
        : text_(text), reader_(bytes, origin), line_(first_line) {}

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::size_t line() const noexcept { return line_; }
    [[nodiscard]] std::string_view text() const noexcept { return text_; }

    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }

    /// The next line without its line break.
    std::string_view next() {
        const auto start = pos_;
        auto end = text_.find('\n', pos_);
        if (end == std::string_view::npos) {
            end = text_.size();
            pos_ = end;
        } else {
            pos_ = end + 1;
        }
        auto line = text_.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        ++line_;
        return line;
    }

    /// Skip blank lines; false at the end.
    bool skip_blank() {
        while (!at_end()) {
            const auto save = pos_;
            const auto line_save = line_;
            if (!next().empty()) {
                pos_ = save;
                line_ = line_save;
                return true;
            }
        }
        return false;
    }

    std::unexpected<io::ParseError> fail(std::string detail) const {
        return reader_.fail(io::ErrorKind::corrupt, "line " + std::to_string(line_) + ": " + std::move(detail));
    }

private:
    std::string_view text_;
    io::SpanReader reader_;
    std::size_t pos_{};
    std::size_t line_{};
};

/// The next `n` lines as one view, for a block that is parsed on its own.
std::optional<std::string_view> block(Lines& lines, std::uint32_t n) {
    const auto start = lines.position();
    for (std::uint32_t i = 0; i < n; ++i) {
        if (lines.at_end()) {
            return std::nullopt;
        }
        (void)lines.next();
    }
    return lines.text().substr(start, lines.position() - start);
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<std::uint32_t> to_uint(std::string_view s) {
    s = trim(s);
    std::uint32_t v{};
    const char* first = std::to_address(s.begin());
    const char* last = std::to_address(s.end());
    const auto [end, ec] = std::from_chars(first, last, v);
    if (ec != std::errc() || end != last || s.empty()) {
        return std::nullopt;
    }
    return v;
}

std::optional<float> to_float(std::string_view s) {
    s = trim(s);
    float v{};
    const char* first = std::to_address(s.begin());
    const char* last = std::to_address(s.end());
    const auto [end, ec] = std::from_chars(first, last, v);
    if (ec != std::errc() || end != last || s.empty() || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

/// Whitespace-separated floats; exactly `n` of them.
bool floats(std::string_view s, std::span<float> out) {
    std::size_t i = 0;
    while (true) {
        s = trim(s);
        if (s.empty()) {
            break;
        }
        const auto space = s.find_first_of(" \t");
        const auto word = s.substr(0, space);
        const auto v = to_float(word);
        if (!v || i >= out.size()) {
            return false;
        }
        out[i++] = *v;
        s = space == std::string_view::npos ? std::string_view() : s.substr(space);
    }
    return i == out.size();
}

/// A count line, checked against the limit.
std::optional<std::uint32_t> count(Lines& lines, std::string_view what, std::string& error,
                                   std::uint32_t limit = k_max_project_entries) {
    if (lines.at_end()) {
        error = std::string(what) + " count missing";
        return std::nullopt;
    }
    const auto line = lines.next();
    const auto n = to_uint(line);
    if (!n) {
        error = std::string(what) + " count is not a number: '" + std::string(line.substr(0, 40)) + "'";
        return std::nullopt;
    }
    if (*n > limit) {
        error = std::to_string(*n) + " " + std::string(what);
        return std::nullopt;
    }
    return n;
}

io::ParseResult<std::string_view> text_of(std::span<const std::byte> bytes, std::string_view origin) {
    io::SpanReader reader(bytes, origin);
    return reader.chars(bytes.size());
}

} // namespace

bool is_animation_data(std::string_view vpath) noexcept {
    if (vpath == k_single) {
        return true;
    }
    if (!vpath.starts_with(k_folder) || !vpath.ends_with(".txt")) {
        return false;
    }
    const auto rest = vpath.substr(k_folder.size());
    if (rest.find('/') == std::string_view::npos) {
        // dirlist.txt only lists the project files.
        return rest != "dirlist.txt";
    }
    return vpath.starts_with(k_bound) && vpath.substr(k_bound.size()).find('/') == std::string_view::npos;
}

namespace {

io::ParseResult<ProjectData> parse_project(Lines& lines) {
    ProjectData out;
    std::string error;
    if (lines.at_end() || to_uint(lines.next()) != 1u) {
        return lines.fail("a project file starts with 1");
    }
    const auto files = count(lines, "files", error);
    if (!files) {
        return lines.fail(error);
    }
    for (std::uint32_t i = 0; i < *files; ++i) {
        if (lines.at_end()) {
            return lines.fail("file list ends early");
        }
        out.files.emplace_back(lines.next());
    }
    if (lines.at_end()) {
        return lines.fail("clip flag missing");
    }
    const auto has_clips = to_uint(lines.next());
    if (!has_clips || *has_clips > 1) {
        return lines.fail("clip flag is not 0 or 1");
    }
    out.has_clips = *has_clips == 1;
    while (out.has_clips && lines.skip_blank()) {
        if (out.clips.size() >= k_max_project_entries) {
            return lines.fail("too many clips");
        }
        ProjectClip clip;
        clip.name = std::string(lines.next());
        const auto index = to_uint(lines.next());
        const auto speed = to_float(lines.next());
        const auto crop_start = to_float(lines.next());
        const auto crop_end = to_float(lines.next());
        if (!index || !speed || !crop_start || !crop_end) {
            return lines.fail("clip '" + clip.name + "': index, speed or crop is not a number");
        }
        clip.animation = *index;
        clip.speed = *speed;
        clip.crop_start = *crop_start;
        clip.crop_end = *crop_end;
        const auto notes = count(lines, "annotations", error);
        if (!notes) {
            return lines.fail("clip '" + clip.name + "': " + error);
        }
        for (std::uint32_t i = 0; i < *notes; ++i) {
            const auto line = lines.next();
            const auto colon = line.rfind(':');
            const auto time = colon == std::string_view::npos ? std::nullopt : to_float(line.substr(colon + 1));
            if (!time) {
                return lines.fail("clip '" + clip.name + "': annotation is not text:time");
            }
            clip.annotations.emplace_back(*time, std::string(line.substr(0, colon)));
        }
        out.clips.push_back(std::move(clip));
    }
    return out;
}

io::ParseResult<ProjectData> parse_bound_anims(Lines& lines) {
    ProjectData out;
    std::string error;
    while (lines.skip_blank()) {
        if (out.motions.size() >= k_max_project_entries) {
            return lines.fail("too many motions");
        }
        Motion m;
        const auto index = to_uint(lines.next());
        const auto duration = to_float(lines.next());
        if (!index || !duration) {
            return lines.fail("motion index or duration is not a number");
        }
        m.animation = *index;
        m.duration = *duration;
        const auto moves = count(lines, "translations", error);
        if (!moves) {
            return lines.fail(error);
        }
        for (std::uint32_t i = 0; i < *moves; ++i) {
            std::array<float, 4> v{};
            if (!floats(lines.next(), v)) {
                return lines.fail("translation key is not 'time x y z'");
            }
            m.translations.push_back(MotionKey{v[0], {v[1], v[2], v[3]}});
        }
        const auto turns = count(lines, "rotations", error);
        if (!turns) {
            return lines.fail(error);
        }
        for (std::uint32_t i = 0; i < *turns; ++i) {
            std::array<float, 5> v{};
            if (!floats(lines.next(), v)) {
                return lines.fail("rotation key is not 'time x y z w'");
            }
            m.rotations.push_back(RotationKey{v[0], {v[1], v[2], v[3], v[4]}});
        }
        out.motions.push_back(std::move(m));
    }
    return out;
}

} // namespace

io::ParseResult<ProjectData> read_project(std::span<const std::byte> bytes, std::string_view origin) {
    auto text = text_of(bytes, origin);
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    Lines lines(*text, origin, bytes);
    return parse_project(lines);
}

io::ParseResult<ProjectData> read_bound_anims(std::span<const std::byte> bytes, std::string_view origin) {
    auto text = text_of(bytes, origin);
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    Lines lines(*text, origin, bytes);
    return parse_bound_anims(lines);
}

io::ParseResult<ProjectData> read_single_file(std::span<const std::byte> bytes, std::string_view origin) {
    auto text = text_of(bytes, origin);
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    Lines lines(*text, origin, bytes);
    std::string error;
    const auto projects = count(lines, "projects", error);
    if (!projects) {
        return lines.fail(error);
    }
    ProjectData out;
    out.projects.resize(*projects);
    for (auto& p : out.projects) {
        if (lines.at_end()) {
            return lines.fail("project names end early");
        }
        p.name = std::string(lines.next());
        if (p.name.size() > 4 && (p.name.ends_with(".txt") || p.name.ends_with(".TXT"))) {
            p.name.resize(p.name.size() - 4);
        }
    }
    for (auto& p : out.projects) {
        const auto size = count(lines, "project lines", error, k_max_block_lines);
        if (!size) {
            return lines.fail(p.name + ": " + error);
        }
        const auto first = lines.line();
        const auto project = block(lines, *size);
        if (!project) {
            return lines.fail(p.name + ": project block ends early");
        }
        Lines inner(*project, origin, bytes, first);
        auto parsed = parse_project(inner);
        if (!parsed) {
            return std::unexpected(std::move(parsed).error());
        }
        p.files = std::move(parsed->files);
        p.clips = std::move(parsed->clips);
        p.has_clips = parsed->has_clips;
        // Projects with clips are followed by their motions.
        if (parsed->has_clips) {
            const auto motion_size = count(lines, "motion lines", error, k_max_block_lines);
            if (!motion_size) {
                return lines.fail(p.name + ": " + error);
            }
            const auto motion_first = lines.line();
            const auto motions = block(lines, *motion_size);
            if (!motions) {
                return lines.fail(p.name + ": motion block ends early");
            }
            Lines motion_lines(*motions, origin, bytes, motion_first);
            auto bound = parse_bound_anims(motion_lines);
            if (!bound) {
                return std::unexpected(std::move(bound).error());
            }
            p.motions = std::move(bound->motions);
        }
    }
    return out;
}

io::ParseResult<ProjectData> read_animation_data(std::span<const std::byte> bytes, std::string_view vpath) {
    if (vpath == k_single) {
        return read_single_file(bytes, vpath);
    }
    if (vpath.starts_with(k_bound)) {
        return read_bound_anims(bytes, vpath);
    }
    return read_project(bytes, vpath);
}

} // namespace bethconv::animation
