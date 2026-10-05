// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/world.hpp"

#include "bethconv/io/byte_view.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/record/types.hpp"

#include "bethconv/pack/world_generated.h"
#include "skydot_formats/flags.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace bethconv::pack {
namespace {

using record::FormId;

/// The entry of a vector whose `(key)` is `id` (the schema marks them), or
/// null; also for a file without the vector.
template <typename T>
const T* lookup(const flatbuffers::Vector<flatbuffers::Offset<T>>* list, std::uint32_t id) {
    return list != nullptr ? list->LookupByKey(id) : nullptr;
}

/// `table` copied out as its object type, or nothing for null.
template <typename T>
std::optional<typename T::NativeTableType> unpack(const T* table) {
    if (table == nullptr) {
        return std::nullopt;
    }
    std::optional<typename T::NativeTableType> out(std::in_place);
    table->UnPackTo(&*out);
    return out;
}

/// Every table of `list` copied out as its object type.
template <typename T>
std::vector<typename T::NativeTableType> unpack_all(
    const flatbuffers::Vector<flatbuffers::Offset<T>>* list) {
    std::vector<typename T::NativeTableType> out;
    if (list != nullptr) {
        out.reserve(list->size());
        for (const auto* table : *list) {
            table->UnPackTo(&out.emplace_back());
        }
    }
    return out;
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
        script.status = static_cast<std::uint8_t>(s->status());
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

record::Vec3 from_fb(const wfb::Vec3f& v) { return record::Vec3{v.x(), v.y(), v.z()}; }

} // namespace

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
    const auto* it = lookup(impl_->root->cells(), id);
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
    const auto* it = lookup(impl_->root->bases(), id);
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

std::size_t WorldFile::quest_count() const noexcept {
    const auto* quests = impl_->root->quests();
    return quests == nullptr ? 0 : quests->size();
}

std::optional<wfb::QuestT> WorldFile::quest(std::uint32_t id) const {
    return unpack(lookup(impl_->root->quests(), id));
}

std::optional<wfb::GlobalT> WorldFile::global(std::uint32_t id) const {
    return unpack(lookup(impl_->root->globals(), id));
}

std::optional<wfb::NpcT> WorldFile::npc(std::uint32_t id) const {
    return unpack(lookup(impl_->root->npcs(), id));
}

std::size_t WorldFile::package_count() const noexcept {
    const auto* list = impl_->root->packages();
    return list != nullptr ? list->size() : 0;
}

std::optional<wfb::PackageT> WorldFile::package(std::uint32_t id) const {
    return unpack(lookup(impl_->root->packages(), id));
}

std::optional<wfb::RaceT> WorldFile::race(std::uint32_t id) const {
    return unpack(lookup(impl_->root->races(), id));
}

std::optional<wfb::ArmorAddonT> WorldFile::armor_addon(std::uint32_t id) const {
    return unpack(lookup(impl_->root->armor_addons(), id));
}

std::vector<wfb::PluginT> WorldFile::plugins() const { return unpack_all(impl_->root->plugins()); }

std::vector<wfb::ActorRef> WorldFile::actors() const {
    std::vector<wfb::ActorRef> out;
    if (const auto* actors = impl_->root->actors()) {
        out.reserve(actors->size());
        for (const auto* a : *actors) {
            out.push_back(*a);
        }
    }
    return out;
}

std::vector<wfb::WorldspaceT> WorldFile::worldspaces() const {
    return unpack_all(impl_->root->worlds());
}

std::optional<wfb::LandTextureT> WorldFile::land_texture(std::uint32_t id) const {
    return unpack(lookup(impl_->root->land_textures(), id));
}

std::optional<wfb::WaterT> WorldFile::water(std::uint32_t id) const {
    return unpack(lookup(impl_->root->waters(), id));
}

std::optional<wfb::ClimateT> WorldFile::climate(std::uint32_t id) const {
    return unpack(lookup(impl_->root->climates(), id));
}

std::optional<wfb::ImageSpaceT> WorldFile::image_space(std::uint32_t id) const {
    return unpack(lookup(impl_->root->image_spaces(), id));
}

std::optional<wfb::WeatherT> WorldFile::weather(std::uint32_t id) const {
    return unpack(lookup(impl_->root->weathers(), id));
}

std::optional<wfb::PrecipitationT> WorldFile::precipitation(std::uint32_t id) const {
    return unpack(lookup(impl_->root->precipitations(), id));
}

std::vector<wfb::RegionT> WorldFile::regions() const { return unpack_all(impl_->root->regions()); }

std::optional<WorldCell> WorldFile::cell_at_grid(std::uint32_t world, std::int32_t x,
                                                 std::int32_t y) const {
    const auto* cells = impl_->root->cells();
    if (cells == nullptr) {
        return std::nullopt;
    }
    for (const auto* c : *cells) {
        if (c->world() == world && c->has_grid() && c->grid_x() == x && c->grid_y() == y &&
            !skydot::formats::has_flag(c->flags(), wfb::CellFlags::interior) &&
            !c->persistent()) {
            return to_cell(*c);
        }
    }
    return std::nullopt;
}

} // namespace bethconv::pack
