// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/io/mapped_file.hpp"

#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace bethconv::io;

namespace {

/// Writes a temp file and removes it on scope exit. It sits in a TempDir so
/// parallel ctest processes cannot share a name.
class TempFile {
public:
    explicit TempFile(std::string_view contents) : path_(dir_.write("file.bin", contents)) {}

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    bethconv::test::TempDir dir_;
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("mapping a file exposes its bytes through a reader", "[mapped_file]") {
    // Explicit length: the payload contains NULs.
    const TempFile file(std::string_view("TES4\x2c\x00\x00\x00", 8));
    auto mapped = MappedFile::open(file.path());
    REQUIRE(mapped.has_value());
    CHECK(mapped->size() == 8);

    auto r = mapped->reader();
    const auto tag = r.tag();
    REQUIRE(tag.has_value());
    CHECK(*tag == FourCC{"TES4"});

    const auto size = r.get<std::uint32_t>();
    REQUIRE(size.has_value());
    CHECK(*size == 0x2C);
}

TEST_CASE("an empty file maps to an empty span, not an error", "[mapped_file]") {
    const TempFile file("");
    auto mapped = MappedFile::open(file.path());
    REQUIRE(mapped.has_value());
    CHECK(mapped->size() == 0);
    CHECK(mapped->reader().at_end());
}

TEST_CASE("a missing file reports the OS error", "[mapped_file]") {
    const auto missing = std::filesystem::temp_directory_path() / "bethconv_no_such_file";
    const auto mapped = MappedFile::open(missing);
    REQUIRE_FALSE(mapped.has_value());
    CHECK_FALSE(mapped.error().detail.empty());
}

TEST_CASE("a directory is rejected rather than mapped", "[mapped_file]") {
    const auto mapped = MappedFile::open(std::filesystem::temp_directory_path());
    REQUIRE_FALSE(mapped.has_value());
}

TEST_CASE("moving a mapping transfers ownership exactly once", "[mapped_file]") {
    const TempFile file("hello");
    auto mapped = MappedFile::open(file.path());
    REQUIRE(mapped.has_value());

    MappedFile moved = std::move(*mapped);
    CHECK(moved.size() == 5);
    CHECK(mapped->size() == 0);
    CHECK_FALSE(mapped->is_open());
}
