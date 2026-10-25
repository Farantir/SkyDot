// SPDX-License-Identifier: GPL-3.0-or-later
#include "magic/shouts.hpp"

#include "data/fb_search.hpp"

#include <algorithm>
#include <cmath>

namespace skydot::magic {

int words_for_hold(double seconds, const ShoutSettings& settings) {
    if (seconds < settings.shout_time1) {
        return 1;
    }
    return seconds < settings.shout_time2 ? 2 : 3;
}

double recovery_seconds(const wfb::World& world, std::uint32_t shout, int words, double mult) {
    const auto* s = lookup(world.shouts(), shout);
    if (s == nullptr || s->words() == nullptr || words < 1 || static_cast<std::size_t>(words) > s->words()->size()) {
        return 0.0;
    }
    return std::max(0.0, static_cast<double>(s->words()->Get(static_cast<flatbuffers::uoffset_t>(words - 1))->recovery()) *
                             mult);
}

const wfb::Shout* ShoutBook::shout_of(std::uint32_t form) const { return lookup(world_.shouts(), form); }

bool ShoutBook::is_shout(std::uint32_t form) const { return shout_of(form) != nullptr; }

const std::vector<std::uint32_t>& ShoutBook::base_of(std::uint32_t ref) {
    const auto it = base_cache_.find(ref);
    if (it != base_cache_.end()) {
        return it->second;
    }
    return base_cache_.emplace(ref, source_ ? source_(ref) : std::vector<std::uint32_t>{}).first->second;
}

std::vector<std::uint32_t> ShoutBook::shouts_of(std::uint32_t ref) {
    std::vector<std::uint32_t> out;
    const auto rec = records_.find(ref);
    const auto removed = [&](std::uint32_t s) { return rec != records_.end() && rec->second.removed.contains(s); };
    const auto add = [&](std::uint32_t s) {
        if (!removed(s) && std::ranges::find(out, s) == out.end()) {
            out.push_back(s);
        }
    };
    for (const auto s : base_of(ref)) {
        add(s);
    }
    if (rec != records_.end()) {
        for (const auto s : rec->second.added) {
            add(s);
        }
    }
    if (ref == k_player && rec != records_.end() && world_.shouts() != nullptr) {
        for (const auto* sh : *world_.shouts()) {
            if (sh->words() == nullptr) {
                continue;
            }
            for (const auto* w : *sh->words()) {
                if (rec->second.known.contains(w->word())) {
                    add(sh->id());
                    break;
                }
            }
        }
    }
    return out;
}

bool ShoutBook::has_shout(std::uint32_t ref, std::uint32_t shout) {
    const auto all = shouts_of(ref);
    return std::ranges::find(all, shout) != all.end();
}

bool ShoutBook::add_shout(std::uint32_t ref, std::uint32_t shout) {
    if (!is_shout(shout) || has_shout(ref, shout)) {
        return false;
    }
    auto& rec = records_[ref];
    rec.removed.erase(shout);
    rec.added.insert(shout);
    return true;
}

bool ShoutBook::remove_shout(std::uint32_t ref, std::uint32_t shout) {
    if (!has_shout(ref, shout)) {
        return false;
    }
    auto& rec = records_[ref];
    rec.added.erase(shout);
    rec.removed.insert(shout);
    if (rec.equipped == shout) {
        rec.equipped = 0;
    }
    return true;
}

bool ShoutBook::teach_word(std::uint32_t ref, std::uint32_t word) {
    return lookup(world_.words_of_power(), word) != nullptr && records_[ref].known.insert(word).second;
}

bool ShoutBook::unlock_word(std::uint32_t ref, std::uint32_t word) {
    return lookup(world_.words_of_power(), word) != nullptr && records_[ref].unlocked.insert(word).second;
}

bool ShoutBook::is_word_known(std::uint32_t ref, std::uint32_t word) const {
    const auto it = records_.find(ref);
    return it != records_.end() && it->second.known.contains(word);
}

bool ShoutBook::is_word_unlocked(std::uint32_t ref, std::uint32_t word) const {
    const auto it = records_.find(ref);
    return it != records_.end() && it->second.unlocked.contains(word);
}

UnlockResult ShoutBook::unlock_with_soul(std::uint32_t ref, std::uint32_t word, double souls) {
    if (!is_word_known(ref, word)) {
        return {false, "unknown word"};
    }
    if (is_word_unlocked(ref, word)) {
        return {false, "already unlocked"};
    }
    if (souls < 1.0) {
        return {false, "no dragon soul"};
    }
    (void)unlock_word(ref, word);
    return {true, {}};
}

int ShoutBook::usable_words(std::uint32_t ref, std::uint32_t shout) const {
    const auto* s = shout_of(shout);
    if (s == nullptr || s->words() == nullptr) {
        return 0;
    }
    const int total = static_cast<int>(std::min<std::size_t>(s->words()->size(), 3));
    if (ref != k_player) {
        return total;
    }
    int run = 0;
    for (int i = 0; i < total; ++i, ++run) {
        const auto word = s->words()->Get(static_cast<flatbuffers::uoffset_t>(i))->word();
        if (!is_word_known(ref, word) || !is_word_unlocked(ref, word)) {
            break;
        }
    }
    return run;
}

bool ShoutBook::equip(std::uint32_t ref, std::uint32_t shout) {
    if (!has_shout(ref, shout)) {
        return false;
    }
    records_[ref].equipped = shout;
    return true;
}

void ShoutBook::unequip(std::uint32_t ref) {
    const auto it = records_.find(ref);
    if (it != records_.end()) {
        it->second.equipped = 0;
    }
}

std::uint32_t ShoutBook::equipped(std::uint32_t ref) const {
    const auto it = records_.find(ref);
    return it != records_.end() ? it->second.equipped : 0;
}

double ShoutBook::recovery_left(std::uint32_t ref) const {
    const auto it = records_.find(ref);
    return it != records_.end() ? std::max(0.0, it->second.recovery) : 0.0;
}

void ShoutBook::set_recovery(std::uint32_t ref, double seconds) {
    seconds = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
    if (seconds > 0.0 || records_.contains(ref)) {
        records_[ref].recovery = seconds;
    }
}

void ShoutBook::tick(double seconds) {
    if (!(seconds > 0.0)) {
        return;
    }
    for (auto it = records_.begin(); it != records_.end();) {
        auto& rec = it->second;
        if (rec.recovery > 0.0) {
            rec.recovery = std::max(0.0, rec.recovery - seconds);
        }
        it = rec.empty() ? records_.erase(it) : std::next(it);
    }
}

ShoutRelease ShoutBook::shout(std::uint32_t ref, std::uint32_t shout, double held_seconds,
                              const ShoutSettings& settings, double mult) {
    ShoutRelease out;
    const auto* s = shout_of(shout);
    if (s == nullptr || s->words() == nullptr || !has_shout(ref, shout)) {
        out.reason = "unknown shout";
        return out;
    }
    if (!ready(ref)) {
        out.reason = "recovering";
        return out;
    }
    const int usable = usable_words(ref, shout);
    if (usable < 1) {
        out.reason = "no word";
        return out;
    }
    out.words = std::min(words_for_hold(held_seconds, settings), usable);
    out.spell = s->words()->Get(static_cast<flatbuffers::uoffset_t>(out.words - 1))->spell();
    out.recovery = recovery_seconds(world_, shout, out.words, mult);
    set_recovery(ref, out.recovery);
    out.ok = true;
    return out;
}

std::vector<std::uint32_t> ShoutBook::refs() const {
    std::vector<std::uint32_t> out;
    for (const auto& [ref, rec] : records_) {
        out.push_back(ref);
    }
    return out;
}

void ShoutBook::forget(std::uint32_t ref) {
    records_.erase(ref);
    base_cache_.erase(ref);
}

void ShoutBook::clear() {
    records_.clear();
    base_cache_.clear();
}

void ShoutBook::adopt(ShoutBook&& other) {
    records_ = std::move(other.records_);
    base_cache_.clear();
}

namespace {

void write_set(save::ByteWriter& w, const std::set<std::uint32_t>& set) {
    w.u32(static_cast<std::uint32_t>(set.size()));
    for (const auto v : set) {
        w.u32(v);
    }
}

bool read_set(save::ByteReader& r, std::set<std::uint32_t>& out) {
    std::uint32_t n = 0;
    if (!r.count(n, 4)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t v = 0;
        if (!r.u32(v)) {
            return false;
        }
        out.insert(v);
    }
    return true;
}

} // namespace

std::vector<std::uint8_t> ShoutBook::save() const {
    save::ByteWriter w;
    w.u32(k_section_version);
    std::uint32_t n = 0;
    for (const auto& [ref, rec] : records_) {
        n += rec.empty() ? 0U : 1U;
    }
    w.u32(n);
    for (const auto& [ref, rec] : records_) {
        if (rec.empty()) {
            continue;
        }
        w.u32(ref);
        write_set(w, rec.known);
        write_set(w, rec.unlocked);
        write_set(w, rec.added);
        write_set(w, rec.removed);
        w.u32(rec.equipped);
        w.f64(rec.recovery);
    }
    return w.out;
}

bool ShoutBook::load(save::ByteReader r, ShoutBook& out) {
    std::uint32_t version = 0, n = 0;
    if (!r.u32(version) || version != k_section_version || !r.count(n, 32)) {
        return false;
    }
    std::map<std::uint32_t, Record> records;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0;
        Record rec;
        if (!r.u32(ref) || !read_set(r, rec.known) || !read_set(r, rec.unlocked) || !read_set(r, rec.added) ||
            !read_set(r, rec.removed) || !r.u32(rec.equipped) || !r.f64(rec.recovery) || !std::isfinite(rec.recovery)) {
            return false;
        }
        rec.recovery = std::max(0.0, rec.recovery);
        records[ref] = std::move(rec);
    }
    if (!r.at_end()) {
        return false;
    }
    out.records_ = std::move(records);
    out.base_cache_.clear();
    return true;
}

bool equip_voice(SpellBook& spells, ShoutBook& shouts, std::uint32_t ref, std::uint32_t form) {
    if (shouts.is_shout(form)) {
        if (!shouts.equip(ref, form)) {
            return false;
        }
        spells.unequip(ref, Slot::power);
        return true;
    }
    if (!spells.equip(ref, form, Slot::power)) {
        return false;
    }
    shouts.unequip(ref);
    return true;
}

} // namespace skydot::magic
