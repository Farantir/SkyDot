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
#include "vm/vm.hpp"
#include "world/world.hpp"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace skydot {

class SkydotPapyrus : public godot::RefCounted {
    GDCLASS(SkydotPapyrus, godot::RefCounted)

public:
    /// The player's reference, which activations come from.
    static constexpr std::int64_t PLAYER_REF = 0x14;
    /// Seconds PlayAnimationAndWait waits for its event before carrying on.
    static constexpr double ANIMATION_TIMEOUT = 10.0;
    /// Alias handles are form ids from here up, above those create_instance
    /// hands out.
    static constexpr std::int64_t ALIAS_HANDLE_BASE = 0xFF800000;

    SkydotPapyrus();

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
    /// Send OnActivate(activator) to `ref`'s scripts. Returns how many ran.
    std::int64_t activate(std::int64_t ref, std::int64_t activator = PLAYER_REF);
    /// Send any event with arguments (ints are passed as ObjectReference).
    std::int64_t send_event(std::int64_t ref, const godot::String& event, const godot::Array& args);
    /// An animation event (a text key or a finished clip) on `ref`, for
    /// PlayAnimationAndWait.
    void notify_animation_event(std::int64_t ref, const godot::String& event);
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
    std::int64_t create_instance(const godot::String& cls);
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
    bool is_activation_blocked(std::int64_t ref) const;
    /// The lock level if `ref` is locked, else -1.
    std::int64_t get_lock_level(std::int64_t ref) const;
    void set_locked(std::int64_t ref, bool locked);
    /// GetOpenState's values: 0 none, 1 open, 2 opening, 3 closed, 4 closing.
    std::int64_t get_open_state(std::int64_t ref) const;
    void set_open_state(std::int64_t ref, std::int64_t state);
    /// Actors inside trigger `ref`.
    std::int64_t get_trigger_count(std::int64_t ref) const;
    /// Registered trigger volumes: ref, type, origin (Skyrim space), half
    /// extents, inside (actor count).
    godot::Array get_triggers() const;

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

    /// Everything scripts and their view of the world need to carry on.
    godot::PackedByteArray save_state() const;
    /// Replace the current state with a save. Scripts must be the same.
    godot::Error load_state(const godot::PackedByteArray& bytes);
    godot::String get_last_error() const;

    std::int64_t get_error_count() const;
    std::int64_t get_instruction_count() const;
    std::int64_t get_thread_count() const;
    double get_time() const;

protected:
    static void _bind_methods();

private:
    struct Trigger {
        godot::Vector3 origin;
        godot::Basis to_local; ///< Skyrim space to the volume's axes, scale removed.
        godot::Vector3 half;   ///< Half extents; x is the radius of a sphere.
        std::uint32_t type = 0;
        std::set<std::uint32_t> inside;
    };

    void bind_natives();
    void bind_quest_natives();
    struct QuestState {
        bool running = false;
        bool started = false; ///< Has ever started.
        bool completed = false;
        bool failed = false;
        bool active = false;
        std::int32_t stage = 0;
        std::set<std::int32_t> done;
        /// Objective index -> bit 0 displayed, 1 completed, 2 failed.
        std::map<std::int32_t, std::uint8_t> objectives;
    };
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
    bool disabled(std::uint32_t ref, int depth = 0) const;
    void set_disabled(std::uint32_t ref, bool value);
    bool locked(std::uint32_t ref) const;
    /// The reference whose lock `ref` uses: itself, or a load door's partner.
    std::uint32_t lock_holder(std::uint32_t ref) const;
    void register_trigger(std::uint32_t ref, const godot::Dictionary& info);
    static bool contains(const Trigger& t, const godot::Vector3& p);
    godot::Vector3 position_of(std::uint32_t ref) const;
    void reset();

    std::unique_ptr<vm::Vm> vm_;
    godot::Ref<SkydotPack> pack_;
    godot::Ref<SkydotWorld> world_;
    std::set<std::uint32_t> attached_;
    /// References whose model attach_built has seen.
    std::set<std::uint32_t> built_;
    std::map<std::uint32_t, bool> disabled_;
    std::set<std::uint32_t> blocked_;
    std::map<std::uint32_t, std::int64_t> open_;
    std::map<std::uint32_t, bool> locked_;
    std::map<std::uint32_t, Trigger> triggers_;
    std::map<std::uint32_t, godot::Vector3> actors_;
    std::uint32_t next_form_ = 0xFF000000u;
    std::map<std::uint32_t, QuestState> quests_;
    std::set<std::uint32_t> quest_scripts_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> alias_handles_;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> handle_alias_;
    /// Alias handle -> the reference or location filling it.
    std::map<std::uint32_t, std::uint32_t> alias_fill_;
    std::map<std::uint32_t, double> globals_;
    std::uint32_t next_alias_handle_ = static_cast<std::uint32_t>(ALIAS_HANDLE_BASE);
    mutable std::map<std::uint32_t, godot::Dictionary> quest_info_;
    godot::String last_error_;
};

} // namespace skydot
