// SPDX-License-Identifier: GPL-3.0-or-later
//
// `bethconv ragdoll PACK`: the ragdolls a pack's skeleton models carry
// (`extras.bethconv.ragdoll`, formats/pack-format.md), checked against the
// ragdoll skeleton of the `.hkx` beside each: hkaRagdollInstance's skeleton,
// one `Ragdoll_<bone>` per rigid body, whose reference pose is where the
// bodies are at rest. Reports the bodies only one side has and, per body
// both have, how far the hkx bone lies from the NIF bone at bind.
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/pack/animation_asset.hpp"
#include "bethconv/pack/asset_store.hpp"
#include "bethconv/pack/pack_view.hpp"
#include "bethconv/pack/vpath_index.hpp"

#include "skydot_formats/units.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace bethconv::cli {
namespace {

using Vec = std::array<double, 3>;
using Quat = std::array<double, 4>; // x y z w

Quat multiply(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Vec rotate(const Quat& q, const Vec& v) {
    const Vec uv{q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2], q[0] * v[1] - q[1] * v[0]};
    const Vec uuv{q[1] * uv[2] - q[2] * uv[1], q[2] * uv[0] - q[0] * uv[2], q[0] * uv[1] - q[1] * uv[0]};
    return {v[0] + 2 * (q[3] * uv[0] + uuv[0]), v[1] + 2 * (q[3] * uv[1] + uuv[1]),
            v[2] + 2 * (q[3] * uv[2] + uuv[2])};
}

/// Degrees between two orientations.
double angle_between(const Quat& a, const Quat& b) {
    double dot = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        dot += a[i] * b[i];
    }
    const double na = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]);
    const double nb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
    dot = std::min(1.0, std::abs(dot) / (na * nb));
    return 2.0 * std::acos(dot) * 180.0 / std::numbers::pi;
}

double distance(const Vec& a, const Vec& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

Vec vec_of(const nlohmann::json& j) {
    Vec out{};
    if (j.is_array()) {
        for (std::size_t i = 0; i < 3 && i < j.size(); ++i) {
            out[i] = j[i].is_number() ? j[i].get<double>() : 0.0;
        }
    }
    return out;
}

Quat quat_of(const nlohmann::json& j) {
    Quat out{0, 0, 0, 1};
    if (j.is_array()) {
        for (std::size_t i = 0; i < 4 && i < j.size(); ++i) {
            out[i] = j[i].is_number() ? j[i].get<double>() : 0.0;
        }
    }
    return out;
}

/// The model's `extras.bethconv.ragdoll`, or null.
nlohmann::json ragdoll_of(const nlohmann::json& doc) {
    const auto nodes = doc.find("nodes");
    if (nodes == doc.end() || !nodes->is_array()) {
        return nullptr;
    }
    for (const auto& node : *nodes) {
        const auto extras = node.find("extras");
        if (extras == node.end() || !extras->is_object()) {
            continue;
        }
        const auto block = extras->find("bethconv");
        if (block != extras->end() && block->is_object() && block->contains("ragdoll")) {
            return (*block)["ragdoll"];
        }
    }
    return nullptr;
}

struct Pose {
    Vec position; ///< Metres.
    Quat rotation;
};

constexpr std::string_view k_ragdoll_prefix = "Ragdoll_";

/// The hkx ragdoll skeleton (the one whose bones are `Ragdoll_*`) in model space, by body
/// name (prefix taken off), its reference pose read in game units.
std::optional<std::map<std::string, Pose>> hkx_ragdoll(const animation::HkxFile& file, std::string& name) {
    const animation::Skeleton* best = nullptr;
    std::size_t best_count = 0;
    for (const auto& skeleton : file.skeletons) {
        const auto count = static_cast<std::size_t>(std::ranges::count_if(
            skeleton.bones, [](const std::string& bone) { return bone.starts_with(k_ragdoll_prefix); }));
        if (count > best_count) {
            best = &skeleton;
            best_count = count;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    name = best->name;
    const double m = skydot::formats::k_metres_per_unit;
    std::vector<Pose> model(best->bones.size());
    std::map<std::string, Pose> out;
    for (std::size_t i = 0; i < best->bones.size() && i < best->reference_pose.size(); ++i) {
        const auto& local = best->reference_pose[i];
        const auto d = [](float v) { return static_cast<double>(v); };
        const Vec t{d(local.translation[0]) * m, d(local.translation[1]) * m, d(local.translation[2]) * m};
        const Quat r{d(local.rotation[0]), d(local.rotation[1]), d(local.rotation[2]), d(local.rotation[3])};
        const std::int16_t parent = i < best->parents.size() ? best->parents[i] : -1;
        if (parent >= 0 && static_cast<std::size_t>(parent) < i) {
            const Pose& p = model[static_cast<std::size_t>(parent)];
            const Vec moved = rotate(p.rotation, t);
            model[i] = {{p.position[0] + moved[0], p.position[1] + moved[1], p.position[2] + moved[2]},
                        multiply(p.rotation, r)};
        } else {
            model[i] = {t, r};
        }
        const std::string& bone = best->bones[i];
        if (bone.starts_with(k_ragdoll_prefix)) {
            out[bone.substr(k_ragdoll_prefix.size())] = model[i];
        }
    }
    return out;
}

/// Reads a pack asset by virtual path; prints why not and returns nullopt on failure.
std::optional<pack::AssetReader::Bytes> read_entry(const pack::AssetReader& assets, const pack::VpathEntry& entry) {
    auto bytes = assets.read(entry);
    if (!bytes) {
        std::fprintf(stderr, "error: %s: %s\n", entry.vpath.c_str(), bytes.error().to_string().c_str());
        return std::nullopt;
    }
    return std::move(*bytes);
}

void print_detail(const nlohmann::json& ragdoll) {
    const auto& bodies = ragdoll["bodies"];
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const auto& b = bodies[i];
        const Vec at = vec_of(b["bone_bind"]["translation"]);
        std::printf("    body %zu '%s' %s mass %.2f layer %d part 0x%02X shapes %zu at (%.3f %.3f %.3f) m\n", i,
                    b.value("bone", "").c_str(), b.value("block", "").c_str(), b.value("mass", 0.0),
                    b["filter"].value("layer", 0), b["filter"].value("flags_and_part", 0), b["shapes"].size(),
                    at[0], at[1], at[2]);
        for (const auto& s : b["shapes"]) {
            std::printf("      %s radius %.3f", s.value("kind", "").c_str(), s.value("radius", 0.0));
            if (s.contains("point_a")) {
                std::printf(" length %.3f", distance(vec_of(s["point_a"]), vec_of(s["point_b"])));
            }
            std::printf("\n");
        }
    }
    const auto deg = [](const nlohmann::json& c, const char* key) {
        return c.value(key, 0.0) * 180.0 / std::numbers::pi;
    };
    for (const auto& c : ragdoll["constraints"]) {
        const auto name = [&](const char* key) {
            const std::size_t index = c.value(key, std::size_t{0});
            return index < bodies.size() ? bodies[index].value("bone", "") : std::string("?");
        };
        const std::string type = c.value("type", "");
        std::printf("    %s%s%s %s -> %s", type.c_str(), c.contains("wrapper") ? " in " : "",
                    c.value("wrapper", "").c_str(), name("body_a").c_str(), name("body_b").c_str());
        if (type == "ragdoll") {
            std::printf(" cone %.1f plane %.1f..%.1f twist %.1f..%.1f", deg(c, "cone_max"), deg(c, "plane_min"),
                        deg(c, "plane_max"), deg(c, "twist_min"), deg(c, "twist_max"));
        } else if (type == "limited_hinge") {
            std::printf(" %.1f..%.1f", deg(c, "min_angle"), deg(c, "max_angle"));
        }
        if (c.contains("max_friction")) {
            std::printf(" friction %.2f", c.value("max_friction", 0.0));
        }
        std::printf("\n");
    }
}

int cmd_ragdoll(const std::filesystem::path& pack_dir, const std::string& filter, bool detail, double tolerance_cm,
                double tolerance_deg) {
    auto index = pack::VpathIndex::read(pack_dir / "vpath.idx");
    if (!index) {
        std::fprintf(stderr, "error: %s\n", index.error().to_string().c_str());
        return 1;
    }
    auto assets = pack::AssetReader::open(pack_dir);
    if (!assets) {
        std::fprintf(stderr, "error: %s\n", assets.error().to_string().c_str());
        return 1;
    }
    std::size_t models = 0;
    std::size_t with_ragdoll = 0;
    std::size_t disagree = 0;
    for (const auto& entry : index->entries()) {
        if (entry.kind != pack::AssetKind::mesh || !entry.vpath.starts_with("meshes/") ||
            entry.vpath.find("skeleton") == std::string::npos ||
            (!filter.empty() && entry.vpath.find(filter) == std::string::npos)) {
            continue;
        }
        ++models;
        const auto glb = read_entry(*assets, entry);
        if (!glb) {
            ++disagree;
            continue;
        }
        const auto chunks = pack::glb_chunks(glb->data, entry.vpath);
        const auto doc = chunks ? nlohmann::json::parse(chunks->json, nullptr, false) : nlohmann::json();
        if (!chunks || doc.is_discarded()) {
            std::fprintf(stderr, "error: %s: not a GLB with a JSON chunk\n", entry.vpath.c_str());
            ++disagree;
            continue;
        }
        const nlohmann::json ragdoll = ragdoll_of(doc);
        if (!ragdoll.is_object() || !ragdoll["bodies"].is_array() || !ragdoll["constraints"].is_array()) {
            if (detail) {
                std::printf("%s: no ragdoll\n", entry.vpath.c_str());
            }
            continue;
        }
        ++with_ragdoll;
        std::map<std::string, std::size_t> by_type;
        for (const auto& c : ragdoll["constraints"]) {
            ++by_type[c.value("type", "?")];
        }
        std::printf("%s: %zu bodies, %zu constraints", entry.vpath.c_str(), ragdoll["bodies"].size(),
                    ragdoll["constraints"].size());
        for (const auto& [type, count] : by_type) {
            std::printf(" (%zu %s)", count, type.c_str());
        }
        std::printf("\n");
        if (detail) {
            print_detail(ragdoll);
        }

        std::string hkx_path = entry.vpath.substr(0, entry.vpath.size() - 4) + ".hkx";
        const pack::VpathEntry* hkx_entry = index->find(hkx_path);
        if (hkx_entry == nullptr || hkx_entry->kind != pack::AssetKind::animation) {
            std::printf("  hkx: no %s in the pack\n", hkx_path.c_str());
            continue;
        }
        const auto hkx_bytes = read_entry(*assets, *hkx_entry);
        auto hkx = hkx_bytes ? pack::read_animation_asset(hkx_bytes->data, hkx_path)
                             : io::ParseResult<animation::HkxFile>();
        if (!hkx_bytes || !hkx) {
            if (hkx_bytes) {
                std::fprintf(stderr, "error: %s\n", hkx.error().to_string().c_str());
            }
            ++disagree;
            continue;
        }
        std::string skeleton_name;
        const auto hkx_bodies = hkx_ragdoll(*hkx, skeleton_name);
        if (!hkx_bodies) {
            std::printf("  hkx: %s has no Ragdoll_ skeleton (%zu skeletons)\n", hkx_path.c_str(),
                        hkx->skeletons.size());
            ++disagree;
            continue;
        }
        const auto& bodies = ragdoll["bodies"];
        // Each body's frame at bind: the bone's, then the body's own transform in it.
        std::vector<Pose> nif_pose;
        for (const auto& body : bodies) {
            const Vec bone_at = vec_of(body["bone_bind"]["translation"]);
            const Quat bone_rot = quat_of(body["bone_bind"]["rotation"]);
            const Vec offset = rotate(bone_rot, vec_of(body["body_transform"]["translation"]));
            nif_pose.push_back({{bone_at[0] + offset[0], bone_at[1] + offset[1], bone_at[2] + offset[2]},
                                multiply(bone_rot, quat_of(body["body_transform"]["rotation"]))});
        }
        // Bodies by name, then the rest by nearest position (relative to the first body).
        std::vector<std::optional<std::string>> partner(bodies.size());
        std::map<std::string, bool> taken;
        std::size_t by_name = 0;
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const std::string bone = bodies[i].value("bone", "");
            if (hkx_bodies->contains(bone) && !taken[bone]) {
                partner[i] = bone;
                taken[bone] = true;
                ++by_name;
            }
        }
        // Positions relative to the first body when both sides name it, else as they are.
        std::optional<Vec> nif_root;
        std::optional<Vec> hkx_root;
        if (!bodies.empty() && partner[0]) {
            nif_root = nif_pose[0].position;
            hkx_root = hkx_bodies->at(*partner[0]).position;
        }
        const auto relative = [](const Vec& p, const std::optional<Vec>& root) {
            return root ? Vec{p[0] - (*root)[0], p[1] - (*root)[1], p[2] - (*root)[2]} : p;
        };
        std::size_t by_position = 0;
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            if (partner[i]) {
                continue;
            }
            const Vec at = relative(nif_pose[i].position, nif_root);
            double best = 1e30;
            std::string best_name;
            for (const auto& [name, pose] : *hkx_bodies) {
                const double d = distance(at, relative(pose.position, hkx_root));
                if (!taken[name] && d < best) {
                    best = d;
                    best_name = name;
                }
            }
            if (!best_name.empty()) {
                partner[i] = best_name;
                taken[best_name] = true;
                ++by_position;
                std::printf("    no hkx body named '%s': paired by position with '%s' (%.2f cm)\n",
                            bodies[i].value("bone", "").c_str(), best_name.c_str(), best * 100.0);
            } else {
                std::printf("    only in the NIF: '%s'\n", bodies[i].value("bone", "").c_str());
            }
        }
        for (const auto& [name, pose] : *hkx_bodies) {
            if (!taken[name]) {
                std::printf("    only in the hkx: '%s'\n", name.c_str());
            }
        }
        double worst_cm = 0;
        double worst_deg = 0;
        std::string worst_cm_bone;
        std::string worst_deg_bone;
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            if (!partner[i]) {
                continue;
            }
            const Pose& h = hkx_bodies->at(*partner[i]);
            const std::string bone = bodies[i].value("bone", "");
            const double cm = distance(relative(nif_pose[i].position, nif_root), relative(h.position, hkx_root)) * 100.0;
            const double deg = angle_between(nif_pose[i].rotation, h.rotation);
            if (cm > worst_cm) {
                worst_cm = cm;
                worst_cm_bone = bone;
            }
            if (deg > worst_deg) {
                worst_deg = deg;
                worst_deg_bone = bone;
            }
            if (detail && (cm > tolerance_cm || deg > tolerance_deg)) {
                std::printf("    differs: '%s' by %.2f cm, %.2f deg (NIF %.3f %.3f %.3f, hkx %.3f %.3f %.3f m)\n",
                            bone.c_str(), cm, deg, nif_pose[i].position[0], nif_pose[i].position[1],
                            nif_pose[i].position[2], h.position[0], h.position[1], h.position[2]);
            }
        }
        // How far apart a constraint's two pivots are, with the bodies at the NIF's bind and at
        // the hkx ragdoll's reference pose: the pose the constraint frames were made in has none.
        const auto pivot_gap = [&](const nlohmann::json& c, bool hkx_pose) -> std::optional<double> {
            const auto world = [&](const char* body_key, const char* frame_key) -> std::optional<Vec> {
                const std::size_t at = c.value(body_key, std::size_t{0});
                if (at >= bodies.size() || (hkx_pose && !partner[at])) {
                    return std::nullopt;
                }
                const auto& body = bodies[at];
                // Pivots are in the bone's frame; undo the body's transform for the hkx pose,
                // which places the body.
                Vec p = vec_of(c[frame_key]["pivot"]);
                Pose pose = nif_pose[at];
                const Vec bt = vec_of(body["body_transform"]["translation"]);
                p = {p[0] - bt[0], p[1] - bt[1], p[2] - bt[2]};
                const Quat br = quat_of(body["body_transform"]["rotation"]);
                p = rotate({-br[0], -br[1], -br[2], br[3]}, p);
                if (hkx_pose) {
                    pose = hkx_bodies->at(*partner[at]);
                }
                const Vec r = rotate(pose.rotation, p);
                return Vec{pose.position[0] + r[0], pose.position[1] + r[1], pose.position[2] + r[2]};
            };
            const auto a = world("body_a", "in_a");
            const auto b = world("body_b", "in_b");
            if (!a || !b) {
                return std::nullopt;
            }
            return distance(*a, *b) * 100.0;
        };
        double gap_bind = 0;
        double gap_hkx = 0;
        std::string gap_bind_at;
        std::string gap_hkx_at;
        for (const auto& c : ragdoll["constraints"]) {
            const std::size_t a = c.value("body_a", std::size_t{0});
            const std::string at = a < bodies.size() ? bodies[a].value("bone", "") : "?";
            if (const auto g = pivot_gap(c, false); g && *g > gap_bind) {
                gap_bind = *g;
                gap_bind_at = at;
            }
            if (const auto g = pivot_gap(c, true); g && *g > gap_hkx) {
                gap_hkx = *g;
                gap_hkx_at = at;
            }
        }
        const bool same_set = by_name == bodies.size() && by_name == hkx_bodies->size();
        const bool close = worst_cm <= tolerance_cm && worst_deg <= tolerance_deg;
        std::printf("  hkx '%s': %zu bodies; %zu paired by name, %zu by position; root offset %.2f cm; max %.2f cm "
                    "('%s'), max %.2f deg ('%s'); widest pivot gap %.2f cm at bind ('%s'), %.2f cm in the hkx "
                    "pose ('%s'): %s\n",
                    skeleton_name.c_str(), hkx_bodies->size(), by_name, by_position,
                    nif_root && hkx_root ? distance(*nif_root, *hkx_root) * 100.0 : 0.0, worst_cm,
                    worst_cm_bone.c_str(), worst_deg, worst_deg_bone.c_str(), gap_bind, gap_bind_at.c_str(), gap_hkx,
                    gap_hkx_at.c_str(),
                    same_set && close ? "agree"
                    : close           ? "names differ, poses agree"
                                      : "DISAGREE");
        if (!same_set || !close) {
            ++disagree;
        }
    }
    std::printf("%zu skeleton models, %zu with a ragdoll, %zu disagree with their hkx or could not be read\n",
                models, with_ragdoll, disagree);
    return disagree == 0 ? 0 : 1;
}

struct RagdollArgs {
    std::filesystem::path pack;
    std::string filter;
    bool detail = false;
    double tolerance_cm = 0.5;
    double tolerance_deg = 1.0;
};

} // namespace

void register_ragdoll(CLI::App& app) {
    auto args = std::make_shared<RagdollArgs>();
    auto* cmd = app.add_subcommand(
        "ragdoll", "List the ragdolls of a pack's skeleton models and check them against the hkx ragdoll skeletons");
    cmd->add_option("pack", args->pack, "The pack directory")->required()->check(CLI::ExistingDirectory);
    cmd->add_option("--filter", args->filter, "Only models whose path contains this");
    cmd->add_flag("--detail", args->detail, "Print every body and constraint, and each body that differs");
    cmd->add_option("--tolerance-cm", args->tolerance_cm, "Largest body position difference that agrees")
        ->default_val(0.5);
    cmd->add_option("--tolerance-deg", args->tolerance_deg, "Largest body orientation difference that agrees")
        ->default_val(1.0);
    cmd->callback([args] {
        set_exit_status(
            cmd_ragdoll(args->pack, args->filter, args->detail, args->tolerance_cm, args->tolerance_deg));
    });
}

} // namespace bethconv::cli
