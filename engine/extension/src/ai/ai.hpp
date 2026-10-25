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

#include "ai/combat_rules.hpp"
#include "ai/flight.hpp"
#include "ai/magic_choice.hpp"
#include "ai/packages.hpp"
#include "magic/active_effects.hpp"
#include "world/world.hpp"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace skydot {

class SkydotActor;
class SkydotPapyrus;

class SkydotAi : public godot::RefCounted {
    GDCLASS(SkydotAi, godot::RefCounted)

public:
    SkydotAi();
    ~SkydotAi() override;
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
    /// A scene puts `package` on top of `ref`'s packages: it is chosen
    /// whatever the schedule says, until `pop_package` takes it off.
    void push_package(std::int64_t ref, std::int64_t package);
    void pop_package(std::int64_t ref, std::int64_t package);
    /// Where `ref`'s package wants it now (space, position, radius), or empty.
    godot::Dictionary get_destination(std::int64_t ref);

    // ---- combat (ai_combat.cpp; rules in combat_rules.hpp) ----

    /// Whether minds start and fight combats at all (default on). A mind in combat sets its
    /// package aside whatever `drive` says, so shots with still actors still show a fight.
    void set_combat(bool v) { combat_enabled_ = v; }
    bool get_combat() const { return combat_enabled_; }
    /// The player, given each frame: Skyrim position (feet) and whether it lives. It is ref 0x14.
    void set_player(const godot::Vector3& position, bool alive);
    /// The player's eyes: the camera whose position and facing HasLOS reads (and whose world holds
    /// the bodies the ray meets). Null forgets it.
    void set_player_view(godot::Node3D* camera);
    /// Actor.HasLOS: whether `ref` can see `target` (a reference): the target within the viewing
    /// cone of `ref` (the player: the camera's) and no world or terrain body on the line from the
    /// eyes to it.
    bool has_los(std::uint32_t ref, std::uint32_t target) const;
    /// `attacker` hit `target`: the victim fights back, and allies within 1024 units that see
    /// the attacker and whose assistance says so join.
    void on_hit(std::int64_t attacker, std::int64_t target);
    /// Force `ref` into combat with `target`; false if `ref` is not a built, living actor.
    bool start_combat(std::int64_t ref, std::int64_t target);
    void stop_combat(std::int64_t ref);
    /// `ref` staggers (magic effect archetype 33, a bash): it plays a stagger clip of its
    /// behaviour project (small below magnitude 0.5, else large; PLACEHOLDER) and its AI pauses
    /// for the clip's length (1 s without one). Returns that pause, 0 if `ref` is not a built,
    /// living actor.
    double stagger(std::int64_t ref, double magnitude);
    /// `ref` is knocked down (the game layer ragdolls it): its AI pauses for `seconds` (0 lets it go on)
    /// and its walk stops, with no clip. False if `ref` is not a built, living actor.
    bool hold(std::int64_t ref, double seconds);
    /// `ref` lives again (reanimate): its node is alive for the SkydotActor again (set_dead(false)),
    /// its combat and package are forgotten and the AI takes it up again. Call it once the game
    /// layer has stood the body up as a SkydotActor (see SkydotWorld.build_placed_actor and
    /// attach_built); true if a node is attached.
    bool revive(std::int64_t ref);
    std::int64_t get_combat_target(std::int64_t ref);
    /// Run the fights under way only (not the packages, not new fights): for runs that keep the
    /// world still, as --screenshot does, but show a fight started by hand.
    void update_fights(double seconds);
    /// How `a` sees `b` (combat_ai::Reaction: 0 enemy, 1 neutral, 2 ally, 3 friend); -1 if a
    /// reference is no placed actor (or the player).
    std::int64_t get_reaction(std::int64_t a, std::int64_t b);
    /// 0 not in combat, 1 combat, 2 searching.
    std::int64_t get_combat_state(std::int64_t ref);

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

    /// What a mind knows of its fight.
    struct CombatMind {
        std::uint32_t target{};
        std::int32_t state{};            ///< 0 none, 1 combat, 2 searching
        godot::Vector3 last_seen;        ///< Skyrim space
        double since_seen{};             ///< Seconds the target has been out of sight
        double searching{};              ///< Seconds spent searching
        double cooldown{};               ///< Seconds until the next attack
        double think{};                  ///< Seconds before the next decision
        double detect{};                 ///< Seconds before the next look for an enemy
        double repath{};
        bool fleeing{};
        // Magic (ai_combat.cpp magic_step): transitions are logged once.
        bool paralyzed{};
        bool calmed{};
        bool frenzied{};
        bool following{};                ///< Follows its commander instead of its package.
        bool fear_entered{};             ///< The fight began as a flight from a fear caster.
        std::uint32_t afraid_of{};       ///< The fear caster it flees from; 0 none.
        double stagger{};                ///< Seconds its AI still pauses for a stagger clip.
        bool ranged{};                   ///< Fights with its bow
        bool was_wander{};
        int circle{1};                   ///< Which way it circles
        double reach{};                  ///< Game units, from its weapon
        bool directional_melee{};        ///< Its race attacks in all directions (the dragon): melee first.
        bool has_bow{};
        bool has_melee{};
        double gear_age{1e9};            ///< Seconds since reach and gear were read
        combat_ai::MeleeStyle style;
        // Magic (ai_combat.cpp mage_tick): the spells it knows and the cast under way.
        combat_ai::WeaponStyle weapon_style;
        std::vector<combat_ai::SpellFacts> spells; ///< Static facts; cost and active are read per decision.
        bool spells_read{};
        // Shouts (ai_combat.cpp shout_tick): the shouts it knows, how long it has faced its target
        // and the shout under way (inhaling, then said).
        std::vector<combat_ai::ShoutFacts> shouts;
        double aimed{};
        bool facing{};   ///< Within the tolerance of the target at the last decision.
        struct Shout {
            bool active{};
            std::uint32_t shout{};
            double elapsed{};
        } shout;
        // Flight (ai_flight.cpp): the dragon's choice of maneuvers, from its combat style.
        flight::Tactics flight;
        flight::Style flight_style;
        double injured{0.35};            ///< Race injured health: the fraction below which it is hurt.
        int flight_logged{-1};           ///< The maneuver last logged.
        double flight_log{};             ///< Seconds before the next periodic flight line.
        std::optional<flight::Vec3> idle_land; ///< Where it lands when it may not fly (out of combat).
        struct Cast {
            bool active{};
            std::uint32_t spell{};
            combat_ai::SpellRole role{};
            bool concentration{};
            double charge{};  ///< Seconds of fire and forget before the release.
            double hold{};    ///< Seconds of concentration.
            double cost{};    ///< Magicka per second, concentration only.
            double elapsed{};
        } cast;
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
        double idle{};                   ///< Seconds since the actor was last updated (far actors wait).
        std::vector<godot::Vector3> patrol; ///< Patrol markers (Skyrim space).
        std::size_t patrol_next{};
        std::map<std::int32_t, std::uint32_t> lists; ///< ObjectList key -> found ref.
        /// Packages it completed, with the clock (days) then: not chosen again
        /// for the rest of their window.
        std::map<std::uint32_t, double> completed;
        std::uint64_t dice{};            ///< RNG state.
        bool announced{};                ///< actor_arrived sent for its place.
        CombatMind combat;
        bool woken{};                    ///< Sent OnGetUp after the player came close while it slept.
    };

    class Conditions;
    friend class Conditions;
    /// Conditions, with the places and clock this AI keeps; made in `setup`.
    std::unique_ptr<Conditions> conditions_;

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

    bool combat_tick(Mind& m, SkydotActor& actor, double seconds);
    void enter_combat(Mind& m, SkydotActor& actor, std::uint32_t target);
    void leave_combat(Mind& m, SkydotActor* actor);
    void read_gear(Mind& m);
    /// Skyrim position of a living, built actor or of the player; nullopt otherwise.
    std::optional<godot::Vector3> position_of(std::uint32_t ref) const;
    bool alive(std::uint32_t ref) const;
    /// `cone` off: all round (an actor with an aggro radius feels what comes close). `range` > 0
    /// replaces the sight distance (game units).
    bool sees(const Mind& m, const SkydotActor& actor, std::uint32_t target, bool cone = true,
              double range = 0.0) const;
    /// The attack distance of `m`'s aggro radius (game units), 0 if it has none.
    double aggro_radius(const Mind& m) const;
    combat_ai::Reaction reaction_of(std::uint32_t a, std::uint32_t b);
    /// Factions and relationships only: reaction_of without the commanders.
    combat_ai::Reaction faction_reaction(std::uint32_t a, std::uint32_t b);
    /// The held magic on `ref` (the host's, kept for the frame: the caches are cleared by
    /// update, update_fights and on_hit).
    const magic::Overrides& overrides_of(std::uint32_t ref) const;
    combat_ai::MagicState magic_state(std::uint32_t ref) const;
    std::uint32_t commander_of(std::uint32_t ref) const;
    /// Paralysis, fear, calm and following a commander. True when it took over this tick.
    bool magic_step(Mind& m, SkydotActor& actor, const combat_ai::MagicState& state, double seconds);
    bool follow_commander(Mind& m, SkydotActor& actor, std::uint32_t commander);
    /// The plan of a placed or a runtime actor.
    godot::Dictionary plan_of(std::uint32_t ref) const;
    std::vector<combat_ai::Membership> memberships(std::uint32_t ref) const;
    std::uint32_t npc_of(std::uint32_t ref) const;
    std::uint8_t value_of(std::uint32_t ref, const char* actor_value) const;
    void combat_walk(Mind& m, SkydotActor& actor, const godot::Vector3& skyrim_point, bool run);
    void melee_tick(Mind& m, SkydotActor& actor, double distance, const godot::Vector3& to);
    void ranged_tick(Mind& m, SkydotActor& actor, double distance, const godot::Vector3& to, bool los);
    void flee_tick(Mind& m, SkydotActor& actor, const godot::Vector3& from);
    /// A mage's turn (magic_choice.hpp): true if it cast, walked for a spell or waits for one; false
    /// if it fights with its weapons.
    /// A shout under way: true while it inhales (the actor stands), then it is said (`combat_shout`).
    bool shout_tick(Mind& m, double seconds);
    /// Whether to start a shout now (the aim rule, the die and the style); true if the actor stands
    /// aiming or inhaling.
    /// `airborne` (a breath in flight): the ground rules of aim time, standing still and reach do not apply.
    bool shout_decide(Mind& m, SkydotActor& actor, double distance, const godot::Vector3& to, bool los,
                      bool airborne = false);
    /// A flying-capable actor in combat: the flight tactics decide and order (flight::decide); true
    /// while the mover has the body (no ground combat then), false on the ground.
    bool flight_combat(Mind& m, SkydotActor& actor, double seconds);
    /// A flying actor out of combat: its patrol markers, else its orbit marker or editor spot; lands
    /// when it may no longer fly.
    void flight_idle(Mind& m, SkydotActor& actor, double seconds);
    void flight_log_tick(Mind& m, SkydotActor& actor, double distance, double seconds);
    /// Patrol markers of the current step (the start and the references linked on from it).
    void load_patrol(Mind& m);
    std::uint32_t orbit_keyword_{0};
    bool orbit_keyword_known_{false};
    bool mage_tick(Mind& m, SkydotActor& actor, double distance, const godot::Vector3& to, bool los);
    /// The cast under way, once per frame: release or stop it when its time is up. True while one runs.
    bool cast_tick(Mind& m, double seconds);
    /// Stop the cast under way ("stop": nothing is cast for a fire-and-forget spell).
    void end_cast(Mind& m);
    void read_spells(Mind& m);
    std::unordered_map<std::uint32_t, combat_ai::SpellFacts> spell_facts_; ///< Static facts by spell.
    mutable std::unordered_map<std::uint32_t, magic::Overrides> overrides_cache_;
    bool combat_enabled_{true};
    godot::Vector3 player_position_;
    bool player_alive_{false};
    bool player_known_{false};
    std::uint64_t player_view_{0};  ///< ObjectID of the camera (set_player_view), 0 if none
    std::unordered_map<std::uint64_t, std::uint16_t> relationships_;
    bool relationships_built_{false};

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
    /// Static: whether a door has a lock.
    mutable std::unordered_map<std::uint32_t, bool> lock_cache_;
    /// Static per actor: its package list (NPC_ templates followed).
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> package_lists_;
    /// Packages scenes pushed, per actor, the last one on top.
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> forced_;
    const std::vector<std::uint32_t>& packages_of(const Mind& m);
    std::unordered_map<std::uint32_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> doors_;
};

} // namespace skydot
