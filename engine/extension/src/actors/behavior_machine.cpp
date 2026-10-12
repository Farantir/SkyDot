// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors/behavior_machine.hpp"

#include "animation_generated.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace skydot {
namespace {

namespace afb = bethconv::pack::afb;

bool same_event(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

constexpr int k_max_chain = 64; ///< Events handled in one step, against loops.

} // namespace

BehaviorDef behavior_from_asset(const afb::Animation& asset) {
    BehaviorDef def;
    const auto* graphs = asset.behavior_graphs();
    if (graphs == nullptr || graphs->size() == 0) {
        return def;
    }
    const auto* graph = graphs->Get(0);
    const auto* machines = graph->state_machines();
    if (machines == nullptr || machines->size() == 0) {
        return def;
    }
    const auto event_name = [&](std::int32_t id) {
        const auto* names = graph->event_names();
        if (names == nullptr || id < 0 || static_cast<flatbuffers::uoffset_t>(id) >= names->size()) {
            return std::string();
        }
        return names->Get(static_cast<flatbuffers::uoffset_t>(id))->str();
    };
    const auto transitions = [&](const auto* list, std::vector<BehaviorTransition>& to) {
        if (list == nullptr) {
            return;
        }
        for (const auto* t : *list) {
            // Conditions are not evaluated: see the header.
            to.push_back({event_name(t->event()), t->to_state()});
        }
    };
    // Clips: every clip generator of the file, indexed as the states say.
    if (const auto* generators = asset.clip_generators()) {
        for (const auto* g : *generators) {
            BehaviorClip clip;
            clip.name = g->name() != nullptr ? g->name()->str() : std::string();
            clip.animation = g->animation() != nullptr ? g->animation()->str() : std::string();
            clip.mode = g->mode();
            clip.speed = g->playback_speed();
            clip.start_time = g->start_time();
            clip.user_fraction = g->user_controlled_fraction();
            if (const auto* triggers = g->triggers()) {
                for (const auto* t : *triggers) {
                    clip.triggers.push_back({t->time(), event_name(t->event()), t->relative_to_end()});
                }
            }
            def.clips.push_back(std::move(clip));
        }
    }
    const auto* machine = machines->Get(0);
    def.start_state = machine->start_state();
    transitions(machine->wildcard_transitions(), def.wildcard);
    if (const auto* states = machine->states()) {
        for (const auto* s : *states) {
            BehaviorState state;
            state.id = s->id();
            state.name = s->name() != nullptr ? s->name()->str() : std::string();
            state.clip = s->generator();
            if (state.clip >= static_cast<int>(def.clips.size())) {
                state.clip = -1;
            }
            if (const auto* ids = s->enter_events()) {
                for (const auto id : *ids) {
                    state.enter_events.push_back(event_name(id));
                }
            }
            if (const auto* ids = s->exit_events()) {
                for (const auto id : *ids) {
                    state.exit_events.push_back(event_name(id));
                }
            }
            transitions(s->transitions(), state.transitions);
            def.states.push_back(std::move(state));
        }
    }
    return def;
}

BehaviorMachine::BehaviorMachine(BehaviorDef def) : def_(std::move(def)) {}

int BehaviorMachine::find_state(int id) const {
    for (std::size_t i = 0; i < def_.states.size(); ++i) {
        if (def_.states[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const BehaviorState* BehaviorMachine::state() const {
    return current_ >= 0 ? &def_.states[static_cast<std::size_t>(current_)] : nullptr;
}

int BehaviorMachine::clip() const {
    return current_ >= 0 ? def_.states[static_cast<std::size_t>(current_)].clip : -1;
}

void BehaviorMachine::start(BehaviorOutput& out) {
    const int index = find_state(def_.start_state);
    if (index < 0) {
        return;
    }
    change(index, "", out);
    drain(out);
}

void BehaviorMachine::raise(std::string event, bool echo, BehaviorOutput& out) {
    if (event.empty()) {
        return;
    }
    if (echo) {
        out.events.push_back(event);
    }
    queue_.push_back({std::move(event)});
}

void BehaviorMachine::change(int index, std::string_view cause, BehaviorOutput& out) {
    std::string from;
    if (current_ >= 0) {
        const BehaviorState& old = def_.states[static_cast<std::size_t>(current_)];
        from = old.name;
        if (old.clip >= 0) {
            held_clip_ = old.clip;
            held_time_ = time_;
        }
        for (const auto& e : old.exit_events) {
            raise(e, true, out);
        }
    }
    current_ = index;
    const BehaviorState& now = def_.states[static_cast<std::size_t>(index)];
    time_ = 0.0F;
    backwards_ = false;
    if (now.clip >= 0) {
        const BehaviorClip& clip = def_.clips[static_cast<std::size_t>(now.clip)];
        time_ = clip.mode == 2 ? clip.user_fraction * clip.duration : clip.start_time;
        fired_.assign(clip.triggers.size(), false);
    } else {
        fired_.clear();
    }
    out.changes.push_back({from, now.name, std::string(cause)});
    for (const auto& e : now.enter_events) {
        raise(e, true, out);
    }
}

void BehaviorMachine::drain(BehaviorOutput& out, bool* first_matched) {
    for (int guard = 0; !queue_.empty() && guard < k_max_chain; ++guard) {
        const std::string event = std::move(queue_.front().event);
        queue_.erase(queue_.begin());
        if (current_ < 0) {
            continue;
        }
        const BehaviorState& here = def_.states[static_cast<std::size_t>(current_)];
        const BehaviorTransition* taken = nullptr;
        for (const auto& t : here.transitions) {
            if (same_event(t.event, event)) {
                taken = &t;
                break;
            }
        }
        if (taken == nullptr) {
            for (const auto& t : def_.wildcard) {
                if (same_event(t.event, event)) {
                    taken = &t;
                    break;
                }
            }
        }
        if (guard == 0 && first_matched != nullptr) {
            *first_matched = taken != nullptr;
        }
        if (taken == nullptr) {
            continue;
        }
        const int target = find_state(taken->to_state);
        if (target < 0 || target == current_) {
            continue;
        }
        change(target, event, out);
    }
    queue_.clear();
}

void BehaviorMachine::send(std::string_view event, BehaviorOutput& out) {
    raise(std::string(event), false, out);
    drain(out, &out.accepted);
}

void BehaviorMachine::advance(float seconds, BehaviorOutput& out) {
    const int clip_index = clip();
    if (clip_index < 0 || seconds <= 0.0F) {
        return;
    }
    const BehaviorClip& clip = def_.clips[static_cast<std::size_t>(clip_index)];
    if (clip.mode == 2) {
        return; // Held at the user-controlled fraction.
    }
    const float length = clip.duration;
    const float step = seconds * clip.speed;
    // Triggers whose time has come, earliest first; the ones of a clip that
    // wraps or turns fire up to its end before the next lap starts over.
    const auto fire = [&](float until) {
        struct Due {
            float at;
            std::size_t index;
        };
        std::vector<Due> due;
        for (std::size_t i = 0; i < clip.triggers.size(); ++i) {
            const BehaviorTrigger& t = clip.triggers[i];
            const float at = t.relative_to_end ? length + t.time : t.time;
            if (!fired_[i] && at <= until + 1e-6F) {
                fired_[i] = true;
                due.push_back({at, i});
            }
        }
        std::ranges::stable_sort(due, {}, &Due::at);
        for (const Due& d : due) {
            raise(clip.triggers[d.index].event, true, out);
        }
    };
    if (clip.mode == 0 || length <= 0.0F) {
        time_ = length > 0.0F ? std::min(time_ + step, length) : 0.0F;
    } else if (clip.mode == 1) {
        time_ += step;
        if (time_ >= length) {
            fire(length);
            time_ = std::fmod(time_, length);
            fired_.assign(fired_.size(), false);
        }
    } else {
        time_ += backwards_ ? -step : step;
        if (time_ >= length) {
            fire(length);
            time_ = std::max(0.0F, 2.0F * length - time_);
            backwards_ = true;
            fired_.assign(fired_.size(), false);
        } else if (time_ <= 0.0F) {
            time_ = -time_;
            backwards_ = false;
            fired_.assign(fired_.size(), false);
        }
    }
    fire(time_);
    drain(out);
}

} // namespace skydot
