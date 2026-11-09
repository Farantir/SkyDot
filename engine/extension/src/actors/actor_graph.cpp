// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors/actor_graph.hpp"

#include "assets/vpath.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace skydot {
namespace {

namespace afb = bethconv::pack::afb;

/// Joins behaviour graphs into one definition, expanding references.
class Expander {
public:
    Expander(ActorGraphDef& out, const AssetReader& read) : out_(out), read_(read) {}

    /// The root machines of the graph file `file` (a virtual path) in the joined graph; nothing if the
    /// pack lacks it or it has no machines. A file included twice is joined once.
    std::vector<int> include(const std::string& file) {
        if (const auto done = included_.find(file); done != included_.end()) {
            return done->second;
        }
        included_[file] = {}; // a reference back to a graph being joined finds nothing
        const std::vector<std::uint8_t> bytes = read_(file);
        const auto* asset = animation_asset(bytes);
        if (asset == nullptr) {
            return {};
        }
        BehaviorGraphDef graph = behavior_graph_from_asset(*asset);
        if (graph.machines.empty()) {
            return {};
        }
        // Variables are joined by name; the first graph to name one (the root's) gives its value.
        for (const auto& [name, value] : graph.variables) {
            out_.graph.variables.try_emplace(name, value);
        }
        const int machine_base = static_cast<int>(out_.graph.machines.size());
        const int clip_base = static_cast<int>(out_.graph.clips.size());
        for (BehaviorClip& clip : graph.clips) {
            out_.clip_files.push_back(normalize_path(out_.project.folder + clip.animation));
            out_.graph.clips.push_back(std::move(clip));
        }
        for (BehaviorDef& machine : graph.machines) {
            for (BehaviorState& state : machine.states) {
                if (state.clip >= 0) {
                    state.clip += clip_base;
                }
                for (int& id : state.nested) {
                    id += machine_base;
                }
            }
            out_.graph.machines.push_back(std::move(machine));
        }
        std::vector<int> roots;
        for (const int root : graph.roots) {
            roots.push_back(root + machine_base);
        }
        included_[file] = roots;
        // Expand the references of the machines just added (they may add more).
        const int end = static_cast<int>(out_.graph.machines.size());
        for (int m = machine_base; m < end; ++m) {
            for (std::size_t s = 0; s < out_.graph.machines[static_cast<std::size_t>(m)].states.size(); ++s) {
                const std::string behavior =
                    out_.graph.machines[static_cast<std::size_t>(m)].states[s].behavior;
                if (behavior.empty()) {
                    continue;
                }
                const std::vector<int> referenced = include(normalize_path(out_.project.folder + behavior));
                auto& nested = out_.graph.machines[static_cast<std::size_t>(m)].states[s].nested;
                for (const int id : referenced) {
                    if (std::ranges::find(nested, id) == nested.end()) {
                        nested.push_back(id);
                    }
                }
            }
        }
        return roots;
    }

private:
    ActorGraphDef& out_;
    const AssetReader& read_;
    std::unordered_map<std::string, std::vector<int>> included_;
};

/// The machines' depths: roots 0, a state's nested machines one deeper than the machine.
std::vector<int> depths(const BehaviorGraphDef& graph) {
    std::vector<int> depth(graph.machines.size(), -1);
    std::vector<int> queue;
    for (const int root : graph.roots) {
        if (root >= 0 && static_cast<std::size_t>(root) < depth.size() && depth[static_cast<std::size_t>(root)] < 0) {
            depth[static_cast<std::size_t>(root)] = 0;
            queue.push_back(root);
        }
    }
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const int m = queue[i];
        for (const BehaviorState& state : graph.machines[static_cast<std::size_t>(m)].states) {
            for (const int id : state.nested) {
                if (depth[static_cast<std::size_t>(id)] < 0) {
                    depth[static_cast<std::size_t>(id)] = depth[static_cast<std::size_t>(m)] + 1;
                    queue.push_back(id);
                }
            }
        }
    }
    return depth;
}

float lerp(float a, float b, float f) { return a + (b - a) * f; }

/// The skeleton of a skeleton file whose bones are `Ragdoll_*` (the ragdoll skeleton beside the
/// animation one), with model-space positions from its reference pose; empty if there is none.
std::vector<ActorGraphDef::RagdollBone> ragdoll_skeleton(const std::vector<std::uint8_t>& bytes) {
    std::vector<ActorGraphDef::RagdollBone> out;
    const auto* asset = animation_asset(bytes);
    if (asset == nullptr || asset->skeletons() == nullptr) {
        return out;
    }
    for (const auto* skeleton : *asset->skeletons()) {
        const auto* bones = skeleton->bones();
        const auto* parents = skeleton->parents();
        const auto* pose = skeleton->reference_pose();
        if (bones == nullptr || parents == nullptr || pose == nullptr || bones->size() == 0 ||
            parents->size() != bones->size() || pose->size() != bones->size() ||
            !bones->Get(0)->string_view().starts_with("Ragdoll_")) {
            continue;
        }
        struct Model {
            float t[3];
            float q[4]; // x, y, z, w
        };
        const auto rotate = [](const float* q, const float* v, float* r) {
            // r = q v q*, with t = 2 (q.xyz x v)
            const float tx = 2.0F * (q[1] * v[2] - q[2] * v[1]);
            const float ty = 2.0F * (q[2] * v[0] - q[0] * v[2]);
            const float tz = 2.0F * (q[0] * v[1] - q[1] * v[0]);
            r[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
            r[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
            r[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
        };
        std::vector<Model> model(bones->size());
        for (flatbuffers::uoffset_t i = 0; i < bones->size(); ++i) {
            const auto* p = pose->Get(i);
            const float local_t[3] = {p->tx(), p->ty(), p->tz()};
            const float local_q[4] = {p->rx(), p->ry(), p->rz(), p->rw()};
            const int parent = parents->Get(i);
            Model& m = model[i];
            if (parent < 0 || static_cast<flatbuffers::uoffset_t>(parent) >= i) {
                std::copy(local_t, local_t + 3, m.t);
                std::copy(local_q, local_q + 4, m.q);
            } else {
                const Model& pm = model[static_cast<std::size_t>(parent)];
                float r[3];
                rotate(pm.q, local_t, r);
                for (int a = 0; a < 3; ++a) {
                    m.t[a] = pm.t[a] + r[a];
                }
                const float* a = pm.q;
                const float* b = local_q;
                m.q[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
                m.q[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
                m.q[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
                m.q[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
            }
            out.push_back({bones->Get(i)->str(), m.t[0], m.t[1], m.t[2]});
        }
        return out;
    }
    return out;
}

} // namespace

std::shared_ptr<const ActorGraphDef> build_actor_graph(const std::string& behaviour, const AssetReader& read) {
    auto def = std::make_shared<ActorGraphDef>();
    def->project = read_animation_project(behaviour, read);
    if (!def->project.missing.empty()) {
        def->missing = def->project.missing;
        return def;
    }
    // The character: the project file entry that reads as a character ("Characters\...",
    // "Characters Female\...", "LargeCharacters\..."); behaviours are not looked at.
    std::string root;
    for (const std::string& file : def->project.files) {
        if (file.starts_with("behaviors") || file.find("character") == std::string::npos) {
            continue;
        }
        const std::vector<std::uint8_t> bytes = read(def->project.folder + file);
        const auto* asset = animation_asset(bytes);
        if (asset != nullptr && asset->characters() != nullptr && asset->characters()->size() > 0) {
            if (const auto* animations = asset->characters()->Get(0)->animations()) {
                for (const auto* a : *animations) {
                    def->character_animations.push_back(a->str());
                }
            }
            if (const auto* ragdoll = asset->characters()->Get(0)->ragdoll(); ragdoll != nullptr && ragdoll->size() > 0) {
                def->ragdoll_bones = ragdoll_skeleton(read(normalize_path(def->project.folder + ragdoll->str())));
            }
            const auto* behavior = asset->characters()->Get(0)->behavior();
            if (behavior != nullptr) {
                root = normalize_path(def->project.folder + behavior->str());
            }
            break;
        }
    }
    if (root.empty()) {
        def->missing = "no character with a behaviour in " + def->project.name;
        return def;
    }
    def->behaviour = root;
    Expander expander(*def, read);
    def->graph.roots = expander.include(root);
    if (def->graph.machines.empty() || def->graph.roots.empty()) {
        def->missing = "no behaviour graph " + root;
        return def;
    }
    // A behaviour names one file for every character (Animations\male\Chair_RightEnter.hkx); a character
    // that lists its own of that name (Animations\female\Chair_RightEnter.hkx) plays that one.
    {
        const auto base = [](const std::string& path) { return path.substr(path.rfind('/') + 1); };
        std::unordered_map<std::string, std::string> listed;
        std::unordered_map<std::string, std::string> by_name; ///< Empty path: the name is listed twice.
        for (const std::string& name : def->character_animations) {
            const std::string path = normalize_path(def->project.folder + name);
            listed.emplace(path, path);
            const auto [it, fresh] = by_name.emplace(base(path), path);
            if (!fresh && it->second != path) {
                it->second.clear();
            }
        }
        for (std::string& file : def->clip_files) {
            if (listed.contains(file)) {
                continue;
            }
            if (const auto it = by_name.find(base(file)); it != by_name.end() && !it->second.empty()) {
                file = it->second;
            }
        }
    }
    def->depth = depths(def->graph);
    def->parents.assign(def->graph.machines.size(), {});
    for (std::size_t m = 0; m < def->graph.machines.size(); ++m) {
        for (const BehaviorState& state : def->graph.machines[m].states) {
            for (const int id : state.nested) {
                def->parents[static_cast<std::size_t>(id)].emplace_back(static_cast<int>(m), state.id);
            }
        }
    }
    return def;
}

std::vector<BehaviorRagdollControl> ActorGraphDef::ragdoll_controls(const std::string& clip_file) const {
    for (std::size_t m = 0; m < graph.machines.size(); ++m) {
        for (const BehaviorState& state : graph.machines[m].states) {
            if (state.clip < 0 || static_cast<std::size_t>(state.clip) >= clip_files.size() ||
                clip_files[static_cast<std::size_t>(state.clip)] != clip_file) {
                continue;
            }
            // The chain of nesting states, innermost first, then reversed: outermost first.
            std::vector<const BehaviorState*> chain{&state};
            std::size_t machine = m;
            for (int guard = 0; guard < 64 && !parents[machine].empty(); ++guard) {
                const auto [parent, id] = parents[machine].front();
                const auto& states = graph.machines[static_cast<std::size_t>(parent)].states;
                const auto it = std::ranges::find_if(states, [id](const BehaviorState& s) { return s.id == id; });
                if (it == states.end()) {
                    break;
                }
                chain.push_back(&*it);
                machine = static_cast<std::size_t>(parent);
            }
            std::vector<BehaviorRagdollControl> out;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                out.insert(out.end(), (*it)->ragdoll.begin(), (*it)->ragdoll.end());
            }
            return out;
        }
    }
    return {};
}

std::optional<RootMotion> ActorGraphDef::root_motion(const std::string& clip, float time) const {
    const ProjectClip* project_clip = project.clip(clip);
    if (project_clip == nullptr) {
        return std::nullopt;
    }
    const auto it = project.motions.find(project_clip->animation);
    if (it == project.motions.end()) {
        return std::nullopt;
    }
    const ProjectMotion& motion = it->second;
    RootMotion out;
    if (!motion.translations.empty()) {
        const auto& keys = motion.translations;
        std::size_t next = 0;
        while (next < keys.size() && keys[next][0] < time) {
            ++next;
        }
        if (next == 0) {
            // Before the first key the body comes from where the clip starts: the origin.
            const float f = keys[0][0] > 0.0F ? std::max(time, 0.0F) / keys[0][0] : 1.0F;
            out.x = lerp(0.0F, keys[0][1], f);
            out.y = lerp(0.0F, keys[0][2], f);
            out.z = lerp(0.0F, keys[0][3], f);
        } else if (next == keys.size()) {
            out.x = keys.back()[1];
            out.y = keys.back()[2];
            out.z = keys.back()[3];
        } else {
            const auto& a = keys[next - 1];
            const auto& b = keys[next];
            const float span = b[0] - a[0];
            const float f = span > 0.0F ? (time - a[0]) / span : 1.0F;
            out.x = lerp(a[1], b[1], f);
            out.y = lerp(a[2], b[2], f);
            out.z = lerp(a[3], b[3], f);
        }
    }
    if (!motion.rotations.empty()) {
        const auto& keys = motion.rotations;
        std::size_t next = 0;
        while (next < keys.size() && keys[next][0] < time) {
            ++next;
        }
        std::array<float, 4> q{};
        if (next == keys.size()) {
            const auto& k = keys.back();
            q = {k[1], k[2], k[3], k[4]};
        } else if (next == 0) {
            // From no turn at the start of the clip to the first key.
            const auto& b = keys[0];
            const float f = b[0] > 0.0F ? std::max(time, 0.0F) / b[0] : 1.0F;
            const float dot = b[4];
            const float sign = dot < 0.0F ? -1.0F : 1.0F;
            q = {lerp(0.0F, sign * b[1], f), lerp(0.0F, sign * b[2], f), lerp(0.0F, sign * b[3], f),
                 lerp(1.0F, sign * b[4], f)};
        } else {
            const auto& a = keys[next - 1];
            const auto& b = keys[next];
            const float span = b[0] - a[0];
            const float f = span > 0.0F ? (time - a[0]) / span : 1.0F;
            // Normalised lerp along the shorter arc.
            const float dot = a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4];
            const float sign = dot < 0.0F ? -1.0F : 1.0F;
            for (std::size_t i = 0; i < 4; ++i) {
                q[i] = lerp(a[i + 1], sign * b[i + 1], f);
            }
        }
        const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (length > 0.0F) {
            for (float& c : q) {
                c /= length;
            }
            out.yaw = std::atan2(2.0F * (q[3] * q[2] + q[0] * q[1]), 1.0F - 2.0F * (q[1] * q[1] + q[2] * q[2]));
        }
    }
    return out;
}

std::shared_ptr<const ActorGraphDef> ActorGraphLibrary::graph(const std::string& behaviour) {
    std::string key = normalize_path(behaviour);
    if (!key.starts_with("meshes/")) {
        key = "meshes/" + key;
    }
    auto it = graphs_.find(key);
    if (it == graphs_.end()) {
        it = graphs_.emplace(key, build_actor_graph(key, read_)).first;
    }
    return it->second;
}

float ActorGraphLibrary::clip_duration(const std::string& vpath) {
    const auto it = durations_.find(vpath);
    if (it != durations_.end()) {
        return it->second;
    }
    float seconds = 0.0F;
    const std::vector<std::uint8_t> bytes = read_(vpath);
    if (const auto* asset = animation_asset(bytes); asset != nullptr && asset->clips() != nullptr && asset->clips()->size() > 0) {
        seconds = asset->clips()->Get(0)->duration();
    }
    durations_.emplace(vpath, seconds);
    return seconds;
}

ActorGraph::ActorGraph(std::shared_ptr<const ActorGraphDef> def, std::function<float(const std::string&)> duration)
    : def_(std::move(def)), duration_(std::move(duration)), set_(def_->graph) {
    known_.assign(def_->graph.clips.size(), false);
}

void ActorGraph::resolve_durations() {
    for (const int m : set_.running()) {
        const int clip = set_.machine(m).clip();
        if (clip >= 0 && !known_[static_cast<std::size_t>(clip)]) {
            known_[static_cast<std::size_t>(clip)] = true;
            set_.set_clip_duration(clip, duration_ ? duration_(def_->clip_files[static_cast<std::size_t>(clip)]) : 0.0F);
        }
    }
}

void ActorGraph::configure(bool player) {
    set_.set_variable("IsNPC", player ? 0.0F : 1.0F);
    set_.set_variable("IsPlayer", player ? 1.0F : 0.0F);
    set_.set_variable("IsFirstPerson", 0.0F);
    set_.set_variable("i1stPerson", 0.0F);
}

BehaviorOutput ActorGraph::start() {
    BehaviorOutput out;
    set_.start(out);
    resolve_durations();
    return out;
}

BehaviorOutput ActorGraph::send(std::string_view event) {
    BehaviorOutput out;
    set_.send(event, out);
    resolve_durations();
    return out;
}

BehaviorOutput ActorGraph::advance(float seconds) {
    BehaviorOutput out;
    set_.advance(seconds, out);
    resolve_durations();
    return out;
}

namespace {

/// The state id a running machine is in, -1 if it is not.
int state_id_of(const BehaviorSet& set, int machine) {
    const BehaviorState* state = set.machine(machine).state();
    return state != nullptr ? state->id : -1;
}

} // namespace

BehaviorOutput ActorGraph::send_entering(std::string_view event) {
    const std::size_t count = set_.machine_count();
    std::vector<int> before(count, -1);
    for (const int m : set_.running()) {
        before[static_cast<std::size_t>(m)] = state_id_of(set_, m);
    }
    BehaviorOutput out;
    if (!set_.handles(event)) {
        // The machines with a wildcard for the event, and the way up from each to a running machine.
        struct Step {
            int machine;
            int state;
        };
        struct Node {
            int machine;
            std::vector<Step> down; ///< From the running machine down to `machine`.
        };
        std::vector<Node> queue;
        std::vector<bool> seen(count, false);
        for (std::size_t m = 0; m < count; ++m) {
            const auto& wildcard = def_->graph.machines[m].wildcard;
            const bool wants = std::ranges::any_of(wildcard, [&](const BehaviorTransition& t) {
                return t.event.size() == event.size() &&
                       std::ranges::equal(t.event, event, [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                       });
            });
            if (wants) {
                queue.push_back({static_cast<int>(m), {}});
                seen[m] = true;
            }
        }
        for (std::size_t q = 0; q < queue.size(); ++q) {
            const Node node = queue[q];
            bool found = false;
            for (const auto& [parent, state] : def_->parents[static_cast<std::size_t>(node.machine)]) {
                if (before[static_cast<std::size_t>(parent)] >= 0) {
                    // Put the running machines into the states on the way down.
                    std::vector<Step> path{{parent, state}};
                    path.insert(path.end(), node.down.begin(), node.down.end());
                    for (std::size_t i = 0; i < path.size(); ++i) {
                        // Each machine nested below starts in the state the next step enters.
                        set_.enter_state(path[i].machine, path[i].state, i + 1 < path.size() ? path[i + 1].state : -1, out);
                    }
                    found = true;
                    break;
                }
                if (!seen[static_cast<std::size_t>(parent)]) {
                    seen[static_cast<std::size_t>(parent)] = true;
                    std::vector<Step> down{{parent, state}};
                    down.insert(down.end(), node.down.begin(), node.down.end());
                    queue.push_back({parent, std::move(down)});
                }
            }
            if (found) {
                break;
            }
        }
    }
    BehaviorOutput sent;
    set_.send(event, sent);
    out.accepted = sent.accepted;
    out.changes.insert(out.changes.end(), sent.changes.begin(), sent.changes.end());
    out.events.insert(out.events.end(), sent.events.begin(), sent.events.end());
    resolve_durations();
    // The root-most machine that left a state is the one the furniture ends in.
    home_machine_ = -1;
    home_state_ = -1;
    for (std::size_t m = 0; m < count; ++m) {
        if (before[m] >= 0 && set_.machine(static_cast<int>(m)).state() != nullptr &&
            state_id_of(set_, static_cast<int>(m)) != before[m] &&
            (home_machine_ < 0 || def_->depth[m] < def_->depth[static_cast<std::size_t>(home_machine_)])) {
            home_machine_ = static_cast<int>(m);
            home_state_ = before[m];
        }
    }
    return out;
}

bool ActorGraph::in_furniture() const {
    return home_machine_ >= 0 && set_.machine(home_machine_).state() != nullptr &&
           state_id_of(set_, home_machine_) != home_state_;
}

std::optional<PlayingClip> ActorGraph::playing() const {
    int best = -1;
    for (const int m : set_.running()) {
        if (set_.machine(m).clip() < 0) {
            continue;
        }
        if (best < 0 || def_->depth[static_cast<std::size_t>(m)] > def_->depth[static_cast<std::size_t>(best)]) {
            best = m;
        }
    }
    if (best < 0) {
        return std::nullopt;
    }
    const BehaviorMachine& machine = set_.machine(best);
    const auto clip_index = static_cast<std::size_t>(machine.clip());
    const BehaviorClip& clip = set_.clips()[clip_index];
    return PlayingClip{clip.name, def_->clip_files[clip_index], machine.clip_time(), clip.mode, clip.speed};
}

std::optional<RootMotion> ActorGraph::root_motion() const {
    const auto now = playing();
    return now ? def_->root_motion(now->clip, now->time) : std::nullopt;
}

} // namespace skydot
