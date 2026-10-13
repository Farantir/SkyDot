// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors/behavior_machine.hpp"

#include "animation_generated.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <utility>

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

BehaviorGraphDef behavior_graph_from_asset(const afb::Animation& asset) {
    BehaviorGraphDef def;
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
    const auto machine_count = static_cast<int>(machines->size());
    for (const auto* machine : *machines) {
        BehaviorDef m;
        m.start_state = machine->start_state();
        transitions(machine->wildcard_transitions(), m.wildcard);
        if (const auto* states = machine->states()) {
            for (const auto* s : *states) {
                BehaviorState state;
                state.id = s->id();
                state.name = s->name() != nullptr ? s->name()->str() : std::string();
                state.clip = s->generator();
                if (state.clip >= static_cast<int>(def.clips.size())) {
                    state.clip = -1;
                }
                state.sequence = s->sequence() != nullptr ? s->sequence()->str() : std::string();
                if (const auto* nested = s->nested_machines()) {
                    for (const auto id : *nested) {
                        if (id >= 0 && id < machine_count) {
                            state.nested.push_back(id);
                        }
                    }
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
                m.states.push_back(std::move(state));
            }
        }
        def.machines.push_back(std::move(m));
    }
    if (const auto* roots = graph->root_machines()) {
        for (const auto id : *roots) {
            if (id >= 0 && id < machine_count) {
                def.roots.push_back(id);
            }
        }
    }
    if (def.roots.empty()) {
        def.roots.push_back(0); // an older asset, or a root that is not a machine
    }
    return def;
}

BehaviorDef behavior_from_asset(const afb::Animation& asset) {
    BehaviorGraphDef graph = behavior_graph_from_asset(asset);
    if (graph.machines.empty()) {
        return {};
    }
    BehaviorDef def = std::move(graph.machines[static_cast<std::size_t>(graph.roots.front())]);
    def.clips = std::move(graph.clips);
    return def;
}

BehaviorMachine::BehaviorMachine(BehaviorDef def) : def_(std::move(def)) {}

void BehaviorMachine::stop() {
    current_ = -1;
    time_ = 0.0F;
    held_clip_ = -1;
    held_time_ = 0.0F;
    backwards_ = false;
    fired_.clear();
    queue_.clear();
}

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

BehaviorMachine::Snapshot BehaviorMachine::snapshot() const {
    Snapshot s;
    s.state_id = current_ >= 0 ? def_.states[static_cast<std::size_t>(current_)].id : 0;
    s.time = time_;
    s.held_clip = held_clip_;
    s.held_time = held_time_;
    s.backwards = backwards_;
    s.fired = fired_;
    return s;
}

bool BehaviorMachine::restore(const Snapshot& s) {
    const int index = find_state(s.state_id);
    if (index < 0 || !std::isfinite(s.time) || !std::isfinite(s.held_time)) {
        return false;
    }
    const BehaviorState& state = def_.states[static_cast<std::size_t>(index)];
    const std::size_t triggers =
        state.clip >= 0 ? def_.clips[static_cast<std::size_t>(state.clip)].triggers.size() : 0;
    if (s.fired.size() != triggers || s.held_clip < -1 ||
        s.held_clip >= static_cast<int>(def_.clips.size())) {
        return false;
    }
    current_ = index;
    time_ = s.time;
    held_clip_ = s.held_clip;
    held_time_ = s.held_time;
    backwards_ = s.backwards;
    fired_ = s.fired;
    queue_.clear();
    return true;
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
    out.changes.push_back({from, now.name, std::string(cause), now.sequence, 0});
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

// ---- BehaviorSet ----------------------------------------------------------

namespace {
constexpr int k_max_broadcast = 64; ///< Events passed between machines in one step, against loops.
} // namespace

BehaviorSet::BehaviorSet(BehaviorGraphDef def) : def_(std::move(def)) {
    for (BehaviorDef& machine : def_.machines) {
        BehaviorDef copy = machine;
        copy.clips = def_.clips;
        machines_.emplace_back(std::move(copy));
    }
    running_.assign(machines_.size(), false);
}

std::vector<int> BehaviorSet::running() const {
    std::vector<int> out;
    for (std::size_t i = 0; i < running_.size(); ++i) {
        if (running_[i]) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

void BehaviorSet::merge(int machine, BehaviorOutput& part, BehaviorOutput& out) {
    for (BehaviorChange& change : part.changes) {
        change.machine = machine;
        out.changes.push_back(std::move(change));
    }
    for (std::string& event : part.events) {
        out.events.push_back(std::move(event));
    }
    out.accepted = out.accepted || part.accepted;
}

void BehaviorSet::refresh(BehaviorOutput& out) {
    for (int round = 0; round < 8; ++round) {
        std::vector<bool> wanted(machines_.size(), false);
        for (const int root : def_.roots) {
            if (root >= 0 && static_cast<std::size_t>(root) < wanted.size()) {
                wanted[static_cast<std::size_t>(root)] = true;
            }
        }
        for (std::size_t i = 0; i < machines_.size(); ++i) {
            const BehaviorState* state = running_[i] ? machines_[i].state() : nullptr;
            if (state != nullptr) {
                for (const int id : state->nested) {
                    wanted[static_cast<std::size_t>(id)] = true;
                }
            }
        }
        bool changed = false;
        for (std::size_t i = 0; i < machines_.size(); ++i) {
            if (wanted[i] && !running_[i]) {
                running_[i] = true;
                BehaviorOutput part;
                machines_[i].start(part);
                merge(static_cast<int>(i), part, out);
                changed = true;
            } else if (!wanted[i] && running_[i]) {
                running_[i] = false;
                machines_[i].stop();
                changed = true;
            }
        }
        if (!changed) {
            return;
        }
    }
}

void BehaviorSet::start(BehaviorOutput& out) { refresh(out); }

namespace {

constexpr std::uint32_t k_snapshot_magic = 0x47424B53; ///< "SKBG"
constexpr std::uint8_t k_snapshot_version = 1;

template <class T>
void put(std::vector<std::uint8_t>& out, T value) {
    std::uint8_t raw[sizeof(T)];
    std::memcpy(raw, &value, sizeof(T));
    out.insert(out.end(), raw, raw + sizeof(T));
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    template <class T>
    bool get(T& value) {
        if (bytes_.size() - pos_ < sizeof(T)) {
            return false;
        }
        std::memcpy(&value, bytes_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    [[nodiscard]] std::size_t left() const { return bytes_.size() - pos_; }
    [[nodiscard]] bool done() const { return pos_ == bytes_.size(); }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t pos_{};
};

} // namespace

std::vector<std::uint8_t> BehaviorSet::save() const {
    std::vector<std::uint8_t> out;
    if (std::ranges::none_of(running_, [](bool r) { return r; })) {
        return out;
    }
    put(out, k_snapshot_magic);
    put(out, k_snapshot_version);
    put(out, static_cast<std::uint32_t>(machines_.size()));
    for (std::size_t i = 0; i < machines_.size(); ++i) {
        const bool on = running_[i] && machines_[i].state() != nullptr;
        put(out, static_cast<std::uint8_t>(on ? 1 : 0));
        if (!on) {
            continue;
        }
        const BehaviorMachine::Snapshot s = machines_[i].snapshot();
        put(out, static_cast<std::int32_t>(s.state_id));
        put(out, s.time);
        put(out, static_cast<std::int32_t>(s.held_clip));
        put(out, s.held_time);
        put(out, static_cast<std::uint8_t>(s.backwards ? 1 : 0));
        put(out, static_cast<std::uint32_t>(s.fired.size()));
        for (const bool f : s.fired) {
            put(out, static_cast<std::uint8_t>(f ? 1 : 0));
        }
    }
    return out;
}

bool BehaviorSet::load(std::span<const std::uint8_t> bytes) {
    Reader in(bytes);
    std::uint32_t magic{};
    std::uint8_t version{};
    std::uint32_t count{};
    if (!in.get(magic) || !in.get(version) || !in.get(count) || magic != k_snapshot_magic ||
        version != k_snapshot_version || count != machines_.size()) {
        return false;
    }
    std::vector<BehaviorMachine> machines = machines_;
    std::vector<bool> running(machines.size(), false);
    bool any = false;
    for (std::size_t i = 0; i < machines.size(); ++i) {
        std::uint8_t on{};
        if (!in.get(on) || on > 1) {
            return false;
        }
        if (on == 0) {
            machines[i].stop();
            continue;
        }
        BehaviorMachine::Snapshot s;
        std::int32_t state_id{};
        std::int32_t held_clip{};
        std::uint8_t backwards{};
        std::uint32_t fired{};
        if (!in.get(state_id) || !in.get(s.time) || !in.get(held_clip) || !in.get(s.held_time) ||
            !in.get(backwards) || !in.get(fired) || backwards > 1 || fired > in.left()) {
            return false;
        }
        s.state_id = state_id;
        s.held_clip = held_clip;
        s.backwards = backwards != 0;
        for (std::uint32_t k = 0; k < fired; ++k) {
            std::uint8_t f{};
            if (!in.get(f) || f > 1) {
                return false;
            }
            s.fired.push_back(f != 0);
        }
        if (!machines[i].restore(s)) {
            return false;
        }
        running[i] = true;
        any = true;
    }
    if (!any || !in.done()) {
        return false;
    }
    machines_ = std::move(machines);
    running_ = std::move(running);
    return true;
}

void BehaviorSet::send(std::string_view event, BehaviorOutput& out) {
    struct Item {
        std::string event;
        int from; ///< -1: from outside.
    };
    std::vector<Item> work{{std::string(event), -1}};
    for (int guard = 0; !work.empty() && guard < k_max_broadcast; ++guard) {
        const Item item = std::move(work.front());
        work.erase(work.begin());
        for (std::size_t i = 0; i < machines_.size(); ++i) {
            if (!running_[i] || static_cast<int>(i) == item.from) {
                continue;
            }
            BehaviorOutput part;
            machines_[i].send(item.event, part);
            for (const std::string& raised : part.events) {
                work.push_back({raised, static_cast<int>(i)});
            }
            merge(static_cast<int>(i), part, out);
        }
    }
    refresh(out);
}

void BehaviorSet::advance(float seconds, BehaviorOutput& out) {
    std::vector<std::pair<std::string, int>> raised;
    for (std::size_t i = 0; i < machines_.size(); ++i) {
        if (!running_[i]) {
            continue;
        }
        BehaviorOutput part;
        machines_[i].advance(seconds, part);
        for (const std::string& event : part.events) {
            raised.emplace_back(event, static_cast<int>(i));
        }
        merge(static_cast<int>(i), part, out);
    }
    // What one machine raises, the others hear.
    BehaviorOutput heard;
    for (const auto& [event, from] : raised) {
        for (std::size_t i = 0; i < machines_.size(); ++i) {
            if (running_[i] && static_cast<int>(i) != from) {
                BehaviorOutput part;
                machines_[i].send(event, part);
                merge(static_cast<int>(i), part, heard);
            }
        }
    }
    out.changes.insert(out.changes.end(), heard.changes.begin(), heard.changes.end());
    out.events.insert(out.events.end(), heard.events.begin(), heard.events.end());
    refresh(out);
}

} // namespace skydot
