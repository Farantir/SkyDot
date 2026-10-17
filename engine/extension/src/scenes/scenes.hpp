// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scenes (SCEN): a quest's scripted exchanges between actors. Rules, from the
// plan (GAMEPLAY-PLAN.md, Q6 logic); nothing here touches Godot.
//
// - A scene starts by `start`/`force_start`, or when its quest starts and its
//   flag 0x1 is set (it waits while it cannot). `start` needs the quest
//   running, the scene's conditions passing and every actor alias filled with a
//   loaded, living actor that is in no other scene; `force_start` skips the
//   conditions and stops the other scenes those actors are in.
// - Phases run in order. A phase whose start conditions fail is skipped.
//   Entering a phase starts the actions whose start phase it is. The phase is
//   done when every action ending in it is done and its completion conditions
//   pass (checked at each update); then the next begins, and after the last
//   the scene ends.
// - A dialogue action has its actor say the topic (selection as dialogue, the
//   actor the subject); the game layer speaks the lines and reports
//   `action_finished`. A timer is done after its seconds of `update` time. A
//   package action pushes the package on the actor and is done at once; the
//   package is popped when the action's end phase completes (or the scene
//   ends or stops).
// - The SF_ fragments run at the scene's begin and end and at each phase's
//   begin and end. Stopping the quest of a scene with flag 0x2 stops it.
//
// Saved as the SCEN section (version 1): running scenes with their phase,
// action states and pushed packages. A dialogue action that was speaking asks
// for its lines again after a load.
#pragma once

#include "dialogue/dialogue.hpp"
#include "save/bytes.hpp"
#include "world_generated.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace skydot::scenes {

namespace wfb = bethconv::pack::wfb;

/// What scenes ask of their owner.
class SceneHost {
public:
    virtual ~SceneHost() = default;
    [[nodiscard]] virtual bool quest_running(std::uint32_t quest) const = 0;
    /// The reference filling alias `alias` of `quest`; 0 if empty.
    [[nodiscard]] virtual std::uint32_t alias_ref(std::uint32_t quest, std::int32_t alias) const = 0;
    /// Whether `actor` is loaded and alive.
    [[nodiscard]] virtual bool actor_ready(std::uint32_t actor) const = 0;
    /// The Q5 evaluator with `quest` as context; `subject` 0 is the player.
    [[nodiscard]] virtual bool conditions_pass(
        const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, std::uint32_t quest,
        std::uint32_t subject) = 0;
    /// The lines `actor` says for `topic`, as dialogue selects them.
    [[nodiscard]] virtual std::vector<dialogue::Line> say(std::uint32_t actor, std::uint32_t topic) = 0;
    /// Run function `function` of the SF_ script of `scene`.
    virtual void run_fragment(std::uint32_t scene, const std::string& script, const std::string& function) = 0;
    virtual void push_package(std::uint32_t actor, std::uint32_t package) = 0;
    virtual void pop_package(std::uint32_t actor, std::uint32_t package) = 0;
    virtual void log(const std::string& text) = 0;
};

/// A dialogue action asking the game layer to speak.
struct DialogueRequest {
    std::uint32_t scene = 0;
    std::uint32_t action = 0; ///< The action's INAM index.
    std::uint32_t speaker = 0;
    std::vector<dialogue::Line> lines;
};

class Scenes {
public:
    static constexpr std::uint32_t k_section_version = 1;

    Scenes(const wfb::World& world, SceneHost& host) : world_(world), host_(host) {}

    bool start(std::uint32_t scene) { return begin(scene, false); }
    bool force_start(std::uint32_t scene) { return begin(scene, true); }
    void stop(std::uint32_t scene);
    [[nodiscard]] bool is_playing(std::uint32_t scene) const { return runs_.contains(scene); }
    /// The quest of `scene`; 0 if it is none.
    [[nodiscard]] std::uint32_t quest_of(std::uint32_t scene) const;
    /// Whether action `index` (INAM) of a playing scene is done; also true for
    /// an action that does not exist or a scene that is not playing.
    [[nodiscard]] bool action_complete(std::uint32_t scene, std::uint32_t index) const;

    /// The quest started / stopped: scenes with flag 0x1 wait to begin, those
    /// with 0x2 stop.
    void quest_started(std::uint32_t quest);
    void quest_stopped(std::uint32_t quest);

    /// Advance by `seconds` of game time; returns the dialogue the game layer
    /// is to speak.
    std::vector<DialogueRequest> update(double seconds);
    /// The game layer finished the lines of a dialogue action.
    void action_finished(std::uint32_t scene, std::uint32_t index);

    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] static bool load(save::ByteReader r, Scenes& out);
    /// Take the running scenes of `other` (a loaded section).
    void adopt(Scenes&& other);
    /// Every package pushed by the running scenes: actor, package.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> pushed_packages() const;
    void clear();

private:
    enum class Status : std::uint8_t { waiting, active, done };
    struct ActionRun {
        Status status = Status::waiting;
        double left = 0.0;       ///< Timer seconds left.
        bool requested = false;  ///< Dialogue lines handed out (not saved).
        std::uint32_t actor = 0; ///< Who acts (dialogue, package).
    };
    struct Run {
        std::int32_t phase = -1; ///< -1: the phase is being entered.
        std::vector<ActionRun> actions;
        struct Pushed {
            std::uint32_t actor;
            std::uint32_t package;
            std::uint32_t end_phase; ///< Popped when this phase completes.
        };
        std::vector<Pushed> pushed;
    };

    [[nodiscard]] const wfb::Scene* find(std::uint32_t scene) const;
    bool begin(std::uint32_t scene, bool force);
    bool can_start(const wfb::Scene& scene, bool force, std::vector<std::uint32_t>& actors);
    [[nodiscard]] std::string name(const wfb::Scene& scene) const;
    [[nodiscard]] std::vector<std::uint32_t> actors_of(const wfb::Scene& scene) const;
    /// Enter phase `phase` or the next one that is not skipped; false if the scene ended.
    bool enter(const wfb::Scene& scene, Run& run, std::int32_t phase);
    void start_action(const wfb::Scene& scene, Run& run, std::size_t i);
    void finish_phase(const wfb::Scene& scene, Run& run);
    void end(std::uint32_t scene, const char* how);
    void fragment(const wfb::Scene& scene, const std::string& function);
    void phase_fragment(const wfb::Scene& scene, std::int32_t phase, bool on_end);
    [[nodiscard]] static bool has_flag(const wfb::Scene& scene, wfb::SceneFlags flag);

    const wfb::World& world_;
    SceneHost& host_;
    std::map<std::uint32_t, Run> runs_;
    std::set<std::uint32_t> waiting_; ///< Scenes that begin as soon as they can.
    std::string why_;                 ///< Why the last can_start said no.
    std::map<std::uint32_t, std::string> waited_; ///< The reason last logged per waiting scene.
    std::vector<DialogueRequest> outbox_;
};

} // namespace skydot::scenes
