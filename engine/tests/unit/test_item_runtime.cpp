// SPDX-License-Identifier: GPL-3.0-or-later
//
// The item domain module (vm/item_runtime.cpp) over a world built in memory
// and a fake host: start inventories, equipping and its events, the stack
// refresh after units left, consumables and the item natives.
#include "support/fake_world.hpp"
#include "support/native_calls.hpp"
#include "support/vm_rig.hpp"
#include "support/world_builder.hpp"

#include "vm/item_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::ItemRuntime;
namespace wfb = bethconv::pack::wfb;
namespace etyp = skydot::vm::etyp;

namespace {

constexpr std::uint32_t k_npc_ref = 0x800;
constexpr std::uint32_t k_npc = 0x100;
constexpr std::uint32_t k_outfit = 0x110;
constexpr std::uint32_t k_sword = 0x10;
constexpr std::uint32_t k_cuirass = 0x20;
constexpr std::uint32_t k_potion = 0x40;
constexpr std::uint32_t k_player = 0x14;

WorldSpec small_world() {
    WorldSpec w;
    ItemSpec sword;
    sword.category = wfb::ItemCategory::weapon;
    sword.equip_slot = etyp::either_hand;
    sword.weapon_type = 1;
    ItemSpec cuirass;
    cuirass.category = wfb::ItemCategory::armor;
    cuirass.biped_slots = 1U << 2;
    ItemSpec potion;
    potion.category = wfb::ItemCategory::potion;
    w.bases = {{.id = k_sword, .name = "Sword", .item = sword},
               {.id = k_cuirass, .name = "Cuirass", .item = cuirass},
               {.id = k_potion, .name = "Potion", .item = potion}};
    w.outfits = {{.id = k_outfit, .items = {k_cuirass}}};
    w.npcs = {{.id = k_npc, .default_outfit = k_outfit, .items = {{k_sword, 1}}}};
    return w;
}

struct Host final : ItemRuntime::Host {
    BuiltWorld data{small_world()};
    FakeWorld world;
    skydot::vm::Inventory inventory_;
    skydot::vm::CreatedForms created_;
    skydot::vm::Vm* the_vm = nullptr;
    std::vector<std::string> events; // in the order they happened
    std::optional<bool> consumed_answer;
    std::vector<std::uint32_t> eaten;

    const wfb::World* world_root() const override { return &*data; }
    skydot::vm::WorldAccess& world_access() override { return world; }
    skydot::vm::Inventory& inventory() override { return inventory_; }
    skydot::vm::CreatedForms& created_forms() override { return created_; }
    skydot::vm::Vm& vm() override { return *the_vm; }
    void dispatch_event(std::uint32_t ref, std::string_view event, const std::vector<skydot::vm::Value>& args) override {
        events.push_back(std::string(event) + " " + std::to_string(ref) + " " + std::to_string(args[0].form()));
    }
    std::uint32_t placed_actor_base(std::uint32_t ref) const override { return ref == k_npc_ref ? k_npc : 0; }
    std::optional<double> global_value_of(std::uint32_t) const override { return std::nullopt; }
    void sync_worn_magic(std::uint32_t) override {}
    void worn_changed(std::uint32_t owner) override { events.push_back("worn_changed " + std::to_string(owner)); }
    std::optional<bool> apply_consumed(std::uint32_t, std::uint32_t form, std::uint32_t) override {
        events.push_back("consumed " + std::to_string(form));
        return consumed_answer;
    }
    void note_eaten(std::uint32_t form) override { eaten.push_back(form); }
    void log(const std::string&) override {}
    void item_trace(const std::string&) override {}
};

struct Fixture {
    Host host;
    Rig rig;
    ItemRuntime items;
    Fixture() {
        host.the_vm = &rig.vm;
        rig.add({.name = "Actor",
                 .functions = {native("IsEquipped", {{"f", "Form"}}, "Bool"), native("UnequipAll")}});
        rig.add({.name = "Form"});
        rig.add(script("Caller", {wrap("isequipped", "Actor", "IsEquipped", {{"f", "Form"}}, "Bool"),
                                  wrap("unequipall", "Actor", "UnequipAll")}));
        items.bind_natives(rig.vm, host);
    }
};

} // namespace

TEST_CASE("an owner is resolved once: its items, its outfit worn and its weapon in hand", "[item_runtime]") {
    Fixture f;
    f.items.resolve_owner(k_npc_ref);
    CHECK(f.host.inventory_.count(k_npc_ref, k_sword) == 1);
    CHECK(f.host.inventory_.count(k_npc_ref, k_cuirass) == 1);
    const auto* equipment = f.items.states().find(k_npc_ref);
    REQUIRE(equipment != nullptr);
    CHECK(equipment->worn.contains(k_cuirass));
    CHECK(equipment->right == k_sword);
    f.items.resolve_owner(k_npc_ref); // again: nothing is added
    CHECK(f.host.inventory_.count(k_npc_ref, k_sword) == 1);
}

TEST_CASE("equipping and unequipping tell the scripts and the body, in that order", "[item_runtime]") {
    Fixture f;
    f.host.inventory_.add(k_player, k_cuirass, 1);
    f.host.inventory_.add(k_player, k_sword, 1);
    CHECK(f.items.equip_item(k_player, k_cuirass, false, false));
    CHECK(f.host.events == std::vector<std::string>{"worn_changed 20", "OnObjectEquipped 20 32"});
    f.host.events.clear();
    CHECK(f.items.equip_item(k_player, k_sword, false, false));
    CHECK(f.host.events == std::vector<std::string>{"OnObjectEquipped 20 16"});
    f.host.events.clear();
    CHECK(f.items.unequip_item(k_player, k_cuirass));
    CHECK(f.host.events == std::vector<std::string>{"worn_changed 20", "OnObjectUnequipped 20 32"});
    CHECK_FALSE(f.items.equip_item(k_player, 0x999, false, true)); // unknown
}

TEST_CASE("removing the last unit of an equipped item takes it off", "[item_runtime]") {
    Fixture f;
    f.host.inventory_.add(k_player, k_sword, 1);
    REQUIRE(f.items.equip_item(k_player, k_sword, false, false));
    CHECK(f.items.states().find(k_player)->right == k_sword);
    CHECK(f.items.remove_item(k_player, k_sword, 1) == 1);
    CHECK(f.items.states().find(k_player)->right == 0);
}

TEST_CASE("a potion is consumed through the host and one unit goes", "[item_runtime]") {
    Fixture f;
    f.host.inventory_.add(k_player, k_potion, 2);
    CHECK(f.items.equip_item(k_player, k_potion, false, false));
    CHECK(f.host.events == std::vector<std::string>{"consumed 64"});
    CHECK(f.host.inventory_.count(k_player, k_potion) == 1);
    // A poisoning keeps the unit (the host used it up) and answers for the call.
    f.host.consumed_answer = false;
    CHECK_FALSE(f.items.equip_item(k_player, k_potion, false, false));
    CHECK(f.host.inventory_.count(k_player, k_potion) == 1);
}

TEST_CASE("the item natives answer from the equipment", "[item_runtime][vm][natives]") {
    Fixture f;
    f.host.inventory_.add(k_player, k_sword, 1);
    f.items.equip_item(k_player, k_sword, false, false);
    const auto actor = f.rig.vm.object(k_player, "Actor");
    CHECK(f.rig.call("caller", "isequipped", {actor, f.rig.vm.object(k_sword, "Form")}).b());
    CHECK_FALSE(f.rig.call("caller", "isequipped", {actor, f.rig.vm.object(k_cuirass, "Form")}).b());
    f.rig.call("caller", "unequipall", {actor});
    CHECK_FALSE(f.rig.call("caller", "isequipped", {actor, f.rig.vm.object(k_sword, "Form")}).b());
}

TEST_CASE("item states are saved and adopted", "[item_runtime]") {
    Fixture f;
    f.host.inventory_.add(k_player, k_sword, 1);
    f.items.equip_item(k_player, k_sword, false, false);
    const auto bytes = f.items.save();
    skydot::vm::ItemStates loaded;
    REQUIRE(skydot::vm::ItemStates::load(skydot::save::ByteReader(bytes), loaded));
    Fixture other;
    other.items.adopt(std::move(loaded));
    REQUIRE(other.items.states().find(k_player) != nullptr);
    CHECK(other.items.states().find(k_player)->right == k_sword);
    other.items.clear();
    CHECK(other.items.states().find(k_player) == nullptr);
}
