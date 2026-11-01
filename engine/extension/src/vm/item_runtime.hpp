// SPDX-License-Identifier: GPL-3.0-or-later
//
// The item domain of the Papyrus runtime: start inventories resolved on first
// touch, what owners hold and wear, equipping, and the natives that equip. The
// rules are state/equipment.hpp (pure), the data data/start_inventory.hpp and
// data/leveled.hpp (pure); this is the glue to the VM's inventory and the
// world's records. No Godot: it reaches the engine through ItemRuntime::Host.
#pragma once

#include "data/actors.hpp"
#include "state/created_forms.hpp"
#include "state/item_states.hpp"
#include "vm/item_access.hpp"
#include "vm/reference_host.hpp"
#include "vm/vm.hpp"
#include "vm/world_access.hpp"
#include "world_generated.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace skydot::vm {

class ItemRuntime final : public ItemAccess {
public:
    class Host {
    public:
        virtual ~Host() = default;
        /// The pack's world records; null without a world.
        [[nodiscard]] virtual const bethconv::pack::wfb::World* world_root() const = 0;
        [[nodiscard]] virtual WorldAccess& world_access() = 0;
        /// The VM's inventory (it changes with each `setup`).
        [[nodiscard]] virtual Inventory& inventory() = 0;
        [[nodiscard]] virtual CreatedForms& created_forms() = 0;
        /// The running VM, for form values.
        [[nodiscard]] virtual Vm& vm() = 0;
        /// OnObjectEquipped/OnObjectUnequipped and the like: to `ref`'s scripts and the aliases it fills.
        virtual void dispatch_event(std::uint32_t ref, std::string_view event, const std::vector<Value>& args) = 0;
        /// The NPC_ of a placed actor; 0 if `ref` is none.
        [[nodiscard]] virtual std::uint32_t placed_actor_base(std::uint32_t ref) const = 0;
        /// The value of GLOB `id`; nullopt if the world has no such global.
        [[nodiscard]] virtual std::optional<double> global_value_of(std::uint32_t id) const = 0;
        /// The worn magic of `owner` follows what it wears.
        virtual void sync_worn_magic(std::uint32_t owner) = 0;
        /// What `owner` wears changed (the game layer rebuilds the body).
        virtual void worn_changed(std::uint32_t owner) = 0;
        /// A potion, food or ingredient used up: nullopt if its effects were applied normally, else the
        /// answer of the poisoning that took it (a poison goes on the weapon in the right hand).
        virtual std::optional<bool> apply_consumed(std::uint32_t owner, std::uint32_t form, std::uint32_t extra) = 0;
        /// Eating an ingredient teaches its first effect.
        virtual void note_eaten(std::uint32_t form) = 0;
        /// A line for the log (shared with the other hosts) and one for the player's trace signal.
        virtual void log(const std::string& line) = 0;
        virtual void item_trace(const std::string& line) = 0;
    };

    /// Remember the host; the host outlives the runtime's use.
    void bind(Host& host) { host_ = &host; }
    /// Remember the host and bind the item natives on `vm`.
    void bind_natives(Vm& vm, Host& host);

    /// Resolved owners, leveled actor picks, equipment and extras (ITST). Mutable: an inventory resolves on
    /// the first look at it, also from const queries.
    [[nodiscard]] ItemStates& states() const { return states_; }
    void clear() { states_.clear(); }
    /// The ITST section's bytes (over the VM's inventory).
    [[nodiscard]] std::vector<std::uint8_t> save() const { return states_.save(host_->inventory()); }
    void adopt(ItemStates&& loaded) { states_.adopt(std::move(loaded)); }

    // ItemAccess
    void resolve_owner(std::uint32_t owner) const override;
    [[nodiscard]] ItemInfo item_info_of(std::uint32_t form) const override;
    [[nodiscard]] std::string item_name_of(std::uint32_t form) const override { return item_info_of(form).name; }
    void refresh_equipment(std::uint32_t owner, std::uint32_t form) override;
    [[nodiscard]] std::int64_t dropped_count(std::uint32_t ref) const override { return states_.dropped_count(ref); }
    [[nodiscard]] std::uint32_t dropped_extra(std::uint32_t ref) const override { return states_.dropped_extra(ref); }

    /// A dying actor's death item (INAM) goes into its inventory; called once per death, after the start
    /// inventory is resolved so that comes first.
    void give_death_item(std::uint32_t ref);
    /// Take items from `owner` (resolved first); an item that runs out is taken off.
    std::int64_t remove_item(std::uint32_t owner, std::uint32_t form, std::int64_t count);
    /// Like remove_item for the units carrying `extra` only.
    std::int64_t remove_item_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t extra, std::int64_t count);
    /// Take items, saying which units (extras) went.
    std::vector<Inventory::Taken> remove_item_split(std::uint32_t owner, std::uint32_t form, std::int64_t count);
    /// The extra id of a stack named by the API's `extra` (-1: the first carried: plain, else the lowest id);
    /// 0 if `form` is not carried.
    [[nodiscard]] std::uint32_t resolve_extra(std::uint32_t owner, std::uint32_t form, std::int64_t extra) const;
    /// Equip one `form` of `owner`'s; `left` asks for the left hand where either will do. Adds the item if
    /// `add_missing`.
    bool equip_item(std::uint32_t owner, std::uint32_t form, bool left, bool add_missing, std::uint32_t extra = 0);
    bool unequip_item(std::uint32_t owner, std::uint32_t form);
    /// Unequip what the stack-and-extra `extra` of `form` is equipped as (nullopt: any); false if nothing was.
    bool unequip_extra(std::uint32_t owner, std::uint32_t form, std::optional<std::uint32_t> extra);
    void unequip_all(std::uint32_t owner);
    /// `owner`'s worn armor, and everything it holds or wears (hands, ammo, worn).
    [[nodiscard]] std::set<std::uint32_t> worn_of(std::uint32_t owner) const;
    [[nodiscard]] std::set<std::uint32_t> held_of(std::uint32_t owner) const;
    /// OnObjectEquipped/OnObjectUnequipped(form, none) to `owner`'s scripts and aliases.
    void send_equip_event(std::uint32_t owner, std::uint32_t form, bool equipped);
    /// GetEquippedItemType of `owner`'s hand (0 left, else right); 0 if empty.
    [[nodiscard]] std::int64_t equipped_item_type(std::uint32_t owner, std::int64_t hand);
    /// The armor and clothing bases `owner` wears, a shield in the left hand included (perk conditions
    /// WornHasKeyword and WornApparelHasKeywordCount).
    [[nodiscard]] std::vector<std::uint32_t> worn_armor_of(std::uint32_t owner) const;
    /// Whether `owner` has `form` equipped (a hand or worn); for the conditions.
    [[nodiscard]] bool is_equipped(std::uint32_t owner, std::uint32_t form) const;
    /// equipped_item_type for a const caller (the conditions).
    [[nodiscard]] std::int64_t hand_item_type(std::uint32_t owner, std::int64_t hand) const;
    /// The worn armor of resolved actors, for the actor builder.
    [[nodiscard]] WornProvider worn_provider() const;

private:
    /// Sends OnObjectUnequipped for what left `before` and OnObjectEquipped for what joined it.
    void note_held(std::uint32_t owner, const std::set<std::uint32_t>& before);
    /// The signal worn_changed if `owner` wears something else than `before`.
    void note_worn(std::uint32_t owner, const std::set<std::uint32_t>& before);

    Host* host_ = nullptr;
    mutable ItemStates states_;
};

} // namespace skydot::vm
