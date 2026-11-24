// SPDX-License-Identifier: GPL-3.0-or-later
//
// The magic domain of the Papyrus runtime (vm/magic_runtime.hpp).
#include "vm/magic_runtime.hpp"

#include "data/actors.hpp"
#include "data/fb_search.hpp"
#include "data/leveled.hpp"
#include "data/world_data.hpp"
#include "magic/casting.hpp"
#include "magic/magic_save.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace skydot::vm {

namespace {

/// The log word of a held archetype ("" for the others); fear names its source.
std::string held_state_word(const magic::EffectView& v) {
    switch (v.archetype) {
    case magic::archetype::calm: return "calm";
    case magic::archetype::demoralize:
    case magic::archetype::turn_undead: return "fear from " + magic_hex(v.caster);
    case magic::archetype::frenzy: return "frenzy";
    case magic::archetype::paralysis: return "paralyzed";
    case magic::archetype::invisibility: return "invisible";
    case magic::archetype::slow_time: return "slow time " + magic_number(magic::slow_time_scale(v.magnitude));
    default: return {};
    }
}

/// MGEF archetype 23.
constexpr std::uint32_t k_archetype_soul_trap = 23;

} // namespace

MagicRuntime::MagicRuntime(Host& host, const wfb::World& world, ItemRuntime& items, CombatRuntime& combat)
    : host_(&host), world_(world), items_(items), combat_(combat), effects(world, *this), book(world, effects), shouts(world),
      worn(world, effects, &host.created_forms()) {
    shouts.set_base_source([this](std::uint32_t ref) {
        if (!host_->actor_states().is_actor(ref)) {
            return std::vector<std::uint32_t>{};
        }
        const std::uint32_t npc = host_->dialogue_npc(ref);
        return npc != 0 ? magic::base_shouts(world_, npc, ref, leveled::player_level())
                        : std::vector<std::uint32_t>{};
    });
    book.set_base_source([this](std::uint32_t ref) {
        if (!host_->actor_states().is_actor(ref)) {
            return std::vector<std::uint32_t>{};
        }
        const std::uint32_t npc = host_->dialogue_npc(ref);
        if (npc == 0) {
            return std::vector<std::uint32_t>{};
        }
        auto spells = magic::base_spells(world_, npc, ref, leveled::player_level());
        for (const std::uint32_t ability : host_->perk_abilities(ref)) {
            if (std::ranges::find(spells, ability) == spells.end()) {
                spells.push_back(ability);
            }
        }
        return spells;
    });
    register_forgets();
}

void MagicRuntime::register_forgets() {
    auto& registry = host_->vm().forgets();
    forgets.push_back(registry.add(
        "magic.effects",
        [this](std::uint32_t ref) {
            // The effects on a gone actor end without being undone; their script objects go with them.
            for (const std::uint32_t id : effects.forget(ref)) {
                host_->vm().detach(id);
                snapshots.erase(id);
            }
        },
        [this] { return effects.entries(); },
        // Casters, targets and summoned refs of the held effects, of the views waiting to be sent and of
        // the snapshots of effects with script objects: values the forget does not scrub.
        [this](std::set<std::uint32_t>& out) {
            effects.held_refs(out);
            for (const auto& p : pending) {
                out.insert({p.view.caster, p.view.target, p.view.summoned});
            }
            for (const auto& [id, view] : snapshots) {
                out.insert({view.caster, view.target, view.summoned});
            }
        }));
    forgets.push_back(registry.add(
        "magic.spell_book", [this](std::uint32_t ref) { book.forget(ref); }, [this] { return book.entries(); }));
    forgets.push_back(registry.add(
        "magic.shouts", [this](std::uint32_t ref) { shouts.forget(ref); }, [this] { return shouts.entries(); }));
    forgets.push_back(registry.add(
        "magic.worn", [this](std::uint32_t ref) { worn.forget(ref); }, [this] { return worn.entries(); }));
    forgets.push_back(registry.add(
        "magic.pending",
        [this](std::uint32_t ref) {
            std::erase_if(pending, [ref](const Pending& p) { return p.view.target == ref; });
        },
        [this] { return pending.size(); }));
    // Keyed by effect instance id (the VM's own store drops the script objects).
    forgets.push_back(registry.add(
        "magic.script_effects",
        [this](std::uint32_t id) {
            snapshots.erase(id);
            detach_later.forget(id);
        },
        [this] { return snapshots.size() + detach_later.size(); }));
}

std::uint32_t MagicRuntime::actor_level_of(const wfb::Npc& npc) {
    return skydot::actor_level(npc);
}

void MagicRuntime::damage_value(std::uint32_t ref, std::uint32_t av, double amount, std::uint32_t caster) {
    if (av == vm::av::k_health && amount > 0.0 && combat_.ghost_ignores_hit(ref)) {
        return;
    }
    host_->actor_states().damage(ref, av, amount);
    if (caster != 0 && av == vm::av::k_health) {
        if (host_->actor_states().get(ref, vm::av::k_health) <= 0.0 && host_->actor_states().killer_of(ref) == 0) {
            host_->actor_states().set_killer(ref, caster);
        }
        if (ref == k_player_ref) {
            combat_.set_last_damager(caster);
        }
    }
}

bool MagicRuntime::conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list,
                                   std::uint32_t subject, std::uint32_t caster) {
    const auto cw = host_->new_condition_world();
    conditions::ConditionContext ctx;
    ctx.subject = subject;
    ctx.target = caster;
    ctx.unknown = conditions::UnknownPolicy::fails;
    return conditions::conditions_pass(list, *cw, ctx, nullptr);
}

double MagicRuntime::game_setting(std::string_view name, double fallback) const {
    return static_cast<double>(WorldData::gmst_float(&world_, name, static_cast<float>(fallback)));
}

std::uint32_t MagicRuntime::actor_level(std::uint32_t ref) const {
    if (ref == k_player_ref) {
        return leveled::player_level();
    }
    if (!host_->actor_states().is_actor(ref)) {
        return 0;
    }
    const auto* npc = resolve_npc(world_, host_->dialogue_npc(ref), wfb::NpcTemplateFlags::use_stats, ref);
    return npc != nullptr ? actor_level_of(*npc) : 0;
}

std::uint32_t MagicRuntime::place_summon(std::uint32_t caster, std::uint32_t npc_base) {
    PlacedRef placed;
    placed.base = npc_base;
    placed.cell = host_->world_access().parent_cell(caster);
    const Xyz position = host_->world_access().ref_position(caster);
    const Xyz rotation = host_->world_access().ref_rotation(caster);
    // PLACEHOLDER: 150 units in front of the caster (rotation z is the heading, radians).
    const double heading = rotation[2];
    placed.position[0] = position[0] + std::sin(heading) * k_summon_distance;
    placed.position[1] = position[1] + std::cos(heading) * k_summon_distance;
    placed.position[2] = position[2];
    for (std::size_t i = 0; i < 3; ++i) {
        placed.rotation[i] = rotation[i];
    }
    placed.scale = 1.0;
    const std::uint32_t ref = host_->place_summon_ref(placed);
    if (ref == 0 || !host_->actor_states().is_actor(ref)) {
        if (ref != 0) {
            host_->forget_ref(ref);
        }
        return 0;
    }
    log("magic: " + magic_hex(npc_base) + " summoned " + magic_hex(ref) + " for " + magic_hex(caster));
    return ref;
}

void MagicRuntime::remove_summon(std::uint32_t ref) {
    log("magic: summon " + magic_hex(ref) + " gone");
    host_->forget_ref(ref);
}

void MagicRuntime::reanimate(std::uint32_t target, std::uint32_t caster) {
    host_->actor_states().revive(target);
    log("magic: " + magic_hex(target) + " reanimated for " + magic_hex(caster));
}

void MagicRuntime::end_reanimate(std::uint32_t target) {
    // PLACEHOLDER: it just dies (UESP: it turns to ash).
    host_->actor_states().damage(target, vm::av::k_health, 1e9);
}

void MagicRuntime::bound_add(std::uint32_t target, std::uint32_t weapon) {
    // PLACEHOLDER: always the right hand (the MGEF names no hand).
    if (items_.equip_item(target, weapon, false, true)) {
        log("magic: " + magic_hex(target) + " bound weapon " + magic_hex(weapon) + " added");
    } else {
        log("magic: " + magic_hex(target) + " cannot equip bound weapon " + magic_hex(weapon));
    }
}

void MagicRuntime::bound_remove(std::uint32_t target, std::uint32_t weapon) {
    (void)items_.unequip_item(target, weapon);
    (void)items_.remove_item(target, weapon, 1);
    log("magic: " + magic_hex(target) + " bound weapon " + magic_hex(weapon) + " removed");
}

std::vector<std::uint32_t> MagicRuntime::actors_near(std::uint32_t ref, double radius) const {
    std::vector<std::uint32_t> out;
    const auto float_position = [this](std::uint32_t actor) {
        const Xyz p = host_->world_access().ref_position(actor);
        return std::array<float, 3>{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
    };
    const auto at = float_position(ref);
    const auto consider = [&](std::uint32_t actor) {
        if (actor == ref || !host_->actor_states().is_actor(actor) || host_->actor_states().is_dead(actor)) {
            return;
        }
        // The distance as the float vector math of the engine rounds it.
        const auto p = float_position(actor);
        const float dx = p[0] - at[0];
        const float dy = p[1] - at[1];
        const float dz = p[2] - at[2];
        if (static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)) <= radius) {
            out.push_back(actor);
        }
    };
    for (const std::uint32_t actor : host_->triggered_actors()) {
        if (actor != k_player_ref) {
            consider(actor);
        }
    }
    consider(k_player_ref);
    return out;
}

magic::MagicSettings MagicRuntime::settings() const {
    magic::MagicSettings s;
    s.max_resistance = game_setting("fPlayerMaxResistance", s.max_resistance);
    s.skill_cost_exponent = game_setting("fMagicPCSkillCostScale", s.skill_cost_exponent);
    s.area_cost_mult = game_setting("fMagicAreaBaseCostMult", s.area_cost_mult);
    return s;
}

std::string MagicRuntime::game_text(std::string_view name, const char* fallback) const {
    const auto* g = WorldData::find_gmst(&world_, name);
    return g != nullptr && g->text_value() != nullptr && g->text_value()->size() > 0 ? g->text_value()->str()
                                                                                     : std::string(fallback);
}

std::string MagicRuntime::word_name(std::uint32_t word) const {
    const auto* w = lookup(world_.words_of_power(), word);
    return w != nullptr && w->name() != nullptr ? magic::word_spelling(w->name()->str()) : magic_hex(word);
}

std::string MagicRuntime::spell_name(std::uint32_t spell) const {
    const auto* s = lookup(world_.spells(), spell);
    return s != nullptr && s->name() != nullptr ? s->name()->str() : magic_hex(spell);
}

void MagicRuntime::prime(std::uint32_t actor) {
    if (host_->actor_states().is_actor(actor)) {
        book.ensure_abilities(actor);
        sync_worn(actor);
    }
}

void MagicRuntime::sync_worn(std::uint32_t actor) {
    if (!host_->actor_states().is_actor(actor)) {
        return;
    }
    if (host_->actor_states().is_dead(actor)) {
        worn.forget(actor);
        return;
    }
    items_.resolve_owner(actor);
    std::vector<magic::WornEnchant> list;
    if (const vm::Equipment* eq = items_.states().find(actor)) {
        const auto add = [&](std::uint32_t form, std::uint32_t extra) {
            const vm::ItemInfo info = items_.item_info_of(form);
            if (!info.known || (info.category != vm::ItemCategory::armor && info.category != vm::ItemCategory::clothing &&
                                info.category != vm::ItemCategory::jewelry)) {
                return;
            }
            if (const std::uint32_t ench = magic::enchantment_of(world_, form, items_.states().extras().find(extra))) {
                list.push_back({form, extra, ench});
            }
        };
        for (const std::uint32_t form : eq->worn) {
            add(form, eq->worn_extra_of(form));
        }
        if (eq->left != 0 && vm::is_shield(items_.item_info_of(eq->left))) {
            add(eq->left, eq->left_extra);
        }
    }
    worn.sync(actor, list);
}

bool MagicRuntime::carried_scroll(std::uint32_t actor, std::uint32_t form) {
    const auto* base = lookup(world_.bases(), form);
    const auto* item = base != nullptr ? base->item() : nullptr;
    if (item == nullptr || item->category() != wfb::ItemCategory::scroll) {
        return false;
    }
    items_.resolve_owner(actor);
    return host_->inventory().count(actor, form) > 0;
}

bool MagicRuntime::item_effects(std::uint32_t form, std::vector<magic::EffectInput>& inputs, magic::SourceKind& kind,
                                bool& poison) {
    kind = magic::SourceKind::potion;
    poison = false;
    const auto* base = lookup(world_.bases(), form);
    const auto* item = base != nullptr ? base->item() : nullptr;
    if (item != nullptr) {
        inputs = magic::inputs_of(item->effects());
        switch (item->category()) {
        case wfb::ItemCategory::ingredient: kind = magic::SourceKind::ingredient; break;
        case wfb::ItemCategory::scroll: kind = magic::SourceKind::scroll; break;
        case wfb::ItemCategory::potion:
            // The pack has no ALCH poison flag (ENIT 0x20000): a potion whose effects are all hostile is one.
            if (!item->food() && !inputs.empty()) {
                poison = std::ranges::all_of(inputs, [&](const magic::EffectInput& in) {
                    const auto* m = lookup(world_.magic_effects(), in.effect);
                    return m != nullptr && (m->flags() & magic::mgef_flag::hostile) != 0;
                });
            }
            break;
        default: break;
        }
        return true;
    }
    if (const auto* created = host_->created_forms().find(form)) {
        poison = created->poison;
        std::int32_t index = 0;
        for (const auto& e : created->effects) {
            inputs.push_back({e.effect, e.magnitude, 0, e.duration, index++});
        }
        return true;
    }
    return false;
}

magic::SourceKind MagicRuntime::kind_of(const wfb::Spell& s) {
    switch (s.type()) {
    case magic::spell_type::ability: return magic::SourceKind::ability;
    case magic::spell_type::poison: return magic::SourceKind::poison;
    default: return s.casting_type() == magic::casting::scroll ? magic::SourceKind::scroll : magic::SourceKind::spell;
    }
}

std::vector<std::uint32_t> MagicRuntime::land(std::uint32_t caster, std::uint32_t blame, std::uint32_t spell,
                                              std::uint32_t target) {
    const auto* s = lookup(world_.spells(), spell);
    if (s == nullptr) {
        return {};
    }
    const std::uint32_t source_actor = blame != 0 ? blame : caster;
    prime(source_actor);
    prime(target);
    const auto inputs = magic::inputs_of(s->effects());
    auto ids = effects.apply(source_actor, target, {kind_of(*s), spell, 0}, inputs);
    return ids;
}

bool MagicRuntime::cast(std::uint32_t spell, std::uint32_t caster, std::uint32_t blame, std::uint32_t target) {
    if (lookup(world_.spells(), spell) == nullptr || caster == 0) {
        return false;
    }
    const std::uint32_t on = target != 0 ? target : caster;
    log("magic: " + magic_hex(blame != 0 ? blame : caster) + " casts " + magic_hex(spell) + " on " + magic_hex(on) + " (script)");
    land(caster, blame, spell, on);
    flush();
    return true;
}

double MagicRuntime::spell_cost(std::uint32_t spell, std::uint32_t caster) {
    const auto* s = lookup(world_.spells(), spell);
    if (s == nullptr) {
        return 0.0;
    }
    if (caster == 0) {
        return static_cast<double>(s->cost());
    }
    return magic::perk_spell_cost(world_, *this, caster, *s, settings());
}

bool MagicRuntime::add_spell(std::uint32_t actor, std::uint32_t spell, bool verbose) {
    if (!host_->actor_states().is_actor(actor)) {
        return false;
    }
    prime(actor);
    const bool added = book.add_spell(actor, spell);
    if (added && verbose && actor == k_player_ref) {
        message(spell_name(spell) + " added");
    }
    flush();
    return added;
}

bool MagicRuntime::remove_spell(std::uint32_t actor, std::uint32_t spell) {
    prime(actor);
    const bool removed = book.remove_spell(actor, spell);
    flush();
    return removed;
}

bool MagicRuntime::dispel_spell(std::uint32_t actor, std::uint32_t spell) {
    const bool any = effects.dispel(actor, spell) != 0;
    flush();
    return any;
}

void MagicRuntime::dispel_all_spells(std::uint32_t actor) {
    effects.dispel_all(actor);
    flush();
}

bool MagicRuntime::has_magic_effect(std::uint32_t actor, std::uint32_t mgef) {
    prime(actor);
    return effects.has_effect(actor, mgef);
}

bool MagicRuntime::has_magic_effect_keyword(std::uint32_t actor, std::uint32_t keyword) {
    prime(actor);
    return effects.has_effect_keyword(actor, keyword);
}

bool MagicRuntime::equip_spell(std::uint32_t actor, std::uint32_t spell, int hand) {
    if (hand == 2) {
        const bool changed = shouts.equipped(actor) != spell && book.equipped(actor, magic::Slot::power) != spell;
        const bool done = magic::equip_voice(book, shouts, actor, spell); // the voice slot: a power or a shout
        if (done && changed) {
            items_.send_equip_event(actor, spell, true);
        }
        return done;
    }
    const bool scroll = !book.has_spell(actor, spell) && carried_scroll(actor, spell);
    const bool changed = equipped_spell(actor, hand) != spell;
    const bool done = book.equip(actor, spell, slot_of(hand), scroll);
    if (done && changed) {
        items_.send_equip_event(actor, spell, true);
    }
    return done;
}

bool MagicRuntime::teach_word(std::uint32_t word) {
    const bool learned = shouts.teach_word(k_player_ref, word);
    if (learned) {
        message(game_text("sShoutAdded", "Word of Power Learned") + ": " + word_name(word));
    }
    return learned;
}

bool MagicRuntime::equip_shout(std::uint32_t actor, std::uint32_t shout) {
    if (!shouts.is_shout(shout)) {
        return false;
    }
    const bool changed = shouts.equipped(actor) != shout;
    const bool done = magic::equip_voice(book, shouts, actor, shout);
    if (done && changed) {
        items_.send_equip_event(actor, shout, true);
    }
    return done;
}

void MagicRuntime::unequip_shout(std::uint32_t actor, std::uint32_t shout) {
    if (shout == 0 || shouts.equipped(actor) == shout) {
        shouts.unequip(actor);
    }
}

std::optional<MagicApi::Effect> MagicRuntime::effect(std::uint32_t handle) {
    if (const auto live = effects.instance(handle)) {
        return Effect{live->mgef, live->target, live->caster, live->magnitude, live->duration, live->elapsed};
    }
    const auto snap = snapshots.find(handle);
    if (snap == snapshots.end()) {
        return std::nullopt;
    }
    const auto& v = snap->second;
    return Effect{v.mgef, v.target, v.caster, v.magnitude, v.duration, v.elapsed};
}

void MagicRuntime::dispel_effect(std::uint32_t handle) {
    effects.dispel_instance(handle);
    flush();
}

void MagicRuntime::flush() {
    if (flushing) {
        return;
    }
    flushing = true;
    while (!pending.empty()) {
        auto batch = std::move(pending);
        pending.clear();
        for (const auto& event : batch) {
            const auto& v = event.view;
            host_->effect_event(v, event.started);
            if (const std::string word = held_state_word(v); !word.empty()) {
                log("magic: " + magic_hex(v.target) + " " + word + (event.started ? " starts" : " ends"));
            }
            if (event.started && v.archetype == magic::archetype::stagger) {
                log("magic: " + magic_hex(v.target) + " staggered (" + magic_number(v.magnitude) + ")");
            }
            const auto* mgef = lookup(world_.magic_effects(), v.mgef);
            const auto* scripts = mgef != nullptr ? mgef->scripts() : nullptr;
            if (scripts == nullptr || scripts->size() == 0) {
                continue;
            }
            // The script object of this effect: its handle is the instance id.
            snapshots[v.id] = v;
            auto& vm = host_->vm();
            const Value target = vm.object(v.target, "Actor");
            const Value caster = vm.object(v.caster, "Actor");
            if (event.started) {
                host_->attach_effect_scripts(v.id, v.mgef);
                vm.send_event(v.id, "OnEffectStart", {target, caster});
            } else {
                vm.send_event(v.id, "OnEffectFinish", {target, caster});
                detach_later.add(v.id);
            }
        }
    }
    flushing = false;
}

void MagicRuntime::tick(double seconds) {
    auto& vm = host_->vm();
    effects.tick(seconds);
    for (const std::uint32_t id : effects.take_forgotten()) { // forgotten while the effects ran
        vm.detach(id);
        snapshots.erase(id);
    }
    shouts.tick(seconds); // real seconds (docs/adr/0008)
    flush();
    // Script objects of finished effects go once their OnEffectFinish had its updates (VM seconds).
    for (const std::uint32_t id : detach_later.tick(seconds)) {
        vm.detach(id);
        snapshots.erase(id);
    }
}

bool MagicRuntime::soul_trapped_by_player(std::uint32_t ref) const {
    if (ref == k_player_ref) {
        return false;
    }
    const auto views = effects.effects_on(ref);
    // The spell's own effect is a Script archetype with Papyrus scripts (SoulTrapFFActor); the
    // enchantments' and perks' are archetype 23. Either, cast by the player, is a Soul Trap.
    const auto soul_trap = [&](const magic::EffectView& v) {
        if (v.caster != k_player_ref) {
            return false;
        }
        if (v.archetype == k_archetype_soul_trap) {
            return true;
        }
        const auto* mgef = lookup(world_.magic_effects(), v.mgef);
        return mgef != nullptr && mgef->editor_id() != nullptr && mgef->editor_id()->str().starts_with("SoulTrap");
    };
    return std::ranges::any_of(views, soul_trap);
}

void MagicRuntime::actor_died(std::uint32_t ref) {
    effects.on_death(ref);
    worn.forget(ref);
    flush();
}

std::int64_t MagicRuntime::break_invisibility(std::uint32_t ref) {
    const auto n = effects.break_invisibility(ref);
    if (n != 0) {
        log("magic: invisibility of " + magic_hex(ref) + " broken");
    }
    flush();
    return static_cast<std::int64_t>(n);
}

bool MagicRuntime::dispel_bound_weapon(std::uint32_t actor, std::uint32_t weapon) {
    const bool any = effects.dispel_bound(actor, weapon) != 0;
    flush();
    return any;
}

void MagicRuntime::clear() {
    effects.clear();
    book.clear();
    shouts.clear();
    worn.clear();
    pending.clear();
    snapshots.clear();
    detach_later.clear();
}

std::vector<std::uint8_t> MagicRuntime::save() const { return magic::write_magc(effects, book, shouts); }

bool MagicRuntime::decode(const wfb::World& world, save::ByteReader r, std::vector<std::uint8_t>& effects,
                          std::vector<std::uint8_t>& book, std::vector<std::uint8_t>& shouts) {
    magic::MagcParts parts;
    if (!magic::read_magc(r, parts) || !magic::check_magc(world, parts)) {
        return false;
    }
    effects = std::move(parts.effects);
    book = std::move(parts.book);
    shouts = std::move(parts.shouts);
    return true;
}

void MagicRuntime::adopt(bool has, const std::vector<std::uint8_t>& saved_effects,
                         const std::vector<std::uint8_t>& saved_book, const std::vector<std::uint8_t>& saved_shouts) {
    clear();
    if (has) {
        // The actor values were replaced without their temporary part: the effects hold it again.
        (void)magic::SpellBook::load(save::ByteReader(saved_book), book);
        // A version 1 section has no shout part: the book stays empty.
        if (!saved_shouts.empty()) {
            (void)magic::ShoutBook::load(save::ByteReader(saved_shouts), shouts);
        }
        (void)effects.load(save::ByteReader(saved_effects));
    }
    // The VM was restored first: script objects of effects that are over (finished before the save, or not in the
    // loaded effects) have nothing to belong to; they get their grace for a handler under way, then go.
    for (const std::uint32_t form : host_->vm().attached_forms()) {
        if (id_ranges::effect_instances.contains(form) && !effects.instance(form)) {
            detach_later.add(form);
        }
    }
    // Abilities are not saved: start them again for whoever is known.
    book.reset_abilities();
    std::vector<std::uint32_t> wearers;
    for (const auto& [owner, equipment] : items_.states().equipments()) {
        if (!equipment.worn.empty() || equipment.left != 0) {
            wearers.push_back(owner);
        }
    }
    const auto refs = magic::actors_to_rederive(book, effects, wearers);
    for (const auto ref : refs) {
        prime(ref);
    }
}

void MagicRuntime::sync_worn_magic(std::uint32_t owner) {
    sync_worn(owner);
    flush();
}

void MagicRuntime::move_unit_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to) {
    if (from == to || owner == 0) {
        return;
    }
    auto& inventory = host_->inventory();
    if (inventory.remove_extra(owner, form, from, 1) > 0) {
        inventory.add(owner, form, 1, to);
    }
    retarget_equipment(owner, form, from, to);
}

void MagicRuntime::retarget_equipment(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to) {
    if (from == to || owner == 0 || items_.states().find(owner) == nullptr) {
        return;
    }
    auto& equipment = items_.states().equipment(owner);
    if (equipment.right == form && equipment.right_extra == from) {
        equipment.right_extra = to;
    } else if (equipment.left == form && equipment.left_extra == from) {
        equipment.left_extra = to;
    } else if (equipment.ammo == form && equipment.ammo_extra == from) {
        equipment.ammo_extra = to;
    } else if (equipment.worn.contains(form) && equipment.worn_extra_of(form) == from) {
        if (to == 0) {
            equipment.worn_extra.erase(form);
        } else {
            equipment.worn_extra[form] = to;
        }
    }
}

MagicRuntime::HitMagic MagicRuntime::hit_magic(std::uint32_t attacker, std::uint32_t target, std::uint32_t weapon,
                                               std::uint32_t extra, bool target_alive) {
    HitMagic out;
    if (weapon == 0) {
        return out;
    }
    prime(attacker);
    prime(target);
    magic::HitMagicInput in;
    in.attacker = attacker;
    in.target = target;
    in.weapon = weapon;
    in.extra = extra;
    in.attacker_is_player = attacker == k_player_ref;
    in.target_alive = target_alive;
    in.extra_unique = extra != 0 && host_->inventory().units_with_extra(extra) == 1 && !items_.states().extra_dropped(extra);
    const auto poison_inputs = [this](std::uint32_t poison, std::vector<magic::EffectInput>& inputs) {
        magic::SourceKind kind = magic::SourceKind::potion;
        bool is_poison = false;
        return item_effects(poison, inputs, kind, is_poison);
    };
    const auto result = magic::hit_magic(world_, effects, items_.states().extras(), in, poison_inputs, settings(),
                                         &host_->created_forms());
    if (result.new_extra != extra) {
        move_unit_extra(attacker, weapon, extra, result.new_extra);
    }
    flush();
    out.poison = result.poison;
    out.enchantment = result.enchantment;
    out.fired = result.fired;
    out.charge = static_cast<double>(result.charge);
    out.charge_max = static_cast<double>(result.max);
    if (result.enchantment != 0 && attacker == k_player_ref) {
        log("magic: " + magic_hex(weapon) + " enchantment " + magic_hex(result.enchantment) +
            (result.fired ? " fires" : " has no charge") + " (charge " + magic_number(static_cast<double>(result.charge)) +
            "/" + magic_number(static_cast<double>(result.max)) + ")");
    }
    return out;
}

MagicRuntime::PoisonResult MagicRuntime::poison_weapon(std::uint32_t actor, std::uint32_t form, std::int64_t extra) {
    PoisonResult out;
    if (!host_->actor_states().is_actor(actor)) {
        out.reason = "no actor";
        return out;
    }
    std::vector<magic::EffectInput> inputs;
    magic::SourceKind kind = magic::SourceKind::potion;
    bool poison = false;
    if (!item_effects(form, inputs, kind, poison) || !poison) {
        out.reason = "not a poison";
        return out;
    }
    items_.resolve_owner(actor);
    const std::uint32_t poison_extra = items_.resolve_extra(actor, form, extra);
    if (host_->inventory().count_extra(actor, form, poison_extra) <= 0) {
        out.reason = "not carried";
        return out;
    }
    const Equipment* equipment = items_.states().find(actor);
    const std::uint32_t weapon = equipment != nullptr ? equipment->right : 0;
    const ItemInfo info = items_.item_info_of(weapon);
    // UESP: a poison goes on the weapon in the right hand, a bow included; a staff or fists have no blade.
    if (weapon == 0 || !info.known || info.category != ItemCategory::weapon || info.weapon_type == 0 ||
        info.weapon_type == 8) {
        out.reason = "no weapon";
        return out;
    }
    const std::uint32_t weapon_extra = equipment->right_extra;
    ItemExtra changed;
    if (const auto* now = items_.states().extras().find(weapon_extra)) {
        changed = *now;
    }
    if (!magic::can_poison(changed)) {
        out.reason = "already poisoned";
        return out;
    }
    (void)items_.remove_item_extra(actor, form, poison_extra, 1);
    // Mod Poison Dose Count (83): Concentrated Poison sets 2 (subjects: the owner, the weapon, the poison).
    const auto doses = static_cast<std::uint32_t>(std::max<long>(
        1, std::lround(host_->perk_entry(magic::perk_ep::mod_poison_dose_count, actor, {actor, weapon, form},
                                         static_cast<double>(magic::k_base_poison_doses)))));
    const std::uint32_t after = items_.states().extras().intern(magic::poisoned(changed, form, doses));
    move_unit_extra(actor, weapon, weapon_extra, after);
    log("magic: poison " + magic_hex(form) + " put on " + magic_hex(weapon) + " of " + magic_hex(actor));
    out.ok = true;
    out.weapon = weapon;
    out.doses = static_cast<int>(doses);
    return out;
}

MagicRuntime::WeaponCharge MagicRuntime::weapon_charge(std::uint32_t actor, std::uint32_t form, std::int64_t extra) {
    items_.resolve_owner(actor);
    std::uint32_t unit = 0;
    const Equipment* equipment = items_.states().find(actor);
    if (extra >= 0) {
        unit = static_cast<std::uint32_t>(extra);
    } else if (equipment != nullptr && equipment->right == form) {
        unit = equipment->right_extra;
    } else {
        unit = items_.resolve_extra(actor, form, -1);
    }
    const auto info = magic::weapon_charge(world_, form, items_.states().extras().find(unit), settings(),
                                           &host_->created_forms());
    WeaponCharge out;
    out.enchantment = info.enchantment;
    out.charge = static_cast<double>(info.charge);
    out.max = static_cast<double>(info.max);
    out.cost = info.cost;
    return out;
}

void MagicRuntime::bind_natives(Vm& vm) {
    bind_spell_natives(vm, world_, *this);
    bind_magiceffect_natives(vm, world_);
    bind_activemagiceffect_natives(vm, *this);
    bind_actor_magic_natives(vm, *this);
    bind_shout_natives(vm, world_);
}

} // namespace skydot::vm
