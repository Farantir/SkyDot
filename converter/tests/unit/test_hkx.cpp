// SPDX-License-Identifier: GPL-3.0-or-later
//
// Havok packfiles: skeletons, spline-compressed and interleaved animations,
// and the animation asset. Fixtures come from tests/support/hkx_builder.hpp
// in both pointer sizes (LE 4, SE 8).
#include "bethconv/animation/hkx.hpp"
#include "bethconv/pack/animation_asset.hpp"

#include "../support/hkx_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using bethconv::animation::read_hkx;
using bethconv::animation::sample;
using bethconv::test::HkxBlock;
using bethconv::test::HkxBone;
using bethconv::test::HkxBuilder;
using bethconv::test::HkxChannel;
using bethconv::test::HkxClip;
using bethconv::test::HkxTrack;
using Catch::Matchers::WithinAbs;

namespace {

constexpr std::uint8_t k_pointer_sizes[] = {4, 8};

/// Degree-1 spline from `a` at frame 0 to `b` at frame `last`.
HkxChannel line(std::array<float, 4> a, std::array<float, 4> b, std::uint8_t last) {
    HkxChannel c;
    c.kind = HkxChannel::Kind::spline;
    c.degree = 1;
    c.knots = {0, 0, last, last};
    c.points = {a, b};
    return c;
}

std::array<float, 4> z_turn(float degrees) {
    const float h = degrees * 3.14159265f / 360.0f;
    return {0, 0, std::sin(h), std::cos(h)};
}

std::vector<HkxBone> three_bones() {
    return {{"NPC Root [Root]", -1, {0, 0, 0}, {0, 0, 0, 1}},
            {"NPC Spine [Spn0]", 0, {0, 0, 70}, {0, 0, 0, 1}},
            {"NPC Head [Head]", 1, {0, 2, 50}, z_turn(30)}};
}

/// 11 frames, one block: track 0 moves (0,0,0) -> (10,20,30), track 1 turns
/// 0 -> 90 degrees about Z, track 2 holds a constant pose.
HkxClip walking_clip() {
    HkxClip clip;
    clip.frames = 11;
    clip.tracks = 3;
    clip.skeleton_name = "NPC Root [Root]";
    clip.annotations = {{0.2f, "FootLeft"}, {0.3f, "SoundPlay.NPCHumanFootstep"}};
    HkxBlock block;
    block.tracks.push_back(HkxTrack{line({0, 0, 0, 0}, {10, 20, 30, 0}, 10), {}});
    block.tracks.push_back(HkxTrack{{}, line(z_turn(0), z_turn(90), 10)});
    block.tracks.push_back(HkxTrack{HkxChannel::constant({1, 2, 3, 0}), HkxChannel::constant(z_turn(45))});
    clip.blocks.push_back(std::move(block));
    return clip;
}

std::vector<std::byte> file_with(std::uint8_t pointer_size, const HkxClip& clip, bool skeleton = true) {
    HkxBuilder b(pointer_size);
    if (skeleton) {
        b.add_skeleton("NPC Root [Root]", three_bones(), {"hkVis:Shield"});
    }
    b.add_clip(clip);
    return b.bytes();
}

float angle_degrees(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    d = std::min(d, 1.0f);
    return 2.0f * std::acos(d) * 180.0f / 3.14159265f;
}

} // namespace

TEST_CASE("a skeleton reads the same with 4- and 8-byte pointers", "[animation][hkx]") {
    for (const auto ptr : k_pointer_sizes) {
        HkxBuilder b(ptr);
        b.add_skeleton("NPC Root [Root]", three_bones(), {"hkVis:Shield", "hkFade:AnimObjectA"});
        const auto file = read_hkx(b.bytes(), "skeleton.hkx");
        REQUIRE(file.has_value());
        CHECK(file->pointer_size == ptr);
        CHECK(file->version == "hk_2010.2.0-r1");
        REQUIRE(file->skeletons.size() == 1);
        const auto& s = file->skeletons[0];
        CHECK(s.name == "NPC Root [Root]");
        CHECK(s.bones == std::vector<std::string>{"NPC Root [Root]", "NPC Spine [Spn0]", "NPC Head [Head]"});
        CHECK(s.parents == std::vector<std::int16_t>{-1, 0, 1});
        CHECK(s.float_slots == std::vector<std::string>{"hkVis:Shield", "hkFade:AnimObjectA"});
        REQUIRE(s.reference_pose.size() == 3);
        CHECK(s.reference_pose[2].translation == std::array<float, 3>{0, 2, 50});
        CHECK_THAT(static_cast<double>(s.reference_pose[2].rotation[2]),
                   WithinAbs(static_cast<double>(z_turn(30)[2]), 1e-6));
        CHECK(file->classes.at("hkaSkeleton") == 1);
    }
}

TEST_CASE("spline tracks sample to their control points", "[animation][hkx]") {
    for (const auto ptr : k_pointer_sizes) {
        const auto file = read_hkx(file_with(ptr, walking_clip()), "walk.hkx");
        REQUIRE(file.has_value());
        REQUIRE(file->clips.size() == 1);
        const auto& clip = file->clips[0];
        CHECK(clip.frame_count == 11);
        CHECK(clip.transform_tracks == 3);
        CHECK(clip.skeleton_name == "NPC Root [Root]");
        CHECK_THAT(clip.duration, WithinAbs(10.0 / 30.0, 1e-5));
        REQUIRE(clip.annotations.size() == 1);
        REQUIRE(clip.annotations[0].annotations.size() == 2);
        CHECK(clip.annotations[0].annotations[1].text == "SoundPlay.NPCHumanFootstep");

        // Translation: linear and exact at 16 bits.
        const auto mid = sample(clip, 0, 5);
        CHECK_THAT(mid.translation[0], WithinAbs(5.0, 1e-3));
        CHECK_THAT(mid.translation[1], WithinAbs(10.0, 1e-3));
        CHECK_THAT(mid.translation[2], WithinAbs(15.0, 1e-3));
        CHECK(mid.rotation == std::array<float, 4>{0, 0, 0, 1});
        // Rotation: the ends to THREECOMP40 precision, the middle normalized.
        CHECK(angle_degrees(sample(clip, 1, 0).rotation, z_turn(0)) < 0.1f);
        CHECK(angle_degrees(sample(clip, 1, 10).rotation, z_turn(90)) < 0.1f);
        CHECK(angle_degrees(sample(clip, 1, 5).rotation, z_turn(45)) < 0.2f);
        // Constants.
        const auto held = sample(clip, 2, 7);
        CHECK(held.translation == std::array<float, 3>{1, 2, 3});
        CHECK(angle_degrees(held.rotation, z_turn(45)) < 0.1f);
        CHECK(held.scale == std::array<float, 3>{1, 1, 1});
    }
}

TEST_CASE("a clip spanning blocks continues across their shared frame", "[animation][hkx]") {
    for (const auto ptr : k_pointer_sizes) {
        // 300 frames in blocks of 256: block 0 has frames 0-255, block 1
        // frames 255-299 at local frames 0-44.
        HkxClip clip;
        clip.frames = 300;
        clip.tracks = 2;
        clip.track_offsets = true;
        HkxBlock first;
        first.tracks.push_back(HkxTrack{line({0, 0, 0, 0}, {255, 0, 0, 0}, 255), {}});
        first.tracks.push_back(HkxTrack{HkxChannel::constant({0, 0, 1, 0}), line(z_turn(0), z_turn(10), 255)});
        HkxBlock second;
        second.tracks.push_back(HkxTrack{line({255, 0, 0, 0}, {299, 0, 0, 0}, 44), {}});
        second.tracks.push_back(HkxTrack{HkxChannel::constant({0, 0, 1, 0}), HkxChannel::constant(z_turn(10))});
        clip.blocks = {first, second};
        const auto file = read_hkx(file_with(ptr, clip), "long.hkx");
        REQUIRE(file.has_value());
        const auto& c = file->clips[0];
        REQUIRE(c.blocks.size() == 2);
        CHECK(c.frames_per_block == 256);
        for (const std::uint32_t f : {0u, 100u, 254u, 255u, 256u, 270u, 299u}) {
            CHECK_THAT(sample(c, 0, f).translation[0], WithinAbs(static_cast<double>(f), 0.01));
        }
    }
}

TEST_CASE("a track that does not start where transformOffsets says is corrupt", "[animation][hkx]") {
    auto clip = walking_clip();
    clip.track_offsets = true;
    REQUIRE(read_hkx(file_with(8, clip), "ok.hkx").has_value());
    clip.wrong_track_offset = true;
    const auto file = read_hkx(file_with(8, clip), "bad.hkx");
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().kind == bethconv::io::ErrorKind::corrupt);
}

TEST_CASE("float tracks: masks padded to 4 bytes, constants and 16-bit splines", "[animation][hkx]") {
    for (const auto ptr : k_pointer_sizes) {
        // 3 transform tracks + 3 float tracks: 15 mask bytes, stored as 16.
        auto clip = walking_clip();
        clip.float_tracks = 3;
        HkxChannel fade;
        fade.kind = HkxChannel::Kind::spline;
        fade.degree = 1;
        fade.knots = {0, 0, 10, 10};
        fade.points = {{0, 0, 0, 0}, {1, 0, 0, 0}};
        clip.blocks[0].floats = {HkxChannel::constant({1, 0, 0, 0}), fade, HkxChannel::constant({0, 0, 0, 0})};
        const auto file = read_hkx(file_with(ptr, clip), "book.hkx");
        REQUIRE(file.has_value());
        const auto& block = file->clips[0].blocks[0];
        REQUIRE(block.floats.size() == 3);
        CHECK_FALSE(block.floats[0].is_spline());
        CHECK(block.floats[0].lo == std::vector<float>{1.0f});
        REQUIRE(block.floats[1].is_spline());
        std::array<float, 1> v{};
        bethconv::animation::evaluate(block.floats[1], 5.0f, v);
        CHECK_THAT(v[0], WithinAbs(0.5, 1e-4));
    }
}

TEST_CASE("8-bit positions widen to 16 bits exactly", "[animation][hkx]") {
    auto clip = walking_clip();
    clip.blocks[0].tracks[0].translation.eight_bit = true;
    const auto file = read_hkx(file_with(8, clip), "eight.hkx");
    REQUIRE(file.has_value());
    const auto& ch = file->clips[0].blocks[0].tracks[0].translation;
    REQUIRE(ch.points.size() == 6);
    CHECK(ch.points[3] == 65535); // 255 * 257
    CHECK_THAT(sample(file->clips[0], 0, 10).translation[2], WithinAbs(30.0, 1e-4));
}

TEST_CASE("an interleaved animation becomes one linear spline per block", "[animation][hkx]") {
    for (const auto ptr : k_pointer_sizes) {
        std::vector<std::vector<std::pair<std::array<float, 3>, std::array<float, 4>>>> poses;
        for (int f = 0; f < 300; ++f) {
            const float x = static_cast<float>(f);
            poses.push_back({{{x, 0, 0}, {0, 0, 0, 1}}, {{0, 0, 5}, z_turn(x / 10.0f)}});
        }
        HkxBuilder b(ptr);
        b.add_interleaved(poses, 299.0f / 30.0f);
        const auto file = read_hkx(b.bytes(), "interleaved.hkx");
        REQUIRE(file.has_value());
        REQUIRE(file->clips.size() == 1);
        const auto& c = file->clips[0];
        CHECK(c.encoding == bethconv::animation::ClipEncoding::interleaved);
        CHECK(c.frame_count == 300);
        CHECK(c.blocks.size() == 2);
        for (const std::uint32_t f : {0u, 7u, 255u, 256u, 299u}) {
            CHECK_THAT(sample(c, 0, f).translation[0], WithinAbs(static_cast<double>(f), 0.01));
            CHECK(angle_degrees(sample(c, 1, f).rotation, z_turn(static_cast<float>(f) / 10.0f)) < 0.01f);
        }
    }
}

TEST_CASE("rotation quantizations vanilla never uses are refused, not guessed", "[animation][hkx]") {
    auto clip = walking_clip();
    clip.rotation_quantization = 0; // POLAR32
    const auto file = read_hkx(file_with(8, clip), "polar.hkx");
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().kind == bethconv::io::ErrorKind::unsupported);
}

TEST_CASE("malformed packfiles fail instead of crashing", "[animation][hkx][robustness]") {
    const auto good = file_with(8, walking_clip());
    REQUIRE(read_hkx(good, "good.hkx").has_value());
    // Every truncation either fails or (past the data) still reads.
    for (std::size_t n = 0; n < good.size(); n += 7) {
        const std::span<const std::byte> cut(good.data(), n);
        (void)read_hkx(cut, "cut.hkx");
    }
    // Single flipped bytes anywhere.
    for (std::size_t i = 0; i < good.size(); i += 3) {
        auto bad = good;
        bad[i] = static_cast<std::byte>(static_cast<std::uint8_t>(bad[i]) ^ 0xA5u);
        if (const auto file = read_hkx(bad, "flip.hkx")) {
            for (const auto& clip : file->clips) {
                for (std::uint32_t t = 0; t < clip.transform_tracks; ++t) {
                    (void)sample(clip, t, clip.frame_count / 2);
                }
            }
        }
    }
    CHECK_FALSE(read_hkx(std::vector<std::byte>(64), "zeros.hkx").has_value());
}

TEST_CASE("an animation asset reads back what was written", "[animation][pack]") {
    auto clip = walking_clip();
    clip.float_tracks = 1;
    clip.blocks[0].floats = {line({0, 0, 0, 0}, {2, 0, 0, 0}, 10)};
    const auto file = read_hkx(file_with(8, clip), "walk.hkx");
    REQUIRE(file.has_value());
    const auto asset = bethconv::pack::write_animation_asset(*file);
    const auto back = bethconv::pack::read_animation_asset(asset, "walk.animfb");
    REQUIRE(back.has_value());
    REQUIRE(back->skeletons.size() == 1);
    CHECK(back->skeletons[0].bones == file->skeletons[0].bones);
    CHECK(back->skeletons[0].float_slots == file->skeletons[0].float_slots);
    REQUIRE(back->clips.size() == 1);
    const auto& a = file->clips[0];
    const auto& b = back->clips[0];
    CHECK(b.frame_count == a.frame_count);
    CHECK(b.skeleton_name == a.skeleton_name);
    CHECK(b.annotations[0].annotations[0].text == "FootLeft");
    for (std::uint32_t t = 0; t < a.transform_tracks; ++t) {
        for (std::uint32_t f = 0; f < a.frame_count; ++f) {
            const auto x = sample(a, t, f);
            const auto y = sample(b, t, f);
            CHECK(x.translation == y.translation);
            CHECK(x.rotation == y.rotation);
        }
    }
    CHECK(b.blocks[0].floats[0].points == a.blocks[0].floats[0].points);

    auto broken = asset;
    broken.resize(broken.size() / 2);
    CHECK_FALSE(bethconv::pack::read_animation_asset(broken, "broken.animfb").has_value());
}
