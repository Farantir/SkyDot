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

namespace {

std::string str(const flatbuffers::String* s) { return s != nullptr ? s->str() : std::string{}; }

} // namespace

std::size_t WorldFile::quest_count() const noexcept {
    const auto* quests = impl_->root->quests();
    return quests == nullptr ? 0 : quests->size();
}

std::optional<WorldQuest> WorldFile::quest(std::uint32_t id) const {
    const auto* q = lookup(impl_->root->quests(), id);
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

std::optional<wfb::GlobalT> WorldFile::global(std::uint32_t id) const {
    return unpack(lookup(impl_->root->globals(), id));
}

std::optional<WorldNpc> WorldFile::npc(std::uint32_t id) const {
    const auto* n = lookup(impl_->root->npcs(), id);
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
    const auto* p = lookup(impl_->root->packages(), id);
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
    const auto* r = lookup(impl_->root->races(), id);
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
    const auto* a = lookup(impl_->root->armor_addons(), id);
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
    const auto* it = lookup(impl_->root->land_textures(), id);
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
    const auto* it = lookup(impl_->root->waters(), id);
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
    const auto* it = lookup(impl_->root->climates(), id);
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
    const auto* it = lookup(impl_->root->image_spaces(), id);
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
    const auto* it = lookup(impl_->root->weathers(), id);
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
    const auto* it = lookup(impl_->root->precipitations(), id);
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
            !skydot::formats::has_flag(c->flags(), wfb::CellFlags::interior) &&
            !c->persistent()) {
            return to_cell(*c);
        }
    }
    return std::nullopt;
}

} // namespace bethconv::pack
