// SPDX-License-Identifier: GPL-3.0-or-later
//
// The magic effects that are running on actors (docs/adr/0008). One `ActiveEffects` owns them all:
// an instance is an effect (MGEF) from a source (spell, enchantment, potion...) cast by a caster on
// a target, with a magnitude, a duration and the time it has run. The world the effects act on
// (actor values, conditions, deaths, the log, events for Papyrus and for presentation) comes
// through `MagicHost`, as the crafting rules get theirs through CraftingHost. Nothing here touches
// Godot.
//
// Rules (GAMEPLAY-PLAN 7a-2, UESP "Magic Effects"): a value modifier with duration 0 acts once; with
// a duration and no Recover flag it acts magnitude per second; with Recover (or when constant) it
// holds a temporary modifier that is taken away at the end. Detrimental subtracts. A peak value
// modifier lets only the largest of its kind count per actor value; a dual value modifier also acts
// on a second actor value with magnitude x weight; absorb takes from the target and gives the
// caster. Hostile effects are cut by the target's resistance. Conditions are checked at the start
// and every second (PLACEHOLDER interval); an effect whose conditions fail is held inactive.
// Archetypes without rules yet are logged once and counted; the instance lives all the same.
#pragma once

#include "magic/magic_rules.hpp"
#include "save/bytes.hpp"
#include "vm/id_ranges.hpp"

#include "world_generated.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::magic {

enum class SourceKind : std::uint8_t { spell = 0, enchantment, potion, ingredient, scroll, ability, poison };

struct Source {
    SourceKind kind = SourceKind::spell;
    std::uint32_t form = 0;  ///< The SPEL, ENCH or item.
    std::uint32_t extra = 0; ///< The item extra (an enchanted weapon's), 0 if none.
    friend bool operator==(const Source&, const Source&) = default;
};

/// One EFID/EFIT entry to apply. `area` is in feet, `duration` in seconds.
struct EffectInput {
    std::uint32_t effect = 0;
    float magnitude = 0.0F;
    std::uint32_t area = 0;
    std::uint32_t duration = 0;
    /// The entry's position in its source's list (its conditions are found by it); -1 if none.
    std::int32_t index = -1;
};

/// The entries of a SPEL, ENCH or item.
[[nodiscard]] std::vector<EffectInput> inputs_of(
    const flatbuffers::Vector<flatbuffers::Offset<wfb::EffectItem>>* effects);

struct ApplyOptions {
    /// Multiplies the magnitude (dual casting, power; 1 until P6).
    double scale = 1.0;
    bool dual_cast = false;
    /// The effects are constant (an ability, a worn enchantment): they hold until dispelled.
    bool constant = false;
};

/// What the menus, the log and the events see of an instance.
struct EffectView {
    std::uint32_t id = 0;
    std::uint32_t mgef = 0;
    std::uint32_t archetype = 0;
    Source source;
    std::uint32_t caster = 0;
    std::uint32_t target = 0;
    double magnitude = 0.0;
    double duration = 0.0;
    double elapsed = 0.0;
    double area = 0.0; ///< Game units.
    bool active = true;
    bool permanent = false;
    std::uint32_t flags = 0; ///< The MGEF's.
};

class MagicHost {
public:
    virtual ~MagicHost() = default;
    [[nodiscard]] virtual double actor_value(std::uint32_t ref, std::uint32_t av) const = 0;
    /// Lower the value of `av` by `amount` (> 0).
    virtual void damage_value(std::uint32_t ref, std::uint32_t av, double amount) = 0;
    /// Give back at most `amount` (> 0), never above the maximum.
    virtual void restore_value(std::uint32_t ref, std::uint32_t av, double amount) = 0;
    /// Add `delta` (either sign) to the temporary modifier; counts in value and maximum.
    virtual void modify_temp(std::uint32_t ref, std::uint32_t av, double delta) = 0;
    /// Whether `list` holds for `subject`, with `caster` as the reference.
    [[nodiscard]] virtual bool conditions_pass(
        const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, std::uint32_t subject,
        std::uint32_t caster) = 0;
    [[nodiscard]] virtual bool is_dead(std::uint32_t ref) const = 0;
    [[nodiscard]] virtual double game_setting(std::string_view /*name*/, double fallback) const { return fallback; }
    /// An effect started (it acts for the first time) or finished: the VM sends OnEffectStart and
    /// OnEffectFinish, the game layer shows it.
    virtual void effect_changed(bool /*started*/, const EffectView& /*effect*/) {}
    virtual void log(const std::string& line) = 0;
};

class ActiveEffects {
public:
    static constexpr std::uint32_t k_section_version = 1;
    /// PLACEHOLDER: how often conditions are checked, seconds.
    static constexpr double k_condition_interval = 1.0;
    /// The caster 0 is "no caster" (a trap, a hazard).

    ActiveEffects(const wfb::World& world, MagicHost& host) : world_(world), host_(host) {}

    /// Apply `effects` of `source` from `caster` to `target`; the instances made or refreshed, in
    /// order (an effect that does nothing is left out). The same caster, target, source and entry
    /// refresh the instance they made (concentration, a recast) rather than stack.
    std::vector<std::uint32_t> apply(std::uint32_t caster, std::uint32_t target, const Source& source,
                                     std::span<const EffectInput> effects, const ApplyOptions& options = {});
    /// Run all effects by `seconds` of play.
    void tick(double seconds);

    /// Remove what `source_form` put on `target`; how many instances.
    std::size_t dispel(std::uint32_t target, std::uint32_t source_form);
    std::size_t dispel_all(std::uint32_t target);
    bool dispel_instance(std::uint32_t id);
    /// `target` died: everything goes except effects with the no-death-dispel flag.
    void on_death(std::uint32_t target);

    /// Whether an active effect `mgef` runs on `target`.
    [[nodiscard]] bool has_effect(std::uint32_t target, std::uint32_t mgef) const;
    [[nodiscard]] bool has_effect_keyword(std::uint32_t target, std::uint32_t keyword) const;
    /// All effects on `target`, in instance order (for the menu).
    [[nodiscard]] std::vector<EffectView> effects_on(std::uint32_t target) const;
    [[nodiscard]] std::optional<EffectView> instance(std::uint32_t id) const;
    [[nodiscard]] std::size_t size() const { return instances_.size(); }
    /// How often an effect of an archetype without rules was started.
    [[nodiscard]] const std::map<std::uint32_t, std::uint64_t>& unhandled() const { return unhandled_; }

    /// Drop everything without undoing it (the actor values were replaced by a load).
    void clear();
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    /// Replace the instances by a saved section; the temporary modifiers are held again (the actor
    /// values must not hold them yet). False if it is cut short or names nothing sensible.
    [[nodiscard]] bool load(save::ByteReader r);
    /// Whether `r` decodes, changing nothing (a load is checked before it is committed).
    [[nodiscard]] static bool check(const wfb::World& world, save::ByteReader r);

private:
    struct Instance {
        std::uint32_t id = 0;
        std::uint32_t mgef = 0;
        Source source;
        std::int32_t index = -1;
        std::uint32_t caster = 0;
        std::uint32_t target = 0;
        double magnitude = 0.0;
        double duration = 0.0;
        double elapsed = 0.0;
        double area = 0.0;
        double since_check = 0.0;
        bool active = true;
        bool started = false;
        bool permanent = false;
        // Derived from the MGEF, not saved:
        std::uint32_t archetype = 0;
        std::uint32_t flags = 0;
        std::int32_t actor_value = -1;
        std::int32_t second_actor_value = -1;
        double second_weight = 0.0;
        std::map<std::uint32_t, double> held; ///< Temporary modifiers this instance holds, by actor value.
    };

    [[nodiscard]] MagicSettings settings() const;
    [[nodiscard]] bool handled(std::uint32_t archetype) const;
    [[nodiscard]] bool instant(const Instance& i) const { return !i.permanent && i.duration <= 0.0; }
    [[nodiscard]] bool holds(const Instance& i) const;
    [[nodiscard]] bool detrimental(const Instance& i) const { return (i.flags & mgef_flag::detrimental) != 0; }
    [[nodiscard]] EffectView view(const Instance& i) const;
    [[nodiscard]] bool conditions_ok(const Instance& i) const;
    [[nodiscard]] const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* entry_conditions(
        const Instance& i) const;
    bool fill_from_mgef(Instance& i) const;
    void begin(Instance& i);
    void act_once(const Instance& i);
    void act_over(const Instance& i, double seconds);
    void apply_to(std::uint32_t target, std::uint32_t av, double amount, bool takes);
    void sync_holds(std::uint32_t target);
    void finish(std::uint32_t id);
    void log_start(const Instance& i);

    const wfb::World& world_;
    MagicHost& host_;
    std::map<std::uint32_t, Instance> instances_;
    vm::IdAllocator ids_{vm::id_ranges::effect_instances};
    std::map<std::uint32_t, std::uint64_t> unhandled_;
    std::set<std::uint32_t> logged_;
    bool dual_logged_ = false;
};

} // namespace skydot::magic
