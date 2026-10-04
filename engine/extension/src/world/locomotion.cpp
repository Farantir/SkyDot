// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/locomotion.hpp"

#include "animation_generated.h"
#include "assets/vpath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skydot {
namespace {

namespace afb = bethconv::pack::afb;

constexpr std::uint32_t k_animation_format = 1;

const afb::Animation* read_asset(const godot::PackedByteArray& bytes) {
    if (bytes.is_empty()) {
        return nullptr;
    }
    flatbuffers::Verifier verifier(bytes.ptr(), static_cast<std::size_t>(bytes.size()));
    if (!afb::VerifyAnimationBuffer(verifier)) {
        return nullptr;
    }
    const auto* root = afb::GetAnimation(bytes.ptr());
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

std::string normalize_path(const std::string& path) {
    std::string s = ascii_lower(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto end = s.find('/', start);
        if (end == std::string::npos) {
            end = s.size();
        }
        const std::string part = s.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = end + 1;
    }
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) {
            out += '/';
        }
        out += p;
    }
    return out;
}

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
    const std::string folder = path.substr(0, slash + 1);
    const std::string project = path.substr(slash + 1, dot - slash - 1);

    // The project's own files, else its entry in the single file (the only
    // place the DLC creatures' are).
    using Strings = flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>;
    using Clips = flatbuffers::Vector<flatbuffers::Offset<afb::ProjectClip>>;
    using Motions = flatbuffers::Vector<flatbuffers::Offset<afb::Motion>>;
    const Strings* files = nullptr;
    const Clips* clips = nullptr;
    const Motions* motion_list = nullptr;
    const godot::PackedByteArray project_bytes = bytes("meshes/animationdata/" + project + ".txt");
    const godot::PackedByteArray bound_bytes = bytes("meshes/animationdata/boundanims/anims_" + project + ".txt");
    godot::PackedByteArray single_bytes;
    if (const auto* proj = read_asset(project_bytes); proj != nullptr && proj->project_clips() != nullptr) {
        files = proj->project_files();
        clips = proj->project_clips();
        if (const auto* bound = read_asset(bound_bytes)) {
            motion_list = bound->motions();
        }
    } else {
        single_bytes = bytes("meshes/animationdatasinglefile.txt");
        if (const auto* single = read_asset(single_bytes); single != nullptr && single->projects() != nullptr) {
            for (const auto* p : *single->projects()) {
                if (p->name() != nullptr && ascii_lower(p->name()->str()) == project) {
                    files = p->files();
                    clips = p->clips();
                    motion_list = p->motions();
                    break;
                }
            }
        }
    }
    if (clips == nullptr) {
        out.missing = "no animationdata for " + project;
        return out;
    }
    // Clip name -> animation file, from the project's behaviour graphs.
    std::unordered_map<std::string, std::string> generators;
    if (files != nullptr) {
        for (const auto* f : *files) {
            const std::string file = normalize_path(f->str());
            if (!file.starts_with("behaviors")) {
                continue;
            }
            const godot::PackedByteArray graph_bytes = bytes(folder + file);
            const auto* graph = read_asset(graph_bytes);
            if (graph == nullptr || graph->clip_generators() == nullptr) {
                continue;
            }
            for (const auto* g : *graph->clip_generators()) {
                if (g->name() != nullptr && g->animation() != nullptr) {
                    generators.emplace(ascii_lower(g->name()->str()), normalize_path(folder + g->animation()->str()));
                }
            }
        }
    }
    if (generators.empty()) {
        out.missing = "no clip generators in the behaviours of " + project;
        return out;
    }
    std::unordered_map<std::uint32_t, Motion> motions;
    if (motion_list != nullptr) {
        for (const auto* m : *motion_list) {
            Motion motion{m->duration(), 0.0F};
            if (const auto* keys = m->translations(); keys != nullptr && keys->size() > 0) {
                motion.forward = keys->Get(keys->size() - 1)->y();
            }
            motions[m->animation()] = motion;
        }
    }

    const auto clip_of = [&](const afb::ProjectClip& c) {
        GaitClip g;
        g.name = c.name() != nullptr ? c.name()->str() : std::string();
        g.playback = c.speed() > 0.0F ? c.speed() : 1.0F;
        if (const auto it = generators.find(ascii_lower(g.name)); it != generators.end()) {
            g.file = it->second;
        }
        if (const auto it = motions.find(c.animation()); it != motions.end() && it->second.duration > 0.0F) {
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
            for (const auto* c : *clips) {
                if (c->name() != nullptr && bare(c->name()->str()) == want) {
                    if (GaitClip g = clip_of(*c); usable(g, needs_motion)) {
                        return g;
                    }
                }
            }
        }
        GaitClip best;
        for (const auto* c : *clips) {
            if (c->name() == nullptr || !accept(c->name()->str())) {
                continue;
            }
            GaitClip g = clip_of(*c);
            if (usable(g, needs_motion) && (best.empty() || g.name.size() < best.name.size())) {
                best = g;
            }
        }
        return best;
    };
    // Names seen in vanilla, humans first, then creatures by their schemes.
    out.idle = pick({"mt_idle", "mainidle", "idle"}, [](const std::string&) { return false; }, false);
    out.walk = pick({"mt_walkforward", "walkforward", "mtwalkforward", "forward_walk", "walkforward00",
                     "walkforward00_wolf", "walkf", "mt_walk_f", "h2hwalkforward", "mtforward", "mt_forward",
                     "defaultforward", "forward"},
                    [](const std::string& n) { return is_gait(n, "walk"); }, true);
    out.run = pick({"mt_runforward", "runforward", "mtrunforward", "forward_run", "runforward00", "runf",
                    "mt_run_f", "h2hrunforward", "mtfastforward", "mt_fastforward", "defaultfastforward",
                    "fastforward"},
                   [](const std::string& n) { return is_gait(n, "run"); }, true);
    // A run no faster than the walk is mislabelled; walk instead.
    if (!out.run.empty() && !out.walk.empty() && out.run.speed <= out.walk.speed) {
        out.run = GaitClip{};
    }
    return out;
}

} // namespace skydot
