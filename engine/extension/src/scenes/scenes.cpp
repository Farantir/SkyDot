// SPDX-License-Identifier: GPL-3.0-or-later
#include "scenes/scenes.hpp"

#include "data/fb_search.hpp"
#include "skydot_formats/flags.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace skydot::scenes {
namespace {

constexpr int k_max_rounds = 64; ///< Phases that finish at once in one update.

std::string hex(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

} // namespace

const wfb::Scene* Scenes::find(std::uint32_t scene) const { return lookup(world_.scenes(), scene); }

bool Scenes::has_flag(const wfb::Scene& scene, wfb::SceneFlags flag) { return formats::has_flag(scene.flags(), flag); }

std::string Scenes::name(const wfb::Scene& scene) const {
    return scene.editor_id() != nullptr && scene.editor_id()->size() != 0 ? scene.editor_id()->str() : hex(scene.id());
}

std::uint32_t Scenes::quest_of(std::uint32_t scene) const {
    const auto* s = find(scene);
    return s != nullptr ? s->quest() : 0;
}

std::vector<std::uint32_t> Scenes::actors_of(const wfb::Scene& scene) const {
    std::vector<std::uint32_t> out;
    if (scene.actors() != nullptr) {
        for (const auto* a : *scene.actors()) {
            out.push_back(host_.alias_ref(scene.quest(), a->alias()));
        }
    }
    return out;
}

bool Scenes::can_start(const wfb::Scene& scene, bool force, std::vector<std::uint32_t>& actors) {
    why_.clear();
    if (!host_.quest_running(scene.quest())) {
        why_ = "its quest is not running";
        return false;
    }
    if (!force && !host_.conditions_pass(scene.conditions(), scene.quest(), 0)) {
        why_ = "its conditions fail";
        return false;
    }
    actors = actors_of(scene);
    for (const auto actor : actors) {
        if (actor == 0 || !host_.actor_ready(actor)) {
            why_ = actor == 0 ? "an alias is empty" : hex(actor) + " is not loaded and alive";
            return false;
        }
    }
    if (!force) {
        // An actor already in a scene cannot take part in another.
        for (const auto& [id, run] : runs_) {
            const auto* other = find(id);
            if (other == nullptr) {
                continue;
            }
            for (const auto theirs : actors_of(*other)) {
                if (std::ranges::find(actors, theirs) != actors.end()) {
                    why_ = "an actor is in scene " + name(*other);
                    return false;
                }
            }
        }
    }
    return true;
}

bool Scenes::begin(std::uint32_t id, bool force) {
    const auto* scene = find(id);
    if (scene == nullptr || runs_.contains(id)) {
        return false;
    }
    std::vector<std::uint32_t> actors;
    if (!can_start(*scene, force, actors)) {
        return false;
    }
    if (force) {
        std::vector<std::uint32_t> others;
        for (const auto& [other_id, run] : runs_) {
            const auto* other = find(other_id);
            if (other == nullptr) {
                continue;
            }
            for (const auto theirs : actors_of(*other)) {
                if (std::ranges::find(actors, theirs) != actors.end()) {
                    others.push_back(other_id);
                    break;
                }
            }
        }
        for (const auto other : others) {
            stop(other);
        }
    }
    waiting_.erase(id);
    Run run;
    run.actions.resize(scene->actions() != nullptr ? scene->actions()->size() : 0);
    runs_[id] = std::move(run);
    host_.log("scene " + name(*scene) + " start");
    fragment(*scene, scene->begin_fragment() != nullptr ? scene->begin_fragment()->str() : "");
    if (runs_.contains(id)) {
        enter(*scene, runs_[id], 0);
    }
    return true;
}

void Scenes::fragment(const wfb::Scene& scene, const std::string& function) {
    if (!function.empty() && scene.fragment_script() != nullptr && scene.fragment_script()->size() != 0) {
        host_.run_fragment(scene.id(), scene.fragment_script()->str(), function);
    }
}

void Scenes::phase_fragment(const wfb::Scene& scene, std::int32_t phase, bool on_end) {
    if (scene.phase_fragments() == nullptr) {
        return;
    }
    for (const auto* f : *scene.phase_fragments()) {
        if (static_cast<std::int32_t>(f->phase()) == phase && f->on_end() == on_end && f->fragment() != nullptr) {
            fragment(scene, f->fragment()->str());
        }
    }
}

bool Scenes::enter(const wfb::Scene& scene, Run&, std::int32_t first) {
    const auto id = scene.id();
    const auto count = static_cast<std::int32_t>(scene.phases() != nullptr ? scene.phases()->size() : 0);
    for (std::int32_t p = first; runs_.contains(id); ++p) {
        auto& run = runs_[id];
        if (p >= count) {
            end(id, "end");
            return false;
        }
        const auto* phase = scene.phases()->Get(static_cast<flatbuffers::uoffset_t>(p));
        if (!host_.conditions_pass(phase->start_conditions(), scene.quest(), 0)) {
            host_.log("scene " + name(scene) + " skip phase " + std::to_string(p) + " (conditions)");
            for (std::size_t i = 0; i < run.actions.size(); ++i) {
                if (static_cast<std::int32_t>(scene.actions()->Get(static_cast<flatbuffers::uoffset_t>(i))->start_phase()) == p) {
                    run.actions[i].status = Status::done;
                }
            }
            continue;
        }
        run.phase = p;
        host_.log("scene " + name(scene) + " phase " + std::to_string(p));
        phase_fragment(scene, p, false);
        if (!runs_.contains(id)) {
            return false;
        }
        for (std::size_t i = 0; i < runs_[id].actions.size() && runs_.contains(id); ++i) {
            if (static_cast<std::int32_t>(scene.actions()->Get(static_cast<flatbuffers::uoffset_t>(i))->start_phase()) == p) {
                start_action(scene, runs_[id], i);
            }
        }
        return runs_.contains(id);
    }
    return false;
}

void Scenes::start_action(const wfb::Scene& scene, Run& run, std::size_t i) {
    const auto* a = scene.actions()->Get(static_cast<flatbuffers::uoffset_t>(i));
    auto& state = run.actions[i];
    const std::uint32_t actor = a->alias() >= 0 ? host_.alias_ref(scene.quest(), a->alias()) : 0;
    state.actor = actor;
    const std::string head = "scene " + name(scene) + " action " + std::to_string(a->index()) + " " +
                             std::to_string(a->type()) + " " + hex(actor);
    switch (a->type()) {
    case 0: {
        std::vector<dialogue::Line> lines;
        if (actor != 0 && host_.actor_ready(actor)) {
            lines = host_.say(actor, a->topic());
        }
        host_.log(head + (lines.empty() ? " (nothing to say)" : ""));
        if (lines.empty()) {
            state.status = Status::done;
        } else {
            state.status = Status::active;
            state.requested = true;
            outbox_.push_back({scene.id(), a->index(), actor, std::move(lines)});
        }
        break;
    }
    case 1:
        host_.log(head);
        if (a->packages() != nullptr && actor != 0) {
            for (const auto package : *a->packages()) {
                host_.push_package(actor, package);
                run.pushed.push_back({actor, package, a->end_phase()});
            }
        }
        state.status = Status::done;
        break;
    default:
        host_.log(head + " " + std::to_string(a->timer_seconds()) + " s");
        state.status = Status::active;
        state.left = a->timer_seconds();
        break;
    }
}

void Scenes::finish_phase(const wfb::Scene& scene, Run& run) {
    const auto id = scene.id();
    const auto phase = run.phase;
    phase_fragment(scene, phase, true);
    if (!runs_.contains(id)) {
        return;
    }
    // Packages of actions that end here come off their actors.
    auto& now = runs_[id];
    std::vector<Run::Pushed> keep;
    std::vector<Run::Pushed> drop;
    for (const auto& p : now.pushed) {
        (static_cast<std::int32_t>(p.end_phase) == phase ? drop : keep).push_back(p);
    }
    now.pushed = keep;
    for (const auto& p : drop) {
        host_.pop_package(p.actor, p.package);
    }
    enter(scene, runs_[id], phase + 1);
}

void Scenes::end(std::uint32_t id, const char* how) {
    const auto* scene = find(id);
    const auto it = runs_.find(id);
    if (scene == nullptr || it == runs_.end()) {
        return;
    }
    const auto pushed = it->second.pushed;
    runs_.erase(it);
    for (const auto& p : pushed) {
        host_.pop_package(p.actor, p.package);
    }
    host_.log("scene " + name(*scene) + " " + how);
    fragment(*scene, scene->end_fragment() != nullptr ? scene->end_fragment()->str() : "");
}

void Scenes::stop(std::uint32_t scene) { end(scene, "stop"); }

bool Scenes::action_complete(std::uint32_t id, std::uint32_t index) const {
    const auto* scene = find(id);
    const auto it = runs_.find(id);
    if (scene == nullptr || it == runs_.end() || scene->actions() == nullptr) {
        return true;
    }
    for (std::size_t i = 0; i < scene->actions()->size() && i < it->second.actions.size(); ++i) {
        if (scene->actions()->Get(static_cast<flatbuffers::uoffset_t>(i))->index() == index) {
            return it->second.actions[i].status == Status::done;
        }
    }
    return true;
}

void Scenes::action_finished(std::uint32_t id, std::uint32_t index) {
    const auto* scene = find(id);
    const auto it = runs_.find(id);
    if (scene == nullptr || it == runs_.end() || scene->actions() == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < scene->actions()->size() && i < it->second.actions.size(); ++i) {
        if (scene->actions()->Get(static_cast<flatbuffers::uoffset_t>(i))->index() == index) {
            it->second.actions[i].status = Status::done;
        }
    }
}

void Scenes::quest_started(std::uint32_t quest) {
    if (world_.scenes() == nullptr) {
        return;
    }
    for (const auto* scene : *world_.scenes()) {
        if (scene->quest() == quest && has_flag(*scene, wfb::SceneFlags::begin_on_quest_start) &&
            !runs_.contains(scene->id()) && !begin(scene->id(), false)) {
            waiting_.insert(scene->id());
            waited_[scene->id()] = why_;
            host_.log("scene " + name(*scene) + " waits: " + why_);
        }
    }
}

void Scenes::quest_stopped(std::uint32_t quest) {
    std::vector<std::uint32_t> stopping;
    for (const auto& [id, run] : runs_) {
        const auto* scene = find(id);
        if (scene != nullptr && scene->quest() == quest && has_flag(*scene, wfb::SceneFlags::stop_on_quest_end)) {
            stopping.push_back(id);
        }
    }
    for (const auto id : stopping) {
        stop(id);
    }
    std::erase_if(waiting_, [&](std::uint32_t id) { return quest_of(id) == quest; });
}

std::vector<DialogueRequest> Scenes::update(double seconds) {
    std::vector<std::uint32_t> retry(waiting_.begin(), waiting_.end());
    for (const auto id : retry) {
        if (!host_.quest_running(quest_of(id))) {
            waiting_.erase(id);
        } else if (begin(id, false)) {
            continue;
        } else if (why_ != waited_[id]) {
            waited_[id] = why_;
            if (const auto* scene = find(id)) {
                host_.log("scene " + name(*scene) + " waits: " + why_);
            }
        }
    }
    std::vector<std::uint32_t> ids;
    for (const auto& [id, run] : runs_) {
        ids.push_back(id);
    }
    for (const auto id : ids) {
        const auto* scene = find(id);
        for (int round = 0; scene != nullptr && runs_.contains(id) && round < k_max_rounds; ++round) {
            auto& run = runs_[id];
            if (round == 0) {
                for (std::size_t i = 0; i < run.actions.size(); ++i) {
                    const auto* a = scene->actions()->Get(static_cast<flatbuffers::uoffset_t>(i));
                    auto& state = run.actions[i];
                    if (state.status != Status::active) {
                        continue;
                    }
                    if (a->type() == 2) {
                        state.left -= seconds;
                        if (state.left <= 0.0) {
                            state.status = Status::done;
                        }
                    } else if (a->type() == 0 && !state.requested) {
                        // Loaded from a save in mid-line: say it again.
                        state.requested = true;
                        auto lines = state.actor != 0 ? host_.say(state.actor, a->topic()) : std::vector<dialogue::Line>{};
                        if (lines.empty()) {
                            state.status = Status::done;
                        } else {
                            outbox_.push_back({id, a->index(), state.actor, std::move(lines)});
                        }
                    }
                }
            }
            if (run.phase < 0) {
                break;
            }
            bool all = true;
            for (std::size_t i = 0; i < run.actions.size(); ++i) {
                const auto* a = scene->actions()->Get(static_cast<flatbuffers::uoffset_t>(i));
                if (static_cast<std::int32_t>(a->end_phase()) == run.phase && run.actions[i].status != Status::done) {
                    all = false;
                }
            }
            const auto* phase = scene->phases()->Get(static_cast<flatbuffers::uoffset_t>(run.phase));
            if (!all || !host_.conditions_pass(phase->completion_conditions(), scene->quest(), 0)) {
                break;
            }
            finish_phase(*scene, run);
        }
    }
    return std::exchange(outbox_, {});
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> Scenes::pushed_packages() const {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    for (const auto& [id, run] : runs_) {
        for (const auto& p : run.pushed) {
            out.emplace_back(p.actor, p.package);
        }
    }
    return out;
}

void Scenes::clear() {
    runs_.clear();
    waiting_.clear();
    outbox_.clear();
}

void Scenes::adopt(Scenes&& other) {
    runs_ = std::move(other.runs_);
    waiting_.clear();
    outbox_.clear();
}

std::vector<std::uint8_t> Scenes::save() const {
    save::ByteWriter w;
    w.u32(static_cast<std::uint32_t>(runs_.size()));
    for (const auto& [id, run] : runs_) {
        w.u32(id);
        w.u32(static_cast<std::uint32_t>(run.phase));
        w.u32(static_cast<std::uint32_t>(run.actions.size()));
        for (const auto& a : run.actions) {
            w.u8(static_cast<std::uint8_t>(a.status));
            w.f64(a.left);
            w.u32(a.actor);
        }
        w.u32(static_cast<std::uint32_t>(run.pushed.size()));
        for (const auto& p : run.pushed) {
            w.u32(p.actor);
            w.u32(p.package);
            w.u32(p.end_phase);
        }
    }
    return w.out;
}

bool Scenes::load(save::ByteReader r, Scenes& out) {
    out.clear();
    std::uint32_t n = 0;
    if (!r.count(n, 12)) {
        return false;
    }
    for (std::uint32_t k = 0; k < n; ++k) {
        std::uint32_t id = 0;
        std::uint32_t phase = 0;
        std::uint32_t actions = 0;
        if (!r.u32(id) || !r.u32(phase) || !r.count(actions, 13)) {
            return false;
        }
        const auto* scene = out.find(id);
        if (scene == nullptr || scene->actions() == nullptr || scene->actions()->size() != actions) {
            return false;
        }
        Run run;
        run.phase = static_cast<std::int32_t>(phase);
        for (std::uint32_t i = 0; i < actions; ++i) {
            std::uint8_t status = 0;
            ActionRun a;
            if (!r.u8(status) || !r.f64(a.left) || !r.u32(a.actor) || status > 2) {
                return false;
            }
            a.status = static_cast<Status>(status);
            run.actions.push_back(a);
        }
        std::uint32_t pushed = 0;
        if (!r.count(pushed, 12)) {
            return false;
        }
        for (std::uint32_t i = 0; i < pushed; ++i) {
            Run::Pushed p{};
            if (!r.u32(p.actor) || !r.u32(p.package) || !r.u32(p.end_phase)) {
                return false;
            }
            run.pushed.push_back(p);
        }
        out.runs_[id] = std::move(run);
    }
    return r.at_end();
}

} // namespace skydot::scenes
