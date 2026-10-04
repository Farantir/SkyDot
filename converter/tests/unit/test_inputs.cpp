// SPDX-License-Identifier: GPL-3.0-or-later
//
// prepare_inputs over synthetic installs: a Data folder, a Data folder with a
// list, and a Mod Organizer 2 instance with two mods. Plugins are bare TES4
// headers; archives are real (rsm-bsa) so mounting is exercised, and loose
// files carry their provider's name so precedence can be read back.
#include "bethconv/io/json_text.hpp"
#include "bethconv/pack/inputs.hpp"

#include "../support/bsa_builder.hpp"
#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>

using namespace bethconv;
using bethconv::test::TempDir;

namespace {

void make_plugin(const std::filesystem::path& path, std::uint32_t flags = 0) {
    test::ByteWriter w;
    test::write_tes4(w, flags, {});
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    for (const auto b : w.bytes()) {
        out.put(static_cast<char>(b));
    }
}

[[nodiscard]] std::vector<std::string> plugin_names(const record::LoadOrder& order) {
    std::vector<std::string> names;
    for (const auto& entry : order.entries()) {
        names.push_back(entry.name);
    }
    return names;
}

[[nodiscard]] std::string read_text(const archive::ArchiveSet& set, std::string_view vpath) {
    const auto bytes = set.read(vpath);
    REQUIRE(bytes.has_value());
    std::string text;
    for (const auto b : *bytes) {
        text.push_back(static_cast<char>(b));
    }
    return text;
}

/// The game's Data folder: both masters, two plugins and one archive.
void make_data(const TempDir& dir) {
    make_plugin(dir / "Game/Data/Skyrim.esm", 0x1);
    make_plugin(dir / "Game/Data/Mod.esp");
    make_plugin(dir / "Game/Data/Later.esp");
    // Mod.esp is older, so the game loads it first.
    std::filesystem::last_write_time(
        dir / "Game/Data/Mod.esp",
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
    test::write_bsa(dir / "Game/Data/Skyrim - Meshes0.bsa",
                    {{"meshes/a.nif", "archived"}, {"meshes/c.nif", "archived c"}});
    dir.write("Game/Data/meshes/c.nif", "loose");
}

/// An instance with two mods, High above Low, as MO2 writes modlist.txt
/// (highest priority first).
void make_instance(const TempDir& dir) {
    dir.write("MO2/ModOrganizer.ini",
              "[General]\r\ngameName=Skyrim Special Edition\r\nselected_profile=@ByteArray(Main)\r\n");
    dir.write("MO2/profiles/Main/modlist.txt", "# generated\r\n+High\r\n+Low\r\n");
    dir.write("MO2/profiles/Main/plugins.txt", "# generated\r\n*Low.esp\r\n*High.esp\r\n");
    dir.write("MO2/profiles/Other/modlist.txt", "+Low\r\n");
    dir.write("MO2/profiles/Other/plugins.txt", "*Low.esp\r\n");
    make_plugin(dir / "MO2/mods/Low/Low.esp");
    make_plugin(dir / "MO2/mods/High/High.esp");
    dir.write("MO2/mods/Low/meshes/a.nif", "low");
    dir.write("MO2/mods/Low/meshes/only_low.nif", "only low");
    dir.write("MO2/mods/High/meshes/a.nif", "high");
    test::write_bsa(dir / "MO2/mods/Low/Low.bsa", {{"meshes/arch.nif", "low archive"}});
    test::write_bsa(dir / "MO2/mods/High/High.bsa", {{"meshes/arch.nif", "high archive"}});
    // Named after no plugin: the game would not load it.
    dir.write("MO2/mods/Low/Orphan.bsa", "");
    dir.write("MO2/overwrite/meshes/over.nif", "overwrite");
}

} // namespace

TEST_CASE("a Data folder gives its plugins by file time and mounts archives under loose files",
          "[inputs]") {
    TempDir dir;
    make_data(dir);
    const auto data = dir / "Game/Data";

    const auto prepared = pack::prepare_inputs(pack::InputSpec{.data_dir = data});
    REQUIRE(prepared.has_value());

    CHECK(plugin_names(prepared->order) ==
          std::vector<std::string>{"Skyrim.esm", "Mod.esp", "Later.esp"});
    CHECK(prepared->mount_failures.empty());
    CHECK(prepared->unloaded_archives.empty());
    CHECK_FALSE(prepared->profile.has_value());

    REQUIRE(prepared->plan.has_value());
    REQUIRE(prepared->plan->archives.size() == 1);
    CHECK(prepared->plan->archives[0].filename() == "Skyrim - Meshes0.bsa");
    CHECK(prepared->plan->loose == std::vector<std::filesystem::path>{data});

    CHECK(read_text(prepared->set, "meshes/a.nif") == "archived");
    CHECK(read_text(prepared->set, "meshes/c.nif") == "loose");  // loose beats archived

    CHECK(prepared->input.kind == "data");
    CHECK(prepared->input.data == io::path_text(data));
    CHECK(prepared->input.plugin_list.empty());
    CHECK(prepared->input.mo2_instance.empty());
    CHECK(prepared->input.mods == 0);
}

TEST_CASE("a list file replaces the folder's order and is recorded", "[inputs]") {
    TempDir dir;
    make_data(dir);
    const auto list = dir.write("loadorder.txt", "Later.esp\r\nMod.esp\r\n");

    const auto prepared = pack::prepare_inputs(
        pack::InputSpec{.data_dir = dir / "Game/Data", .list_file = list});
    REQUIRE(prepared.has_value());
    // The list's order, not the file times'; the master is still hoisted.
    CHECK(plugin_names(prepared->order) ==
          std::vector<std::string>{"Skyrim.esm", "Later.esp", "Mod.esp"});
    CHECK(prepared->input.plugin_list == io::path_text(list));
    CHECK(prepared->input.kind == "data");

    CHECK_FALSE(pack::prepare_inputs(
        pack::InputSpec{.data_dir = dir / "Game/Data", .list_file = dir / "nowhere.txt"}));
}

TEST_CASE("Creation Club plugins load after the masters when a list omits them", "[inputs]") {
    TempDir dir;
    make_data(dir);
    make_plugin(dir / "Game/Data/ccBGSSSE001-Fish.esm", 0x1);
    dir.write("Game/Skyrim.ccc", "ccBGSSSE001-Fish.esm\r\n");
    const auto list = dir.write("plugins.txt", "*Mod.esp\r\n");

    const auto prepared = pack::prepare_inputs(
        pack::InputSpec{.data_dir = dir / "Game/Data", .list_file = list});
    REQUIRE(prepared.has_value());
    CHECK(plugin_names(prepared->order) ==
          std::vector<std::string>{"Skyrim.esm", "ccBGSSSE001-Fish.esm", "Mod.esp"});
    CHECK(prepared->order.problems().empty());
}

TEST_CASE("an MO2 profile's mods mount lowest priority first, plugins from their folders",
          "[inputs][mo2]") {
    TempDir dir;
    make_data(dir);
    make_instance(dir);
    const auto data = dir / "Game/Data";

    std::vector<std::size_t> done;
    const auto prepared = pack::prepare_inputs(
        pack::InputSpec{.data_dir = data, .mo2 = dir / "MO2"},
        [&](std::size_t finished, std::size_t total) {
            done.push_back(finished);
            CHECK(finished <= total);
        });
    REQUIRE(prepared.has_value());

    REQUIRE(prepared->profile.has_value());
    CHECK(prepared->profile->name == "Main");
    REQUIRE(prepared->profile->mods.size() == 2);
    CHECK(prepared->profile->mods[0].name == "Low");   // lowest priority first
    CHECK(prepared->profile->mods[1].name == "High");

    // The list is the profile's: its two plugins, in its order, found in the
    // mod folders; Data supplies the master.
    CHECK(plugin_names(prepared->order) ==
          std::vector<std::string>{"Skyrim.esm", "Low.esp", "High.esp"});
    CHECK(prepared->order.entries()[1].path == dir / "MO2/mods/Low/Low.esp");
    CHECK(prepared->order.entries()[2].path == dir / "MO2/mods/High/High.esp");

    // Loose folders: Data, Low, High, overwrite; the higher one wins a clash.
    REQUIRE(prepared->plan.has_value());
    CHECK(prepared->plan->loose ==
          std::vector<std::filesystem::path>{data, dir / "MO2/mods/Low", dir / "MO2/mods/High",
                                             dir / "MO2/overwrite"});
    CHECK(read_text(prepared->set, "meshes/a.nif") == "high");
    CHECK(read_text(prepared->set, "meshes/only_low.nif") == "only low");
    CHECK(read_text(prepared->set, "meshes/over.nif") == "overwrite");
    // Archives follow their plugins' load order: High.esp loads after Low.esp.
    CHECK(read_text(prepared->set, "meshes/arch.nif") == "high archive");
    // Mod archives sit below every loose file, Data's own included.
    CHECK(read_text(prepared->set, "meshes/c.nif") == "loose");

    REQUIRE(prepared->unloaded_archives.size() == 1);
    CHECK(prepared->unloaded_archives[0].filename() == "Orphan.bsa");
    CHECK(prepared->mount_failures.empty());

    // One call per mounted source, counting up.
    REQUIRE(done.size() == prepared->plan->archives.size() + prepared->plan->loose.size());
    CHECK(done.front() == 1);
    CHECK(done.back() == done.size());

    CHECK(prepared->input.kind == "mo2");
    CHECK(prepared->input.edition == "se");  // from the instance's gameName
    CHECK(prepared->input.data == io::path_text(data));
    CHECK(prepared->input.mo2_instance == io::path_text(dir / "MO2"));
    CHECK(prepared->input.mo2_profile == "Main");
    CHECK(prepared->input.mods == 2);
    CHECK(prepared->input.plugin_list == io::path_text(dir / "MO2/profiles/Main/plugins.txt"));
}

TEST_CASE("an MO2 profile can be named, and a bad one or a bad instance is an error",
          "[inputs][mo2]") {
    TempDir dir;
    make_data(dir);
    make_instance(dir);
    const auto data = dir / "Game/Data";

    const auto other = pack::prepare_inputs(
        pack::InputSpec{.data_dir = data, .mo2 = dir / "MO2", .mo2_profile = "Other"});
    REQUIRE(other.has_value());
    CHECK(other->input.mo2_profile == "Other");
    CHECK(other->input.mods == 1);
    CHECK(plugin_names(other->order) == std::vector<std::string>{"Skyrim.esm", "Low.esp"});
    CHECK(read_text(other->set, "meshes/arch.nif") == "low archive");  // High is not mounted

    const auto missing_profile = pack::prepare_inputs(
        pack::InputSpec{.data_dir = data, .mo2 = dir / "MO2", .mo2_profile = "Nope"});
    CHECK_FALSE(missing_profile.has_value());
    CHECK_FALSE(pack::prepare_inputs(pack::InputSpec{.data_dir = data, .mo2 = dir / "Game"}));
}

TEST_CASE("explicit sources mount in the order given and a bad one is reported, not fatal",
          "[inputs]") {
    TempDir dir;
    make_data(dir);
    dir.write("first/meshes/x.nif", "first");
    dir.write("second/meshes/x.nif", "second");
    dir.write("broken.bsa", "this is not an archive");

    bool progressed = false;
    const auto prepared = pack::prepare_inputs(
        pack::InputSpec{.data_dir = dir / "Game/Data",
                        .sources = {dir / "first", dir / "broken.bsa", dir / "second"}},
        [&](std::size_t, std::size_t) { progressed = true; });
    REQUIRE(prepared.has_value());

    CHECK_FALSE(prepared->plan.has_value());
    REQUIRE(prepared->mount_failures.size() == 1);
    CHECK(prepared->mount_failures[0].starts_with("broken.bsa: "));
    CHECK(read_text(prepared->set, "meshes/x.nif") == "second");  // the later mount wins
    CHECK_FALSE(prepared->set.resolve("meshes/a.nif").has_value());  // Data is not mounted
    CHECK_FALSE(progressed);

    // The order still comes from the Data folder.
    CHECK(plugin_names(prepared->order) ==
          std::vector<std::string>{"Skyrim.esm", "Mod.esp", "Later.esp"});
}

TEST_CASE("string_fetch reads from the set and answers nothing for a missing table",
          "[inputs]") {
    TempDir dir;
    dir.write("Data/strings/mod_english.strings", "table");
    archive::ArchiveSet set;
    REQUIRE(set.mount_loose(dir / "Data", 0).has_value());

    const auto fetch = pack::string_fetch(set);
    const auto found = fetch("strings/mod_english.strings");
    REQUIRE(found.has_value());
    CHECK(found->size() == 5);
    CHECK_FALSE(fetch("strings/mod_french.strings").has_value());
}
