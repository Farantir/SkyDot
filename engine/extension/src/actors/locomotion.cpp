// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors/locomotion.hpp"

#include "animation_generated.h"
#include "actors/animation_project.hpp"
#include "assets/vpath.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skydot {
namespace {

namespace afb = bethconv::pack::afb;

constexpr std::uint32_t k_animation_format = 6;

const afb::Animation* read_asset(const godot::PackedByteArray& bytes) {
    // An empty array may have no data pointer; the root is read only if it has.
    const auto* data = bytes.ptr();
    if (data == nullptr || bytes.is_empty()) {
        return nullptr;
    }
    flatbuffers::Verifier verifier(data, static_cast<std::size_t>(bytes.size()));
    if (!afb::VerifyAnimationBuffer(verifier)) {
        return nullptr;
    }
    const auto* root = afb::GetAnimation(data);
    return root->format_version() == k_animation_format ? root : nullptr;
}

bool contains(const std::string& s, std::string_view what) {
    return s.find(what) != std::string::npos;
}

/// A clip name without a trailing ".hkx" (some idles are named after files).
std::string bare(std::string name) {
    name = ascii_lower(name);
    if (name.ends_with(".hkx")) {
        name.resize(name.size() - 4);
    }
    return name;
}

/// "walk forward" (or run) with no side, pace or stance.
bool is_gait(const std::string& name, std::string_view gait) {
    const std::string n = ascii_lower(name);
    const std::string g(gait);
    if (!contains(n, g + "forward") && !contains(n, g + "f") && !contains(n, g + "_f")) {
        return false;
    }
    for (const char* word : {"left", "right", "slow", "fast", "combat", "magic", "mirror", "attack", "sync",
                             "bank", "camera", "sneak", "drawn", "bow", "mag_", "turn", "1hm", "2hm"}) {
        if (contains(n, word)) {
            return false;
        }
    }
    // A side as a letter right after "forward" (WalkForwardL, Walk_Forward_R).
    for (const std::string& stem : {g + "forward", g + "forward_", g + "f", g + "_f"}) {
        const auto at = n.find(stem);
        if (at == std::string::npos) {
            continue;
        }
        const std::size_t next = at + stem.size();
        if (next < n.size() && (n[next] == 'l' || n[next] == 'r')) {
            return false;
        }
    }
    // "walkf" also starts "walkfast"; that was excluded above.
    return true;
}

struct Motion {
    float duration{};
    float forward{}; ///< +Y at the last key.
};

} // namespace

Locomotion find_locomotion(const std::string& behaviour,
                           const std::function<godot::PackedByteArray(const std::string&)>& bytes,
                           const std::function<bool(const std::string&)>& exists) {
    Locomotion out;
    const std::string path = normalize_path(behaviour);
    const auto slash = path.rfind('/');
    const auto dot = path.rfind('.');
    if (slash == std::string::npos || dot == std::string::npos || dot < slash) {
        out.missing = "no behaviour project";
        return out;
    }
    const AnimationProject project = read_animation_project(behaviour, [&](const std::string& vpath) {
        const godot::PackedByteArray raw = bytes(vpath);
        return raw.is_empty() ? std::vector<std::uint8_t>() : std::vector<std::uint8_t>(raw.ptr(), raw.ptr() + raw.size());
    });
    if (!project.missing.empty()) {
        out.missing = project.missing;
        return out;
    }
    const std::string& folder = project.folder;
    // Clip name -> animation file, from the project's behaviour graphs.
    std::unordered_map<std::string, std::string> generators;
    for (const std::string& file : project.files) {
        if (!file.starts_with("behaviors")) {
            continue;
        }
        const godot::PackedByteArray graph_bytes = bytes(folder + file);
        const auto* graph = read_asset(graph_bytes);
        if (graph == nullptr || graph->clip_generators() == nullptr) {
            continue;
        }
        for (const auto* g : *graph->clip_generators()) {
            const auto* name = g->name();
            const auto* animation = g->animation();
            if (name != nullptr && animation != nullptr) {
                generators.emplace(ascii_lower(name->str()), normalize_path(folder + animation->str()));
            }
        }
    }
    if (generators.empty()) {
        out.missing = "no clip generators in the behaviours of " + project.name;
        return out;
    }
    std::unordered_map<std::uint32_t, Motion> motions;
    for (const auto& [id, m] : project.motions) {
        Motion motion{m.duration, 0.0F};
        if (!m.translations.empty()) {
            motion.forward = m.translations.back()[2];
        }
        motions[id] = motion;
    }

    const auto clip_of = [&](const ProjectClip& c) {
        GaitClip g;
        g.name = c.name;
        g.playback = c.speed;
        if (const auto it = generators.find(ascii_lower(g.name)); it != generators.end()) {
            g.file = it->second;
        }
        g.notes = c.notes;
        if (const auto it = motions.find(c.animation); it != motions.end() && it->second.duration > 0.0F) {
            g.speed = it->second.forward / it->second.duration * g.playback;
        }
        return g;
    };
    const auto usable = [&](const GaitClip& g, bool needs_motion) {
        return !g.file.empty() && exists(g.file) && (!needs_motion || g.speed > 1.0F);
    };
    // The first name of `names` the project has, else the shortest name
    // `accept` takes.
    const auto pick = [&](std::initializer_list<std::string_view> names, const auto& accept, bool needs_motion) {
        for (const std::string_view want : names) {
            for (const ProjectClip& c : project.clips) {
                if (bare(c.name) == want) {
                    if (GaitClip g = clip_of(c); usable(g, needs_motion)) {
                        return g;
                    }
                }
            }
        }
        GaitClip best;
        for (const ProjectClip& c : project.clips) {
            if (!accept(c.name)) {
                continue;
            }
            GaitClip g = clip_of(c);
            if (usable(g, needs_motion) && (best.empty() || g.name.size() < best.name.size())) {
                best = g;
            }
        }
        return best;
    };
    for (const ProjectClip& c : project.clips) {
        if (GaitClip g = clip_of(c); !g.file.empty() && exists(g.file)) {
            out.clips.emplace(bare(c.name), std::move(g));
        }
    }
    for (const auto& [name, clip] : out.clips) {
        std::string file = clip.file;
        std::transform(file.begin(), file.end(), file.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (file.ends_with("deathintro.hkx")) {
            out.death = clip;
        }
    }
    // Names seen in vanilla, humans first, then creatures by their schemes.
    out.idle = pick({"mt_idle", "mainidle", "main_idle", "main_idle_wolf", "idle", "clip_ground_idle_default_standing"}, [](const std::string&) { return false; }, false);
    out.walk = pick({"mt_walkforward", "walkforward", "mtwalkforward", "forward_walk", "walkforward00",
                     "walkforward00_wolf", "walkf", "mt_walk_f", "h2hwalkforward", "mtforward", "mt_forward",
                     "defaultforward", "forward", "clip_ground_locomotion_forward"},
                    [](const std::string& n) { return is_gait(n, "walk"); }, true);
    out.run = pick({"mt_runforward", "runforward", "mtrunforward", "forward_run", "runforward00", "runf",
                    "mt_run_f", "h2hrunforward", "mtfastforward", "mt_fastforward", "defaultfastforward",
                    "fastforward"},
                   [](const std::string& n) { return is_gait(n, "run"); }, true);
    const auto turn = [&](std::initializer_list<std::string_view> names) {
        return pick(names, [](const std::string&) { return false; }, false);
    };
    out.turn_left = turn({"clip_ground_standing_turn_left"});
    out.turn_right = turn({"clip_ground_standing_turn_right"});
    // A run no faster than the walk is mislabelled; walk instead.
    if (!out.run.empty() && !out.walk.empty() && out.run.speed <= out.walk.speed) {
        out.run = GaitClip{};
    }
    return out;
}

Stance find_stance(const Locomotion& locomotion, std::string_view prefix) {
    Stance out;
    const std::string p = ascii_lower(std::string(prefix));
    if (p.empty()) {
        return out;
    }
    const auto first = [&](std::initializer_list<std::string> names, bool needs_motion) {
        for (const std::string& name : names) {
            const auto it = locomotion.clips.find(name);
            if (it != locomotion.clips.end() && (!needs_motion || it->second.speed > 1.0F)) {
                return it->second;
            }
        }
        return GaitClip{};
    };
    // Names seen in vanilla: humans `1hm_idle`, `1hm_walkforward`, `bow_idledrawn`; the draugr's
    // `clip_1hm_idle`, `1hmforward`, `1hmfastforward`; creatures `combatidle`.
    out.idle = first({"clip_ground_" + p + "_default_standing_idle", p + "_idle", p + "_idledrawn", p + "idle", p + "_sword_idle", "clip_" + p + "_idle",
                      p + "_idle_drawn"},
                     false);
    out.walk = first({p == "combat" ? "clip_ground_locomotion_forward" : p + "_walkforward", p + "_walkforward", p + "walkforward", p + "forward", p + "_forward", p + "_forward_walk"}, true);
    out.run = first({p + "_runforward", p + "runforward", p + "fastforward", p + "_fastforward", p + "_forward_run"},
                    true);
    return out;
}

} // namespace skydot
