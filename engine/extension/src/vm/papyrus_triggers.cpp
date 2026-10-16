// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus: the binding of trigger volumes. The volumes, who is inside
// and the saves are vm/triggers.hpp's; this builds them from the world's
// records and the built nodes' phantoms, and sends the events.
#include "vm/papyrus.hpp"

#include "data/coordinates.hpp"
#include "physics/collision.hpp"

#include <godot_cpp/classes/node3d.hpp>

using godot::Array;
using godot::Dictionary;
using godot::Variant;
using godot::Vector3;

namespace skydot {

namespace {

vm::Vec3 vm_vec(const Vector3& v) { return {v.x, v.y, v.z}; }

vm::Affine affine_of(const godot::Basis& basis, const Vector3& origin) {
    vm::Affine out;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out.m[i][j] = basis.rows[i][j];
        }
    }
    out.origin = vm_vec(origin);
    return out;
}

/// Skyrim space (game units) to Godot space (metres) as a transform, so a
/// volume placed in Godot space can be asked about a Skyrim position.
godot::Transform3D skyrim_to_godot() {
    const Vector3 origin = skyrim_position(Vector3());
    const godot::Basis axes(skyrim_position(Vector3(1, 0, 0)) - origin, skyrim_position(Vector3(0, 1, 0)) - origin,
                            skyrim_position(Vector3(0, 0, 1)) - origin);
    return godot::Transform3D(axes, origin);
}

} // namespace

// A box's bounds are half extents: xEdit shows XPRM bounds doubled. A sphere's
// radius is its x. Neither is checked against the game yet.
void SkydotPapyrus::register_trigger(std::uint32_t ref, const Dictionary& info) {
    const Variant primitive = info.get("primitive", Variant());
    if (primitive.get_type() != Variant::DICTIONARY) {
        return;
    }
    const Dictionary p = primitive;
    const auto type = form_of(p["type"]);
    if (type != vm::Trigger::k_box && type != vm::Trigger::k_sphere) {
        return;
    }
    const Vector3 r = info.get("rotation", Vector3());
    // As SkydotWorld::skyrim_transform: clockwise angles, Z first, then Y, X.
    const godot::Basis rx(Vector3(1, 0, 0), -r.x);
    const godot::Basis ry(Vector3(0, 1, 0), -r.y);
    const godot::Basis rz(Vector3(0, 0, 1), -r.z);
    const auto scale = static_cast<godot::real_t>(static_cast<double>(info.get("scale", 1.0)));
    vm::Trigger t;
    t.origin = vm_vec(info.get("position", Vector3()));
    t.axes = affine_of((rx * ry * rz).transposed(), Vector3());
    t.half = vm_vec(Vector3(p["bounds"]) * scale);
    t.type = type;
    triggers_.set_primitive(ref, std::move(t));
}

// The model's phantoms (see physics/collision.hpp) become volumes of the ref
// that carries the model. `node` is the built reference; its transform is the
// placement, like an XPRM trigger's.
void SkydotPapyrus::register_phantoms(std::uint32_t ref, godot::Node3D* node) {
    if (node == nullptr || !node->has_meta(k_phantoms_meta)) {
        return;
    }
    const Variant meta = node->get_meta(k_phantoms_meta);
    if (meta.get_type() != Variant::ARRAY) {
        return;
    }
    const godot::Transform3D to_godot = skyrim_to_godot();
    std::vector<vm::Volume> volumes;
    const godot::Array list = meta;
    for (std::int64_t i = 0; i < list.size(); ++i) {
        if (list[i].get_type() != Variant::DICTIONARY) {
            continue;
        }
        const Dictionary d = list[i];
        const godot::Transform3D local = (node->get_transform() * godot::Transform3D(d["transform"])).affine_inverse();
        const godot::Transform3D from_skyrim = local * to_godot;
        vm::Volume v;
        v.to_local = affine_of(from_skyrim.basis, from_skyrim.origin);
        v.half = vm_vec(d["half"]);
        v.sphere = godot::String(d["kind"]) == "sphere";
        volumes.push_back(v);
    }
    triggers_.set_volumes(ref, std::move(volumes));
}

void SkydotPapyrus::update_actor(std::int64_t actor, const Vector3& position) {
    const auto form = static_cast<std::uint32_t>(actor);
    triggers_.move_actor(
        form, vm_vec(position),
        // A disabled trigger notices no one (the E3 demo's set-stage boxes on
        // the way to Bleak Falls would start its quest and swap the barrow door).
        [this](std::uint32_t trigger) { return !disabled(trigger); },
        [this, actor, form](std::uint32_t trigger, bool entered) {
            emit_signal("trigger", static_cast<std::int64_t>(trigger), actor, entered);
            dispatch(trigger, entered ? "OnTriggerEnter" : "OnTriggerLeave", {vm_->object(form, "ObjectReference")});
        });
}

std::int64_t SkydotPapyrus::get_trigger_count(std::int64_t ref) const {
    return static_cast<std::int64_t>(triggers_.inside_count(static_cast<std::uint32_t>(ref)));
}

Array SkydotPapyrus::get_triggers() const {
    Array out;
    for (const auto& [ref, t] : triggers_.triggers()) {
        Dictionary entry;
        entry["ref"] = static_cast<std::int64_t>(ref);
        entry["type"] = static_cast<std::int64_t>(t.type);
        entry["origin"] = Vector3(t.origin.x, t.origin.y, t.origin.z);
        entry["half"] = Vector3(t.half.x, t.half.y, t.half.z);
        entry["inside"] = static_cast<std::int64_t>(t.inside.size());
        out.push_back(entry);
    }
    return out;
}

} // namespace skydot
