// SPDX-License-Identifier: GPL-3.0-or-later
#include "magic/active_effects.hpp"

#include "data/fb_search.hpp"
#include "vm/actor_values.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace skydot::magic {

namespace {

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

} // namespace

std::vector<EffectInput> inputs_of(const flatbuffers::Vector<flatbuffers::Offset<wfb::EffectItem>>* effects) {
    std::vector<EffectInput> out;
    if (effects == nullptr) {
        return out;
    }
    out.reserve(effects->size());
    std::int32_t index = 0;
    for (const auto* e : *effects) {
        out.push_back({e->effect(), e->magnitude(), e->area(), e->duration(), index++});
    }
    return out;
}

MagicSettings ActiveEffects::settings() const {
    MagicSettings s;
    s.max_resistance = host_.game_setting("fPlayerMaxResistance", s.max_resistance);
    s.skill_cost_exponent = host_.game_setting("fMagicPCSkillCostScale", s.skill_cost_exponent);
    s.area_cost_mult = host_.game_setting("fMagicAreaBaseCostMult", s.area_cost_mult);
    return s;
}

bool ActiveEffects::handled(std::uint32_t a) const {
    return a == archetype::value_modifier || a == archetype::peak_value_modifier ||
           a == archetype::dual_value_modifier || a == archetype::absorb || a == archetype::script ||
           a == archetype::calm || a == archetype::demoralize || a == archetype::frenzy ||
           a == archetype::turn_undead || a == archetype::reanimate || a == archetype::stagger ||
           a == archetype::paralysis || a == archetype::invisibility || a == archetype::light ||
           a == archetype::cloak || a == archetype::bound_weapon || a == archetype::summon_creature ||
           a == archetype::slow_time;
}

bool ActiveEffects::holds(const Instance& i) const {
    const bool value_kind = i.archetype == archetype::value_modifier || i.archetype == archetype::peak_value_modifier ||
                            i.archetype == archetype::dual_value_modifier;
    return value_kind && !instant(i) && (i.permanent || (i.flags & mgef_flag::recover) != 0);
}

EffectView ActiveEffects::view(const Instance& i) const {
    EffectView v;
    v.id = i.id;
    v.mgef = i.mgef;
    v.archetype = i.archetype;
    v.source = i.source;
    v.caster = i.caster;
    v.target = i.target;
    v.magnitude = i.magnitude;
    v.duration = i.duration;
    v.elapsed = i.elapsed;
    v.area = i.area;
    v.active = i.active;
    v.permanent = i.permanent;
    v.flags = i.flags;
    v.associated = i.associated;
    v.summoned = i.summoned;
    return v;
}

bool ActiveEffects::fill_from_mgef(Instance& i) const {
    const auto* m = lookup(world_.magic_effects(), i.mgef);
    if (m == nullptr) {
        return false;
    }
    i.archetype = m->archetype();
    i.flags = m->flags();
    i.associated = m->associated_item();
    i.actor_value = m->actor_value();
    i.second_actor_value = m->second_actor_value();
    i.second_weight = static_cast<double>(m->second_av_weight());
    return true;
}

const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* ActiveEffects::entry_conditions(
    const Instance& i) const {
    if (i.index < 0) {
        return nullptr;
    }
    const flatbuffers::Vector<flatbuffers::Offset<wfb::EffectItem>>* effects = nullptr;
    if (const auto* s = lookup(world_.spells(), i.source.form)) {
        effects = s->effects();
    } else if (const auto* e = lookup(world_.enchantments(), i.source.form)) {
        effects = e->effects();
    } else if (const auto* b = lookup(world_.bases(), i.source.form); b != nullptr && b->item() != nullptr) {
        effects = b->item()->effects();
    }
    if (effects == nullptr || static_cast<std::uint32_t>(i.index) >= effects->size()) {
        return nullptr;
    }
    return effects->Get(static_cast<flatbuffers::uoffset_t>(i.index))->conditions();
}

bool ActiveEffects::conditions_ok(const Instance& i) const {
    const auto* own = entry_conditions(i);
    if (own != nullptr && own->size() != 0 && !host_.conditions_pass(own, i.target, i.caster)) {
        return false;
    }
    const auto* m = lookup(world_.magic_effects(), i.mgef);
    const auto* general = m != nullptr ? m->conditions() : nullptr;
    return general == nullptr || general->size() == 0 || host_.conditions_pass(general, i.target, i.caster);
}

void ActiveEffects::log_start(const Instance& i) {
    host_.log("magic: effect " + hex(i.mgef) + " on " + hex(i.target) + " start mag " + number(i.magnitude) +
              " dur " + (i.permanent ? std::string("constant") : number(i.duration)));
}

void ActiveEffects::apply_to(std::uint32_t caster, std::uint32_t target, std::uint32_t av, double amount, bool takes) {
    if (amount <= 0.0) {
        return;
    }
    if (takes) {
        host_.damage_value(target, av, amount, caster);
    } else {
        host_.restore_value(target, av, amount);
    }
}

void ActiveEffects::act_once(const Instance& i) {
    // Instant: the whole magnitude at once.
    act_over(i, 1.0);
}

void ActiveEffects::act_over(const Instance& i, double seconds) {
    const bool takes = detrimental(i);
    const double amount = i.magnitude * seconds;
    switch (i.archetype) {
    case archetype::value_modifier:
    case archetype::peak_value_modifier:
        if (i.actor_value >= 0) {
            apply_to(i.caster, i.target, static_cast<std::uint32_t>(i.actor_value), amount, takes);
        }
        break;
    case archetype::dual_value_modifier:
        if (i.actor_value >= 0) {
            apply_to(i.caster, i.target, static_cast<std::uint32_t>(i.actor_value), amount, takes);
        }
        if (i.second_actor_value >= 0) {
            apply_to(i.caster, i.target, static_cast<std::uint32_t>(i.second_actor_value), amount * i.second_weight, takes);
        }
        break;
    case archetype::absorb:
        if (i.actor_value >= 0) {
            const auto av = static_cast<std::uint32_t>(i.actor_value);
            const double taken = std::min(amount, std::max(0.0, host_.actor_value(i.target, av)));
            if (taken > 0.0) {
                host_.damage_value(i.target, av, taken, i.caster);
                if (i.caster != 0) {
                    host_.restore_value(i.caster, av, taken);
                }
            }
        }
        break;
    default:
        break; // script: events only; the rest is not done
    }
}

double slow_time_scale(double magnitude) noexcept {
    // PLACEHOLDER (UESP gives no formula): 0 < m < 1 is the scale itself, otherwise m is a percent slowed.
    const double scale = magnitude > 0.0 && magnitude < 1.0 ? magnitude : 1.0 - magnitude / 100.0;
    return std::clamp(scale, 0.1, 1.0);
}

bool ActiveEffects::begin(Instance& i) {
    if (i.archetype == archetype::summon_creature) {
        // One summon per caster (no Twin Souls yet): the earlier one goes once the loops are done.
        for (const auto& [id, other] : instances_) {
            if (id != i.id && other.archetype == archetype::summon_creature && other.caster == i.caster &&
                other.started) {
                dispel_later_.push_back(id);
            }
        }
        i.summoned = i.associated != 0 ? host_.place_summon(i.caster, i.associated) : 0;
        if (i.summoned == 0) {
            host_.log("magic: summon of " + hex(i.associated) + " for " + hex(i.caster) + " failed");
            return false;
        }
    } else if (i.archetype == archetype::reanimate) {
        host_.reanimate(i.target, i.caster);
    } else if (i.archetype == archetype::bound_weapon && i.associated != 0) {
        host_.bound_add(i.target, i.associated);
    }
    i.started = true;
    if (!handled(i.archetype)) {
        if (unhandled_[i.archetype]++ == 0 && logged_.insert(i.archetype).second) {
            host_.log("magic: archetype " + std::to_string(i.archetype) + " (" + archetype_name(i.archetype) +
                      ") not done");
        }
    }
    log_start(i);
    host_.effect_changed(true, view(i));
    if (instant(i)) {
        act_once(i);
    }
    return true;
}

void ActiveEffects::flush_later() {
    auto later = std::move(dispel_later_);
    dispel_later_.clear();
    for (const auto id : later) {
        finish(id);
    }
}

void ActiveEffects::pulse_cloak(const Instance& i) {
    const auto* spell = lookup(world_.spells(), i.associated);
    if (spell == nullptr) {
        return;
    }
    // PLACEHOLDER: an area of 0 is 10 feet.
    const double radius = i.area > 0.0 ? i.area : area_units(10.0);
    const auto inputs = inputs_of(spell->effects());
    for (const std::uint32_t actor : host_.actors_near(i.target, radius)) {
        if (actor == i.target || host_.is_dead(actor)) {
            continue;
        }
        apply(i.target, actor, {SourceKind::spell, i.associated, 0}, inputs);
    }
}

Overrides ActiveEffects::overrides(std::uint32_t ref) const {
    Overrides o;
    std::uint32_t fear_id = 0;
    for (const auto& [id, i] : instances_) {
        if (!i.active || !i.started) {
            continue;
        }
        if (i.archetype == archetype::summon_creature && i.summoned == ref && ref != 0) {
            o.commanded_by = i.caster;
        }
        if (i.target != ref) {
            continue;
        }
        switch (i.archetype) {
        case archetype::calm:
            o.calm = true;
            break;
        case archetype::frenzy:
            o.frenzy = true;
            break;
        case archetype::paralysis:
            o.paralyzed = true;
            break;
        case archetype::invisibility:
            o.invisible = true;
            break;
        case archetype::demoralize:
        case archetype::turn_undead:
            if (id > fear_id) { // the newest
                fear_id = id;
                o.fear_from = i.caster;
            }
            break;
        case archetype::reanimate:
            o.commanded_by = i.caster;
            break;
        case archetype::slow_time:
            o.time_scale = std::min(o.time_scale, slow_time_scale(i.magnitude));
            break;
        default:
            break;
        }
    }
    return o;
}

std::size_t ActiveEffects::break_invisibility(std::uint32_t ref) {
    std::vector<std::uint32_t> ids;
    for (const auto& [id, i] : instances_) {
        if (i.target == ref && i.archetype == archetype::invisibility) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        finish(id);
    }
    return ids.size();
}

std::size_t ActiveEffects::dispel_bound(std::uint32_t target, std::uint32_t weapon) {
    std::vector<std::uint32_t> ids;
    for (const auto& [id, i] : instances_) {
        if (i.target == target && i.archetype == archetype::bound_weapon && i.associated == weapon) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        finish(id);
    }
    return ids.size();
}

void ActiveEffects::sync_holds(std::uint32_t target) {
    struct Want {
        Instance* inst;
        std::uint32_t av;
        double amount;
        bool peak;
    };
    std::vector<Want> wants;
    for (auto& [id, i] : instances_) {
        if (i.target != target || !i.active || !i.started || !holds(i)) {
            continue;
        }
        const double sign = detrimental(i) ? -1.0 : 1.0;
        const bool peak = i.archetype == archetype::peak_value_modifier;
        if (i.actor_value >= 0) {
            wants.push_back({&i, static_cast<std::uint32_t>(i.actor_value), sign * i.magnitude, peak});
        }
        if (i.archetype == archetype::dual_value_modifier && i.second_actor_value >= 0) {
            wants.push_back({&i, static_cast<std::uint32_t>(i.second_actor_value),
                             sign * i.magnitude * i.second_weight, false});
        }
    }
    // Among peak modifiers of one actor value and sign only the largest counts (the oldest on a tie).
    std::map<std::pair<std::uint32_t, int>, std::size_t> winner;
    for (std::size_t k = 0; k < wants.size(); ++k) {
        if (!wants[k].peak) {
            continue;
        }
        const auto key = std::make_pair(wants[k].av, wants[k].amount < 0.0 ? -1 : 1);
        const auto it = winner.find(key);
        if (it == winner.end() || std::abs(wants[k].amount) > std::abs(wants[it->second].amount)) {
            winner[key] = k;
        }
    }
    std::map<Instance*, std::map<std::uint32_t, double>> desired;
    for (std::size_t k = 0; k < wants.size(); ++k) {
        if (wants[k].peak) {
            const auto key = std::make_pair(wants[k].av, wants[k].amount < 0.0 ? -1 : 1);
            if (winner[key] != k) {
                continue;
            }
        }
        desired[wants[k].inst][wants[k].av] += wants[k].amount;
    }
    for (auto& [id, i] : instances_) {
        if (i.target != target) {
            continue;
        }
        std::map<std::uint32_t, double> want;
        if (const auto it = desired.find(&i); it != desired.end()) {
            want = it->second;
        }
        std::set<std::uint32_t> avs;
        for (const auto& [av, amount] : want) {
            avs.insert(av);
        }
        for (const auto& [av, amount] : i.held) {
            avs.insert(av);
        }
        for (const std::uint32_t av : avs) {
            const double d = want.contains(av) ? want[av] : 0.0;
            const double h = i.held.contains(av) ? i.held[av] : 0.0;
            if (std::abs(d - h) > 1e-9) {
                host_.modify_temp(target, av, d - h);
            }
            if (std::abs(d) > 1e-9) {
                i.held[av] = d;
            } else {
                i.held.erase(av);
            }
        }
    }
}

void ActiveEffects::finish(std::uint32_t id) {
    const auto it = instances_.find(id);
    if (it == instances_.end()) {
        return;
    }
    Instance inst = std::move(it->second);
    const EffectView v = view(inst);
    instances_.erase(it);
    for (const auto& [av, amount] : inst.held) {
        host_.modify_temp(inst.target, av, -amount);
    }
    sync_holds(inst.target);
    if (inst.started) {
        if (inst.archetype == archetype::summon_creature && inst.summoned != 0) {
            host_.remove_summon(inst.summoned);
        } else if (inst.archetype == archetype::reanimate) {
            if (!host_.is_dead(inst.target)) {
                host_.end_reanimate(inst.target);
            }
        } else if (inst.archetype == archetype::bound_weapon && inst.associated != 0) {
            host_.bound_remove(inst.target, inst.associated);
        }
        host_.log("magic: effect " + hex(inst.mgef) + " on " + hex(inst.target) + " end");
        host_.effect_changed(false, v);
    }
}

std::vector<std::uint32_t> ActiveEffects::apply(std::uint32_t caster, std::uint32_t target, const Source& source,
                                                 std::span<const EffectInput> effects,
                                                 const ApplyOptions& options) {
    std::vector<std::uint32_t> made;
    if (options.dual_cast && !dual_logged_) {
        dual_logged_ = true;
        host_.log("magic: dual casting is a placeholder (x1) until perks exist");
    }
    const MagicSettings s = settings();
    for (const EffectInput& in : effects) {
        const auto* m = lookup(world_.magic_effects(), in.effect);
        if (m == nullptr) {
            continue;
        }
        double magnitude = static_cast<double>(in.magnitude) * options.scale *
                           (options.dual_cast ? k_placeholder_dual_cast_scale : 1.0);
        if ((m->flags() & mgef_flag::hostile) != 0) {
            if (m->resist_value() >= 0) {
                magnitude = resisted_magnitude(magnitude, host_.actor_value(target, static_cast<std::uint32_t>(m->resist_value())), s);
            }
            magnitude = resisted_magnitude(magnitude, host_.actor_value(target, vm::av::k_resist_magic), s);
        }
        if (const std::uint32_t a = m->archetype(); a == archetype::calm || a == archetype::demoralize ||
                                                    a == archetype::frenzy || a == archetype::turn_undead ||
                                                    a == archetype::reanimate) {
            // The magnitude is the highest level affected.
            const std::uint32_t level = host_.actor_level(target);
            if (static_cast<double>(level) > magnitude) {
                if (gate_logged_.emplace(target, in.effect).second) {
                    host_.log("magic: " + hex(target) + " is too strong for " + (m->editor_id() != nullptr ? std::string(m->editor_id()->c_str()) : hex(in.effect)) +
                              " (level " + std::to_string(level) + " > " + number(magnitude) + ")");
                }
                continue;
            }
            if (a == archetype::reanimate && !host_.is_dead(target)) {
                continue; // only the dead rise
            }
        }
        const bool constant = options.constant || source.kind == SourceKind::ability;
        // The same caster, target, source and entry: refresh.
        Instance* existing = nullptr;
        for (auto& [id, i] : instances_) {
            if (i.archetype != archetype::summon_creature && i.mgef == in.effect && i.caster == caster && i.target == target && i.source == source &&
                i.index == in.index) {
                existing = &i;
                break;
            }
        }
        if (existing != nullptr) {
            existing->magnitude = magnitude;
            existing->duration = in.duration;
            existing->elapsed = 0.0;
            existing->area = area_units(in.area);
            if (const bool now = conditions_ok(*existing); now != existing->active) {
                existing->active = now;
                if (now && !existing->started && !begin(*existing)) {
                    const std::uint32_t gone = existing->id;
                    instances_.erase(gone);
                    flush_later();
                    continue;
                }
            }
            sync_holds(target);
            made.push_back(existing->id);
            continue;
        }
        const std::uint32_t id = ids_.allocate();
        if (id == 0) {
            host_.log("magic: out of effect instance ids");
            break;
        }
        Instance inst;
        inst.id = id;
        inst.mgef = in.effect;
        inst.source = source;
        inst.index = in.index;
        inst.caster = caster;
        inst.target = target;
        inst.magnitude = magnitude;
        inst.duration = in.duration;
        inst.area = area_units(in.area);
        inst.permanent = constant;
        (void)fill_from_mgef(inst);
        inst.active = conditions_ok(inst);
        const bool is_instant = instant(inst);
        auto& stored = instances_.emplace(id, std::move(inst)).first->second;
        if (stored.active) {
            if (!begin(stored)) {
                instances_.erase(id);
                flush_later();
                continue;
            }
            if (is_instant) {
                finish(id);
                made.push_back(id);
                continue;
            }
            sync_holds(target);
        } else if (is_instant) {
            instances_.erase(id); // it never acted
            continue;
        }
        made.push_back(id);
    }
    flush_later();
    return made;
}

void ActiveEffects::tick(double seconds) {
    if (seconds <= 0.0) {
        return;
    }
    std::vector<std::uint32_t> ended;
    std::vector<std::uint32_t> pulses;
    std::set<std::uint32_t> dirty;
    for (auto& [id, i] : instances_) {
        if (i.archetype == archetype::summon_creature && i.started && i.summoned != 0 && host_.is_dead(i.summoned)) {
            ended.push_back(id); // the summon died: the effect ends
            continue;
        }
        if (host_.is_dead(i.target)) {
            continue;
        }
        i.since_check += seconds;
        if (i.since_check >= k_condition_interval) {
            i.since_check = std::fmod(i.since_check, k_condition_interval);
            const bool now = conditions_ok(i);
            if (now != i.active) {
                i.active = now;
                dirty.insert(i.target);
                if (now && !i.started && !begin(i)) {
                    ended.push_back(id);
                    continue;
                }
            }
            if (i.archetype == archetype::cloak && i.active && i.started) {
                pulses.push_back(id);
            }
        }
        if (i.active && i.started && !i.permanent && !holds(i)) {
            const double left = std::max(0.0, i.duration - i.elapsed);
            act_over(i, std::min(seconds, left));
        }
        i.elapsed += seconds;
        if (!i.permanent && i.elapsed >= i.duration) {
            ended.push_back(id);
        }
    }
    for (const std::uint32_t t : dirty) {
        sync_holds(t);
    }
    for (const std::uint32_t id : pulses) {
        if (const auto it = instances_.find(id); it != instances_.end()) {
            pulse_cloak(it->second);
        }
    }
    flush_later();
    for (const std::uint32_t id : ended) {
        finish(id);
    }
}

std::size_t ActiveEffects::dispel(std::uint32_t target, std::uint32_t source_form) {
    std::vector<std::uint32_t> ids;
    for (const auto& [id, i] : instances_) {
        if (i.target == target && i.source.form == source_form) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        finish(id);
    }
    return ids.size();
}

std::size_t ActiveEffects::dispel_all(std::uint32_t target) {
    std::vector<std::uint32_t> ids;
    for (const auto& [id, i] : instances_) {
        if (i.target == target) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        finish(id);
    }
    return ids.size();
}

bool ActiveEffects::dispel_instance(std::uint32_t id) {
    if (!instances_.contains(id)) {
        return false;
    }
    finish(id);
    return true;
}

void ActiveEffects::on_death(std::uint32_t target) {
    std::vector<std::uint32_t> ids;
    for (const auto& [id, i] : instances_) {
        // A reanimated actor lives while its effect runs: only its second death ends it.
        const bool risen = i.archetype == archetype::reanimate && !host_.is_dead(target);
        if (i.target == target && !risen && (i.flags & mgef_flag::no_death_dispel) == 0) {
            ids.push_back(id);
        }
    }
    for (const auto id : ids) {
        finish(id);
    }
}

bool ActiveEffects::has_effect(std::uint32_t target, std::uint32_t mgef) const {
    return std::ranges::any_of(instances_, [&](const auto& p) {
        return p.second.target == target && p.second.mgef == mgef && p.second.active;
    });
}

bool ActiveEffects::has_effect_keyword(std::uint32_t target, std::uint32_t keyword) const {
    for (const auto& [id, i] : instances_) {
        if (i.target != target || !i.active) {
            continue;
        }
        const auto* m = lookup(world_.magic_effects(), i.mgef);
        if (m != nullptr && m->keywords() != nullptr &&
            std::ranges::any_of(*m->keywords(), [&](std::uint32_t k) { return k == keyword; })) {
            return true;
        }
    }
    return false;
}

std::vector<EffectView> ActiveEffects::effects_on(std::uint32_t target) const {
    std::vector<EffectView> out;
    for (const auto& [id, i] : instances_) {
        if (i.target == target) {
            out.push_back(view(i));
        }
    }
    return out;
}

std::set<std::uint32_t> ActiveEffects::targets() const {
    std::set<std::uint32_t> out;
    for (const auto& [id, i] : instances_) {
        out.insert(i.target);
    }
    return out;
}

std::optional<EffectView> ActiveEffects::instance(std::uint32_t id) const {
    const auto it = instances_.find(id);
    return it != instances_.end() ? std::optional<EffectView>(view(it->second)) : std::nullopt;
}

namespace {

/// A host that does nothing, for checking a section without touching the game.
class NullHost final : public MagicHost {
public:
    double actor_value(std::uint32_t, std::uint32_t) const override { return 0.0; }
    void damage_value(std::uint32_t, std::uint32_t, double, std::uint32_t) override {}
    void restore_value(std::uint32_t, std::uint32_t, double) override {}
    void modify_temp(std::uint32_t, std::uint32_t, double) override {}
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>*, std::uint32_t,
                         std::uint32_t) override {
        return true;
    }
    bool is_dead(std::uint32_t) const override { return false; }
    void log(const std::string&) override {}
};

} // namespace

bool ActiveEffects::check(const wfb::World& world, save::ByteReader r) {
    NullHost host;
    ActiveEffects scratch(world, host);
    return scratch.load(r);
}

void ActiveEffects::clear() {
    instances_.clear();
    ids_.clear();
    logged_.clear();
    unhandled_.clear();
}

std::vector<std::uint8_t> ActiveEffects::save() const {
    save::ByteWriter w;
    w.u32(k_section_version);
    w.u32(ids_.next());
    // Abilities are derived from the spells an actor knows and constant enchantments (worn armor) from
    // what is worn: never saved.
    const auto derived = [](const Instance& i) {
        return i.source.kind == SourceKind::ability || (i.source.kind == SourceKind::enchantment && i.permanent);
    };
    w.u32(static_cast<std::uint32_t>(
        std::ranges::count_if(instances_, [&](const auto& p) { return !derived(p.second); })));
    for (const auto& [id, i] : instances_) {
        if (derived(i)) {
            continue;
        }
        w.u32(i.id);
        w.u32(i.mgef);
        w.u8(static_cast<std::uint8_t>(i.source.kind));
        w.u32(i.source.form);
        w.u32(i.source.extra);
        w.u32(static_cast<std::uint32_t>(i.index));
        w.u32(i.caster);
        w.u32(i.target);
        w.f64(i.magnitude);
        w.f64(i.duration);
        w.f64(i.elapsed);
        w.f64(i.area);
        w.f64(i.since_check);
        w.u8(static_cast<std::uint8_t>((i.active ? 1 : 0) | (i.started ? 2 : 0) | (i.permanent ? 4 : 0)));
        w.u32(i.summoned);
    }
    return w.out;
}

bool ActiveEffects::load(save::ByteReader r) {
    std::uint32_t version = 0, next = 0, n = 0;
    if (!r.u32(version) || (version != k_section_version && version != 1) || !r.u32(next) || !r.count(n, 70)) {
        return false;
    }
    std::map<std::uint32_t, Instance> loaded;
    for (std::uint32_t k = 0; k < n; ++k) {
        Instance i;
        std::uint8_t kind = 0, flags = 0;
        std::uint32_t index = 0;
        if (!r.u32(i.id) || !r.u32(i.mgef) || !r.u8(kind) || !r.u32(i.source.form) || !r.u32(i.source.extra) ||
            !r.u32(index) || !r.u32(i.caster) || !r.u32(i.target) || !r.f64(i.magnitude) || !r.f64(i.duration) ||
            !r.f64(i.elapsed) || !r.f64(i.area) || !r.f64(i.since_check) || !r.u8(flags) ||
            (version >= 2 && !r.u32(i.summoned)) || kind > static_cast<std::uint8_t>(SourceKind::poison) || !std::isfinite(i.magnitude) ||
            !std::isfinite(i.duration) || !std::isfinite(i.elapsed) || !std::isfinite(i.area) ||
            !std::isfinite(i.since_check) || !vm::id_ranges::effect_instances.contains(i.id)) {
            return false;
        }
        i.source.kind = static_cast<SourceKind>(kind);
        i.index = static_cast<std::int32_t>(index);
        i.active = (flags & 1) != 0;
        i.started = (flags & 2) != 0;
        i.permanent = (flags & 4) != 0;
        if (!fill_from_mgef(i)) {
            host_.log("magic: a saved effect " + hex(i.mgef) + " is not in the world; dropped");
            continue;
        }
        loaded[i.id] = std::move(i);
    }
    if (!r.at_end()) {
        return false;
    }
    instances_ = std::move(loaded);
    ids_.clear();
    (void)ids_.restore(next);
    for (const auto& [id, i] : instances_) {
        ids_.note_used(id);
    }
    std::set<std::uint32_t> targets;
    for (const auto& [id, i] : instances_) {
        targets.insert(i.target);
    }
    for (const auto t : targets) {
        sync_holds(t);
    }
    return true;
}

} // namespace skydot::magic
