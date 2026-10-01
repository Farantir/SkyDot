// SPDX-License-Identifier: GPL-3.0-or-later
//
// Install detection, Mod Organizer 2 instances and mount plans. Fixtures are
// folder trees shaped like the real thing (Steam libraries, a Wabbajack MO2
// instance), with placeholder files where only names matter.
#include "bethconv/install/game_install.hpp"
#include "bethconv/install/mo2.hpp"
#include "bethconv/install/mount_plan.hpp"
#include "bethconv/install/vdf.hpp"
#include "bethconv/record/load_order.hpp"

#include "../support/esm_builder.hpp"
#include "../support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

using namespace bethconv::install;
using bethconv::test::TempDir;

namespace {

/// A plugin consisting only of a TES4 header.
void make_plugin(const std::filesystem::path& path, std::uint32_t flags = 0) {
    bethconv::test::ByteWriter w;
    bethconv::test::write_tes4(w, flags, {});
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    for (const auto b : w.bytes()) {
        out.put(static_cast<char>(b));
    }
}

[[nodiscard]] std::vector<std::string> names(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::string> out;
    for (const auto& path : paths) {
        out.push_back(path.filename().string());
    }
    return out;
}

/// A Steam library at `library` with SE installed and its app manifest.
void make_steam_se(const TempDir& dir, std::string_view library, std::string_view build) {
    const std::string common = std::string(library) + "/steamapps/common/Skyrim Special Edition";
    dir.write(common + "/SkyrimSE.exe", "");
    make_plugin(dir / (common + "/Data/Skyrim.esm"));
    dir.write(std::string(library) + "/steamapps/appmanifest_489830.acf",
              "\"AppState\"\n{\n\t\"appid\"\t\t\"489830\"\n\t\"installdir\"\t\t\"Skyrim Special "
              "Edition\"\n\t\"buildid\"\t\t\"" +
                  std::string(build) + "\"\n}\n");
}

} // namespace

// ---- KeyValues -----------------------------------------------------------------

TEST_CASE("libraryfolders.vdf is read in both of Steam's layouts", "[install]") {
    const auto current = parse_vdf(R"("libraryfolders"
{
	"0"
	{
		"path"		"/home/u/.local/share/Steam"
		"apps"
		{
			"489830"		"16092533099"
		}
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"   // Windows paths double their backslashes
	}
}
)");
    REQUIRE(current);
    const auto* folders = current->find("LibraryFolders");  // case-insensitive
    REQUIRE(folders != nullptr);
    REQUIRE(folders->children.size() == 2);
    CHECK(folders->children[0].get("path") == "/home/u/.local/share/Steam");
    CHECK(folders->children[0].find("apps")->find("489830") != nullptr);
    CHECK(folders->children[1].get("path") == "D:\\SteamLibrary");

    // Before 2021: index -> path directly.
    const auto old = parse_vdf("\"LibraryFolders\" { \"TimeNextStatsReport\" \"1\" \"1\" \"E:\\\\Games\" }");
    REQUIRE(old);
    CHECK(old->find("libraryfolders")->get("1") == "E:\\Games");
}

TEST_CASE("broken KeyValues fail instead of guessing", "[install]") {
    CHECK_FALSE(parse_vdf("\"a\" { \"b\" \"c\""));       // no closing brace
    CHECK_FALSE(parse_vdf("\"a\" { \"b\" }"));           // key without value
    CHECK_FALSE(parse_vdf("\"a\" \"unterminated"));      // open string
    CHECK_FALSE(parse_vdf("}"));
    std::string deep;
    for (int i = 0; i < 100; ++i) {
        deep += "\"k\" { ";
    }
    CHECK_FALSE(parse_vdf(deep));
}

// ---- Steam detection -----------------------------------------------------------

TEST_CASE("Steam installs are found through every library, with build and plugins.txt",
          "[install]") {
    TempDir dir;
    // Root library has no Skyrim; the second library has SE; a third library
    // is listed but gone (an unplugged disk).
    const auto root = dir / "Steam";
    std::filesystem::create_directories(root / "steamapps");
    const auto library = (dir / "Games").string();
    dir.write("Steam/steamapps/libraryfolders.vdf",
              "\"libraryfolders\" {\n"
              " \"0\" { \"path\" \"" + root.generic_string() + "\" \"apps\" { \"570\" \"1\" } }\n"
              " \"1\" { \"path\" \"" + std::filesystem::path(library).generic_string() +
                  "\" \"apps\" { \"489830\" \"1\" } }\n"
              " \"2\" { \"path\" \"" + (dir / "gone").generic_string() + "\" }\n"
              "}\n");
    make_steam_se(dir, "Games", "24914197");
    // Proton names it with a capital P.
    dir.write("Games/steamapps/compatdata/489830/pfx/drive_c/users/steamuser/AppData/Local/"
              "Skyrim Special Edition/Plugins.txt",
              "*Unofficial Skyrim Special Edition Patch.esp\n");

    DetectOptions options;
    options.steam_roots.push_back(root);
    const auto installs = detect_installs(options);
    REQUIRE(installs.size() == 1);
    const auto& se = installs.front();
    CHECK(se.edition == Edition::se);
    CHECK(se.source == "steam");
    CHECK(se.app_id == "489830");
    CHECK(se.build_id == "24914197");
    CHECK(se.data.filename() == "Data");
    REQUIRE(se.plugins_txt);
    // Found despite the case; Windows reports the spelling that was asked for.
    auto name = se.plugins_txt->filename().string();
    std::ranges::transform(name, name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    CHECK(name == "plugins.txt");
    CHECK(std::filesystem::exists(*se.plugins_txt));
}

TEST_CASE("an app manifest without a game folder is not an install", "[install]") {
    TempDir dir;
    std::filesystem::create_directories(dir / "Steam/steamapps");
    dir.write("Steam/steamapps/appmanifest_489830.acf",
              "\"AppState\" { \"installdir\" \"Skyrim Special Edition\" }");
    DetectOptions options;
    options.steam_roots.push_back(dir / "Steam");
    CHECK(detect_installs(options).empty());
}

TEST_CASE("a picked folder is identified by its executable, as game or Data folder",
          "[install]") {
    TempDir dir;
    dir.write("VR/SkyrimVR.exe", "");
    make_plugin(dir / "VR/Data/Skyrim.esm");
    dir.write("LE/TESV.exe", "");
    make_plugin(dir / "LE/Data/Skyrim.esm");

    const DetectOptions none;
    const auto vr = inspect_folder(dir / "VR", none);
    REQUIRE(vr);
    CHECK(vr->edition == Edition::vr);
    CHECK(vr->data == dir / "VR/Data");

    const auto le = inspect_folder(dir / "LE/Data", none);
    REQUIRE(le);
    CHECK(le->edition == Edition::le);
    CHECK(le->root == dir / "LE");

    CHECK_FALSE(inspect_folder(dir.path(), none));
}

// ---- Mod Organizer 2 -----------------------------------------------------------

TEST_CASE("QSettings values decode as MO2 writes them", "[install][mo2]") {
    CHECK(decode_ini_value("@ByteArray(D:\\\\SteamLibrary\\\\steamapps\\\\common\\\\SkyrimVR)") ==
          "D:\\SteamLibrary\\steamapps\\common\\SkyrimVR");
    CHECK(decode_ini_value("@ByteArray(FUS RO DAH (Basic + Appearance + Gameplay))") ==
          "FUS RO DAH (Basic + Appearance + Gameplay)");
    CHECK(decode_ini_value(" Skyrim VR ") == "Skyrim VR");
    CHECK(decode_ini_value("\"quoted value\"") == "quoted value");
    CHECK(decode_ini_value("a\\x41b") == "aAb");
}

TEST_CASE("game names map to editions", "[install][mo2]") {
    CHECK(edition_from_game_name("Skyrim Special Edition") == Edition::se);
    CHECK(edition_from_game_name("Skyrim VR") == Edition::vr);
    CHECK(edition_from_game_name("Skyrim") == Edition::le);
    CHECK(edition_from_game_name("Fallout 4") == Edition::unknown);
}

namespace {

/// A portable instance shaped like a Wabbajack install: gamePath points to a
/// Windows drive, folders at their defaults, one profile.
void make_instance(const TempDir& dir) {
    dir.write("MO2/ModOrganizer.ini",
              "[General]\r\n"
              "gameName=Skyrim Special Edition\r\n"
              "selected_profile=@ByteArray(Main)\r\n"
              "gamePath=@ByteArray(D:\\\\Modlists\\\\List\\\\Stock Game)\r\n"
              "\r\n[Settings]\r\n"
              "download_directory=D:/Modlists/List/downloads\r\n"
              "mod_directory=D:/Modlists/List/mods\r\n");
    // Highest priority first, as MO2 writes it.
    dir.write("MO2/profiles/Main/modlist.txt",
              "# This file was automatically generated by Mod Organizer.\r\n"
              "+Patches\r\n"
              "-Late_separator\r\n"
              "-Disabled Mod\r\n"
              "+Textures\r\n"
              "+Gone Mod\r\n"
              "+Base Mod\r\n"
              "*DLC: Dawnguard\r\n");
    dir.write("MO2/profiles/Main/plugins.txt",
              "# This file was automatically generated by Mod Organizer.\r\n"
              "*Base.esp\r\n"
              "Inactive.esp\r\n"
              "*Patch.esp\r\n");
    dir.write("MO2/profiles/Other/modlist.txt", "+Base Mod\n");
    make_plugin(dir / "MO2/mods/Base Mod/Base.esp");
    dir.write("MO2/mods/Base Mod/Base.bsa", "");
    dir.write("MO2/mods/Base Mod/meshes/a.nif", "base");
    dir.write("MO2/mods/Textures/Base - Textures.bsa", "");
    dir.write("MO2/mods/Textures/Orphan.bsa", "");
    dir.write("MO2/mods/Textures/meshes/a.nif", "textures");
    make_plugin(dir / "MO2/mods/Patches/Patch.esp");
    make_plugin(dir / "MO2/mods/Patches/Inactive.esp");
    dir.write("MO2/mods/Patches/Patch.bsa", "");
    dir.write("MO2/mods/Disabled Mod/Base.bsa", "");
    std::filesystem::create_directories(dir / "MO2/overwrite");
    dir.write("MO2/Stock Game/SkyrimSE.exe", "");
    make_plugin(dir / "MO2/Stock Game/Data/Skyrim.esm");
    dir.write("MO2/Stock Game/Data/Skyrim - Meshes0.bsa", "");
}

} // namespace

TEST_CASE("an MO2 instance written on Windows is readable here", "[install][mo2]") {
    TempDir dir;
    make_instance(dir);
    const auto instance = read_mo2_instance(dir / "MO2");
    REQUIRE(instance);
    CHECK(instance->edition == Edition::se);
    CHECK(instance->selected_profile == "Main");
    CHECK(instance->profiles == std::vector<std::string>{"Main", "Other"});
    // D:/Modlists/List/mods does not exist here; the instance's mods/ does.
    CHECK(instance->mods_dir == dir / "MO2/mods");
    // gamePath is a Windows path; its last component exists in the instance.
    REQUIRE(instance->game_path);
    CHECK(*instance->game_path == dir / "MO2/Stock Game");

    CHECK_FALSE(read_mo2_instance(dir.path()));  // no ModOrganizer.ini
}

TEST_CASE("a profile's mods come lowest priority first, without separators", "[install][mo2]") {
    TempDir dir;
    make_instance(dir);
    const auto instance = read_mo2_instance(dir / "MO2");
    REQUIRE(instance);
    const auto profile = read_mo2_profile(*instance);
    REQUIRE(profile);
    CHECK(profile->name == "Main");
    std::vector<std::string> mods;
    for (const auto& mod : profile->mods) {
        mods.push_back(mod.name);
    }
    CHECK(mods == std::vector<std::string>{"Base Mod", "Textures", "Patches"});
    CHECK(profile->missing == std::vector<std::string>{"Gone Mod"});
    CHECK(profile->disabled == 1);
    CHECK(profile->separators == 1);
    CHECK(profile->unmanaged == 1);
    CHECK(profile->plugins.marks_active);
    CHECK(profile->plugins.plugins.size() == 3);

    CHECK_FALSE(read_mo2_profile(*instance, "Nope"));
}

TEST_CASE("older games' profiles take the order from loadorder.txt", "[install][mo2]") {
    TempDir dir;
    make_instance(dir);
    // No activation marks: plugins.txt lists the active ones only.
    dir.write("MO2/profiles/Other/plugins.txt", "Patch.esp\nBase.esp\n");
    dir.write("MO2/profiles/Other/loadorder.txt", "Base.esp\nInactive.esp\nPatch.esp\n");
    const auto instance = read_mo2_instance(dir / "MO2");
    REQUIRE(instance);
    const auto profile = read_mo2_profile(*instance, "Other");
    REQUIRE(profile);
    REQUIRE(profile->plugins.plugins.size() == 3);
    CHECK(profile->plugins.marks_active);
    CHECK(profile->plugins.plugins[0].name == "Base.esp");
    CHECK(profile->plugins.plugins[0].active);
    CHECK_FALSE(profile->plugins.plugins[1].active);
    CHECK(profile->plugins.plugins[2].active);
}

// ---- mount plans ---------------------------------------------------------------

TEST_CASE("a Data folder mounts its archives sorted, then its loose files", "[install]") {
    TempDir dir;
    dir.write("Data/b.bsa", "");
    dir.write("Data/A.BSA", "");
    dir.write("Data/sub/c.bsa", "");  // the game ignores subfolders
    dir.write("Data/readme.txt", "");
    const auto plan = plan_data_folder(dir / "Data");
    CHECK(names(plan.archives) == std::vector<std::string>{"A.BSA", "b.bsa"});
    CHECK(plan.loose == std::vector<std::filesystem::path>{dir / "Data"});
    CHECK_FALSE(plan.plugins);
}

TEST_CASE("an MO2 plan orders archives by plugin and loose folders by priority",
          "[install][mo2]") {
    TempDir dir;
    make_instance(dir);
    const auto instance = read_mo2_instance(dir / "MO2");
    REQUIRE(instance);
    const auto profile = read_mo2_profile(*instance);
    REQUIRE(profile);
    const auto data = dir / "MO2/Stock Game/Data";
    const auto plan = plan_mo2(data, *instance, *profile);

    // Data's archives first, then mod archives in their plugins' load order.
    // Orphan.bsa belongs to no plugin; the disabled mod's Base.bsa is not seen.
    CHECK(names(plan.archives) ==
          std::vector<std::string>{"Skyrim - Meshes0.bsa", "Base.bsa", "Base - Textures.bsa",
                                   "Patch.bsa"});
    CHECK(plan.archives[1].parent_path().filename() == "Base Mod");
    CHECK(names(plan.unloaded_archives) == std::vector<std::string>{"Orphan.bsa"});

    REQUIRE(plan.loose.size() == 5);
    CHECK(plan.loose.front() == data);
    CHECK(plan.loose[1].filename() == "Base Mod");
    CHECK(plan.loose[3].filename() == "Patches");
    CHECK(plan.loose.back() == dir / "MO2/overwrite");
    CHECK(plan.plugin_dirs == plan.loose);
    REQUIRE(plan.plugins);
}

TEST_CASE("the higher priority mod's loose file wins once mounted", "[install][mo2]") {
    TempDir dir;
    make_instance(dir);
    const auto instance = read_mo2_instance(dir / "MO2");
    const auto profile = read_mo2_profile(*instance);
    auto plan = plan_mo2(dir / "MO2/Stock Game/Data", *instance, *profile);
    plan.archives.clear();  // placeholders, not real archives

    bethconv::archive::ArchiveSet set;
    std::size_t calls = 0;
    const auto failures = mount(set, plan, [&](std::size_t done, std::size_t total) {
        ++calls;
        CHECK(done <= total);
    });
    CHECK(failures.empty());
    CHECK(calls == plan.loose.size());
    const auto bytes = set.read("meshes/a.nif");
    REQUIRE(bytes);
    CHECK(bytes->size() == std::string_view("textures").size());
}

TEST_CASE("plugins resolve from the last folder that has them", "[install][loadorder]") {
    TempDir dir;
    make_plugin(dir / "Data/Skyrim.esm", 0x1);
    make_plugin(dir / "Data/Mod.esp");
    // A list's cleaned master replaces the game's.
    make_plugin(dir / "mods/Cleaned/Skyrim.esm", 0x1);
    make_plugin(dir / "mods/Other/Mod2.esp");
    bethconv::record::PluginList list;
    list.plugins = {{.name = "Mod.esp"}, {.name = "Mod2.esp"}};
    const std::vector<std::filesystem::path> dirs{dir / "Data", dir / "mods/Cleaned",
                                                  dir / "mods/Other"};
    const auto order = bethconv::record::LoadOrder::build(dirs, list);
    REQUIRE(order.entries().size() == 3);
    CHECK(order.entries()[0].path == dir / "mods/Cleaned/Skyrim.esm");
    CHECK(order.entries()[1].path == dir / "Data/Mod.esp");
    CHECK(order.entries()[2].path == dir / "mods/Other/Mod2.esp");
    CHECK(order.problems().empty());
}

TEST_CASE("Creation Club plugins load after the masters even when plugins.txt omits them",
          "[install][loadorder]") {
    TempDir dir;
    make_plugin(dir / "Game/Data/Skyrim.esm", 0x1);
    make_plugin(dir / "Game/Data/ccBGSSSE001-Fish.esm", 0x1);
    make_plugin(dir / "Game/Data/ccBGSSSE037-Curios.esl", 0x201);
    make_plugin(dir / "Game/Data/Mod.esp");
    // As the game ships it: CRLF, names of content that is not installed.
    dir.write("Game/Skyrim.ccc",
              "ccBGSSSE001-Fish.esm\r\nccQDRSSE001-SurvivalMode.esl\r\nccBGSSSE037-Curios.esl\r\n");
    const auto ccc = creation_club_plugins(dir / "Game/Data");
    REQUIRE(ccc.size() == 3);
    CHECK(ccc[1] == "ccQDRSSE001-SurvivalMode.esl");

    bethconv::record::PluginList list;
    list.marks_active = true;
    // Listing one of them again is not a duplicate: the game decides where it goes.
    list.plugins = {{.name = "Mod.esp"}, {.name = "ccBGSSSE037-Curios.esl"}};
    bethconv::record::LoadOrderOptions options;
    options.always_loaded = ccc;
    const auto order = bethconv::record::LoadOrder::build(dir / "Game/Data", list, options);
    std::vector<std::string> loaded;
    for (const auto& entry : order.entries()) {
        loaded.push_back(entry.name);
    }
    CHECK(loaded == std::vector<std::string>{"Skyrim.esm", "ccBGSSSE001-Fish.esm",
                                             "ccBGSSSE037-Curios.esl", "Mod.esp"});
    CHECK(order.problems().empty());

    CHECK(creation_club_plugins(dir / "nowhere").empty());
}
