// SPDX-License-Identifier: GPL-3.0-or-later
#include "build/decoration.hpp"

#include "assets/model.hpp"
#include "render/animator.hpp"
#include "render/billboard.hpp"
#include "physics/collision.hpp"
#include "render/effect_asset.hpp"
#include "data/fb_search.hpp"
#include "build/refs.hpp"
#include "data/text.hpp"

#include "world_generated.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/core/object.hpp>

#include <string_view>

using godot::Dictionary;
using godot::String;
using godot::Transform3D;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

/// The game places a model by its reference alone: whatever transform the
/// NIF's root node carries is replaced (Riverwood Trader's corner counter
/// piece has its root turned 90 degrees and lines up only without it).
void drop_root_transform(godot::Node3D* model) {
    auto* axes = godot::Object::cast_to<godot::Node3D>(model->get_node_or_null("bethconv_z_up_to_y_up"));
    if (axes == nullptr || axes->get_child_count() == 0) {
        return;
    }
    if (auto* nif_root = godot::Object::cast_to<godot::Node3D>(axes->get_child(0))) {
        nif_root->set_transform(Transform3D());
    }
}

/// Metres a BSOrderedNode's child is moved forward per place in its order
/// when transparent surfaces are sorted: more than the depth between a
/// flask's glass and the liquid in it.
constexpr godot::real_t k_draw_order_step = 0.25F;

void offset_sorting(godot::Node* node, godot::real_t offset, int depth) {
    if (depth > 64) {
        return;
    }
    // A nested ordered node orders its own children within this place.
    const godot::Variant order = bethconv_extras(node).get("draw_order", godot::Variant());
    if (order.get_type() == godot::Variant::INT || order.get_type() == godot::Variant::FLOAT) {
        offset += static_cast<godot::real_t>(static_cast<double>(order)) * k_draw_order_step;
    }
    if (auto* visual = godot::Object::cast_to<godot::VisualInstance3D>(node); visual != nullptr && offset != 0) {
        visual->set_sorting_offset(offset);
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        offset_sorting(node->get_child(i), offset, depth + 1);
    }
}

/// The game draws a BSOrderedNode's children in their order (glass, the
/// liquid in it, the glass around it), not by depth.
void apply_draw_order(godot::Node3D* model) { offset_sorting(model, 0, 0); }

} // namespace

// ---- the steps ------------------------------------------------------------

struct Decorator::Steps {
    using Step = void (*)(Decorator& self, godot::Node3D* node, Decoration& d);
    struct Entry {
        Step run;
        /// Whether an add-on's model takes the step too.
        bool for_addon;
        /// Whether a visual-only model takes it.
        bool visual;
    };

    static void drop_root(Decorator&, godot::Node3D* node, Decoration&) { drop_root_transform(node); }
    static void draw_order(Decorator&, godot::Node3D* node, Decoration&) { apply_draw_order(node); }
    static void skyrim_materials(Decorator& self, godot::Node3D* node, Decoration& d) {
        if (self.options_.skyrim_materials) {
            d.stats.materials += self.materials().apply(node);
        }
    }
    static void water(Decorator& self, godot::Node3D* model, Decoration& d);
    static void directional(Decorator& self, godot::Node3D* model, Decoration& d);
    static void addons(Decorator& self, godot::Node3D* node, Decoration& d) {
        if (self.options_.skyrim_materials && self.options_.effects) {
            d.stats.effects += self.attach_addons(node);
        }
    }
    static void billboards(Decorator&, godot::Node3D* node, Decoration& d) {
        d.stats.billboards += SkydotBillboard::attach(node);
    }
    static void animators(Decorator& self, godot::Node3D* node, Decoration& d) {
        if (self.options_.effects) {
            (void)self.materials(); // The animators take the Ref, so it must exist.
            d.stats.effects += SkydotAnimator::attach(node, self.materials_);
        }
    }
    static void tag(Decorator& self, godot::Node3D* node, Decoration& d) {
        tag_ref(node, d.ref->id(), d.cell, activatable(*self.data_, d.base, d.cell, d.ref->id()));
    }
    static void plain_door(Decorator& self, godot::Node3D* node, Decoration& d) {
        // Doors that swing rather than lead somewhere: actors open them in
        // their way (SkydotActor).
        if (d.base != nullptr && door_type(d.base->type()) && !self.data_->doors().contains(d.ref->id())) {
            node->set_meta("skydot_plain_door", true);
        }
    }
    static void collision(Decorator& self, godot::Node3D* node, Decoration& d) {
        if (const auto& body = d.scene->collision(); body && self.options_.collision) {
            d.stats.bodies += body->attach(node);
        }
    }
};

/// Surfaces of a placed model with a water shader get the water material of
/// the cell's water type.
void Decorator::Steps::water(Decorator& self, godot::Node3D* model, Decoration& d) {
    if (!self.options_.skyrim_materials) {
        return;
    }
    const std::uint32_t cell = d.cell;
    const WorldData& data = *self.data_;
    godot::TypedArray<godot::Node> meshes = model->find_children("*", "MeshInstance3D", true, false);
    godot::Ref<godot::ShaderMaterial> water;
    for (int i = 0; i < meshes.size(); ++i) {
        auto* instance = godot::Object::cast_to<godot::MeshInstance3D>(meshes[i]);
        if (instance == nullptr || instance->get_mesh().is_null()) {
            continue;
        }
        for (int s = 0; s < instance->get_mesh()->get_surface_count(); ++s) {
            const godot::Ref<godot::Material> m = instance->get_surface_override_material(s);
            if (m.is_null() || !m->has_meta("skydot_water")) {
                continue;
            }
            if (water.is_null()) {
                // The activator's own water type (ACTI WNAM) is not in
                // world.fb yet: the cell's, else its worldspace's.
                const auto space = static_cast<std::uint32_t>(data.cell_space(cell));
                water = self.water_material(data.water_ptr(data.water_type(space, data.cell_ptr(cell))));
            }
            instance->set_surface_override_material(s, water);
        }
    }
}

/// Surfaces of a placed model with the Projected UV flag get its base's
/// directional material (STAT DNAM), if it has one.
void Decorator::Steps::directional(Decorator& self, godot::Node3D* model, Decoration& d) {
    if (!self.options_.skyrim_materials) {
        return;
    }
    const wfb::Base& base = *d.base;
    if (base.directional_material() == 0) {
        return;
    }
    const ProjectedMaterial* with = self.projected_material(base.directional_material());
    if (with == nullptr) {
        return;
    }
    const godot::TypedArray<godot::Node> meshes = model->find_children("*", "MeshInstance3D", true, false);
    for (int64_t i = 0; i < meshes.size(); ++i) {
        auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(meshes[i]);
        if (mesh == nullptr || mesh->get_mesh().is_null()) {
            continue;
        }
        for (int s = 0; s < mesh->get_mesh()->get_surface_count(); ++s) {
            const godot::Ref<godot::ShaderMaterial> current = mesh->get_surface_override_material(s);
            const godot::Ref<godot::ShaderMaterial> replaced =
                self.materials().projected(current, base.directional_material(), *with);
            if (replaced != current) {
                mesh->set_surface_override_material(s, replaced);
            }
        }
    }
}

// ---- the decorator --------------------------------------------------------

Decorator::Decorator(std::shared_ptr<const WorldData> data, const BuildOptions& options)
    : data_(std::move(data)), options_(options) {}

void Decorator::open(std::shared_ptr<const WorldData> data) { data_ = std::move(data); }

void Decorator::set_assets(std::shared_ptr<AssetCache> assets) { assets_ = std::move(assets); }

void Decorator::decorate(godot::Node3D* node, Decoration& decoration) {
    // The one order. Add-on models take the steps marked true. Collision is
    // last, so material and effect passes never see the bodies.
    static constexpr Steps::Entry k_steps[] = {
        {Steps::drop_root, true, true},
        {Steps::draw_order, true, true},
        {Steps::skyrim_materials, true, true},
        {Steps::water, false, false},
        {Steps::directional, false, true},
        {Steps::addons, false, false},
        {Steps::billboards, true, true},
        {Steps::animators, true, true},
        {Steps::tag, false, false},
        {Steps::plain_door, false, false},
        {Steps::collision, false, false},
    };
    for (const auto& step : k_steps) {
        if (decoration.visual_only) {
            if (step.visual) {
                step.run(*this, node, decoration);
            }
        } else if (step.for_addon || decoration.ref != nullptr) {
            step.run(*this, node, decoration);
        }
    }
}

godot::Ref<godot::Resource> Decorator::resource(const String& vpath) const {
    return assets_ != nullptr ? assets_->get(utf8(vpath)) : godot::Ref<godot::Resource>();
}

SkydotMaterials& Decorator::materials() {
    if (materials_.is_null()) {
        materials_.instantiate();
        materials_->set_assets(assets_);
    }
    return *materials_.ptr();
}

godot::Ref<godot::ShaderMaterial> Decorator::water_material(const wfb::Water* water) {
    const auto load = [&](const std::string& vpath) -> godot::Ref<godot::Texture> {
        return resource(String::utf8(vpath.c_str()));
    };
    return water_.material(water, load);
}

std::int64_t Decorator::warm_up() {
    water_.warm_up();
    return materials().warm_up();
}

const ProjectedMaterial* Decorator::projected_material(std::uint32_t id) {
    const std::scoped_lock lock(projected_mutex_);
    if (auto it = projected_.find(id); it != projected_.end()) {
        return it->second ? &*it->second : nullptr;
    }
    const auto* list = data_->root() != nullptr ? data_->root()->material_objects() : nullptr;
    const auto* mato = lookup(list, id);
    if (mato == nullptr) {
        projected_.emplace(id, std::nullopt);
        return nullptr;
    }
    ProjectedMaterial out;
    // Single pass materials (the common snow) take the engine's projected
    // textures (noise, diffuse, normal and its detail) and the material's
    // colour; the noise is as large as the material says, in game units.
    // The others use the albedo of their model's first shape.
    if (mato->single_pass() && assets_ != nullptr) {
        out.snow_noise = assets_->texture("textures/effects/projectednoise.dds");
        out.snow_diffuse = assets_->texture("textures/effects/projecteddiffuse.dds");
        out.snow_normal = assets_->texture("textures/effects/projectednormal.dds");
        out.snow_detail = assets_->texture("textures/effects/projectednormaldetail.dds");
        out.noise_units = mato->noise_uv_scale() > 0.0F ? mato->noise_uv_scale() : 50.0F;
    }
    if (const auto* model = mato->model(); !mato->single_pass() && model != nullptr && model->size() != 0) {
        const godot::Ref<SkydotModel> scene = resource(model_path(model->string_view()));
        if (scene.is_valid()) {
            godot::Node* node = scene->instantiate();
            const godot::TypedArray<godot::Node> meshes = node != nullptr
                ? node->find_children("*", "MeshInstance3D", true, false)
                : godot::TypedArray<godot::Node>();
            for (int64_t i = 0; i < meshes.size() && out.albedo.is_null(); ++i) {
                auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(meshes[i]);
                if (mesh == nullptr || mesh->get_mesh().is_null() || mesh->get_mesh()->get_surface_count() == 0) {
                    continue;
                }
                const godot::Ref<godot::ShaderMaterial> converted =
                    materials().convert(mesh->get_mesh()->surface_get_material(0));
                if (converted.is_valid()) {
                    out.albedo = converted->get_shader_parameter("albedo_tex");
                }
            }
            if (node != nullptr) {
                memdelete(node);
            }
        }
    }
    const auto scale = static_cast<float>(formats::k_metres_per_unit);
    const auto per_metre = [&](float units) { return units > 0.0F ? 1.0F / (units * scale) : 0.0F; };
    out.params = godot::Vector4(mato->falloff_scale(), mato->falloff_bias(), per_metre(mato->noise_uv_scale()),
                                per_metre(mato->material_uv_scale()));
    // Projected along the vector: faces turned against it take the material
    // (snow's is straight down).
    if (const auto* p = mato->projection(); p != nullptr && p->size() >= 3) {
        const Vector3 game(-p->Get(0), -p->Get(1), -p->Get(2));
        const Vector3 world(game.x, game.z, -game.y);
        if (world.length() > 0.001F) {
            out.direction = world.normalized();
        }
    }
    Vector3 colour(1, 1, 1);
    if (const auto* c = mato->single_pass_color(); c != nullptr && c->size() >= 3 &&
                                                   (c->Get(0) > 0.0F || c->Get(1) > 0.0F || c->Get(2) > 0.0F)) {
        colour = Vector3(c->Get(0), c->Get(1), c->Get(2));
    }
    out.color = colour;
    out.normal_dampener = mato->normal_dampener();
    return &*projected_.emplace(id, out).first->second;
}

std::int64_t Decorator::attach_addons(godot::Node* model) {
    std::call_once(addon_index_once_, [this] {
        if (const auto* list = data_->root() != nullptr ? data_->root()->addon_nodes() : nullptr) {
            for (const auto* a : *list) {
                if (a->model() != nullptr && a->model()->size() != 0) {
                    addon_models_.emplace(a->index(), a->model()->str());
                }
            }
        }
    });
    if (addon_models_.empty()) {
        return 0;
    }
    std::int64_t attached = 0;
    const godot::TypedArray<godot::Node> nodes = model->find_children("AddOnNode*", "Node3D", true, false);
    for (int64_t i = 0; i < nodes.size(); ++i) {
        auto* node = godot::Object::cast_to<godot::Node3D>(nodes[i]);
        if (node == nullptr || !node->has_meta("extras")) {
            continue;
        }
        const godot::Variant extras = node->get_meta("extras");
        const godot::Variant block = extras.get_type() == godot::Variant::DICTIONARY ? Dictionary(extras).get("bethconv", godot::Variant())
                                                                       : godot::Variant();
        const godot::Variant index = block.get_type() == godot::Variant::DICTIONARY ? Dictionary(block).get("addon", godot::Variant())
                                                                      : godot::Variant();
        if (index.get_type() != godot::Variant::INT && index.get_type() != godot::Variant::FLOAT) {
            continue;
        }
        const auto found = addon_models_.find(static_cast<std::int32_t>(static_cast<std::int64_t>(index)));
        if (found == addon_models_.end()) {
            continue;
        }
        const godot::Ref<SkydotModel> scene = resource(model_path(found->second));
        auto* addon = scene.is_valid() ? godot::Object::cast_to<godot::Node3D>(scene->instantiate()) : nullptr;
        if (addon == nullptr) {
            continue;
        }
        addon->set_name("AddOn");
        // What the steps count for an add-on is not counted for the model.
        DecorationStats unused;
        Decoration inner{unused};
        decorate(addon, inner);
        // The AddOnNode sits in the model's NIF space (under its axis and
        // unit conversion), and the addon brings its own conversion: hang it
        // from the model's root where the AddOnNode is, without converting
        // twice.
        godot::Transform3D at;
        for (godot::Node* n = node; n != nullptr && n != model; n = n->get_parent()) {
            if (auto* spatial = godot::Object::cast_to<godot::Node3D>(n)) {
                at = spatial->get_transform() * at;
            }
        }
        if (auto* own = godot::Object::cast_to<godot::Node3D>(addon->get_node_or_null("bethconv_z_up_to_y_up"))) {
            at = at * own->get_transform().affine_inverse();
        }
        addon->set_transform(at);
        model->add_child(addon);
        ++attached;
    }
    return attached;
}

} // namespace skydot
