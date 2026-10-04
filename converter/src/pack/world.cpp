// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/world.hpp"

#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/byte_view.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_stream.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_actor.hpp"
#include "bethconv/record/forms_game.hpp"
#include "bethconv/record/forms_object.hpp"
#include "bethconv/record/forms_world.hpp"
#include "bethconv/record/types.hpp"

#include "bethconv/pack/world_generated.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace bethconv::pack {
namespace {

/// The entry of a vector sorted by id with this id, or null. By index: MSVC
/// warns about std::lower_bound over FlatBuffers' 32-bit iterators.
template <typename T>
const T* find_sorted(const flatbuffers::Vector<flatbuffers::Offset<T>>* list, std::uint32_t id) {
    if (list == nullptr) {
        return nullptr;
    }
    flatbuffers::uoffset_t lo = 0;
    flatbuffers::uoffset_t hi = list->size();
    while (lo < hi) {
        const flatbuffers::uoffset_t mid = lo + (hi - lo) / 2;
        if (list->Get(mid)->id() < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < list->size() && list->Get(lo)->id() == id ? list->Get(lo) : nullptr;
}

namespace wfb = bethconv::pack::wfb;
using io::FourCC;
using record::FormId;

struct CellEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::uint32_t world{};
    std::uint16_t flags{};
    std::optional<record::Cell::Grid> grid;
    float water_height{};
    std::optional<WorldCellLighting> lighting;
    std::uint32_t lighting_template{};
    std::uint32_t image_space{};
    bool persistent{};
    std::uint32_t water{};
};

/// LGTM: DATA has XCLL's layout up to the light fade distances (its
/// directional ambient block is unused); the directional ambient is DALC.
struct LightingTemplateEntry {
    WorldCellLighting lighting;
};

struct TerrainEntry {
    WorldTerrain terrain;
};

struct TextureSetEntry {
    std::string diffuse;
    std::string normal;
};

struct LandTextureEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::uint32_t texture_set{};
    std::uint8_t specular{};
    std::vector<std::uint32_t> grasses;
};

/// ADDN; see world.fbs `AddonNode`.
struct AddonEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::int32_t index{};
    std::string model;
};

/// GRAS; see world.fbs `Grass`.
struct GrassEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::string model;
    std::uint8_t density{};
    std::uint8_t min_slope{};
    std::uint8_t max_slope{};
    std::uint16_t units_from_water{};
    std::uint32_t water_type{};
    float position_range{};
    float height_range{};
    float color_range{};
    float wave_period{};
    std::uint8_t flags{};
};

struct BaseEntry {
    std::uint32_t id{};
    std::uint32_t type{};
    std::string editor_id;
    std::string model;
    std::optional<WorldLight> light;
    std::uint32_t flags{};
    std::vector<record::Script> scripts;
    std::uint32_t record_flags{};
    std::uint32_t directional_material{};
    float directional_max_angle{};
};

/// MATO; see world.fbs `MaterialObject`.
struct MaterialObjectEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::string model;
    std::array<float, 11> data{}; ///< DATA's first 44 bytes
    std::uint32_t flags{};         ///< DATA's last word; bit 0 single pass
};

/// Per-cell data about its references beyond placement.
struct CellExtras {
    std::vector<WorldRefScripts> scripts;
    std::vector<WorldLock> locks;
    std::vector<WorldLink> links;
    std::vector<WorldActivateParent> activate_parents;
    std::vector<WorldPrimitive> primitives;
    std::vector<WorldLightOverride> light_overrides;
};

/// Types whose VMAD is followed by fragment data; their scripts only run
/// through systems that do not exist yet.
bool has_fragments(FourCC type) {
    return type == FourCC{"QUST"} || type == FourCC{"INFO"} || type == FourCC{"PACK"} ||
           type == FourCC{"SCEN"} || type == FourCC{"PERK"};
}

/// MODL values are relative to `Data\meshes\`, though some plugins include the
/// prefix. Returns a normalized virtual path.
std::string model_vpath(std::string_view modl) {
    std::string path = archive::normalize_vpath(modl);
    if (path.empty() || path.starts_with("meshes/")) {
        return path;
    }
    return "meshes/" + path;
}

/// TXST paths are relative to `Data\textures\`, like MODL to meshes.
std::string texture_vpath(std::string_view path) {
    std::string out = archive::normalize_vpath(path);
    if (out.empty() || out.starts_with("textures/")) {
        return out;
    }
    return "textures/" + out;
}

/// VHGT: a float offset, 33 x 33 signed deltas, 3 bytes of padding.
std::optional<std::pair<float, std::vector<std::int8_t>>> decode_vhgt(
    std::span<const std::byte> raw) {
    constexpr std::size_t count = WorldTerrain::k_grid * WorldTerrain::k_grid;
    if (raw.size() < 4 + count) {
        return std::nullopt;
    }
    io::SpanReader r(raw, "VHGT");
    const float offset = r.get<float>().value_or(0.0F);
    std::vector<std::int8_t> deltas;
    deltas.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        deltas.push_back(r.get<std::int8_t>().value_or(0));
    }
    return std::pair{offset, std::move(deltas)};
}

/// XCLL, 92-byte layout (source: UESP, CELL record, XCLL). The 64-byte pre-1.70
/// form is not decoded.
std::optional<WorldCellLighting> decode_xcll(std::span<const std::byte> raw) {
    if (raw.size() < 92) {
        return std::nullopt;
    }
    io::SpanReader r(raw, "XCLL");
    WorldCellLighting out;
    const auto u32 = [&]() { return r.get<std::uint32_t>().value_or(0); };
    const auto i32 = [&]() { return r.get<std::int32_t>().value_or(0); };
    const auto f32 = [&]() { return r.get<float>().value_or(0.0F); };
    out.ambient = u32();
    out.directional = u32();
    out.fog_near_color = u32();
    out.fog_near = f32();
    out.fog_far = f32();
    out.directional_rotation_xy = i32();
    out.directional_rotation_z = i32();
    out.directional_fade = f32();
    (void)f32(); // fog clip distance
    out.fog_power = f32();
    for (auto& colour : out.directional_ambient) {
        colour = u32();
    }
    (void)r.skip(4 + 4); // specular, fresnel power
    out.fog_far_color = u32();
    out.fog_max = f32();
    out.light_fade_begin = f32();
    out.light_fade_end = f32();
    out.inherit = u32();
    return out;
}

/// XCLL inherit flags (UESP, CELL record): set bits take the value from the
/// lighting template. The directional ambient goes with the ambient colour.
constexpr std::uint32_t k_inherit_ambient = 0x1;
constexpr std::uint32_t k_inherit_directional = 0x2;
constexpr std::uint32_t k_inherit_fog_color = 0x4;
constexpr std::uint32_t k_inherit_fog_near = 0x8;
constexpr std::uint32_t k_inherit_fog_far = 0x10;
constexpr std::uint32_t k_inherit_rotation = 0x20;
constexpr std::uint32_t k_inherit_fade = 0x40;
constexpr std::uint32_t k_inherit_fog_power = 0x100;
constexpr std::uint32_t k_inherit_fog_max = 0x200;
constexpr std::uint32_t k_inherit_light_fade = 0x400;

class WorldSink final : public record::MergedRecordSink {
public:
    explicit WorldSink(const record::LoadOrder& order) : order_(order) {}

    void on_record(const record::MergedRecord& merged, const record::RecordContext& ctx,
                   io::SpanReader& data, const record::FormContext& form_ctx) override {
        if (merged.deleted) {
            return;
        }
        if (merged.type == FourCC{"CELL"}) {
            on_cell(merged, data, form_ctx);
        } else if (merged.type == FourCC{"REFR"}) {
            on_reference(merged, ctx, data, form_ctx);
        } else if (merged.type == FourCC{"LIGH"}) {
            on_light(merged, data, form_ctx);
        } else if (merged.type == FourCC{"LAND"}) {
            on_land(merged, data, form_ctx);
        } else if (merged.type == FourCC{"WRLD"}) {
            on_worldspace(merged, data, form_ctx);
        } else if (merged.type == FourCC{"LTEX"}) {
            on_land_texture(merged, data, form_ctx);
        } else if (merged.type == FourCC{"TXST"}) {
            on_texture_set(merged, data, form_ctx);
        } else if (merged.type == FourCC{"WATR"}) {
            on_water(merged, data);
        } else if (merged.type == FourCC{"CLMT"}) {
            on_climate(merged, data, form_ctx);
        } else if (merged.type == FourCC{"WTHR"}) {
            on_weather(merged, data, form_ctx);
        } else if (merged.type == FourCC{"IMGS"}) {
            on_image_space(merged, data, form_ctx);
        } else if (merged.type == FourCC{"LGTM"}) {
            on_lighting_template(merged, data);
        } else if (merged.type == FourCC{"MATO"}) {
            on_material_object(merged, data);
        } else if (merged.type == FourCC{"GRAS"}) {
            on_grass(merged, data);
        } else if (merged.type == FourCC{"ADDN"}) {
            on_addon_node(merged, data);
        } else if (merged.type == FourCC{"QUST"}) {
            on_quest(merged, data, form_ctx);
        } else if (merged.type == FourCC{"GLOB"}) {
            on_global(merged, data, form_ctx);
        } else if (merged.type == FourCC{"ACHR"}) {
            on_actor(merged, ctx, data, form_ctx);
        } else if (merged.type == FourCC{"NAVM"}) {
            on_navmesh(merged, data, form_ctx);
        } else if (merged.type == FourCC{"SPGD"}) {
            on_precipitation(merged, data, form_ctx);
        } else if (merged.type == FourCC{"REGN"}) {
            on_region(merged, data, form_ctx);
        } else if (merged.type == FourCC{"RACE"}) {
            on_race(merged, data, form_ctx);
        } else if (merged.type == FourCC{"ARMA"}) {
            on_armor_addon(merged, data, form_ctx);
        } else if (merged.type == FourCC{"OTFT"}) {
            on_outfit(merged, data, form_ctx);
        } else if (merged.type == FourCC{"LVLI"}) {
            on_leveled_item(merged, data, form_ctx);
        } else if (merged.type == FourCC{"PACK"}) {
            on_package(merged, data, form_ctx);
        } else if (merged.type == FourCC{"FLST"}) {
            on_form_list(merged, data, form_ctx);
        } else if (merged.type != FourCC{"INFO"}) {
            // These are bases too (placed armor, scripted NPCs); read twice.
            io::SpanReader copy = data;
            if (merged.type == FourCC{"NPC_"}) {
                on_npc(merged, copy, form_ctx);
            } else if (merged.type == FourCC{"ARMO"}) {
                on_armor(merged, copy, form_ctx);
            } else if (merged.type == FourCC{"LVLN"}) {
                on_leveled_npc(merged, copy, form_ctx);
            }
            on_other(merged, data);
        }
    }

    [[nodiscard]] WorldStats& stats() noexcept { return stats_; }
    [[nodiscard]] std::map<std::uint32_t, WorldNpc>& npcs() noexcept { return npcs_; }
    [[nodiscard]] std::map<std::uint32_t, WorldRace>& races() noexcept { return races_; }
    [[nodiscard]] std::map<std::uint32_t, WorldArmor>& armors() noexcept { return armors_; }
    [[nodiscard]] std::map<std::uint32_t, WorldArmorAddon>& armor_addons() noexcept {
        return armor_addons_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldOutfit>& outfits() noexcept { return outfits_; }
    [[nodiscard]] std::map<std::uint32_t, WorldLeveledList>& leveled_lists() noexcept {
        return leveled_lists_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldPackage>& packages() noexcept { return packages_; }
    [[nodiscard]] const std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& form_lists()
        const noexcept {
        return form_lists_;
    }
    [[nodiscard]] std::map<std::uint32_t, CellEntry>& cells() noexcept { return cells_; }
    [[nodiscard]] std::map<std::uint32_t, BaseEntry>& bases() noexcept { return bases_; }
    [[nodiscard]] std::unordered_map<std::uint32_t, std::vector<WorldRef>>& refs() noexcept {
        return refs_;
    }
    [[nodiscard]] std::unordered_map<std::uint32_t, std::vector<WorldDoor>>& doors() noexcept {
        return doors_;
    }
    [[nodiscard]] std::unordered_map<std::uint32_t, WorldTerrain>& terrains() noexcept {
        return terrains_;
    }
    [[nodiscard]] std::map<std::uint32_t, Worldspace>& worlds() noexcept { return worlds_; }
    [[nodiscard]] std::map<std::uint32_t, LandTextureEntry>& land_textures() noexcept {
        return land_textures_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldWater>& waters() noexcept { return waters_; }
    [[nodiscard]] std::map<std::uint32_t, WorldClimate>& climates() noexcept { return climates_; }
    [[nodiscard]] std::map<std::uint32_t, WorldWeather>& weathers() noexcept { return weathers_; }
    [[nodiscard]] std::map<std::uint32_t, WorldImageSpace>& image_spaces() noexcept {
        return image_spaces_;
    }
    [[nodiscard]] const std::map<std::uint32_t, GrassEntry>& grasses() const noexcept { return grasses_; }
    [[nodiscard]] const std::map<std::uint32_t, AddonEntry>& addons() const noexcept { return addons_; }
    [[nodiscard]] const std::map<std::uint32_t, MaterialObjectEntry>& material_objects() const noexcept {
        return material_objects_;
    }
    [[nodiscard]] const std::map<std::uint32_t, LightingTemplateEntry>& lighting_templates() const noexcept {
        return lighting_templates_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldPrecipitation>& precipitations() noexcept {
        return precipitations_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldRegion>& regions() noexcept { return regions_; }
    [[nodiscard]] std::unordered_map<std::uint32_t, TextureSetEntry>& texture_sets() noexcept {
        return texture_sets_;
    }
    [[nodiscard]] std::unordered_map<std::uint32_t, CellExtras>& extras() noexcept {
        return extras_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldQuest>& quests() noexcept { return quests_; }
    [[nodiscard]] std::map<std::uint32_t, WorldGlobal>& globals() noexcept { return globals_; }
    [[nodiscard]] std::vector<WorldActor>& actors() noexcept { return actors_; }
    [[nodiscard]] std::unordered_map<std::uint32_t, std::vector<WorldNavMesh>>& navmeshes() noexcept {
        return navmeshes_;
    }

private:
    /// A FormID from inside the winning record's payload, made global. 0 (and
    /// counted) if it cannot be resolved.
    std::uint32_t global(const record::MergedRecord& merged, FormId local, bool& failed) {
        if (local.is_null()) {
            return 0;
        }
        auto resolved = order_.resolve(merged.winner, local);
        if (!resolved) {
            failed = true;
            return 0;
        }
        return resolved->value;
    }

    /// Script data with object properties made global.
    std::vector<record::Script> global_scripts(const record::MergedRecord& merged,
                                               record::ScriptData data, bool& failed) {
        for (auto& script : data.scripts) {
            for (auto& property : script.properties) {
                for (auto& object : property.objects) {
                    object.form = FormId{global(merged, object.form, failed)};
                }
            }
        }
        return std::move(data.scripts);
    }

    void on_cell(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx) {
        auto cell = record::parse_cell(data, form_ctx);
        if (!cell) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        CellEntry entry{
            .id = merged.form.value,
            .editor_id = cell->editor_id,
            .world = cell->is_interior() ? 0 : merged.parent.value,
            .flags = cell->flags,
            .grid = cell->grid,
            .water_height = cell->water_height,
            .lighting = decode_xcll(cell->lighting),
            .lighting_template = global(merged, cell->lighting_template, failed),
            .image_space = global(merged, cell->image_space, failed),
            .persistent = record::has_flag(merged.flags, record::RecordFlag::persistent),
            .water = global(merged, cell->water, failed),
        };
        if (failed) {
            ++stats_.unresolved;
        }
        cells_[entry.id] = std::move(entry);
    }

    void on_reference(const record::MergedRecord& merged, const record::RecordContext& ctx,
                      io::SpanReader& data, const record::FormContext& form_ctx) {
        auto ref = record::parse_reference(ctx.header, data, form_ctx);
        if (!ref) {
            ++stats_.parse_errors;
            return;
        }
        if (merged.parent.is_null()) {
            ++stats_.orphan_refs;
            return;
        }
        bool failed = false;
        WorldRef out{
            .id = merged.form.value,
            .base = global(merged, ref->base, failed),
            .position = ref->position,
            .rotation = ref->rotation,
            .scale = ref->scale,
            .flags = 0,
            .enable_parent = 0,
        };
        if (ref->initially_disabled) {
            out.flags |= k_ref_initially_disabled;
        }
        if (ref->persistent) {
            out.flags |= k_ref_persistent;
        }
        if (ref->enable_parent) {
            out.enable_parent = global(merged, ref->enable_parent->parent, failed);
            if (ref->enable_parent->set_enable_state_opposite()) {
                out.flags |= k_ref_enable_opposite;
            }
        }
        if ((ref->activate_parent_flags & 0x01u) != 0) {
            out.flags |= k_ref_parent_activate_only;
        }
        auto& extras = extras_[merged.parent.value];
        if (!ref->scripts.empty()) {
            extras.scripts.push_back(WorldRefScripts{
                .ref = out.id,
                .scripts = global_scripts(merged, std::move(ref->scripts), failed),
            });
        }
        if (ref->has_radius || ref->light_data.size() >= 16) {
            WorldLightOverride light{.ref = out.id, .has_radius = ref->has_radius, .radius = ref->radius};
            if (ref->light_data.size() >= 16) {
                io::SpanReader r(ref->light_data, "REFR XLIG");
                light.has_light_data = true;
                light.fov = r.get<float>().value_or(0.0F);
                light.fade = r.get<float>().value_or(0.0F);
                light.end_distance_cap = r.get<float>().value_or(0.0F);
                light.shadow_depth_bias = r.get<float>().value_or(0.0F);
            }
            extras.light_overrides.push_back(light);
        }
        if (ref->lock) {
            extras.locks.push_back(WorldLock{
                .ref = out.id,
                .level = ref->lock->level,
                .flags = ref->lock->flags,
                .key = global(merged, ref->lock->key, failed),
            });
        }
        for (const auto& link : ref->linked_references) {
            extras.links.push_back(WorldLink{
                .ref = out.id,
                .keyword = global(merged, link.keyword, failed),
                .target = global(merged, link.target, failed),
            });
        }
        for (const auto& parent : ref->activate_parents) {
            extras.activate_parents.push_back(WorldActivateParent{
                .ref = out.id,
                .parent = global(merged, parent.ref, failed),
                .delay = parent.delay,
            });
        }
        if (ref->primitive) {
            extras.primitives.push_back(WorldPrimitive{
                .ref = out.id,
                .bounds = ref->primitive->bounds,
                .type = ref->primitive->type,
            });
        }
        if (ref->teleport) {
            doors_[merged.parent.value].push_back(WorldDoor{
                .ref = out.id,
                .destination = global(merged, ref->teleport->destination_door, failed),
                .position = ref->teleport->position,
                .rotation = ref->teleport->rotation,
            });
        }
        if (failed) {
            ++stats_.unresolved;
        }
        refs_[merged.parent.value].push_back(out);
    }

    void on_light(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx) {
        auto light = record::parse_light(data, form_ctx);
        if (!light) {
            ++stats_.parse_errors;
            return;
        }
        bases_[merged.form.value] = BaseEntry{
            .id = merged.form.value,
            .type = merged.type.value,
            .editor_id = light->editor_id,
            .model = light->model.path.empty() ? std::string{} : model_vpath(light->model.path),
            .light =
                WorldLight{
                    .radius = light->radius,
                    .color = light->colour,
                    .flags = light->light_flags,
                    .falloff_exponent = light->falloff_exponent,
                    .fov = light->fov,
                    .near_clip = light->near_clip,
                    .fade = light->fade,
                    .flicker_period = light->flicker_period,
                    .flicker_intensity = light->flicker_intensity_amplitude,
                    .flicker_movement = light->flicker_movement_amplitude,
                },
            .flags = 0,
            .scripts = {},
            .record_flags = merged.flags,
        };
        bool failed = false;
        bases_[merged.form.value].scripts = global_scripts(merged, std::move(light->scripts), failed);
        if (failed) {
            ++stats_.unresolved;
        }
    }

    void on_quest(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx) {
        auto q = record::parse_quest(data, form_ctx);
        if (!q) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        WorldQuest out{
            .id = merged.form.value,
            .editor_id = q->editor_id,
            .name = q->name.text,
            .flags = q->flags,
            .priority = q->priority,
            .type = q->type,
            .event = q->event.value,
            .scripts = global_scripts(merged, std::move(q->scripts), failed),
            .fragment_script = q->fragments.script,
            .fragments = {},
            .stages = {},
            .objectives = {},
            .aliases = {},
        };
        for (const auto& f : q->fragments.fragments) {
            out.fragments.push_back(WorldQuestFragment{
                .stage = f.stage, .log_entry = f.log_entry, .function = f.function});
            if (out.fragment_script.empty()) {
                out.fragment_script = f.script;
            }
        }
        std::ranges::stable_sort(out.fragments, [](const auto& a, const auto& b) {
            return std::pair{a.stage, a.log_entry} < std::pair{b.stage, b.log_entry};
        });
        for (const auto& stage : q->stages) {
            auto& s = out.stages.emplace_back();
            s.index = stage.index;
            s.flags = stage.flags;
            for (const auto& entry : stage.log) {
                s.log.push_back(WorldQuestLogEntry{
                    .flags = entry.flags,
                    .text = entry.text.text,
                    .conditions = static_cast<std::uint16_t>(entry.conditions.raw.size())});
            }
        }
        std::ranges::stable_sort(out.stages, {}, &WorldQuestStage::index);
        for (const auto& objective : q->objectives) {
            auto& o = out.objectives.emplace_back();
            o.index = objective.index;
            o.flags = objective.flags;
            o.text = objective.text.text;
            for (const auto& target : objective.targets) {
                o.targets.push_back(target.alias);
            }
        }
        for (const auto& alias : q->aliases) {
            WorldQuestAlias a{
                .id = alias.id,
                .name = alias.name,
                .location = alias.location,
                .flags = alias.flags,
                .forced = global(merged, alias.location ? alias.specific_location
                                                        : alias.forced_ref,
                                 failed),
                .unique_actor = global(merged, alias.unique_actor, failed),
                .external_quest = global(merged, alias.external_quest, failed),
                .external_alias = alias.external_alias,
                .created_object = global(merged, alias.created_object, failed),
                .create_at = alias.create_at,
                .conditions = static_cast<std::uint16_t>(alias.conditions.raw.size()),
                .display_name = global(merged, alias.display_name, failed),
                .scripts = {},
            };
            for (auto& attached : q->fragments.aliases) {
                if (attached.alias.alias >= 0 &&
                    static_cast<std::uint32_t>(attached.alias.alias) == alias.id) {
                    record::ScriptData data_for_alias;
                    data_for_alias.scripts = std::move(attached.scripts);
                    a.scripts = global_scripts(merged, std::move(data_for_alias), failed);
                }
            }
            out.aliases.push_back(std::move(a));
        }
        if (failed) {
            ++stats_.unresolved;
        }
        quests_[out.id] = std::move(out);
    }

    void on_global(const record::MergedRecord& merged, io::SpanReader& data,
                   const record::FormContext& form_ctx) {
        auto g = record::parse_global(data, form_ctx);
        if (!g) {
            ++stats_.parse_errors;
            return;
        }
        globals_[merged.form.value] = WorldGlobal{
            .id = merged.form.value, .editor_id = g->editor_id, .kind = g->kind, .value = g->value};
    }

    void on_actor(const record::MergedRecord& merged, const record::RecordContext& ctx,
                  io::SpanReader& data, const record::FormContext& form_ctx) {
        auto actor = record::parse_actor_reference(ctx.header, data, form_ctx);
        if (!actor) {
            ++stats_.parse_errors;
            return;
        }
        if (merged.parent.is_null()) {
            ++stats_.orphan_refs;
            return;
        }
        bool failed = false;
        WorldActor out{
            .ref = merged.form.value,
            .base = global(merged, actor->base, failed),
            .cell = merged.parent.value,
            .position = actor->position,
            .rotation = actor->rotation,
            .flags = 0,
        };
        if (actor->initially_disabled) {
            out.flags |= k_ref_initially_disabled;
        }
        if (actor->persistent) {
            out.flags |= k_ref_persistent;
        }
        // Packages walk to linked references (beds, work markers, patrols).
        for (const auto& link : actor->linked_references) {
            extras_[merged.parent.value].links.push_back(WorldLink{
                .ref = out.ref,
                .keyword = global(merged, link.keyword, failed),
                .target = global(merged, link.target, failed),
            });
        }
        if (failed) {
            ++stats_.unresolved;
        }
        actors_.push_back(out);
    }

    /// NAVM's parent is its CELL. Edge link and door FormIDs are made global.
    void on_navmesh(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx) {
        auto navm = record::parse_nav_mesh(data, form_ctx);
        if (!navm) {
            ++stats_.parse_errors;
            return;
        }
        auto geometry = record::decode_nav_mesh_geometry(navm->geometry);
        if (!geometry) {
            ++stats_.parse_errors;
            return;
        }
        if (merged.parent.is_null()) {
            ++stats_.orphan_navmeshes;
            return;
        }
        bool failed = false;
        WorldNavMesh out;
        out.id = merged.form.value;
        out.vertices = std::move(geometry->vertices);
        out.triangles.reserve(geometry->triangles.size());
        for (const auto& t : geometry->triangles) {
            out.triangles.push_back({.vertices = t.vertices, .edges = t.edges, .flags = t.flags,
                                     .cover = t.cover});
        }
        for (const auto& l : geometry->edge_links) {
            out.links.push_back({.type = l.type,
                                 .navmesh = global(merged, l.navmesh, failed),
                                 .triangle = l.triangle});
        }
        for (const auto& d : geometry->doors) {
            out.doors.push_back({.triangle = d.triangle, .door = global(merged, d.door, failed)});
        }
        if (failed) {
            ++stats_.unresolved;
        }
        navmeshes_[merged.parent.value].push_back(std::move(out));
    }

    // ---- what actors are built from -------------------------------------

    std::vector<std::uint32_t> global_all(const record::MergedRecord& merged,
                                          const std::vector<FormId>& forms, bool& failed) {
        std::vector<std::uint32_t> out;
        out.reserve(forms.size());
        for (const FormId f : forms) {
            out.push_back(global(merged, f, failed));
        }
        return out;
    }

    /// The precomputed FaceGen head: named by the plugin owning the form and
    /// the form's id within it.
    std::string face_model(const record::MergedRecord& merged) const {
        const auto& entries = order_.entries();
        if (merged.owner >= entries.size()) {
            return {};
        }
        const auto& owner = entries[merged.owner];
        std::array<char, 16> id{};
        std::snprintf(id.data(), id.size(), "%08x", merged.form.value & owner.object_mask());
        std::string plugin = owner.name;
        std::ranges::transform(plugin, plugin.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string hex(id.data());
        return "meshes/actors/character/facegendata/facegeom/" + plugin + "/" + hex + ".nif";
    }

    void on_npc(const record::MergedRecord& merged, io::SpanReader& data,
                const record::FormContext& form_ctx) {
        auto npc = record::parse_npc(data, form_ctx);
        if (!npc) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        WorldNpc out;
        out.id = merged.form.value;
        out.editor_id = npc->editor_id;
        out.name = npc->name.text;
        out.flags = npc->flags;
        out.level = npc->level;
        out.race = global(merged, npc->race, failed);
        out.template_form = global(merged, npc->npc_template, failed);
        out.template_flags = npc->template_flags;
        out.skin = global(merged, npc->worn_armor, failed);
        out.default_outfit = global(merged, npc->default_outfit, failed);
        out.sleeping_outfit = global(merged, npc->sleeping_outfit, failed);
        out.height = npc->height;
        out.weight = npc->weight;
        out.head_parts = global_all(merged, npc->head_parts, failed);
        out.packages = global_all(merged, npc->packages, failed);
        out.default_package_list = global(merged, npc->default_package_list, failed);
        for (const auto& f : npc->factions) {
            out.factions.emplace_back(global(merged, f.faction, failed), f.rank);
        }
        for (const auto& item : npc->items) {
            out.items.emplace_back(global(merged, item.item, failed), item.count);
        }
        out.face_model = face_model(merged);
        out.skin_tone = {npc->skin_red, npc->skin_green, npc->skin_blue};
        if (failed) {
            ++stats_.unresolved;
        }
        npcs_[out.id] = std::move(out);
    }

    void on_race(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx) {
        auto race = record::parse_race(data, form_ctx);
        if (!race) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        WorldRace out;
        out.id = merged.form.value;
        out.editor_id = race->editor_id;
        for (std::size_t sex = 0; sex < 2; ++sex) {
            const auto& s = race->sexes[sex];
            out.skeletons[sex] = s.skeleton.empty() ? std::string{} : model_vpath(s.skeleton);
            out.behaviours[sex] = s.behaviour.empty() ? std::string{} : model_vpath(s.behaviour);
            for (const auto& part : s.body_parts) {
                out.body_parts.push_back(WorldRace::BodyPart{
                    .female = sex == 1,
                    .index = part.index,
                    .model = part.model.empty() ? std::string{} : model_vpath(part.model)});
            }
            out.head_parts[sex] = global_all(merged, s.head_parts, failed);
        }
        out.skin = global(merged, race->skin, failed);
        out.heights = race->height;
        out.weights = race->weight;
        out.flags = race->flags;
        out.armor_race = global(merged, race->armor_race, failed);
        if (failed) {
            ++stats_.unresolved;
        }
        races_[out.id] = std::move(out);
    }

    void on_armor(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx) {
        auto armor = record::parse_armor(data, form_ctx);
        if (!armor) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        WorldArmor out{.id = merged.form.value,
                       .editor_id = armor->editor_id,
                       .slots = armor->body.slots,
                       .race = global(merged, armor->race, failed),
                       .addons = global_all(merged, armor->addons, failed)};
        if (failed) {
            ++stats_.unresolved;
        }
        armors_[out.id] = std::move(out);
    }

    void on_armor_addon(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx) {
        auto addon = record::parse_armor_addon(data, form_ctx);
        if (!addon) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        WorldArmorAddon out;
        out.id = merged.form.value;
        out.editor_id = addon->editor_id;
        out.slots = addon->body.slots;
        out.race = global(merged, addon->race, failed);
        out.additional_races = global_all(merged, addon->additional_races, failed);
        out.models = {addon->male_model.empty() ? std::string{} : model_vpath(addon->male_model.path),
                      addon->female_model.empty() ? std::string{} : model_vpath(addon->female_model.path)};
        out.priorities = {addon->male_priority, addon->female_priority};
        out.weight_sliders = {addon->male_weight_slider, addon->female_weight_slider};
        if (failed) {
            ++stats_.unresolved;
        }
        armor_addons_[out.id] = std::move(out);
    }

    void on_outfit(const record::MergedRecord& merged, io::SpanReader& data,
                   const record::FormContext& form_ctx) {
        auto outfit = record::parse_outfit(data, form_ctx);
        if (!outfit) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        outfits_[merged.form.value] =
            WorldOutfit{.id = merged.form.value, .items = global_all(merged, outfit->items, failed)};
        if (failed) {
            ++stats_.unresolved;
        }
    }

    template <typename Entries>
    void add_leveled(const record::MergedRecord& merged, std::uint8_t flags, std::uint8_t chance_none,
                     const Entries& entries) {
        bool failed = false;
        WorldLeveledList out{.id = merged.form.value,
                             .type = merged.type.value,
                             .flags = flags,
                             .chance_none = chance_none,
                             .entries = {}};
        for (const auto& e : entries) {
            out.entries.push_back(
                {.level = e.level, .count = e.count, .form = global(merged, e.reference, failed)});
        }
        if (failed) {
            ++stats_.unresolved;
        }
        leveled_lists_[out.id] = std::move(out);
    }

    /// FLST: kept to expand NPCs' default package lists.
    void on_form_list(const record::MergedRecord& merged, io::SpanReader& data,
                      const record::FormContext& form_ctx) {
        auto list = record::parse_form_list(data, form_ctx);
        if (!list) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        form_lists_[merged.form.value] = global_all(merged, list->forms, failed);
        if (failed) {
            ++stats_.unresolved;
        }
    }

    /// A condition with its FormID parameters made global.
    record::Condition global_condition(const record::MergedRecord& merged, record::Condition c,
                                       bool& failed) {
        c.value_global = FormId{global(merged, c.value_global, failed)};
        if (c.run_on == record::Condition::k_run_on_reference) {
            c.reference = FormId{global(merged, c.reference, failed)};
        }
        if (record::condition_param_is_form(c, 1)) {
            c.param1 = global(merged, FormId{c.param1}, failed);
        }
        if (record::condition_param_is_form(c, 2)) {
            c.param2 = global(merged, FormId{c.param2}, failed);
        }
        return c;
    }

    std::vector<record::Condition> global_conditions(const record::MergedRecord& merged,
                                                     std::vector<record::Condition> list,
                                                     bool& failed) {
        for (auto& c : list) {
            c = global_condition(merged, std::move(c), failed);
        }
        return list;
    }

    void on_package(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx) {
        auto pack = record::parse_package(data, form_ctx);
        if (!pack) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        WorldPackage out{
            .id = merged.form.value,
            .editor_id = pack->editor_id,
            .type = pack->type,
            .flags = pack->flags,
            .interrupt_override = pack->interrupt_override,
            .speed = pack->preferred_speed,
            .interrupt_flags = pack->interrupt_flags,
            .schedule = pack->schedule,
            .conditions = global_conditions(merged, std::move(pack->conditions), failed),
            .template_package = global(merged, pack->template_package, failed),
            .idle_flags = pack->idle_flags,
            .idle_timer = pack->idle_timer,
            .idles = global_all(merged, pack->idles, failed),
            .owner_quest = global(merged, pack->owner_quest, failed),
            .combat_style = global(merged, pack->combat_style, failed),
            .on_begin_idle = global(merged, pack->on_begin.idle, failed),
            .on_end_idle = global(merged, pack->on_end.idle, failed),
            .on_change_idle = global(merged, pack->on_change.idle, failed),
        };
        for (const auto& in : pack->inputs) {
            WorldPackage::Input w{.key = in.key, .type = in.type};
            // CNAM: one byte for Bool, a word otherwise; Float and ObjectList
            // (a radius) hold floats, Int an integer.
            io::SpanReader v{in.value, "PACK CNAM"};
            if (in.value.size() == 1) {
                w.number = static_cast<float>(v.get<std::uint8_t>().value_or(0));
            } else if (in.value.size() == 4) {
                w.number = in.type == "Int" ? static_cast<float>(v.get<std::int32_t>().value_or(0))
                                            : v.get<float>().value_or(0.0F);
            }
            if (in.location) {
                w.location = *in.location;
                const auto t = w.location.type;
                if (t == 0 || t == 1 || t == 4 || t == 6) {
                    w.location.value = global(merged, FormId{w.location.value}, failed);
                }
            }
            if (in.target) {
                w.target = *in.target;
                const auto t = w.target.type;
                if (t == 0 || t == 1 || t == 3) {
                    w.target.value = global(merged, FormId{w.target.value}, failed);
                }
            }
            for (const auto& named : pack->public_inputs) {
                if (named.key == in.key) {
                    w.name = named.name;
                }
            }
            out.inputs.push_back(std::move(w));
        }
        for (auto& b : pack->branches) {
            WorldPackage::Branch w{
                .type = b.type,
                .conditions = global_conditions(merged, std::move(b.conditions), failed),
                .children = b.branch_count,
                .flags = b.root_flags,
                .procedure = b.procedure,
                .success_completes = b.success_completes,
                .inputs = b.input_keys,
            };
            if (!b.flag_overrides.empty()) {
                const auto& o = b.flag_overrides.front();
                w.set_flags = o.set_flags;
                w.clear_flags = o.clear_flags;
                w.speed = static_cast<std::int8_t>(o.speed);
            }
            out.branches.push_back(std::move(w));
        }
        if (failed) {
            ++stats_.unresolved;
        }
        packages_[out.id] = std::move(out);
    }

    void on_leveled_item(const record::MergedRecord& merged, io::SpanReader& data,
                         const record::FormContext& form_ctx) {
        auto list = record::parse_leveled_item(data, form_ctx);
        if (!list) {
            ++stats_.parse_errors;
            return;
        }
        add_leveled(merged, list->flags, list->chance_none, list->entries);
    }

    void on_leveled_npc(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx) {
        auto list = record::parse_leveled_npc(data, form_ctx);
        if (!list) {
            ++stats_.parse_errors;
            return;
        }
        add_leveled(merged, list->flags, list->chance_none, list->entries);
    }

    /// Any other type: keep it as a base if it has a model or scripts. ARMO's
    /// MODL is an armature FormID, not a path; its world model is MOD2 (male)
    /// or MOD4 (female). Source: UESP, ARMO record.
    void on_other(const record::MergedRecord& merged, io::SpanReader& data) {
        const bool armor = merged.type == FourCC{"ARMO"};
        const bool door = merged.type == FourCC{"DOOR"};
        const bool scripted = !has_fragments(merged.type);
        std::string editor_id;
        std::string modl;
        std::string mod2;
        std::string mod4;
        std::uint32_t flags = 0;
        record::ScriptData scripts;
        const bool stat = merged.type == FourCC{"STAT"};
        float max_angle = 0.0F;
        std::uint32_t material = 0;
        const auto walked = record::for_each_field(
            data, [&](const record::FieldHeader& field, io::SpanReader& body) {
                if (field.type == FourCC{"DNAM"} && stat && body.remaining() >= 8) {
                    max_angle = body.get<float>().value_or(0.0F);
                    material = body.get<std::uint32_t>().value_or(0);
                } else if (field.type == FourCC{"EDID"} && editor_id.empty()) {
                    editor_id = std::string(body.zstring().value_or(""));
                } else if (field.type == FourCC{"MODL"} && modl.empty() && !armor) {
                    modl = std::string(body.zstring().value_or(""));
                } else if (field.type == FourCC{"MOD2"} && mod2.empty()) {
                    mod2 = std::string(body.zstring().value_or(""));
                } else if (field.type == FourCC{"MOD4"} && mod4.empty() && armor) {
                    mod4 = std::string(body.zstring().value_or(""));
                } else if (field.type == FourCC{"FNAM"} && door) {
                    flags = body.get<std::uint8_t>().value_or(0);
                } else if (field.type == FourCC{"VMAD"} && scripted) {
                    if (auto read = record::read_script_data(body)) {
                        scripts = std::move(*read);
                    } else {
                        ++stats_.script_errors;
                    }
                }
            });
        if (!walked) {
            ++stats_.parse_errors;
            return;
        }
        const std::string& path = !modl.empty() ? modl : !mod2.empty() ? mod2 : mod4;
        if (path.empty() && scripts.empty()) {
            return;
        }
        bool failed = false;
        bases_[merged.form.value] = BaseEntry{
            .id = merged.form.value,
            .type = merged.type.value,
            .editor_id = std::move(editor_id),
            .model = path.empty() ? std::string{} : model_vpath(path),
            .light = std::nullopt,
            .flags = flags,
            .scripts = global_scripts(merged, std::move(scripts), failed),
            .record_flags = merged.flags,
            .directional_material = material != 0 ? global(merged, record::FormId{material}, failed) : 0,
            .directional_max_angle = max_angle,
        };
        if (failed) {
            ++stats_.unresolved;
        }
    }

    void on_material_object(const record::MergedRecord& merged, io::SpanReader& data) {
        MaterialObjectEntry out;
        out.id = merged.form.value;
        const auto walked = record::for_each_field(
            data, [&](const record::FieldHeader& field, io::SpanReader& body) {
                if (field.type == FourCC{"EDID"}) {
                    out.editor_id = std::string(body.zstring().value_or(""));
                } else if (field.type == FourCC{"MODL"}) {
                    out.model = model_vpath(std::string(body.zstring().value_or("")));
                } else if (field.type == FourCC{"DATA"}) {
                    for (auto& f : out.data) {
                        f = body.remaining() >= 4 ? body.get<float>().value_or(0.0F) : 0.0F;
                    }
                    out.flags = body.remaining() >= 4 ? body.get<std::uint32_t>().value_or(0) : 0;
                }
            });
        if (!walked) {
            ++stats_.parse_errors;
            return;
        }
        material_objects_[out.id] = std::move(out);
    }

    /// LAND's parent is its CELL. Texture FormIDs are made global; an
    /// unresolvable one falls back to the default texture (0).
    void on_land(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx) {
        auto land = record::parse_landscape(data, form_ctx);
        if (!land) {
            ++stats_.parse_errors;
            return;
        }
        auto heights = decode_vhgt(land->heights);
        if (!heights || merged.parent.is_null()) {
            return;
        }
        bool failed = false;
        WorldTerrain terrain;
        terrain.height_offset = heights->first;
        terrain.height_deltas = std::move(heights->second);
        if (land->vertex_colours.size() == record::Landscape::k_vnml_size) {
            terrain.colours.reserve(land->vertex_colours.size());
            for (const std::byte b : land->vertex_colours) {
                terrain.colours.push_back(static_cast<std::uint8_t>(b));
            }
        }
        for (const auto& base : land->base_layers) {
            terrain.layers.push_back(WorldTerrainLayer{
                .texture = global(merged, base.texture, failed),
                .quadrant = base.quadrant,
                .layer = -1,
                .points = {},
                .opacity = {},
            });
        }
        for (const auto& extra : land->additional_layers) {
            WorldTerrainLayer layer{
                .texture = global(merged, extra.texture, failed),
                .quadrant = extra.quadrant,
                .layer = extra.layer,
                .points = {},
                .opacity = {},
            };
            // VTXT, 8 bytes a point: vertex index, an unknown word, opacity.
            io::SpanReader r(extra.alpha_map, "VTXT");
            while (r.remaining() >= record::Landscape::k_alpha_point_size) {
                const auto point = r.get<std::uint16_t>().value_or(0);
                (void)r.get<std::uint16_t>();
                const float opacity = std::clamp(r.get<float>().value_or(0.0F), 0.0F, 1.0F);
                layer.points.push_back(point);
                layer.opacity.push_back(static_cast<std::uint8_t>(opacity * 255.0F + 0.5F));
            }
            terrain.layers.push_back(std::move(layer));
        }
        std::ranges::stable_sort(terrain.layers, [](const auto& a, const auto& b) {
            return std::pair{a.quadrant, a.layer} < std::pair{b.quadrant, b.layer};
        });
        if (failed) {
            ++stats_.unresolved;
        }
        terrains_[merged.parent.value] = std::move(terrain);
    }

    void on_worldspace(const record::MergedRecord& merged, io::SpanReader& data,
                       const record::FormContext& form_ctx) {
        auto w = record::parse_worldspace(data, form_ctx);
        if (!w) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        Worldspace out{
            .id = merged.form.value,
            .editor_id = w->editor_id,
            .parent = global(merged, w->parent, failed),
            .parent_flags = w->parent_flags,
            .flags = w->flags,
            .defaults = std::nullopt,
            .water = global(merged, w->water, failed),
            .climate = global(merged, w->climate, failed),
            .bounds = {w->min_x, w->min_y, w->max_x, w->max_y},
        };
        if (w->default_land_height && w->default_water_height) {
            out.defaults = std::array{*w->default_land_height, *w->default_water_height};
        }
        if (failed) {
            ++stats_.unresolved;
        }
        worlds_[out.id] = std::move(out);
    }

    void on_land_texture(const record::MergedRecord& merged, io::SpanReader& data,
                         const record::FormContext& form_ctx) {
        auto ltex = record::parse_land_texture(data, form_ctx);
        if (!ltex) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        std::vector<std::uint32_t> grasses;
        for (const auto grass : ltex->grasses) {
            grasses.push_back(global(merged, grass, failed));
        }
        land_textures_[merged.form.value] = LandTextureEntry{
            .id = merged.form.value,
            .editor_id = ltex->editor_id,
            .texture_set = global(merged, ltex->texture_set, failed),
            .specular = ltex->specular,
            .grasses = std::move(grasses),
        };
        if (failed) {
            ++stats_.unresolved;
        }
    }

    void on_addon_node(const record::MergedRecord& merged, io::SpanReader& data) {
        AddonEntry out;
        out.id = merged.form.value;
        bool has_index = false;
        const auto walked = record::for_each_field(
            data, [&](const record::FieldHeader& field, io::SpanReader& body) {
                if (field.type == FourCC{"EDID"}) {
                    out.editor_id = std::string(body.zstring().value_or(""));
                } else if (field.type == FourCC{"MODL"}) {
                    out.model = model_vpath(std::string(body.zstring().value_or("")));
                } else if (field.type == FourCC{"DATA"} && body.remaining() >= 4) {
                    out.index = body.get<std::int32_t>().value_or(0);
                    has_index = true;
                }
            });
        if (!walked || !has_index || out.model.empty()) {
            ++stats_.parse_errors;
            return;
        }
        addons_[out.id] = std::move(out);
    }

    void on_grass(const record::MergedRecord& merged, io::SpanReader& data) {
        GrassEntry out;
        out.id = merged.form.value;
        const auto walked = record::for_each_field(
            data, [&](const record::FieldHeader& field, io::SpanReader& body) {
                if (field.type == FourCC{"EDID"}) {
                    out.editor_id = std::string(body.zstring().value_or(""));
                } else if (field.type == FourCC{"MODL"}) {
                    out.model = model_vpath(std::string(body.zstring().value_or("")));
                } else if (field.type == FourCC{"DATA"} && body.remaining() >= 29) {
                    out.density = body.get<std::uint8_t>().value_or(0);
                    out.min_slope = body.get<std::uint8_t>().value_or(0);
                    out.max_slope = body.get<std::uint8_t>().value_or(90);
                    (void)body.skip(1);
                    out.units_from_water = body.get<std::uint16_t>().value_or(0);
                    (void)body.skip(2);
                    out.water_type = body.get<std::uint32_t>().value_or(0);
                    out.position_range = body.get<float>().value_or(0.0F);
                    out.height_range = body.get<float>().value_or(0.0F);
                    out.color_range = body.get<float>().value_or(0.0F);
                    out.wave_period = body.get<float>().value_or(0.0F);
                    out.flags = body.get<std::uint8_t>().value_or(0);
                }
            });
        if (!walked || out.model.empty()) {
            ++stats_.parse_errors;
            return;
        }
        grasses_[out.id] = std::move(out);
    }

    void on_texture_set(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx) {
        auto txst = record::parse_texture_set(data, form_ctx);
        if (!txst) {
            ++stats_.parse_errors;
            return;
        }
        texture_sets_[merged.form.value] = TextureSetEntry{
            .diffuse = texture_vpath(txst->textures[0]),
            .normal = texture_vpath(txst->textures[1]),
        };
    }

    /// WATR is not one of the record layer's types; only what rendering needs
    /// is read here. DNAM offsets: UESP's field order, checked against
    /// Skyrim.esm (228 bytes, 232 in some SE records).
    void on_water(const record::MergedRecord& merged, io::SpanReader& data) {
        WorldWater out;
        out.id = merged.form.value;
        const auto walked = record::for_each_field(
            data, [&](const record::FieldHeader& field, io::SpanReader& body) {
                if (field.type == FourCC{"EDID"}) {
                    out.editor_id = std::string(body.zstring().value_or(""));
                } else if (field.type == FourCC{"ANAM"}) {
                    out.opacity = body.get<std::uint8_t>().value_or(0);
                } else if (field.type == FourCC{"FNAM"}) {
                    out.flags = body.get<std::uint8_t>().value_or(0);
                } else if (field.type == FourCC{"NNAM"} || field.type == FourCC{"NAM2"} ||
                           field.type == FourCC{"NAM3"} || field.type == FourCC{"NAM4"}) {
                    std::string path = archive::normalize_vpath(body.zstring().value_or(""));
                    if (path.starts_with("data/")) {
                        path.erase(0, 5);
                    }
                    out.noise.push_back(texture_vpath(path));
                } else if (field.type == FourCC{"DNAM"} && body.remaining() >= 228) {
                    const auto f32 = [&](std::size_t offset) {
                        io::SpanReader r = body;
                        (void)r.skip(offset);
                        return r.get<float>().value_or(0.0F);
                    };
                    const auto u32 = [&](std::size_t offset) {
                        io::SpanReader r = body;
                        (void)r.skip(offset);
                        return r.get<std::uint32_t>().value_or(0) & 0x00FF'FFFFu;
                    };
                    out.sun_specular_power = f32(16);
                    out.reflectivity = f32(20);
                    out.fresnel = f32(24);
                    out.fog_near = f32(32);
                    out.fog_far = f32(36);
                    out.shallow_color = u32(40);
                    out.deep_color = u32(44);
                    out.reflection_color = u32(48);
                    for (std::size_t i = 0; i < 3; ++i) {
                        out.layers[i] = WorldWater::Layer{
                            .wind_direction = f32(100 + 4 * i),
                            .wind_speed = f32(112 + 4 * i),
                            .uv_scale = f32(172 + 4 * i),
                            .amplitude = f32(184 + 4 * i),
                        };
                    }
                    out.refraction_magnitude = f32(152);
                    out.specular_power = f32(156);
                    out.reflection_magnitude = f32(196);
                }
            });
        if (!walked) {
            ++stats_.parse_errors;
            return;
        }
        waters_[out.id] = std::move(out);
    }

    void on_climate(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx) {
        auto climate = record::parse_climate(data, form_ctx);
        if (!climate) {
            ++stats_.parse_errors;
            return;
        }
        bool failed = false;
        WorldClimate out;
        out.id = merged.form.value;
        out.editor_id = climate->editor_id;
        for (const auto& entry : climate->weathers) {
            out.weathers.emplace_back(global(merged, entry.weather, failed), entry.chance);
        }
        const auto hours = [](std::uint8_t steps) { return static_cast<float>(steps) / 6.0F; };
        out.sun = {hours(climate->sunrise_begin), hours(climate->sunrise_end),
                   hours(climate->sunset_begin), hours(climate->sunset_end)};
        out.sun_texture = texture_vpath(climate->sun_texture);
        out.sun_glare_texture = texture_vpath(climate->sun_glare_texture);
        out.sky = model_vpath(climate->model.path);
        out.volatility = climate->volatility;
        // TNAM's last byte: phase length in days, bit 6 Masser, bit 7 Secunda
        // (UESP, CLMT record).
        const auto moons = climate->moons_and_phase_length;
        out.moons = static_cast<std::uint8_t>(((moons >> 6) & 1U) | (((moons >> 7) & 1U) << 1));
        out.phase_length = static_cast<std::uint8_t>(moons & 0x3FU);
        if (failed) {
            ++stats_.unresolved;
        }
        climates_[out.id] = std::move(out);
    }

    /// NAM0 is 17 x 4 RGBA colours (272 bytes); FNAM eight floats; DALC 32
    /// bytes per time of day, the 24-byte form padded with black.
    void on_weather(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx) {
        auto weather = record::parse_weather(data, form_ctx);
        if (!weather) {
            ++stats_.parse_errors;
            return;
        }
        WorldWeather out;
        out.id = merged.form.value;
        out.editor_id = weather->editor_id;
        const auto words = [](std::span<const std::byte> raw, std::size_t count, auto& into) {
            io::SpanReader r(raw, "WTHR");
            for (std::size_t i = 0; i < count; ++i) {
                using T = typename std::remove_reference_t<decltype(into)>::value_type;
                into.push_back(r.get<T>().value_or(T{}));
            }
        };
        if (weather->weather_colours.size() >= 272) {
            words(weather->weather_colours, 68, out.colors);
        }
        if (weather->fog_distance.size() >= 32) {
            words(weather->fog_distance, 8, out.fog);
        }
        for (const auto& block : weather->directional_ambient) {
            if (out.directional_ambient.size() >= 28) {
                break;
            }
            std::vector<std::uint32_t> colours;
            words(block, std::min<std::size_t>(block.size() / 4, 7), colours);
            colours.resize(7, 0);
            out.directional_ambient.insert(out.directional_ambient.end(), colours.begin(),
                                           colours.end());
        }
        read_clouds(*weather, out);
        // DATA, 19 bytes (xEdit's TES5 layout; flags checked against the
        // rain and snow weathers): fractions stored as 0..255.
        if (weather->data.size() >= record::Weather::k_data_size) {
            io::SpanReader r(weather->data, "WTHR DATA");
            std::array<std::uint8_t, record::Weather::k_data_size> d{};
            for (auto& b : d) {
                b = r.get<std::uint8_t>().value_or(0);
            }
            const auto unit = [](std::uint8_t v) { return static_cast<float>(v) / 255.0F; };
            out.wind_speed = unit(d[0]);
            out.transition_delta = unit(d[3]);
            out.sun_glare = unit(d[4]);
            out.sun_damage = unit(d[5]);
            out.precipitation_begin = unit(d[6]);
            out.precipitation_end = unit(d[7]);
            out.thunder_begin = unit(d[8]);
            out.thunder_end = unit(d[9]);
            out.thunder_frequency = unit(d[10]);
            out.classification = d[11];
            out.lightning_color = static_cast<std::uint32_t>(d[12]) |
                                  (static_cast<std::uint32_t>(d[13]) << 8) |
                                  (static_cast<std::uint32_t>(d[14]) << 16);
            out.wind_direction = static_cast<float>(d[17]) * 360.0F / 256.0F;
            out.wind_direction_range = static_cast<float>(d[18]) * 180.0F / 256.0F;
        }
        bool failed = false;
        out.precipitation = global(merged, weather->precipitation, failed);
        out.aurora = model_vpath(weather->model.path);
        for (const auto image_space : weather->image_spaces) {
            out.image_spaces.push_back(global(merged, image_space, failed));
        }
        if (failed) {
            ++stats_.unresolved;
        }
        weathers_[out.id] = std::move(out);
    }

    /// IMGS: HNAM, CNAM and TNAM as floats (see world.fbs `ImageSpace`).
    void on_image_space(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx) {
        auto image_space = record::parse_image_space(data, form_ctx);
        if (!image_space) {
            ++stats_.parse_errors;
            return;
        }
        WorldImageSpace out;
        out.id = merged.form.value;
        out.editor_id = image_space->editor_id;
        const auto floats = [](std::span<const std::byte> raw, std::size_t count) {
            std::vector<float> values;
            io::SpanReader r(raw, "IMGS");
            for (std::size_t i = 0; i < count && r.remaining() >= 4; ++i) {
                values.push_back(r.get<float>().value_or(0.0F));
            }
            return values.size() == count ? values : std::vector<float>{};
        };
        out.hdr = floats(image_space->hdr, 9);
        out.cinematic = floats(image_space->cinematic, 3);
        out.tint = floats(image_space->tint, 4);
        image_spaces_[out.id] = std::move(out);
    }

    /// LGTM (UESP, LGTM record): DATA as XCLL, DALC 32 bytes.
    void on_lighting_template(const record::MergedRecord& merged, io::SpanReader& data) {
        LightingTemplateEntry out;
        bool has_data = false;
        const auto walked = record::for_each_field(
            data, [&](const record::FieldHeader& field, io::SpanReader& body) {
                if (field.type == FourCC{"DATA"}) {
                    const auto raw = body.bytes(body.remaining());
                    if (raw) {
                        if (auto decoded = decode_xcll(*raw)) {
                            const auto ambient = out.lighting.directional_ambient;
                            out.lighting = *decoded;
                            out.lighting.directional_ambient = ambient;
                            out.lighting.inherit = 0;
                            has_data = true;
                        }
                    }
                } else if (field.type == FourCC{"DALC"} && body.remaining() >= 24) {
                    for (auto& colour : out.lighting.directional_ambient) {
                        colour = body.get<std::uint32_t>().value_or(0);
                    }
                }
            });
        if (!walked || !has_data) {
            ++stats_.parse_errors;
            return;
        }
        lighting_templates_[merged.form.value] = std::move(out);
    }

    /// Cloud layers: textures (00TX..), speeds (QNAM, RNAM: one byte each,
    /// 127 still), colours by time (PNAM, RGBA), alphas by time (JNAM) and
    /// NAM1, whose set bits disable a layer. Layer i is drawn on the i-th
    /// shape of meshes/sky/clouds.nif.
    static void read_clouds(const record::Weather& weather, WorldWeather& out) {
        out.clouds.resize(record::Weather::k_cloud_layers);
        for (std::size_t i = 0; i < out.clouds.size(); ++i) {
            auto& layer = out.clouds[i];
            layer.texture = texture_vpath(weather.cloud_textures[i]);
            layer.enabled = !layer.texture.empty() &&
                            (weather.cloud_layers_disabled & (1U << i)) == 0;
            const auto speed = [&](const std::vector<std::byte>& raw) {
                return i < raw.size()
                           ? (static_cast<float>(static_cast<std::uint8_t>(raw[i])) - 127.0F) / 127.0F
                           : 0.0F;
            };
            layer.speed_x = speed(weather.cloud_speed_x);
            layer.speed_y = speed(weather.cloud_speed_y);
            io::SpanReader colours(weather.cloud_colours, "WTHR PNAM");
            io::SpanReader alphas(weather.cloud_alphas, "WTHR JNAM");
            (void)colours.skip(i * 16);
            (void)alphas.skip(i * 16);
            for (std::size_t t = 0; t < 4; ++t) {
                layer.colors[t] = colours.get<std::uint32_t>().value_or(0);
                layer.alphas[t] = alphas.get<float>().value_or(1.0F);
            }
        }
    }

    void on_precipitation(const record::MergedRecord& merged, io::SpanReader& data,
                          const record::FormContext& form_ctx) {
        auto spgd = record::parse_shader_particle_geometry(data, form_ctx);
        if (!spgd) {
            ++stats_.parse_errors;
            return;
        }
        precipitations_[merged.form.value] = WorldPrecipitation{
            .id = merged.form.value,
            .editor_id = spgd->editor_id,
            .texture = texture_vpath(spgd->texture),
            .gravity_velocity = spgd->gravity_velocity,
            .rotation_velocity = spgd->rotation_velocity,
            .size_x = spgd->particle_size_x,
            .size_y = spgd->particle_size_y,
            .center_offset_min = spgd->center_offset_min,
            .center_offset_max = spgd->center_offset_max,
            .rotation_range = spgd->initial_rotation_range,
            .subtextures_x = spgd->subtextures_x,
            .subtextures_y = spgd->subtextures_y,
            .type = static_cast<std::uint8_t>(spgd->type),
            .box_size = spgd->box_size,
            .density = spgd->particle_density,
        };
    }

    /// Regions with a weather list (RDAT type 3); the others are not needed
    /// yet.
    void on_region(const record::MergedRecord& merged, io::SpanReader& data,
                   const record::FormContext& form_ctx) {
        auto region = record::parse_region(data, form_ctx);
        if (!region) {
            ++stats_.parse_errors;
            return;
        }
        constexpr std::uint32_t k_weather = 3;
        bool failed = false;
        WorldRegion out;
        out.id = merged.form.value;
        out.editor_id = region->editor_id;
        out.world = global(merged, region->worldspace, failed);
        for (const auto& entry : region->entries) {
            if (entry.type != k_weather || entry.weathers.empty()) {
                continue;
            }
            out.weather_priority = entry.priority;
            out.weather_override = (entry.flags & 0x1U) != 0;
            for (const auto& w : entry.weathers) {
                out.weathers.push_back({.weather = global(merged, w.weather, failed),
                                        .chance = w.chance,
                                        .global = global(merged, w.global, failed)});
            }
        }
        if (out.weathers.empty()) {
            return;
        }
        for (const auto& area : region->areas) {
            std::vector<float> points;
            points.reserve(area.points.size() * 2);
            for (const auto& [x, y] : area.points) {
                points.push_back(x);
                points.push_back(y);
            }
            out.areas.push_back(std::move(points));
        }
        if (failed) {
            ++stats_.unresolved;
        }
        regions_[out.id] = std::move(out);
    }

    const record::LoadOrder& order_;
    WorldStats stats_;
    std::map<std::uint32_t, WorldWater> waters_;
    std::map<std::uint32_t, WorldClimate> climates_;
    std::map<std::uint32_t, WorldWeather> weathers_;
    std::map<std::uint32_t, WorldImageSpace> image_spaces_;
    std::map<std::uint32_t, LightingTemplateEntry> lighting_templates_;
    std::map<std::uint32_t, MaterialObjectEntry> material_objects_;
    std::map<std::uint32_t, GrassEntry> grasses_;
    std::map<std::uint32_t, AddonEntry> addons_;
    std::map<std::uint32_t, WorldPrecipitation> precipitations_;
    std::map<std::uint32_t, WorldRegion> regions_;
    std::unordered_map<std::uint32_t, WorldTerrain> terrains_;
    std::map<std::uint32_t, Worldspace> worlds_;
    std::map<std::uint32_t, LandTextureEntry> land_textures_;
    std::unordered_map<std::uint32_t, TextureSetEntry> texture_sets_;
    std::map<std::uint32_t, CellEntry> cells_;
    std::map<std::uint32_t, BaseEntry> bases_;
    std::unordered_map<std::uint32_t, std::vector<WorldRef>> refs_;
    std::unordered_map<std::uint32_t, std::vector<WorldDoor>> doors_;
    std::unordered_map<std::uint32_t, CellExtras> extras_;
    std::map<std::uint32_t, WorldQuest> quests_;
    std::map<std::uint32_t, WorldGlobal> globals_;
    std::vector<WorldActor> actors_;
    std::unordered_map<std::uint32_t, std::vector<WorldNavMesh>> navmeshes_;
    std::map<std::uint32_t, WorldNpc> npcs_;
    std::map<std::uint32_t, WorldRace> races_;
    std::map<std::uint32_t, WorldArmor> armors_;
    std::map<std::uint32_t, WorldArmorAddon> armor_addons_;
    std::map<std::uint32_t, WorldOutfit> outfits_;
    std::map<std::uint32_t, WorldLeveledList> leveled_lists_;
    std::map<std::uint32_t, WorldPackage> packages_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> form_lists_;
};

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<wfb::Script>>> write_scripts(
    flatbuffers::FlatBufferBuilder& builder, const std::vector<record::Script>& scripts) {
    std::vector<flatbuffers::Offset<wfb::Script>> out;
    out.reserve(scripts.size());
    for (const auto& script : scripts) {
        std::vector<flatbuffers::Offset<wfb::ScriptProperty>> properties;
        properties.reserve(script.properties.size());
        for (const auto& p : script.properties) {
            std::vector<wfb::ScriptObject> objects;
            objects.reserve(p.objects.size());
            for (const auto& o : p.objects) {
                objects.emplace_back(o.form.value, o.alias);
            }
            std::vector<flatbuffers::Offset<flatbuffers::String>> strings;
            strings.reserve(p.strings.size());
            for (const auto& text : p.strings) {
                strings.push_back(builder.CreateString(text));
            }
            const auto name = builder.CreateString(p.name);
            const auto objects_off = objects.empty() ? 0 : builder.CreateVectorOfStructs(objects);
            const auto strings_off = strings.empty() ? 0 : builder.CreateVector(strings);
            const auto ints_off = p.integers.empty() ? 0 : builder.CreateVector(p.integers);
            const auto floats_off = p.floats.empty() ? 0 : builder.CreateVector(p.floats);
            properties.push_back(wfb::CreateScriptProperty(
                builder, name, static_cast<std::uint8_t>(p.type), p.status, objects_off,
                strings_off, ints_off, floats_off));
        }
        const auto name = builder.CreateString(script.name);
        const auto properties_off = builder.CreateVector(properties);
        out.push_back(wfb::CreateScript(builder, name, script.status, properties_off));
    }
    return builder.CreateVector(out);
}

std::vector<record::Script> read_scripts(
    const flatbuffers::Vector<flatbuffers::Offset<wfb::Script>>* scripts) {
    std::vector<record::Script> out;
    if (scripts == nullptr) {
        return out;
    }
    out.reserve(scripts->size());
    for (const auto* s : *scripts) {
        record::Script script;
        if (const auto* name = s->name()) {
            script.name = name->str();
        }
        script.status = s->status();
        if (const auto* properties = s->properties()) {
            for (const auto* p : *properties) {
                record::ScriptProperty property;
                if (const auto* name = p->name()) {
                    property.name = name->str();
                }
                property.type = static_cast<record::ScriptPropertyType>(p->type());
                property.status = p->status();
                if (const auto* objects = p->objects()) {
                    for (const auto* o : *objects) {
                        property.objects.push_back(
                            record::ScriptObject{.form = FormId{o->form()}, .alias = o->alias()});
                    }
                }
                if (const auto* strings = p->strings()) {
                    for (const auto* text : *strings) {
                        property.strings.push_back(text->str());
                    }
                }
                if (const auto* ints = p->ints()) {
                    property.integers.assign(ints->begin(), ints->end());
                }
                if (const auto* floats = p->floats()) {
                    property.floats.assign(floats->begin(), floats->end());
                }
                script.properties.push_back(std::move(property));
            }
        }
        out.push_back(std::move(script));
    }
    return out;
}

wfb::Vec3f to_fb(const record::Vec3& v) { return wfb::Vec3f(v.x, v.y, v.z); }
record::Vec3 from_fb(const wfb::Vec3f& v) { return record::Vec3{v.x(), v.y(), v.z()}; }

} // namespace

/// The lighting of an interior as the game uses it: XCLL with the inherited
/// values taken from the lighting template, or the template's alone when the
/// cell has no XCLL. Exteriors keep XCLL as it is.
std::optional<WorldCellLighting> resolve_lighting(
    const CellEntry& cell, const std::map<std::uint32_t, LightingTemplateEntry>& templates) {
    const auto it = templates.find(cell.lighting_template);
    if ((cell.flags & 0x1u) == 0 || it == templates.end()) {
        return cell.lighting;
    }
    const auto& t = it->second.lighting;
    if (!cell.lighting) {
        return t;
    }
    WorldCellLighting out = *cell.lighting;
    const auto inherits = [&](std::uint32_t bit) { return (out.inherit & bit) != 0; };
    if (inherits(k_inherit_ambient)) {
        out.ambient = t.ambient;
        out.directional_ambient = t.directional_ambient;
    }
    if (inherits(k_inherit_directional)) {
        out.directional = t.directional;
    }
    if (inherits(k_inherit_fog_color)) {
        out.fog_near_color = t.fog_near_color;
        out.fog_far_color = t.fog_far_color;
    }
    if (inherits(k_inherit_fog_near)) {
        out.fog_near = t.fog_near;
    }
    if (inherits(k_inherit_fog_far)) {
        out.fog_far = t.fog_far;
    }
    if (inherits(k_inherit_rotation)) {
        out.directional_rotation_xy = t.directional_rotation_xy;
        out.directional_rotation_z = t.directional_rotation_z;
    }
    if (inherits(k_inherit_fade)) {
        out.directional_fade = t.directional_fade;
    }
    if (inherits(k_inherit_fog_power)) {
        out.fog_power = t.fog_power;
    }
    if (inherits(k_inherit_fog_max)) {
        out.fog_max = t.fog_max;
    }
    if (inherits(k_inherit_light_fade)) {
        out.light_fade_begin = t.light_fade_begin;
        out.light_fade_end = t.light_fade_end;
    }
    return out;
}

io::ParseResult<WorldStats> write_world(const record::MergedWorld& world,
                                        const record::LoadOrder& order,
                                        const std::filesystem::path& out) {
    WorldSink sink(order);
    world.for_each_record(sink);
    auto& stats = sink.stats();

    flatbuffers::FlatBufferBuilder builder(1u << 20);

    std::vector<flatbuffers::Offset<wfb::Cell>> cells;
    cells.reserve(sink.cells().size());
    for (auto& [id, cell] : sink.cells()) { // std::map: sorted by id
        auto& refs = sink.refs()[id];
        std::ranges::sort(refs, {}, &WorldRef::id);
        auto& doors = sink.doors()[id];
        std::ranges::sort(doors, {}, &WorldDoor::ref);

        std::vector<wfb::Ref> fb_refs;
        fb_refs.reserve(refs.size());
        for (const auto& r : refs) {
            fb_refs.emplace_back(r.id, r.base, to_fb(r.position), to_fb(r.rotation), r.scale,
                                 r.flags, r.enable_parent);
        }
        std::vector<wfb::DoorLink> fb_doors;
        fb_doors.reserve(doors.size());
        for (const auto& d : doors) {
            fb_doors.emplace_back(d.ref, d.destination, to_fb(d.position), to_fb(d.rotation));
        }
        stats.refs += refs.size();
        stats.doors += doors.size();

        auto& extras = sink.extras()[id];
        std::ranges::stable_sort(extras.scripts, {}, &WorldRefScripts::ref);
        std::ranges::stable_sort(extras.locks, {}, &WorldLock::ref);
        std::ranges::stable_sort(extras.links, {}, &WorldLink::ref);
        std::ranges::stable_sort(extras.activate_parents, {}, &WorldActivateParent::ref);
        std::ranges::stable_sort(extras.primitives, {}, &WorldPrimitive::ref);
        std::ranges::stable_sort(extras.light_overrides, {}, &WorldLightOverride::ref);
        std::vector<flatbuffers::Offset<wfb::RefScripts>> fb_scripts;
        fb_scripts.reserve(extras.scripts.size());
        for (const auto& r : extras.scripts) {
            fb_scripts.push_back(
                wfb::CreateRefScripts(builder, r.ref, write_scripts(builder, r.scripts)));
            stats.scripts += r.scripts.size();
        }
        std::vector<wfb::Lock> fb_locks;
        for (const auto& l : extras.locks) {
            fb_locks.emplace_back(l.ref, l.level, l.flags, l.key);
        }
        std::vector<wfb::LinkedRef> fb_links;
        for (const auto& l : extras.links) {
            fb_links.emplace_back(l.ref, l.keyword, l.target);
        }
        std::vector<wfb::ActivateParent> fb_parents;
        for (const auto& a : extras.activate_parents) {
            fb_parents.emplace_back(a.ref, a.parent, a.delay);
        }
        std::vector<wfb::Primitive> fb_primitives;
        for (const auto& p : extras.primitives) {
            fb_primitives.emplace_back(p.ref, to_fb(p.bounds), p.type);
        }
        stats.locks += fb_locks.size();
        stats.links += fb_links.size();
        stats.activate_parents += fb_parents.size();
        stats.primitives += fb_primitives.size();
        const auto scripts_off = builder.CreateVector(fb_scripts);
        const auto locks_off = builder.CreateVectorOfStructs(fb_locks);
        const auto links_off = builder.CreateVectorOfStructs(fb_links);
        const auto parents_off = builder.CreateVectorOfStructs(fb_parents);
        const auto primitives_off = builder.CreateVectorOfStructs(fb_primitives);
        std::vector<wfb::LightOverride> fb_lights;
        for (const auto& l : extras.light_overrides) {
            fb_lights.emplace_back(l.ref, l.has_radius, l.radius, l.has_light_data, l.fov, l.fade,
                                   l.end_distance_cap, l.shadow_depth_bias);
        }
        const auto light_overrides_off = builder.CreateVectorOfStructs(fb_lights);

        std::vector<flatbuffers::Offset<wfb::NavMesh>> fb_navmeshes;
        if (const auto n = sink.navmeshes().find(id); n != sink.navmeshes().end()) {
            std::ranges::sort(n->second, {}, &WorldNavMesh::id);
            for (const auto& nav : n->second) {
                std::vector<wfb::Vec3f> vertices;
                vertices.reserve(nav.vertices.size());
                for (const auto& v : nav.vertices) {
                    vertices.push_back(to_fb(v));
                }
                std::vector<wfb::NavTriangle> triangles;
                triangles.reserve(nav.triangles.size());
                for (const auto& t : nav.triangles) {
                    triangles.emplace_back(t.vertices[0], t.vertices[1], t.vertices[2],
                                           t.edges[0], t.edges[1], t.edges[2], t.flags, t.cover);
                }
                std::vector<wfb::NavLink> nav_links;
                for (const auto& l : nav.links) {
                    nav_links.emplace_back(l.type, l.navmesh, l.triangle);
                }
                std::vector<wfb::NavDoor> nav_doors;
                for (const auto& d : nav.doors) {
                    nav_doors.emplace_back(d.triangle, d.door);
                }
                const auto v_off = builder.CreateVectorOfStructs(vertices);
                const auto t_off = builder.CreateVectorOfStructs(triangles);
                const auto l_off = builder.CreateVectorOfStructs(nav_links);
                const auto d_off = builder.CreateVectorOfStructs(nav_doors);
                fb_navmeshes.push_back(wfb::CreateNavMesh(builder, nav.id, v_off, t_off, l_off, d_off));
                ++stats.navmeshes;
                stats.nav_triangles += nav.triangles.size();
            }
        }
        const auto navmeshes_off = builder.CreateVector(fb_navmeshes);

        const auto editor_id = builder.CreateString(cell.editor_id);
        const auto refs_off = builder.CreateVectorOfStructs(fb_refs);
        const auto doors_off = builder.CreateVectorOfStructs(fb_doors);

        flatbuffers::Offset<wfb::Terrain> terrain_off;
        if (const auto t = sink.terrains().find(id); t != sink.terrains().end()) {
            const WorldTerrain& terrain = t->second;
            std::vector<flatbuffers::Offset<wfb::TerrainLayer>> layers;
            layers.reserve(terrain.layers.size());
            for (const auto& layer : terrain.layers) {
                const auto points = builder.CreateVector(layer.points);
                const auto opacity = builder.CreateVector(layer.opacity);
                layers.push_back(wfb::CreateTerrainLayer(builder, layer.texture, layer.quadrant,
                                                         layer.layer, points, opacity));
            }
            const auto deltas = builder.CreateVector(terrain.height_deltas);
            const auto colours = builder.CreateVector(terrain.colours);
            const auto layers_off = builder.CreateVector(layers);
            terrain_off = wfb::CreateTerrain(builder, terrain.height_offset, deltas, colours,
                                             layers_off);
            ++stats.terrains;
            stats.terrain_layers += terrain.layers.size();
        }

        const auto resolved = resolve_lighting(cell, sink.lighting_templates());
        std::optional<wfb::CellLighting> lighting;
        flatbuffers::Offset<flatbuffers::Vector<std::uint32_t>> ambient_off;
        if (resolved) {
            const auto& l = *resolved;
            if (std::ranges::any_of(l.directional_ambient, [](std::uint32_t c) { return c != 0; })) {
                ambient_off = builder.CreateVector(l.directional_ambient.data(), l.directional_ambient.size());
            }
            lighting = wfb::CellLighting(
                l.ambient, l.directional, l.fog_near_color, l.fog_far_color, l.fog_near,
                l.fog_far, l.fog_power, l.fog_max, l.directional_rotation_xy,
                l.directional_rotation_z, l.directional_fade, l.light_fade_begin,
                l.light_fade_end, l.inherit);
        }

        wfb::CellBuilder cb(builder);
        cb.add_id(cell.id);
        cb.add_editor_id(editor_id);
        cb.add_world(cell.world);
        cb.add_flags(cell.flags);
        if (cell.grid) {
            cb.add_has_grid(true);
            cb.add_grid_x(cell.grid->x);
            cb.add_grid_y(cell.grid->y);
        }
        cb.add_water_height(cell.water_height);
        if (lighting) {
            cb.add_has_lighting(true);
            cb.add_lighting(&*lighting);
        }
        cb.add_lighting_template(cell.lighting_template);
        cb.add_refs(refs_off);
        cb.add_doors(doors_off);
        if (!terrain_off.IsNull()) {
            cb.add_terrain(terrain_off);
        }
        cb.add_persistent(cell.persistent);
        cb.add_water(cell.water);
        cb.add_scripts(scripts_off);
        cb.add_locks(locks_off);
        cb.add_links(links_off);
        cb.add_activate_parents(parents_off);
        cb.add_primitives(primitives_off);
        cb.add_navmeshes(navmeshes_off);
        if (!ambient_off.IsNull()) {
            cb.add_directional_ambient(ambient_off);
        }
        cb.add_image_space(cell.image_space);
        cb.add_light_overrides(light_overrides_off);
        cells.push_back(cb.Finish());

        ++stats.cells;
        if ((cell.flags & 0x1u) != 0) {
            ++stats.interior_cells;
        }
    }
    // References whose parent is not a cell record.
    for (const auto& [parent, refs] : sink.refs()) {
        if (!sink.cells().contains(parent)) {
            stats.orphan_refs += refs.size();
        }
    }
    for (const auto& [parent, navmeshes] : sink.navmeshes()) {
        if (!sink.cells().contains(parent)) {
            stats.orphan_navmeshes += navmeshes.size();
        }
    }

    std::vector<flatbuffers::Offset<wfb::Base>> bases;
    bases.reserve(sink.bases().size());
    for (const auto& [id, base] : sink.bases()) {
        const auto editor_id = builder.CreateString(base.editor_id);
        const auto model = builder.CreateString(base.model);
        const auto scripts = base.scripts.empty() ? 0 : write_scripts(builder, base.scripts);
        stats.scripts += base.scripts.size();
        std::optional<wfb::LightData> light;
        if (base.light) {
            const auto& l = *base.light;
            light = wfb::LightData(l.radius, l.color, l.flags, l.falloff_exponent, l.fov,
                                   l.near_clip, l.fade, l.flicker_period, l.flicker_intensity,
                                   l.flicker_movement);
        }
        wfb::BaseBuilder bb(builder);
        bb.add_id(base.id);
        bb.add_type(base.type);
        bb.add_editor_id(editor_id);
        bb.add_model(model);
        bb.add_flags(base.flags);
        bb.add_record_flags(base.record_flags);
        if (base.directional_material != 0) {
            bb.add_directional_material(base.directional_material);
            bb.add_directional_max_angle(base.directional_max_angle);
        }
        if (!base.scripts.empty()) {
            bb.add_scripts(scripts);
        }
        if (light) {
            bb.add_has_light(true);
            bb.add_light(&*light);
            ++stats.lights;
        }
        bases.push_back(bb.Finish());
        ++stats.bases;
    }

    std::vector<flatbuffers::Offset<wfb::Worldspace>> worlds;
    for (const auto& [id, w] : sink.worlds()) {
        const auto editor_id = builder.CreateString(w.editor_id);
        wfb::WorldspaceBuilder wsb(builder);
        wsb.add_id(w.id);
        wsb.add_editor_id(editor_id);
        wsb.add_parent(w.parent);
        wsb.add_parent_flags(w.parent_flags);
        wsb.add_flags(w.flags);
        if (w.defaults) {
            wsb.add_has_defaults(true);
            wsb.add_default_land_height((*w.defaults)[0]);
            wsb.add_default_water_height((*w.defaults)[1]);
        }
        wsb.add_water(w.water);
        wsb.add_climate(w.climate);
        wsb.add_min_x(w.bounds[0]);
        wsb.add_min_y(w.bounds[1]);
        wsb.add_max_x(w.bounds[2]);
        wsb.add_max_y(w.bounds[3]);
        worlds.push_back(wsb.Finish());
        ++stats.worlds;
    }

    std::vector<flatbuffers::Offset<wfb::LandTexture>> land_textures;
    for (const auto& [id, ltex] : sink.land_textures()) {
        TextureSetEntry paths;
        if (const auto t = sink.texture_sets().find(ltex.texture_set);
            t != sink.texture_sets().end()) {
            paths = t->second;
        }
        land_textures.push_back(wfb::CreateLandTexture(
            builder, ltex.id, builder.CreateString(ltex.editor_id),
            builder.CreateString(paths.diffuse), builder.CreateString(paths.normal),
            ltex.specular, builder.CreateVector(ltex.grasses)));
        ++stats.land_textures;
    }

    std::vector<flatbuffers::Offset<wfb::Water>> waters;
    for (const auto& [id, w] : sink.waters()) {
        std::vector<wfb::WaterLayer> layers;
        for (const auto& l : w.layers) {
            layers.emplace_back(l.wind_direction, l.wind_speed, l.uv_scale, l.amplitude);
        }
        std::vector<flatbuffers::Offset<flatbuffers::String>> noise;
        for (const auto& path : w.noise) {
            noise.push_back(builder.CreateString(path));
        }
        const auto editor_id = builder.CreateString(w.editor_id);
        const auto layers_off = builder.CreateVectorOfStructs(layers);
        const auto noise_off = builder.CreateVector(noise);
        wfb::WaterBuilder wb2(builder);
        wb2.add_id(w.id);
        wb2.add_editor_id(editor_id);
        wb2.add_opacity(w.opacity);
        wb2.add_flags(w.flags);
        wb2.add_shallow_color(w.shallow_color);
        wb2.add_deep_color(w.deep_color);
        wb2.add_reflection_color(w.reflection_color);
        wb2.add_sun_specular_power(w.sun_specular_power);
        wb2.add_reflectivity(w.reflectivity);
        wb2.add_fresnel(w.fresnel);
        wb2.add_fog_near(w.fog_near);
        wb2.add_fog_far(w.fog_far);
        wb2.add_specular_power(w.specular_power);
        wb2.add_refraction_magnitude(w.refraction_magnitude);
        wb2.add_reflection_magnitude(w.reflection_magnitude);
        wb2.add_layers(layers_off);
        wb2.add_noise(noise_off);
        waters.push_back(wb2.Finish());
        ++stats.waters;
    }

    std::vector<flatbuffers::Offset<wfb::Climate>> climates;
    for (const auto& [id, c] : sink.climates()) {
        std::vector<wfb::ClimateWeather> entries;
        for (const auto& [weather, chance] : c.weathers) {
            entries.emplace_back(weather, chance);
        }
        climates.push_back(wfb::CreateClimate(
            builder, c.id, builder.CreateString(c.editor_id),
            builder.CreateVectorOfStructs(entries), c.sun[0], c.sun[1], c.sun[2], c.sun[3],
            builder.CreateString(c.sun_texture), builder.CreateString(c.sun_glare_texture),
            builder.CreateString(c.sky), c.volatility, c.moons, c.phase_length));
        ++stats.climates;
    }
    std::vector<flatbuffers::Offset<wfb::Weather>> weathers;
    for (const auto& [id, w] : sink.weathers()) {
        std::vector<flatbuffers::Offset<wfb::CloudLayer>> clouds;
        clouds.reserve(w.clouds.size());
        for (const auto& layer : w.clouds) {
            clouds.push_back(wfb::CreateCloudLayer(
                builder, builder.CreateString(layer.texture), layer.speed_x, layer.speed_y,
                builder.CreateVector(layer.colors.data(), layer.colors.size()),
                builder.CreateVector(layer.alphas.data(), layer.alphas.size()), layer.enabled));
        }
        weathers.push_back(wfb::CreateWeather(
            builder, w.id, builder.CreateString(w.editor_id), builder.CreateVector(w.colors),
            builder.CreateVector(w.fog), builder.CreateVector(w.directional_ambient),
            builder.CreateVector(clouds), w.wind_speed, w.wind_direction,
            w.wind_direction_range, w.transition_delta, w.sun_glare, w.sun_damage,
            w.precipitation_begin, w.precipitation_end, w.thunder_begin, w.thunder_end,
            w.thunder_frequency, w.classification, w.lightning_color, w.precipitation,
            builder.CreateString(w.aurora), builder.CreateVector(w.image_spaces)));
        ++stats.weathers;
    }
    std::vector<flatbuffers::Offset<wfb::ImageSpace>> image_spaces;
    for (const auto& [id, i] : sink.image_spaces()) {
        image_spaces.push_back(wfb::CreateImageSpace(
            builder, i.id, builder.CreateString(i.editor_id), builder.CreateVector(i.hdr),
            builder.CreateVector(i.cinematic), builder.CreateVector(i.tint)));
        ++stats.image_spaces;
    }
    stats.lighting_templates = sink.lighting_templates().size();
    std::vector<flatbuffers::Offset<wfb::MaterialObject>> material_objects;
    for (const auto& [id, m] : sink.material_objects()) {
        const auto& d = m.data;
        const std::array<float, 3> projection{d[4], d[5], d[6]};
        const std::array<float, 3> colour{d[8], d[9], d[10]};
        material_objects.push_back(wfb::CreateMaterialObject(
            builder, m.id, builder.CreateString(m.editor_id), builder.CreateString(m.model), d[0], d[1],
            d[2], d[3], builder.CreateVector(projection.data(), projection.size()), d[7],
            builder.CreateVector(colour.data(), colour.size()), (m.flags & 1U) != 0));
    }
    std::vector<flatbuffers::Offset<wfb::Precipitation>> precipitations;
    for (const auto& [id, p] : sink.precipitations()) {
        precipitations.push_back(wfb::CreatePrecipitation(
            builder, p.id, builder.CreateString(p.editor_id), builder.CreateString(p.texture),
            p.gravity_velocity, p.rotation_velocity, p.size_x, p.size_y, p.center_offset_min,
            p.center_offset_max, p.rotation_range, p.subtextures_x, p.subtextures_y, p.type,
            p.box_size, p.density));
        ++stats.precipitations;
    }
    std::vector<flatbuffers::Offset<wfb::Region>> regions;
    for (const auto& [id, r] : sink.regions()) {
        std::vector<flatbuffers::Offset<wfb::RegionArea>> areas;
        for (const auto& area : r.areas) {
            areas.push_back(wfb::CreateRegionArea(builder, builder.CreateVector(area)));
        }
        std::vector<wfb::RegionWeather> entries;
        for (const auto& e : r.weathers) {
            entries.emplace_back(e.weather, e.chance, e.global);
        }
        regions.push_back(wfb::CreateRegion(builder, r.id, builder.CreateString(r.editor_id),
                                            r.world, builder.CreateVector(areas),
                                            builder.CreateVectorOfStructs(entries),
                                            r.weather_priority, r.weather_override));
        ++stats.regions;
    }

    std::vector<flatbuffers::Offset<wfb::Quest>> quests;
    for (const auto& [id, q] : sink.quests()) {
        std::vector<flatbuffers::Offset<wfb::QuestFragment>> fragments;
        for (const auto& f : q.fragments) {
            fragments.push_back(wfb::CreateQuestFragment(builder, f.stage, f.log_entry,
                                                         builder.CreateString(f.function)));
        }
        std::vector<flatbuffers::Offset<wfb::QuestStage>> stages;
        for (const auto& stage : q.stages) {
            std::vector<flatbuffers::Offset<wfb::QuestLogEntry>> log;
            for (const auto& e : stage.log) {
                log.push_back(wfb::CreateQuestLogEntry(builder, e.flags,
                                                       builder.CreateString(e.text),
                                                       e.conditions));
            }
            stages.push_back(wfb::CreateQuestStage(builder, stage.index, stage.flags,
                                                   builder.CreateVector(log)));
        }
        std::vector<flatbuffers::Offset<wfb::QuestObjective>> objectives;
        for (const auto& o : q.objectives) {
            objectives.push_back(wfb::CreateQuestObjective(builder, o.index, o.flags,
                                                           builder.CreateString(o.text),
                                                           builder.CreateVector(o.targets)));
        }
        std::vector<flatbuffers::Offset<wfb::QuestAlias>> aliases;
        for (const auto& a : q.aliases) {
            const auto name = builder.CreateString(a.name);
            const auto scripts = a.scripts.empty() ? 0 : write_scripts(builder, a.scripts);
            stats.scripts += a.scripts.size();
            wfb::QuestAliasBuilder ab(builder);
            ab.add_id(a.id);
            ab.add_name(name);
            ab.add_location(a.location);
            ab.add_flags(a.flags);
            ab.add_forced(a.forced);
            ab.add_unique_actor(a.unique_actor);
            ab.add_external_quest(a.external_quest);
            ab.add_external_alias(a.external_alias);
            ab.add_created_object(a.created_object);
            ab.add_create_at(a.create_at);
            ab.add_conditions(a.conditions);
            ab.add_display_name(a.display_name);
            if (!a.scripts.empty()) {
                ab.add_scripts(scripts);
            }
            aliases.push_back(ab.Finish());
        }
        stats.quest_aliases += q.aliases.size();
        stats.quest_fragments += q.fragments.size();
        stats.scripts += q.scripts.size();
        const auto editor_id = builder.CreateString(q.editor_id);
        const auto name = builder.CreateString(q.name);
        const auto scripts = q.scripts.empty() ? 0 : write_scripts(builder, q.scripts);
        const auto fragment_script = builder.CreateString(q.fragment_script);
        const auto fragments_off = builder.CreateVector(fragments);
        const auto stages_off = builder.CreateVector(stages);
        const auto objectives_off = builder.CreateVector(objectives);
        const auto aliases_off = builder.CreateVector(aliases);
        wfb::QuestBuilder qb(builder);
        qb.add_id(q.id);
        qb.add_editor_id(editor_id);
        qb.add_name(name);
        qb.add_flags(q.flags);
        qb.add_priority(q.priority);
        qb.add_type(q.type);
        qb.add_event(q.event);
        if (!q.scripts.empty()) {
            qb.add_scripts(scripts);
        }
        qb.add_fragment_script(fragment_script);
        qb.add_fragments(fragments_off);
        qb.add_stages(stages_off);
        qb.add_objectives(objectives_off);
        qb.add_aliases(aliases_off);
        quests.push_back(qb.Finish());
        ++stats.quests;
    }
    std::vector<flatbuffers::Offset<wfb::Global>> globals;
    for (const auto& [id, g] : sink.globals()) {
        globals.push_back(wfb::CreateGlobal(builder, g.id, builder.CreateString(g.editor_id),
                                            static_cast<std::uint8_t>(g.kind), g.value));
        ++stats.globals;
    }
    // ---- what actors are built from ----
    const auto strings = [&](const auto& list) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> offsets;
        for (const auto& text : list) {
            offsets.push_back(builder.CreateString(text));
        }
        return builder.CreateVector(offsets);
    };
    // DPLT names an FLST of packages.
    const auto default_packages = [&](const WorldNpc& n) {
        const auto found = sink.form_lists().find(n.default_package_list);
        return found != sink.form_lists().end() ? found->second : std::vector<std::uint32_t>{};
    };
    std::vector<flatbuffers::Offset<wfb::Npc>> npcs;
    for (const auto& [id, n] : sink.npcs()) {
        std::vector<wfb::NpcItem> items;
        for (const auto& [form, count] : n.items) {
            items.emplace_back(form, count);
        }
        std::vector<wfb::NpcFaction> factions;
        for (const auto& [faction, rank] : n.factions) {
            factions.emplace_back(faction, rank);
        }
        npcs.push_back(wfb::CreateNpc(builder, n.id, builder.CreateString(n.editor_id),
                                      builder.CreateString(n.name), n.flags, n.level, n.race,
                                      n.template_form, n.template_flags, n.skin, n.default_outfit,
                                      n.sleeping_outfit, n.height, n.weight,
                                      builder.CreateVector(n.head_parts),
                                      builder.CreateVectorOfStructs(items),
                                      builder.CreateString(n.face_model),
                                      builder.CreateVector(std::vector<float>(n.skin_tone.begin(), n.skin_tone.end())),
                                      builder.CreateVector(n.packages),
                                      builder.CreateVector(default_packages(n)),
                                      builder.CreateVectorOfStructs(factions)));
        ++stats.npcs;
    }
    const auto conditions = [&](const std::vector<record::Condition>& list) {
        std::vector<flatbuffers::Offset<wfb::Condition>> offsets;
        for (const auto& c : list) {
            offsets.push_back(wfb::CreateCondition(
                builder, c.type, c.function, c.value, c.value_global.value, c.param1, c.param2,
                c.run_on, c.reference.value, c.param3,
                c.string1.empty() ? 0 : builder.CreateString(c.string1),
                c.string2.empty() ? 0 : builder.CreateString(c.string2)));
        }
        return builder.CreateVector(offsets);
    };
    std::vector<flatbuffers::Offset<wfb::Package>> packages;
    for (const auto& [id, p] : sink.packages()) {
        std::vector<flatbuffers::Offset<wfb::PackageInput>> inputs;
        for (const auto& in : p.inputs) {
            inputs.push_back(wfb::CreatePackageInput(
                builder, in.key, builder.CreateString(in.type),
                in.name.empty() ? 0 : builder.CreateString(in.name), in.number, in.location.type,
                in.location.value, in.location.radius, in.target.type, in.target.value,
                in.target.count));
        }
        std::vector<flatbuffers::Offset<wfb::PackageBranch>> branches;
        for (const auto& b : p.branches) {
            branches.push_back(wfb::CreatePackageBranch(
                builder, builder.CreateString(b.type), conditions(b.conditions), b.children,
                b.flags, b.procedure.empty() ? 0 : builder.CreateString(b.procedure),
                b.success_completes, builder.CreateVector(b.inputs), b.set_flags, b.clear_flags,
                b.speed));
        }
        const auto& sch = p.schedule;
        packages.push_back(wfb::CreatePackage(
            builder, p.id, builder.CreateString(p.editor_id), p.type, p.flags,
            p.interrupt_override, p.speed, p.interrupt_flags, sch.month, sch.day_of_week,
            sch.date, sch.hour, sch.minute, sch.duration, conditions(p.conditions),
            p.template_package, builder.CreateVector(inputs), builder.CreateVector(branches),
            p.idle_flags, p.idle_timer, builder.CreateVector(p.idles), p.owner_quest,
            p.combat_style, p.on_begin_idle, p.on_end_idle, p.on_change_idle));
        ++stats.packages;
    }
    std::vector<flatbuffers::Offset<wfb::Race>> races;
    for (const auto& [id, r] : sink.races()) {
        std::vector<flatbuffers::Offset<wfb::RaceBodyPart>> parts;
        for (const auto& p : r.body_parts) {
            parts.push_back(wfb::CreateRaceBodyPart(builder, p.female, p.index, builder.CreateString(p.model)));
        }
        const std::vector<float> heights(r.heights.begin(), r.heights.end());
        const std::vector<float> weights(r.weights.begin(), r.weights.end());
        races.push_back(wfb::CreateRace(builder, r.id, builder.CreateString(r.editor_id),
                                        strings(r.skeletons), strings(r.behaviours), r.skin,
                                        builder.CreateVector(heights), builder.CreateVector(weights),
                                        r.flags, builder.CreateVector(parts),
                                        builder.CreateVector(r.head_parts[0]),
                                        builder.CreateVector(r.head_parts[1]), r.armor_race));
        ++stats.races;
    }
    std::vector<flatbuffers::Offset<wfb::Armor>> armors;
    for (const auto& [id, a] : sink.armors()) {
        armors.push_back(wfb::CreateArmor(builder, a.id, builder.CreateString(a.editor_id), a.slots,
                                          a.race, builder.CreateVector(a.addons)));
        ++stats.armors;
    }
    std::vector<flatbuffers::Offset<wfb::ArmorAddon>> addons;
    for (const auto& [id, a] : sink.armor_addons()) {
        addons.push_back(wfb::CreateArmorAddon(
            builder, a.id, builder.CreateString(a.editor_id), a.slots, a.race,
            builder.CreateVector(a.additional_races), builder.CreateString(a.models[0]),
            builder.CreateString(a.models[1]), a.priorities[0], a.priorities[1], a.weight_sliders[0],
            a.weight_sliders[1]));
        ++stats.armor_addons;
    }
    std::vector<flatbuffers::Offset<wfb::Outfit>> outfits;
    for (const auto& [id, o] : sink.outfits()) {
        outfits.push_back(wfb::CreateOutfit(builder, o.id, builder.CreateVector(o.items)));
        ++stats.outfits;
    }
    std::vector<flatbuffers::Offset<wfb::LeveledList>> leveled;
    for (const auto& [id, l] : sink.leveled_lists()) {
        std::vector<wfb::LeveledEntry> entries;
        for (const auto& e : l.entries) {
            entries.emplace_back(e.level, e.count, e.form);
        }
        leveled.push_back(wfb::CreateLeveledList(builder, l.id, l.type, l.flags, l.chance_none,
                                                 builder.CreateVectorOfStructs(entries)));
        ++stats.leveled_lists;
    }
    const auto npcs_off = builder.CreateVector(npcs);
    const auto packages_off = builder.CreateVector(packages);
    const auto image_spaces_off = builder.CreateVector(image_spaces);
    const auto material_objects_off = builder.CreateVector(material_objects);
    std::vector<flatbuffers::Offset<wfb::Grass>> grasses;
    for (const auto& [id, g] : sink.grasses()) {
        grasses.push_back(wfb::CreateGrass(builder, g.id, builder.CreateString(g.editor_id),
                                           builder.CreateString(g.model), g.density, g.min_slope,
                                           g.max_slope, g.units_from_water, g.water_type, g.position_range,
                                           g.height_range, g.color_range, g.wave_period, g.flags));
    }
    const auto grasses_off = builder.CreateVector(grasses);
    std::vector<flatbuffers::Offset<wfb::AddonNode>> addon_nodes;
    for (const auto& [id, a] : sink.addons()) {
        addon_nodes.push_back(wfb::CreateAddonNode(builder, a.id, builder.CreateString(a.editor_id), a.index,
                                                   builder.CreateString(a.model)));
    }
    const auto addon_nodes_off = builder.CreateVector(addon_nodes);
    const auto races_off = builder.CreateVector(races);
    const auto armors_off = builder.CreateVector(armors);
    const auto addons_off = builder.CreateVector(addons);
    const auto outfits_off = builder.CreateVector(outfits);
    const auto leveled_off = builder.CreateVector(leveled);

    auto& actors = sink.actors();
    std::ranges::sort(actors, {}, &WorldActor::ref);
    std::vector<wfb::ActorRef> fb_actors;
    fb_actors.reserve(actors.size());
    for (const auto& a : actors) {
        fb_actors.emplace_back(a.ref, a.base, a.cell, to_fb(a.position), to_fb(a.rotation),
                               a.flags);
    }
    stats.actors += fb_actors.size();

    std::vector<flatbuffers::Offset<wfb::Plugin>> plugins;
    for (const auto& entry : order.entries()) {
        if (entry.active) {
            plugins.push_back(wfb::CreatePlugin(builder, builder.CreateString(entry.name),
                                                entry.form_prefix(), entry.is_light));
        }
    }
    const auto plugins_off = builder.CreateVector(plugins);
    const auto quests_off = builder.CreateVector(quests);
    const auto globals_off = builder.CreateVector(globals);
    const auto actors_off = builder.CreateVectorOfStructs(fb_actors);
    const auto cells_off = builder.CreateVector(cells);
    const auto bases_off = builder.CreateVector(bases);
    const auto climates_off = builder.CreateVector(climates);
    const auto weathers_off = builder.CreateVector(weathers);
    const auto precipitations_off = builder.CreateVector(precipitations);
    const auto regions_off = builder.CreateVector(regions);
    const auto worlds_off = builder.CreateVector(worlds);
    const auto waters_off = builder.CreateVector(waters);
    const auto land_textures_off = builder.CreateVector(land_textures);
    wfb::WorldBuilder wb(builder);
    wb.add_format_version(k_world_format_version);
    wb.add_worlds(worlds_off);
    wb.add_land_textures(land_textures_off);
    wb.add_waters(waters_off);
    wb.add_climates(climates_off);
    wb.add_weathers(weathers_off);
    wb.add_cells(cells_off);
    wb.add_bases(bases_off);
    wb.add_quests(quests_off);
    wb.add_globals(globals_off);
    wb.add_actors(actors_off);
    wb.add_plugins(plugins_off);
    wb.add_precipitations(precipitations_off);
    wb.add_regions(regions_off);
    wb.add_npcs(npcs_off);
    wb.add_packages(packages_off);
    wb.add_image_spaces(image_spaces_off);
    wb.add_material_objects(material_objects_off);
    wb.add_grasses(grasses_off);
    wb.add_addon_nodes(addon_nodes_off);
    wb.add_races(races_off);
    wb.add_armors(armors_off);
    wb.add_armor_addons(addons_off);
    wb.add_outfits(outfits_off);
    wb.add_leveled_lists(leveled_off);
    wfb::FinishWorldBuffer(builder, wb.Finish());

    const std::span<const std::uint8_t> buffer(builder.GetBufferPointer(), builder.GetSize());
    std::string error;
    if (!io::write_file(out, std::as_bytes(buffer), error)) {
        return std::unexpected(io::ParseError{.origin = out.string(),
                                              .offset = 0,
                                              .kind = io::ErrorKind::corrupt,
                                              .detail = "cannot write: " + error});
    }
    stats.file_bytes = buffer.size();
    return stats;
}

// ---- reading --------------------------------------------------------------

class WorldFile::Impl {
public:
    io::MappedFile mapping; ///< Empty when bytes are borrowed.
    std::span<const std::byte> bytes;
    const wfb::World* root{};
};

WorldFile::WorldFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WorldFile::WorldFile(WorldFile&&) noexcept = default;
WorldFile& WorldFile::operator=(WorldFile&&) noexcept = default;
WorldFile::~WorldFile() = default;

namespace {

io::ParseResult<const wfb::World*> verify_world(std::span<const std::byte> bytes,
                                                std::string_view origin) {
    io::SpanReader reader(bytes, origin);
    const std::span<const std::uint8_t> raw = io::as_u8(bytes);
    flatbuffers::Verifier verifier(raw.data(), raw.size());
    if (!wfb::VerifyWorldBuffer(verifier)) {
        return reader.fail(io::ErrorKind::corrupt, "not a valid world.fb");
    }
    const auto* root = wfb::GetWorld(raw.data());
    if (root->format_version() != k_world_format_version) {
        return reader.fail(io::ErrorKind::unsupported,
                           "world.fb format version " + std::to_string(root->format_version()) +
                               " is not one this build reads (it reads " +
                               std::to_string(k_world_format_version) + ")");
    }
    return root;
}

WorldCell to_cell(const wfb::Cell& c) {
    WorldCell out;
    out.id = c.id();
    if (const auto* name = c.editor_id()) {
        out.editor_id = name->str();
    }
    out.world = c.world();
    out.flags = c.flags();
    if (c.has_grid()) {
        out.grid = std::array<std::int32_t, 2>{c.grid_x(), c.grid_y()};
    }
    out.water_height = c.water_height();
    if (const auto* l = c.lighting(); l != nullptr && c.has_lighting()) {
        out.lighting = WorldCellLighting{
            .ambient = l->ambient(),
            .directional = l->directional(),
            .fog_near_color = l->fog_near_color(),
            .fog_far_color = l->fog_far_color(),
            .fog_near = l->fog_near(),
            .fog_far = l->fog_far(),
            .fog_power = l->fog_power(),
            .fog_max = l->fog_max(),
            .directional_rotation_xy = l->directional_rotation_xy(),
            .directional_rotation_z = l->directional_rotation_z(),
            .directional_fade = l->directional_fade(),
            .light_fade_begin = l->light_fade_begin(),
            .light_fade_end = l->light_fade_end(),
            .inherit = l->inherit(),
        };
    }
    if (out.lighting) {
        if (const auto* d = c.directional_ambient(); d != nullptr && d->size() >= 6) {
            for (flatbuffers::uoffset_t i = 0; i < 6; ++i) {
                out.lighting->directional_ambient[i] = d->Get(i);
            }
        }
    }
    out.lighting_template = c.lighting_template();
    out.image_space = c.image_space();
    out.persistent = c.persistent();
    out.water = c.water();
    if (const auto* refs = c.refs()) {
        out.refs.reserve(refs->size());
        for (const auto* r : *refs) {
            out.refs.push_back(WorldRef{
                .id = r->id(),
                .base = r->base(),
                .position = from_fb(r->position()),
                .rotation = from_fb(r->rotation()),
                .scale = r->scale(),
                .flags = r->flags(),
                .enable_parent = r->enable_parent(),
            });
        }
    }
    if (const auto* doors = c.doors()) {
        for (const auto* d : *doors) {
            out.doors.push_back(WorldDoor{
                .ref = d->ref(),
                .destination = d->destination(),
                .position = from_fb(d->position()),
                .rotation = from_fb(d->rotation()),
            });
        }
    }
    if (const auto* scripts = c.scripts()) {
        for (const auto* r : *scripts) {
            out.scripts.push_back(
                WorldRefScripts{.ref = r->ref(), .scripts = read_scripts(r->scripts())});
        }
    }
    if (const auto* locks = c.locks()) {
        for (const auto* l : *locks) {
            out.locks.push_back(
                WorldLock{.ref = l->ref(), .level = l->level(), .flags = l->flags(), .key = l->key()});
        }
    }
    if (const auto* links = c.links()) {
        for (const auto* l : *links) {
            out.links.push_back(
                WorldLink{.ref = l->ref(), .keyword = l->keyword(), .target = l->target()});
        }
    }
    if (const auto* parents = c.activate_parents()) {
        for (const auto* a : *parents) {
            out.activate_parents.push_back(
                WorldActivateParent{.ref = a->ref(), .parent = a->parent(), .delay = a->delay()});
        }
    }
    if (const auto* primitives = c.primitives()) {
        for (const auto* p : *primitives) {
            out.primitives.push_back(WorldPrimitive{
                .ref = p->ref(), .bounds = from_fb(p->bounds()), .type = p->type()});
        }
    }
    if (const auto* navmeshes = c.navmeshes()) {
        for (const auto* n : *navmeshes) {
            WorldNavMesh nav;
            nav.id = n->id();
            if (const auto* v = n->vertices()) {
                for (const auto* p : *v) {
                    nav.vertices.push_back(from_fb(*p));
                }
            }
            if (const auto* t = n->triangles()) {
                for (const auto* p : *t) {
                    nav.triangles.push_back({.vertices = {p->v0(), p->v1(), p->v2()},
                                             .edges = {p->e0(), p->e1(), p->e2()},
                                             .flags = p->flags(),
                                             .cover = p->cover()});
                }
            }
            if (const auto* l = n->links()) {
                for (const auto* p : *l) {
                    nav.links.push_back(
                        {.type = p->type(), .navmesh = p->navmesh(), .triangle = p->triangle()});
                }
            }
            if (const auto* d = n->doors()) {
                for (const auto* p : *d) {
                    nav.doors.push_back({.triangle = p->triangle(), .door = p->door()});
                }
            }
            out.navmeshes.push_back(std::move(nav));
        }
    }
    if (const auto* t = c.terrain()) {
        WorldTerrain terrain;
        terrain.height_offset = t->height_offset();
        if (const auto* d = t->height_deltas()) {
            terrain.height_deltas.assign(d->begin(), d->end());
        }
        if (const auto* colours = t->colours()) {
            terrain.colours.assign(colours->begin(), colours->end());
        }
        if (const auto* layers = t->layers()) {
            for (const auto* l : *layers) {
                WorldTerrainLayer layer{
                    .texture = l->texture(),
                    .quadrant = l->quadrant(),
                    .layer = l->layer(),
                    .points = {},
                    .opacity = {},
                };
                if (const auto* p = l->points()) {
                    layer.points.assign(p->begin(), p->end());
                }
                if (const auto* o = l->opacity()) {
                    layer.opacity.assign(o->begin(), o->end());
                }
                terrain.layers.push_back(std::move(layer));
            }
        }
        out.terrain = std::move(terrain);
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

} // namespace

std::vector<float> WorldTerrain::heights() const {
    std::vector<float> out(k_grid * k_grid, 0.0F);
    if (height_deltas.size() != out.size()) {
        return out;
    }
    float row = height_offset;
    for (std::size_t y = 0; y < k_grid; ++y) {
        row += static_cast<float>(height_deltas[y * k_grid]);
        float column = row;
        out[y * k_grid] = column * 8.0F;
        for (std::size_t x = 1; x < k_grid; ++x) {
            column += static_cast<float>(height_deltas[y * k_grid + x]);
            out[y * k_grid + x] = column * 8.0F;
        }
    }
    return out;
}

io::ParseResult<WorldFile> WorldFile::open(const std::filesystem::path& path) {
    auto mapping = io::MappedFile::open(path);
    if (!mapping) {
        return std::unexpected(std::move(mapping).error());
    }
    auto root = verify_world(mapping->bytes(), mapping->origin());
    if (!root) {
        return std::unexpected(std::move(root).error());
    }
    auto impl = std::make_unique<Impl>();
    impl->mapping = std::move(*mapping);
    impl->bytes = impl->mapping.bytes();
    impl->root = *root;
    return WorldFile(std::move(impl));
}

io::ParseResult<WorldFile> WorldFile::from_bytes(std::span<const std::byte> bytes,
                                                 std::string_view origin) {
    auto root = verify_world(bytes, origin);
    if (!root) {
        return std::unexpected(std::move(root).error());
    }
    auto impl = std::make_unique<Impl>();
    impl->bytes = bytes;
    impl->root = *root;
    return WorldFile(std::move(impl));
}

std::size_t WorldFile::cell_count() const noexcept {
    const auto* cells = impl_->root->cells();
    return cells == nullptr ? 0 : cells->size();
}

std::size_t WorldFile::base_count() const noexcept {
    const auto* bases = impl_->root->bases();
    return bases == nullptr ? 0 : bases->size();
}

std::optional<WorldCell> WorldFile::cell(std::uint32_t id) const {
    const auto* cells = impl_->root->cells();
    if (cells == nullptr) {
        return std::nullopt;
    }
    const auto* it = find_sorted(cells, id);
    if (it == nullptr) {
        return std::nullopt;
    }
    return to_cell(*it);
}

std::optional<WorldCell> WorldFile::cell_at(std::size_t index) const {
    const auto* cells = impl_->root->cells();
    if (cells == nullptr || index >= cells->size()) {
        return std::nullopt;
    }
    return to_cell(*cells->Get(static_cast<flatbuffers::uoffset_t>(index)));
}

std::optional<WorldCell> WorldFile::cell_by_editor_id(std::string_view editor_id) const {
    const auto* cells = impl_->root->cells();
    if (cells == nullptr) {
        return std::nullopt;
    }
    for (const auto* c : *cells) {
        const auto* name = c->editor_id();
        if (name != nullptr && iequals(name->string_view(), editor_id)) {
            return to_cell(*c);
        }
    }
    return std::nullopt;
}

std::optional<WorldBase> WorldFile::base(std::uint32_t id) const {
    const auto* bases = impl_->root->bases();
    if (bases == nullptr) {
        return std::nullopt;
    }
    const auto* it = find_sorted(bases, id);
    if (it == nullptr) {
        return std::nullopt;
    }
    WorldBase out;
    out.id = it->id();
    out.type = io::FourCC(it->type());
    if (const auto* name = it->editor_id()) {
        out.editor_id = name->str();
    }
    if (const auto* model = it->model()) {
        out.model = model->str();
    }
    if (const auto* l = it->light(); l != nullptr && it->has_light()) {
        out.light = WorldLight{
            .radius = l->radius(),
            .color = l->color(),
            .flags = l->flags(),
            .falloff_exponent = l->falloff_exponent(),
            .fov = l->fov(),
            .near_clip = l->near_clip(),
            .fade = l->fade(),
            .flicker_period = l->flicker_period(),
            .flicker_intensity = l->flicker_intensity(),
            .flicker_movement = l->flicker_movement(),
        };
    }
    out.flags = it->flags();
    out.scripts = read_scripts(it->scripts());
    out.record_flags = it->record_flags();
    return out;
}

namespace {

std::string str(const flatbuffers::String* s) { return s != nullptr ? s->str() : std::string{}; }

template <typename T>
const T* find_by_id(const flatbuffers::Vector<flatbuffers::Offset<T>>* list, std::uint32_t id) {
    if (list == nullptr) {
        return nullptr;
    }
    return find_sorted(list, id);
}

} // namespace

std::size_t WorldFile::quest_count() const noexcept {
    const auto* quests = impl_->root->quests();
    return quests == nullptr ? 0 : quests->size();
}

std::optional<WorldQuest> WorldFile::quest(std::uint32_t id) const {
    const auto* q = find_by_id(impl_->root->quests(), id);
    if (q == nullptr) {
        return std::nullopt;
    }
    WorldQuest out;
    out.id = q->id();
    out.editor_id = str(q->editor_id());
    out.name = str(q->name());
    out.flags = q->flags();
    out.priority = q->priority();
    out.type = q->type();
    out.event = q->event();
    out.scripts = read_scripts(q->scripts());
    out.fragment_script = str(q->fragment_script());
    if (const auto* fragments = q->fragments()) {
        for (const auto* f : *fragments) {
            out.fragments.push_back(WorldQuestFragment{
                .stage = f->stage(), .log_entry = f->log_entry(), .function = str(f->function())});
        }
    }
    if (const auto* stages = q->stages()) {
        for (const auto* st : *stages) {
            auto& stage = out.stages.emplace_back();
            stage.index = st->index();
            stage.flags = st->flags();
            if (const auto* log = st->log()) {
                for (const auto* e : *log) {
                    stage.log.push_back(WorldQuestLogEntry{
                        .flags = e->flags(), .text = str(e->text()), .conditions = e->conditions()});
                }
            }
        }
    }
    if (const auto* objectives = q->objectives()) {
        for (const auto* o : *objectives) {
            auto& objective = out.objectives.emplace_back();
            objective.index = o->index();
            objective.flags = o->flags();
            objective.text = str(o->text());
            if (const auto* targets = o->targets()) {
                objective.targets.assign(targets->begin(), targets->end());
            }
        }
    }
    if (const auto* aliases = q->aliases()) {
        for (const auto* a : *aliases) {
            out.aliases.push_back(WorldQuestAlias{
                .id = a->id(),
                .name = str(a->name()),
                .location = a->location(),
                .flags = a->flags(),
                .forced = a->forced(),
                .unique_actor = a->unique_actor(),
                .external_quest = a->external_quest(),
                .external_alias = a->external_alias(),
                .created_object = a->created_object(),
                .create_at = a->create_at(),
                .conditions = a->conditions(),
                .display_name = a->display_name(),
                .scripts = read_scripts(a->scripts()),
            });
        }
    }
    return out;
}

std::optional<WorldGlobal> WorldFile::global(std::uint32_t id) const {
    const auto* g = find_by_id(impl_->root->globals(), id);
    if (g == nullptr) {
        return std::nullopt;
    }
    return WorldGlobal{.id = g->id(),
                       .editor_id = str(g->editor_id()),
                       .kind = static_cast<char>(g->kind()),
                       .value = g->value()};
}

std::optional<WorldNpc> WorldFile::npc(std::uint32_t id) const {
    const auto* n = find_by_id(impl_->root->npcs(), id);
    if (n == nullptr) {
        return std::nullopt;
    }
    WorldNpc out{.id = n->id(),
                 .editor_id = str(n->editor_id()),
                 .name = str(n->name()),
                 .flags = n->flags(),
                 .level = n->level(),
                 .race = n->race(),
                 .template_form = n->template_(),
                 .template_flags = n->template_flags(),
                 .skin = n->skin(),
                 .default_outfit = n->default_outfit(),
                 .sleeping_outfit = n->sleeping_outfit(),
                 .height = n->height(),
                 .weight = n->weight(),
                 .head_parts = {},
                 .items = {},
                 .face_model = str(n->face_model())};
    if (const auto* parts = n->head_parts()) {
        out.head_parts.assign(parts->begin(), parts->end());
    }
    if (const auto* items = n->items()) {
        for (const auto* i : *items) {
            out.items.emplace_back(i->form(), i->count());
        }
    }
    if (const auto* tone = n->skin_tone(); tone != nullptr && tone->size() == 3) {
        out.skin_tone = {tone->Get(0), tone->Get(1), tone->Get(2)};
    }
    if (const auto* list = n->packages()) {
        out.packages.assign(list->begin(), list->end());
    }
    if (const auto* list = n->default_packages()) {
        out.default_packages.assign(list->begin(), list->end());
    }
    if (const auto* list = n->factions()) {
        for (const auto* f : *list) {
            out.factions.emplace_back(f->faction(), f->rank());
        }
    }
    return out;
}

namespace {

std::vector<record::Condition> read_conditions(
    const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list) {
    std::vector<record::Condition> out;
    if (list == nullptr) {
        return out;
    }
    for (const auto* c : *list) {
        out.push_back(record::Condition{.type = c->type(),
                                        .value = c->value(),
                                        .value_global = FormId{c->value_global()},
                                        .function = c->function(),
                                        .param1 = c->param1(),
                                        .param2 = c->param2(),
                                        .run_on = c->run_on(),
                                        .reference = FormId{c->reference()},
                                        .param3 = c->param3(),
                                        .string1 = str(c->string1()),
                                        .string2 = str(c->string2())});
    }
    return out;
}

} // namespace

std::size_t WorldFile::package_count() const noexcept {
    const auto* list = impl_->root->packages();
    return list != nullptr ? list->size() : 0;
}

std::optional<WorldPackage> WorldFile::package(std::uint32_t id) const {
    const auto* p = find_by_id(impl_->root->packages(), id);
    if (p == nullptr) {
        return std::nullopt;
    }
    WorldPackage out{.id = p->id(),
                     .editor_id = str(p->editor_id()),
                     .type = p->type(),
                     .flags = p->flags(),
                     .interrupt_override = p->interrupt_override(),
                     .speed = p->speed(),
                     .interrupt_flags = p->interrupt_flags(),
                     .schedule = {.month = p->month(),
                                  .day_of_week = p->day_of_week(),
                                  .date = p->date(),
                                  .hour = p->hour(),
                                  .minute = p->minute(),
                                  .duration = p->duration()},
                     .conditions = read_conditions(p->conditions()),
                     .template_package = p->template_(),
                     .idle_flags = p->idle_flags(),
                     .idle_timer = p->idle_timer(),
                     .owner_quest = p->owner_quest(),
                     .combat_style = p->combat_style(),
                     .on_begin_idle = p->on_begin_idle(),
                     .on_end_idle = p->on_end_idle(),
                     .on_change_idle = p->on_change_idle()};
    if (const auto* list = p->inputs()) {
        for (const auto* in : *list) {
            out.inputs.push_back(WorldPackage::Input{
                .key = in->key(),
                .type = str(in->type()),
                .name = str(in->name()),
                .number = in->number(),
                .location = {.type = in->location_type(),
                             .value = in->location_value(),
                             .radius = in->location_radius()},
                .target = {.type = in->target_type(),
                           .value = in->target_value(),
                           .count = in->target_count()}});
        }
    }
    if (const auto* list = p->branches()) {
        for (const auto* b : *list) {
            WorldPackage::Branch branch{.type = str(b->type()),
                                        .conditions = read_conditions(b->conditions()),
                                        .children = b->children(),
                                        .flags = b->flags(),
                                        .procedure = str(b->procedure()),
                                        .success_completes = b->success_completes(),
                                        .inputs = {},
                                        .set_flags = b->set_flags(),
                                        .clear_flags = b->clear_flags(),
                                        .speed = b->speed()};
            if (const auto* keys = b->inputs()) {
                branch.inputs.assign(keys->begin(), keys->end());
            }
            out.branches.push_back(std::move(branch));
        }
    }
    if (const auto* idles = p->idles()) {
        out.idles.assign(idles->begin(), idles->end());
    }
    return out;
}

std::optional<WorldRace> WorldFile::race(std::uint32_t id) const {
    const auto* r = find_by_id(impl_->root->races(), id);
    if (r == nullptr) {
        return std::nullopt;
    }
    WorldRace out;
    out.id = r->id();
    out.editor_id = str(r->editor_id());
    for (flatbuffers::uoffset_t i = 0; i < 2; ++i) {
        if (r->skeletons() != nullptr && i < r->skeletons()->size()) {
            out.skeletons[i] = str(r->skeletons()->Get(i));
        }
        if (r->behaviours() != nullptr && i < r->behaviours()->size()) {
            out.behaviours[i] = str(r->behaviours()->Get(i));
        }
        if (r->heights() != nullptr && i < r->heights()->size()) {
            out.heights[i] = r->heights()->Get(i);
        }
        if (r->weights() != nullptr && i < r->weights()->size()) {
            out.weights[i] = r->weights()->Get(i);
        }
    }
    out.skin = r->skin();
    out.flags = r->flags();
    out.armor_race = r->armor_race();
    if (const auto* parts = r->body_parts()) {
        for (const auto* p : *parts) {
            out.body_parts.push_back({.female = p->female(), .index = p->index(), .model = str(p->model())});
        }
    }
    if (const auto* m = r->head_parts_male()) {
        out.head_parts[0].assign(m->begin(), m->end());
    }
    if (const auto* f = r->head_parts_female()) {
        out.head_parts[1].assign(f->begin(), f->end());
    }
    return out;
}

std::optional<WorldArmorAddon> WorldFile::armor_addon(std::uint32_t id) const {
    const auto* a = find_by_id(impl_->root->armor_addons(), id);
    if (a == nullptr) {
        return std::nullopt;
    }
    WorldArmorAddon out;
    out.id = a->id();
    out.editor_id = str(a->editor_id());
    out.slots = a->slots();
    out.race = a->race();
    if (const auto* races = a->additional_races()) {
        out.additional_races.assign(races->begin(), races->end());
    }
    out.models = {str(a->male_model()), str(a->female_model())};
    out.priorities = {a->male_priority(), a->female_priority()};
    out.weight_sliders = {a->male_weight_slider(), a->female_weight_slider()};
    return out;
}

std::vector<std::pair<std::string, std::uint32_t>> WorldFile::plugins() const {
    std::vector<std::pair<std::string, std::uint32_t>> out;
    if (const auto* plugins = impl_->root->plugins()) {
        for (const auto* p : *plugins) {
            out.emplace_back(str(p->name()), p->prefix());
        }
    }
    return out;
}

std::vector<WorldActor> WorldFile::actors() const {
    std::vector<WorldActor> out;
    if (const auto* actors = impl_->root->actors()) {
        out.reserve(actors->size());
        for (const auto* a : *actors) {
            out.push_back(WorldActor{.ref = a->ref(),
                                     .base = a->base(),
                                     .cell = a->cell(),
                                     .position = from_fb(a->position()),
                                     .rotation = from_fb(a->rotation()),
                                     .flags = a->flags()});
        }
    }
    return out;
}

std::vector<Worldspace> WorldFile::worldspaces() const {
    std::vector<Worldspace> out;
    const auto* worlds = impl_->root->worlds();
    if (worlds == nullptr) {
        return out;
    }
    for (const auto* w : *worlds) {
        Worldspace ws{
            .id = w->id(),
            .editor_id = w->editor_id() != nullptr ? w->editor_id()->str() : std::string{},
            .parent = w->parent(),
            .parent_flags = w->parent_flags(),
            .flags = w->flags(),
            .defaults = std::nullopt,
            .water = w->water(),
            .climate = w->climate(),
            .bounds = {w->min_x(), w->min_y(), w->max_x(), w->max_y()},
        };
        if (w->has_defaults()) {
            ws.defaults = std::array{w->default_land_height(), w->default_water_height()};
        }
        out.push_back(std::move(ws));
    }
    return out;
}

std::optional<WorldLandTexture> WorldFile::land_texture(std::uint32_t id) const {
    const auto* ltex = impl_->root->land_textures();
    if (ltex == nullptr) {
        return std::nullopt;
    }
    const auto* it = find_sorted(ltex, id);
    if (it == nullptr) {
        return std::nullopt;
    }
    const auto str = [](const flatbuffers::String* s) { return s != nullptr ? s->str() : std::string{}; };
    return WorldLandTexture{
        .id = it->id(),
        .editor_id = str(it->editor_id()),
        .diffuse = str(it->diffuse()),
        .normal = str(it->normal()),
        .specular = it->specular(),
    };
}

std::optional<WorldWater> WorldFile::water(std::uint32_t id) const {
    const auto* waters = impl_->root->waters();
    if (waters == nullptr) {
        return std::nullopt;
    }
    const auto* it = find_sorted(waters, id);
    if (it == nullptr) {
        return std::nullopt;
    }
    WorldWater out;
    out.id = it->id();
    out.editor_id = it->editor_id() != nullptr ? it->editor_id()->str() : std::string{};
    out.opacity = it->opacity();
    out.flags = it->flags();
    out.shallow_color = it->shallow_color();
    out.deep_color = it->deep_color();
    out.reflection_color = it->reflection_color();
    out.sun_specular_power = it->sun_specular_power();
    out.reflectivity = it->reflectivity();
    out.fresnel = it->fresnel();
    out.fog_near = it->fog_near();
    out.fog_far = it->fog_far();
    out.specular_power = it->specular_power();
    out.refraction_magnitude = it->refraction_magnitude();
    out.reflection_magnitude = it->reflection_magnitude();
    if (const auto* layers = it->layers()) {
        for (flatbuffers::uoffset_t i = 0; i < layers->size() && i < 3; ++i) {
            const auto* l = layers->Get(i);
            out.layers[i] = WorldWater::Layer{.wind_direction = l->wind_direction(),
                                              .wind_speed = l->wind_speed(),
                                              .uv_scale = l->uv_scale(),
                                              .amplitude = l->amplitude()};
        }
    }
    if (const auto* noise = it->noise()) {
        for (const auto* path : *noise) {
            out.noise.push_back(path->str());
        }
    }
    return out;
}

std::optional<WorldClimate> WorldFile::climate(std::uint32_t id) const {
    const auto* climates = impl_->root->climates();
    if (climates == nullptr) {
        return std::nullopt;
    }
    const auto* it = find_sorted(climates, id);
    if (it == nullptr) {
        return std::nullopt;
    }
    WorldClimate out;
    out.id = it->id();
    out.editor_id = it->editor_id() != nullptr ? it->editor_id()->str() : std::string{};
    if (const auto* entries = it->weathers()) {
        for (const auto* e : *entries) {
            out.weathers.emplace_back(e->weather(), e->chance());
        }
    }
    out.sun = {it->sunrise_begin(), it->sunrise_end(), it->sunset_begin(), it->sunset_end()};
    out.sun_texture = it->sun_texture() != nullptr ? it->sun_texture()->str() : std::string{};
    out.sun_glare_texture =
        it->sun_glare_texture() != nullptr ? it->sun_glare_texture()->str() : std::string{};
    out.sky = it->sky() != nullptr ? it->sky()->str() : std::string{};
    out.volatility = it->volatility();
    out.moons = it->moons();
    out.phase_length = it->phase_length();
    return out;
}

std::optional<WorldImageSpace> WorldFile::image_space(std::uint32_t id) const {
    const auto* list = impl_->root->image_spaces();
    if (list == nullptr) {
        return std::nullopt;
    }
    const auto* it = find_sorted(list, id);
    if (it == nullptr) {
        return std::nullopt;
    }
    WorldImageSpace out;
    out.id = it->id();
    out.editor_id = it->editor_id() != nullptr ? it->editor_id()->str() : std::string{};
    const auto floats = [](const flatbuffers::Vector<float>* v, std::vector<float>& into) {
        if (v != nullptr) {
            into.assign(v->begin(), v->end());
        }
    };
    floats(it->hdr(), out.hdr);
    floats(it->cinematic(), out.cinematic);
    floats(it->tint(), out.tint);
    return out;
}

std::optional<WorldWeather> WorldFile::weather(std::uint32_t id) const {
    const auto* weathers = impl_->root->weathers();
    if (weathers == nullptr) {
        return std::nullopt;
    }
    const auto* it = find_sorted(weathers, id);
    if (it == nullptr) {
        return std::nullopt;
    }
    WorldWeather out;
    out.id = it->id();
    out.editor_id = it->editor_id() != nullptr ? it->editor_id()->str() : std::string{};
    if (const auto* c = it->colors()) {
        out.colors.assign(c->begin(), c->end());
    }
    if (const auto* f = it->fog()) {
        out.fog.assign(f->begin(), f->end());
    }
    if (const auto* d = it->directional_ambient()) {
        out.directional_ambient.assign(d->begin(), d->end());
    }
    if (const auto* i = it->image_spaces()) {
        out.image_spaces.assign(i->begin(), i->end());
    }
    const auto str = [](const flatbuffers::String* t) { return t != nullptr ? t->str() : std::string{}; };
    if (const auto* clouds = it->clouds()) {
        for (const auto* c : *clouds) {
            WorldCloudLayer layer;
            layer.texture = str(c->texture());
            layer.speed_x = c->speed_x();
            layer.speed_y = c->speed_y();
            for (flatbuffers::uoffset_t t = 0; t < 4; ++t) {
                if (c->colors() != nullptr && t < c->colors()->size()) {
                    layer.colors[t] = c->colors()->Get(t);
                }
                if (c->alphas() != nullptr && t < c->alphas()->size()) {
                    layer.alphas[t] = c->alphas()->Get(t);
                }
            }
            layer.enabled = c->enabled();
            out.clouds.push_back(std::move(layer));
        }
    }
    out.wind_speed = it->wind_speed();
    out.wind_direction = it->wind_direction();
    out.wind_direction_range = it->wind_direction_range();
    out.transition_delta = it->transition_delta();
    out.sun_glare = it->sun_glare();
    out.sun_damage = it->sun_damage();
    out.precipitation_begin = it->precipitation_begin();
    out.precipitation_end = it->precipitation_end();
    out.thunder_begin = it->thunder_begin();
    out.thunder_end = it->thunder_end();
    out.thunder_frequency = it->thunder_frequency();
    out.classification = it->classification();
    out.lightning_color = it->lightning_color();
    out.precipitation = it->precipitation();
    out.aurora = str(it->aurora());
    return out;
}

std::optional<WorldPrecipitation> WorldFile::precipitation(std::uint32_t id) const {
    const auto* it = find_sorted(impl_->root->precipitations(), id);
    if (it == nullptr) {
        return std::nullopt;
    }
    const auto str = [](const flatbuffers::String* t) { return t != nullptr ? t->str() : std::string{}; };
    return WorldPrecipitation{
        .id = it->id(),
        .editor_id = str(it->editor_id()),
        .texture = str(it->texture()),
        .gravity_velocity = it->gravity_velocity(),
        .rotation_velocity = it->rotation_velocity(),
        .size_x = it->size_x(),
        .size_y = it->size_y(),
        .center_offset_min = it->center_offset_min(),
        .center_offset_max = it->center_offset_max(),
        .rotation_range = it->rotation_range(),
        .subtextures_x = it->subtextures_x(),
        .subtextures_y = it->subtextures_y(),
        .type = it->type(),
        .box_size = it->box_size(),
        .density = it->density(),
    };
}

std::vector<WorldRegion> WorldFile::regions() const {
    std::vector<WorldRegion> out;
    const auto* regions = impl_->root->regions();
    if (regions == nullptr) {
        return out;
    }
    for (const auto* r : *regions) {
        WorldRegion region;
        region.id = r->id();
        region.editor_id = r->editor_id() != nullptr ? r->editor_id()->str() : std::string{};
        region.world = r->world();
        if (const auto* areas = r->areas()) {
            for (const auto* a : *areas) {
                region.areas.emplace_back();
                if (a->points() != nullptr) {
                    region.areas.back().assign(a->points()->begin(), a->points()->end());
                }
            }
        }
        if (const auto* weathers = r->weathers()) {
            for (const auto* w : *weathers) {
                region.weathers.push_back(
                    {.weather = w->weather(), .chance = w->chance(), .global = w->global()});
            }
        }
        region.weather_priority = r->weather_priority();
        region.weather_override = r->weather_override();
        out.push_back(std::move(region));
    }
    return out;
}

std::optional<WorldCell> WorldFile::cell_at_grid(std::uint32_t world, std::int32_t x,
                                                 std::int32_t y) const {
    const auto* cells = impl_->root->cells();
    if (cells == nullptr) {
        return std::nullopt;
    }
    for (const auto* c : *cells) {
        if (c->world() == world && c->has_grid() && c->grid_x() == x && c->grid_y() == y &&
            (c->flags() & 0x1u) == 0 && !c->persistent()) {
            return to_cell(*c);
        }
    }
    return std::nullopt;
}

} // namespace bethconv::pack
