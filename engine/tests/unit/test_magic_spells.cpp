// SPDX-License-Identifier: GPL-3.0-or-later
//
// Spell lists (docs/adr/0008): what an NPC_ and its race give, leveled spell lists resolved by the
// reference, spells added and removed, abilities as constant effects that are never saved,
// equipped spells, power cooldowns, known enchantments and the MAGC section.
#include "support/world_builder.hpp"

#include "data/fb_search.hpp"
#include "magic/magic_save.hpp"
#include "vm/actor_states.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace skydot::testing;
namespace magic = skydot::magic;
namespace vm = skydot::vm;
using Catch::Approx;

namespace {

constexpr std::uint32_t k_player = 0x14;
constexpr std::uint32_t k_mage = 0x15;
constexpr std::uint32_t k_npc = 0x100;       // the player's NPC_
constexpr std::uint32_t k_mage_npc = 0x101;
constexpr std::uint32_t k_race = 0x50;
constexpr std::uint32_t k_fortify = 0x1004;
constexpr std::uint32_t k_flames = 0x2001;
constexpr std::uint32_t k_healing = 0x2002;
constexpr std::uint32_t k_ability = 0x2003;
constexpr std::uint32_t k_power = 0x2004;
constexpr std::uint32_t k_lesser = 0x2005;
constexpr std::uint32_t k_extra = 0x2006;
constexpr std::uint32_t k_list2 = 0x3001;    // an LVSP of a shout
constexpr std::uint32_t k_list = 0x3000;     // an LVSP of Healing and Extra
constexpr std::uint32_t k_shout = 0x9999;    // a SHOU
constexpr std::uint32_t k_shout2 = 0x999A;   // a SHOU only the LVSP names
constexpr std::uint32_t k_shout3 = 0x999B;   // a SHOU of the race

bool fake_source(std::uint32_t ref, vm::ActorBase& out) {
    if (ref != k_player && ref != k_mage) {
        return false;
    }
    out = vm::ActorBase{};
    out.base[vm::av::k_health] = 100;
    return true;
}

struct Host : magic::MagicHost {
    vm::ActorStates states;
    Host() { states.set_source(fake_source); }
    double actor_value(std::uint32_t r, std::uint32_t av) const override { return states.get(r, av); }
    void damage_value(std::uint32_t r, std::uint32_t av, double a, std::uint32_t) override { states.damage(r, av, a); }
    void restore_value(std::uint32_t r, std::uint32_t av, double a) override { states.restore(r, av, a); }
    void modify_temp(std::uint32_t r, std::uint32_t av, double d) override { states.mod_temp(r, av, d); }
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
    MagicEffectSpec fortify{.id = k_fortify};
    fortify.archetype = 34;
    fortify.actor_value = static_cast<std::int32_t>(vm::av::k_health);
    fortify.flags = magic::mgef_flag::recover;
    fortify.casting_type = 0;
    s.magic_effects = {fortify};
    const auto spell = [](std::uint32_t id, std::uint32_t type, std::uint32_t casting) {
        SpellSpec sp{.id = id};
        sp.type = type;
        sp.casting_type = casting;
        sp.effects = {{.effect = k_fortify, .magnitude = 25.0F, .duration = 0}};
        return sp;
    };
    s.spells = {spell(k_flames, 0, 2), spell(k_healing, 0, 2), spell(k_ability, 4, 0), spell(k_power, 2, 1),
                spell(k_lesser, 3, 1), spell(k_extra, 0, 1)};
    s.shouts = {ShoutSpec{.id = k_shout, .name = "Fus", .words = {{0x500, k_flames, 15.0F}}},
                ShoutSpec{.id = k_shout2}, ShoutSpec{.id = k_shout3}};
    s.races = {RaceSpec{.id = k_race, .spells = {k_ability, k_shout3, k_shout}}};
    s.npcs = {NpcSpec{.id = k_npc, .race = k_race, .spells = {k_flames, k_list, k_list2, k_shout}},
              NpcSpec{.id = k_mage_npc, .race = k_race, .spells = {k_power}}};
    s.leveled_lists = {LeveledListSpec{.id = k_list, .entries = {{.level = 1, .form = k_healing}}},
                     LeveledListSpec{.id = k_list2, .entries = {{.level = 1, .form = k_shout2}}}};
    return s;
}

struct Rig {
    BuiltWorld world{spec()};
    Host host;
    magic::ActiveEffects effects{*world, host};
    magic::SpellBook book{*world, effects};
    Rig() {
        book.set_base_source([this](std::uint32_t ref) {
            const std::uint32_t npc = ref == k_player ? k_npc : (ref == k_mage ? k_mage_npc : 0);
            return npc != 0 ? magic::base_spells(*world, npc, ref, 1) : std::vector<std::uint32_t>{};
        });
    }
};

bool has(const std::vector<std::uint32_t>& v, std::uint32_t x) { return std::ranges::find(v, x) != v.end(); }

} // namespace

TEST_CASE("an actor's base spells are its NPC_ list and its race's, lists resolved, shouts dropped", "[magic][spells]") {
    Rig rig;
    const auto spells = magic::base_spells(*rig.world, k_npc, k_player, 1);
    CHECK(spells == std::vector<std::uint32_t>{k_flames, k_healing, k_ability});
    CHECK(magic::base_spells(*rig.world, k_npc, k_player, 1) == spells); // the same every time
    CHECK(magic::base_spells(*rig.world, 0x7777, k_player, 1).empty());
}

TEST_CASE("an actor's base shouts are the SHOUs of the same lists, once each", "[magic][spells]") {
    Rig rig;
    // NPC_: the LVSP's k_shout2, then k_shout; the race adds k_shout3 (k_shout is not repeated).
    CHECK(magic::base_shouts(*rig.world, k_npc, k_player, 1) ==
          std::vector<std::uint32_t>{k_shout2, k_shout, k_shout3});
    CHECK(magic::base_shouts(*rig.world, k_mage_npc, k_mage, 1) ==
          std::vector<std::uint32_t>{k_shout3, k_shout});
    CHECK(magic::base_shouts(*rig.world, 0x7777, k_player, 1).empty());
}

TEST_CASE("added and removed spells change what is known", "[magic][spells]") {
    Rig rig;
    CHECK(rig.book.has_spell(k_player, k_flames));
    CHECK_FALSE(rig.book.has_spell(k_player, k_extra));
    CHECK(rig.book.add_spell(k_player, k_extra));
    CHECK_FALSE(rig.book.add_spell(k_player, k_extra)); // known already
    CHECK_FALSE(rig.book.add_spell(k_player, 0x1234));   // no spell
    CHECK(rig.book.has_spell(k_player, k_extra));
    CHECK(rig.book.remove_spell(k_player, k_flames));
    CHECK_FALSE(rig.book.remove_spell(k_player, k_flames));
    CHECK_FALSE(rig.book.has_spell(k_player, k_flames));
    CHECK(has(rig.book.spells_of(k_player), k_extra));
    CHECK(rig.book.add_spell(k_player, k_flames)); // taught again
    CHECK(rig.book.has_spell(k_player, k_flames));
    CHECK(rig.book.spells_of(k_mage) == std::vector<std::uint32_t>{k_power, k_ability});
}

TEST_CASE("abilities are constant effects while known and are not saved", "[magic][spells]") {
    Rig rig;
    rig.book.ensure_abilities(k_player);
    rig.book.ensure_abilities(k_player); // idempotent
    CHECK(rig.effects.size() == 1);
    CHECK(rig.host.states.max(k_player, vm::av::k_health) == Approx(125));
    // Saved: no instance.
    Rig other;
    REQUIRE(other.effects.load(skydot::save::ByteReader(rig.effects.save())));
    CHECK(other.effects.size() == 0);
    CHECK(other.host.states.max(k_player, vm::av::k_health) == Approx(100));
    // Taught later: starts at once; forgotten: ends.
    rig.book.ensure_abilities(k_mage);
    CHECK(rig.effects.size() == 2);
    CHECK(rig.book.remove_spell(k_mage, k_ability));
    CHECK(rig.effects.size() == 1);
    CHECK(rig.host.states.max(k_mage, vm::av::k_health) == Approx(100));
    CHECK(rig.book.add_spell(k_mage, k_ability));
    CHECK(rig.host.states.max(k_mage, vm::av::k_health) == Approx(125));
}

TEST_CASE("equipped spells: hands for spells, the power slot for powers", "[magic][spells]") {
    Rig rig;
    CHECK(rig.book.equip(k_player, k_flames, magic::Slot::right));
    CHECK(rig.book.equip(k_player, k_healing, magic::Slot::left));
    CHECK(rig.book.equipped(k_player, magic::Slot::right) == k_flames);
    CHECK(rig.book.equipped(k_player, magic::Slot::left) == k_healing);
    CHECK_FALSE(rig.book.equip(k_player, k_extra, magic::Slot::right)); // not known
    CHECK_FALSE(rig.book.equip(k_player, k_flames, magic::Slot::power)); // not a power
    CHECK(rig.book.add_spell(k_player, k_power));
    CHECK_FALSE(rig.book.equip(k_player, k_power, magic::Slot::left));
    CHECK(rig.book.equip(k_player, k_power, magic::Slot::power));
    rig.book.unequip(k_player, magic::Slot::right);
    CHECK(rig.book.equipped(k_player, magic::Slot::right) == 0);
    CHECK(rig.book.remove_spell(k_player, k_healing)); // forgotten spells are put away
    CHECK(rig.book.equipped(k_player, magic::Slot::left) == 0);
}

TEST_CASE("a power may be used once per game day, a lesser power any time", "[magic][spells]") {
    Rig rig;
    CHECK(rig.book.add_spell(k_player, k_power));
    CHECK(rig.book.add_spell(k_player, k_lesser));
    CHECK(rig.book.ready(k_player, k_power, 3.0));
    rig.book.note_used(k_player, k_power, 3.25);
    rig.book.note_used(k_player, k_lesser, 3.25);
    CHECK_FALSE(rig.book.ready(k_player, k_power, 3.5));
    CHECK(rig.book.cooldown_left(k_player, k_power, 3.5) == Approx(0.75));
    CHECK(rig.book.ready(k_player, k_lesser, 3.26));
    CHECK(rig.book.ready(k_player, k_power, 4.25));
    CHECK(rig.book.ready(k_mage, k_power, 3.5)); // each actor has its own
}

TEST_CASE("known enchantments are a set", "[magic][spells]") {
    Rig rig;
    CHECK_FALSE(rig.book.learn_enchantment(0x55)); // no such ENCH in this world
    CHECK_FALSE(rig.book.knows_enchantment(0x55));
}

TEST_CASE("MAGC carries effects, spells, hands, cooldowns", "[magic][spells][save]") {
    Rig rig;
    rig.book.ensure_abilities(k_player);
    rig.book.add_spell(k_player, k_extra);
    rig.book.add_spell(k_player, k_power);
    rig.book.remove_spell(k_player, k_flames);
    rig.book.equip(k_player, k_extra, magic::Slot::left);
    rig.book.note_used(k_player, k_power, 7.5);
    const magic::EffectInput in{k_fortify, 40.0F, 0, 60, -1};
    rig.effects.apply(k_player, k_player, {magic::SourceKind::potion, 0x3EADE, 0}, std::span(&in, 1));
    const auto bytes = magic::write_magc(rig.effects, rig.book);

    magic::MagcParts parts;
    REQUIRE(magic::read_magc(skydot::save::ByteReader(bytes), parts));
    CHECK(magic::check_magc(*rig.world, parts));

    Rig other;
    REQUIRE(magic::SpellBook::load(skydot::save::ByteReader(parts.book), other.book));
    REQUIRE(other.effects.load(skydot::save::ByteReader(parts.effects)));
    other.book.ensure_abilities(k_player);
    CHECK(other.host.states.max(k_player, vm::av::k_health) == Approx(140)); // peak: the potion's 40 beats the ability's 25
}

TEST_CASE("MAGC refuses damage", "[magic][spells][save]") {
    Rig rig;
    rig.book.add_spell(k_player, k_extra);
    auto bytes = rig.book.save();
    Rig other;
    CHECK(magic::SpellBook::load(skydot::save::ByteReader(bytes), other.book));
    CHECK(other.book.has_spell(k_player, k_extra));
    bytes.pop_back();
    CHECK_FALSE(magic::SpellBook::load(skydot::save::ByteReader(bytes), other.book));
    magic::MagcParts parts;
    CHECK_FALSE(magic::read_magc(skydot::save::ByteReader(std::vector<std::uint8_t>{1, 0, 0, 0}), parts));
}
