// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors/actor_animation.hpp"

#include "animation_generated.h"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skydot {
namespace {

namespace afb = bethconv::pack::afb;
using godot::String;

constexpr std::uint32_t k_animation_format = 1;
constexpr std::uint8_t k_max_degree = 7;
/// A translation this far out is a prop bone parked out of sight.
constexpr float k_parked = 1.0e5f;

int g_missing_bones = 0;

const afb::Animation* read_asset(const godot::PackedByteArray& bytes) {
    // An empty array may have no data pointer; the root is read only if it has.
    const auto* data = bytes.ptr();
    flatbuffers::Verifier verifier(data, static_cast<std::size_t>(bytes.size()));
    if (data == nullptr || bytes.is_empty() || !afb::VerifyAnimationBuffer(verifier)) {
        return nullptr;
    }
    const auto* root = afb::GetAnimation(data);
    return root->format_version() == k_animation_format ? root : nullptr;
}

/// The channel at local frame `u`, into `out` (width floats). Returns false
/// for the identity (no `lo`). A malformed spline evaluates to its constant.
bool evaluate(const afb::Channel* c, float u, std::array<float, 4>& out) {
    if (c == nullptr || c->lo() == nullptr || c->lo()->size() == 0 || c->lo()->size() > 4) {
        return false;
    }
    const auto* lo = c->lo();
    const std::size_t width = lo->size();
    for (std::size_t a = 0; a < width; ++a) {
        out[a] = lo->Get(static_cast<flatbuffers::uoffset_t>(a));
    }
    const auto* knots = c->knots();
    const auto* hi = c->hi();
    const auto* points = c->points();
    const std::size_t p = c->degree();
    if (knots == nullptr || knots->size() == 0 || hi == nullptr || hi->size() != width || points == nullptr ||
        points->size() % width != 0 || p < 1 || p > k_max_degree) {
        return true;
    }
    const std::size_t count = points->size() / width;
    if (count <= p || knots->size() != count + p + 1) {
        return true;
    }
    const std::size_t n = count - 1;
    std::size_t span = p;
    while (span < n && static_cast<float>(knots->Get(static_cast<flatbuffers::uoffset_t>(span + 1))) <= u) {
        ++span;
    }
    std::array<std::array<float, 4>, k_max_degree + 1> d{};
    for (std::size_t j = 0; j <= p; ++j) {
        const std::size_t i = j + span - p;
        for (std::size_t a = 0; a < width; ++a) {
            const float l = lo->Get(static_cast<flatbuffers::uoffset_t>(a));
            const float h = hi->Get(static_cast<flatbuffers::uoffset_t>(a));
            const auto q = points->Get(static_cast<flatbuffers::uoffset_t>(i * width + a));
            d[j][a] = l + (h - l) * static_cast<float>(q) / 65535.0f;
        }
    }
    for (std::size_t r = 1; r <= p; ++r) {
        for (std::size_t j = p; j >= r; --j) {
            const std::size_t i = j + span - p;
            const float k0 = knots->Get(static_cast<flatbuffers::uoffset_t>(i));
            const float k1 = knots->Get(static_cast<flatbuffers::uoffset_t>(i + p - r + 1));
            const float alpha = k1 > k0 ? (u - k0) / (k1 - k0) : 0.0f;
            for (std::size_t a = 0; a < width; ++a) {
                d[j][a] = (1.0f - alpha) * d[j - 1][a] + alpha * d[j][a];
            }
        }
    }
    for (std::size_t a = 0; a < width; ++a) {
        out[a] = d[p][a];
    }
    return true;
}

godot::Transform3D rest_of(const afb::QsTransform* q) {
    godot::Quaternion rot(q->rx(), q->ry(), q->rz(), q->rw());
    if (rot.length_squared() <= 0.0f) {
        rot = godot::Quaternion();
    }
    godot::Basis basis(rot.normalized());
    basis.scale(godot::Vector3(q->sx(), q->sy(), q->sz()));
    return {basis, godot::Vector3(q->tx(), q->ty(), q->tz())};
}

/// The node's transform relative to the topmost ancestor, excluding the
/// converter's z-up root (whose children are in game units).
godot::Transform3D in_game_units(godot::Node* node) {
    godot::Transform3D t;
    while (node != nullptr) {
        auto* n3 = godot::Object::cast_to<godot::Node3D>(node);
        if (n3 == nullptr || String(node->get_name()).begins_with("bethconv_")) {
            break;
        }
        t = n3->get_transform() * t;
        node = node->get_parent();
    }
    return t;
}

void collect_meshes(godot::Node* node, std::vector<godot::MeshInstance3D*>& out) {
    if (auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(node); mesh != nullptr && mesh->get_skin().is_valid()) {
        out.push_back(mesh);
    }
    for (int i = 0; i < node->get_child_count(); ++i) {
        collect_meshes(node->get_child(i), out);
    }
}

} // namespace

godot::Dictionary SkydotAnimation::describe(const godot::PackedByteArray& asset) {
    godot::Dictionary out;
    const auto* root = read_asset(asset);
    if (root == nullptr) {
        out["error"] = "not an animation asset this engine reads";
        return out;
    }
    godot::Array skeletons;
    if (const auto* s = root->skeletons()) {
        for (const auto* sk : *s) {
            skeletons.push_back(sk->name() != nullptr ? String::utf8(sk->name()->c_str()) : String());
        }
    }
    godot::Array clips;
    if (const auto* c = root->clips()) {
        for (const auto* clip : *c) {
            godot::Dictionary d;
            d["duration"] = clip->duration();
            d["frames"] = static_cast<int64_t>(clip->frame_count());
            d["tracks"] = static_cast<int64_t>(clip->transform_tracks());
            d["skeleton"] = clip->skeleton_name() != nullptr ? String::utf8(clip->skeleton_name()->c_str()) : String();
            godot::Array notes;
            if (const auto* tracks = clip->annotations()) {
                for (const auto* t : *tracks) {
                    if (const auto* entries = t->annotations()) {
                        for (const auto* a : *entries) {
                            godot::Dictionary n;
                            n["time"] = a->time();
                            n["text"] = a->text() != nullptr ? String::utf8(a->text()->c_str()) : String();
                            notes.push_back(n);
                        }
                    }
                }
            }
            d["annotations"] = notes;
            clips.push_back(d);
        }
    }
    out["skeletons"] = skeletons;
    out["clips"] = clips;
    return out;
}

/// A skeleton asset's first skeleton as names, parents and rests, named as
/// `build_skeleton` names its bones. Empty (with an error printed) if the
/// asset has none.
SkydotAnimation::Bones SkydotAnimation::read_bones(const godot::PackedByteArray& asset) {
    const auto* root = read_asset(asset);
    if (root == nullptr || root->skeletons() == nullptr || root->skeletons()->size() == 0) {
        godot::UtilityFunctions::push_error("SkydotAnimation: no skeleton in this asset");
        return {};
    }
    const auto* s = root->skeletons()->Get(0);
    const auto* bones = s->bones();
    const auto* parents = s->parents();
    const auto* pose = s->reference_pose();
    if (bones == nullptr || parents == nullptr || pose == nullptr || parents->size() != bones->size() ||
        pose->size() != bones->size()) {
        godot::UtilityFunctions::push_error("SkydotAnimation: skeleton bones, parents and poses disagree");
        return {};
    }
    Bones out;
    out.name = s->name() != nullptr ? String::utf8(s->name()->c_str()) : String("Skeleton");
    std::unordered_set<std::string> seen;
    for (flatbuffers::uoffset_t i = 0; i < bones->size(); ++i) {
        std::string name = bones->Get(i)->str();
        if (name.empty() || seen.contains(name)) {
            name = "bone_" + std::to_string(i);
        }
        seen.insert(name);
        out.names.push_back(String::utf8(name.c_str()));
        const auto parent = parents->Get(i);
        // The converter checked that parents come first; refuse otherwise.
        out.parents.push_back(parent >= 0 && parent < static_cast<std::int16_t>(i) ? parent : -1);
        out.rests.push_back(rest_of(pose->Get(i)));
    }
    return out;
}

godot::Skeleton3D* SkydotAnimation::build_skeleton(const godot::PackedByteArray& asset) {
    const Bones bones = read_bones(asset);
    if (bones.names.empty()) {
        return nullptr;
    }
    auto* skeleton = memnew(godot::Skeleton3D);
    skeleton->set_name(bones.name);
    for (std::size_t i = 0; i < bones.names.size(); ++i) {
        const auto bone = static_cast<int32_t>(i);
        skeleton->add_bone(bones.names[i]);
        if (bones.parents[i] >= 0) {
            skeleton->set_bone_parent(bone, bones.parents[i]);
        }
        skeleton->set_bone_rest(bone, bones.rests[i]);
    }
    skeleton->reset_bone_poses();
    return skeleton;
}

godot::Ref<godot::Animation> SkydotAnimation::build_clip(const godot::PackedByteArray& asset,
                                                         godot::Skeleton3D* skeleton,
                                                         const godot::String& skeleton_path) {
    if (skeleton == nullptr) {
        godot::UtilityFunctions::push_error("SkydotAnimation: no skeleton");
        return {};
    }
    Bones bones;
    for (int i = 0; i < skeleton->get_bone_count(); ++i) {
        bones.names.push_back(skeleton->get_bone_name(i));
        bones.parents.push_back(skeleton->get_bone_parent(i));
        bones.rests.push_back(skeleton->get_bone_rest(i));
    }
    return clip_onto(asset, bones, skeleton_path);
}

godot::Ref<godot::Animation> SkydotAnimation::build_clip_for(const godot::PackedByteArray& asset,
                                                             const godot::PackedByteArray& skeleton_asset,
                                                             const godot::String& skeleton_path) {
    const Bones bones = read_bones(skeleton_asset);
    if (bones.names.empty()) {
        return {};
    }
    return clip_onto(asset, bones, skeleton_path);
}

godot::Ref<godot::Animation> SkydotAnimation::clip_onto(const godot::PackedByteArray& asset, const Bones& bones,
                                                        const godot::String& skeleton_path) {
    const auto* root = read_asset(asset);
    if (root == nullptr || root->clips() == nullptr || root->clips()->size() == 0) {
        godot::UtilityFunctions::push_error("SkydotAnimation: no clip in this asset");
        return {};
    }
    const auto* clip = root->clips()->Get(0);
    const auto* blocks = clip->blocks();
    const std::uint32_t frames = clip->frame_count();
    const std::uint32_t tracks = clip->transform_tracks();
    if (blocks == nullptr || blocks->size() == 0 || frames == 0) {
        godot::UtilityFunctions::push_error("SkydotAnimation: clip has no frames");
        return {};
    }
    const std::uint32_t step = clip->frames_per_block() > 1 ? clip->frames_per_block() - 1 : 1;
    const float frame_time = clip->frame_duration() > 0.0f ? clip->frame_duration() : 1.0f / 30.0f;
    const auto* binding = clip->track_to_bone();

    godot::Ref<godot::Animation> anim;
    anim.instantiate();
    anim->set_length(static_cast<double>(std::max(clip->duration(), frame_time * static_cast<float>(frames - 1))));
    anim->set_step(frame_time);
    for (std::uint32_t t = 0; t < tracks; ++t) {
        int bone = static_cast<int>(t);
        if (binding != nullptr && binding->size() == tracks) {
            bone = binding->Get(t);
        }
        if (bone < 0 || bone >= static_cast<int>(bones.names.size())) {
            continue;
        }
        std::vector<godot::Vector3> positions(frames);
        std::vector<godot::Quaternion> rotations(frames);
        bool parked = false;
        const godot::Transform3D& rest = bones.rests[static_cast<std::size_t>(bone)];
        for (std::uint32_t f = 0; f < frames; ++f) {
            const std::size_t b = std::min<std::size_t>(f / step, blocks->size() - 1);
            const auto* block_tracks = blocks->Get(static_cast<flatbuffers::uoffset_t>(b))->tracks();
            const float u = static_cast<float>(f - b * step);
            positions[f] = rest.origin;
            rotations[f] = rest.basis.get_rotation_quaternion();
            if (block_tracks == nullptr || t >= block_tracks->size()) {
                continue;
            }
            const auto* track = block_tracks->Get(t);
            std::array<float, 4> v{};
            if (evaluate(track->translation(), u, v)) {
                positions[f] = godot::Vector3(v[0], v[1], v[2]);
                parked = parked || std::fabs(v[0]) > k_parked || std::fabs(v[1]) > k_parked ||
                         std::fabs(v[2]) > k_parked;
            } else {
                positions[f] = godot::Vector3();
            }
            if (evaluate(track->rotation(), u, v)) {
                godot::Quaternion q(v[0], v[1], v[2], v[3]);
                rotations[f] = q.length_squared() > 0.0f ? q.normalized() : godot::Quaternion();
            } else {
                rotations[f] = godot::Quaternion();
            }
        }
        if (parked) {
            continue;
        }
        const String path = skeleton_path + String(":") + bones.names[static_cast<std::size_t>(bone)];
        // Every track keys the bone, even an identity one (no translation
        // is Havok's zero, not the rest pose): the clip states the whole
        // pose, and a missing key would keep another clip's.
        const int pos_track = anim->add_track(godot::Animation::TYPE_POSITION_3D);
        anim->track_set_path(pos_track, godot::NodePath(path));
        const int rot_track = anim->add_track(godot::Animation::TYPE_ROTATION_3D);
        anim->track_set_path(rot_track, godot::NodePath(path));
        for (std::uint32_t f = 0; f < frames; ++f) {
            const double time = static_cast<double>(f) * static_cast<double>(frame_time);
            anim->position_track_insert_key(pos_track, time, positions[f]);
            if (f > 0 && rotations[f].dot(rotations[f - 1]) < 0.0f) {
                rotations[f] = -rotations[f];
            }
            anim->rotation_track_insert_key(rot_track, time, rotations[f]);
        }
    }
    if (const auto* notes = clip->annotations()) {
        for (const auto* t : *notes) {
            if (const auto* entries = t->annotations()) {
                for (const auto* a : *entries) {
                    if (a->text() == nullptr) {
                        continue;
                    }
                    // Markers need unique names; Skyrim repeats texts.
                    String name = String::utf8(a->text()->c_str());
                    String unique = name;
                    for (int n = 2; anim->has_marker(unique); ++n) {
                        unique = name + "#" + String::num_int64(n);
                    }
                    anim->add_marker(unique, std::clamp<double>(static_cast<double>(a->time()), 0.0, anim->get_length()));
                }
            }
        }
    }
    return anim;
}

int SkydotAnimation::attach_skinned(godot::Node* model, godot::Skeleton3D* skeleton) {
    g_missing_bones = 0;
    if (model == nullptr || skeleton == nullptr) {
        return 0;
    }
    std::unordered_map<std::string, int> by_name;
    for (int i = 0; i < skeleton->get_bone_count(); ++i) {
        by_name.emplace(String(skeleton->get_bone_name(i)).to_lower().utf8().get_data(), i);
    }
    // Our bones' rest in the skeleton's space.
    std::vector<godot::Transform3D> rest_global(static_cast<std::size_t>(skeleton->get_bone_count()));
    for (int i = 0; i < skeleton->get_bone_count(); ++i) {
        const int parent = skeleton->get_bone_parent(i);
        rest_global[static_cast<std::size_t>(i)] =
            parent >= 0 ? rest_global[static_cast<std::size_t>(parent)] * skeleton->get_bone_rest(i)
                        : skeleton->get_bone_rest(i);
    }
    const godot::Transform3D ours = in_game_units(skeleton);

    std::vector<godot::MeshInstance3D*> meshes;
    collect_meshes(model, meshes);
    int moved = 0;
    for (godot::MeshInstance3D* mesh : meshes) {
        const godot::Ref<godot::Skin> skin = mesh->get_skin();
        auto* original = godot::Object::cast_to<godot::Skeleton3D>(mesh->get_node_or_null(mesh->get_skeleton_path()));
        if (original == nullptr) {
            continue;
        }
        // Original skeleton space -> ours, both in game units.
        const godot::Transform3D to_ours = ours.affine_inverse() * in_game_units(original);
        godot::Ref<godot::Skin> rebound;
        rebound.instantiate();
        for (int b = 0; b < skin->get_bind_count(); ++b) {
            String name = skin->get_bind_name(b);
            const int orig_bone = skin->get_bind_bone(b);
            if (name.is_empty() && orig_bone >= 0 && orig_bone < original->get_bone_count()) {
                name = original->get_bone_name(orig_bone);
            }
            const int orig_index = original->find_bone(name);
            // Mesh space -> original skeleton space, at the bind pose.
            const godot::Transform3D placed =
                (orig_index >= 0 ? original->get_bone_global_rest(orig_index) : godot::Transform3D()) *
                skin->get_bind_pose(b);
            const auto found = by_name.find(name.to_lower().utf8().get_data());
            int bone = 0;
            if (found == by_name.end()) {
                ++g_missing_bones;
            } else {
                bone = found->second;
            }
            // As the game does: the bind stays relative to its bone, so the mesh
            // follows the actor's bones, not the bones of the NIF it came from
            // (a FaceGen head's neck then meets the body's). A bone the
            // skeleton lacks keeps the mesh where its NIF put it, on the root.
            const godot::Transform3D bind =
                found != by_name.end() && orig_index >= 0
                    ? skin->get_bind_pose(b)
                    : rest_global[static_cast<std::size_t>(bone)].affine_inverse() * to_ours * placed;
            rebound->add_named_bind(skeleton->get_bone_name(bone), bind);
        }
        mesh->get_parent()->remove_child(mesh);
        skeleton->add_child(mesh);
        mesh->set_owner(skeleton->get_owner());
        mesh->set_transform(godot::Transform3D());
        mesh->set_skin(rebound);
        mesh->set_skeleton_path(godot::NodePath(".."));
        ++moved;
    }
    return moved;
}

int SkydotAnimation::get_last_missing_bones() {
    return g_missing_bones;
}

void SkydotAnimation::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_static_method("SkydotAnimation", D_METHOD("describe", "asset"), &SkydotAnimation::describe);
    godot::ClassDB::bind_static_method("SkydotAnimation", D_METHOD("build_skeleton", "asset"),
                                       &SkydotAnimation::build_skeleton);
    godot::ClassDB::bind_static_method("SkydotAnimation", D_METHOD("build_clip", "asset", "skeleton", "skeleton_path"),
                                       &SkydotAnimation::build_clip);
    godot::ClassDB::bind_static_method("SkydotAnimation", D_METHOD("attach_skinned", "model", "skeleton"),
                                       &SkydotAnimation::attach_skinned);
    godot::ClassDB::bind_static_method("SkydotAnimation", D_METHOD("get_last_missing_bones"),
                                       &SkydotAnimation::get_last_missing_bones);
}

} // namespace skydot
