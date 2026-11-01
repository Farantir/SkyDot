// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's magic: the state (MagicState, papyrus_magic_state.hpp), the effects' events, the
// spell book, the save section and the natives' binding. Events of effects (a start, a finish) are
// queued while the rules run and sent afterwards (`flush_magic_events`), because a script that
// answers one may dispel another effect. The game layer's API is papyrus_casting.cpp,
// papyrus_shouts.cpp and papyrus_enchantment_effects.cpp.
#include "vm/papyrus_magic_state.hpp"

namespace skydot {

SkydotPapyrus::MagicState* SkydotPapyrus::magic_state() {
    if (magic_) {
        return static_cast<MagicState*>(magic_.get());
    }
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (root == nullptr || !vm_) {
        return nullptr;
    }
    magic_ = std::make_unique<MagicState>(*this, *root);
    return static_cast<MagicState*>(magic_.get());
}

namespace {

/// The log word of a held archetype ("" for the others); fear names its source.
std::string held_state_word(const magic::EffectView& v) {
    switch (v.archetype) {
    case magic::archetype::calm: return "calm";
    case magic::archetype::demoralize:
    case magic::archetype::turn_undead: return "fear from " + hex(v.caster);
    case magic::archetype::frenzy: return "frenzy";
    case magic::archetype::paralysis: return "paralyzed";
    case magic::archetype::invisibility: return "invisible";
    case magic::archetype::slow_time: return "slow time " + number(magic::slow_time_scale(v.magnitude));
    default: return {};
    }
}

Dictionary effect_event(const magic::EffectView& v, bool started) {
    Dictionary d;
    d["started"] = started;
    d["instance"] = static_cast<std::int64_t>(v.id);
    d["mgef"] = static_cast<std::int64_t>(v.mgef);
    d["archetype"] = static_cast<std::int64_t>(v.archetype);
    d["caster"] = static_cast<std::int64_t>(v.caster);
    d["target"] = static_cast<std::int64_t>(v.target);
    d["magnitude"] = v.magnitude;
    d["duration"] = v.duration;
    d["area"] = v.area;
    d["associated"] = static_cast<std::int64_t>(v.associated);
    d["summoned"] = static_cast<std::int64_t>(v.summoned);
    return d;
}

godot::Dictionary overrides_dictionary(const magic::Overrides& o) {
    Dictionary d;
    d["calm"] = o.calm;
    d["frenzy"] = o.frenzy;
    d["paralyzed"] = o.paralyzed;
    d["invisible"] = o.invisible;
    d["fear_from"] = static_cast<std::int64_t>(o.fear_from);
    d["commanded_by"] = static_cast<std::int64_t>(o.commanded_by);
    d["time_scale"] = o.time_scale;
    return d;
}

} // namespace

magic::Overrides SkydotPapyrus::magic_overrides_of(std::uint32_t ref) const {
    return magic_ ? static_cast<const MagicState*>(magic_.get())->effects.overrides(ref) : magic::Overrides{};
}

Dictionary SkydotPapyrus::magic_overrides(std::int64_t ref) const {
    return overrides_dictionary(magic_overrides_of(static_cast<std::uint32_t>(ref)));
}

std::int64_t SkydotPapyrus::break_invisibility(std::int64_t ref) {
    auto* m = magic_state();
    if (m == nullptr) {
        return 0;
    }
    const auto n = m->effects.break_invisibility(static_cast<std::uint32_t>(ref));
    if (n != 0) {
        m->log("magic: invisibility of " + hex(static_cast<std::uint32_t>(ref)) + " broken");
    }
    flush_magic_events();
    return static_cast<std::int64_t>(n);
}

bool SkydotPapyrus::dispel_bound_weapon(std::int64_t actor, std::int64_t weapon) {
    auto* m = magic_state();
    const bool any = m != nullptr && m->effects.dispel_bound(static_cast<std::uint32_t>(actor),
                                                            static_cast<std::uint32_t>(weapon)) != 0;
    flush_magic_events();
    return any;
}

void SkydotPapyrus::flush_magic_events() {
    auto* m = magic_state();
    if (m == nullptr || m->flushing) {
        return;
    }
    m->flushing = true;
    while (!m->pending.empty()) {
        auto batch = std::move(m->pending);
        m->pending.clear();
        for (const auto& event : batch) {
            const auto& v = event.view;
            emit_signal("magic_effect_changed", static_cast<std::int64_t>(v.target), static_cast<std::int64_t>(v.mgef),
                        static_cast<std::int64_t>(v.archetype), event.started, static_cast<std::int64_t>(v.id));
            emit_signal("magic_effect_event", effect_event(v, event.started));
            if (const std::string word = held_state_word(v); !word.empty()) {
                m->log("magic: " + hex(v.target) + " " + word + (event.started ? " starts" : " ends"));
            }
            if (event.started && v.archetype == magic::archetype::stagger) {
                m->log("magic: " + hex(v.target) + " staggered (" + number(v.magnitude) + ")");
            }
            const auto* mgef = lookup(m->world_.magic_effects(), v.mgef);
            const auto* scripts = mgef != nullptr ? mgef->scripts() : nullptr;
            if (scripts == nullptr || scripts->size() == 0) {
                continue;
            }
            // The script object of this effect: its handle is the instance id.
            m->snapshots[v.id] = v;
            const vm::Value target = vm_->object(v.target, "Actor");
            const vm::Value caster = vm_->object(v.caster, "Actor");
            if (event.started) {
                attach_scripts(v.id, script_list(scripts, false), 0);
                vm_->send_event(v.id, "OnEffectStart", {target, caster});
            } else {
                vm_->send_event(v.id, "OnEffectFinish", {target, caster});
                m->detach_later.add(v.id);
            }
        }
    }
    m->flushing = false;
}

void SkydotPapyrus::tick_magic(double seconds) {
    auto* m = magic_state();
    if (m == nullptr) {
        return;
    }
    m->effects.tick(seconds);
    for (const std::uint32_t id : m->effects.take_forgotten()) { // forgotten while the effects ran
        vm_->detach(id);
        m->snapshots.erase(id);
    }
    m->shouts.tick(seconds); // real seconds (docs/adr/0008)
    flush_magic_events();
    // Script objects of finished effects go once their OnEffectFinish had its updates (VM seconds).
    for (const std::uint32_t id : m->detach_later.tick(seconds)) {
        vm_->detach(id);
        m->snapshots.erase(id);
    }
}


magic::SpellBook* SkydotPapyrus::spell_book() {
    auto* m = magic_state();
    return m != nullptr ? &m->book : nullptr;
}

magic::ActorSoul SkydotPapyrus::soul_of(std::uint32_t actor) {
    auto* m = magic_state();
    const std::uint32_t level = m != nullptr ? m->actor_level(actor) : 0;
    const std::uint32_t npc = world_.is_valid() ? world_->data().keyword_by_editor_id("ActorTypeNPC") : 0;
    const bool is_npc = npc != 0 && PapyrusConditionWorld(this).has_keyword(actor, npc);
    return magic::actor_soul(std::max(level, 1U), is_npc);
}

void SkydotPapyrus::magic_actor_died(std::uint32_t ref) {
    if (!magic_) {
        return;
    }
    auto* m = static_cast<MagicState*>(magic_.get());
    // Soul Trap cast by the player: the soul goes into a gem as the actor dies.
    if (ref != k_player_ref) {
        const auto views = m->effects.effects_on(ref);
        // The spell's own effect is a Script archetype with Papyrus scripts (SoulTrapFFActor); the
        // enchantments' and perks' are archetype 23. Either, cast by the player, is a Soul Trap.
        const auto soul_trap = [&](const magic::EffectView& v) {
            if (v.caster != k_player_ref) {
                return false;
            }
            if (v.archetype == k_archetype_soul_trap) {
                return true;
            }
            const auto* mgef = lookup(m->world_.magic_effects(), v.mgef);
            return mgef != nullptr && mgef->editor_id() != nullptr && mgef->editor_id()->str().starts_with("SoulTrap");
        };
        if (std::ranges::any_of(views, soul_trap)) {
            const auto fill = fill_gem(ref);
            emit_signal("soul_trapped", static_cast<std::int64_t>(ref), fill["filled"], fill["gem"], fill["soul"]);
        }
    }
    m->effects.on_death(ref);
    m->worn.forget(ref);
    flush_magic_events();
}

void SkydotPapyrus::clear_magic() {
    if (!magic_) {
        return;
    }
    auto* m = static_cast<MagicState*>(magic_.get());
    m->effects.clear();
    m->book.clear();
    m->shouts.clear();
    m->worn.clear();
    m->pending.clear();
    m->snapshots.clear();
    m->detach_later.clear();
}

std::vector<std::uint8_t> SkydotPapyrus::magic_section() const {
    if (!magic_) {
        return {};
    }
    const auto* m = static_cast<const MagicState*>(magic_.get());
    return magic::write_magc(m->effects, m->book, m->shouts);
}

bool SkydotPapyrus::magic_decode(save::ByteReader r, std::vector<std::uint8_t>& effects,
                                 std::vector<std::uint8_t>& book, std::vector<std::uint8_t>& shouts) {
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    magic::MagcParts parts;
    if (root == nullptr || !magic::read_magc(r, parts) || !magic::check_magc(*root, parts)) {
        return false;
    }
    effects = std::move(parts.effects);
    book = std::move(parts.book);
    shouts = std::move(parts.shouts);
    return true;
}

void SkydotPapyrus::magic_adopt(bool has, const std::vector<std::uint8_t>& effects,
                                const std::vector<std::uint8_t>& book, const std::vector<std::uint8_t>& shouts) {
    auto* m = magic_state();
    if (m == nullptr) {
        return;
    }
    m->effects.clear();
    m->book.clear();
    m->shouts.clear();
    m->worn.clear();
    m->pending.clear();
    m->snapshots.clear();
    m->detach_later.clear();
    if (has) {
        // The actor values were replaced without their temporary part: the effects hold it again.
        (void)magic::SpellBook::load(save::ByteReader(book), m->book);
        // A version 1 section has no shout part: the book stays empty.
        if (!shouts.empty()) {
            (void)magic::ShoutBook::load(save::ByteReader(shouts), m->shouts);
        }
        (void)m->effects.load(save::ByteReader(effects));
    }
    // The VM was restored first: script objects of effects that are over (finished before the save, or not in the
    // loaded effects) have nothing to belong to; they get their grace for a handler under way, then go.
    for (const std::uint32_t form : vm_->attached_forms()) {
        if (vm::id_ranges::effect_instances.contains(form) && !m->effects.instance(form)) {
            m->detach_later.add(form);
        }
    }
    // Abilities are not saved: start them again for whoever is known.
    m->book.reset_abilities();
    std::vector<std::uint32_t> wearers;
    for (const auto& [owner, equipment] : item_.states().equipments()) {
        if (!equipment.worn.empty() || equipment.left != 0) {
            wearers.push_back(owner);
        }
    }
    const auto refs = magic::actors_to_rederive(m->book, m->effects, wearers);
    for (const auto ref : refs) {
        m->prime(ref);
    }
}


void SkydotPapyrus::bind_magic_natives() {
    // The natives need the world; a VM without one has no spells to cast (setup binds again).
    auto* m = magic_state();
    if (m == nullptr) {
        return;
    }
    vm::bind_spell_natives(*vm_, m->world_, *m);
    vm::bind_magiceffect_natives(*vm_, m->world_);
    vm::bind_activemagiceffect_natives(*vm_, *m);
    vm::bind_actor_magic_natives(*vm_, *m);
    vm::bind_shout_natives(*vm_, m->world_);
}


} // namespace skydot
