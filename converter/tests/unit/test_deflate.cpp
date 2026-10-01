// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two decompressors, especially the LZ4 one.
//
// rsm-bsa 4.1.0's LZ4 loop never checks progress, and LZ4F_decompress makes
// none on a frame truncated mid-block, so it loops forever (found by fuzz_bsa
// with a 321-byte archive). Our replacement must fail instead.
//
// If any of these tests hangs, that bug is back.
#include "bethconv/io/deflate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <lz4frame.h>
#include <zlib.h>

#include <span>
#include <string>
#include <vector>

using namespace bethconv::io;

namespace {

std::vector<std::byte> as_bytes(const std::string& text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

/// An LZ4 frame (what BSA v105 stores), not a raw block.
std::vector<std::byte> lz4_frame(std::span<const std::byte> plain) {
    const std::size_t bound = ::LZ4F_compressFrameBound(plain.size(), nullptr);
    std::vector<std::byte> out(bound);
    const std::size_t written = ::LZ4F_compressFrame(
        out.data(), out.size(), plain.data(), plain.size(), nullptr);
    REQUIRE_FALSE(::LZ4F_isError(written));
    out.resize(written);
    return out;
}

std::vector<std::byte> zlib_stream(std::span<const std::byte> plain) {
    uLongf bound = ::compressBound(static_cast<uLong>(plain.size()));
    std::vector<std::byte> out(bound);
    const int rc = ::compress(reinterpret_cast<Bytef*>(out.data()), &bound,
                              reinterpret_cast<const Bytef*>(plain.data()),
                              static_cast<uLong>(plain.size()));
    REQUIRE(rc == Z_OK);
    out.resize(bound);
    return out;
}

const std::string k_text = [] {
    std::string s;
    for (int i = 0; i < 400; ++i) {
        s += "the quick brown fox jumps over the lazy dog ";
    }
    return s;
}();

} // namespace

TEST_CASE("an LZ4 frame round-trips to exactly the promised size", "[io][deflate]") {
    const auto plain = as_bytes(k_text);
    const auto frame = lz4_frame(plain);
    REQUIRE(frame.size() < plain.size()); // it really did compress

    const auto out = lz4_decompress_exact(frame, plain.size(), "fixture", 0);
    REQUIRE(out.has_value());
    CHECK(out->size() == plain.size());
    CHECK(*out == plain);
}

TEST_CASE("a frame truncated mid-block fails instead of spinning", "[io][deflate]") {
    // Regression test: this call used to never return.
    const auto plain = as_bytes(k_text);
    auto frame = lz4_frame(plain);
    for (const double fraction : {0.9, 0.75, 0.5, 0.25, 0.1}) {
        auto cut = frame;
        cut.resize(static_cast<std::size_t>(static_cast<double>(frame.size()) * fraction));
        INFO("kept " << cut.size() << " of " << frame.size() << " bytes");
        const auto out = lz4_decompress_exact(cut, plain.size(), "fixture", 0);
        REQUIRE_FALSE(out.has_value());
        CHECK((out.error().kind == ErrorKind::truncated ||
               out.error().kind == ErrorKind::corrupt));
    }
}

TEST_CASE("a frame that ends early fails rather than returning short",
          "[io][deflate]") {
    // Declared size larger than the frame: must fail, not return partial data.
    const auto plain = as_bytes(k_text);
    const auto frame = lz4_frame(plain);
    const auto out = lz4_decompress_exact(frame, plain.size() * 2, "fixture", 0);
    REQUIRE_FALSE(out.has_value());
}

TEST_CASE("a frame that holds more than promised is rejected", "[io][deflate]") {
    // Frame larger than the declared size: detected thanks to the slack byte,
    // not silently truncated.
    const auto plain = as_bytes(k_text);
    const auto frame = lz4_frame(plain);
    const auto out = lz4_decompress_exact(frame, plain.size() - 100, "fixture", 0);
    REQUIRE_FALSE(out.has_value());
    CHECK(out.error().kind == ErrorKind::corrupt);
    CHECK(out.error().detail.find("more than") != std::string::npos);
}

TEST_CASE("garbage is an error, not a hang and not a crash", "[io][deflate]") {
    std::vector<std::byte> junk(4096);
    for (std::size_t i = 0; i < junk.size(); ++i) {
        junk[i] = static_cast<std::byte>((i * 37) & 0xFF);
    }
    CHECK_FALSE(lz4_decompress_exact(junk, 8192, "fixture", 0).has_value());
    CHECK_FALSE(inflate_exact(junk, 8192, "fixture", 0).has_value());
}

TEST_CASE("both decompressors refuse an absurd promised size", "[io][deflate]") {
    const auto plain = as_bytes(k_text);
    const auto frame = lz4_frame(plain);
    const auto stream = zlib_stream(plain);
    const std::size_t absurd = k_max_inflate_size + 1;

    const auto lz4 = lz4_decompress_exact(frame, absurd, "fixture", 0);
    REQUIRE_FALSE(lz4.has_value());
    CHECK(lz4.error().kind == ErrorKind::too_large);

    const auto zlib = inflate_exact(stream, absurd, "fixture", 0);
    REQUIRE_FALSE(zlib.has_value());
    CHECK(zlib.error().kind == ErrorKind::too_large);
}

TEST_CASE("an empty payload is an empty result, not an error", "[io][deflate]") {
    // A zero-length entry is valid.
    CHECK(lz4_decompress_exact({}, 0, "fixture", 0).has_value());
    CHECK(inflate_exact({}, 0, "fixture", 0).has_value());
    // Zero input with nonzero declared output is not.
    CHECK_FALSE(lz4_decompress_exact({}, 16, "fixture", 0).has_value());
    CHECK_FALSE(inflate_exact({}, 16, "fixture", 0).has_value());
}
