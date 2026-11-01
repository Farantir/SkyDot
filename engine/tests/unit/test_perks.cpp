// SPDX-License-Identifier: GPL-3.0-or-later
//
// perks::EntryPoints (perks/entry_points.hpp): the value functions, application order, condition tabs
// with a base-form subject, rank chains, the EP* conditions, and the ability and quest effects of
// a perk that is added or removed.
#include "support/fake_condition_world.hpp"
#include "support/world_builder.hpp"

#include "conditions/functions.hpp"
#include "perks/entry_points.hpp"
#include "vm/progression_runtime.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using Catch::Approx;
namespace perks = skydot::perks;
namespace conditions = skydot::conditions;
namespace vm = skydot::vm;
using EP = perks::EntryPoint;
using Fn = perks::Function;

namespace {

constexpr std::uint32_t k_owner = 0x14;
constexpr std::uint32_t k_sword = 0x700;     // a weapon base
constexpr std::uint32_t k_target = 0x701;
constexpr std::uint32_t k_kw_blade = 0x800;
constexpr std::uint32_t k_kw_fire = 0x801;
constexpr std::uint32_t k_flames = 0x900;    // a destruction spell with a fire effect
constexpr std::uint32_t k_heal = 0x901;      // a restoration spell
constexpr std::uint32_t k_fire_effect = 0x910;
constexpr std::uint32_t k_heal_effect = 0x911;
constexpr std::uint32_t k_ability = 0x920;
constexpr std::uint32_t k_quest = 0x930;

PerkEffectSpec entry(EP ep, Fn fn, std::vector<float> values, std::uint8_t priority = 0,
                     std::vector<PerkEffectSpec::Tab> tabs = {}, std::int32_t actor_value = -1) {
    PerkEffectSpec e;
    e.kind = bethconv::pack::wfb::PerkEffectKind::entry_point;
    e.entry_point = static_cast<std::uint8_t>(ep);
    e.function = static_cast<std::uint8_t>(fn);
    e.priority = priority;
    e.values = std::move(values);
    e.tabs = std::move(tabs);
    e.actor_value = actor_value;
    e.data_type = actor_value >= 0 ? 2 : 1;
    return e;
}

PerkEffectSpec::Tab tab(std::uint8_t index, std::vector<ConditionSpec> c) { return {index, std::move(c)}; }

struct Rig {
    explicit Rig(std::vector<PerkSpec> perk_specs, std::vector<std::pair<std::uint32_t, std::uint8_t>> start)
        : world(WorldSpec{
              .magic_effects = {{.id = k_fire_effect, .magic_skill = 20, .keywords = {k_kw_fire}},
                                {.id = k_heal_effect, .magic_skill = 22}},
              .spells = {{.id = k_flames, .effects = {{.effect = k_fire_effect, .magnitude = 8.0F}}},
                         {.id = k_heal, .effects = {{.effect = k_heal_effect, .magnitude = 20.0F}}}},
              .perks = std::move(perk_specs)}),
          owned(std::move(start)) {
        for (const auto& [perk, rank] : this->owned) {
            fake.perk_ranks[{k_owner, perk}] = rank;
        }
    }
    double apply(EP ep, const perks::EntryContext& ctx, double value) {
        return points().apply(ep, ctx, value);
    }
    perks::EntryPoints points() {
        return perks::EntryPoints(*world, fake, [this](std::uint32_t) { return owned; });
    }
    BuiltWorld world;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> owned;
    FakeConditionWorld fake;
};

perks::EntryContext attack(std::uint32_t weapon = k_sword, std::uint32_t target = k_target) {
    perks::EntryContext c;
    c.owner = k_owner;
    c.subjects = {k_owner, weapon, target};
    return c;
}

} // namespace

TEST_CASE("every entry point has its CK name", "[perks]") {
    CHECK(std::string(perks::entry_point_name(EP::ModAttackDamage)) == "Mod Attack Damage");
    CHECK(std::string(perks::entry_point_name(EP::ModSkillUse)) == "Mod Skill Use");
    CHECK(std::string(perks::entry_point_name(EP::GetMaxCarryWeight)) == "Get Max Carry Weight");
    CHECK(std::string(perks::entry_point_name(EP::AllowMountActor)) == "Allow Mount Actor");
    for (std::size_t i = 0; i < perks::k_entry_point_count; ++i) {
        CHECK(std::string(perks::entry_point_name(static_cast<EP>(i))) != "");
    }
    CHECK(std::string(perks::entry_point_name(static_cast<EP>(200))).empty());
}

TEST_CASE("the value functions", "[perks]") {
    const std::vector<float> two{3.0F, 5.0F};
    const std::vector<float> one{1.5F};
    CHECK(perks::EntryPoints::combine(Fn::SetValue, 10.0, one, 0) == Approx(1.5));
    CHECK(perks::EntryPoints::combine(Fn::AddValue, 10.0, one, 0) == Approx(11.5));
    CHECK(perks::EntryPoints::combine(Fn::MultiplyValue, 10.0, one, 0) == Approx(15.0));
    CHECK(perks::EntryPoints::combine(Fn::AddRangeToValue, 10.0, two, 0) == Approx(14.0)); // mean of 3 and 5
    CHECK(perks::EntryPoints::combine(Fn::AddActorValueMult, 10.0, one, 40.0) == Approx(70.0));      // 10 + 40*1.5
    CHECK(perks::EntryPoints::combine(Fn::AbsoluteValue, -4.0, one, 0) == Approx(4.0));
    CHECK(perks::EntryPoints::combine(Fn::NegativeAbsoluteValue, 4.0, one, 0) == Approx(-4.0));
    CHECK(perks::EntryPoints::combine(Fn::SetToActorValueMult, 10.0, one, 40.0) == Approx(60.0));    // 40*1.5
    CHECK(perks::EntryPoints::combine(Fn::MultiplyActorValueMult, 10.0, one, 40.0) == Approx(600.0)); // 10*40*1.5
    CHECK(perks::EntryPoints::combine(Fn::MultiplyOnePlusActorValueMult, 10.0, {0.01F}, 50.0) == Approx(15.0));
    CHECK(perks::EntryPoints::combine(Fn::SelectSpell, 10.0, one, 0) == Approx(10.0)); // not a value function
}

TEST_CASE("effects apply in priority order, ties by perk then record", "[perks]") {
    // 10: *2 at priority 5, then +3 at priority 9 -> 23; set 1 at priority 7 between them -> 1*... see below.
    Rig a({{.id = 0x100, .effects = {entry(EP::ModAttackDamage, Fn::MultiplyValue, {2.0F}, 5),
                                      entry(EP::ModAttackDamage, Fn::AddValue, {3.0F}, 9)}}},
          {{0x100, 1}});
    CHECK(a.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(23.0));
    // A set after a multiply discards it; before the multiply it keeps it.
    Rig after({{.id = 0x100, .effects = {entry(EP::ModAttackDamage, Fn::SetValue, {7.0F}, 9),
                                         entry(EP::ModAttackDamage, Fn::MultiplyValue, {2.0F}, 5)}}},
              {{0x100, 1}});
    CHECK(after.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(7.0));
    Rig before({{.id = 0x100, .effects = {entry(EP::ModAttackDamage, Fn::SetValue, {7.0F}, 1),
                                          entry(EP::ModAttackDamage, Fn::MultiplyValue, {2.0F}, 5)}}},
               {{0x100, 1}});
    CHECK(before.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(14.0));
    // Same priority: the lower perk id first (set 4, then perk 0x101 adds 1 -> 5; not 4 if the set came last).
    Rig tie({{.id = 0x102, .effects = {entry(EP::ModAttackDamage, Fn::SetValue, {4.0F}, 3)}},
             {.id = 0x101, .effects = {entry(EP::ModAttackDamage, Fn::AddValue, {1.0F}, 3)}}},
            {{0x101, 1}, {0x102, 1}});
    CHECK(tie.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(4.0)); // 0x101: 11, then 0x102 sets 4
    // Another entry point is untouched.
    CHECK(tie.apply(EP::ModIncomingDamage, attack(), 10.0) == Approx(10.0));
}

TEST_CASE("the owner's current actor value feeds function 14", "[perks]") {
    // PerkSkillBoosts: * (1 + 0.01 * OneHanded) at Mod Attack Damage.
    Rig r({{.id = 0x100, .effects = {entry(EP::ModAttackDamage, Fn::MultiplyOnePlusActorValueMult, {0.01F}, 20, {}, 6)}}},
          {{0x100, 1}});
    r.fake.values[{k_owner, 6}] = 50.0;
    CHECK(r.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(15.0));
}

TEST_CASE("a failing tab skips the effect, tab t reads subject t", "[perks]") {
    namespace fn = conditions::fn;
    // Armsman-like: x1.2 with a weapon that has the blade keyword (tab 1) and a target that is not the owner (tab 2).
    Rig r({{.id = 0x100,
            .effects = {entry(EP::ModAttackDamage, Fn::MultiplyValue, {1.2F}, 0,
                              {tab(1, {condition(fn::has_keyword, eq, 1.0F, 0, k_kw_blade)}),
                               tab(2, {condition(fn::get_is_id, ne, 1.0F, 0, k_owner)})})}}},
          {{0x100, 1}});
    CHECK(r.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(10.0)); // no keyword
    r.fake.keywords.insert({k_sword, k_kw_blade});
    r.fake.npcs[k_target] = 0x5000;
    CHECK(r.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(12.0));
    CHECK(r.apply(EP::ModAttackDamage, attack(0x999), 10.0) == Approx(10.0)); // another weapon
}

TEST_CASE("a rank chain with HasPerk(next) == 0 applies only the highest rank", "[perks]") {
    namespace fn = conditions::fn;
    const auto lowest_only = [](std::uint32_t next) {
        return std::vector<PerkEffectSpec::Tab>{tab(0, {condition(fn::has_perk, eq, 0.0F, 0, next)})};
    };
    std::vector<PerkSpec> chain = {
        {.id = 0x100, .next_perk = 0x101,
         .effects = {entry(EP::ModAttackDamage, Fn::MultiplyValue, {1.2F}, 0, lowest_only(0x101))}},
        {.id = 0x101, .next_perk = 0x102,
         .effects = {entry(EP::ModAttackDamage, Fn::MultiplyValue, {1.4F}, 0, lowest_only(0x102))}},
        {.id = 0x102, .effects = {entry(EP::ModAttackDamage, Fn::MultiplyValue, {1.6F}, 0, {})}}};
    Rig one(chain, {{0x100, 1}});
    CHECK(one.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(12.0));
    Rig two(chain, {{0x100, 1}, {0x101, 1}});
    CHECK(two.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(14.0));
    Rig three(chain, {{0x100, 1}, {0x101, 1}, {0x102, 1}});
    CHECK(three.apply(EP::ModAttackDamage, attack(), 10.0) == Approx(16.0));
}

TEST_CASE("an effect applies at its own rank", "[perks]") {
    auto second = entry(EP::ModAttackDamage, Fn::AddValue, {5.0F});
    second.rank = 1;
    Rig r({{.id = 0x100, .ranks = 2, .effects = {entry(EP::ModAttackDamage, Fn::AddValue, {1.0F}), second}}},
          {{0x100, 1}});
    CHECK(r.apply(EP::ModAttackDamage, attack(), 0.0) == Approx(1.0));
    r.owned = {{0x100, 2}};
    CHECK(r.apply(EP::ModAttackDamage, attack(), 0.0) == Approx(5.0));
    r.owned.clear();
    CHECK(r.apply(EP::ModAttackDamage, attack(), 0.0) == Approx(0.0));
}

TEST_CASE("EPMagic_SpellHasSkill and SpellHasKeyword read a spell subject", "[perks]") {
    // Mod Spell Cost: * 0.8 for a destruction spell (skill 20), * 0.5 for a spell with a fire effect.
    Rig r({{.id = 0x100,
            .effects = {entry(EP::ModSpellCost, Fn::MultiplyValue, {0.8F}, 1,
                              {tab(1, {condition(696, eq, 1.0F, 0, 20)})}),
                        entry(EP::ModSpellCost, Fn::MultiplyValue, {0.5F}, 2,
                              {tab(1, {condition(693, eq, 1.0F, 0, k_kw_fire)})})}}},
          {{0x100, 1}});
    perks::EntryContext c;
    c.owner = k_owner;
    c.subjects = {k_owner, k_flames, 0};
    CHECK(r.apply(EP::ModSpellCost, c, 100.0) == Approx(40.0));
    c.subjects[1] = k_heal;
    CHECK(r.apply(EP::ModSpellCost, c, 100.0) == Approx(100.0));
    // HasKeyword on a spell also looks at its effects.
    Rig k({{.id = 0x100, .effects = {entry(EP::ModSpellCost, Fn::AddValue, {1.0F}, 0,
                                           {tab(1, {condition(conditions::fn::has_keyword, eq, 1.0F, 0, k_kw_fire)})})}}},
          {{0x100, 1}});
    c.subjects[1] = k_flames;
    CHECK(k.apply(EP::ModSpellCost, c, 0.0) == Approx(1.0));
}

TEST_CASE("the context answers the skill-use, worn and attack-type conditions", "[perks]") {
    Rig r({{.id = 0x100,
            .effects = {entry(EP::ModSkillUse, Fn::MultiplyValue, {1.5F}, 0, {tab(0, {condition(681, eq, 1.0F, 0, 14)})}),
                        entry(EP::ModArmorRating, Fn::MultiplyValue, {2.0F}, 0,
                              {tab(0, {condition(722, ge, 2.0F, 0, k_kw_blade)})}),
                        entry(EP::ModAttackDamage, Fn::AddValue, {10.0F}, 0, {tab(0, {condition(697, eq, 1.0F, 0, 0xA00)})})}}},
          {{0x100, 1}});
    perks::EntryContext c;
    c.owner = k_owner;
    c.advance_skill = 14;
    CHECK(r.apply(EP::ModSkillUse, c, 2.0) == Approx(3.0));
    c.advance_skill = 15;
    CHECK(r.apply(EP::ModSkillUse, c, 2.0) == Approx(2.0));
    r.fake.keywords.insert({0x601, k_kw_blade});
    r.fake.keywords.insert({0x602, k_kw_blade});
    c.worn = {0x601};
    CHECK(r.apply(EP::ModArmorRating, c, 100.0) == Approx(100.0));
    c.worn = {0x601, 0x602, 0x603};
    CHECK(r.apply(EP::ModArmorRating, c, 100.0) == Approx(200.0));
    CHECK(r.apply(EP::ModAttackDamage, c, 1.0) == Approx(1.0));
    c.attack_keywords = {0xA00};
    CHECK(r.apply(EP::ModAttackDamage, c, 1.0) == Approx(11.0));
}

TEST_CASE("Select Spell and Select Text list their data", "[perks]") {
    PerkEffectSpec spell = entry(EP::ApplyCombatHitSpell, Fn::SelectSpell, {}, 0);
    spell.data_type = 5;
    spell.form = k_flames;
    PerkEffectSpec text = entry(EP::SetActivateLabel, Fn::SetText, {}, 0);
    text.data_type = 7;
    text.text = "Steal";
    Rig r({{.id = 0x100, .effects = {spell, text}}}, {{0x100, 1}});
    CHECK(r.points().select_spells(EP::ApplyCombatHitSpell, attack()) == std::vector<std::uint32_t>{k_flames});
    CHECK(r.points().select_text(EP::SetActivateLabel, attack()) == std::vector<std::string>{"Steal"});
    CHECK(r.points().select_spells(EP::ModSpellCost, attack()).empty());
}

namespace {

struct Log final : perks::EffectSink {
    std::vector<std::string> lines;
    void add_ability(std::uint32_t a, std::uint32_t s) override { lines.push_back("add " + std::to_string(a) + " " + std::to_string(s)); }
    void remove_ability(std::uint32_t a, std::uint32_t s) override { lines.push_back("remove " + std::to_string(a) + " " + std::to_string(s)); }
    void set_stage(std::uint32_t q, std::uint16_t st) override { lines.push_back("stage " + std::to_string(q) + " " + std::to_string(st)); }
};

PerkSpec effects_perk() {
    PerkEffectSpec ability;
    ability.kind = bethconv::pack::wfb::PerkEffectKind::ability;
    ability.spell = k_ability;
    PerkEffectSpec quest;
    quest.kind = bethconv::pack::wfb::PerkEffectKind::quest;
    quest.quest = k_quest;
    quest.stage = 20;
    return {.id = 0x100, .effects = {ability, quest}};
}

/// A progression runtime whose host carries a perk's effects out into a log.
class EffectHost final : public vm::ProgressionRuntime::Host {
public:
    EffectHost() : world(WorldSpec{.npcs = {{.id = 0x7}}, .perks = {effects_perk()}}) {
        runtime.bind(*this);
        runtime.clear();
    }
    const bethconv::pack::wfb::World* world_root() const override { return &*world; }
    std::uint32_t base_of(std::uint32_t ref) const override { return ref == 0x14 ? 0x7 : 0; }
    void perk_gained(std::uint32_t ref, std::uint32_t perk) override { perks::perk_added(*world, ref, perk, sink); }
    void perk_lost(std::uint32_t ref, std::uint32_t perk) override { perks::perk_removed(*world, ref, perk, sink); }
    BuiltWorld world;
    vm::ProgressionRuntime runtime;
    Log sink;
};

} // namespace

TEST_CASE("adding a perk adds its ability and sets its quest stage, removing it removes the ability", "[perks]") {
    EffectHost h;
    h.runtime.add_perk(0x14, 0x100);
    CHECK(h.sink.lines == std::vector<std::string>{"add 20 " + std::to_string(k_ability), "stage " + std::to_string(k_quest) + " 20"});
    h.sink.lines.clear();
    h.runtime.add_perk(0x14, 0x100); // owned already: nothing again
    h.runtime.add_perk(0x14, 0x100, 2);
    CHECK(h.sink.lines.empty());
    h.runtime.remove_perk(0x14, 0x100);
    CHECK(h.sink.lines == std::vector<std::string>{"remove 20 " + std::to_string(k_ability)});
    h.sink.lines.clear();
    h.runtime.remove_perk(0x14, 0x100); // gone already
    CHECK(h.sink.lines.empty());
}

TEST_CASE("an NPC's perk adds its ability but sets no quest stage", "[perks]") {
    EffectHost h;
    h.runtime.add_perk(0x1234, 0x100);
    CHECK(h.sink.lines == std::vector<std::string>{"add " + std::to_string(0x1234) + " " + std::to_string(k_ability)});
}
