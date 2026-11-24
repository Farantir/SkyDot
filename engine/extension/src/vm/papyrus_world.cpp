// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hosts the natives families ask the world and the game layer through.
#include "vm/papyrus.hpp"

#include "data/actors.hpp"
#include "data/fb_search.hpp"
#include "skydot_formats/flags.hpp"
#include "world_generated.h"

namespace skydot {

namespace {

vm::Xyz xyz_from(const godot::Vector3& p) {
    return {static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z)};
}

} // namespace

/// The world as natives ask about it.
struct SkydotPapyrus::WorldHost final : vm::WorldAccess {
    explicit WorldHost(SkydotPapyrus& owner) : owner_(owner) {}

    const WorldData* world() const { return owner_.world_data(); }

    std::uint32_t base_form(std::uint32_t ref) const override {
        return owner_.base_form_of(ref);
    }
    std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const override {
        vm::RefInfo info;
        if (owner_.ref_record(ref, info)) {
            for (const auto& link : info.links) {
                if (link.keyword == keyword) {
                    return link.target;
                }
            }
        }
        return 0;
    }
    bool ref_info(std::uint32_t ref, vm::RefInfo& out) const override { return owner_.ref_record(ref, out); }
    std::vector<vm::ActivateChild> activate_children(std::uint32_t ref) const override {
        std::vector<vm::ActivateChild> out;
        if (world() != nullptr) {
            for (const auto& [child, cell, delay] : world()->activate_children(ref)) {
                out.push_back({child, static_cast<double>(delay)});
            }
        }
        return out;
    }
    std::uint32_t parent_cell(std::uint32_t ref) const override {
        return owner_.cell_of(ref);
    }
    bool cell_interior(std::uint32_t cell) const override {
        return !owner_.world_.is_null() && static_cast<bool>(owner_.world_->get_cell(cell).get("interior", false));
    }
    bool cell_attached(std::uint32_t cell) const override { return owner_.attached_cells_.contains(cell); }
    bool has_keyword(std::uint32_t form, std::uint32_t keyword) const override {
        if (world() == nullptr) {
            return false;
        }
        // A reference answers for its base; anything else is taken for a base.
        const auto placed = base_form(form);
        return world()->base_has_keyword(placed != 0 ? placed : form, keyword);
    }
    std::uint32_t find_keyword(const std::string& editor_id) const override {
        return world() != nullptr ? world()->keyword_by_editor_id(editor_id) : 0;
    }
    bool form_list(std::uint32_t list, std::vector<std::uint32_t>& forms) const override {
        const auto* l = world() != nullptr ? world()->form_list_ptr(list) : nullptr;
        if (l == nullptr) {
            return false;
        }
        if (l->forms() != nullptr) {
            forms.assign(l->forms()->begin(), l->forms()->end());
        }
        return true;
    }
    bool message(std::uint32_t id, vm::MessageText& out) const override {
        const auto* m = world() != nullptr ? world()->message_ptr(id) : nullptr;
        if (m == nullptr) {
            return false;
        }
        const auto text = [](const flatbuffers::String* s) { return s != nullptr ? s->str() : std::string(); };
        out.title = text(m->title());
        out.text = text(m->text());
        out.box = formats::has_flag(m->flags(), bethconv::pack::wfb::MessageFlags::message_box);
        if (m->buttons() != nullptr) {
            for (const auto* b : *m->buttons()) {
                out.buttons.push_back(text(b));
            }
        }
        return true;
    }
    bool actor_base(std::uint32_t ref, vm::ActorBase& out) const override {
        const WorldData* data = world();
        if (data == nullptr || data->root() == nullptr) {
            return false;
        }
        // The player is the NPC_ 0x7; a placed actor names its NPC_; one a
        // script placed has it as its base.
        std::uint32_t npc = 0;
        if (ref == k_player_ref) {
            npc = 0x7;
        } else if (const auto* actor = data->actor_ptr(ref)) {
            npc = actor->base();
        } else {
            npc = owner_.base_form_of(ref);
        }
        if (npc == 0 || !actor_base_values(*data->root(), npc, ref, out)) {
            return false;
        }
        out.is_player = ref == k_player_ref;
        return true;
    }
    std::uint32_t form_from_file(std::int64_t id, const std::string& plugin) const override {
        if (owner_.world_.is_null()) {
            return 0;
        }
        return static_cast<std::uint32_t>(owner_.world_->get_form_from_file(id, godot::String::utf8(plugin.c_str())));
    }
    vm::RefPlacement ref_placement(std::uint32_t ref) const override {
        vm::RefInfo info;
        return owner_.ref_record(ref, info) ? info.placement : vm::RefPlacement{};
    }
    vm::Xyz ref_position(std::uint32_t ref) const override { return xyz_from(owner_.position_of(ref)); }
    vm::Xyz ref_rotation(std::uint32_t ref) const override { return xyz_from(owner_.rotation_of(ref)); }
    bool ref_disabled(std::uint32_t ref) const override { return owner_.disabled(ref); }
    bool npc_unique(std::uint32_t npc) const override {
        const auto* root = world() != nullptr ? world()->root() : nullptr;
        const auto* record = root != nullptr ? lookup(root->npcs(), npc) : nullptr;
        return record != nullptr && formats::has_flag(record->flags(), bethconv::pack::wfb::NpcFlags::unique);
    }
    bool is_npc(std::uint32_t form) const override {
        const auto* root = world() != nullptr ? world()->root() : nullptr;
        return root != nullptr && lookup(root->npcs(), form) != nullptr;
    }
    std::uint32_t package_owner_quest(std::uint32_t package) const override {
        const auto* root = world() != nullptr ? world()->root() : nullptr;
        const auto* record = root != nullptr ? lookup(root->packages(), package) : nullptr;
        return record != nullptr ? record->owner_quest() : 0;
    }
    bool ref_3d_loaded(std::uint32_t ref) const override { return owner_.is_3d_loaded(ref); }
    std::int32_t player_level() const override { return owner_.progression_.player().level; }
    std::int32_t actor_level(std::uint32_t ref) const override {
        const std::int32_t level = owner_.condition_world().level(ref);
        return level > 0 ? level : 1;
    }
    double game_days() const override {
        return owner_.clock_.is_valid() ? owner_.clock_->get_game_days() : 0.0;
    }
    double time_scale() const override {
        return owner_.clock_.is_valid() ? owner_.clock_->get_time_scale() : 0.0;
    }
    SkydotPapyrus& owner_;
};

std::unique_ptr<vm::WorldAccess> SkydotPapyrus::new_world_host() { return std::make_unique<WorldHost>(*this); }

struct SkydotPapyrus::SoundHost final : vm::SoundHost {
    explicit SoundHost(SkydotPapyrus& owner) : owner_(owner) {}
    std::int64_t new_sound_instance() override { return owner_.new_sound_instance(); }
    SkydotPapyrus& owner_;
};

std::unique_ptr<vm::SoundHost> SkydotPapyrus::new_sound_host() { return std::make_unique<SoundHost>(*this); }

struct SkydotPapyrus::ActorHost final : vm::ActorHost {
    explicit ActorHost(SkydotPapyrus& owner) : owner_(owner) {}
    vm::ActorStates& actor_states() override { return owner_.actors_; }
    conditions::ConditionWorld& conditions() override { return owner_.condition_world(); }
    bool location_loaded(std::uint32_t location) override { return owner_.location_loaded(location); }
    void evaluate_package(std::uint32_t ref) override { owner_.evaluate_package(ref); }
    void set_last_damager(std::uint32_t killer) override { owner_.combat_.set_last_damager(killer); }
    SkydotPapyrus& owner_;
};

std::unique_ptr<vm::ActorHost> SkydotPapyrus::new_actor_host() { return std::make_unique<ActorHost>(*this); }

struct SkydotPapyrus::ReferenceHost final : vm::ReferenceHost {
    explicit ReferenceHost(SkydotPapyrus& owner) : owner_(owner) {}
    vm::RefStates& ref_states() override { return owner_.states_; }
    void set_disabled(std::uint32_t ref, bool disabled) override { owner_.set_disabled(ref, disabled); }
    void clear_destruction(std::uint32_t ref) override { owner_.clear_destruction(ref); }
    std::int64_t open_state(std::int64_t ref) const override { return owner_.get_open_state(ref); }
    void set_open_state(std::int64_t ref, std::int64_t state) override { owner_.set_open_state(ref, state); }
    void set_locked(std::int64_t ref, bool locked) override { owner_.set_locked(ref, locked); }
    bool is_locked(std::uint32_t ref) const override { return owner_.locked(ref); }
    std::int64_t lock_level(std::int64_t ref) const override { return owner_.get_lock_level(ref); }
    std::uint32_t lock_key(std::uint32_t ref) const override {
        vm::RefInfo info;
        return owner_.ref_record(owner_.lock_holder(ref), info) && info.locked ? info.lock_key : 0;
    }
    std::int64_t trigger_count(std::int64_t ref) const override { return owner_.get_trigger_count(ref); }
    void move_ref_to(std::uint32_t ref, std::uint32_t target, const vm::Xyz& offset, bool match_rotation) override {
        owner_.move_ref_to(ref, target, vec3_of(offset), match_rotation);
    }
    void relocate_ref(std::uint32_t ref, const vm::Xyz& position, const vm::Xyz& rotation) override {
        owner_.relocate_ref(ref, 0, vec3_of(position), vec3_of(rotation));
    }
    void send_event(std::uint32_t ref, std::string_view event, const std::vector<vm::Value>& args) override {
        owner_.dispatch(ref, event, args);
    }
    void resolve_owner(std::uint32_t owner) const override { owner_.item_.resolve_owner(owner); }
    std::vector<vm::Inventory::Taken> remove_item_split(std::uint32_t owner, std::uint32_t form,
                                                        std::int64_t count) override {
        return owner_.item_.remove_item_split(owner, form, count);
    }
    SkydotPapyrus& owner_;
};

std::unique_ptr<vm::ReferenceHost> SkydotPapyrus::new_reference_host() { return std::make_unique<ReferenceHost>(*this); }

} // namespace skydot
