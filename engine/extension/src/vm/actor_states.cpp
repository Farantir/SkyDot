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
    return base_value(ref, r, av) + modifier_value(r, av) + damage_value(r, av);
}

double ActorStates::percentage(std::uint32_t ref, std::uint32_t av) const {
    const double m = max(ref, av);
    return m > 0.0 ? get(ref, av) / m : 1.0;
}

bool ActorStates::is_dead(std::uint32_t ref) const {
    const Record* r = find(ref);
    return r != nullptr && r->dead;
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
            r->magicka_pause = k_damaged_magicka_regen_delay;
        } else if (av == av::k_stamina) {
            r->stamina_pause = k_damaged_stamina_regen_delay;
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
        // A pause that ends during this tick still holds back all of it.
        const bool magicka_paused = record.magicka_pause > 0.0;
        const bool stamina_paused = record.stamina_pause > 0.0;
        record.magicka_pause = std::max(0.0, record.magicka_pause - seconds);
        record.stamina_pause = std::max(0.0, record.stamina_pause - seconds);
        if (record.dead) {
            continue; // a dead actor does not recover
        }
        struct Vital {
            std::uint32_t value, rate, mult;
            bool paused;
        };
        Vital vitals[] = {
            {av::k_health, av::k_heal_rate, av::k_heal_rate_mult, false},
            {av::k_magicka, av::k_magicka_rate, av::k_magicka_rate_mult, magicka_paused},
            {av::k_stamina, av::k_stamina_rate, av::k_stamina_rate_mult, stamina_paused},
        };
        for (const Vital& vital : vitals) {
            const auto it = record.values.find(vital.value);
            if (it == record.values.end() || it->second.damage >= 0.0) {
                continue;
            }
            if (vital.paused) {
                continue;
            }
            const double per_second = std::max(0.0, max(ref, vital.value)) *
                                      std::max(0.0, get(ref, vital.rate)) / 100.0 *
                                      std::max(0.0, get(ref, vital.mult)) / 100.0;
            it->second.damage = std::min(0.0, it->second.damage + per_second * seconds);
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
    }
    return w.out;
}

bool ActorStates::load(save::ByteReader r, ActorStates& out) {
    out = ActorStates{};
    std::uint32_t n = 0;
    if (!r.count(n, 9)) {
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
    }
    return r.at_end();
}

} // namespace skydot::vm
