// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotPapyrus`: the VM (vm.hpp) bound to a pack and its world. Scripts load
// from the pack's script assets; `attach` gives a reference the scripts its
// base and VMAD name, with their property values, and sends OnInit.
//
// Natives that change the world do not touch nodes: they update this object's
// view of the world (enabled, open, locked, activation blocked) and emit a
// signal the game layer acts on. See docs/papyrus.md for the list.
//
// Trigger volumes: an attached reference with an XPRM box or sphere is a
// trigger. `update_actor` moves an actor (the player) and sends OnTriggerEnter
// and OnTriggerLeave as it crosses them.
//
// Quests (vm/quests.cpp): a quest's scripts attach when it starts or is first
// named by a property; starting fills its aliases, sends OnInit and sets its
// start-up stage; SetStage runs the stage's fragment. An alias is an object
// of its own (a handle from ALIAS_HANDLE_BASE up) carrying the alias's
// scripts; events sent to a reference also go to the aliases it fills.
//
// `save_state`/`load_state` carry the VM (instances, threads, timers) and this
// object's world view; see vm/save.cpp.
#pragma once

#include "assets/pack.hpp"
#include "data/godot_values.hpp"
#include "save/save_binding.hpp"
#include "session/clock.hpp"
#include "vm/actor_states.hpp"
#include "vm/condition_world.hpp"
#include "vm/id_ranges.hpp"
#include "vm/quest_states.hpp"
#include "vm/ref_states.hpp"
#include "vm/triggers.hpp"
#include "vm/vm.hpp"
#include "vm/activation.hpp"
#include "vm/world_access.hpp"
#include "world/world.hpp"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace skydot {

class SkydotPapyrus : public godot::RefCounted {
    GDCLASS(SkydotPapyrus, godot::RefCounted)

public:
    /// The player's reference, which activations come from.
    static constexpr std::int64_t PLAYER_REF = static_cast<std::int64_t>(k_player_ref);
    /// Seconds PlayAnimationAndWait waits for its event before carrying on.
    static constexpr double ANIMATION_TIMEOUT = 10.0;
    /// Sound.PlayAndWait gives up on a clip that never reports its end (a loop).
    static constexpr double SOUND_TIMEOUT = 30.0;
    /// Alias handles are form ids from here up, above those create_instance
    /// hands out.
    static constexpr std::int64_t ALIAS_HANDLE_BASE = vm::id_ranges::alias_handles.first;

    SkydotPapyrus();

    /// The game clock (SkydotClock): the one source of game time. Utility.
    /// GetCurrentGameTime reads it, and so do the GameHour, GameDaysPassed and
    /// TimeScale globals; SetValue on them moves it.
    void attach_clock(const godot::Ref<SkydotClock>& clock) { clock_ = clock; }

    /// Load scripts from `pack` and look references up in `world`.
    godot::Error setup(const godot::Ref<SkydotPack>& pack, const godot::Ref<SkydotWorld>& world);

    /// Attach the scripts of reference `ref` in `cell` (base first, then the
    /// reference's own), set their properties and send OnInit. Returns how
    /// many were attached; 0 if the reference has none or already has them.
    std::int64_t attach(std::int64_t cell, std::int64_t ref);
    /// Attach every tagged model under `root` (as SkydotWorld builds them)
    /// and send OnLoad to each reference with scripts. Returns how many
    /// references have scripts.
    std::int64_t attach_built(godot::Node* root);
    /// Attach every scripted reference of `cell`, built or not (triggers have
    /// no model). Returns how many references got scripts.
    std::int64_t attach_cell(std::int64_t cell);
    /// A cell went away: its scripted references get OnCellDetach and
    /// OnUnload and lose their update timers. Returns how many.
    std::int64_t detach_cell(std::int64_t cell);
    /// detach_cell for every loaded cell.
    std::int64_t detach_all();
    /// The natives called from the scripts named in `classes` (see
    /// `Vm::census`): an Array of Dictionaries with `class`, `function`,
    /// `sites`, `bound` and `ignored`.
    godot::Array census(const godot::PackedStringArray& classes);
    /// Send OnActivate(activator) to `ref`'s scripts. Returns how many ran.
    std::int64_t activate(std::int64_t ref, std::int64_t activator = PLAYER_REF);
    /// Put `count` of `item` into `owner`'s inventory (the viewer's --give).
    void add_item(std::int64_t owner, std::int64_t item, std::int64_t count);
    /// How a reference is activated (activate_ref's `source`).
    static constexpr std::int64_t ACTIVATE_PLAYER = 0;
    static constexpr std::int64_t ACTIVATE_SCRIPT = 1;
    static constexpr std::int64_t ACTIVATE_PARENT = 2;

    /// The whole of an activation, minus what only the host can do: the rules
    /// of vm/activation.hpp (parent-only, locks), then OnActivate with
    /// `activator` as akActionRef (not with `default_only`), then the
    /// activate children on the VM's clock (not if scripts block activation),
    /// as the same activator. Returns the outcome: `kind` is one of missing,
    /// parent_only, locked, blocked (the host shows `note`), door (with `door`
    /// data), furniture (with `event`), toggle_open or none (the host applies
    /// it; `note` is for when it cannot); `label` names the reference and
    /// `scripts` counts the OnActivate handlers that ran. A script's Activate
    /// and the children arrive the same way through `activation_outcome`.
    godot::Dictionary activate_ref(std::int64_t ref, std::int64_t activator, bool default_only,
                                   std::int64_t source, bool force);
    /// Finish an activation that activate_ref left pending (`deferred` in its
    /// outcome, kind furniture): the player used a piece of furniture whose
    /// behaviour graph raises ObjectActivated, and its scripts and activate
    /// children wait for that event (notify_animation_event). The host calls
    /// this when the graph will not get there, e.g. it refused the event.
    /// The result arrives through `activation_outcome`. Nothing pending: no-op.
    void complete_activation(std::int64_t ref);
    /// Whether the model of `ref` is built and it is not disabled.
    bool is_3d_loaded(std::uint32_t ref) const;
    bool is_3d_loaded_i(std::int64_t ref) const { return is_3d_loaded(static_cast<std::uint32_t>(ref)); }
    std::int64_t get_scheduled_count() const { return static_cast<std::int64_t>(vm_->scheduled_count()); }
    /// Send any event with arguments (ints are passed as ObjectReference).
    std::int64_t send_event(std::int64_t ref, const godot::String& event, const godot::Array& args);
    /// An animation event (a text key or a finished clip) on `ref`, for
    /// PlayAnimationAndWait.
    void notify_animation_event(std::int64_t ref, const godot::String& event);
    /// A sound instance (play_sound's third argument) has ended, for
    /// Sound.PlayAndWait.
    void notify_sound_finished(std::int64_t instance);
    /// Move `actor` to `position` (Skyrim space, game units): OnTriggerEnter
    /// and OnTriggerLeave for the trigger volumes it crosses.
    void update_actor(std::int64_t actor, const godot::Vector3& position);

    /// Advance the VM's clock and run what is ready.
    void update(double seconds);

    /// Start a global function; returns a thread id for take_result, 0 if
    /// there is no such function.
    std::int64_t call_global(const godot::String& cls, const godot::String& fn,
                             const godot::Array& args);
    /// An unattached instance of `cls` on a new form id, for tests. Returns
    /// the form, 0 if the class does not load.
    /// 0 when the class cannot be attached or the 4,096 test ids are used up.
    std::int64_t create_instance(const godot::String& cls);
    /// A playback instance id for a sound the game layer starts itself, from
    /// the same range as Sound.Play's; 0 (and an error) once the range is used up.
    std::int64_t new_sound_instance();
    std::int64_t call_method(std::int64_t form, const godot::String& cls, const godot::String& fn,
                             const godot::Array& args);
    /// {done: bool, value}; value is only present once done.
    godot::Dictionary take_result(std::int64_t thread);
    godot::Variant get_variable(std::int64_t form, const godot::String& cls,
                                const godot::String& name) const;
    godot::Array get_scripts(std::int64_t ref) const;

    bool is_ref_disabled(std::int64_t ref) const;
    /// References whose enabled state scripts changed: ref -> disabled.
    godot::Dictionary get_disabled_changes() const;
    /// Where scripts moved references to: ref -> [position, rotation], as the
    /// `ref_moved` signal sends them (Skyrim space, radians).
    godot::Dictionary get_moved_changes() const;
    /// Take a PlaceAtMe reference out of the world (`ref_deleted`); the game
    /// layer calls it when an effect has run its course. No-op for any other.
    void remove_placed(std::int64_t ref);
    /// The one path that forgets a reference: its scripts, timers, threads,
    /// inventory, animation, destruction, script-made state and, for a placed
    /// reference, the reference itself (`ref_deleted`). Delete and finished
    /// effects use it.
    void forget_ref(std::int64_t ref);
    /// DamageObject from the host (the viewer's --damage).
    void damage_ref(std::int64_t ref, double amount) { damage_object(static_cast<std::uint32_t>(ref), amount); }
    /// The game layer built the node of a placed reference: OnCellAttach,
    /// OnCellLoad and OnLoad. Returns how many handlers ran.
    std::int64_t ref_built(std::int64_t ref);
    /// The base form of a reference, placed or runtime; 0 if unknown.
    std::int64_t get_ref_base(std::int64_t ref) const {
        return static_cast<std::int64_t>(ref_info(static_cast<std::uint32_t>(ref)).get("base", 0));
    }
    bool is_activation_blocked(std::int64_t ref) const;
    /// The lock level if `ref` is locked, else -1.
    std::int64_t get_lock_level(std::int64_t ref) const;
    void set_locked(std::int64_t ref, bool locked);
    /// GetOpenState's values: 0 none, 1 open, 2 opening, 3 closed, 4 closing.
    std::int64_t get_open_state(std::int64_t ref) const;

    // ---- actor values (vm/actor_states.hpp): 0 for a reference that is no actor
    // or a name that is no actor value (reported once)
    bool is_actor_dead(std::int64_t ref) const { return actors_.is_dead(static_cast<std::uint32_t>(ref)); }
    /// How many of `item` `owner` carries (the player's key check).
    std::int64_t get_item_count(std::int64_t owner, std::int64_t item) const;
    double get_actor_value(std::int64_t ref, const godot::String& name) const;
    double get_actor_value_max(std::int64_t ref, const godot::String& name) const;
    /// Lower `name` of `ref` by `amount`; an actor at Health 0 dies (signal
    /// `actor_died`) unless it is essential or the player.
    void damage_actor_value(std::int64_t ref, const godot::String& name, double amount);
    void restore_actor_value(std::int64_t ref, const godot::String& name, double amount);
    void set_open_state(std::int64_t ref, std::int64_t state);
    /// Actors inside trigger `ref`.
    std::int64_t get_trigger_count(std::int64_t ref) const;
    /// Registered trigger volumes: ref, type, origin (Skyrim space), half
    /// extents, inside (actor count).
    godot::Array get_triggers() const;

    // ---- conditions
    /// The game as condition functions ask about it (vm/condition_world.hpp);
    /// its counters of unbound functions log one warning per function.
    conditions::ConditionWorld& condition_world();

    // ---- quests
    /// Start every start-game-enabled quest, as a new game does. Returns how
    /// many started.
    std::int64_t start_game_enabled_quests();
    /// Start a quest: fill its aliases, send OnInit, set its start-up stage.
    /// False if it is not a quest or runs only once and has run.
    bool start_quest(std::int64_t quest);
    /// Stop a quest and clear its aliases.
    void stop_quest(std::int64_t quest);
    /// SetStage: starts the quest if needed, runs the stage's fragment.
    /// False if the stage does not exist or is done and may not repeat.
    bool set_stage(std::int64_t quest, std::int64_t stage);
    /// running, completed, failed, stage, stages_done (Array), objectives
    /// (index -> displayed, completed, failed), aliases (id -> filling ref or
    /// location, 0 if empty). Empty if the quest never started.
    godot::Dictionary get_quest_state(std::int64_t quest) const;
    /// Running quests, sorted.
    godot::PackedInt64Array get_running_quests() const;
    /// The reference (or location) filling alias `alias` of `quest`, or 0.
    std::int64_t get_alias_ref(std::int64_t quest, std::int64_t alias) const;
    double get_global_value(std::int64_t global) const;
    void set_global_value(std::int64_t global, double value);

    /// A reference's behaviour graph snapshot (opaque bytes), kept and saved
    /// with the VM; empty `bytes` forgets it.
    void set_animation_state(std::int64_t ref, const godot::PackedByteArray& bytes);
    /// The snapshot set for `ref`, empty if none.
    godot::PackedByteArray get_animation_state(std::int64_t ref) const;

    // ---- saves (vm/papyrus_save.cpp; the container is save/save_file.hpp)
    /// Add the sections scripts need to carry on: HEAD (engine version, the
    /// pack's world hash and load order), the VM's (VMST, INVT, ANIM, FLST,
    /// PLCD, DEST), RFST (what scripts changed per reference), PAPY
    /// (attached references, triggers, actors) and QUST.
    void write_sections(const godot::Ref<SkydotSaveWriter>& writer) const;
    /// Phase one of a load: check HEAD against the open pack and decode every
    /// section into new objects. The running state is not touched, whatever
    /// the outcome; `get_last_error` says what was wrong.
    godot::Error prepare_load(const godot::Ref<SkydotSaveReader>& reader);
    /// Phase two: swap in what `prepare_load` decoded (OK), tell the game
    /// layer which placed references are gone and which are back.
    godot::Error commit_load();
    /// Forget what `prepare_load` decoded.
    void cancel_load();
    /// The sections above as a file of their own, and prepare plus commit of
    /// one: for tests and tools; the game saves through SaveService.
    godot::PackedByteArray save_state() const;
    godot::Error load_state(const godot::PackedByteArray& bytes);
    godot::String get_last_error() const;

    std::int64_t get_error_count() const;
    std::int64_t get_instruction_count() const;
    std::int64_t get_thread_count() const;
    double get_time() const;

protected:
    static void _bind_methods();

private:
    friend class PapyrusConditionWorld;
    std::unique_ptr<PapyrusConditionWorld> conditions_;
    /// The index of the first log entry of stage `index` of `quest` whose
    /// conditions pass (subject: the player, quest: this one); 0 if none does.
    std::size_t chosen_log_entry(std::uint32_t quest, std::int32_t index);
    void bind_natives();
    void bind_debug_natives();
    void bind_utility_natives();
    void bind_game_natives();
    void bind_form_natives();
    void bind_objectreference_natives();
    void bind_effectshader_natives();
    void bind_weapon_natives();
    void bind_sound_natives();
    void bind_quest_natives();
    void bind_noop_natives();
    void bind_world_natives();
    void bind_runtime_natives();
    void bind_actor_natives();
    /// Emit `actor_died` for the actors that died since the last call.
    void flush_deaths();
    /// get_quest's dictionary, cached; empty if not a quest.
    const godot::Dictionary& quest_info(std::uint32_t quest) const;
    /// The object standing for alias `alias` of `quest`.
    std::uint32_t alias_handle(std::uint32_t quest, std::uint32_t alias);
    /// Attach a quest's scripts and its aliases' once; with `on_init`, send
    /// them OnInit. No-op for anything but a quest.
    void attach_quest_scripts(std::uint32_t quest, bool on_init);
    void fill_aliases(std::uint32_t quest);
    void set_objective(std::uint32_t quest, std::int32_t index, std::uint8_t bit, bool on);
    /// Send an event to `ref`'s scripts and to the aliases it fills.
    std::size_t dispatch(std::uint32_t ref, std::string_view event, const std::vector<vm::Value>& args);
    vm::Value to_value(const godot::Variant& v, std::string_view type, std::uint32_t owner_quest = 0);
    /// Attach a script list (as get_ref_info gives it) to `form` and set the
    /// properties; alias properties with form 0 belong to `owner_quest`.
    /// Returns how many attached.
    std::int64_t attach_scripts(std::uint32_t form, const godot::Array& scripts,
                                std::uint32_t owner_quest);
    godot::Variant to_variant(const vm::Value& v) const;
    godot::Dictionary ref_info(std::uint32_t ref) const;
    /// ref_info for a PlaceAtMe reference: its base's scripts, where it was put.
    godot::Dictionary placed_info(const vm::PlacedRef& placed) const;
    /// The cell a reference is in, placed or runtime; 0 if unknown.
    std::uint32_t cell_of(std::uint32_t ref) const;
    /// PlaceAtMe: a new runtime reference of `base` where `source` is.
    std::uint32_t place_at(std::uint32_t source, std::uint32_t base, bool initially_disabled);
    /// The base's DEST, null if `ref` is not destructible.
    const bethconv::pack::wfb::Destruction* destruction_of(std::uint32_t ref) const;
    /// The base form of `ref`, placed by a script or by the world; 0 if unknown.
    std::uint32_t base_form_of(std::uint32_t ref) const;
    /// DamageObject: open the stages the damage reaches and send
    /// OnDestructionStageChanged.
    void damage_object(std::uint32_t ref, double amount);
    void clear_destruction(std::uint32_t ref);
    bool disabled(std::uint32_t ref, int depth = 0) const;
    void set_disabled(std::uint32_t ref, bool value);
    bool locked(std::uint32_t ref) const;
    /// The reference whose lock `ref` uses: itself, or a load door's partner.
    std::uint32_t lock_holder(std::uint32_t ref) const;
    void register_trigger(std::uint32_t ref, const godot::Dictionary& info);
    void register_phantoms(std::uint32_t ref, godot::Node3D* node);
    godot::Vector3 position_of(std::uint32_t ref) const;
    /// Radians; where a script moved the reference, else as placed.
    godot::Vector3 rotation_of(std::uint32_t ref) const;
    void reset();
    void install_scheduled_handler();

    /// Typed questions about the world for natives (vm/world_access.hpp).
    struct Access;
    std::unique_ptr<vm::WorldAccess> access_;
    std::unique_ptr<vm::Vm> vm_;
    /// Playback instances (id_ranges::sound_instances): Sound.Play's and the
    /// game layer's, from `new_sound_instance`.
    vm::IdAllocator sound_ids_{vm::id_ranges::sound_instances};
    godot::Ref<SkydotPack> pack_;
    godot::Ref<SkydotClock> clock_;
    /// Which of the clock's values a global form is (found by editor id), if any.
    enum class ClockGlobal { none, hour, days_passed, time_scale };
    ClockGlobal clock_global(std::uint32_t global) const;
    godot::Ref<SkydotWorld> world_;
    std::set<std::uint32_t> attached_;
    /// Cells whose references are handed to the VM and loaded (until
    /// detach_cell / detach_all). `attached_` is separate: it only says that
    /// a reference's scripts exist and got OnInit.
    std::set<std::uint32_t> attached_cells_;
    /// The scripted references of loaded cells: reference -> cell.
    std::map<std::uint32_t, std::uint32_t> loaded_;
    /// What scripts changed about references: enabled, open, locked, blocked, moved.
    vm::RefStates states_;
    /// What scripts and the game did to actors' values (ACTV).
    vm::ActorStates actors_;
    /// The rules and sequence of activations (vm/activation.hpp); the adapter
    /// answers what they ask of the world (vm/papyrus_activation.cpp).
    struct ActivationAdapter;
    std::unique_ptr<ActivationHost> activation_host_;
    std::unique_ptr<Activations> activations_;
    void bind_activations();
    godot::Dictionary outcome_dictionary(const ActivationOutcome& outcome) const;
    bool graph_raises_activation(std::uint32_t form) const;
    /// Trigger volumes and the actors moving through them.
    vm::Triggers triggers_;
    vm::IdAllocator test_ids_{vm::id_ranges::test_instances};
    /// Quests, their aliases and the globals scripts set (QUST).
    vm::QuestStates quests_;
    mutable std::map<std::uint32_t, godot::Dictionary> quest_info_;
    godot::String last_error_;

    /// The parts of a save this class decodes besides the VM's and RefStates.
    struct HostSnapshot {
        std::set<std::uint32_t> attached;
        vm::Triggers::Saved triggers;
        vm::IdAllocator test_ids{vm::id_ranges::test_instances};
        vm::IdAllocator sound_ids{vm::id_ranges::sound_instances};
        vm::QuestStates quests;
    };
    struct PendingLoad {
        vm::Vm::Snapshot vm;
        vm::RefStates states;
        vm::ActorStates actors;
        HostSnapshot host;
    };
    std::unique_ptr<PendingLoad> pending_;
    godot::Error prepare(const save::SaveReader& file);
    void write_host(save::SaveWriter& out) const;
    bool decode_host(const save::SaveReader& file, HostSnapshot& out, std::string& error) const;
};

} // namespace skydot
