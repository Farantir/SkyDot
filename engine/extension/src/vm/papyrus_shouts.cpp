// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's shouts for the game layer: words of power, the voice recovery and casting a
// shout (rules in magic/shouts.hpp; state in MagicState).
#include "vm/papyrus_magic_state.hpp"

namespace skydot {

// ---- shouts: the game layer's API ----------------------------------------------------------------

std::uint32_t SkydotPapyrus::voice_shout(std::uint32_t ref) const {
    const auto* m = magic_.get();
    return m != nullptr ? m->shouts.equipped(ref) : 0;
}

Dictionary SkydotPapyrus::shout_info(std::int64_t actor_in, std::int64_t shout_in) {
    Dictionary out;
    out["found"] = false;
    auto* m = magic_state();
    const auto actor = static_cast<std::uint32_t>(actor_in);
    const auto shout = static_cast<std::uint32_t>(shout_in);
    const auto* s = m != nullptr ? lookup(m->world_.shouts(), shout) : nullptr;
    if (s == nullptr) {
        return out;
    }
    const auto id = [](std::uint32_t v) { return static_cast<std::int64_t>(v); };
    out["found"] = true;
    out["shout"] = shout_in;
    out["name"] = s->name() != nullptr ? to_godot(s->name()->str()) : String();
    out["description"] = s->description() != nullptr ? to_godot(s->description()->str()) : String();
    const double mult = actors_.get(actor, vm::av::k_shout_recovery_mult);
    Array words;
    if (s->words() != nullptr) {
        int index = 1;
        for (const auto* w : *s->words()) {
            Dictionary d;
            const auto* word = lookup(m->world_.words_of_power(), w->word());
            d["word"] = id(w->word());
            d["name"] = word != nullptr && word->name() != nullptr ? to_godot(magic::word_spelling(word->name()->str())) : String();
            d["translation"] = word != nullptr && word->translation() != nullptr ? to_godot(word->translation()->str()) : String();
            d["known"] = m->shouts.is_word_known(actor, w->word());
            d["unlocked"] = m->shouts.is_word_unlocked(actor, w->word());
            d["spell"] = id(w->spell());
            d["recovery"] = magic::recovery_seconds(m->world_, shout, index++, mult);
            words.push_back(d);
        }
    }
    out["words"] = words;
    out["usable"] = m->shouts.usable_words(actor, shout);
    out["equipped"] = m->shouts.equipped(actor) == shout;
    return out;
}

godot::PackedInt64Array SkydotPapyrus::shouts_of(std::int64_t actor_in) {
    godot::PackedInt64Array out;
    auto* m = magic_state();
    const auto actor = static_cast<std::uint32_t>(actor_in);
    if (m != nullptr && actors_.is_actor(actor)) {
        for (const auto s : m->shouts.shouts_of(actor)) {
            out.push_back(s);
        }
    }
    return out;
}

std::int64_t SkydotPapyrus::equipped_shout(std::int64_t actor) {
    return static_cast<std::int64_t>(voice_shout(static_cast<std::uint32_t>(actor)));
}

bool SkydotPapyrus::equip_shout(std::int64_t actor, std::int64_t shout) {
    auto* m = magic_state();
    return m != nullptr && m->equip_shout(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(shout));
}

bool SkydotPapyrus::teach_word(std::int64_t actor, std::int64_t word) {
    auto* m = magic_state();
    if (m == nullptr) {
        return false;
    }
    if (static_cast<std::uint32_t>(actor) == k_player_ref) {
        return m->teach_word(static_cast<std::uint32_t>(word));
    }
    return m->shouts.teach_word(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(word));
}

bool SkydotPapyrus::unlock_word(std::int64_t actor, std::int64_t word) {
    auto* m = magic_state();
    return m != nullptr && m->shouts.unlock_word(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(word));
}

Dictionary SkydotPapyrus::unlock_word_with_soul(std::int64_t actor_in, std::int64_t word) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    auto* m = magic_state();
    const auto actor = static_cast<std::uint32_t>(actor_in);
    if (m == nullptr || !actors_.is_actor(actor)) {
        out["reason"] = "no actor";
        return out;
    }
    const double souls = actors_.get(actor, vm::av::k_dragon_souls);
    const auto result = m->shouts.unlock_with_soul(actor, static_cast<std::uint32_t>(word), souls);
    out["ok"] = result.ok;
    if (result.ok) {
        actors_.mod(actor, vm::av::k_dragon_souls, -1.0); // base + what scripts added
    } else if (result.reason == "no dragon soul") {
        out["reason"] = to_godot(m->game_text("sFailSpendSoul", "You do not have a dragon soul to spend."));
    } else {
        out["reason"] = to_godot(result.reason);
    }
    return out;
}

double SkydotPapyrus::voice_recovery(std::int64_t actor) {
    auto* m = magic_state();
    return m != nullptr ? m->shouts.recovery_left(static_cast<std::uint32_t>(actor)) : 0.0;
}

void SkydotPapyrus::set_voice_recovery(std::int64_t actor, double seconds) {
    if (auto* m = magic_state()) {
        m->shouts.set_recovery(static_cast<std::uint32_t>(actor), seconds);
    }
}

Dictionary SkydotPapyrus::cast_shout(std::int64_t actor_in, std::int64_t shout_in, std::int64_t words_in,
                                     std::int64_t target_in) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["words"] = 0;
    out["spell"] = 0;
    out["recovery"] = 0.0;
    auto* m = magic_state();
    const auto actor = static_cast<std::uint32_t>(actor_in);
    if (m == nullptr || !m->shouts.is_shout(static_cast<std::uint32_t>(shout_in))) {
        out["reason"] = "unknown shout";
        return out;
    }
    if (!caster_can_act(actors_, actor, out)) {
        return out;
    }
    m->prime(actor);
    const double mult = actors_.get(actor, vm::av::k_shout_recovery_mult);
    const auto shouted = m->shouts.shout(actor, static_cast<std::uint32_t>(shout_in), static_cast<int>(words_in), mult);
    out["reason"] = to_godot(shouted.reason);
    if (!shouted.ok) {
        return out;
    }
    out["ok"] = true;
    out["words"] = shouted.words;
    out["spell"] = static_cast<std::int64_t>(shouted.spell);
    out["recovery"] = shouted.recovery;
    send_spell_cast(actor, shouted.spell);
    if (target_in != 0) {
        // Voice spells cost nothing: land them as land_spell does.
        const auto target = static_cast<std::uint32_t>(target_in);
        m->prime(target);
        (void)magic::land_form(m->world_, m->effects, actor, shouted.spell, target);
        flush_magic_events();
        flush_deaths();
    }
    return out;
}


} // namespace skydot
