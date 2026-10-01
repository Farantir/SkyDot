// SPDX-License-Identifier: GPL-3.0-or-later
//
// SpanStream feeds nifly. nifly seeks when following block references and the
// default streambuf cannot seek a user-provided get area, so seeking is what
// needs testing.
#include "bethconv/io/span_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using bethconv::io::SpanStream;

namespace {
constexpr std::array<std::byte, 8> kData{
    std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'},
    std::byte{'e'}, std::byte{'f'}, std::byte{'g'}, std::byte{'h'}};
}

TEST_CASE("reads sequentially without copying", "[io][stream]") {
    SpanStream s(kData);
    std::array<char, 4> buf{};
    s.read(buf.data(), 4);
    CHECK(s.gcount() == 4);
    CHECK(std::string(buf.data(), 4) == "abcd");
}

TEST_CASE("seeks from the beginning, the current position and the end",
          "[io][stream]") {
    SpanStream s(kData);
    char c = 0;

    s.seekg(5, std::ios_base::beg);
    s.get(c);
    CHECK(c == 'f');

    s.seekg(1, std::ios_base::cur);
    s.get(c);
    CHECK(c == 'h');

    s.seekg(-3, std::ios_base::end);
    s.get(c);
    CHECK(c == 'f');
}

TEST_CASE("a seek past either end fails instead of pointing outside the buffer",
          "[io][stream]") {
    SpanStream s(kData);
    s.seekg(100, std::ios_base::beg);
    CHECK(s.fail());

    s.clear();
    s.seekg(-1, std::ios_base::beg);
    CHECK(s.fail());

    // Seeking to exactly the end is legal; the next read gives EOF.
    s.clear();
    s.seekg(0, std::ios_base::end);
    CHECK_FALSE(s.fail());
    char c = 0;
    s.get(c);
    CHECK(s.eof());
}

TEST_CASE("an empty span is a stream that is immediately at EOF", "[io][stream]") {
    SpanStream s({});
    char c = 0;
    s.get(c);
    CHECK(s.eof());
}
