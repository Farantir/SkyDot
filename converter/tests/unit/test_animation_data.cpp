// SPDX-License-Identifier: GPL-3.0-or-later
//
// Behaviour characters' animation lists (hkbCharacterStringData) and the
// animationdata text files: project clips and root motion. Text fixtures
// mimic vanilla's (CRLF, names with spaces).
#include "bethconv/animation/animation_data.hpp"
#include "bethconv/animation/hkx.hpp"
#include "bethconv/pack/animation_asset.hpp"

#include "../support/hkx_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <vector>

using bethconv::animation::is_animation_data;
using bethconv::animation::read_animation_data;
using bethconv::animation::read_bound_anims;
using bethconv::animation::read_hkx;
using bethconv::animation::read_project;
using bethconv::animation::read_single_file;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<std::byte> bytes_of(const std::string& text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

const std::string k_project = "1\r\n3\r\nBehaviors\\0_Master.hkx\r\nCharacters\\DefaultMale.hkx\r\n"
                              "Character Assets\\skeleton.HKX\r\n1\r\n"
                              "MT_WalkForward\r\n965\r\n1\r\n0\r\n0\r\n2\r\nFootLeft:0.2\r\nFootRight:0.766667\r\n\r\n"
                              "MT WalkingCamera\r\n969\r\n1.2\r\n0\r\n0\r\n0\r\n\r\n";

const std::string k_bound = "965\r\n1.13333\r\n1\r\n1.13333 0 93.448 0\r\n1\r\n1.13333 0 0 0 1\r\n\r\n"
                            "960\r\n2\r\n2\r\n1 0 10 0\r\n2 0 20 0\r\n2\r\n1 0 0 0.7071 0.7071\r\n2 0 0 1 0\r\n";

} // namespace

TEST_CASE("a character's animation list reads with 4- and 8-byte pointers", "[animation][hkx]") {
    for (const std::uint8_t ptr : {std::uint8_t{4}, std::uint8_t{8}}) {
        bethconv::test::HkxBuilder b(ptr);
        b.add_character("DefaultMale", "Character Assets\\skeleton.HKX", "Behaviors\\0_Master.hkx",
                        {"Animations\\mt_idle.hkx", "Animations\\MT_WalkForward.hkx"});
        const auto file = read_hkx(b.bytes(), "defaultmale.hkx");
        REQUIRE(file.has_value());
        REQUIRE(file->characters.size() == 1);
        const auto& c = file->characters[0];
        CHECK(c.name == "DefaultMale");
        CHECK(c.rig == "Character Assets\\skeleton.HKX");
        CHECK(c.ragdoll.empty());
        CHECK(c.behavior == "Behaviors\\0_Master.hkx");
        CHECK(c.animations == std::vector<std::string>{"Animations\\mt_idle.hkx", "Animations\\MT_WalkForward.hkx"});

        const auto back = bethconv::pack::read_animation_asset(bethconv::pack::write_animation_asset(*file), "c");
        REQUIRE(back.has_value());
        REQUIRE(back->characters.size() == 1);
        CHECK(back->characters[0].animations == c.animations);
        CHECK(back->characters[0].behavior == c.behavior);
    }
}

TEST_CASE("a behaviour's clip generators name their files", "[animation][hkx]") {
    for (const std::uint8_t ptr : {std::uint8_t{4}, std::uint8_t{8}}) {
        bethconv::test::HkxBuilder b(ptr);
        b.add_clip_generator("MT_WalkForward", "Animations\\MT_WalkForward.hkx");
        b.add_clip_generator("MT_Idle", "Animations\\mt_idle.hkx");
        const auto file = read_hkx(b.bytes(), "mt_behavior.hkx");
        REQUIRE(file.has_value());
        REQUIRE(file->clip_generators.size() == 2);
        CHECK(file->clip_generators[0].name == "MT_WalkForward");
        CHECK(file->clip_generators[0].animation == "Animations\\MT_WalkForward.hkx");
        const auto back = bethconv::pack::read_animation_asset(bethconv::pack::write_animation_asset(*file), "b");
        REQUIRE(back.has_value());
        REQUIRE(back->clip_generators.size() == 2);
        CHECK(back->clip_generators[1].animation == "Animations\\mt_idle.hkx");
    }
}

TEST_CASE("animationdata paths", "[animation][data]") {
    CHECK(is_animation_data("meshes/animationdata/defaultmale.txt"));
    CHECK(is_animation_data("meshes/animationdata/boundanims/anims_defaultmale.txt"));
    CHECK_FALSE(is_animation_data("meshes/animationdata/dirlist.txt"));
    CHECK(is_animation_data("meshes/animationdatasinglefile.txt"));
    CHECK_FALSE(is_animation_data("meshes/animationdata/other/x.txt"));
    CHECK_FALSE(is_animation_data("meshes/animationdata/defaultmale.hkx"));
}

TEST_CASE("a project file gives files and clips", "[animation][data]") {
    const auto data = read_project(bytes_of(k_project), "defaultmale.txt");
    REQUIRE(data.has_value());
    CHECK(data->files.size() == 3);
    CHECK(data->files[1] == "Characters\\DefaultMale.hkx");
    REQUIRE(data->clips.size() == 2);
    CHECK(data->clips[0].name == "MT_WalkForward");
    CHECK(data->clips[0].animation == 965);
    REQUIRE(data->clips[0].annotations.size() == 2);
    CHECK(data->clips[0].annotations[1].second == "FootRight");
    CHECK_THAT(data->clips[0].annotations[1].first, WithinAbs(0.766667, 1e-6));
    CHECK(data->clips[1].name == "MT WalkingCamera");
    CHECK_THAT(data->clips[1].speed, WithinAbs(1.2, 1e-6));

    // Without clips (traps, doors): files only.
    const auto none = read_project(bytes_of("1\r\n1\r\nAnimations\\fire.hkx\r\n0\r\n"), "trap.txt");
    REQUIRE(none.has_value());
    CHECK(none->clips.empty());
    CHECK(none->files == std::vector<std::string>{"Animations\\fire.hkx"});
}

TEST_CASE("a boundanims file gives root motion", "[animation][data]") {
    const auto data = read_bound_anims(bytes_of(k_bound), "anims_defaultmale.txt");
    REQUIRE(data.has_value());
    REQUIRE(data->motions.size() == 2);
    const auto& walk = data->motions[0];
    CHECK(walk.animation == 965);
    CHECK_THAT(walk.duration, WithinAbs(1.13333, 1e-6));
    REQUIRE(walk.translations.size() == 1);
    CHECK_THAT(walk.translations[0].translation[1], WithinAbs(93.448, 1e-4));
    REQUIRE(data->motions[1].rotations.size() == 2);
    CHECK(data->motions[1].rotations[1].rotation[2] == 1.0F);

    // The asset keeps all of it.
    auto both = *data;
    both.files = {"Characters\\DefaultMale.hkx"};
    const auto back = bethconv::pack::read_project_asset(bethconv::pack::write_animation_asset(both), "x");
    REQUIRE(back.has_value());
    CHECK(back->files == both.files);
    REQUIRE(back->motions.size() == 2);
    CHECK(back->motions[1].translations[1].translation[1] == 20.0F);
    CHECK(back->motions[1].rotations[0].rotation[3] == data->motions[1].rotations[0].rotation[3]);
}

TEST_CASE("the single file holds projects with their motions", "[animation][data]") {
    // Line counts: the project block, then (it has clips) the motion block.
    const auto lines = [](const std::string& text) {
        std::size_t n = 0;
        for (const char c : text) {
            n += c == '\n' ? 1 : 0;
        }
        return std::to_string(n) + "\r\n";
    };
    const std::string trap = "1\r\n1\r\nAnimations\\fire.hkx\r\n0\r\n";
    const std::string text = "2\r\nDefaultMale.txt\r\nBowTrap.txt\r\n" + lines(k_project) + k_project + lines(k_bound) +
                             k_bound + lines(trap) + trap;
    const auto data = read_single_file(bytes_of(text), "meshes/animationdatasinglefile.txt");
    REQUIRE(data.has_value());
    REQUIRE(data->projects.size() == 2);
    CHECK(data->projects[0].name == "DefaultMale");
    CHECK(data->projects[0].clips.size() == 2);
    CHECK(data->projects[0].motions.size() == 2);
    CHECK(data->projects[1].name == "BowTrap");
    CHECK(data->projects[1].files.size() == 1);
    CHECK(data->projects[1].motions.empty());

    const auto back = bethconv::pack::read_project_asset(bethconv::pack::write_animation_asset(*data), "x");
    REQUIRE(back.has_value());
    REQUIRE(back->projects.size() == 2);
    // Sorted by lowercase name.
    CHECK(back->projects[0].name == "BowTrap");
    CHECK(back->projects[1].motions[0].translations[0].translation[1] == data->projects[0].motions[0].translations[0].translation[1]);

    // A block count past the end is an error.
    CHECK_FALSE(read_single_file(bytes_of("1\r\nA.txt\r\n99\r\n1\r\n"), "x").has_value());
    for (std::size_t n = 0; n < text.size(); n += 7) {
        (void)read_single_file(bytes_of(text.substr(0, n)), "cut");
    }
}

TEST_CASE("malformed animationdata is an error, not a crash", "[animation][data]") {
    CHECK_FALSE(read_project(bytes_of("2\r\n0\r\n0\r\n"), "x").has_value());
    CHECK_FALSE(read_project(bytes_of("1\r\n5\r\na\r\n"), "x").has_value());
    CHECK_FALSE(read_project(bytes_of("1\r\n0\r\n1\r\nClip\r\nx\r\n1\r\n0\r\n0\r\n0\r\n"), "x").has_value());
    CHECK_FALSE(read_project(bytes_of("1\r\n0\r\n1\r\nClip\r\n1\r\n1\r\n0\r\n0\r\n1\r\nno time\r\n"), "x").has_value());
    CHECK_FALSE(read_project(bytes_of("1\r\n99999999\r\n"), "x").has_value());
    CHECK_FALSE(read_bound_anims(bytes_of("1\r\n1\r\n1\r\n1 2 3\r\n0\r\n"), "x").has_value());
    CHECK_FALSE(read_bound_anims(bytes_of("1\r\n1\r\n0\r\n1\r\n1 0 0 0 nan\r\n"), "x").has_value());
    CHECK(read_bound_anims(bytes_of(""), "x").has_value());
    // Every prefix of a good file reads or fails cleanly.
    for (std::size_t n = 0; n < k_project.size(); ++n) {
        (void)read_project(bytes_of(k_project.substr(0, n)), "cut");
    }
    for (std::size_t n = 0; n < k_bound.size(); ++n) {
        (void)read_animation_data(bytes_of(k_bound.substr(0, n)), "meshes/animationdata/boundanims/cut.txt");
    }
}
