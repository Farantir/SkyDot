// SPDX-License-Identifier: GPL-3.0-or-later
//
// The save container (save/save_file.hpp): sections round trip, a damaged,
// truncated or foreign file is refused before anything is decoded, unknown
// tags are skipped, and writing goes through a temporary file.
#include "save/save_file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace skydot::save;

namespace {

std::vector<std::uint8_t> sample() {
    SaveWriter w;
    w.add("HEAD", 1, {1, 2, 3});
    w.add("VMST", 2, {});
    w.add("QUST", 1, std::vector<std::uint8_t>(1000, 7));
    return w.finish();
}

} // namespace

TEST_CASE("the CRC-32 matches the standard check value", "[save]") {
    const std::vector<std::uint8_t> digits{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc32(digits.data(), digits.size()) == 0xCBF43926U);
}

TEST_CASE("sections come back as written", "[save]") {
    SaveReader r;
    std::string error;
    REQUIRE(r.parse(sample(), error));
    REQUIRE(r.sections().size() == 3);
    const auto* head = r.find("HEAD");
    REQUIRE(head != nullptr);
    CHECK(head->version == 1);
    CHECK(head->size == 3);
    auto in = r.open(*head);
    std::uint8_t b = 0;
    REQUIRE(in.u8(b));
    CHECK(b == 1);
    CHECK(r.find("VMST")->version == 2);
    CHECK(r.find("VMST")->size == 0);
    CHECK(r.find("QUST")->size == 1000);
    CHECK(r.find("NONE") == nullptr);
}

TEST_CASE("a file with other bytes, a bad checksum or a cut end is refused", "[save]") {
    SaveReader r;
    std::string error;
    auto bytes = sample();
    auto flipped = bytes;
    flipped[40] ^= 0x01;
    CHECK_FALSE(r.parse(flipped, error));
    CHECK(error.find("checksum") != std::string::npos);
    for (const std::size_t keep : {std::size_t{0}, std::size_t{3}, std::size_t{11}, bytes.size() / 2, bytes.size() - 1}) {
        CHECK_FALSE(r.parse(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(keep)), error));
    }
    auto foreign = bytes;
    foreign[0] = 'X';
    CHECK_FALSE(r.parse(foreign, error));
    CHECK(error == "not a save");
    CHECK_FALSE(r.parse({'j', 'u', 'n', 'k'}, error));
}

TEST_CASE("a section running past the end or a repeated tag is refused even with a valid CRC", "[save]") {
    auto reframe = [](std::vector<std::uint8_t> body) {
        ByteWriter w;
        for (const char c : {'S', 'K', 'Y', 'S'}) {
            w.u8(static_cast<std::uint8_t>(c));
        }
        w.u32(k_container_version);
        w.out.insert(w.out.end(), body.begin(), body.end());
        w.u32(crc32(w.out.data(), w.out.size()));
        return w.out;
    };
    SaveReader r;
    std::string error;
    ByteWriter longer;
    for (const char c : {'A', 'B', 'C', 'D'}) {
        longer.u8(static_cast<std::uint8_t>(c));
    }
    longer.u32(1);
    longer.u64(500); // more than follows
    CHECK_FALSE(r.parse(reframe(longer.out), error));
    SaveWriter twice;
    twice.add("ABCD", 1, {});
    twice.add("ABCD", 1, {});
    CHECK_FALSE(r.parse(twice.finish(), error));
    SaveWriter future;
    future.add("ABCD", 1, {});
    auto bytes = future.finish();
    bytes[4] = 9; // container version
    CHECK_FALSE(r.parse(bytes, error));
}

TEST_CASE("writing goes through a temporary file and replaces the old one", "[save]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "skydot_save_file_test";
    fs::create_directories(dir);
    const std::string path = (dir / "game.skydot").string();
    std::string error;
    REQUIRE(SaveWriter::write_file(path, {1, 2, 3}, error));
    REQUIRE(SaveWriter::write_file(path, sample(), error));
    CHECK_FALSE(fs::exists(path + ".tmp"));
    std::vector<std::uint8_t> back;
    REQUIRE(SaveReader::read_file(path, back, error));
    SaveReader r;
    CHECK(r.parse(back, error));
    CHECK_FALSE(SaveWriter::write_file((dir / "no" / "such" / "dir.skydot").string(), {1}, error));
    CHECK_FALSE(SaveReader::read_file((dir / "missing").string(), back, error));
    fs::remove_all(dir);
}
