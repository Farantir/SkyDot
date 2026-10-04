// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"
#include "front_end.hpp"

#include "bethconv/io/json_text.hpp"
#include "bethconv/pack/vpath_index.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/record/forms.hpp"
#include "skydot_formats/flags.hpp"
#include "skydot_formats/units.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace bethconv::cli {
namespace {

/// Game units per exterior cell side.
constexpr float k_cell_units = static_cast<float>(skydot::formats::k_cell_units);

/// The exterior region `grid` +- `radius` of worldspace `world_name`: its
/// cells, the persistent references positioned inside it, and terrain (from
/// the parent worldspace when the world uses its parent's land). With
/// `models`, print unique model and land texture paths (input for
/// `view --from`).
int cmd_cell_region(const bethconv::pack::WorldFile& world, const std::string& world_name,
                    const std::string& grid, int radius, bool models) {
    const auto worlds = world.worldspaces();
    const auto lower = [](std::string text) {
        std::ranges::transform(text, text.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const auto ws = std::ranges::find_if(
        worlds, [&](const auto& w) { return lower(w.editor_id) == lower(world_name); });
    if (ws == worlds.end()) {
        std::fprintf(stderr, "error: no worldspace %s\n", world_name.c_str());
        return 1;
    }
    int cx = 0;
    int cy = 0;
    if (!grid.empty()) {
        const char* end = grid.data() + grid.size();
        const auto x = std::from_chars(grid.data(), end, cx);
        const auto y = x.ec == std::errc{} && x.ptr != end && *x.ptr == ','
                           ? std::from_chars(x.ptr + 1, end, cy)
                           : std::from_chars_result{x.ptr, std::errc::invalid_argument};
        if (y.ec != std::errc{} || y.ptr != end) {
            std::fprintf(stderr, "error: --grid wants X,Y\n");
            return 1;
        }
    }
    const std::uint32_t land_world = ws->uses_parent_land() ? ws->parent : ws->id;

    std::vector<bethconv::pack::WorldRef> refs;
    std::set<std::uint32_t> land_textures;
    std::set<std::uint32_t> waters;
    if (ws->water != 0) {
        waters.insert(ws->water);
    }
    std::size_t cells = 0;
    std::size_t terrains = 0;
    const auto inside = [&](const bethconv::record::Vec3& p) {
        const int gx = static_cast<int>(std::floor(p.x / k_cell_units));
        const int gy = static_cast<int>(std::floor(p.y / k_cell_units));
        return std::abs(gx - cx) <= radius && std::abs(gy - cy) <= radius;
    };
    for (std::size_t i = 0; i < world.cell_count(); ++i) {
        const auto cell = world.cell_at(i);
        if (!cell || cell->interior() || !cell->grid) {
            continue;
        }
        if (cell->world == ws->id && cell->persistent) {
            std::ranges::copy_if(cell->refs, std::back_inserter(refs),
                                 [&](const auto& r) { return inside(r.position); });
            continue;
        }
        const auto [gx, gy] = *cell->grid;
        if (std::abs(gx - cx) > radius || std::abs(gy - cy) > radius) {
            continue;
        }
        if (cell->world == ws->id) {
            ++cells;
            refs.insert(refs.end(), cell->refs.begin(), cell->refs.end());
            if (cell->water != 0) {
                waters.insert(cell->water);
            }
        }
        if (cell->world == land_world && cell->terrain) {
            ++terrains;
            for (const auto& layer : cell->terrain->layers) {
                land_textures.insert(layer.texture);
            }
        }
    }

    if (models) {
        std::set<std::string> unique;
        for (const auto& ref : refs) {
            if (const auto base = world.base(ref.base); base && !base->model.empty()) {
                unique.insert(base->model);
            }
        }
        for (const auto id : land_textures) {
            if (const auto ltex = world.land_texture(id)) {
                for (const auto* path : {&ltex->diffuse, &ltex->normal}) {
                    if (!path->empty()) {
                        unique.insert(*path);
                    }
                }
            }
        }
        for (const auto id : waters) {
            if (const auto water = world.water(id)) {
                unique.insert(water->noise.begin(), water->noise.end());
            }
        }
        for (const auto& path : unique) {
            std::printf("%s\n", path.c_str());
        }
        return 0;
    }
    std::printf("0x%08X %s around (%d, %d) radius %d: %zu cells, %zu refs, %zu with terrain%s, "
                "%zu land textures, %zu water types\n",
                ws->id, ws->editor_id.c_str(), cx, cy, radius, cells, refs.size(), terrains,
                land_world != ws->id ? " (the parent's)" : "", land_textures.size(), waters.size());
    return 0;
}

void print_scripts(const std::vector<bethconv::record::Script>& scripts) {
    using bethconv::record::ScriptPropertyType;
    namespace wfb = bethconv::pack::wfb;
    for (const auto& script : scripts) {
        const bool removed = skydot::formats::has_flag(static_cast<wfb::ScriptStatus>(script.status),
                                                       wfb::ScriptStatus::removed);
        std::printf("    %s%s\n", script.name.c_str(), removed ? " (removed)" : "");
        for (const auto& p : script.properties) {
            std::string value;
            for (const auto& o : p.objects) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), o.alias >= 0 ? " 0x%08X:%d" : " 0x%08X",
                              o.form.value, o.alias);
                value += buffer;
            }
            for (const auto& text : p.strings) {
                value += " \"" + text + "\"";
            }
            for (const auto i : p.integers) {
                value += " " + std::to_string(i);
            }
            for (const auto f : p.floats) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), " %g", static_cast<double>(f));
                value += buffer;
            }
            std::printf("      %s =%s\n", p.name.c_str(), value.c_str());
        }
    }
}

/// The LOD meshes of a worldspace and its tree atlas, as virtual paths the
/// pack has (input for view --from).
int cmd_cell_lod(const std::filesystem::path& pack, const std::string& world_name) {
    auto index = bethconv::pack::VpathIndex::read(pack / "vpath.idx");
    if (!index) {
        std::fprintf(stderr, "error: %s\n", index.error().to_string().c_str());
        return 1;
    }
    std::string name = world_name;
    std::ranges::transform(name, name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string meshes = "meshes/terrain/" + name + "/";
    const std::string atlas = "textures/terrain/" + name + "/trees/" + name + "treelod.dds";
    std::size_t count = 0;
    for (const auto& entry : index->entries()) {
        const bool mesh = entry.vpath.starts_with(meshes) &&
                          (entry.vpath.ends_with(".btr") || entry.vpath.ends_with(".bto"));
        if (mesh || entry.vpath == atlas) {
            std::printf("%s\n", entry.vpath.c_str());
            ++count;
        }
    }
    if (count == 0) {
        std::fprintf(stderr, "error: the pack has no LOD for %s\n", world_name.c_str());
        return 1;
    }
    return 0;
}

/// Inspect cells in a pack's world.fb. With `--models`, print only the unique
/// model paths the cell's references use (input for `view --from`).
int cmd_cell(const std::filesystem::path& pack, const std::string& which, const std::string& filter,
             bool list, bool models, bool worlds, const std::string& world_name,
             const std::string& grid, int radius, bool lod, bool json) {
    using nlohmann::ordered_json;
    auto world = bethconv::pack::WorldFile::open(pack / "world.fb");
    if (!world) {
        std::fprintf(stderr, "error: %s\n", world.error().to_string().c_str());
        if (json) {
            bethconv::cli::emit(ordered_json{
                {"json_version", bethconv::cli::k_json_version},
                {"error", world.error().to_string()}});
        }
        return 1;
    }
    if (worlds && json) {
        auto spaces = ordered_json::array();
        for (const auto& w : world->worldspaces()) {
            spaces.push_back(ordered_json{
                {"id", w.id},
                {"editor_id", bethconv::io::json_text(w.editor_id)},
                {"parent", w.parent},
                {"uses_parent_land", w.uses_parent_land()},
                {"bounds", {w.bounds[0], w.bounds[1], w.bounds[2], w.bounds[3]}}});
        }
        bethconv::cli::emit(ordered_json{{"json_version", bethconv::cli::k_json_version},
                                         {"worlds", std::move(spaces)}});
        return 0;
    }
    if ((list || which.empty()) && world_name.empty() && json) {
        auto cells = ordered_json::array();
        for (std::size_t i = 0; i < world->cell_count(); ++i) {
            const auto cell = world->cell_at(i);
            if (!cell || cell->editor_id.empty()) {
                continue;
            }
            cells.push_back(ordered_json{{"id", cell->id},
                                         {"editor_id", bethconv::io::json_text(cell->editor_id)},
                                         {"interior", cell->interior()},
                                         {"refs", cell->refs.size()}});
        }
        bethconv::cli::emit(ordered_json{{"json_version", bethconv::cli::k_json_version},
                                         {"total", world->cell_count()},
                                         {"cells", std::move(cells)}});
        return 0;
    }
    if (worlds) {
        for (const auto& w : world->worldspaces()) {
            std::printf("0x%08X  %-28s parent 0x%08X%s  bounds (%.0f %.0f)-(%.0f %.0f)\n", w.id,
                        w.editor_id.c_str(), w.parent,
                        w.uses_parent_land() ? " (its land)" : "",
                        static_cast<double>(w.bounds[0]), static_cast<double>(w.bounds[1]),
                        static_cast<double>(w.bounds[2]), static_cast<double>(w.bounds[3]));
        }
        return 0;
    }
    if (lod) {
        if (world_name.empty()) {
            std::fprintf(stderr, "error: --lod needs --world\n");
            return 1;
        }
        return cmd_cell_lod(pack, world_name);
    }
    if (!world_name.empty()) {
        return cmd_cell_region(*world, world_name, grid, radius, models);
    }
    if (list || which.empty()) {
        std::size_t shown = 0;
        for (std::size_t i = 0; i < world->cell_count(); ++i) {
            const auto cell = world->cell_at(i);
            if (!cell || cell->editor_id.empty()) {
                continue;
            }
            std::string lower = cell->editor_id;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (!filter.empty() && lower.find(filter) == std::string::npos) {
                continue;
            }
            std::printf("0x%08X  %-9s %5zu refs  %s\n", cell->id,
                        cell->interior() ? "interior" : "exterior", cell->refs.size(),
                        cell->editor_id.c_str());
            ++shown;
        }
        std::printf("%zu of %zu cells\n", shown, world->cell_count());
        return 0;
    }

    std::optional<bethconv::pack::WorldCell> cell;
    if (which.starts_with("0x") || which.starts_with("0X")) {
        const auto id = parse_u32(which, 16);
        if (!id) {
            std::fprintf(stderr, "error: '%s' is not a cell FormID\n", which.c_str());
            return 2;
        }
        cell = world->cell(*id);
    } else {
        cell = world->cell_by_editor_id(which);
    }
    if (!cell) {
        std::fprintf(stderr, "error: no cell %s\n", which.c_str());
        return 1;
    }

    if (models) {
        std::set<std::string> unique;
        for (const auto& ref : cell->refs) {
            if (const auto base = world->base(ref.base); base && !base->model.empty()) {
                unique.insert(base->model);
            }
        }
        for (const auto& path : unique) {
            std::printf("%s\n", path.c_str());
        }
        return 0;
    }

    std::printf("0x%08X %s  %s  %zu refs, %zu doors\n", cell->id, cell->editor_id.c_str(),
                cell->interior() ? "interior" : "exterior", cell->refs.size(),
                cell->doors.size());
    if (cell->lighting) {
        std::printf("  lighting: ambient %08X directional %08X fog %.0f-%.0f\n",
                    cell->lighting->ambient, cell->lighting->directional,
                    static_cast<double>(cell->lighting->fog_near),
                    static_cast<double>(cell->lighting->fog_far));
    }
    std::size_t no_base = 0;
    for (const auto& ref : cell->refs) {
        const auto base = world->base(ref.base);
        if (!base) {
            ++no_base;
            continue;
        }
        std::printf("  0x%08X %s %-28s pos (%.0f %.0f %.0f) scale %.2f%s  %s\n", ref.id,
                    base->type.to_string().c_str(), base->editor_id.c_str(),
                    static_cast<double>(ref.position.x), static_cast<double>(ref.position.y),
                    static_cast<double>(ref.position.z), static_cast<double>(ref.scale),
                    skydot::formats::has_flag(ref.flags,
                                              bethconv::pack::wfb::RefFlags::initially_disabled)
                        ? " disabled"
                        : "",
                    base->light ? "(light)" : base->model.c_str());
    }
    if (no_base != 0) {
        std::printf("  %zu refs place a base with no model or light\n", no_base);
    }
    for (const auto& door : cell->doors) {
        std::printf("  door 0x%08X -> 0x%08X at (%.0f %.0f %.0f)\n", door.ref, door.destination,
                    static_cast<double>(door.position.x), static_cast<double>(door.position.y),
                    static_cast<double>(door.position.z));
    }
    for (const auto& lock : cell->locks) {
        std::printf("  lock 0x%08X level %u key 0x%08X\n", lock.ref, lock.level, lock.key);
    }
    for (const auto& link : cell->links) {
        std::printf("  link 0x%08X -> 0x%08X keyword 0x%08X\n", link.ref, link.target,
                    link.keyword);
    }
    for (const auto& parent : cell->activate_parents) {
        std::printf("  activate parent 0x%08X of 0x%08X delay %.2f\n", parent.parent, parent.ref,
                    static_cast<double>(parent.delay));
    }
    std::set<std::uint32_t> scripted_bases;
    for (const auto& ref : cell->refs) {
        if (const auto base = world->base(ref.base); base && !base->scripts.empty()) {
            scripted_bases.insert(base->id);
        }
    }
    for (const auto id : scripted_bases) {
        std::printf("  base 0x%08X %s:\n", id, world->base(id)->editor_id.c_str());
        print_scripts(world->base(id)->scripts);
    }
    for (const auto& ref : cell->scripts) {
        std::printf("  ref 0x%08X:\n", ref.ref);
        print_scripts(ref.scripts);
    }
    return 0;
}

struct CellArgs {
    std::filesystem::path pack;
    std::string which;
    std::string filter;
    bool list = false;
    bool models = false;
    bool worlds = false;
    std::string world;
    std::string grid;
    int radius = 0;
    bool lod = false;
    bool json = false;
};

} // namespace

void register_cell(CLI::App& app) {
    auto args = std::make_shared<CellArgs>();
    auto* cell = app.add_subcommand("cell", "Inspect cells in a pack's world.fb");
    cell->add_option("pack", args->pack, "The pack directory")
        ->required()
        ->check(CLI::ExistingDirectory);
    cell->add_option("cell", args->which, "Editor id or 0x FormID; omit to list cells");
    cell->add_option("--filter", args->filter,
                     "With --list: only editor ids containing this (lowercase)");
    cell->add_flag("--list", args->list, "List cells with an editor id");
    cell->add_flag("--models", args->models,
                   "Print unique model paths only (with --world, also land textures)");
    cell->add_flag("--worlds", args->worlds, "List worldspaces");
    cell->add_option("--world", args->world, "Worldspace editor id: select an exterior region");
    cell->add_option("--grid", args->grid, "With --world: centre cell as X,Y (default 0,0)");
    cell->add_option("--radius", args->radius, "With --world: cells around the centre (default 0)");
    cell->add_flag("--json", args->json, "With --worlds or --list: one JSON document on stdout");
    cell->add_flag("--lod", args->lod,
                   "With --world: print its LOD meshes and tree atlas (input for view --from)");
    cell->callback([args] {
        set_exit_status(cmd_cell(args->pack, args->which, args->filter, args->list, args->models,
                                 args->worlds, args->world, args->grid, args->radius, args->lod,
                                 args->json));
    });
}

} // namespace bethconv::cli
