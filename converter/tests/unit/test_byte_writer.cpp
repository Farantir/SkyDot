// SPDX-License-Identifier: GPL-3.0-or-later
//
// ByteWriter lays out glTF buffers. Byte-order and alignment mistakes would be
// invisible otherwise: most loaders accept a misaligned bufferView.
#include "bethconv/io/byte_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using bethconv::io::ByteWriter;

namespace {
std::uint8_t at(const ByteWriter& w, std::size_t i) {
    return static_cast<std::uint8_t>(w.view()[i]);
}
} // namespace

TEST_CASE("scalars are written little-endian regardless of host order", "[io][writer]") {
    ByteWriter w;
    w.put(std::uint32_t{0x12345678});
    REQUIRE(w.size() == 4);
    CHECK(at(w, 0) == 0x78);
    CHECK(at(w, 1) == 0x56);
    CHECK(at(w, 2) == 0x34);
    CHECK(at(w, 3) == 0x12);
}

TEST_CASE("a float keeps its exact bit pattern", "[io][writer]") {
    ByteWriter w;
    w.put(1.0f); // 0x3F800000
    CHECK(at(w, 0) == 0x00);
    CHECK(at(w, 1) == 0x00);
    CHECK(at(w, 2) == 0x80);
    CHECK(at(w, 3) == 0x3F);
}

TEST_CASE("put_all writes elements back to back with no padding", "[io][writer]") {
    ByteWriter w;
    const std::vector<std::uint16_t> values{1, 2, 3};
    w.put_all(std::span<const std::uint16_t>(values));
    REQUIRE(w.size() == 6);
    CHECK(at(w, 0) == 1);
    CHECK(at(w, 2) == 2);
    CHECK(at(w, 4) == 3);
}

TEST_CASE("align_to pads with zeros and is a no-op when already aligned",
          "[io][writer]") {
    ByteWriter w;
    w.put(std::uint8_t{0xFF});
    w.align_to(4);
    REQUIRE(w.size() == 4);
    CHECK(at(w, 1) == 0);
    CHECK(at(w, 3) == 0);

    w.align_to(4);
    CHECK(w.size() == 4);
    w.align_to(1);
    CHECK(w.size() == 4);
    w.align_to(0);
    CHECK(w.size() == 4);
}

TEST_CASE("take moves the buffer out and leaves the writer empty", "[io][writer]") {
    ByteWriter w;
    w.put(std::uint32_t{7});
    const auto bytes = w.take();
    CHECK(bytes.size() == 4);
    CHECK(w.empty());
}
