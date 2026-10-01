// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wrapper around rsm-bsa, the one dependency that parses untrusted bytes
// without SpanReader. It throws on failure, so every call is wrapped and turned
// into a ParseError here.
#include "bethconv/archive/archive_set.hpp"

#include "bethconv/io/deflate.hpp"

#include <bsa/bsa.hpp>

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <variant>

namespace bethconv::archive {
namespace {

/// Index entry: the source and what it needs to find the file again. Kept
/// small; an install has over 100k paths.
struct Provider {
    std::size_t source{};
    int priority{};
    bool loose{};
};

/// Display name including the parent folder, because mod-manager layouts often
/// contain several archives with the same basename ("Skyrim - Patch.bsa").
std::string display_name(const std::filesystem::path& path) {
    const auto parent = path.parent_path().filename().string();
    const auto leaf = path.filename().string();
    return parent.empty() ? leaf : parent + "/" + leaf;
}

io::ParseError make_error(const std::filesystem::path& path, io::ErrorKind kind,
                          std::string detail) {
    return io::ParseError{.origin = path.filename().string(),
                          .offset = 0,
                          .kind = kind,
                          .detail = std::move(detail)};
}

} // namespace

std::string_view to_string(SourceKind kind) noexcept {
    switch (kind) {
    case SourceKind::loose: return "loose";
    case SourceKind::tes4:  return "bsa";
    case SourceKind::fo4:   return "ba2";
    case SourceKind::tes3:  return "bsa-tes3";
    }
    return "unknown";
}

struct ArchiveSet::Impl {
    /// Kept open for the set's lifetime; reads borrow bytes from them.
    std::vector<std::variant<std::monostate, bsa::tes4::archive, bsa::fo4::archive>>
        archives;
    std::vector<std::filesystem::path> loose_roots;

    /// Per loose source: virtual path -> on-disk relative path, stored only
    /// where they differ. Virtual paths are lowercase, but on Linux
    /// `textures/!_Rudy_Misc/foo.dds` cannot be reopened as `!_rudy_misc/...`.
    std::vector<std::unordered_map<std::string, std::string>> loose_disk_names;

    /// vpath -> providers, kept sorted best-first.
    std::unordered_map<std::string, std::vector<Provider>> index;

    void insert(std::string vpath, Provider provider) {
        auto& providers = index[std::move(vpath)];
        // Priority first, then loose over archived, then later mount (hence >=).
        const auto pos = std::ranges::find_if(providers, [&](const Provider& p) {
            if (p.priority != provider.priority) {
                return p.priority < provider.priority;
            }
            if (p.loose != provider.loose) {
                return provider.loose;
            }
            return p.source < provider.source;
        });
        providers.insert(pos, provider);
    }
};

ArchiveSet::ArchiveSet() : impl_(std::make_unique<Impl>()) {}
ArchiveSet::~ArchiveSet() = default;
ArchiveSet::ArchiveSet(ArchiveSet&&) noexcept = default;
ArchiveSet& ArchiveSet::operator=(ArchiveSet&&) noexcept = default;

io::ParseResult<std::size_t> ArchiveSet::mount_archive(const std::filesystem::path& path,
                                                       int priority) {
    std::optional<bsa::file_format> format;
    try {
        format = bsa::guess_file_format(path);
    } catch (const std::exception& e) {
        return std::unexpected(
            make_error(path, io::ErrorKind::corrupt,
                       std::string("could not read archive header: ") + e.what()));
    }
    if (!format) {
        return std::unexpected(
            make_error(path, io::ErrorKind::bad_magic, "not a recognized BSA/BA2"));
    }

    const auto source_index = sources_.size();
    SourceInfo info{.name = display_name(path),
                    .path = path,
                    .kind = SourceKind::tes4,
                    .priority = priority,
                    .file_count = 0,
                    .version = 0};

    try {
        switch (*format) {
        case bsa::file_format::tes4: {
            bsa::tes4::archive archive;
            const auto version = archive.read(path);
            info.kind = SourceKind::tes4;
            info.version = static_cast<std::uint32_t>(version);
            for (const auto& [dir_key, directory] : archive) {
                for (const auto& [file_key, file] : directory) {
                    (void)file;
                    std::string vpath = normalize_vpath(
                        std::string(dir_key.name()) + "/" + std::string(file_key.name()));
                    impl_->insert(std::move(vpath),
                                  Provider{.source = source_index,
                                           .priority = priority,
                                           .loose = false});
                    ++info.file_count;
                }
            }
            impl_->archives.emplace_back(std::move(archive));
            break;
        }
        case bsa::file_format::fo4: {
            bsa::fo4::archive archive;
            const auto format_type = archive.read(path);
            info.kind = SourceKind::fo4;
            // BA2 reports a layout (general vs. DX10 textures), not a version.
            // Stored so `scan` can show it.
            info.version = static_cast<std::uint32_t>(format_type);
            for (const auto& [file_key, file] : archive) {
                (void)file;
                std::string vpath = normalize_vpath(std::string(file_key.name()));
                impl_->insert(std::move(vpath), Provider{.source = source_index,
                                                         .priority = priority,
                                                         .loose = false});
                ++info.file_count;
            }
            impl_->archives.emplace_back(std::move(archive));
            break;
        }
        case bsa::file_format::tes3:
            // Morrowind: give a clear message instead of "not an archive".
            return std::unexpected(make_error(
                path, io::ErrorKind::unsupported,
                "TES3 (Morrowind) archive -- this tool targets TES5 data"));
        }
    } catch (const std::exception& e) {
        return std::unexpected(make_error(path, io::ErrorKind::corrupt,
                                          std::string("archive read failed: ") + e.what()));
    }

    sources_.push_back(std::move(info));
    return sources_.back().file_count;
}

io::ParseResult<std::size_t> ArchiveSet::mount_loose(const std::filesystem::path& dir,
                                                     int priority) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return std::unexpected(
            make_error(dir, io::ErrorKind::bad_value, "not a directory"));
    }

    const auto source_index = sources_.size();
    SourceInfo info{.name = display_name(dir),
                    .path = dir,
                    .kind = SourceKind::loose,
                    .priority = priority,
                    .file_count = 0,
                    .version = 0};

    std::unordered_map<std::string, std::string> disk_names;
    auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(dir, options, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            // An unreadable subtree must not fail the whole mount.
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const auto relative = std::filesystem::relative(it->path(), dir, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        auto on_disk = relative.generic_string();
        auto vpath = normalize_vpath(on_disk);
        if (vpath != on_disk) {
            // First spelling wins. Case-only duplicates cannot exist on Windows,
            // where the game runs.
            disk_names.emplace(vpath, std::move(on_disk));
        }
        impl_->insert(std::move(vpath), Provider{.source = source_index,
                                                 .priority = priority,
                                                 .loose = true});
        ++info.file_count;
    }

    impl_->archives.emplace_back(std::monostate{});
    impl_->loose_roots.resize(sources_.size() + 1);
    impl_->loose_roots[source_index] = dir;
    impl_->loose_disk_names.resize(sources_.size() + 1);
    impl_->loose_disk_names[source_index] = std::move(disk_names);

    sources_.push_back(std::move(info));
    return sources_.back().file_count;
}

std::optional<Resolution> ArchiveSet::resolve(std::string_view path) const {
    const auto vpath = normalize_vpath(path);
    const auto it = impl_->index.find(vpath);
    if (it == impl_->index.end() || it->second.empty()) {
        return std::nullopt;
    }
    Resolution resolution{.vpath = vpath, .winner = it->second.front().source, .shadowed = {}};
    resolution.shadowed.reserve(it->second.size() - 1);
    for (std::size_t i = 1; i < it->second.size(); ++i) {
        resolution.shadowed.push_back(it->second[i].source);
    }
    return resolution;
}

io::ParseResult<std::vector<std::byte>> ArchiveSet::read(std::string_view path) const {
    const auto resolution = resolve(path);
    if (!resolution) {
        return std::unexpected(io::ParseError{.origin = std::string(path),
                                              .offset = 0,
                                              .kind = io::ErrorKind::bad_value,
                                              .detail = "no source provides this path"});
    }

    const auto& source = sources_[resolution->winner];
    const auto fail = [&](io::ErrorKind kind, std::string detail) {
        return std::unexpected(io::ParseError{.origin = resolution->vpath,
                                              .offset = 0,
                                              .kind = kind,
                                              .detail = std::move(detail)});
    };

    if (source.kind == SourceKind::loose) {
        // Open the file under its recorded on-disk spelling if it differs.
        const auto& names = impl_->loose_disk_names[resolution->winner];
        const auto named = names.find(resolution->vpath);
        const std::filesystem::path relative =
            named == names.end() ? std::filesystem::path(resolution->vpath)
                                 : std::filesystem::path(named->second);
        const auto full = impl_->loose_roots[resolution->winner] / relative;
        auto mapped = io::MappedFile::open(full);
        if (!mapped) {
            return std::unexpected(std::move(mapped).error());
        }
        const auto bytes = mapped->bytes();
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    }

    try {
        const auto& entry = impl_->archives[resolution->winner];
        if (const auto* tes4 = std::get_if<bsa::tes4::archive>(&entry)) {
            const auto slash = resolution->vpath.rfind('/');
            const std::string dir =
                slash == std::string::npos ? std::string{} : resolution->vpath.substr(0, slash);
            const std::string name =
                slash == std::string::npos ? resolution->vpath : resolution->vpath.substr(slash + 1);

            const auto dir_it = tes4->find(dir);
            if (dir_it == tes4->end()) {
                return fail(io::ErrorKind::corrupt, "directory vanished from the index");
            }
            const auto file_it = dir_it->second.find(name);
            if (file_it == dir_it->second.end()) {
                return fail(io::ErrorKind::corrupt, "file vanished from the index");
            }
            const auto& file = file_it->second;
            if (file.compressed()) {
                // Decompressed via io/deflate.cpp instead of rsm-bsa's
                // file::decompress, whose LZ4 loop hangs on truncated frames
                // (found by fuzz_bsa). A hang cannot be caught like an exception.
                const auto raw = file.as_bytes();
                const auto declared = file.decompressed_size();
                const auto version = static_cast<bsa::tes4::version>(source.version);
                // The codec depends on the archive version: v104 zlib, v105 LZ4
                // (docs/format-notes/bsa-archives.md).
                auto decompressed =
                    version == bsa::tes4::version::sse
                        ? io::lz4_decompress_exact(raw, declared, resolution->vpath, 0)
                        : io::inflate_exact(raw, declared, resolution->vpath, 0);
                if (!decompressed) {
                    return std::unexpected(std::move(decompressed).error());
                }
                return std::move(*decompressed);
            }
            const auto bytes = file.as_bytes();
            return std::vector<std::byte>(bytes.begin(), bytes.end());
        }
        if (const auto* fo4 = std::get_if<bsa::fo4::archive>(&entry)) {
            const auto file_it = fo4->find(resolution->vpath);
            if (file_it == fo4->end()) {
                return fail(io::ErrorKind::corrupt, "file vanished from the index");
            }
            // A BA2 file is a sequence of chunks; concatenate them.
            std::vector<std::byte> out;
            for (const auto& chunk : file_it->second) {
                if (chunk.compressed()) {
                    bsa::fo4::chunk copy = chunk;
                    copy.decompress();
                    const auto bytes = copy.as_bytes();
                    out.insert(out.end(), bytes.begin(), bytes.end());
                } else {
                    const auto bytes = chunk.as_bytes();
                    out.insert(out.end(), bytes.begin(), bytes.end());
                }
            }
            return out;
        }
        return fail(io::ErrorKind::corrupt, "source is not a mounted archive");
    } catch (const std::exception& e) {
        return fail(io::ErrorKind::corrupt, std::string("extraction failed: ") + e.what());
    }
}

std::size_t ArchiveSet::unique_paths() const noexcept { return impl_->index.size(); }

std::vector<Resolution> ArchiveSet::conflicts() const {
    std::vector<Resolution> out;
    for (const auto& [vpath, providers] : impl_->index) {
        if (providers.size() < 2) {
            continue;
        }
        Resolution resolution{.vpath = vpath, .winner = providers.front().source, .shadowed = {}};
        for (std::size_t i = 1; i < providers.size(); ++i) {
            resolution.shadowed.push_back(providers[i].source);
        }
        out.push_back(std::move(resolution));
    }
    return out;
}

void ArchiveSet::for_each(const std::function<void(const Resolution&)>& fn) const {
    for (const auto& [vpath, providers] : impl_->index) {
        if (providers.empty()) {
            continue;
        }
        Resolution resolution{.vpath = vpath, .winner = providers.front().source, .shadowed = {}};
        for (std::size_t i = 1; i < providers.size(); ++i) {
            resolution.shadowed.push_back(providers[i].source);
        }
        fn(resolution);
    }
}

} // namespace bethconv::archive
