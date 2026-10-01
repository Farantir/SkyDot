// SPDX-License-Identifier: GPL-3.0-or-later
//
// Content hash. Plain concatenation (source ‖ converter ‖ settings) is not
// injective, so each component is length-prefixed; these tests keep it that
// way.
#include "bethconv/pack/content_hash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

using namespace bethconv::pack;

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

} // namespace

TEST_CASE("a hash is 64 hex characters and its own first byte", "[content-hash]") {
    const auto hash = content_hash(bytes_of("abc"), "bethconv 0.0.1", "mesh/1");
    const auto hex = hash.hex();
    REQUIRE(hex.size() == 64);
    CHECK(hex.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(hash.prefix() == hex.substr(0, 2));
}

TEST_CASE("the same input hashes the same way twice", "[content-hash]") {
    const auto a = content_hash(bytes_of("payload"), "bethconv 0.0.1", "mesh/1");
    const auto b = content_hash(bytes_of("payload"), "bethconv 0.0.1", "mesh/1");
    CHECK(a == b);
}

TEST_CASE("a shifted component boundary does not collide", "[content-hash]") {
    // Without length prefixes these two would be the same byte stream.
    const auto a = content_hash(bytes_of("ab"), "c", "settings");
    const auto b = content_hash(bytes_of("a"), "bc", "settings");
    CHECK_FALSE(a == b);
}

TEST_CASE("an empty component is distinguishable from an absent one",
          "[content-hash]") {
    const auto a = content_hash({}, "bethconv", "mesh/1");
    const auto b = content_hash(bytes_of(""), "bethconv", "mesh/1");
    // Both empty: equal.
    CHECK(a == b);
    // But empty source + settings "x" differs from source "x" + empty
    // settings.
    CHECK_FALSE(content_hash({}, "bethconv", "x") ==
                content_hash(bytes_of("x"), "bethconv", ""));
}

TEST_CASE("the converter version is part of the name", "[content-hash]") {
    // A new converter version gives a new name, so stale assets are never
    // reused.
    const auto old_build = content_hash(bytes_of("payload"), "bethconv 0.0.1", "mesh/1");
    const auto new_build = content_hash(bytes_of("payload"), "bethconv 0.0.2", "mesh/1");
    CHECK_FALSE(old_build == new_build);
}

TEST_CASE("the settings are part of the name", "[content-hash]") {
    const auto scaled = content_hash(bytes_of("payload"), "bethconv", "mesh/1;scale=0.0142875");
    const auto unscaled = content_hash(bytes_of("payload"), "bethconv", "mesh/1;scale=1");
    CHECK_FALSE(scaled == unscaled);
}

TEST_CASE("streaming a component in chunks matches hashing it whole",
          "[content-hash]") {
    // `absorb_raw` must not add a length prefix, or chunked hashing breaks.
    const auto whole = bytes_of("the quick brown fox");
    Hasher chunked;
    chunked.absorb(std::span<const std::byte>{});  // frames nothing
    Hasher direct;
    direct.absorb(std::span<const std::byte>{});
    direct.absorb_raw(whole);

    chunked.absorb_raw(std::span(whole).first(4));
    chunked.absorb_raw(std::span(whole).subspan(4));
    CHECK(chunked.finish() == direct.finish());
}

TEST_CASE("a text component and the same bytes hash alike", "[content-hash]") {
    // Both absorb overloads must frame identically.
    Hasher as_text;
    as_text.absorb(std::string_view{"settings"});
    Hasher as_bytes;
    as_bytes.absorb(std::span<const std::byte>(bytes_of("settings")));
    CHECK(as_text.finish() == as_bytes.finish());
}
