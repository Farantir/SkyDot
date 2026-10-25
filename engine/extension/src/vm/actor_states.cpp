// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/actor_states.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace skydot::vm {

namespace {

bool is_vital(std::uint32_t av) { return av == av::k_health || av == av::k_magicka || av == av::k_stamina; }

} // namespace

const ActorBase* ActorStates::base_of(std::uint32_t ref) const {
    auto it = bases_.find(ref);
    if (it == bases_.end()) {
        ActorBase base;
        const bool known = source_ && source_(ref, base);
        it = bases_.emplace(ref, known ? std::optional<ActorBase>(base) : std::nullopt).first;
    }
    return it->second ? &*it->second : nullptr;
}

const ActorStates::Record* ActorStates::find(std::uint32_t ref) const {
    const auto it = records_.find(ref);
    return it != records_.end() ? &it->second : nullptr;
}

double ActorStates::base_value(std::uint32_t ref, const Record* record, std::uint32_t av) const {
    if (record != nullptr) {
        const auto it = record->values.find(av);
        if (it != record->values.end() && it->second.base) {
            return *it->second.base;
        }
    }
    const ActorBase* base = base_of(ref);
    return base != nullptr ? base->base[av] : 0.0;
}

double ActorStates::modifier_value(const Record* record, std::uint32_t av) const {
    if (record != nullptr) {
        const auto it = record->values.find(av);
        if (it != record->values.end()) {
            return it->second.modifier + it->second.temp;
        }
    }
    return 0.0;
}

double ActorStates::damage_value(const Record* record, std::uint32_t av) const {
    if (record != nullptr) {
        const auto it = record->values.find(av);
        if (it != record->values.end()) {
            return it->second.damage;
        }
    }
    return 0.0;
}

double ActorStates::get_base(std::uint32_t ref, std::uint32_t av) const {
    if (av >= av::k_count || base_of(ref) == nullptr) {
        return 0.0;
    }
    return base_value(ref, find(ref), av);
}

double ActorStates::max(std::uint32_t ref, std::uint32_t av) const {
    if (av >= av::k_count || base_of(ref) == nullptr) {
        return 0.0;
    }
    const Record* r = find(ref);
    return base_value(ref, r, av) + modifier_value(r, av);
}

double ActorStates::get(std::uint32_t ref, std::uint32_t av) const {
    if (av >= av::k_count || base_of(ref) == nullptr) {
        return 0.0;
    }
    const Record* r = find(ref);
    if (av == av::k_health && is_dead(ref)) {
        return std::min(0.0, base_value(ref, r, av) + modifier_value(r, av) + damage_value(r, av));
    }
    return base_value(ref, r, av) + modifier_value(r, av) + damage_value(r, av);
}

double ActorStates::percentage(std::uint32_t ref, std::uint32_t av) const {
    const double m = max(ref, av);
    return m > 0.0 ? get(ref, av) / m : 1.0;
}

void ActorStates::revive(std::uint32_t ref) {
    Record* r = touch(ref, av::k_health);
    if (r == nullptr) {
        return;
    }
    r->dead = false;
    r->risen = true;
    r->killer = 0;
    r->corpse.reset();
    restore(ref, av::k_health, 1e9);
}

bool ActorStates::is_dead(std::uint32_t ref) const {
    const Record* r = find(ref);
    if (r != nullptr && r->dead) {
        return true;
    }
    if (r != nullptr && r->risen) {
        return false;
    }
    // A reference placed dead is dead before anything touches it.
    const ActorBase* base = base_of(ref);
    return base != nullptr && base->starts_dead && !base->is_player;
}

std::vector<std::uint32_t> ActorStates::dead_refs() const {
    std::vector<std::uint32_t> out;
    for (const auto& [ref, record] : records_) {
        if (record.dead) {
            out.push_back(ref);
        }
    }
    return out;
}

ActorStates::Record* ActorStates::touch(std::uint32_t ref, std::uint32_t av) {
    if (av >= av::k_count || base_of(ref) == nullptr) {
        return nullptr;
    }
    return &records_[ref];
}

void ActorStates::check_death(std::uint32_t ref, Record& record) {
    if (record.dead || get(ref, av::k_health) > 0.0) {
        return;
    }
    const ActorBase* base = base_of(ref);
    if (base != nullptr && !is_essential(ref) && !base->is_player) {
        record.dead = true;
        deaths_.push_back(ref);
    }
}

void ActorStates::kill(std::uint32_t ref, std::uint32_t killer) {
    Record* r = touch(ref, av::k_health);
    if (r == nullptr || is_dead(ref)) {
        return;
    }
    if (killer != 0) {
        r->killer = killer;
    }
    Value& v = r->values[av::k_health];
    v.damage = -(base_value(ref, r, av::k_health) + v.modifier + v.temp);
    if (!base_of(ref)->is_player) {
        r->dead = true;
        r->risen = false;
        deaths_.push_back(ref);
    }
}

void ActorStates::set_base_essential(std::uint32_t npc, bool essential) { base_flags_[npc].essential = essential; }

void ActorStates::set_base_invulnerable(std::uint32_t npc, bool invulnerable) {
    base_flags_[npc].invulnerable = invulnerable;
}

bool ActorStates::is_essential(std::uint32_t ref) const {
    const ActorBase* base = base_of(ref);
    if (base == nullptr) {
        return false;
    }
    if (const auto it = base_flags_.find(base->npc); it != base_flags_.end() && it->second.essential) {
        return *it->second.essential;
    }
    return base->essential;
}

bool ActorStates::is_invulnerable(std::uint32_t ref) const {
    if (base_flags_.empty()) {
        return false;
    }
    const ActorBase* base = base_of(ref);
    if (base == nullptr) {
        return false;
    }
    const auto it = base_flags_.find(base->npc);
    return it != base_flags_.end() && it->second.invulnerable.value_or(false);
}

namespace {
std::uint64_t pair_key(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::uint64_t>(std::min(a, b)) << 32 | std::max(a, b);
}
} // namespace

void ActorStates::set_relationship_rank(std::uint32_t a, std::uint32_t b, std::int32_t rank) {
    relationships_[pair_key(a, b)] = std::clamp(rank, -4, 4);
}

std::optional<std::int32_t> ActorStates::relationship_rank(std::uint32_t a, std::uint32_t b) const {
    const auto it = relationships_.find(pair_key(a, b));
    return it != relationships_.end() ? std::optional<std::int32_t>(it->second) : std::nullopt;
}

void ActorStates::set_faction_reaction(std::uint32_t from, std::uint32_t to, std::uint8_t reaction) {
    reactions_[static_cast<std::uint64_t>(from) << 32 | to] = std::min<std::uint8_t>(reaction, 3);
}

std::optional<std::uint8_t> ActorStates::faction_reaction(std::uint32_t from, std::uint32_t to) const {
    const auto it = reactions_.find(static_cast<std::uint64_t>(from) << 32 | to);
    return it != reactions_.end() ? std::optional<std::uint8_t>(it->second) : std::nullopt;
}

std::vector<std::pair<std::uint32_t, std::uint8_t>> ActorStates::faction_reactions_from(std::uint32_t from) const {
    std::vector<std::pair<std::uint32_t, std::uint8_t>> out;
    for (auto it = reactions_.lower_bound(static_cast<std::uint64_t>(from) << 32);
         it != reactions_.end() && (it->first >> 32) == from; ++it) {
        out.emplace_back(static_cast<std::uint32_t>(it->first & 0xFFFFFFFFU), it->second);
    }
    return out;
}

void ActorStates::set_living_place(std::uint32_t ref, const CorpsePlace& place) {
    if (base_of(ref) != nullptr) {
        living_[ref] = place;
    }
}

std::optional<ActorStates::CorpsePlace> ActorStates::living_place(std::uint32_t ref) const {
    const auto it = living_.find(ref);
    return it != living_.end() ? std::optional<CorpsePlace>(it->second) : std::nullopt;
}

void ActorStates::set_allowed_to_fly(std::uint32_t ref, bool allowed) {
    if (Record* r = touch(ref, av::k_health)) {
        r->no_flying = !allowed;
    }
}

bool ActorStates::allowed_to_fly(std::uint32_t ref) const {
    const Record* r = find(ref);
    return r == nullptr || !r->no_flying;
}

void ActorStates::set_landing_marker(std::uint32_t ref, std::uint32_t marker) {
    if (Record* r = touch(ref, av::k_health)) {
        r->landing_marker = marker;
    }
}

std::uint32_t ActorStates::landing_marker(std::uint32_t ref) const {
    const Record* r = find(ref);
    return r != nullptr ? r->landing_marker : 0;
}

void ActorStates::defer_death(std::uint32_t ref, double seconds) { dying_[ref] = std::max(seconds, 0.0); }

bool ActorStates::finish_dying(std::uint32_t ref) { return dying_.erase(ref) > 0; }

std::vector<std::uint32_t> ActorStates::take_due_deaths() { return std::exchange(due_, {}); }

void ActorStates::set_ghost(std::uint32_t ref, bool ghost) {
    if (Record* r = touch(ref, av::k_health)) {
        r->ghost = ghost;
    }
}

bool ActorStates::is_ghost(std::uint32_t ref) const {
    const Record* r = find(ref);
    return r != nullptr && r->ghost;
}

void ActorStates::set_faction_rank(std::uint32_t ref, std::uint32_t faction, std::int32_t rank) {
    if (Record* r = touch(ref, av::k_health)) {
        r->factions[faction] = std::max<std::int32_t>(rank, -1);
    }
}

std::optional<std::int32_t> ActorStates::faction_override(std::uint32_t ref, std::uint32_t faction) const {
    const Record* r = find(ref);
    if (r == nullptr) {
        return std::nullopt;
    }
    const auto it = r->factions.find(faction);
    return it != r->factions.end() ? std::optional<std::int32_t>(it->second) : std::nullopt;
}

std::vector<std::uint32_t> ActorStates::added_factions(std::uint32_t ref) const {
    std::vector<std::uint32_t> out;
    if (const Record* r = find(ref)) {
        for (const auto& [faction, rank] : r->factions) {
            if (rank >= 0) {
                out.push_back(faction);
            }
        }
    }
    return out;
}

void ActorStates::set_killer(std::uint32_t ref, std::uint32_t killer) {
    if (Record* r = touch(ref, av::k_health)) {
        r->killer = killer;
    }
}

std::uint32_t ActorStates::killer_of(std::uint32_t ref) const {
    const Record* r = find(ref);
    return r != nullptr ? r->killer : 0;
}

void ActorStates::set_corpse_place(std::uint32_t ref, const CorpsePlace& place) {
    if (Record* r = touch(ref, av::k_health)) {
        r->corpse = place;
    }
}

std::optional<ActorStates::CorpsePlace> ActorStates::corpse_place(std::uint32_t ref) const {
    const Record* r = find(ref);
    return r != nullptr ? r->corpse : std::nullopt;
}

std::vector<std::pair<std::uint32_t, ActorStates::CorpsePlace>> ActorStates::corpse_places() const {
    std::vector<std::pair<std::uint32_t, CorpsePlace>> out;
    for (const auto& [ref, record] : records_) {
        if (record.corpse) {
            out.emplace_back(ref, *record.corpse);
        }
    }
    return out;
}

void ActorStates::set_base(std::uint32_t ref, std::uint32_t av, double value) {
    if (Record* r = touch(ref, av)) {
        r->values[av].base = value;
        if (av == av::k_health) {
            check_death(ref, *r);
        }
    }
}

void ActorStates::mod(std::uint32_t ref, std::uint32_t av, double amount) {
    if (Record* r = touch(ref, av)) {
        r->values[av].modifier += amount;
        if (av == av::k_health) {
            check_death(ref, *r);
        }
    }
}

void ActorStates::mod_temp(std::uint32_t ref, std::uint32_t av, double amount) {
    if (Record* r = touch(ref, av)) {
        Value& v = r->values[av];
        v.temp += amount;
        if (std::abs(v.temp) < 1e-9) {
            v.temp = 0.0;
        }
        if (is_vital(av)) {
            // A lower maximum takes the value with it.
            v.damage = std::max(v.damage, std::min(-(base_value(ref, r, av) + v.modifier + v.temp), 0.0));
        }
        if (av == av::k_health) {
            check_death(ref, *r);
        }
    }
}

double ActorStates::get_temp(std::uint32_t ref, std::uint32_t av) const {
    const Record* r = find(ref);
    if (r != nullptr) {
        const auto it = r->values.find(av);
        if (it != r->values.end()) {
            return it->second.temp;
        }
    }
    return 0.0;
}

void ActorStates::force(std::uint32_t ref, std::uint32_t av, double value) {
    if (Record* r = touch(ref, av)) {
        Value& v = r->values[av];
        v.modifier = value - base_value(ref, r, av) - v.temp - v.damage;
        if (av == av::k_health) {
            check_death(ref, *r);
        }
    }
}

void ActorStates::damage(std::uint32_t ref, std::uint32_t av, double amount) {
    if (amount <= 0.0 || (av == av::k_health && is_invulnerable(ref))) {
        return;
    }
    if (Record* r = touch(ref, av)) {
        Value& v = r->values[av];
        v.damage -= amount;
        if (is_vital(av)) {
            // Health, Magicka and Stamina stop at 0.
            const double floor = -(base_value(ref, r, av) + v.modifier + v.temp);
            v.damage = std::max(v.damage, std::min(floor, 0.0));
        }
        if (av == av::k_magicka) {
            r->magicka_pause = magicka_regen_delay_;
        } else if (av == av::k_stamina) {
            r->stamina_pause = stamina_regen_delay_;
        } else if (av == av::k_health) {
            check_death(ref, *r);
        }
    }
}

void ActorStates::restore(std::uint32_t ref, std::uint32_t av, double amount) {
    if (amount <= 0.0) {
        return;
    }
    if (Record* r = touch(ref, av)) {
        Value& v = r->values[av];
        v.damage = std::min(0.0, v.damage + amount);
    }
}

void ActorStates::tick(double seconds) {
    if (seconds <= 0.0) {
        return;
    }
    for (auto it = dying_.begin(); it != dying_.end();) {
        it->second -= seconds;
        if (it->second <= 0.0) {
            due_.push_back(it->first);
            it = dying_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [ref, record] : records_) {
        // What is left of a pause holds back that much of this tick.
        const double magicka_held = std::min(record.magicka_pause, seconds);
        const double stamina_held = std::min(record.stamina_pause, seconds);
        record.magicka_pause = std::max(0.0, record.magicka_pause - seconds);
        record.stamina_pause = std::max(0.0, record.stamina_pause - seconds);
        if (record.dead) {
            continue; // a dead actor does not recover
        }
        struct Vital {
            std::uint32_t value, rate, mult;
            double held;
        };
        Vital vitals[] = {
            {av::k_health, av::k_heal_rate, av::k_heal_rate_mult, 0.0},
            {av::k_magicka, av::k_magicka_rate, av::k_magicka_rate_mult, magicka_held},
            {av::k_stamina, av::k_stamina_rate, av::k_stamina_rate_mult, stamina_held},
        };
        for (const Vital& vital : vitals) {
            const auto it = record.values.find(vital.value);
            if (it == record.values.end() || it->second.damage >= 0.0) {
                continue;
            }
            const double per_second = std::max(0.0, max(ref, vital.value)) *
                                      std::max(0.0, get(ref, vital.rate)) / 100.0 *
                                      std::max(0.0, get(ref, vital.mult)) / 100.0;
            it->second.damage = std::min(0.0, it->second.damage + per_second * (seconds - vital.held));
        }
    }
}

std::vector<std::uint32_t> ActorStates::take_deaths() { return std::exchange(deaths_, {}); }

void ActorStates::forget(std::uint32_t ref) {
    records_.erase(ref);
    bases_.erase(ref);
}

void ActorStates::clear() {
    records_.clear();
    bases_.clear();
    deaths_.clear();
    dying_.clear();
    due_.clear();
    base_flags_.clear();
    relationships_.clear();
    reactions_.clear();
    living_.clear();
}

void ActorStates::adopt(ActorStates&& other) {
    records_ = std::move(other.records_);
    dying_ = std::move(other.dying_);
    base_flags_ = std::move(other.base_flags_);
    relationships_ = std::move(other.relationships_);
    reactions_ = std::move(other.reactions_);
    living_ = std::move(other.living_);
    bases_.clear();
    deaths_.clear();
    due_.clear();
}

std::vector<std::uint8_t> ActorStates::save() const {
    save::ByteWriter w;
    w.u32(static_cast<std::uint32_t>(records_.size()));
    for (const auto& [ref, record] : records_) {
        w.u32(ref);
        w.u8(record.dead ? 1 : 0);
        w.u32(static_cast<std::uint32_t>(record.values.size()));
        for (const auto& [av, v] : record.values) {
            w.u32(av);
            w.u8(v.base ? 1 : 0);
            w.f64(v.base.value_or(0.0));
            w.f64(v.modifier);
            w.f64(v.damage);
        }
        w.u32(record.killer);
        w.u8(record.corpse ? 1 : 0);
        if (record.corpse) {
            w.u32(record.corpse->space);
            w.f64(record.corpse->x);
            w.f64(record.corpse->y);
            w.f64(record.corpse->z);
            w.f64(record.corpse->rot_z);
        }
        w.u8(record.no_flying ? 1 : 0);
        w.u32(record.landing_marker);
        w.u8(record.ghost ? 1 : 0);
        w.u32(static_cast<std::uint32_t>(record.factions.size()));
        for (const auto& [faction, rank] : record.factions) {
            w.u32(faction);
            w.u32(static_cast<std::uint32_t>(rank));
        }
    }
    w.u32(static_cast<std::uint32_t>(dying_.size()));
    for (const auto& [ref, left] : dying_) {
        w.u32(ref);
        w.f64(left);
    }
    w.u32(static_cast<std::uint32_t>(base_flags_.size()));
    const auto tri = [](const std::optional<bool>& v) { return static_cast<std::uint8_t>(v ? (*v ? 2 : 1) : 0); };
    for (const auto& [npc, flags] : base_flags_) {
        w.u32(npc);
        w.u8(tri(flags.essential));
        w.u8(tri(flags.invulnerable));
    }
    w.u32(static_cast<std::uint32_t>(relationships_.size()));
    for (const auto& [key, rank] : relationships_) {
        w.u32(static_cast<std::uint32_t>(key >> 32));
        w.u32(static_cast<std::uint32_t>(key & 0xFFFFFFFFU));
        w.u32(static_cast<std::uint32_t>(rank));
    }
    w.u32(static_cast<std::uint32_t>(reactions_.size()));
    for (const auto& [key, reaction] : reactions_) {
        w.u32(static_cast<std::uint32_t>(key >> 32));
        w.u32(static_cast<std::uint32_t>(key & 0xFFFFFFFFU));
        w.u8(reaction);
    }
    w.u32(static_cast<std::uint32_t>(living_.size()));
    for (const auto& [ref, place] : living_) {
        w.u32(ref);
        w.u32(place.space);
        w.f64(place.x);
        w.f64(place.y);
        w.f64(place.z);
        w.f64(place.rot_z);
    }
    return w.out;
}

bool ActorStates::load(save::ByteReader r, ActorStates& out, std::uint32_t version) {
    out = ActorStates{};
    std::uint32_t n = 0;
    if (!r.count(n, version >= 4 ? 24 : version >= 3 ? 19 : version >= 2 ? 14 : 9)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0, values = 0;
        std::uint8_t dead = 0;
        if (!r.u32(ref) || !r.u8(dead) || !r.count(values, 29)) {
            return false;
        }
        Record& record = out.records_[ref];
        record.dead = dead != 0;
        for (std::uint32_t k = 0; k < values; ++k) {
            std::uint32_t av = 0;
            std::uint8_t has_base = 0;
            double base = 0.0;
            Value v;
            if (!r.u32(av) || !r.u8(has_base) || !r.f64(base) || !r.f64(v.modifier) || !r.f64(v.damage) ||
                av >= av::k_count || !std::isfinite(base) || !std::isfinite(v.modifier) ||
                !std::isfinite(v.damage)) {
                return false;
            }
            if (has_base != 0) {
                v.base = base;
            }
            record.values[av] = v;
        }
        if (version >= 2) {
            std::uint8_t has_corpse = 0;
            if (!r.u32(record.killer) || !r.u8(has_corpse)) {
                return false;
            }
            if (has_corpse != 0) {
                CorpsePlace place;
                if (!r.u32(place.space) || !r.f64(place.x) || !r.f64(place.y) || !r.f64(place.z) ||
                    !r.f64(place.rot_z) || !std::isfinite(place.x) || !std::isfinite(place.y) ||
                    !std::isfinite(place.z) || !std::isfinite(place.rot_z)) {
                    return false;
                }
                record.corpse = place;
            }
        }
        if (version >= 3) {
            std::uint8_t no_flying = 0;
            if (!r.u8(no_flying) || !r.u32(record.landing_marker)) {
                return false;
            }
            record.no_flying = no_flying != 0;
        }
        if (version >= 4) {
            std::uint8_t ghost = 0;
            std::uint32_t factions = 0;
            if (!r.u8(ghost) || !r.count(factions, 8)) {
                return false;
            }
            record.ghost = ghost != 0;
            for (std::uint32_t k = 0; k < factions; ++k) {
                std::uint32_t faction = 0, rank = 0;
                if (!r.u32(faction) || !r.u32(rank)) {
                    return false;
                }
                record.factions[faction] = std::max<std::int32_t>(static_cast<std::int32_t>(rank), -1);
            }
        }
    }
    if (version >= 5) {
        std::uint32_t dying = 0;
        if (!r.count(dying, 12)) {
            return false;
        }
        for (std::uint32_t i = 0; i < dying; ++i) {
            std::uint32_t ref = 0;
            double left = 0.0;
            if (!r.u32(ref) || !r.f64(left) || !std::isfinite(left)) {
                return false;
            }
            out.dying_[ref] = std::max(left, 0.0);
        }
    }
    if (version >= 6) {
        std::uint32_t n_flags = 0;
        if (!r.count(n_flags, 6)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n_flags; ++i) {
            std::uint32_t npc = 0;
            std::uint8_t essential = 0, invulnerable = 0;
            if (!r.u32(npc) || !r.u8(essential) || !r.u8(invulnerable) || essential > 2 || invulnerable > 2) {
                return false;
            }
            BaseFlags& flags = out.base_flags_[npc];
            if (essential != 0) {
                flags.essential = essential == 2;
            }
            if (invulnerable != 0) {
                flags.invulnerable = invulnerable == 2;
            }
        }
        std::uint32_t n_rel = 0;
        if (!r.count(n_rel, 12)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n_rel; ++i) {
            std::uint32_t a = 0, b = 0, rank = 0;
            if (!r.u32(a) || !r.u32(b) || !r.u32(rank)) {
                return false;
            }
            out.set_relationship_rank(a, b, static_cast<std::int32_t>(rank));
        }
        std::uint32_t n_react = 0;
        if (!r.count(n_react, 9)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n_react; ++i) {
            std::uint32_t from = 0, to = 0;
            std::uint8_t reaction = 0;
            if (!r.u32(from) || !r.u32(to) || !r.u8(reaction) || reaction > 3) {
                return false;
            }
            out.set_faction_reaction(from, to, reaction);
        }
    }
    if (version >= 7) {
        std::uint32_t n_living = 0;
        if (!r.count(n_living, 40)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n_living; ++i) {
            std::uint32_t ref = 0;
            CorpsePlace place;
            if (!r.u32(ref) || !r.u32(place.space) || !r.f64(place.x) || !r.f64(place.y) || !r.f64(place.z) ||
                !r.f64(place.rot_z) || !std::isfinite(place.x) || !std::isfinite(place.y) || !std::isfinite(place.z) ||
                !std::isfinite(place.rot_z)) {
                return false;
            }
            out.living_[ref] = place;
        }
    }
    return r.at_end();
}

} // namespace skydot::vm
