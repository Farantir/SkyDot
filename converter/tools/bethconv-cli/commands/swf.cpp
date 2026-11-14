// SPDX-License-Identifier: GPL-3.0-or-later
//
// `bethconv swf`: one menu movie's tags, and its assets as the pack's UI
// bundle would hold them (ui/ui_bundle.hpp), written to a folder for checks.
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_stream.hpp"
#include "bethconv/ui/swf.hpp"
#include "bethconv/ui/ui_bundle.hpp"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace bethconv::cli {
namespace {

struct SwfArgs {
    std::vector<std::filesystem::path> sources;
    std::string input;
    std::filesystem::path out;
    bool tags = false;
    bool zip = false;
    bool allow_slow_target = false;
};

void print_tags(std::span<const std::byte> stream, std::size_t base, int indent,
                std::string_view origin) {
    auto tags = ui::split_tags(stream, base, origin);
    if (!tags) {
        std::printf("%*s<%s>\n", indent, "", tags.error().to_string().c_str());
        return;
    }
    for (const auto& t : *tags) {
        std::printf("%*s%8zu  %-24s %4u  %zu bytes\n", indent, "", t.offset,
                    ui::tag_name(t.code).c_str(), t.code, t.data.size());
        if (t.code == ui::tag_define_sprite && t.data.size() >= 4) {
            print_tags(t.data.subspan(4), t.offset + 4, indent + 4, origin);
        }
    }
}

int cmd_swf(const SwfArgs& args) {
    std::vector<std::byte> owned;
    std::span<const std::byte> bytes;
    std::optional<io::MappedFile> mapped;
    std::string vpath = args.input;
    if (!args.sources.empty()) {
        archive::ArchiveSet set;
        mount_all(set, args.sources);
        auto data = set.read(args.input);
        if (!data) {
            std::fprintf(stderr, "error: %s\n", data.error().to_string().c_str());
            return 1;
        }
        owned = std::move(*data);
        bytes = owned;
    } else {
        auto file = io::MappedFile::open(args.input);
        if (!file) {
            std::fprintf(stderr, "error: %s\n", file.error().to_string().c_str());
            return 1;
        }
        mapped = std::move(*file);
        bytes = mapped->bytes();
        vpath = std::filesystem::path(args.input).filename().string();
    }

    if (args.tags) {
        auto body = ui::decompress_swf(bytes, vpath);
        if (!body) {
            std::fprintf(stderr, "error: %s\n", body.error().to_string().c_str());
            return 1;
        }
        std::printf("%s version %u, %zu bytes uncompressed\n", body->signature.c_str(),
                    body->version, body->body.size() + 8);
        // The header's RECT is variable-length: 5 bits of size, 4 fields.
        const auto bits = std::to_integer<unsigned>(body->body[0]) >> 3;
        const std::size_t rect = (5 + 4 * bits + 7) / 8;
        print_tags(std::span<const std::byte>(body->body).subspan(rect + 4), 8 + rect + 4, 0,
                   vpath);
    }

    auto movie = ui::read_swf(bytes, vpath);
    if (!movie) {
        std::fprintf(stderr, "error: %s\n", movie.error().to_string().c_str());
        return 1;
    }
    std::printf("%s: %s v%u, stage %.0fx%.0f px, %u frames at %.1f fps\n", vpath.c_str(),
                movie->signature.c_str(), movie->version,
                (movie->frame_size.x_max - movie->frame_size.x_min) / 20.0,
                (movie->frame_size.y_max - movie->frame_size.y_min) / 20.0, movie->root.frame_count,
                movie->frame_rate);
    std::printf("  %zu shapes, %zu sprites, %zu bitmaps, %zu fonts, %zu text fields, %zu buttons, "
                "%zu other characters, %zu exports, %zu imports\n",
                movie->shapes.size(), movie->sprites.size(), movie->bitmaps.size(),
                movie->fonts.size(), movie->edit_texts.size(), movie->buttons.size(),
                movie->other_characters.size(), movie->exports.size(), movie->imports.size());
    for (const auto& [id, f] : movie->fonts) {
        std::printf("  font %u: \"%s\"%s%s, %zu glyphs, %zu kerning pairs\n", id, f.name.c_str(),
                    f.bold ? " bold" : "", f.italic ? " italic" : "", f.glyphs.size(),
                    f.kerning.size());
    }
    for (const auto& im : movie->imports) {
        std::printf("  imports %zu from %s\n", im.assets.size(), im.url.c_str());
    }
    for (const auto& n : movie->notes) {
        std::printf("  note: %s\n", n.c_str());
    }

    if (args.out.empty()) {
        return 0;
    }
    const auto files = ui::bundle_files(*movie, vpath);
    std::error_code ec;
    if (args.zip) {
        const auto zip = ui::write_zip(files);
        std::filesystem::create_directories(args.out.parent_path(), ec);
        std::string error;
        if (!io::write_file(args.out, zip, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        std::printf("wrote %s (%zu files, %zu bytes)\n", args.out.string().c_str(), files.size(),
                    zip.size());
        return 0;
    }
    std::size_t total = 0;
    for (const auto& f : files) {
        const auto path = args.out / f.path;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::string error;
        if (!io::write_file(path, f.bytes, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        total += f.bytes.size();
    }
    std::printf("wrote %zu files, %.1f MiB, to %s\n", files.size(),
                static_cast<double>(total) / (1024.0 * 1024.0), args.out.string().c_str());
    return 0;
}

} // namespace

void register_swf(CLI::App& app) {
    auto args = std::make_shared<SwfArgs>();
    auto* swf = app.add_subcommand(
        "swf", "Dump a menu movie's tags and extract its fonts, shapes, bitmaps and manifest");
    swf->add_option("input", args->input,
                    "A .swf file, or with --source a virtual path such as interface/hudmenu.swf")
        ->required();
    swf->add_option("--source", args->sources,
                    "BSA/BA2 file or loose directory to read the virtual path from; repeat, in "
                    "load order")
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    swf->add_flag("--tags", args->tags, "List every tag, sprites' tags indented");
    swf->add_option("-o,--out", args->out, "Write the bundle's files into this folder");
    swf->add_flag("--zip", args->zip, "Write the bundle as one ZIP (the pack's asset) to --out");
    swf->add_flag("--allow-slow-target", args->allow_slow_target, k_allow_slow_help);
    swf->callback([args] {
        if (!args->out.empty() && !args->zip &&
            !output_target_ok(args->out, true, args->allow_slow_target)) {
            set_exit_status(2);
            return;
        }
        set_exit_status(cmd_swf(*args));
    });
}

} // namespace bethconv::cli
