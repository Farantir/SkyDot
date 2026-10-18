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
            return it->second.modifier;
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

bool ActorStates::is_dead(std::uint32_t ref) const {
    const Record* r = find(ref);
    if (r != nullptr && r->dead) {
        return true;
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
    if (base != nullptr && !base->essential && !base->is_player) {
        record.dead = true;
        deaths_.push_back(ref);
    }
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

void ActorStates::force(std::uint32_t ref, std::uint32_t av, double value) {
    if (Record* r = touch(ref, av)) {
        Value& v = r->values[av];
        v.modifier = value - base_value(ref, r, av) - v.damage;
        if (av == av::k_health) {
            check_death(ref, *r);
        }
    }
}

void ActorStates::damage(std::uint32_t ref, std::uint32_t av, double amount) {
    if (amount <= 0.0) {
        return;
    }
    if (Record* r = touch(ref, av)) {
        Value& v = r->values[av];
        v.damage -= amount;
        if (is_vital(av)) {
            // Health, Magicka and Stamina stop at 0.
            const double floor = -(base_value(ref, r, av) + v.modifier);
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
}

void ActorStates::adopt(ActorStates&& other) {
    records_ = std::move(other.records_);
    bases_.clear();
    deaths_.clear();
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
    }
    return w.out;
}

bool ActorStates::load(save::ByteReader r, ActorStates& out, std::uint32_t version) {
    out = ActorStates{};
    std::uint32_t n = 0;
    if (!r.count(n, version >= 2 ? 14 : 9)) {
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
    }
    return r.at_end();
}

} // namespace skydot::vm
