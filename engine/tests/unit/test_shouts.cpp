// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shouts (docs/adr/0008, "Shouts"): hold time to words, the words the player can speak, recovery,
// spending a dragon soul, the voice slot shared with powers and the MAGC section (versions 1 and 2).
#include "support/world_builder.hpp"

#include "magic/magic_save.hpp"
#include "magic/shouts.hpp"
#include "vm/actor_states.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
namespace magic = skydot::magic;
namespace vm = skydot::vm;
using Catch::Approx;

namespace {

constexpr std::uint32_t k_player = 0x14;
constexpr std::uint32_t k_dragon = 0x15;
constexpr std::uint32_t k_unrelenting = 0x13E07;
constexpr std::uint32_t k_other_shout = 0x13E08;
constexpr std::uint32_t k_fus = 0x13E22;
constexpr std::uint32_t k_ro = 0x13E23;
constexpr std::uint32_t k_dah = 0x13E24;
constexpr std::uint32_t k_wuld = 0x13E30;
constexpr std::uint32_t k_spell1 = 0x13E09;
constexpr std::uint32_t k_spell2 = 0x13F39;
constexpr std::uint32_t k_spell3 = 0x13F3A;
constexpr std::uint32_t k_power = 0x2004;

class Silent final : public magic::MagicHost {
public:
    double actor_value(std::uint32_t, std::uint32_t) const override { return 0.0; }
    void damage_value(std::uint32_t, std::uint32_t, double, std::uint32_t) override {}
    void restore_value(std::uint32_t, std::uint32_t, double) override {}
    void modify_temp(std::uint32_t, std::uint32_t, double) override {}
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<magic::wfb::Condition>>*, std::uint32_t,
                         std::uint32_t) override {
        return true;
    }
    bool is_dead(std::uint32_t) const override { return false; }
    void log(const std::string&) override {}
};

WorldSpec spec() {
    WorldSpec s;
    s.format_version = 36;
    SpellSpec power{.id = k_power};
    power.type = 2;
    power.casting_type = 1;
    s.spells = {SpellSpec{.id = k_spell1}, SpellSpec{.id = k_spell2}, SpellSpec{.id = k_spell3}, power};
    s.words_of_power = {WordSpec{.id = k_fus, .name = "Fus", .translation = "Force"},
                        WordSpec{.id = k_ro, .name = "Ro", .translation = "Balance"},
                        WordSpec{.id = k_dah, .name = "Dah", .translation = "Push"},
                        WordSpec{.id = k_wuld, .name = "Wuld", .translation = "Whirlwind"}};
    s.shouts = {ShoutSpec{.id = k_unrelenting,
                          .name = "Unrelenting Force",
                          .words = {{k_fus, k_spell1, 15.0F}, {k_ro, k_spell2, 20.0F}, {k_dah, k_spell3, 45.0F}}},
                ShoutSpec{.id = k_other_shout, .name = "Whirlwind", .words = {{k_wuld, k_spell1, 10.0F}}}};
    return s;
}

struct Rig {
    BuiltWorld world{spec()};
    Silent host;
    magic::ActiveEffects effects{*world, host};
    magic::SpellBook spells{*world, effects};
    magic::ShoutBook book{*world};
    magic::ShoutSettings settings;
    Rig() {
        // The dragon (0x15) has Unrelenting Force from its NPC_.
        book.set_base_source([](std::uint32_t ref) {
            return ref == k_dragon ? std::vector<std::uint32_t>{k_unrelenting} : std::vector<std::uint32_t>{};
        });
    }
    /// The player knows and has unlocked the given words.
    void learn(std::initializer_list<std::uint32_t> known, std::initializer_list<std::uint32_t> unlocked) {
        for (const auto w : known) {
            REQUIRE(book.teach_word(k_player, w));
        }
        for (const auto w : unlocked) {
            REQUIRE(book.unlock_word(k_player, w));
        }
    }
};

} // namespace

TEST_CASE("a held shout button speaks one, two or three words", "[shouts]") {
    const magic::ShoutSettings s;
    CHECK(magic::words_for_hold(0.0, s) == 1);
    CHECK(magic::words_for_hold(0.1, s) == 1);
    CHECK(magic::words_for_hold(0.2, s) == 2); // exactly fShoutTime1 is already two
    CHECK(magic::words_for_hold(0.5, s) == 2);
    CHECK(magic::words_for_hold(0.9, s) == 3);
    CHECK(magic::words_for_hold(1.0, s) == 3);
    CHECK(magic::words_for_hold(0.5, {.shout_time1 = 0.6, .shout_time2 = 1.0}) == 1);
}

TEST_CASE("recovery is the last word's time times the multiplier", "[shouts]") {
    Rig rig;
    CHECK(magic::recovery_seconds(*rig.world, k_unrelenting, 1, 1.0) == Approx(15));
    CHECK(magic::recovery_seconds(*rig.world, k_unrelenting, 2, 1.0) == Approx(20));
    CHECK(magic::recovery_seconds(*rig.world, k_unrelenting, 3, 0.5) == Approx(22.5));
    CHECK(magic::recovery_seconds(*rig.world, k_unrelenting, 3, -2.0) == Approx(0));
    CHECK(magic::recovery_seconds(*rig.world, k_unrelenting, 4, 1.0) == Approx(0));
    CHECK(magic::recovery_seconds(*rig.world, 0x9999, 1, 1.0) == Approx(0));
}

TEST_CASE("the player speaks the leading run of known and unlocked words, others all of them", "[shouts]") {
    Rig rig;
    CHECK(rig.book.usable_words(k_player, k_unrelenting) == 0);
    rig.learn({k_fus, k_ro, k_dah}, {k_fus, k_dah}); // Ro is locked
    CHECK(rig.book.usable_words(k_player, k_unrelenting) == 1);
    REQUIRE(rig.book.unlock_word(k_player, k_ro));
    CHECK(rig.book.usable_words(k_player, k_unrelenting) == 3);
    CHECK(rig.book.usable_words(k_dragon, k_unrelenting) == 3);
    CHECK(rig.book.usable_words(k_dragon, k_other_shout) == 1);
    CHECK(rig.book.usable_words(k_dragon, 0x1234) == 0);
    // Unlocked without being known does not count (the leading word is the gap).
    Rig other;
    other.learn({k_ro}, {k_fus, k_ro});
    CHECK(other.book.usable_words(k_player, k_unrelenting) == 0);
}

TEST_CASE("the player has the shouts of the words it knows, unless removed", "[shouts]") {
    Rig rig;
    CHECK(rig.book.shouts_of(k_player).empty());
    rig.learn({k_ro}, {});
    CHECK(rig.book.shouts_of(k_player) == std::vector<std::uint32_t>{k_unrelenting});
    CHECK(rig.book.has_shout(k_player, k_unrelenting));
    CHECK(rig.book.remove_shout(k_player, k_unrelenting));
    CHECK_FALSE(rig.book.has_shout(k_player, k_unrelenting));
    CHECK_FALSE(rig.book.remove_shout(k_player, k_unrelenting));
    CHECK(rig.book.add_shout(k_player, k_unrelenting));
    CHECK_FALSE(rig.book.add_shout(k_player, k_unrelenting));
    CHECK_FALSE(rig.book.add_shout(k_player, 0x4444)); // no SHOU
    // A dragon: base + added - removed.
    CHECK(rig.book.shouts_of(k_dragon) == std::vector<std::uint32_t>{k_unrelenting});
    CHECK(rig.book.add_shout(k_dragon, k_other_shout));
    CHECK(rig.book.remove_shout(k_dragon, k_unrelenting));
    CHECK(rig.book.shouts_of(k_dragon) == std::vector<std::uint32_t>{k_other_shout});
}

TEST_CASE("words: teach and unlock need real words; unlock needs no knowledge", "[shouts]") {
    Rig rig;
    CHECK_FALSE(rig.book.teach_word(k_player, 0x1234));
    CHECK(rig.book.unlock_word(k_player, k_fus));
    CHECK_FALSE(rig.book.unlock_word(k_player, k_fus));
    CHECK(rig.book.is_word_unlocked(k_player, k_fus));
    CHECK_FALSE(rig.book.is_word_known(k_player, k_fus));
    CHECK(rig.book.teach_word(k_player, k_fus));
    CHECK_FALSE(rig.book.teach_word(k_player, k_fus));
    CHECK(rig.book.is_word_known(k_player, k_fus));
    CHECK_FALSE(rig.book.is_word_known(k_dragon, k_fus));
}

TEST_CASE("a dragon soul unlocks a known, locked word", "[shouts]") {
    Rig rig;
    auto r = rig.book.unlock_with_soul(k_player, k_fus, 3);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "unknown word");
    rig.learn({k_fus}, {});
    r = rig.book.unlock_with_soul(k_player, k_fus, 0);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "no dragon soul");
    CHECK_FALSE(rig.book.is_word_unlocked(k_player, k_fus));
    r = rig.book.unlock_with_soul(k_player, k_fus, 1);
    CHECK(r.ok);
    CHECK(rig.book.is_word_unlocked(k_player, k_fus));
    r = rig.book.unlock_with_soul(k_player, k_fus, 1);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "already unlocked");
}

TEST_CASE("a shout speaks as many words as held and usable, casts the last one's spell and recovers", "[shouts]") {
    Rig rig;
    rig.learn({k_fus, k_ro}, {k_fus, k_ro});
    auto r = rig.book.shout(k_player, k_unrelenting, 1.0, rig.settings, 0.5); // three held, two usable
    REQUIRE(r.ok);
    CHECK(r.words == 2);
    CHECK(r.spell == k_spell2);
    CHECK(r.recovery == Approx(10)); // 20 * 0.5
    CHECK(rig.book.recovery_left(k_player) == Approx(10));
    CHECK_FALSE(rig.book.ready(k_player));

    r = rig.book.shout(k_player, k_unrelenting, 0.0, rig.settings, 1.0);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "recovering");
    rig.book.tick(4.0);
    CHECK(rig.book.recovery_left(k_player) == Approx(6));
    rig.book.tick(7.0);
    CHECK(rig.book.recovery_left(k_player) == Approx(0));
    CHECK(rig.book.ready(k_player));

    r = rig.book.shout(k_player, k_unrelenting, 0.0, rig.settings, 1.0);
    REQUIRE(r.ok);
    CHECK(r.words == 1);
    CHECK(r.spell == k_spell1);
    CHECK(r.recovery == Approx(15));
}

TEST_CASE("a shout is refused without a usable word or a shout", "[shouts]") {
    Rig rig;
    rig.learn({k_ro, k_dah}, {k_ro, k_dah}); // no Fus: the player has the shout, speaks nothing
    auto r = rig.book.shout(k_player, k_unrelenting, 1.0, rig.settings, 1.0);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "no word");
    CHECK(rig.book.recovery_left(k_player) == Approx(0));
    r = rig.book.shout(k_player, k_other_shout, 1.0, rig.settings, 1.0);
    CHECK(r.reason == "unknown shout");
    r = rig.book.shout(k_dragon, k_unrelenting, 1.0, rig.settings, 1.0);
    REQUIRE(r.ok);
    CHECK(r.words == 3);
    CHECK(r.spell == k_spell3);
    CHECK(r.recovery == Approx(45));
}

TEST_CASE("the voice slot holds a shout or a power, never both", "[shouts]") {
    Rig rig;
    REQUIRE(rig.spells.add_spell(k_player, k_power));
    CHECK_FALSE(magic::equip_voice(rig.spells, rig.book, k_player, k_unrelenting)); // not the player's shout yet
    rig.learn({k_fus}, {});
    CHECK(magic::equip_voice(rig.spells, rig.book, k_player, k_power));
    CHECK(rig.spells.equipped(k_player, magic::Slot::power) == k_power);
    CHECK(rig.book.equipped(k_player) == 0);

    CHECK(magic::equip_voice(rig.spells, rig.book, k_player, k_unrelenting));
    CHECK(rig.book.equipped(k_player) == k_unrelenting);
    CHECK(rig.spells.equipped(k_player, magic::Slot::power) == 0);

    CHECK(magic::equip_voice(rig.spells, rig.book, k_player, k_power));
    CHECK(rig.book.equipped(k_player) == 0);
    CHECK(rig.spells.equipped(k_player, magic::Slot::power) == k_power);

    // Removing the equipped shout empties the slot.
    REQUIRE(magic::equip_voice(rig.spells, rig.book, k_player, k_unrelenting));
    CHECK(rig.book.remove_shout(k_player, k_unrelenting));
    CHECK(rig.book.equipped(k_player) == 0);
    CHECK_FALSE(rig.book.equip(k_player, k_unrelenting));
}

TEST_CASE("the ShoutBook survives a save and refuses damage", "[shouts][save]") {
    Rig rig;
    rig.learn({k_fus, k_ro}, {k_fus});
    REQUIRE(rig.book.add_shout(k_player, k_other_shout));
    REQUIRE(rig.book.remove_shout(k_dragon, k_unrelenting));
    REQUIRE(rig.book.equip(k_player, k_unrelenting));
    rig.book.set_recovery(k_player, 12.5);
    auto bytes = rig.book.save();

    Rig other;
    REQUIRE(magic::ShoutBook::load(skydot::save::ByteReader(bytes), other.book));
    CHECK(other.book.is_word_known(k_player, k_ro));
    CHECK(other.book.is_word_unlocked(k_player, k_fus));
    CHECK_FALSE(other.book.is_word_unlocked(k_player, k_ro));
    CHECK(other.book.has_shout(k_player, k_other_shout));
    CHECK_FALSE(other.book.has_shout(k_dragon, k_unrelenting));
    CHECK(other.book.equipped(k_player) == k_unrelenting);
    CHECK(other.book.recovery_left(k_player) == Approx(12.5));

    Rig third;
    third.book.adopt(std::move(other.book));
    CHECK(third.book.recovery_left(k_player) == Approx(12.5));
    third.book.clear();
    CHECK(third.book.refs().empty());

    bytes.pop_back();
    CHECK_FALSE(magic::ShoutBook::load(skydot::save::ByteReader(bytes), third.book));
}

TEST_CASE("MAGC version 2 carries the ShoutBook and version 1 still reads", "[shouts][save]") {
    Rig rig;
    rig.learn({k_fus}, {k_fus});
    const auto bytes = magic::write_magc(rig.effects, rig.spells, rig.book);
    REQUIRE(bytes.size() > 4);
    CHECK(bytes[0] == 2);
    magic::MagcParts parts;
    REQUIRE(magic::read_magc(skydot::save::ByteReader(bytes), parts));
    CHECK_FALSE(parts.shouts.empty());
    CHECK(magic::check_magc(*rig.world, parts));
    Rig other;
    REQUIRE(magic::ShoutBook::load(skydot::save::ByteReader(parts.shouts), other.book));
    CHECK(other.book.is_word_unlocked(k_player, k_fus));

    // The layout of version 1: u32 1, then the effects and the spell book as texts.
    skydot::save::ByteWriter w;
    w.u32(1);
    const auto e = rig.effects.save();
    const auto b = rig.spells.save();
    w.text(std::string_view(reinterpret_cast<const char*>(e.data()), e.size()));
    w.text(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
    magic::MagcParts old;
    REQUIRE(magic::read_magc(skydot::save::ByteReader(w.out), old));
    CHECK(old.shouts.empty());
    CHECK(magic::check_magc(*rig.world, old));
    CHECK(old.effects == parts.effects);
    CHECK(old.book == parts.book);

    // A version 3 and a cut version 2 are refused.
    auto future = bytes;
    future[0] = 3;
    CHECK_FALSE(magic::read_magc(skydot::save::ByteReader(future), parts));
    auto cut = bytes;
    cut.pop_back();
    CHECK_FALSE(magic::read_magc(skydot::save::ByteReader(cut), parts));
    // Damage in the shout part is found by the check.
    magic::MagcParts damaged = parts;
    damaged.shouts.pop_back();
    CHECK_FALSE(magic::check_magc(*rig.world, damaged));
}

TEST_CASE("word_spelling gives the dragon font's digits their vowels", "[shouts]") {
    CHECK(magic::word_spelling("D4") == "Dah");
    CHECK(magic::word_spelling("F1s") == "Faas");
    CHECK(magic::word_spelling("F2m") == "Feim");
    CHECK(magic::word_spelling("Z3") == "Zii");
    CHECK(magic::word_spelling("K8r") == "Koor");
    CHECK(magic::word_spelling("Fus") == "Fus");
}
