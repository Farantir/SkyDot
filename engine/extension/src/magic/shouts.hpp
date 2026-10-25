// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shouts (docs/adr/0008, "Shouts"): which words of power an actor knows and has unlocked, which
// shouts it has, which one it holds in the voice slot, how long until it may shout again, and the
// rules that turn a held button into words, a spell and a recovery time. A shout (SHOU) has up to
// three words (WOOP), each with the spell cast when it is the last word spoken and the seconds of
// recovery. The player (0x14) speaks only the leading run of words it knows and has unlocked; any
// other actor speaks all of them. Nothing here touches Godot.
//
// Not decided here, the game layer's (7b-3): a shout and a cast exclude each other (the GMSTs
// sMagicCastShoutWhileCasting and sMagicCastCastWhileShouting); this book only knows the voice slot
// and the recovery.
#pragma once

#include "magic/spell_lists.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace skydot::magic {

/// GMSTs fShoutTime1 and fShoutTime2: held seconds below which one, then two, words are spoken.
/// That they are the word thresholds is a guess from their names (docs/adr/0008).
struct ShoutSettings {
    double shout_time1 = 0.2;
    double shout_time2 = 0.9;
};

/// Words spoken after holding the button for `seconds`: 1 below time1, 2 below time2, else 3.
[[nodiscard]] int words_for_hold(double seconds, const ShoutSettings& settings);

/// Seconds before `shout` can be used again after `words` words (1 to 3): the SNAM recovery of the
/// last word spoken times `mult` (the ShoutRecoveryMult actor value), never below 0. 0 for an
/// unknown shout or a word count it has no word for.
[[nodiscard]] double recovery_seconds(const wfb::World& world, std::uint32_t shout, int words, double mult);

struct UnlockResult {
    bool ok = false;
    /// "" or "unknown word" (not known, or no such word) / "already unlocked" / "no dragon soul".
    std::string reason;
};

struct ShoutRelease {
    bool ok = false;
    /// "" or "unknown shout" / "recovering" / "no word".
    std::string reason;
    int words = 0;               ///< Words spoken.
    std::uint32_t spell = 0;     ///< The spell of the last word spoken (only that one is cast).
    double recovery = 0.0;       ///< Seconds until the next shout (also set on the book).
};

class ShoutBook {
public:
    static constexpr std::uint32_t k_section_version = 1;
    /// The reference whose words are learned and unlocked; every other actor speaks every word.
    static constexpr std::uint32_t k_player = 0x14;

    /// The resolved base shouts of an actor reference (NPC_ + race); empty for a non-actor.
    using BaseSource = std::function<std::vector<std::uint32_t>(std::uint32_t ref)>;

    explicit ShoutBook(const wfb::World& world) : world_(world) {}
    void set_base_source(BaseSource source) { source_ = std::move(source); }

    [[nodiscard]] bool is_shout(std::uint32_t form) const;

    /// Shouts of `ref`: base + added - removed; for the player also every shout with a known word
    /// (unless removed). Each once, in order.
    [[nodiscard]] std::vector<std::uint32_t> shouts_of(std::uint32_t ref);
    [[nodiscard]] bool has_shout(std::uint32_t ref, std::uint32_t shout);
    /// AddShout: false if it is no SHOU or `ref` has it. RemoveShout: false if `ref` lacks it; the
    /// voice slot is emptied if it held it.
    bool add_shout(std::uint32_t ref, std::uint32_t shout);
    bool remove_shout(std::uint32_t ref, std::uint32_t shout);

    /// Learn a word (false if it is no WOOP or known already).
    bool teach_word(std::uint32_t ref, std::uint32_t word);
    /// Unlock a word with a dragon soul's worth (false if it is no WOOP or unlocked already). Allowed
    /// without knowing it, as Game.UnlockWord does.
    bool unlock_word(std::uint32_t ref, std::uint32_t word);
    [[nodiscard]] bool is_word_known(std::uint32_t ref, std::uint32_t word) const;
    [[nodiscard]] bool is_word_unlocked(std::uint32_t ref, std::uint32_t word) const;
    /// Spend one of `souls` to unlock a word that is known and still locked; the caller takes the
    /// soul off DragonSouls when `ok`.
    UnlockResult unlock_with_soul(std::uint32_t ref, std::uint32_t word, double souls);

    /// Words of `shout` that `ref` can speak, 0 to 3 (see the class comment).
    [[nodiscard]] int usable_words(std::uint32_t ref, std::uint32_t shout) const;

    /// Hold `shout` in the voice slot (false if `ref` does not have it). To keep the slot shared
    /// with powers use `equip_voice`.
    bool equip(std::uint32_t ref, std::uint32_t shout);
    void unequip(std::uint32_t ref);
    [[nodiscard]] std::uint32_t equipped(std::uint32_t ref) const;

    /// Real seconds until `ref` may shout again.
    [[nodiscard]] double recovery_left(std::uint32_t ref) const;
    void set_recovery(std::uint32_t ref, double seconds);
    [[nodiscard]] bool ready(std::uint32_t ref) const { return recovery_left(ref) <= 0.0; }
    void tick(double seconds);

    /// Shout `shout` after holding for `held_seconds` with recovery multiplier `mult`: refuses while
    /// recovering or with no usable word; else speaks min(words_for_hold, usable) words, casts the
    /// spell of the last, and starts the recovery.
    ShoutRelease shout(std::uint32_t ref, std::uint32_t shout, double held_seconds, const ShoutSettings& settings,
                       double mult);

    /// Actors with anything of their own here.
    [[nodiscard]] std::vector<std::uint32_t> refs() const;
    void forget(std::uint32_t ref);
    void clear();
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] static bool load(save::ByteReader r, ShoutBook& out);
    /// Take the state of `other` (a decoded section), keeping this object's source.
    void adopt(ShoutBook&& other);

private:
    struct Record {
        std::set<std::uint32_t> known;
        std::set<std::uint32_t> unlocked;
        std::set<std::uint32_t> added;
        std::set<std::uint32_t> removed;
        std::uint32_t equipped = 0;
        double recovery = 0.0;
        [[nodiscard]] bool empty() const {
            return known.empty() && unlocked.empty() && added.empty() && removed.empty() && equipped == 0 &&
                   recovery <= 0.0;
        }
    };
    [[nodiscard]] const wfb::Shout* shout_of(std::uint32_t form) const;
    [[nodiscard]] const std::vector<std::uint32_t>& base_of(std::uint32_t ref);

    const wfb::World& world_;
    BaseSource source_;
    std::map<std::uint32_t, Record> records_;
    std::map<std::uint32_t, std::vector<std::uint32_t>> base_cache_;
};

/// Put `form` in the voice slot of `ref`, which powers and shouts share: a SHOU goes into the
/// ShoutBook and empties the power slot, anything else is a power for the SpellBook and empties the
/// shout. False (nothing changed) if `ref` does not have it.
bool equip_voice(SpellBook& spells, ShoutBook& shouts, std::uint32_t ref, std::uint32_t form);

} // namespace skydot::magic
