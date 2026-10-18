// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Papyrus natives of magic (vm/natives_magic.hpp) over a fake MagicApi: Actor.AddSpell and its
// kin, Spell.Cast and the cost natives, MagicEffect's data, ActiveMagicEffect's object.
#include "support/vm_rig.hpp"
#include "support/world_builder.hpp"

#include "vm/natives_magic.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>

using namespace skydot::testing;
namespace vm = skydot::vm;
using Catch::Approx;

namespace {

constexpr std::uint32_t k_hero = 0x14;
constexpr std::uint32_t k_guard = 0x100;
constexpr std::uint32_t k_flames = 0x2001;
constexpr std::uint32_t k_fire = 0x1003;
constexpr std::uint32_t k_handle = 0xFFF00005;

struct FakeApi final : vm::MagicApi {
    struct Cast {
        std::uint32_t spell, caster, blame, target;
    };
    std::vector<Cast> casts;
    std::map<std::uint32_t, std::set<std::uint32_t>> known;
    std::map<std::pair<std::uint32_t, int>, std::uint32_t> hands;
    std::vector<std::uint32_t> dispelled;
    bool verbose_seen = false;

    bool cast(std::uint32_t spell, std::uint32_t caster, std::uint32_t blame, std::uint32_t target) override {
        casts.push_back({spell, caster, blame, target});
        return true;
    }
    double spell_cost(std::uint32_t, std::uint32_t caster) override { return caster == 0 ? 14.0 : 12.9; }
    bool add_spell(std::uint32_t actor, std::uint32_t spell, bool verbose) override {
        verbose_seen = verbose;
        return known[actor].insert(spell).second;
    }
    bool remove_spell(std::uint32_t actor, std::uint32_t spell) override { return known[actor].erase(spell) != 0; }
    bool has_spell(std::uint32_t actor, std::uint32_t spell) override { return known[actor].contains(spell); }
    bool dispel_spell(std::uint32_t, std::uint32_t spell) override {
        dispelled.push_back(spell);
        return true;
    }
    void dispel_all_spells(std::uint32_t actor) override { dispelled.push_back(actor); }
    bool has_magic_effect(std::uint32_t, std::uint32_t mgef) override { return mgef == k_fire; }
    bool has_magic_effect_keyword(std::uint32_t, std::uint32_t keyword) override { return keyword == 0x77; }
    bool equip_spell(std::uint32_t actor, std::uint32_t spell, int hand) override {
        hands[{actor, hand}] = spell;
        return true;
    }
    void unequip_spell(std::uint32_t actor, int hand) override { hands.erase({actor, hand}); }
    std::uint32_t equipped_spell(std::uint32_t actor, int hand) override {
        const auto it = hands.find({actor, hand});
        return it != hands.end() ? it->second : 0;
    }
    std::optional<Effect> effect(std::uint32_t handle) override {
        if (handle != k_handle) {
            return std::nullopt;
        }
        return Effect{k_fire, k_guard, k_hero, 7.5, 30.0, 12.9};
    }
    void dispel_effect(std::uint32_t handle) override { dispelled.push_back(handle); }
};

WorldSpec spec() {
    WorldSpec s;
    s.format_version = 33;
    MagicEffectSpec fire{.id = k_fire};
    fire.magic_skill = 20;
    fire.resist_value = 41;
    fire.casting_type = 2;
    fire.delivery = 2;
    fire.base_cost = 2.5F;
    s.magic_effects = {fire};
    SpellSpec flames{.id = k_flames, .name = "Flames", .cost = 14, .casting_type = 2, .delivery = 2};
    flames.effects = {{.effect = k_fire, .magnitude = 8.0F, .area = 3, .duration = 1}};
    s.spells = {flames};
    return s;
}

struct Fixture {
    BuiltWorld world{spec()};
    FakeApi api;
    Rig rig;

    Fixture() {
        rig.add({.name = "Form"});
        rig.add({.name = "Keyword", .parent = "Form"});
        rig.add({.name = "Actor",
                 .functions = {native_function("AddSpell", {{"s", "Spell"}, {"v", "Bool"}}, "Bool", false),
                               native_function("RemoveSpell", {{"s", "Spell"}}, "Bool", false),
                               native_function("HasSpell", {{"s", "Form"}}, "Bool", false),
                               native_function("DispelSpell", {{"s", "Spell"}}, "Bool", false),
                               native_function("DispelAllSpells", {}, "None", false),
                               native_function("HasMagicEffect", {{"e", "MagicEffect"}}, "Bool", false),
                               native_function("HasMagicEffectWithKeyword", {{"k", "Keyword"}}, "Bool", false),
                               native_function("DoCombatSpellApply", {{"s", "Spell"}, {"t", "ObjectReference"}}, "None", false),
                               native_function("EquipSpell", {{"s", "Spell"}, {"h", "Int"}}, "None", false),
                               native_function("UnequipSpell", {{"s", "Spell"}, {"h", "Int"}}, "None", false),
                               native_function("GetEquippedSpell", {{"h", "Int"}}, "Spell", false)}});
        rig.add({.name = "Spell", .parent = "Form",
                 .functions = {native_function("Cast", {{"s", "ObjectReference"}, {"t", "ObjectReference"}}, "None", false),
                               native_function("RemoteCast", {{"s", "ObjectReference"}, {"b", "Actor"}, {"t", "ObjectReference"}}, "None", false),
                               native_function("GetMagickaCost", {}, "Int", false),
                               native_function("GetEffectiveMagickaCost", {{"c", "Actor"}}, "Int", false),
                               native_function("GetCastTime", {}, "Float", false),
                               native_function("GetNumEffects", {}, "Int", false),
                               native_function("GetNthEffectMagnitude", {{"i", "Int"}}, "Float", false),
                               native_function("GetNthEffectArea", {{"i", "Int"}}, "Int", false),
                               native_function("GetNthEffectDuration", {{"i", "Int"}}, "Int", false),
                               native_function("GetNthEffectMagicEffect", {{"i", "Int"}}, "MagicEffect", false),
                               native_function("IsHostile", {}, "Bool", false)}});
        rig.add({.name = "MagicEffect", .parent = "Form",
                 .functions = {native_function("GetAssociatedSkill", {}, "String", false),
                               native_function("GetResistance", {}, "String", false),
                               native_function("GetCastingType", {}, "Int", false),
                               native_function("GetDeliveryType", {}, "Int", false),
                               native_function("GetBaseCost", {}, "Float", false),
                               native_function("IsEffectFlagSet", {{"f", "Int"}}, "Bool", false)}});
        rig.add({.name = "ActiveMagicEffect",
                 .functions = {native_function("GetTargetActor", {}, "Actor", false),
                               native_function("GetCasterActor", {}, "Actor", false),
                               native_function("GetBaseObject", {}, "MagicEffect", false),
                               native_function("Dispel", {}, "None", false),
                               native_function("GetMagnitude", {}, "Float", false),
                               native_function("GetDuration", {}, "Int", false),
                               native_function("GetTimeElapsed", {}, "Int", false)}});
        vm::bind_spell_natives(rig.vm, *world, api);
        vm::bind_magiceffect_natives(rig.vm, *world);
        vm::bind_activemagiceffect_natives(rig.vm, api);
        vm::bind_actor_magic_natives(rig.vm, api);
    }
};

/// A global function `name(self: cls, args...)` that calls `fn` on self, returning `ret`.
FunctionSpec caller(const std::string& name, const std::string& cls, const std::string& fn,
                    std::vector<Param> args, const std::string& ret) {
    std::vector<Param> params{{"self", cls}};
    params.insert(params.end(), args.begin(), args.end());
    const bool none = ret == "None";
    std::vector<Arg> ops{ident(fn), ident("self"), ident(none ? "::NoneVar" : "r")};
    for (const auto& a : args) {
        ops.push_back(ident(a.name));
    }
    if (none) {
        auto f = global_function(name, params);
        f.add(vm::Op::callmethod, ops).add(vm::Op::return_, {ident("::NoneVar")});
        return f;
    }
    const std::vector<Param> locals{{"r", ret}};
    auto f = global_function(name, params, locals, ret);
    f.add(vm::Op::callmethod, ops).add(vm::Op::return_, {ident("r")});
    return f;
}

} // namespace

TEST_CASE("Actor.AddSpell, HasSpell and RemoveSpell round trip", "[magic][natives]") {
    Fixture f;
    f.rig.add(script("Caller", {caller("add", "Actor", "AddSpell", {{"s", "Spell"}, {"v", "Bool"}}, "Bool"),
                                caller("has", "Actor", "HasSpell", {{"s", "Form"}}, "Bool"),
                                caller("remove", "Actor", "RemoveSpell", {{"s", "Spell"}}, "Bool")}));
    const auto hero = f.rig.vm.object(k_hero, "Actor");
    const auto flames = f.rig.vm.object(k_flames, "Spell");
    CHECK_FALSE(f.rig.call("caller", "has", {hero, flames}).b());
    CHECK(f.rig.call("caller", "add", {hero, flames, Bool(true)}).b());
    CHECK(f.api.verbose_seen);
    CHECK_FALSE(f.rig.call("caller", "add", {hero, flames, Bool(false)}).b());
    CHECK_FALSE(f.api.verbose_seen);
    CHECK(f.rig.call("caller", "has", {hero, flames}).b());
    CHECK(f.rig.call("caller", "remove", {hero, flames}).b());
    CHECK_FALSE(f.rig.call("caller", "has", {hero, flames}).b());
    for (const auto& line : f.rig.log) {
        INFO(line);
    }
    CHECK(f.rig.vm.errors() == 0);
}

TEST_CASE("Spell.Cast and RemoteCast pass source, blame and target", "[magic][natives]") {
    Fixture f;
    f.rig.add(script("Caller", {caller("cast", "Spell", "Cast", {{"s", "ObjectReference"}, {"t", "ObjectReference"}}, "None"),
                                caller("remote", "Spell", "RemoteCast",
                                       {{"s", "ObjectReference"}, {"b", "Actor"}, {"t", "ObjectReference"}}, "None"),
                                caller("combat", "Actor", "DoCombatSpellApply", {{"s", "Spell"}, {"t", "ObjectReference"}}, "None")}));
    const auto hero = f.rig.vm.object(k_hero, "ObjectReference");
    const auto guard = f.rig.vm.object(k_guard, "ObjectReference");
    const auto flames = f.rig.vm.object(k_flames, "Spell");
    f.rig.call("caller", "cast", {flames, hero, vm::Value{}});
    f.rig.call("caller", "cast", {flames, hero, guard});
    f.rig.call("caller", "cast", {flames, vm::Value{}, guard}); // no source: nothing
    f.rig.call("caller", "remote", {flames, hero, f.rig.vm.object(k_guard, "Actor"), vm::Value{}});
    f.rig.call("caller", "combat", {f.rig.vm.object(k_guard, "Actor"), flames, hero});
    REQUIRE(f.api.casts.size() == 4);
    CHECK((f.api.casts[0].caster == k_hero && f.api.casts[0].target == 0 && f.api.casts[0].blame == 0));
    CHECK(f.api.casts[1].target == k_guard);
    CHECK((f.api.casts[2].caster == k_hero && f.api.casts[2].blame == k_guard));
    CHECK((f.api.casts[3].caster == k_guard && f.api.casts[3].target == k_hero && f.api.casts[3].spell == k_flames));
}

TEST_CASE("Spell reads its data and costs", "[magic][natives]") {
    Fixture f;
    f.rig.add(script("Caller", {caller("cost", "Spell", "GetMagickaCost", {}, "Int"),
                                caller("effective", "Spell", "GetEffectiveMagickaCost", {{"c", "Actor"}}, "Int"),
                                caller("count", "Spell", "GetNumEffects", {}, "Int"),
                                caller("magnitude", "Spell", "GetNthEffectMagnitude", {{"i", "Int"}}, "Float"),
                                caller("area", "Spell", "GetNthEffectArea", {{"i", "Int"}}, "Int"),
                                caller("hostile", "Spell", "IsHostile", {}, "Bool")}));
    const auto flames = f.rig.vm.object(k_flames, "Spell");
    CHECK(f.rig.call("caller", "cost", {flames}).i() == 14);
    CHECK(f.rig.call("caller", "effective", {flames, f.rig.vm.object(k_hero, "Actor")}).i() == 12);
    CHECK(f.rig.call("caller", "count", {flames}).i() == 1);
    CHECK(f.rig.call("caller", "magnitude", {flames, Int(0)}).f() == Approx(8.0));
    CHECK(f.rig.call("caller", "magnitude", {flames, Int(5)}).f() == Approx(0.0));
    CHECK(f.rig.call("caller", "area", {flames, Int(0)}).i() == 3);
    CHECK_FALSE(f.rig.call("caller", "hostile", {flames}).b());
}

TEST_CASE("MagicEffect answers from the record", "[magic][natives]") {
    Fixture f;
    f.rig.add(script("Caller", {caller("skill", "MagicEffect", "GetAssociatedSkill", {}, "String"),
                                caller("resist", "MagicEffect", "GetResistance", {}, "String"),
                                caller("casting", "MagicEffect", "GetCastingType", {}, "Int"),
                                caller("delivery", "MagicEffect", "GetDeliveryType", {}, "Int"),
                                caller("cost", "MagicEffect", "GetBaseCost", {}, "Float")}));
    const auto fire = f.rig.vm.object(k_fire, "MagicEffect");
    CHECK(vm::Vm::to_string(f.rig.call("caller", "skill", {fire})) == "Destruction");
    CHECK(vm::Vm::to_string(f.rig.call("caller", "resist", {fire})) == "ResistFire");
    CHECK(f.rig.call("caller", "casting", {fire}).i() == 2);
    CHECK(f.rig.call("caller", "delivery", {fire}).i() == 2);
    CHECK(f.rig.call("caller", "cost", {fire}).f() == Approx(2.5));
}

TEST_CASE("ActiveMagicEffect is the object of one running effect", "[magic][natives]") {
    Fixture f;
    f.rig.add(script("Caller", {caller("target", "ActiveMagicEffect", "GetTargetActor", {}, "Actor"),
                                caller("caster", "ActiveMagicEffect", "GetCasterActor", {}, "Actor"),
                                caller("base", "ActiveMagicEffect", "GetBaseObject", {}, "MagicEffect"),
                                caller("magnitude", "ActiveMagicEffect", "GetMagnitude", {}, "Float"),
                                caller("duration", "ActiveMagicEffect", "GetDuration", {}, "Int"),
                                caller("elapsed", "ActiveMagicEffect", "GetTimeElapsed", {}, "Int"),
                                caller("dispel", "ActiveMagicEffect", "Dispel", {}, "None")}));
    const auto live = f.rig.vm.object(k_handle, "ActiveMagicEffect");
    CHECK(f.rig.call("caller", "target", {live}).form() == k_guard);
    CHECK(f.rig.call("caller", "caster", {live}).form() == k_hero);
    CHECK(f.rig.call("caller", "base", {live}).form() == k_fire);
    CHECK(f.rig.call("caller", "magnitude", {live}).f() == Approx(7.5));
    CHECK(f.rig.call("caller", "duration", {live}).i() == 30);
    CHECK(f.rig.call("caller", "elapsed", {live}).i() == 12);
    f.rig.call("caller", "dispel", {live});
    REQUIRE(f.api.dispelled.size() == 1);
    CHECK(f.api.dispelled[0] == k_handle);
    const auto gone = f.rig.vm.object(0xFFF00006, "ActiveMagicEffect");
    CHECK(f.rig.call("caller", "target", {gone}).form() == 0);
}

TEST_CASE("equipped spells map the hands, and the dispels reach the API", "[magic][natives]") {
    Fixture f;
    f.rig.add(script("Caller", {caller("equip", "Actor", "EquipSpell", {{"s", "Spell"}, {"h", "Int"}}, "None"),
                                caller("unequip", "Actor", "UnequipSpell", {{"s", "Spell"}, {"h", "Int"}}, "None"),
                                caller("held", "Actor", "GetEquippedSpell", {{"h", "Int"}}, "Spell"),
                                caller("dispel", "Actor", "DispelSpell", {{"s", "Spell"}}, "Bool"),
                                caller("all", "Actor", "DispelAllSpells", {}, "None"),
                                caller("has_effect", "Actor", "HasMagicEffect", {{"e", "MagicEffect"}}, "Bool"),
                                caller("has_keyword", "Actor", "HasMagicEffectWithKeyword", {{"k", "Keyword"}}, "Bool")}));
    const auto hero = f.rig.vm.object(k_hero, "Actor");
    const auto flames = f.rig.vm.object(k_flames, "Spell");
    f.rig.call("caller", "equip", {hero, flames, Int(1)});
    CHECK(f.rig.call("caller", "held", {hero, Int(1)}).form() == k_flames);
    CHECK(f.rig.call("caller", "held", {hero, Int(0)}).form() == 0);
    f.rig.call("caller", "unequip", {hero, flames, Int(1)});
    CHECK(f.rig.call("caller", "held", {hero, Int(1)}).form() == 0);
    CHECK(f.rig.call("caller", "dispel", {hero, flames}).b());
    f.rig.call("caller", "all", {hero});
    CHECK(f.api.dispelled == std::vector<std::uint32_t>{k_flames, k_hero});
    CHECK(f.rig.call("caller", "has_effect", {hero, f.rig.vm.object(k_fire, "MagicEffect")}).b());
    CHECK_FALSE(f.rig.call("caller", "has_effect", {hero, f.rig.vm.object(0x1, "MagicEffect")}).b());
    CHECK(f.rig.call("caller", "has_keyword", {hero, f.rig.vm.object(0x77, "Keyword")}).b());
}
