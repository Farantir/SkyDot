// SPDX-License-Identifier: GPL-3.0-or-later
//
// SpanReader guards all untrusted input, so its failure paths get the most
// tests.
#include "bethconv/io/span_reader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace bethconv::io;

namespace {

std::vector<std::byte> make_bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

SpanReader reader_over(const std::vector<std::byte>& data) {
    return SpanReader(std::span<const std::byte>(data), "test.bin");
}

} // namespace

TEST_CASE("get<T> reads little-endian scalars", "[span_reader]") {
    const auto data = make_bytes({0x78, 0x56, 0x34, 0x12, 0xFF, 0xFF});
    auto r = reader_over(data);

    const auto dword = r.get<std::uint32_t>();
    REQUIRE(dword.has_value());
    CHECK(*dword == 0x12345678u);
    CHECK(r.position() == 4);
    CHECK(r.remaining() == 2);

    const auto word = r.get<std::int16_t>();
    REQUIRE(word.has_value());
    CHECK(*word == -1);
    CHECK(r.at_end());
}

TEST_CASE("get<T> past the end fails instead of reading", "[span_reader]") {
    const auto data = make_bytes({0x01, 0x02});
    auto r = reader_over(data);

    const auto v = r.get<std::uint32_t>();
    REQUIRE_FALSE(v.has_value());
    CHECK(v.error().kind == ErrorKind::truncated);
    CHECK(v.error().origin == "test.bin");
    CHECK(v.error().offset == 0);
    // A failed read must not consume: the caller may try a shorter field.
    CHECK(r.position() == 0);
}

TEST_CASE("peek does not advance", "[span_reader]") {
    const auto data = make_bytes({0xAA, 0xBB});
    auto r = reader_over(data);

    const auto a = r.peek<std::uint8_t>();
    const auto b = r.peek<std::uint8_t>();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == 0xAA);
    CHECK(*b == 0xAA);
    CHECK(r.position() == 0);
}

TEST_CASE("tags read in character order regardless of host endianness", "[span_reader]") {
    const auto data = make_bytes({'T', 'E', 'S', '4'});
    auto r = reader_over(data);

    const auto tag = r.tag();
    REQUIRE(tag.has_value());
    CHECK(*tag == FourCC{"TES4"});
    CHECK(*tag != FourCC{"GRUP"});
    CHECK(tag->to_string() == "TES4");
}

TEST_CASE("FourCC printing sanitizes control bytes", "[span_reader]") {
    const auto data = make_bytes({0x00, 0x01, 'A', 0x7F});
    auto r = reader_over(data);
    const auto tag = r.tag();
    REQUIRE(tag.has_value());
    CHECK(tag->to_string() == "..A.");
}

TEST_CASE("zstring stops at the NUL and consumes it", "[span_reader]") {
    const auto data = make_bytes({'a', 'b', 'c', 0x00, 'd'});
    auto r = reader_over(data);

    const auto s = r.zstring();
    REQUIRE(s.has_value());
    CHECK(*s == "abc");
    CHECK(r.position() == 4);
}

TEST_CASE("unterminated zstring is an error, not a truncated string", "[span_reader]") {
    // A name field without terminator must not run into the next record.
    const auto data = make_bytes({'a', 'b', 'c'});
    auto r = reader_over(data);

    const auto s = r.zstring();
    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().kind == ErrorKind::unterminated);
}

TEST_CASE("bzstring length includes the terminator", "[span_reader]") {
    const auto data = make_bytes({0x04, 'f', 'o', 'o', 0x00});
    auto r = reader_over(data);

    const auto s = r.bzstring();
    REQUIRE(s.has_value());
    CHECK(*s == "foo");
    CHECK(r.at_end());
}

TEST_CASE("bzstring with a length past the end fails", "[span_reader]") {
    const auto data = make_bytes({0x40, 'f', 'o', 'o'});
    auto r = reader_over(data);

    const auto s = r.bzstring();
    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().kind == ErrorKind::truncated);
}

TEST_CASE("wstring is length-prefixed and unterminated", "[span_reader]") {
    const auto data = make_bytes({0x03, 0x00, 'b', 'a', 'r'});
    auto r = reader_over(data);

    const auto s = r.wstring();
    REQUIRE(s.has_value());
    CHECK(*s == "bar");
}

TEST_CASE("fixed_string trims NUL and space padding", "[span_reader]") {
    const auto data = make_bytes({'i', 'r', 'o', 'n', 0x00, 0x00, ' ', 0x00});
    auto r = reader_over(data);

    const auto s = r.fixed_string(8);
    REQUIRE(s.has_value());
    CHECK(*s == "iron");
    CHECK(r.at_end());
}

TEST_CASE("array rejects a count that would overflow the size computation",
          "[span_reader]") {
    // A hostile count of 2^61 8-byte elements: count * sizeof(T) wraps to a
    // small value, which a naive check would accept.
    const auto data = make_bytes({0, 0, 0, 0, 0, 0, 0, 0});
    auto r = reader_over(data);

    const auto huge = std::size_t{1} << 61;
    const auto v = r.array<std::uint64_t>(huge);
    REQUIRE_FALSE(v.has_value());
    CHECK(v.error().kind == ErrorKind::truncated);
    CHECK(r.position() == 0);
}

TEST_CASE("array reads a contiguous run", "[span_reader]") {
    const auto data = make_bytes({0x01, 0x00, 0x02, 0x00, 0x03, 0x00});
    auto r = reader_over(data);

    const auto v = r.array<std::uint16_t>(3);
    REQUIRE(v.has_value());
    CHECK(*v == std::vector<std::uint16_t>{1, 2, 3});
    CHECK(r.at_end());
}

TEST_CASE("array of zero elements succeeds and consumes nothing", "[span_reader]") {
    const auto data = make_bytes({0x01});
    auto r = reader_over(data);

    const auto v = r.array<std::uint32_t>(0);
    REQUIRE(v.has_value());
    CHECK(v->empty());
    CHECK(r.position() == 0);
}

TEST_CASE("subreader confines a child to its range", "[span_reader]") {
    const auto data = make_bytes({0xAA, 0xBB, 0xCC, 0xDD});
    auto r = reader_over(data);

    auto child = r.subreader(2);
    REQUIRE(child.has_value());
    CHECK(r.position() == 2); // parent advanced past the child's range

    CHECK(child->remaining() == 2);
    const auto overrun = child->get<std::uint32_t>();
    REQUIRE_FALSE(overrun.has_value());
    // The child cannot read the parent's remaining bytes even though they are
    // adjacent in memory; nested formats rely on this.
    CHECK(overrun.error().kind == ErrorKind::truncated);
}

TEST_CASE("subreader errors report absolute file offsets", "[span_reader]") {
    const auto data = make_bytes({0, 0, 0, 0, 0, 0, 0, 0});
    auto r = reader_over(data);
    REQUIRE(r.skip(4).has_value());

    auto child = r.subreader(4);
    REQUIRE(child.has_value());
    REQUIRE(child->skip(2).has_value());
    CHECK(child->position() == 2);
    CHECK(child->absolute_position() == 6);

    const auto fail = child->get<std::uint32_t>();
    REQUIRE_FALSE(fail.has_value());
    // The error offset is the absolute file offset, not 2.
    CHECK(fail.error().offset == 6);
    CHECK(fail.error().to_string().starts_with("test.bin+0x6: truncated"));
}

TEST_CASE("subreader larger than the parent fails", "[span_reader]") {
    const auto data = make_bytes({0x01, 0x02});
    auto r = reader_over(data);

    const auto child = r.subreader(64);
    REQUIRE_FALSE(child.has_value());
    CHECK(child.error().kind == ErrorKind::truncated);
    CHECK(r.position() == 0);
}

TEST_CASE("subreader_at is bounds-checked against overflow", "[span_reader]") {
    const auto data = make_bytes({0x01, 0x02, 0x03, 0x04});
    const auto r = reader_over(data);

    CHECK(r.subreader_at(1, 3).has_value());
    CHECK_FALSE(r.subreader_at(1, 4).has_value());
    CHECK_FALSE(r.subreader_at(5, 0).has_value());
    // offset + n overflowing SIZE_MAX must not count as in range.
    CHECK_FALSE(r.subreader_at(2, std::size_t{0} - 1).has_value());
}

TEST_CASE("seek and skip are checked", "[span_reader]") {
    const auto data = make_bytes({0x01, 0x02, 0x03, 0x04});
    auto r = reader_over(data);

    CHECK(r.seek(4).has_value()); // seeking to exactly the end is legal
    CHECK(r.at_end());
    CHECK_FALSE(r.seek(5).has_value());
    CHECK(r.position() == 4); // a failed seek leaves the cursor alone

    REQUIRE(r.seek(0).has_value());
    CHECK_FALSE(r.skip(5).has_value());
    CHECK(r.position() == 0);
}

TEST_CASE("a default-constructed reader is empty and safe", "[span_reader]") {
    SpanReader r;
    CHECK(r.size() == 0);
    CHECK(r.at_end());
    CHECK_FALSE(r.get<std::uint8_t>().has_value());
    CHECK_FALSE(r.zstring().has_value());
}

TEST_CASE("bytes() borrows without copying", "[span_reader]") {
    const auto data = make_bytes({0x10, 0x20, 0x30});
    auto r = reader_over(data);

    const auto b = r.bytes(2);
    REQUIRE(b.has_value());
    CHECK(b->size() == 2);
    CHECK(b->data() == data.data());
    CHECK(r.remaining() == 1);
}
