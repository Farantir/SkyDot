// SPDX-License-Identifier: GPL-3.0-or-later
//
// MappedFile (assets/mapped_file.hpp): the asset blob and world.fb are read
// through it, so what it refuses and what it returns for an empty file decide
// what those readers see.
#include "assets/mapped_file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using skydot::MappedFile;

namespace {

/// A file in the temp directory that is removed with the test.
class TempFile {
public:
    explicit TempFile(const std::vector<std::uint8_t>& content)
        : path_(std::filesystem::temp_directory_path() / "skydot_mapped_file_test.bin") {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    }
    ~TempFile() { std::filesystem::remove(path_); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("a mapped file shows the bytes of the file", "[mapped_file]") {
    std::vector<std::uint8_t> content(100000);
    for (std::size_t i = 0; i < content.size(); ++i) {
        content[i] = static_cast<std::uint8_t>(i * 7);
    }
    const TempFile file(content);
    MappedFile map;
    std::string error;
    REQUIRE(map.open(file.path(), error));
    CHECK(error.empty());
    CHECK(std::ranges::equal(map.bytes(), content));
}

TEST_CASE("an empty file maps to an empty span", "[mapped_file]") {
    const TempFile file({});
    MappedFile map;
    std::string error;
    REQUIRE(map.open(file.path(), error));
    CHECK(map.bytes().empty());
}

TEST_CASE("a file that is not there is refused and named", "[mapped_file]") {
    const std::string path = (std::filesystem::temp_directory_path() / "skydot_no_such_file.bin").string();
    MappedFile map;
    std::string error;
    CHECK_FALSE(map.open(path, error));
    CHECK(error.find(path) != std::string::npos);
    CHECK(map.bytes().empty());
}
