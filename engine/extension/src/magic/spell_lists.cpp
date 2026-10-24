// SPDX-License-Identifier: GPL-3.0-or-later
#include "magic/spell_lists.hpp"

#include "data/fb_search.hpp"
#include "data/leveled.hpp"

#include <algorithm>
#include <cmath>

namespace skydot::magic {

namespace {

/// The player's reference (build/refs.hpp k_player_ref).
constexpr std::uint32_t k_player_actor = 0x14;

/// NpcTemplateFlags bit 3 (0x8): the spell list comes from the template.
constexpr std::uint32_t k_template_spell_list = 0x8;

void add_forms(const wfb::World& world, const flatbuffers::Vector<std::uint32_t>* raw, std::uint32_t ref,
               std::uint16_t level, std::vector<std::uint32_t>& out) {
    if (raw == nullptr) {
        return;
    }
    leveled::Rng rng(leveled::seed_for(ref, leveled::k_purpose_spells));
    for (const std::uint32_t id : *raw) {
        std::vector<std::uint32_t> found;
        if (lookup(world.spells(), id) != nullptr) {
            found.push_back(id);
        } else if (leveled::is_list(world, id)) {
            for (const auto& e : leveled::resolve(world, id, level, rng)) {
                if (lookup(world.spells(), e.form) != nullptr) {
                    found.push_back(e.form);
                }
            }
        } // else a shout: not in this file
        for (const std::uint32_t f : found) {
            if (std::ranges::find(out, f) == out.end()) {
                out.push_back(f);
            }
        }
    }
}

void collect(const wfb::World& world, std::uint32_t npc_id, std::uint32_t ref, std::uint16_t level, int depth,
             std::vector<std::uint32_t>& out) {
    const auto* npc = lookup(world.npcs(), npc_id);
    if (npc == nullptr) {
        return;
    }
    add_forms(world, npc->spells(), ref, level, out);
    if (depth < 4 && npc->template_() != 0 &&
        (static_cast<std::uint32_t>(npc->template_flags()) & k_template_spell_list) != 0) {
        leveled::Rng rng(leveled::seed_for(ref, leveled::k_purpose_actor));
        const std::uint32_t tpl = leveled::pick_one(world, npc->template_(), level, rng);
        if (tpl != 0) {
            collect(world, tpl, ref, level, depth + 1, out);
        }
    }
    if (const auto* race = lookup(world.races(), npc->race())) {
        add_forms(world, race->spells(), ref, level, out);
    }
}

} // namespace

std::vector<std::uint32_t> base_spells(const wfb::World& world, std::uint32_t npc, std::uint32_t ref,
                                       std::uint16_t level) {
    std::vector<std::uint32_t> out;
    collect(world, npc, ref, level, 0, out);
    return out;
}

// ---- SpellBook ---------------------------------------------------------------------------------

std::uint32_t SpellBook::type_of(std::uint32_t spell) const {
    const auto* s = lookup(world_.spells(), spell);
    return s != nullptr ? s->type() : 0xFFFFFFFFu;
}

const std::vector<std::uint32_t>& SpellBook::base_of(std::uint32_t ref) {
    auto it = base_cache_.find(ref);
    if (it == base_cache_.end()) {
        it = base_cache_.emplace(ref, source_ ? source_(ref) : std::vector<std::uint32_t>{}).first;
    }
    return it->second;
}

std::vector<std::uint32_t> SpellBook::spells_of(std::uint32_t ref) {
    std::vector<std::uint32_t> out;
    const auto rec = records_.find(ref);
    for (const std::uint32_t s : base_of(ref)) {
        if (rec == records_.end() || !rec->second.removed.contains(s)) {
            out.push_back(s);
        }
    }
    if (rec != records_.end()) {
        for (const std::uint32_t s : rec->second.added) {
            if (std::ranges::find(out, s) == out.end()) {
                out.push_back(s);
            }
        }
    }
    return out;
}

bool SpellBook::has_spell(std::uint32_t ref, std::uint32_t spell) {
    const auto known = spells_of(ref);
    return std::ranges::find(known, spell) != known.end();
}

void SpellBook::apply_ability(std::uint32_t ref, std::uint32_t spell) {
    const auto* s = lookup(world_.spells(), spell);
    if (s == nullptr || s->type() != spell_type::ability) {
        return;
    }
    const auto inputs = inputs_of(s->effects());
    effects_.apply(ref, ref, {SourceKind::ability, spell, 0}, inputs, {.constant = true});
}

bool SpellBook::add_spell(std::uint32_t ref, std::uint32_t spell) {
    if (lookup(world_.spells(), spell) == nullptr || has_spell(ref, spell)) {
        return false;
    }
    Record& rec = records_[ref];
    rec.removed.erase(spell);
    // A base spell removed earlier comes back by un-removing; otherwise it is an addition.
    const auto& base = base_of(ref);
    if (std::ranges::find(base, spell) == base.end()) {
        rec.added.insert(spell);
    }
    if (applied_.contains(ref)) {
        apply_ability(ref, spell);
    }
    return true;
}

bool SpellBook::remove_spell(std::uint32_t ref, std::uint32_t spell) {
    if (!has_spell(ref, spell)) {
        return false;
    }
    Record& rec = records_[ref];
    if (rec.added.erase(spell) == 0) {
        rec.removed.insert(spell);
    }
    for (auto& slot : rec.slots) {
        if (slot == spell) {
            slot = 0;
        }
    }
    if (type_of(spell) == spell_type::ability) {
        effects_.dispel(ref, spell);
    }
    return true;
}

void SpellBook::ensure_abilities(std::uint32_t ref) {
    if (!applied_.insert(ref).second) {
        return;
    }
    for (const std::uint32_t s : spells_of(ref)) {
        apply_ability(ref, s);
    }
}

std::vector<std::uint32_t> SpellBook::refs() const {
    std::vector<std::uint32_t> out;
    for (const auto& [ref, rec] : records_) {
        out.push_back(ref);
    }
    return out;
}

std::uint32_t SpellBook::equipped(std::uint32_t ref, Slot slot) const {
    const auto it = records_.find(ref);
    return it != records_.end() ? it->second.slots[static_cast<std::size_t>(slot)] : 0;
}

bool SpellBook::equip(std::uint32_t ref, std::uint32_t spell, Slot slot, bool scroll) {
    if (scroll) {
        if (slot == Slot::power || spell == 0) {
            return false;
        }
    } else {
        if (!has_spell(ref, spell)) {
            return false;
        }
        const std::uint32_t type = type_of(spell);
        const bool power = type == spell_type::power || type == spell_type::lesser_power;
        if ((slot == Slot::power) != power) {
            return false;
        }
    }
    records_[ref].slots[static_cast<std::size_t>(slot)] = spell;
    return true;
}

void SpellBook::unequip(std::uint32_t ref, Slot slot) {
    const auto it = records_.find(ref);
    if (it != records_.end()) {
        it->second.slots[static_cast<std::size_t>(slot)] = 0;
    }
}

double SpellBook::cooldown_left(std::uint32_t ref, std::uint32_t spell, double game_days) const {
    if (type_of(spell) != spell_type::power) {
        return 0.0; // lesser powers and everything else have no cooldown
    }
    const auto rec = records_.find(ref);
    if (rec == records_.end()) {
        return 0.0;
    }
    const auto used = rec->second.used.find(spell);
    if (used == rec->second.used.end()) {
        return 0.0;
    }
    return std::max(0.0, used->second + k_power_cooldown_days - game_days);
}

bool SpellBook::ready(std::uint32_t ref, std::uint32_t spell, double game_days) const {
    return cooldown_left(ref, spell, game_days) <= 0.0;
}

void SpellBook::note_used(std::uint32_t ref, std::uint32_t spell, double game_days) {
    if (type_of(spell) == spell_type::power) {
        records_[ref].used[spell] = game_days;
    }
}

bool SpellBook::learn_enchantment(std::uint32_t enchantment) {
    return lookup(world_.enchantments(), enchantment) != nullptr && known_enchantments_.insert(enchantment).second;
}

void SpellBook::forget(std::uint32_t ref) {
    records_.erase(ref);
    base_cache_.erase(ref);
    applied_.erase(ref);
}

void SpellBook::clear() {
    records_.clear();
    base_cache_.clear();
    applied_.clear();
    known_enchantments_.clear();
}

void SpellBook::adopt(SpellBook&& other) {
    records_ = std::move(other.records_);
    known_enchantments_ = std::move(other.known_enchantments_);
    base_cache_.clear();
    applied_.clear();
}

std::vector<std::uint8_t> SpellBook::save() const {
    save::ByteWriter w;
    w.u32(k_section_version);
    w.u32(static_cast<std::uint32_t>(records_.size()));
    for (const auto& [ref, rec] : records_) {
        w.u32(ref);
        w.u32(static_cast<std::uint32_t>(rec.added.size()));
        for (const auto s : rec.added) {
            w.u32(s);
        }
        w.u32(static_cast<std::uint32_t>(rec.removed.size()));
        for (const auto s : rec.removed) {
            w.u32(s);
        }
        for (const auto s : rec.slots) {
            w.u32(s);
        }
        w.u32(static_cast<std::uint32_t>(rec.used.size()));
        for (const auto& [spell, day] : rec.used) {
            w.u32(spell);
            w.f64(day);
        }
    }
    w.u32(static_cast<std::uint32_t>(known_enchantments_.size()));
    for (const auto e : known_enchantments_) {
        w.u32(e);
    }
    return w.out;
}

bool SpellBook::load(save::ByteReader r, SpellBook& out) {
    std::uint32_t version = 0, n = 0;
    if (!r.u32(version) || version != k_section_version || !r.count(n, 28)) {
        return false;
    }
    std::map<std::uint32_t, Record> records;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0, count = 0;
        Record rec;
        if (!r.u32(ref) || !r.count(count, 4)) {
            return false;
        }
        for (std::uint32_t k = 0; k < count; ++k) {
            std::uint32_t s = 0;
            if (!r.u32(s)) {
                return false;
            }
            rec.added.insert(s);
        }
        if (!r.count(count, 4)) {
            return false;
        }
        for (std::uint32_t k = 0; k < count; ++k) {
            std::uint32_t s = 0;
            if (!r.u32(s)) {
                return false;
            }
            rec.removed.insert(s);
        }
        for (auto& slot : rec.slots) {
            if (!r.u32(slot)) {
                return false;
            }
        }
        if (!r.count(count, 12)) {
            return false;
        }
        for (std::uint32_t k = 0; k < count; ++k) {
            std::uint32_t s = 0;
            double day = 0.0;
            if (!r.u32(s) || !r.f64(day) || !std::isfinite(day)) {
                return false;
            }
            rec.used[s] = day;
        }
        records[ref] = std::move(rec);
    }
    std::uint32_t known = 0;
    if (!r.count(known, 4)) {
        return false;
    }
    std::set<std::uint32_t> enchantments;
    for (std::uint32_t k = 0; k < known; ++k) {
        std::uint32_t e = 0;
        if (!r.u32(e)) {
            return false;
        }
        enchantments.insert(e);
    }
    if (!r.at_end()) {
        return false;
    }
    out.records_ = std::move(records);
    out.known_enchantments_ = std::move(enchantments);
    out.base_cache_.clear();
    out.applied_.clear();
    return true;
}

std::set<std::uint32_t> actors_to_rederive(const SpellBook& book, const ActiveEffects& effects,
                                           const std::vector<std::uint32_t>& wearers) {
    // refs() returns a vector by value: take it once (begin and end of two copies hung a load, 0fec095).
    const std::vector<std::uint32_t> known = book.refs();
    std::set<std::uint32_t> out(known.begin(), known.end());
    out.merge(effects.targets());
    out.insert(wearers.begin(), wearers.end());
    out.insert(k_player_actor);
    return out;
}

} // namespace skydot::magic
