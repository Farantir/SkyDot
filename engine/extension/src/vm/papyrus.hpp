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
#include "data/actors.hpp"
#include "magic/active_effects.hpp"
#include "dialogue/dialogue.hpp"
#include "dialogue/dialogue_states.hpp"
#include "data/godot_values.hpp"
#include "scenes/scenes.hpp"
#include "save/save_binding.hpp"
#include "session/clock.hpp"
#include "vm/actor_states.hpp"
#include "vm/condition_world.hpp"
#include "vm/crafting.hpp"
#include "vm/created_forms.hpp"
#include "vm/harvest.hpp"
#include "vm/equipment.hpp"
#include "vm/id_ranges.hpp"
#include "vm/item_states.hpp"
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
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
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
    ~SkydotPapyrus() override;

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

    // ---- items (vm/papyrus_items.cpp; the rules are vm/equipment.hpp, what
    // containers and actors start with is data/start_inventory.hpp)
    /// Resolve `owner`'s start inventory now, if no one has touched it yet:
    /// a container's CNTO, an actor's CNTO and default outfit (worn), the
    /// leveled lists in them drawn with a seed of the reference. Every other
    /// inventory call does this first.
    void resolve(std::int64_t owner);
    /// What the data says of item `form`: found, form, name, editor_id,
    /// category (misc, weapon, armor, clothing, jewelry, potion, food,
    /// ingredient, book, key, soul_gem, scroll, ammo, light), weight, value,
    /// playable, equip_slot, weapon_type, damage, armor_rating, armor_type,
    /// biped_slots. Empty if `form` is no item.
    godot::Dictionary item_info(std::int64_t form) const;
    /// `owner`'s items, resolved first, by name: {form, extra, name, tier,
    /// count, weight, value, category, equipped, playable} each. Units that
    /// carry extra data (tempered, enchanted; vm/item_extras.hpp) are rows of
    /// their own (`extra` != 0, `name` with the tier: "Iron Dagger (Fine)");
    /// created forms (alchemy) are rows like any item.
    godot::Array inventory(std::int64_t owner);
    /// Move up to `count` of `form` from `from` to `to` (an item that runs out
    /// is taken off); returns how many moved. `extra` names the stack (0
    /// plain, -1 any: plain first, then the extras in id order).
    std::int64_t transfer(std::int64_t from, std::int64_t to, std::int64_t form, std::int64_t count,
                          std::int64_t extra = -1);
    /// Equip (or consume, or read) one `form` of `owner`'s; false if it is
    /// not carried or cannot be equipped. Weapons go to the right hand, or the
    /// left where only that fits (see vm/equipment.hpp). `extra` as in transfer.
    bool equip(std::int64_t owner, std::int64_t form, std::int64_t extra = -1);
    /// Take `form` off `owner`; false if it was not equipped. With `extra`
    /// >= 0 only if the unit equipped carries it.
    bool unequip(std::int64_t owner, std::int64_t form, std::int64_t extra = -1);
    /// {right, left, ammo, worn: [forms], right_extra, left_extra, ammo_extra}
    /// of `owner`.
    godot::Dictionary equipment(std::int64_t owner);
    /// The bow `owner` carries (the one of the most damage) with its best ammo: {weapon, ammo},
    /// empty if it has no bow. Computed from the inventory, nothing is saved.
    godot::Dictionary ranged_weapon(std::int64_t owner);
    /// The melee weapon `owner` carries (the one of the most damage), 0 if none.
    std::int64_t melee_weapon(std::int64_t owner);

    /// What the combat AI answers the combat natives with (set by SkydotAi, which owns the
    /// state; cleared when it goes). Without them every actor is out of combat.
    struct CombatHooks {
        std::function<std::int64_t(std::uint32_t ref)> state;    ///< 0 none, 1 combat, 2 searching
        std::function<std::uint32_t(std::uint32_t ref)> target;
        std::function<void(std::uint32_t ref, std::uint32_t target)> start;
        std::function<void(std::uint32_t ref)> stop;
    };
    void set_combat_hooks(CombatHooks hooks) { combat_hooks_ = std::move(hooks); }
    /// An actor's combat state changed: sends OnCombatStateChanged(akTarget, aeCombatState) to its
    /// scripts and aliases (dispatch as OnHit). Returns how many handlers ran.
    std::int64_t combat_state_changed(std::int64_t ref, std::int64_t target, std::int64_t state);
    /// A sleeping actor got up (woken by the player's closeness): sends OnGetUp(None) to its
    /// scripts, as the game does when an actor leaves its furniture. Returns the handlers run.
    std::int64_t send_get_up(std::int64_t ref);
    /// Actor.GetEquippedItemType: what `owner` holds in `hand` (0 left, 1
    /// right): 0 nothing, 1 sword, 2 dagger, 3 axe, 4 mace, 5 two-handed sword,
    /// 6 two-handed axe, 7 bow, 8 staff, 10 shield, 11 torch, 12 crossbow.
    std::int64_t equipped_item_type(std::int64_t owner, std::int64_t hand);
    /// Take up to `count` of `form` from `owner` and put one reference of the
    /// item in the world in `cell` at `position` (Skyrim space) with
    /// `rotation` (radians); it carries the count (activating it picks them up
    /// again) and is saved like any placed reference. Returns the reference,
    /// 0 if `owner` has none.
    std::int64_t drop(std::int64_t owner, std::int64_t form, std::int64_t count, std::int64_t cell,
                      const godot::Vector3& position, const godot::Vector3& rotation, std::int64_t extra = -1);

    // ---- magic (vm/papyrus_magic.cpp; the rules are magic/, docs/adr/0008)
    /// `caster` casts `spell` on `target` (0 or a spell of self delivery: the caster), paying its
    /// magicka (a concentration spell: for `seconds`, one second if 0) and starting a power's
    /// cooldown. {ok, reason ("" or "not enough magicka", "cooldown", "no such spell", "no
    /// caster", "dead"), cost, instances}. No projectiles: the effects land on `target`.
    godot::Dictionary cast_spell(std::int64_t caster, std::int64_t spell, std::int64_t target, double seconds = 0.0);
    /// Only the paying half of a cast: the cooldown of a power, the magicka (`seconds` of a
    /// concentration spell, one second if 0) taken as damage so the regeneration waits, the power's
    /// use noted. {ok, reason ("" or "not enough magicka", "cooldown", "no such spell", "no caster",
    /// "dead"), cost}. An aimed spell is paid at the release and landed at the impact.
    godot::Dictionary pay_spell(std::int64_t caster, std::int64_t spell, double seconds = 0.0);
    /// Only the landing half: the effects of `form` (a SPEL or a scroll item) on `target` (0: the
    /// caster; so do spells of self delivery), no cost, no scroll used up. An area spell is paid once
    /// and landed on each actor. {ok, reason ("" or "no such spell", "no caster", "dead"), instances}.
    godot::Dictionary land_spell(std::int64_t caster, std::int64_t form, std::int64_t target);
    /// What the game layer needs to cast a SPEL or a scroll item: {found, form, name, is_scroll, type
    /// (SPIT), casting_type (1 fire and forget, 2 concentration; scrolls are 1), delivery (0 self,
    /// 1 touch, 2 aimed, 3 target actor, 4 target location; a scroll's is its effect's), charge_time,
    /// cast_duration, range, effects (count), and of the costliest effect: mgef, magnitude, duration,
    /// area_feet, area_units, projectile, explosion, casting_art, hit_art, casting_light (form ids, 0
    /// if none), hostile, recover (it restores), archetype, actor_value (of the effect, -1 if none),
    /// school (actor value of the magic skill, -1 if none)}.
    godot::Dictionary cast_info(std::int64_t form);
    /// The caster reads a scroll it carries: its effects land on `target` (0: the caster) without
    /// magicka, one scroll is used up, the hands holding it are emptied when the last is gone.
    /// {ok, reason ("" or "no such scroll", "not carried", "no caster", "dead"), instances}.
    godot::Dictionary cast_scroll(std::int64_t caster, std::int64_t scroll, std::int64_t target);
    /// Put a poison (an ALCH or created potion the actor carries) on the weapon in its right hand
    /// (bows included; a staff or fists do not count): one poison is used up, the weapon unit gets
    /// the poison for one hit (the extra's `poison`, `poison_doses`; no perks yet). {ok, reason ("" or
    /// "not a poison", "not carried", "no weapon", "already poisoned", "no actor"), weapon, doses}.
    godot::Dictionary apply_poison(std::int64_t actor, std::int64_t poison);
    /// An enchanted weapon unit's charge for the HUD and the menus: {enchantment (0: none), charge,
    /// max, cost (per hit)}. `extra` -1: the unit held in the right hand, else the first carried.
    godot::Dictionary weapon_charge(std::int64_t actor, std::int64_t form, std::int64_t extra = -1);
    /// The attack button on a hand (`hand`: 0 left, 1 right, 2 power) that holds a spell or scroll.
    /// hand_press starts it: {ok, reason ("" or "no spell", "dead", "cooldown", "not enough magicka",
    /// "not carried"), spell, casting_type, charge_time, is_scroll}.
    godot::Dictionary hand_press(std::int64_t actor, std::int64_t hand);
    /// Advance it by `dt` seconds of the button held: {event ("" nothing, "charged" a fire-and-forget
    /// spell can now be released, "tick" a concentration spell was paid for `seconds` and lands again,
    /// "stop" it ended for lack of magicka), seconds, progress (0..1), cost (paid in this step), spell, hand}.
    godot::Dictionary hand_step(std::int64_t actor, std::int64_t hand, double dt);
    /// The button is released: {event ("cast" fire and forget held long enough: pay_spell (or
    /// cast_scroll) and land it; "stop" a concentration spell ended; "" released too early: nothing,
    /// no cost), seconds (held), spell, is_scroll, hand}.
    godot::Dictionary hand_release(std::int64_t actor, std::int64_t hand);
    /// What `spell` costs `caster` at its skill (0 for `caster` 0: the stored cost).
    double spell_cost(std::int64_t caster, std::int64_t spell);
    /// Take magicka from `caster` as damage, so the regeneration delay applies.
    void spend_magicka(std::int64_t caster, double amount);
    /// The effects of an ALCH, INGR or SCRL (or created potion) on `actor`: {ok, reason, effects}.
    /// An ingredient gives its first effect only; a poison is not applied (reason "poison": see
    /// apply_poison; an ALCH whose effects are all hostile counts as one, the pack has no poison flag).
    godot::Dictionary apply_item_effects(std::int64_t actor, std::int64_t form, std::int64_t extra = 0);
    /// The spells `actor` knows, as form ids.
    godot::PackedInt64Array spells_of(std::int64_t actor);
    bool add_spell(std::int64_t actor, std::int64_t spell);
    bool remove_spell(std::int64_t actor, std::int64_t spell);
    bool has_spell(std::int64_t actor, std::int64_t spell);
    /// `hand`: 0 left, 1 right, 2 power. A scroll the actor carries may be held in a hand too.
    bool equip_spell(std::int64_t actor, std::int64_t spell, std::int64_t hand);
    std::int64_t equipped_spell(std::int64_t actor, std::int64_t hand);
    /// Game days until `spell` (a power) may be used again by `actor`; 0 if it may.
    double power_cooldown(std::int64_t actor, std::int64_t spell);
    /// What runs on `actor`: {instance, mgef, name, magnitude, duration, elapsed, remaining (-1 if
    /// constant), source (a form), source_kind (0 spell, 1 enchantment, 2 potion, 3 ingredient, 4
    /// scroll, 5 ability, 6 poison), caster, archetype, active}.
    godot::Array active_effects(std::int64_t actor);
    /// The held states of the magic archetypes on `ref`: {calm, frenzy, paralyzed, invisible, fear_from,
    /// commanded_by, time_scale}. `magic_overrides_of` is the same for C++ (the AI).
    godot::Dictionary magic_overrides(std::int64_t ref) const;
    [[nodiscard]] magic::Overrides magic_overrides_of(std::uint32_t ref) const;
    /// The base form of `ref`, placed by a script or by the world; 0 if unknown (the AI reads a
    /// summon's NPC_ here).
    std::uint32_t base_form_of(std::uint32_t ref) const;
    /// The actor attacked, cast or activated: its invisibility ends; how many effects.
    std::int64_t break_invisibility(std::int64_t ref);
    /// The player put `weapon` away: its bound weapon effect ends.
    bool dispel_bound_weapon(std::int64_t actor, std::int64_t weapon);
    /// End `spell`'s effects on `actor`, or all of them.
    bool dispel_spell(std::int64_t actor, std::int64_t spell);
    void dispel_all_spells(std::int64_t actor);
    /// The game layer built `actor`: its abilities start (they are not saved).
    void prepare_actor_magic(std::int64_t actor);
    /// Archetypes started without rules: archetype -> count.
    godot::Dictionary get_unhandled_archetypes() const;

    // ---- crafting and harvesting (vm/papyrus_crafting.cpp; the rules are
    // vm/crafting.hpp and vm/harvest.hpp)
    /// The create-object recipes of the bench `bench_ref` (a FURN, type 1):
    /// {recipe, product, name, count, components: [{form, name, need, have}],
    /// craftable} each.
    godot::Array recipes(std::int64_t bench_ref);
    /// Make `count` of the recipe; returns how many were made.
    std::int64_t craft(std::int64_t recipe, std::int64_t count);
    /// The tempering rows of a smithing bench (type 2 or 7): {recipe, form,
    /// extra, name, tier, next_tier, components, craftable} each.
    godot::Array temper_rows(std::int64_t bench_ref);
    /// Temper one unit of the stack (form, extra).
    bool temper(std::int64_t recipe, std::int64_t form, std::int64_t extra);
    /// The enchanting stub (types 3, 4): {form, extra, name} of the carried
    /// items with an enchantment. There are no actions yet (C5).
    godot::Array enchant_rows();
    /// The carried ingredients: {form, name, count, effects: [{name, known}]}.
    godot::Array alchemy_rows();
    /// What mixing `forms` makes: {name, poison, value, effects: [{name,
    /// magnitude, duration, description}], mixable}.
    godot::Dictionary alchemy_preview(const godot::PackedInt64Array& forms);
    /// Mix `forms`; the created potion's form, 0 if it could not be done.
    std::int64_t alchemy_mix(const godot::PackedInt64Array& forms);
    /// Whether the harvestable `ref` is harvested now.
    bool harvested(std::int64_t ref) const;
    /// Every reference that is harvested now (respawned ones are not listed).
    godot::PackedInt64Array get_harvested_refs() const;
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
    /// The player is at Health 0 (`player_died` was sent; Health above 0 again clears it).
    bool is_player_dead() const { return player_dead_; }
    bool is_actor_dead(std::int64_t ref) const { return actors_.is_dead(static_cast<std::uint32_t>(ref)); }
    /// How many of `item` `owner` carries (the player's key check).
    std::int64_t get_item_count(std::int64_t owner, std::int64_t item) const;
    double get_actor_value(std::int64_t ref, const godot::String& name) const;
    double get_actor_value_max(std::int64_t ref, const godot::String& name) const;
    /// Lower `name` of `ref` by `amount`; an actor at Health 0 dies (signal
    /// `actor_died`) unless it is essential or the player.
    void damage_actor_value(std::int64_t ref, const godot::String& name, double amount);

    /// `apply_hit` flags.
    static constexpr std::int64_t HIT_POWER = 1;
    static constexpr std::int64_t HIT_SNEAK = 2;
    static constexpr std::int64_t HIT_BASH = 4;
    static constexpr std::int64_t HIT_BLOCKED = 8;
    /// One melee hit of `attacker` (0: none) on `target` with `weapon` (0:
    /// fists; `extra` its item extra) for the race attack named `attack_event`
    /// (e.g. "attackStart", "attackPowerStartInPlace"; empty: the standing
    /// light or power attack). Resolves the inputs, damages Health, logs a
    /// `combat:` line, sends OnHit and, on a death, OnDying and OnDeath (the
    /// signal actor_died too); keeps the killer for Actor.GetKiller.
    /// {error} if the target is no living actor, else {attacker, target,
    /// weapon, event, base, skill_value, skill, attack, difficulty, sneak,
    /// weapon_damage, armor_rating, armor_percent, damage, health_before,
    /// health_after, killed}. Blocking only passes the flag on.
    /// `ammo` (an AMMO form, 0 for melee) adds the arrow's damage to the bow's.
    godot::Dictionary apply_hit(std::int64_t attacker, std::int64_t target, std::int64_t weapon,
                                std::int64_t extra, const godot::String& attack_event, std::int64_t flags,
                                std::int64_t ammo = 0);
    /// Where a dead actor lies ({space, position, rot_z}; empty: not set).
    godot::Dictionary get_corpse_place(std::int64_t ref) const;
    void set_corpse_place(std::int64_t ref, std::int64_t space, const godot::Vector3& position, double rot_z);
    /// Who killed `ref`; 0 if nobody or alive.
    std::int64_t get_killer(std::int64_t ref) const;
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

    // ---- dialogue (vm/papyrus_dialogue.cpp; the rules are dialogue/dialogue.hpp)
    /// Whether activating `ref` may open a dialogue: a placed actor that is
    /// not the player and not dead.
    bool dialogue_can_talk(std::int64_t ref) const;
    /// The player starts talking to `speaker`. {lines: [{text, seconds, info,
    /// first, last}], topics: [{id, text, said}], closed}. Lines are spoken
    /// one after the other; the host runs `dialogue_fragment` for an INFO at
    /// its `first` line's start and its `last` line's end.
    godot::Dictionary dialogue_open(std::int64_t speaker);
    godot::Dictionary dialogue_choose(std::int64_t topic);
    void dialogue_close();
    bool dialogue_is_open() const;
    /// What `dialogue_open` would say and list, changing nothing (tests).
    godot::Dictionary dialogue_topics_debug(std::int64_t speaker);
    /// Derived voice stems of every INFO against the pack's `vpaths` (tools/voice_census.gd).
    godot::Dictionary voice_census(const godot::PackedStringArray& vpaths) const;
    /// Run the begin (or end) fragment of INFO `info`, with `speaker` as
    /// akSpeakerRef. Returns whether the INFO has one.
    bool dialogue_fragment(std::int64_t info, bool end, std::int64_t speaker);

    // ---- scenes (vm/papyrus_scenes.cpp; the rules are scenes/scenes.hpp)
    /// Advance the scenes by `seconds` of game time. Returns the dialogue
    /// actions to speak: [{scene, action, speaker, lines}], lines as
    /// `dialogue_open` gives them (with `voice`). The host speaks them and
    /// reports `scene_action_done`.
    godot::Array scenes_update(double seconds);
    void scene_action_done(std::int64_t scene, std::int64_t action);
    bool scene_start(std::int64_t scene, bool force);
    void scene_stop(std::int64_t scene);
    bool scene_is_playing(std::int64_t scene) const;
    /// Whether a scene's actors must have their 3D loaded (default), else a
    /// living placed actor is enough (headless tests).
    void set_scene_actors_need_3d(bool need) { scene_needs_3d_ = need; }

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
    /// What was said (DLGS), the selection logic over it and the INFOs whose
    /// fragment scripts are attached (INFO -> its topic's quest).
    dialogue::DialogueStates dialogue_states_;
    std::unique_ptr<dialogue::DialogueHost> dialogue_host_;
    std::unique_ptr<dialogue::Dialogue> dialogue_;
    std::map<std::uint32_t, std::uint32_t> info_quests_;
    dialogue::Dialogue* dialogue_logic();
    friend class DialogueHostImpl;
    friend class SceneHostImpl;
    std::unique_ptr<scenes::SceneHost> scene_host_;
    std::unique_ptr<scenes::Scenes> scenes_;
    std::set<std::uint32_t> scene_scripts_;
    bool scene_needs_3d_ = true;
    scenes::Scenes* scene_logic();
    void bind_scene_natives();
    /// The NPC_ behind an actor reference (the player: 0x7); 0 if none.
    std::uint32_t dialogue_npc(std::uint32_t ref) const;
    godot::Dictionary dialogue_dictionary(const dialogue::Result& result) const;
    godot::Array lines_array(const std::vector<dialogue::Line>& lines) const;
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
    void bind_magic_natives();
    CombatHooks combat_hooks_;
    bool player_dead_ = false;
    std::uint32_t last_damager_ = 0; ///< Who last hurt the player.
    /// Player at Health 0: log `death: player killed by 0xA` and send `player_died` once.
    void check_player_death();
    void bind_item_natives();
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
    std::uint32_t place_ref(const vm::PlacedRef& placed, bool initially_disabled);
    /// The base's DEST, null if `ref` is not destructible.
    const bethconv::pack::wfb::Destruction* destruction_of(std::uint32_t ref) const;
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
    /// Resolved owners, leveled actor picks, equipment (ITST). Mutable: an
    /// inventory resolves on the first look at it, also from const queries.
    mutable vm::ItemStates items_;
    void resolve_owner(std::uint32_t owner) const;
    vm::ItemInfo item_info_of(std::uint32_t form) const;
    /// Take items from `owner` (resolved first); an item that runs out is
    /// taken off.
    std::int64_t remove_item(std::uint32_t owner, std::uint32_t form, std::int64_t count);
    /// Equip one `form` of `owner`'s; `left` asks for the left hand where
    /// either will do. Adds the item if `add_missing`.
    bool equip_item(std::uint32_t owner, std::uint32_t form, bool left, bool add_missing, std::uint32_t extra = 0);
    bool unequip_item(std::uint32_t owner, std::uint32_t form);
    /// Like remove_item for the units carrying `extra` only.
    std::int64_t remove_item_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t extra, std::int64_t count);
    /// Take items, saying which units (extras) went.
    std::vector<vm::Inventory::Taken> remove_item_split(std::uint32_t owner, std::uint32_t form, std::int64_t count);
    /// The extra id of a stack named by the API's `extra` (-1: the first
    /// carried: plain, else the lowest id); 0 if `form` is not carried.
    std::uint32_t resolve_extra(std::uint32_t owner, std::uint32_t form, std::int64_t extra) const;
    /// Unequip what is equipped of `form` once no unit of the equipped
    /// stack is left.
    void refresh_equipment(std::uint32_t owner, std::uint32_t form);
    /// The magic of all actors: effects, spell books and the adapters (vm/papyrus_magic.cpp).
    struct MagicRuntime {
        virtual ~MagicRuntime() = default;
    };
    struct MagicState;
    std::unique_ptr<MagicRuntime> magic_;
    MagicState* magic_state();
    /// Send what effects started or finished since the last call (Papyrus events, the signal).
    void flush_magic_events();
    void tick_magic(double seconds);
    /// An actor died: its effects end (but the ones that survive death).
    void magic_actor_died(std::uint32_t ref);
    /// Forget all effects and spell changes (a load replaces the game, a setup the world).
    void clear_magic();
    /// The MAGC section's bytes; empty if magic never ran.
    std::vector<std::uint8_t> magic_section() const;
    /// Whether `bytes` is a MAGC section this world can load; its two parts.
    bool magic_decode(save::ByteReader r, std::vector<std::uint8_t>& effects, std::vector<std::uint8_t>& book);
    /// Make the constant effects of the enchanted armor `owner` wears what they should be (start the
    /// new, end the gone); no-op before magic ran (priming derives them then).
    void sync_worn_magic(std::uint32_t owner);
    /// The unit of `form` that carried extra `from` carries `to` now: the inventory entry and the
    /// equipment slot move together (ItemExtras interns, so a changed value is a new id).
    void move_unit_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to);
    /// A weapon hit's magic (poison, enchantment, charge): {poison, enchantment, fired, charge, charge_max}.
    godot::Dictionary hit_magic(std::uint32_t attacker, std::uint32_t target, std::uint32_t weapon,
                                std::uint32_t extra, bool target_alive);
    godot::Dictionary poison_weapon(std::uint32_t owner, std::uint32_t poison, std::int64_t extra);
    /// Take a decoded MAGC section (or none) and hold the temporary modifiers again.
    void magic_adopt(bool has, const std::vector<std::uint8_t>& effects, const std::vector<std::uint8_t>& book);
    /// Created potions and what is known of ingredients (CRFT).
    vm::CreatedForms created_;
    /// What is harvested (HRVS).
    vm::HarvestStates harvests_;
    struct CraftingAdapter;
    std::unique_ptr<crafting::CraftingHost> crafting_host_;
    std::unique_ptr<crafting::Crafting> crafting_;
    /// The crafting rules on the VM's inventory; null before setup.
    crafting::Crafting* crafting_logic();
    /// A name for any item, also created forms and ones the pack lacks.
    std::string item_name_of(std::uint32_t form) const;
    /// The game day now (GameDaysPassed).
    double game_days() const;
    void unequip_all(std::uint32_t owner);
    /// The worn armor of resolved actors, for the actor builder.
    WornProvider worn_provider() const;
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
        vm::ItemStates items;
        vm::CreatedForms created;
        std::vector<std::uint8_t> magic_effects;
        std::vector<std::uint8_t> magic_book;
        bool has_magic = false;
        vm::HarvestStates harvests;
        dialogue::DialogueStates dialogue;
        std::unique_ptr<scenes::Scenes> scenes;
        HostSnapshot host;
    };
    std::unique_ptr<PendingLoad> pending_;
    godot::Error prepare(const save::SaveReader& file);
    void write_host(save::SaveWriter& out) const;
    bool decode_host(const save::SaveReader& file, HostSnapshot& out, std::string& error) const;
};

} // namespace skydot
