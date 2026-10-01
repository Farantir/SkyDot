// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared scaffolding for the fuzz targets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

/// Invariant check for the fuzz targets. Not `assert`: `linux-fuzz` defines
/// NDEBUG, which would compile the checks away. Aborts, which libFuzzer records
/// as a finding.
#define BETHCONV_FUZZ_CHECK(cond)                                                        \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "fuzz check failed: %s\n  at %s:%d\n", #cond, __FILE__, \
                         __LINE__);                                                      \
            std::abort();                                                                \
        }                                                                                \
    } while (false)

namespace bethconv::fuzz {

/// Input size cap, so libFuzzer does not waste time on huge inputs.
inline constexpr std::size_t k_max_input = 4u * 1024u * 1024u;

/// A temp file that lives as long as this object.
///
/// `Plugin::open` memory-maps and rsm-bsa opens archives itself, so those
/// targets need a path, not a span. One file per process, rewritten each run.
class ScratchFile {
public:
    explicit ScratchFile(std::string_view suffix) {
        path_ = std::filesystem::temp_directory_path() /
                ("bethconv_fuzz_" + std::to_string(
#ifdef _WIN32
                                        0
#else
                                        static_cast<unsigned long>(::getpid())
#endif
                                        ) +
                 std::string(suffix));
    }

    ~ScratchFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;

    /// Replace the contents. False on write failure (an environment problem,
    /// not a finding).
    [[nodiscard]] bool write(std::span<const std::uint8_t> data) const {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        return out.good();
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// The input as the byte span every span-taking parser wants.
[[nodiscard]] inline std::span<const std::byte> as_bytes(const std::uint8_t* data,
                                                         std::size_t size) noexcept {
    return {reinterpret_cast<const std::byte*>(data), size};
}

} // namespace bethconv::fuzz
