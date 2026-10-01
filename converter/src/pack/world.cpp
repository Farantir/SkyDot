// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/world.hpp"

#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_stream.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_game.hpp"
#include "bethconv/record/forms_object.hpp"
#include "bethconv/record/forms_world.hpp"
#include "bethconv/record/types.hpp"

#include "bethconv/pack/world_generated.h"

#include <algorithm>
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
    bool persistent{};
    std::uint32_t water{};
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
};

struct BaseEntry {
    std::uint32_t id{};
    std::uint32_t type{};
    std::string editor_id;
    std::string model;
    std::optional<WorldLight> light;
    std::uint32_t flags{};
    std::vector<record::Script> scripts;
};

/// Per-cell data about its references beyond placement.
struct CellExtras {
    std::vector<WorldRefScripts> scripts;
    std::vector<WorldLock> locks;
    std::vector<WorldLink> links;
    std::vector<WorldActivateParent> activate_parents;
    std::vector<WorldPrimitive> primitives;
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
    (void)r.skip(24 + 4 + 4); // directional ambient, specular, fresnel power
    out.fog_far_color = u32();
    out.fog_max = f32();
    out.light_fade_begin = f32();
    out.light_fade_end = f32();
    out.inherit = u32();
    return out;
}

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
        } else if (merged.type == FourCC{"QUST"}) {
            on_quest(merged, data, form_ctx);
        } else if (merged.type == FourCC{"GLOB"}) {
            on_global(merged, data, form_ctx);
        } else if (merged.type == FourCC{"ACHR"}) {
            on_actor(merged, ctx, data, form_ctx);
        } else if (merged.type != FourCC{"NAVM"} &&
                   merged.type != FourCC{"LAND"} && merged.type != FourCC{"INFO"}) {
            on_other(merged, data);
        }
    }

    [[nodiscard]] WorldStats& stats() noexcept { return stats_; }
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
    [[nodiscard]] std::unordered_map<std::uint32_t, TextureSetEntry>& texture_sets() noexcept {
        return texture_sets_;
    }
    [[nodiscard]] std::unordered_map<std::uint32_t, CellExtras>& extras() noexcept {
        return extras_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldQuest>& quests() noexcept { return quests_; }
    [[nodiscard]] std::map<std::uint32_t, WorldGlobal>& globals() noexcept { return globals_; }
    [[nodiscard]] std::vector<WorldActor>& actors() noexcept { return actors_; }

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
        if (failed) {
            ++stats_.unresolved;
        }
        actors_.push_back(out);
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
        const auto walked = record::for_each_field(
            data, [&](const record::FieldHeader& field, io::SpanReader& body) {
                if (field.type == FourCC{"EDID"} && editor_id.empty()) {
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
        };
        if (failed) {
            ++stats_.unresolved;
        }
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
        land_textures_[merged.form.value] = LandTextureEntry{
            .id = merged.form.value,
            .editor_id = ltex->editor_id,
            .texture_set = global(merged, ltex->texture_set, failed),
            .specular = ltex->specular,
        };
        if (failed) {
            ++stats_.unresolved;
        }
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
        weathers_[out.id] = std::move(out);
    }

    const record::LoadOrder& order_;
    WorldStats stats_;
    std::map<std::uint32_t, WorldWater> waters_;
    std::map<std::uint32_t, WorldClimate> climates_;
    std::map<std::uint32_t, WorldWeather> weathers_;
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

        std::optional<wfb::CellLighting> lighting;
        if (cell.lighting) {
            const auto& l = *cell.lighting;
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
            ltex.specular));
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
        climates.push_back(wfb::CreateClimate(builder, c.id, builder.CreateString(c.editor_id),
                                              builder.CreateVectorOfStructs(entries), c.sun[0],
                                              c.sun[1], c.sun[2], c.sun[3]));
        ++stats.climates;
    }
    std::vector<flatbuffers::Offset<wfb::Weather>> weathers;
    for (const auto& [id, w] : sink.weathers()) {
        weathers.push_back(wfb::CreateWeather(
            builder, w.id, builder.CreateString(w.editor_id), builder.CreateVector(w.colors),
            builder.CreateVector(w.fog), builder.CreateVector(w.directional_ambient)));
        ++stats.weathers;
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
    const std::span<const std::uint8_t> raw(
        static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data())), bytes.size());
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
    out.lighting_template = c.lighting_template();
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
