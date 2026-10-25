// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/condition_world.hpp"

#include "ai/packages.hpp"
#include "data/actors.hpp"
#include "data/fb_search.hpp"
#include "skydot_formats/flags.hpp"
#include "vm/papyrus.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

namespace skydot {
namespace {

namespace wfb = bethconv::pack::wfb;

constexpr std::uint32_t k_player_npc = 0x7;

double to_number(const godot::Variant& v) {
    switch (v.get_type()) {
    case godot::Variant::BOOL:
        return static_cast<bool>(v) ? 1.0 : 0.0;
    case godot::Variant::INT:
        return static_cast<double>(static_cast<std::int64_t>(v));
    case godot::Variant::FLOAT:
        return static_cast<double>(v);
    default:
        return 0.0;
    }
}

bool contains(const flatbuffers::Vector<std::uint32_t>* list, std::uint32_t value) {
    return list != nullptr && std::find(list->begin(), list->end(), value) != list->end();
}

const wfb::Race* race_in(const wfb::World* world, std::uint32_t race) {
    if (world == nullptr) {
        return nullptr;
    }
    return lookup(world->races(), race);
}

} // namespace

const WorldData* PapyrusConditionWorld::data() const {
    if (owner_ != nullptr) {
        return owner_->world_.is_valid() ? &owner_->world_->data() : nullptr;
    }
    return world_ != nullptr ? &world_->data() : nullptr;
}

const wfb::World* PapyrusConditionWorld::root() const {
    return data() != nullptr ? data()->root() : nullptr;
}

std::uint32_t PapyrusConditionWorld::base_form(std::uint32_t ref) const {
    if (owner_ != nullptr) {
        return owner_->base_form_of(ref);
    }
    WorldData::RefEnable info;
    return data() != nullptr && data()->ref_enable(ref, info) ? info.base : 0;
}

const wfb::Npc* PapyrusConditionWorld::resolved(std::uint32_t ref, wfb::NpcTemplateFlags flag) const {
    const std::uint32_t npc = npc_of(ref);
    return npc != 0 && root() != nullptr ? resolve_npc(*root(), npc, flag, ref) : nullptr;
}

// ---- quests and variables -----------------------------------------------------

const vm::QuestState* PapyrusConditionWorld::quests_of(std::uint32_t quest) const {
    return owner_ != nullptr ? owner_->quests_.find(quest) : nullptr;
}

std::int32_t PapyrusConditionWorld::quest_stage(std::uint32_t quest) const {
    const auto* state = quests_of(quest);
    return state != nullptr ? state->stage : 0;
}

bool PapyrusConditionWorld::stage_done(std::uint32_t quest, std::int32_t stage) const {
    const auto* state = quests_of(quest);
    return state != nullptr && state->done.contains(stage);
}

bool PapyrusConditionWorld::quest_running(std::uint32_t quest) const {
    const auto* state = quests_of(quest);
    return state != nullptr && state->running;
}

bool PapyrusConditionWorld::quest_completed(std::uint32_t quest) const {
    const auto* state = quests_of(quest);
    return state != nullptr && state->completed;
}

std::uint32_t PapyrusConditionWorld::alias_fill(std::uint32_t quest, std::uint32_t alias) const {
    if (owner_ == nullptr) {
        return 0;
    }
    const std::uint32_t handle = owner_->quests_.find_handle(quest, alias);
    return handle != 0 ? owner_->quests_.fill(handle) : 0;
}

double PapyrusConditionWorld::global_value(std::uint32_t global) const {
    if (owner_ != nullptr) {
        return owner_->get_global_value(global);
    }
    return world_ != nullptr ? static_cast<double>(world_->get_global(global).get("value", 0.0)) : 0.0;
}

std::optional<double> PapyrusConditionWorld::script_variable(std::uint32_t form, const std::string& script,
                                                             const std::string& name) const {
    if (owner_ == nullptr || owner_->vm_ == nullptr) {
        return std::nullopt;
    }
    const godot::String variable = godot::String::utf8(name.c_str());
    for (const auto* instance : owner_->vm_->instances(form)) {
        const std::string cls = instance->cls->name();
        if (!script.empty() && cls != script) {
            continue;
        }
        const godot::Variant v = owner_->get_variable(form, godot::String::utf8(cls.c_str()), variable);
        if (v.get_type() != godot::Variant::NIL) {
            return to_number(v);
        }
    }
    return std::nullopt;
}

// ---- actors ---------------------------------------------------------------------

std::uint32_t PapyrusConditionWorld::npc_of(std::uint32_t ref) const {
    if (ref == 0) {
        return 0;
    }
    if (ref == player_ref()) {
        return k_player_npc;
    }
    if (data() != nullptr) {
        if (const auto* actor = data()->actor_ptr(ref)) {
            return actor->base();
        }
    }
    return base_form(ref);
}

std::optional<std::int32_t> PapyrusConditionWorld::faction_rank(std::uint32_t ref,
                                                                std::uint32_t faction) const {
    const auto* n = resolved(ref, wfb::NpcTemplateFlags::use_factions);
    if (n != nullptr && n->factions() != nullptr) {
        for (const auto* f : *n->factions()) {
            if (f->faction() == faction) {
                return f->rank();
            }
        }
    }
    return std::nullopt;
}

std::vector<std::uint32_t> PapyrusConditionWorld::factions_of(std::uint32_t ref) const {
    std::vector<std::uint32_t> out;
    const auto* n = resolved(ref, wfb::NpcTemplateFlags::use_factions);
    if (n != nullptr && n->factions() != nullptr) {
        for (const auto* f : *n->factions()) {
            out.push_back(f->faction());
        }
    }
    return out;
}

std::uint32_t PapyrusConditionWorld::race_of(std::uint32_t ref) const {
    const auto* n = resolved(ref, wfb::NpcTemplateFlags::use_traits);
    return n != nullptr ? n->race() : 0;
}

bool PapyrusConditionWorld::is_female(std::uint32_t ref) const {
    const auto* n = resolved(ref, wfb::NpcTemplateFlags::use_traits);
    return n != nullptr && formats::has_flag(n->flags(), wfb::NpcFlags::female);
}

std::uint32_t PapyrusConditionWorld::voice_type_of(std::uint32_t ref) const {
    const auto* n = resolved(ref, wfb::NpcTemplateFlags::use_traits);
    return n != nullptr ? n->voice_type() : 0;
}

std::pair<std::uint32_t, std::uint32_t> PapyrusConditionWorld::race_voice_types(std::uint32_t race) const {
    const auto* r = root() != nullptr ? lookup(root()->races(), race) : nullptr;
    if (r == nullptr || r->voice_types() == nullptr || r->voice_types()->size() < 2) {
        return {0, 0};
    }
    return {r->voice_types()->Get(0), r->voice_types()->Get(1)};
}

bool PapyrusConditionWorld::form_list(std::uint32_t list, std::vector<std::uint32_t>& forms) const {
    const auto* l = data() != nullptr ? data()->form_list_ptr(list) : nullptr;
    if (l == nullptr) {
        return false;
    }
    if (l->forms() != nullptr) {
        forms.assign(l->forms()->begin(), l->forms()->end());
    }
    return true;
}

bool PapyrusConditionWorld::has_keyword(std::uint32_t ref, std::uint32_t keyword) const {
    if (ref == 0 || data() == nullptr) {
        return false;
    }
    // A reference answers for its base.
    const std::uint32_t base = ref == player_ref() ? k_player_npc : base_form(ref);
    if (data()->base_has_keyword(base != 0 ? base : ref, keyword)) {
        return true;
    }
    const auto* n = resolved(ref, wfb::NpcTemplateFlags::use_traits);
    if (n != nullptr && contains(n->keywords(), keyword)) {
        return true;
    }
    // The race's keywords count for the actor.
    const auto* r = race_in(root(), race_of(ref));
    return r != nullptr && contains(r->keywords(), keyword);
}

bool PapyrusConditionWorld::is_child(std::uint32_t ref) const {
    const auto* r = race_in(root(), race_of(ref));
    return r != nullptr && (r->flags() & 0x4U) != 0;
}

double PapyrusConditionWorld::actor_value(std::uint32_t ref, std::uint32_t av) const {
    return owner_ != nullptr ? owner_->actors_.get(ref, av) : 0.0;
}

double PapyrusConditionWorld::base_actor_value(std::uint32_t ref, std::uint32_t av) const {
    return owner_ != nullptr ? owner_->actors_.get_base(ref, av) : 0.0;
}

double PapyrusConditionWorld::actor_value_percent(std::uint32_t ref, std::uint32_t av) const {
    return owner_ != nullptr ? owner_->actors_.percentage(ref, av) : 1.0;
}

bool PapyrusConditionWorld::is_dead(std::uint32_t ref) const {
    return owner_ != nullptr && owner_->actors_.is_dead(ref);
}

std::int32_t PapyrusConditionWorld::dead_count(std::uint32_t npc) const {
    std::int32_t count = 0;
    if (owner_ == nullptr) {
        return 0;
    }
    for (const std::uint32_t ref : owner_->actors_.dead_refs()) {
        count += npc_of(ref) == npc ? 1 : 0;
    }
    return count;
}

bool PapyrusConditionWorld::is_disabled(std::uint32_t ref) const {
    return owner_ != nullptr && owner_->disabled(ref);
}

std::int64_t PapyrusConditionWorld::item_count(std::uint32_t ref, std::uint32_t item) const {
    if (owner_ == nullptr || owner_->vm_ == nullptr) {
        return 0;
    }
    owner_->resolve_owner(ref);
    return owner_->vm_->inventory().count(ref, item);
}

std::int32_t PapyrusConditionWorld::level(std::uint32_t ref) const {
    if (ref == player_ref()) {
        return 1; // The player has no stats of her own yet.
    }
    const auto* n = resolved(ref, wfb::NpcTemplateFlags::use_stats);
    return n != nullptr ? n->level() : 0;
}

// ---- places ---------------------------------------------------------------------

std::uint32_t PapyrusConditionWorld::cell_of(std::uint32_t ref) const {
    if (ref == 0) {
        return 0;
    }
    if (owner_ != nullptr) {
        return owner_->cell_of(ref);
    }
    return data() != nullptr ? static_cast<std::uint32_t>(data()->cell_of_ref(ref)) : 0;
}

bool PapyrusConditionWorld::cell_interior(std::uint32_t cell) const {
    const auto* c = data() != nullptr ? data()->cell_ptr(cell) : nullptr;
    return c != nullptr && formats::has_flag(c->flags(), wfb::CellFlags::interior);
}

std::uint32_t PapyrusConditionWorld::cell_world(std::uint32_t cell) const {
    const auto* c = data() != nullptr ? data()->cell_ptr(cell) : nullptr;
    return c != nullptr && !formats::has_flag(c->flags(), wfb::CellFlags::interior) ? c->world() : 0;
}

std::uint32_t PapyrusConditionWorld::cell_location(std::uint32_t cell) const {
    const auto* c = data() != nullptr ? data()->cell_ptr(cell) : nullptr;
    return c != nullptr ? c->location() : 0;
}

std::uint32_t PapyrusConditionWorld::location_parent(std::uint32_t location) const {
    const auto* l = root() != nullptr ? lookup(root()->locations(), location) : nullptr;
    return l != nullptr ? l->parent() : 0;
}

bool PapyrusConditionWorld::location_has_keyword(std::uint32_t location, std::uint32_t keyword) const {
    const auto* l = root() != nullptr ? lookup(root()->locations(), location) : nullptr;
    return l != nullptr && contains(l->keywords(), keyword);
}

std::optional<double> PapyrusConditionWorld::distance(std::uint32_t a, std::uint32_t b) const {
    const std::uint32_t cell_a = cell_of(a);
    const std::uint32_t cell_b = cell_of(b);
    if (owner_ == nullptr || cell_a == 0 || cell_b == 0 || data() == nullptr) {
        return std::nullopt;
    }
    if (data()->cell_space(cell_a) != data()->cell_space(cell_b)) {
        return std::nullopt;
    }
    return static_cast<double>(owner_->position_of(a).distance_to(owner_->position_of(b)));
}

std::uint32_t PapyrusConditionWorld::linked_ref(std::uint32_t ref, std::uint32_t keyword) const {
    if (owner_ == nullptr) {
        return 0;
    }
    const godot::Array links = owner_->ref_info(ref).get("links", godot::Array());
    for (std::int64_t i = 0; i < links.size(); ++i) {
        const godot::Dictionary link = links[i];
        if (static_cast<std::int64_t>(link["keyword"]) == keyword) {
            return static_cast<std::uint32_t>(static_cast<std::int64_t>(link["target"]));
        }
    }
    return 0;
}

// ---- relationships and vendors ---------------------------------------------------

std::optional<std::int32_t> PapyrusConditionWorld::relationship_rank(std::uint32_t npc_a,
                                                                     std::uint32_t npc_b) const {
    if (!relationships_built_) {
        relationships_built_ = true;
        if (root() != nullptr && root()->relationships() != nullptr) {
            for (const auto* r : *root()->relationships()) {
                relationships_.try_emplace(static_cast<std::uint64_t>(r->parent()) << 32 | r->child(), r->rank());
                relationships_.try_emplace(static_cast<std::uint64_t>(r->child()) << 32 | r->parent(), r->rank());
            }
        }
    }
    const auto it = relationships_.find(static_cast<std::uint64_t>(npc_a) << 32 | npc_b);
    if (it == relationships_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::pair<std::int32_t, std::int32_t>> PapyrusConditionWorld::vendor_hours(
    std::uint32_t faction) const {
    const auto* f = root() != nullptr ? lookup(root()->factions(), faction) : nullptr;
    if (f == nullptr || !formats::has_flag(f->flags(), wfb::FactionFlags::vendor)) {
        return std::nullopt;
    }
    return std::pair<std::int32_t, std::int32_t>{f->vendor_start_hour(), f->vendor_end_hour()};
}

// ---- clock, chance -----------------------------------------------------------------

double PapyrusConditionWorld::game_days() const {
    return owner_ != nullptr && owner_->clock_.is_valid() ? owner_->clock_->get_game_days() : 0.0;
}

double PapyrusConditionWorld::hour() const { return ai::Clock{.days = game_days()}.hour(); }

int PapyrusConditionWorld::day_of_week() const { return ai::Clock{.days = game_days()}.day_of_week(); }

double PapyrusConditionWorld::random_percent() {
    // xorshift64*
    dice_ ^= dice_ >> 12;
    dice_ ^= dice_ << 25;
    dice_ ^= dice_ >> 27;
    const std::uint64_t r = dice_ * 0x2545F4914F6CDD1Dull;
    return std::floor(static_cast<double>(r >> 11) / static_cast<double>(1ull << 53) * 100.0);
}

bool PapyrusConditionWorld::talked_to_player(std::uint32_t ref) const {
    return owner_ != nullptr && owner_->dialogue_states_.talked_to_player(ref);
}

bool PapyrusConditionWorld::in_dialogue_with_player(std::uint32_t ref) const {
    return owner_ != nullptr && ref != 0 && owner_->dialogue_states_.speaker() == ref;
}

std::uint32_t PapyrusConditionWorld::equipped_shout(std::uint32_t ref) const {
    return owner_ != nullptr ? owner_->voice_shout(ref) : 0;
}

// ---- the shared world, and who uses it --------------------------------------------

conditions::ConditionWorld& SkydotPapyrus::condition_world() {
    if (conditions_ == nullptr) {
        conditions_ = std::make_unique<PapyrusConditionWorld>(this);
        conditions_->unknowns().set_sink([](std::uint16_t function) {
            godot::UtilityFunctions::push_warning(
                godot::String("Conditions: function ") + godot::String::num_int64(function) +
                " has no binding; conditions using it are false (docs/conditions.md)");
        });
    }
    return *conditions_;
}

std::size_t SkydotPapyrus::chosen_log_entry(std::uint32_t quest, std::int32_t index) {
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    const auto* q = root != nullptr ? lookup(root->quests(), quest) : nullptr;
    if (q == nullptr || q->stages() == nullptr) {
        return 0;
    }
    for (const auto* stage : *q->stages()) {
        const auto* log = stage->log();
        if (stage->index() != index || log == nullptr) {
            continue;
        }
        const conditions::ConditionContext ctx{
            .subject = k_player_ref, .target = 0, .quest = quest};
        for (flatbuffers::uoffset_t i = 0; i < log->size(); ++i) {
            if (conditions::conditions_pass(log->Get(i)->condition_list(), condition_world(), ctx)) {
                return i;
            }
        }
        break;
    }
    return 0;
}

} // namespace skydot
