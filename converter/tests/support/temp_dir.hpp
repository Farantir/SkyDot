// SPDX-License-Identifier: GPL-3.0-or-later
//
// A scratch directory that deletes itself. The archive tests need real files:
// they mount directories and memory-map archives.
#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

namespace bethconv::test {

/// Creates a unique directory under the system temp dir and deletes it
/// recursively on destruction.
class TempDir {
public:
    TempDir() {
        // Random component so parallel ctest processes do not collide.
        static std::atomic<int> counter{0};
        static const auto salt = std::random_device{}();
        path_ = std::filesystem::temp_directory_path() /
                ("bethconv_test_" + std::to_string(salt) + "_" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path operator/(std::string_view child) const {
        return path_ / child;
    }

    /// Write `contents` to `relative`, creating parent directories. Returns the
    /// full path.
    std::filesystem::path write(std::string_view relative, std::string_view contents) const {
        const auto full = path_ / relative;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return full;
    }

private:
    std::filesystem::path path_;
};

} // namespace bethconv::test
