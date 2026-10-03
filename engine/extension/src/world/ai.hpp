// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotAi`: runs placed actors' AI packages (packages.hpp) against a game
// clock.
//
// - Actors built in the space the viewer shows (an interior, or a
//   worldspace's streamed cells) are attached: each frame their current
//   step steers their `SkydotActor` (walk to a place, sandbox around it,
//   stand at a bed or marker, patrol linked markers). A place in another
//   space is reached through load doors: the actor walks to the door, goes
//   through, and is gone from this space.
// - Persistent actors that are not built are where their package says:
//   `place_actors` moves them there (SkydotWorld.set_actor_place), so cells
//   built afterwards build them there. One whose package brings it into the
//   space on screen appears at the door it comes through
//   (`actor_arrived`).
// - Packages lock and unlock doors: the UnlockDoors and LockDoors
//   procedures, and the "unlock doors at package start/end" flags, for the
//   space the actor (or the procedure's place) is in. Locks go through
//   SkydotPapyrus, which keeps them.
//
// Conditions see quests, globals and disabled references through
// SkydotPapyrus, and the actor's NPC_, factions and linked references
// through world.fb. Functions without an answer here count as 0 (see
// docs/ai.md for the list).
#pragma once

#include "world/packages.hpp"
#include "world/world.hpp"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace skydot {

class SkydotActor;
class SkydotPapyrus;

class SkydotAi : public godot::RefCounted {
    GDCLASS(SkydotAi, godot::RefCounted)

public:
    /// Game seconds per real second by default (the timescale GMST's).
    static constexpr double TIME_SCALE = 20.0;
    static constexpr std::int64_t PLACE_BUDGET_USEC = 1500;

    /// Use `world` and, if not null, `papyrus` for quests, globals and locks.
    godot::Error setup(const godot::Ref<SkydotWorld>& world, const godot::Ref<godot::RefCounted>& papyrus);

    /// The clock: days passed since the game started, the time of day as
    /// the fraction. `update` advances it by `time_scale`.
    void set_days(double days);
    double get_days() const { return clock_.days; }
    double get_hour() const { return clock_.hour(); }
    std::int64_t get_day_of_week() const { return clock_.day_of_week(); }
    void set_time_scale(double v) { time_scale_ = v; }
    double get_time_scale() const { return time_scale_; }

    /// Whether attached actors are steered. Off, they stand where they are
    /// built (packages still decide where that is).
    void set_drive(bool v) { drive_ = v; }
    bool get_drive() const { return drive_; }

    /// The space shown: an interior cell, or a worldspace. Actors whose
    /// package brings them into it arrive through a door.
    void set_space(std::int64_t space);
    std::int64_t get_space() const { return space_; }

    /// Put every persistent actor that is not built where its package wants
    /// it. Returns how many were moved. `update` calls it every few game
    /// minutes; call it before building a space.
    std::int64_t place_actors();
    /// Start a placement pass now, done a slice per `update` like the
    /// periodic one: call it when a space is about to be entered (the
    /// viewer: when it prepares what is behind a door).
    void begin_placing();
    /// Before building a space instead of `place_actors`: finish a pass under
    /// way, else place everyone unless a pass started less than two game
    /// minutes ago. Returns how many moved.
    std::int64_t settle_actors();

    /// Take over the `SkydotActor`s under `root` (as SkydotWorld builds
    /// them). Returns how many.
    std::int64_t attach_built(godot::Node* root);

    /// Advance the clock by `seconds` of real time and run the packages.
    /// Actors that are not built are moved a few at a time, within
    /// PLACE_BUDGET_USEC per call.
    void update(double seconds);

    /// What `ref` is doing: npc, package (form id), package_editor_id,
    /// procedure, step, steps, target (Dictionary: space, position, radius,
    /// ref) and attached. Empty if `ref` is no placed actor.
    godot::Dictionary get_actor_state(std::int64_t ref);
    /// `ref`'s packages in the order they are tried: id, editor_id,
    /// schedule (hour, duration, day_of_week), scheduled, conditions_pass,
    /// chosen.
    godot::Array get_packages(std::int64_t ref);
    /// Where `ref`'s package wants it now (space, position, radius), or empty.
    godot::Dictionary get_destination(std::int64_t ref);

protected:
    static void _bind_methods();

private:
    /// A place in the world: a space and a point in it (Skyrim space).
    struct Spot {
        std::uint32_t space{};
        godot::Vector3 position;
        float rotation_z{};
        double radius{}; ///< Game units.
        std::uint32_t ref{}; ///< The reference it was taken from, if any.
    };

    struct Mind {
        std::uint32_t ref{};
        std::uint32_t npc{};
        bool persistent{};
        std::uint64_t node{};            ///< SkydotActor's instance id; 0 when not built.
        std::uint32_t space{};           ///< Where the node is (when built).
        godot::Vector3 last;             ///< Its last position (Skyrim space).
        std::uint32_t package{};         ///< Chosen package; 0 none.
        std::vector<ai::Step> steps;
        std::size_t step{};
        double step_time{};              ///< Game-independent seconds in this step.
        bool step_started{};
        std::optional<Spot> target;      ///< Where the current step goes.
        std::uint32_t door{};            ///< The load door it walks to, if any.
        double retry{};                  ///< Seconds before the next path request.
        int failures{};                  ///< Path requests that failed in a row.
        int walks{};                     ///< Walks started towards the current spot.
        double think{};                  ///< Seconds before the package is chosen again.
        std::vector<godot::Vector3> patrol; ///< Patrol markers (Skyrim space).
        std::size_t patrol_next{};
        std::map<std::int32_t, std::uint32_t> lists; ///< ObjectList key -> found ref.
        /// Packages it completed, with the clock (days) then: not chosen again
        /// for the rest of their window.
        std::map<std::uint32_t, double> completed;
        std::uint64_t dice{};            ///< RNG state.
        bool announced{};                ///< actor_arrived sent for its place.
    };

    class Host;
    friend class Host;

    const bethconv::pack::wfb::World* root() const;
    Mind* mind(std::uint32_t ref);
    double roll(Mind& m);
    SkydotActor* node_of(const Mind& m) const;

    /// Choose `m`'s package and plan its steps if it changed. Returns whether
    /// it did.
    bool choose(Mind& m);
    void start_package(Mind& m, std::uint32_t previous);
    /// Where a step goes: its location, target or found reference.
    std::optional<Spot> step_spot(Mind& m, const ai::Step& s);
    /// Where the plan leaves `m` once its steps that move are done.
    std::optional<Spot> settled_spot(Mind& m);
    std::optional<Spot> location_spot(Mind& m, const bethconv::pack::wfb::PackageInput& in);
    std::optional<Spot> target_spot(Mind& m, const bethconv::pack::wfb::PackageInput& in);
    std::optional<Spot> ref_spot(std::uint32_t ref) const;
    Spot actor_spot(const Mind& m) const;
    Spot editor_spot(const Mind& m) const;
    std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const;
    /// The nearest reference within `radius` of `around` whose base matches a
    /// PTDA target selector (object id or object type).
    std::uint32_t find_ref(const Spot& around, std::int32_t type, std::uint32_t value) const;

    /// The load doors of `space`: door ref -> destination space.
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& doors_of(std::uint32_t space);
    /// The first door on the shortest door route from `from` to `to`, and the
    /// door of the last hop (the one arriving in `to`), or 0s if none.
    std::pair<std::uint32_t, std::uint32_t> route(std::uint32_t from, std::uint32_t to);
    /// Lock or unlock the load doors of `spot`'s space (an interior), or near
    /// it outside.
    void set_doors_locked(const Spot& spot, bool locked);

    /// place_actors; with `through_doors`, those coming into the shown space
    /// arrive at the door they come through.
    std::int64_t place(bool through_doors);
    void do_find(Mind& m, const ai::Step& s);
    bool has_lock(std::uint32_t door) const;
    void steer(Mind& m, SkydotActor& actor, double seconds);
    /// Walk towards `spot`; true once within `reach` game units of it.
    bool go(Mind& m, SkydotActor& actor, const Spot& spot, double reach, double seconds);
    void next_step(Mind& m);
    void leave(Mind& m, SkydotActor& actor, std::uint32_t door);
    void detach(Mind& m);
    /// An attached actor opened or closed a plain door (`door_toggled`).
    void on_door_toggled(std::int64_t ref, std::int64_t open_state);

    godot::Ref<SkydotWorld> world_;
    godot::Ref<godot::RefCounted> papyrus_;
    SkydotPapyrus* vm_{};
    ai::Clock clock_;
    double time_scale_{TIME_SCALE};
    bool drive_{true};
    std::uint32_t space_{};
    double since_placed_{}; ///< Game seconds since the last pass started.
    bool placed_{}; ///< Whether any pass has run.
    std::unordered_map<std::uint32_t, Mind> minds_;
    std::vector<std::uint32_t> persistent_; ///< Persistent actors with packages.
    std::vector<std::uint32_t> attached_;
    /// While place() runs: door -> locked, unlocking winning, applied at its
    /// end (residents of one house may run different packages).
    std::map<std::uint32_t, bool> pending_locks_;
    bool batching_{};
    /// A periodic placement in progress: the next of persistent_ to place,
    /// or npos when none runs.
    std::size_t place_cursor_{static_cast<std::size_t>(-1)};
    /// Place one actor; returns whether it moved.
    bool place_one(std::uint32_t ref, bool through_doors);
    /// Place persistent_ from `first` on, at once; returns how many moved.
    std::int64_t place_from(std::size_t first, bool through_doors);
    /// Per frame and pass: quest states (SkydotPapyrus builds a Dictionary
    /// per call). Static: whether a door has a lock, quests' script names.
    std::unordered_map<std::uint32_t, godot::Dictionary> quest_cache_;
    mutable std::unordered_map<std::uint32_t, bool> lock_cache_;
    std::unordered_map<std::uint32_t, std::vector<godot::String>> quest_scripts_;
    const godot::Dictionary& quest_state(std::uint32_t quest);
    /// Static per actor: its package list (NPC_ templates followed).
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> package_lists_;
    const std::vector<std::uint32_t>& packages_of(const Mind& m);
    std::unordered_map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> doors_;
};

} // namespace skydot
