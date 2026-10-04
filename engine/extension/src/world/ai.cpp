// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/ai.hpp"

#include "assets/vpath.hpp"
#include "world/actor.hpp"
#include "world/actors.hpp"
#include "world/fb_search.hpp"
#include "vm/papyrus.hpp"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>
#include <set>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {
namespace {

constexpr std::uint32_t k_ref_initially_disabled = 0x1;
constexpr std::uint32_t k_ref_persistent = 0x2;
constexpr std::uint32_t k_player_ref = 0x14;
constexpr std::uint32_t k_player_npc = 0x7;
constexpr double k_seconds_per_day = 86400.0;
/// Game seconds between moves of actors that are not built.
constexpr double k_place_interval = 5.0 * 60.0;
/// Game seconds within which a pass counts as current (settle_actors).
constexpr double k_place_fresh = 2.0 * 60.0;
/// Real seconds between choosing a built actor's package again.
constexpr double k_think_interval = 1.5;
/// Game units: close enough to a load door to go through it.
constexpr double k_door_reach = 200.0;
/// Game units: arrived at a travel destination, a bed, a marker.
constexpr double k_arrive = 96.0;
constexpr double k_stand_reach = 80.0;
/// A sandbox this close to its centre wanders; farther, it walks there.
constexpr double k_min_sandbox = 256.0;
/// Walks towards one spot before it counts as reached anyway (the navmesh
/// may end short of it).
constexpr int k_max_walks = 4;
constexpr double k_retry = 2.0;
/// Door routes are short: house, town, maybe a cellar.
constexpr int k_max_route = 6;
/// Searches for furniture reach at least this far.
constexpr double k_min_search = 512.0;
/// Interiors are searched whole for "in cell" locations; outside this far.
constexpr double k_cell_radius = 1024.0;
constexpr float k_cell_units = 4096.0F;

/// PKDT general flags (xEdit, wbPackageFlags).
constexpr std::uint32_t k_unlock_at_start = 0x40;
constexpr std::uint32_t k_unlock_at_end = 0x80;
constexpr std::uint32_t k_use_preferred_speed = 0x2000;

/// PLDT location types and PTDA target types (world.fbs).
enum Location : std::int32_t {
    near_reference = 0,
    in_cell = 1,
    near_package_start = 2,
    near_editor_location = 3,
    near_linked_reference = 6,
    at_package_location = 7,
    reference_alias = 8,
    near_self = 12,
};
enum Target : std::int32_t {
    specific_reference = 0,
    object_id = 1,
    object_type = 2,
    linked_reference = 3,
    target_alias = 4,
    self = 6,
};
/// PTDA object types used to find furniture (xEdit, wbObjectTypeEnum).
constexpr std::uint32_t k_type_furniture = 10;
constexpr std::uint32_t k_type_beds = 26;
constexpr std::uint32_t k_type_chairs = 27;

constexpr std::uint32_t fourcc(const char (&tag)[5]) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(tag[0])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(tag[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(tag[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(tag[3])) << 24;
}

/// Condition functions answered here (indices from xEdit's
/// wbConditionFunctions; bethconv/record/conditions.cpp has the table).
enum Function : std::uint16_t {
    fn_get_distance = 1,
    fn_get_current_time = 18,
    fn_get_disabled = 35,
    fn_get_quest_running = 56,
    fn_get_stage = 58,
    fn_get_stage_done = 59,
    fn_get_in_cell = 67,
    fn_get_is_race = 69,
    fn_get_is_sex = 70,
    fn_get_in_faction = 71,
    fn_get_is_id = 72,
    fn_get_faction_rank = 73,
    fn_get_global_value = 74,
    fn_get_random_percent = 77,
    fn_get_is_reference = 136,
    fn_get_is_current_package = 161,
    fn_get_day_of_week = 170,
    fn_is_in_interior = 300,
    fn_get_in_worldspace = 310,
    fn_has_linked_ref = 362,
    fn_get_quest_completed = 543,
    fn_get_vm_quest_variable = 629,
};

double horizontal(const Vector3& a, const Vector3& b) {
    return static_cast<double>(godot::Vector2(a.x - b.x, a.y - b.y).length());
}

bool contains_any(const flatbuffers::String* text, std::initializer_list<std::string_view> words) {
    if (text == nullptr) {
        return false;
    }
    const std::string lower = ascii_lower(text->string_view());
    return std::any_of(words.begin(), words.end(),
                       [&](std::string_view w) { return lower.find(w) != std::string::npos; });
}

double to_number(const Variant& v) {
    switch (v.get_type()) {
    case Variant::BOOL:
        return static_cast<bool>(v) ? 1.0 : 0.0;
    case Variant::INT:
        return static_cast<double>(static_cast<std::int64_t>(v));
    case Variant::FLOAT:
        return static_cast<double>(v);
    default:
        return 0.0;
    }
}

} // namespace

// ---- conditions -----------------------------------------------------------

class SkydotAi::Host final : public ai::ConditionHost {
public:
    Host(SkydotAi& ai, Mind& m) : ai_(ai), m_(m) {}

    double function_value(const wfb::Condition& c, std::uint32_t subject) override {
        const auto& w = *ai_.root();
        const auto* actor = find_sorted(w.actors(), subject, [](const wfb::ActorRef* a) { return a->ref(); });
        const std::uint32_t npc = subject == k_player_ref ? k_player_npc : actor != nullptr ? actor->base() : 0;
        const auto quest = [&](const char* key) -> Variant {
            if (ai_.vm_ == nullptr) {
                return Variant();
            }
            return ai_.quest_state(c.param1()).get(key, Variant());
        };
        switch (c.function()) {
        case fn_get_is_id:
            return npc == c.param1() || subject == c.param1() ? 1.0 : 0.0;
        case fn_get_is_reference:
            return subject == c.param1() ? 1.0 : 0.0;
        case fn_get_in_faction:
        case fn_get_faction_rank: {
            const auto* n = npc != 0 ? resolve_npc(w, npc, k_template_factions, subject) : nullptr;
            if (n != nullptr && n->factions() != nullptr) {
                for (const auto* f : *n->factions()) {
                    if (f->faction() == c.param1()) {
                        return c.function() == fn_get_in_faction ? 1.0 : static_cast<double>(f->rank());
                    }
                }
            }
            return c.function() == fn_get_in_faction ? 0.0 : -1.0;
        }
        case fn_get_is_race: {
            const auto* n = npc != 0 ? resolve_npc(w, npc, 0x0001, subject) : nullptr;
            return n != nullptr && n->race() == c.param1() ? 1.0 : 0.0;
        }
        case fn_get_is_sex: {
            const auto* n = npc != 0 ? resolve_npc(w, npc, 0x0001, subject) : nullptr;
            const std::uint32_t sex = n != nullptr ? (n->flags() & 0x1u) : 0;
            return sex == c.param1() ? 1.0 : 0.0;
        }
        case fn_get_stage:
            return to_number(quest("stage"));
        case fn_get_stage_done: {
            const Variant done = quest("stages_done");
            return done.get_type() == Variant::ARRAY && Array(done).has(static_cast<std::int64_t>(c.param2()))
                       ? 1.0
                       : 0.0;
        }
        case fn_get_quest_running:
            return to_number(quest("running"));
        case fn_get_quest_completed:
            return to_number(quest("completed"));
        case fn_get_global_value:
            return global_value(c.param1());
        case fn_has_linked_ref:
            return ai_.linked_ref(subject, c.param1()) != 0 ? 1.0 : 0.0;
        case fn_get_disabled:
            return ai_.vm_ != nullptr && ai_.vm_->is_ref_disabled(subject) ? 1.0 : 0.0;
        case fn_get_random_percent:
            return std::floor(ai_.roll(m_) * 100.0);
        case fn_get_current_time:
            return ai_.clock_.hour();
        case fn_get_day_of_week:
            return ai_.clock_.day_of_week();
        case fn_get_is_current_package:
            if (const auto* other = ai_.minds_.count(subject) != 0 ? &ai_.minds_.at(subject) : nullptr) {
                return other->package == c.param1() ? 1.0 : 0.0;
            }
            return 0.0;
        case fn_is_in_interior:
        case fn_get_in_worldspace:
        case fn_get_in_cell:
        case fn_get_distance: {
            const Spot here = subject == m_.ref ? ai_.actor_spot(m_) : ai_.ref_spot(subject).value_or(Spot{});
            const auto* space = ai_.world_->cell_ptr(here.space);
            const bool interior = space != nullptr && (space->flags() & 0x1u) != 0;
            if (c.function() == fn_is_in_interior) {
                return interior ? 1.0 : 0.0;
            }
            if (c.function() == fn_get_in_worldspace) {
                return !interior && here.space == c.param1() ? 1.0 : 0.0;
            }
            if (c.function() == fn_get_in_cell) {
                return interior && here.space == c.param1() ? 1.0 : 0.0;
            }
            const auto other = ai_.ref_spot(c.param1());
            if (!other || other->space != here.space) {
                return 1.0e9;
            }
            return static_cast<double>(here.position.distance_to(other->position));
        }
        case fn_get_vm_quest_variable: {
            if (ai_.vm_ == nullptr || c.string2() == nullptr) {
                return 0.0;
            }
            auto& classes = ai_.quest_scripts_[c.param1()];
            if (classes.empty()) {
                const Array scripts = ai_.world_->get_quest(c.param1()).get("scripts", Array());
                for (int i = 0; i < scripts.size(); ++i) {
                    classes.push_back(Dictionary(scripts[i]).get("name", String()));
                }
            }
            const String name = String::utf8(c.string2()->c_str());
            for (const auto& cls : classes) {
                const Variant v = ai_.vm_->get_variable(c.param1(), cls, name);
                if (v.get_type() != Variant::NIL) {
                    return to_number(v);
                }
            }
            return 0.0;
        }
        default:
            return 0.0;
        }
    }

    double global_value(std::uint32_t global) override {
        if (ai_.vm_ != nullptr) {
            return ai_.vm_->get_global_value(global);
        }
        return static_cast<double>(ai_.world_->get_global(global).get("value", 0.0));
    }

    std::uint32_t linked_ref(std::uint32_t actor, std::uint32_t keyword) override {
        return ai_.linked_ref(actor, keyword);
    }

private:
    SkydotAi& ai_;
    Mind& m_;
};

// ---- setup ------------------------------------------------------------------

void SkydotAi::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::MethodInfo;
    using godot::PropertyInfo;
    ClassDB::bind_method(D_METHOD("setup", "world", "papyrus"), &SkydotAi::setup);
    ClassDB::bind_method(D_METHOD("set_days", "days"), &SkydotAi::set_days);
    ClassDB::bind_method(D_METHOD("get_days"), &SkydotAi::get_days);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "days"), "set_days", "get_days");
    ClassDB::bind_method(D_METHOD("get_hour"), &SkydotAi::get_hour);
    ClassDB::bind_method(D_METHOD("get_day_of_week"), &SkydotAi::get_day_of_week);
    ClassDB::bind_method(D_METHOD("set_time_scale", "value"), &SkydotAi::set_time_scale);
    ClassDB::bind_method(D_METHOD("get_time_scale"), &SkydotAi::get_time_scale);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "time_scale"), "set_time_scale", "get_time_scale");
    ClassDB::bind_method(D_METHOD("set_drive", "value"), &SkydotAi::set_drive);
    ClassDB::bind_method(D_METHOD("get_drive"), &SkydotAi::get_drive);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "drive"), "set_drive", "get_drive");
    ClassDB::bind_method(D_METHOD("set_space", "space"), &SkydotAi::set_space);
    ClassDB::bind_method(D_METHOD("get_space"), &SkydotAi::get_space);
    ClassDB::bind_method(D_METHOD("place_actors"), &SkydotAi::place_actors);
    ClassDB::bind_method(D_METHOD("begin_placing"), &SkydotAi::begin_placing);
    ClassDB::bind_method(D_METHOD("settle_actors"), &SkydotAi::settle_actors);
    ClassDB::bind_method(D_METHOD("attach_built", "root"), &SkydotAi::attach_built);
    ClassDB::bind_method(D_METHOD("update", "seconds"), &SkydotAi::update);
    ClassDB::bind_method(D_METHOD("get_actor_state", "ref"), &SkydotAi::get_actor_state);
    ClassDB::bind_method(D_METHOD("get_packages", "ref"), &SkydotAi::get_packages);
    ClassDB::bind_method(D_METHOD("get_destination", "ref"), &SkydotAi::get_destination);
    BIND_CONSTANT(PLACE_BUDGET_USEC);
    ADD_SIGNAL(MethodInfo("actor_arrived", PropertyInfo(Variant::INT, "ref")));
    ADD_SIGNAL(MethodInfo("actor_left", PropertyInfo(Variant::INT, "ref"), PropertyInfo(Variant::INT, "door")));
}

godot::Error SkydotAi::setup(const godot::Ref<SkydotWorld>& world, const godot::Ref<godot::RefCounted>& papyrus) {
    world_ = world;
    papyrus_ = papyrus;
    vm_ = papyrus.is_valid() ? godot::Object::cast_to<SkydotPapyrus>(papyrus.ptr()) : nullptr;
    minds_.clear();
    package_lists_.clear();
    lock_cache_.clear();
    quest_scripts_.clear();
    persistent_.clear();
    attached_.clear();
    doors_.clear();
    if (world.is_null() || !world->is_open()) {
        return godot::ERR_UNCONFIGURED;
    }
    const auto& w = *root();
    if (w.actors() != nullptr) {
        for (const auto* a : *w.actors()) {
            if ((a->flags() & k_ref_persistent) != 0 && (a->flags() & k_ref_initially_disabled) == 0 &&
                !ai::package_list(w, a->base(), a->ref()).empty()) {
                persistent_.push_back(a->ref());
            }
        }
    }
    return godot::OK;
}

const wfb::World* SkydotAi::root() const {
    return world_.is_valid() ? world_->root_ : nullptr;
}

SkydotAi::Mind* SkydotAi::mind(std::uint32_t ref) {
    if (const auto it = minds_.find(ref); it != minds_.end()) {
        return &it->second;
    }
    const auto* a = root() != nullptr
                        ? find_sorted(root()->actors(), ref, [](const wfb::ActorRef* r) { return r->ref(); })
                        : nullptr;
    if (a == nullptr) {
        return nullptr;
    }
    Mind m;
    m.ref = ref;
    m.npc = a->base();
    m.persistent = (a->flags() & k_ref_persistent) != 0;
    m.dice = 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(ref) * 0xBF58476D1CE4E5B9ull);
    m.think = static_cast<double>(ref % 97) / 97.0 * k_think_interval;
    return &minds_.emplace(ref, std::move(m)).first->second;
}

double SkydotAi::roll(Mind& m) {
    // xorshift64*: deterministic per actor.
    m.dice ^= m.dice >> 12;
    m.dice ^= m.dice << 25;
    m.dice ^= m.dice >> 27;
    const std::uint64_t r = m.dice * 0x2545F4914F6CDD1Dull;
    return static_cast<double>(r >> 11) / static_cast<double>(1ull << 53);
}

SkydotActor* SkydotAi::node_of(const Mind& m) const {
    if (m.node == 0) {
        return nullptr;
    }
    return godot::Object::cast_to<SkydotActor>(godot::ObjectDB::get_instance(m.node));
}

void SkydotAi::set_days(double days) {
    // A jump (not a clock kept in step by the caller) places actors again on
    // the next update.
    if (std::abs(days - clock_.days) * 24.0 * 60.0 > 2.0) {
        since_placed_ = k_place_interval;
    }
    clock_.days = days;
}

void SkydotAi::set_space(std::int64_t space) {
    space_ = static_cast<std::uint32_t>(space);
}

// ---- where things are ------------------------------------------------------

std::optional<SkydotAi::Spot> SkydotAi::ref_spot(std::uint32_t ref) const {
    if (ref == 0 || root() == nullptr) {
        return std::nullopt;
    }
    if (const auto it = minds_.find(ref); it != minds_.end() && it->second.node != 0) {
        return Spot{it->second.space, it->second.last, 0.0F, 0.0, ref};
    }
    const Dictionary place = world_->get_actor_place(ref);
    if (!place.is_empty()) {
        return Spot{static_cast<std::uint32_t>(static_cast<std::int64_t>(place["space"])),
                    place["position"], static_cast<float>(static_cast<double>(place["rotation_z"])), 0.0, ref};
    }
    const auto cell = static_cast<std::uint32_t>(world_->get_ref_cell(ref));
    const auto* c = world_->cell_ptr(cell);
    if (c == nullptr) {
        return std::nullopt;
    }
    const auto* r = find_sorted(c->refs(), ref, [](const wfb::Ref* x) { return x->id(); });
    if (r == nullptr) {
        return std::nullopt;
    }
    return Spot{static_cast<std::uint32_t>(world_->get_cell_space(cell)),
                Vector3(r->position().x(), r->position().y(), r->position().z()), r->rotation().z(), 0.0, ref};
}

SkydotAi::Spot SkydotAi::actor_spot(const Mind& m) const {
    if (m.node != 0) {
        return Spot{m.space, m.last, 0.0F, 0.0, m.ref};
    }
    return ref_spot(m.ref).value_or(Spot{});
}

SkydotAi::Spot SkydotAi::editor_spot(const Mind& m) const {
    const auto* a = find_sorted(root()->actors(), m.ref, [](const wfb::ActorRef* r) { return r->ref(); });
    if (a == nullptr) {
        return actor_spot(m);
    }
    return Spot{static_cast<std::uint32_t>(world_->get_cell_space(a->cell())),
                Vector3(a->position().x(), a->position().y(), a->position().z()), a->rotation().z(), 0.0, 0};
}

std::uint32_t SkydotAi::linked_ref(std::uint32_t ref, std::uint32_t keyword) const {
    if (root() == nullptr || ref == 0) {
        return 0;
    }
    std::uint32_t cell = 0;
    if (const auto* a = find_sorted(root()->actors(), ref, [](const wfb::ActorRef* r) { return r->ref(); })) {
        cell = a->cell();
    } else {
        cell = static_cast<std::uint32_t>(world_->get_ref_cell(ref));
    }
    const auto* c = world_->cell_ptr(cell);
    const auto* links = c != nullptr ? c->links() : nullptr;
    if (links == nullptr) {
        return 0;
    }
    for (auto i = first_at_least(links, ref, [](const wfb::LinkedRef* l) { return l->ref(); });
         i < links->size() && links->Get(i)->ref() == ref; ++i) {
        if (links->Get(i)->keyword() == keyword) {
            return links->Get(i)->target();
        }
    }
    return 0;
}

std::optional<SkydotAi::Spot> SkydotAi::location_spot(Mind& m, const wfb::PackageInput& in) {
    const double radius = std::max(0, in.location_radius());
    std::optional<Spot> out;
    switch (in.location_type()) {
    case near_reference:
        out = ref_spot(in.location_value());
        break;
    case in_cell: {
        const auto cell = in.location_value();
        // Somewhere inside: by a door into it, where the player arrives too.
        for (const auto& [door, to] : doors_of(cell)) {
            (void)to;
            if (auto s = ref_spot(door)) {
                out = Spot{cell, world_->nearest_nav_point(cell, s->position, 1024.0), 0.0F, 0.0, 0};
                break;
            }
        }
        if (out) {
            out->radius = std::max(radius, k_cell_radius);
            return out;
        }
        break;
    }
    case near_editor_location:
        out = editor_spot(m);
        break;
    case near_linked_reference:
        out = ref_spot(linked_ref(m.ref, in.location_value()));
        if (!out) {
            out = editor_spot(m);
        }
        break;
    case reference_alias:
        if (vm_ != nullptr && m.package != 0) {
            if (const auto* p = ai::find_package(*root(), m.package); p != nullptr && p->owner_quest() != 0) {
                out = ref_spot(static_cast<std::uint32_t>(
                    vm_->get_alias_ref(p->owner_quest(), static_cast<std::int64_t>(in.location_value()))));
            }
        }
        break;
    case near_package_start:
    case at_package_location:
    case near_self:
    default:
        out = actor_spot(m);
        break;
    }
    if (out) {
        out->radius = radius;
    }
    return out;
}

std::optional<SkydotAi::Spot> SkydotAi::target_spot(Mind& m, const wfb::PackageInput& in) {
    switch (in.target_type()) {
    case specific_reference:
        return ref_spot(in.target_value());
    case object_id:
    case object_type: {
        Spot around = actor_spot(m);
        around.radius = std::max(k_min_search, static_cast<double>(in.target_count()));
        return ref_spot(find_ref(around, in.target_type(), in.target_value()));
    }
    case linked_reference:
        return ref_spot(linked_ref(m.ref, in.target_value()));
    case target_alias:
        if (vm_ != nullptr && m.package != 0) {
            if (const auto* p = ai::find_package(*root(), m.package); p != nullptr && p->owner_quest() != 0) {
                return ref_spot(static_cast<std::uint32_t>(
                    vm_->get_alias_ref(p->owner_quest(), static_cast<std::int64_t>(in.target_value()))));
            }
        }
        return std::nullopt;
    case self:
    default:
        return actor_spot(m);
    }
}

std::uint32_t SkydotAi::find_ref(const Spot& around, std::int32_t type, std::uint32_t value) const {
    std::vector<const wfb::Cell*> cells;
    const auto* space = world_->cell_ptr(around.space);
    if (space != nullptr && (space->flags() & 0x1u) != 0) {
        cells.push_back(space);
    } else {
        const auto reach = static_cast<float>(around.radius);
        const auto x0 = static_cast<std::int32_t>(std::floor((around.position.x - reach) / k_cell_units));
        const auto x1 = static_cast<std::int32_t>(std::floor((around.position.x + reach) / k_cell_units));
        const auto y0 = static_cast<std::int32_t>(std::floor((around.position.y - reach) / k_cell_units));
        const auto y1 = static_cast<std::int32_t>(std::floor((around.position.y + reach) / k_cell_units));
        for (auto y = y0; y <= y1; ++y) {
            for (auto x = x0; x <= x1; ++x) {
                if (const auto* c = world_->exterior_ptr(around.space, x, y)) {
                    cells.push_back(c);
                }
            }
        }
    }
    const auto matches = [&](const wfb::Base* base) {
        if (base == nullptr) {
            return false;
        }
        if (type == object_id) {
            return base->id() == value;
        }
        if (type != object_type || base->type() != fourcc("FURN")) {
            return false;
        }
        switch (value) {
        case k_type_furniture:
            return true;
        case k_type_beds:
            return contains_any(base->editor_id(), {"bed"});
        case k_type_chairs:
            return contains_any(base->editor_id(), {"chair", "bench", "stool", "throne", "seat"});
        default:
            return false;
        }
    };
    std::uint32_t best = 0;
    double best_d = around.radius * around.radius;
    for (const auto* cell : cells) {
        if (cell->refs() == nullptr) {
            continue;
        }
        for (const auto* r : *cell->refs()) {
            if ((r->flags() & k_ref_initially_disabled) != 0 || !matches(world_->base_ptr(r->base()))) {
                continue;
            }
            const Vector3 p(r->position().x(), r->position().y(), r->position().z());
            const auto d = static_cast<double>(p.distance_squared_to(around.position));
            if (d <= best_d) {
                best_d = d;
                best = r->id();
            }
        }
    }
    return best;
}

std::optional<SkydotAi::Spot> SkydotAi::step_spot(Mind& m, const ai::Step& s) {
    using ai::Procedure;
    switch (s.procedure) {
    case Procedure::sleep:
    case Procedure::sit:
    case Procedure::eat:
        if (const auto it = m.lists.find(s.list_key); it != m.lists.end() && it->second != 0) {
            return ref_spot(it->second);
        }
        [[fallthrough]];
    case Procedure::use_idle_marker:
    case Procedure::activate:
    case Procedure::patrol:
        if (s.target != nullptr) {
            if (auto spot = target_spot(m, *s.target)) {
                return spot;
            }
        }
        return s.location != nullptr ? location_spot(m, *s.location) : std::nullopt;
    case Procedure::wait:
    case Procedure::other:
        return std::nullopt;
    default:
        return s.location != nullptr ? location_spot(m, *s.location) : std::nullopt;
    }
}

void SkydotAi::do_find(Mind& m, const ai::Step& s) {
    if (s.list_key < 0 || s.target == nullptr || m.lists.contains(s.list_key)) {
        return; // Found once per package.
    }
    std::optional<Spot> around = s.location != nullptr ? location_spot(m, *s.location) : actor_spot(m);
    if (!around) {
        return;
    }
    around->radius = std::max(around->radius, k_min_search);
    m.lists[s.list_key] = find_ref(*around, s.target->target_type(), s.target->target_value());
}

std::optional<SkydotAi::Spot> SkydotAi::settled_spot(Mind& m) {
    using ai::Procedure;
    std::optional<Spot> out;
    for (const auto& s : m.steps) {
        if (s.procedure == Procedure::find) {
            do_find(m, s);
            continue;
        }
        if (s.procedure == Procedure::wait || s.procedure == Procedure::other ||
            s.procedure == Procedure::unlock_doors || s.procedure == Procedure::lock_doors) {
            continue;
        }
        if (auto spot = step_spot(m, s)) {
            out = spot;
        }
    }
    return out;
}

// ---- doors ------------------------------------------------------------------

const std::vector<std::pair<std::uint32_t, std::uint32_t>>& SkydotAi::doors_of(std::uint32_t space) {
    if (const auto it = doors_.find(space); it != doors_.end()) {
        return it->second;
    }
    auto& out = doors_[space];
    for (const auto& [door, entry] : world_->doors_) {
        if (static_cast<std::uint32_t>(world_->get_cell_space(entry.first->id())) != space) {
            continue;
        }
        const auto dest = world_->doors_.find(entry.second->destination());
        if (dest == world_->doors_.end()) {
            continue;
        }
        out.emplace_back(door, static_cast<std::uint32_t>(world_->get_cell_space(dest->second.first->id())));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::pair<std::uint32_t, std::uint32_t> SkydotAi::route(std::uint32_t from, std::uint32_t to) {
    if (from == to || from == 0 || to == 0) {
        return {0, 0};
    }
    // Breadth first over spaces; remember the door that reached each.
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> came; // space -> (previous space, door)
    std::deque<std::pair<std::uint32_t, int>> open{{from, 0}};
    came[from] = {0, 0};
    while (!open.empty()) {
        const auto [space, depth] = open.front();
        open.pop_front();
        if (depth >= k_max_route) {
            continue;
        }
        for (const auto& [door, next] : doors_of(space)) {
            if (came.contains(next)) {
                continue;
            }
            came[next] = {space, door};
            if (next == to) {
                std::uint32_t last = door;
                std::uint32_t first = door;
                for (auto at = space; at != from; at = came[at].first) {
                    first = came[at].second;
                }
                return {first, last};
            }
            open.emplace_back(next, depth + 1);
        }
    }
    return {0, 0};
}

const std::vector<std::uint32_t>& SkydotAi::packages_of(const Mind& m) {
    const auto it = package_lists_.find(m.ref);
    if (it != package_lists_.end()) {
        return it->second;
    }
    return package_lists_.emplace(m.ref, ai::package_list(*root(), m.npc, m.ref)).first->second;
}

const Dictionary& SkydotAi::quest_state(std::uint32_t quest) {
    const auto it = quest_cache_.find(quest);
    if (it != quest_cache_.end()) {
        return it->second;
    }
    return quest_cache_.emplace(quest, vm_->get_quest_state(quest)).first->second;
}

bool SkydotAi::has_lock(std::uint32_t door) const {
    if (const auto it = lock_cache_.find(door); it != lock_cache_.end()) {
        return it->second;
    }
    const auto lock_of = [&](std::uint32_t ref) {
        const auto cell = world_->get_ref_cell(ref);
        return world_->get_ref_info(cell, ref).get("lock", Variant()).get_type() == Variant::DICTIONARY;
    };
    if (lock_of(door)) {
        return true;
    }
    const auto it = world_->doors_.find(door);
    const bool out = lock_of(door) || (it != world_->doors_.end() && lock_of(it->second.second->destination()));
    lock_cache_[door] = out;
    return out;
}

void SkydotAi::set_doors_locked(const Spot& spot, bool locked) {
    if (vm_ == nullptr || spot.space == 0) {
        return;
    }
    const auto* space = world_->cell_ptr(spot.space);
    const bool interior = space != nullptr && (space->flags() & 0x1u) != 0;
    for (const auto& [door, to] : doors_of(spot.space)) {
        (void)to;
        if (!interior) {
            const auto at = ref_spot(door);
            if (!at || at->position.distance_to(spot.position) > static_cast<float>(spot.radius + 1024.0)) {
                continue;
            }
        }
        if (locked && !has_lock(door)) {
            continue; // Only doors that have a lock can be locked.
        }
        if (batching_) {
            const auto it = pending_locks_.find(door);
            pending_locks_[door] = it != pending_locks_.end() ? (it->second && locked) : locked;
        } else {
            vm_->set_locked(door, locked);
        }
    }
}

// ---- choosing ---------------------------------------------------------------

bool SkydotAi::choose(Mind& m) {
    if (root() == nullptr) {
        return false;
    }
    Host host(*this, m);
    const auto skip = [&](std::uint32_t pkg) {
        const auto it = m.completed.find(pkg);
        if (it == m.completed.end()) {
            return false;
        }
        const auto* p = ai::find_package(*root(), pkg);
        if (p == nullptr || (clock_.days - it->second) * 24.0 * 60.0 >= ai::done_minutes(*p)) {
            m.completed.erase(it);
            return false;
        }
        return true;
    };
    const auto id = ai::choose_package(*root(), packages_of(m), m.ref, clock_, host, skip);
    if (id == m.package && !m.steps.empty()) {
        return false;
    }
    const auto previous = m.package;
    m.package = id;
    start_package(m, previous);
    return previous != id;
}

void SkydotAi::start_package(Mind& m, std::uint32_t previous) {
    if (previous != 0) {
        if (const auto* p = ai::find_package(*root(), previous); p != nullptr && (p->flags() & k_unlock_at_end) != 0) {
            set_doors_locked(actor_spot(m), false);
        }
    }
    m.steps.clear();
    m.step = 0;
    m.step_started = false;
    m.step_time = 0.0;
    m.target.reset();
    m.door = 0;
    m.failures = 0;
    m.walks = 0;
    m.patrol.clear();
    m.lists.clear();
    m.announced = false;
    if (m.package == 0) {
        return;
    }
    const auto pkg = ai::resolve_package(*root(), m.package);
    Host host(*this, m);
    m.steps = ai::plan_steps(pkg, host, m.ref, [&]() { return roll(m); });
    if ((pkg.own->flags() & k_unlock_at_start) != 0) {
        set_doors_locked(actor_spot(m), false);
    }
    if (m.node == 0) {
        // Not built: what its steps do to doors happens now.
        for (const auto& s : m.steps) {
            if (s.procedure == ai::Procedure::unlock_doors || s.procedure == ai::Procedure::lock_doors) {
                if (auto spot = s.location != nullptr ? location_spot(m, *s.location) : actor_spot(m)) {
                    set_doors_locked(*spot, s.procedure == ai::Procedure::lock_doors);
                }
            }
        }
    }
}

// ---- actors that are not built ---------------------------------------------

std::int64_t SkydotAi::place_actors() {
    return place(false);
}

std::int64_t SkydotAi::place(bool through_doors) {
    since_placed_ = 0.0;
    return place_from(0, through_doors);
}

void SkydotAi::begin_placing() {
    if (root() == nullptr) {
        return;
    }
    since_placed_ = 0.0;
    placed_ = true;
    place_cursor_ = 0;
}

std::int64_t SkydotAi::settle_actors() {
    if (place_cursor_ != static_cast<std::size_t>(-1)) {
        // The rest at once, as place_actors would: arriving builds everyone.
        return place_from(place_cursor_, false);
    }
    if (placed_ && since_placed_ < k_place_fresh) {
        return 0;
    }
    return place(false);
}

std::int64_t SkydotAi::place_from(std::size_t first, bool through_doors) {
    place_cursor_ = static_cast<std::size_t>(-1);
    if (root() == nullptr) {
        return 0;
    }
    placed_ = true;
    quest_cache_.clear();
    std::int64_t moved = 0;
    batching_ = true;
    pending_locks_.clear();
    for (std::size_t i = first; i < persistent_.size(); ++i) {
        moved += place_one(persistent_[i], through_doors) ? 1 : 0;
    }
    batching_ = false;
    if (vm_ != nullptr) {
        for (const auto& [door, locked] : pending_locks_) {
            vm_->set_locked(door, locked);
        }
    }
    pending_locks_.clear();
    return moved;
}

bool SkydotAi::place_one(std::uint32_t ref, bool through_doors) {
    Mind* m = mind(ref);
    if (m == nullptr) {
        return false;
    }
    if (m->node != 0) {
        if (node_of(*m) != nullptr) {
            return false; // Built: its steps move it.
        }
        detach(*m);
    }
    choose(*m);
    auto spot = settled_spot(*m);
    if (!spot) {
        return false;
    }
    const Spot now = actor_spot(*m);
    const double near = std::max(spot->radius, k_min_sandbox) + 64.0;
    if (now.space == spot->space && static_cast<double>(now.position.distance_to(spot->position)) <= near) {
        return false;
    }
    Vector3 at = spot->position;
    float facing = spot->rotation_z;
    if (spot->radius > 0.0) {
        // Somewhere in the sandbox, not on its marker.
        const double angle = roll(*m) * 2.0 * std::numbers::pi;
        const double dist = std::sqrt(roll(*m)) * spot->radius * 0.5;
        at += Vector3(static_cast<float>(std::cos(angle) * dist), static_cast<float>(std::sin(angle) * dist), 0);
    }
    if (through_doors && spot->space == space_ && now.space != space_) {
        // Coming into the space on screen: through the door it uses.
        const auto [first, last] = route(now.space, space_);
        (void)first;
        if (const auto it = world_->doors_.find(last); last != 0 && it != world_->doors_.end()) {
            const auto& p = it->second.second->position();
            at = Vector3(p.x(), p.y(), p.z());
            facing = it->second.second->rotation().z();
        }
    }
    at = world_->nearest_nav_point(spot->space, at, 512.0);
    const auto square = [](const Vector3& p) {
        return std::pair{static_cast<std::int32_t>(std::floor(p.x / k_cell_units)),
                         static_cast<std::int32_t>(std::floor(p.y / k_cell_units))};
    };
    const bool entered = now.space != spot->space || square(now.position) != square(at);
    world_->set_actor_place(ref, spot->space, at, static_cast<double>(facing));
    // Entering a space builds everyone there anyway; only later moves arrive.
    if (through_doors && entered && spot->space == space_ && space_ != 0) {
        m->announced = true;
        emit_signal("actor_arrived", static_cast<std::int64_t>(ref));
    }
    return true;
}

// ---- actors that are built -------------------------------------------------

std::int64_t SkydotAi::attach_built(godot::Node* root_node) {
    if (root_node == nullptr || root() == nullptr) {
        return 0;
    }
    std::int64_t count = 0;
    std::vector<godot::Node*> stack{root_node};
    while (!stack.empty()) {
        godot::Node* n = stack.back();
        stack.pop_back();
        if (auto* actor = godot::Object::cast_to<SkydotActor>(n); actor != nullptr && actor->has_meta("skydot_ref")) {
            const auto ref = static_cast<std::uint32_t>(static_cast<std::int64_t>(actor->get_meta("skydot_ref")));
            Mind* m = mind(ref);
            if (m == nullptr) {
                continue;
            }
            m->node = actor->get_instance_id();
            const Dictionary place = world_->get_actor_place(ref);
            m->space = static_cast<std::uint32_t>(static_cast<std::int64_t>(place.get("space", 0)));
            m->last = place.get("position", Vector3());
            if (std::find(attached_.begin(), attached_.end(), ref) == attached_.end()) {
                attached_.push_back(ref);
            }
            const auto toggled = callable_mp(this, &SkydotAi::on_door_toggled);
            if (!actor->is_connected("door_toggled", toggled)) {
                actor->connect("door_toggled", toggled);
            }
            choose(*m);
            // With no package it keeps wandering about its place.
            actor->set_wander(m->package == 0 ? world_->get_actor_wander() : false);
            ++count;
            continue;
        }
        for (int i = 0; i < n->get_child_count(); ++i) {
            stack.push_back(n->get_child(i));
        }
    }
    return count;
}

void SkydotAi::on_door_toggled(std::int64_t ref, std::int64_t open_state) {
    if (vm_ != nullptr) {
        vm_->set_open_state(ref, open_state);
    }
}

void SkydotAi::detach(Mind& m) {
    // Freed with its cell: it stays where it was, or where it was going.
    std::erase(attached_, m.ref);
    m.node = 0;
    if (m.space == 0) {
        return;
    }
    Vector3 at = m.last;
    if (m.target && m.target->space == m.space && m.door == 0) {
        at = world_->nearest_nav_point(m.space, m.target->position, 512.0);
    }
    world_->set_actor_place(m.ref, m.space, at, 0.0);
}

void SkydotAi::update(double seconds) {
    if (root() == nullptr) {
        return;
    }
    clock_.days += seconds * time_scale_ / k_seconds_per_day;
    since_placed_ += seconds * time_scale_;
    quest_cache_.clear();
    const std::vector<std::uint32_t> attached = attached_;
    for (const auto ref : attached) {
        Mind* m = mind(ref);
        if (m == nullptr) {
            continue;
        }
        SkydotActor* actor = node_of(*m);
        if (actor == nullptr || !actor->is_inside_tree()) {
            if (actor == nullptr) {
                detach(*m);
            }
            continue;
        }
        m->last = SkydotWorld::godot_to_skyrim(actor->get_global_position());
        m->think -= seconds;
        if (m->think <= 0.0) {
            m->think = k_think_interval;
            choose(*m);
        }
        if (drive_ && m->package != 0) {
            steer(*m, *actor, seconds);
        }
    }
    // Those not built: a slice per frame, a whole pass every few game minutes.
    if (place_cursor_ == static_cast<std::size_t>(-1) && since_placed_ >= k_place_interval) {
        since_placed_ = 0.0;
        placed_ = true;
        place_cursor_ = 0;
    }
    if (place_cursor_ != static_cast<std::size_t>(-1)) {
        const auto started = godot::Time::get_singleton()->get_ticks_usec();
        while (place_cursor_ < persistent_.size() &&
               static_cast<std::int64_t>(godot::Time::get_singleton()->get_ticks_usec() - started) < PLACE_BUDGET_USEC) {
            place_one(persistent_[place_cursor_++], true);
        }
        if (place_cursor_ >= persistent_.size()) {
            place_cursor_ = static_cast<std::size_t>(-1);
        }
    }
}

void SkydotAi::next_step(Mind& m) {
    ++m.step;
    m.step_started = false;
    m.target.reset();
    m.door = 0;
    m.failures = 0;
    m.walks = 0;
}

void SkydotAi::leave(Mind& m, SkydotActor& actor, std::uint32_t door) {
    const auto it = world_->doors_.find(door);
    const auto dest = it != world_->doors_.end() ? world_->doors_.find(it->second.second->destination())
                                                 : world_->doors_.end();
    if (it == world_->doors_.end() || dest == world_->doors_.end()) {
        return;
    }
    const auto& p = it->second.second->position();
    const auto space = static_cast<std::uint32_t>(world_->get_cell_space(dest->second.first->id()));
    world_->set_actor_place(m.ref, space, Vector3(p.x(), p.y(), p.z()),
                            static_cast<double>(it->second.second->rotation().z()));
    std::erase(attached_, m.ref);
    m.node = 0;
    m.space = space;
    m.door = 0;
    m.announced = false;
    actor.queue_free();
    // No longer steered: what the rest of its steps do to doors happens now.
    for (auto i = m.step; i < m.steps.size(); ++i) {
        const auto& s = m.steps[i];
        if (s.procedure == ai::Procedure::unlock_doors || s.procedure == ai::Procedure::lock_doors) {
            if (auto spot = s.location != nullptr ? location_spot(m, *s.location) : actor_spot(m)) {
                set_doors_locked(*spot, s.procedure == ai::Procedure::lock_doors);
            }
        }
    }
    emit_signal("actor_left", static_cast<std::int64_t>(m.ref), static_cast<std::int64_t>(door));
}

bool SkydotAi::go(Mind& m, SkydotActor& actor, const Spot& spot, double reach, double seconds) {
    Vector3 to = spot.position;
    bool door = false;
    if (spot.space != m.space) {
        if (!m.persistent) {
            return true; // Not its own: it does not leave its cell.
        }
        const auto [first, last] = route(m.space, spot.space);
        (void)last;
        const auto at = first != 0 ? ref_spot(first) : std::nullopt;
        if (!at) {
            return true; // No way there: as far as it gets.
        }
        if (m.door != first) {
            m.door = first;
            m.walks = 0;
            m.failures = 0;
        }
        to = at->position;
        reach = k_door_reach;
        door = true;
    }
    const double left = horizontal(m.last, to);
    if (left <= reach) {
        if (door) {
            leave(m, actor, m.door);
            return false;
        }
        return true;
    }
    if (actor.is_walking()) {
        return false;
    }
    if (m.walks >= k_max_walks || m.failures >= k_max_walks) {
        // The navmesh ends short of it, or no path leads there: close enough.
        if (door) {
            leave(m, actor, m.door);
            return false;
        }
        return true;
    }
    if (m.retry > 0.0) {
        m.retry -= seconds;
        return false;
    }
    const auto* pkg = ai::find_package(*root(), m.package);
    const bool run = pkg != nullptr && (pkg->flags() & k_use_preferred_speed) != 0 && pkg->speed() == 2;
    if (actor.walk_to(SkydotWorld::skyrim_position(to), run)) {
        ++m.walks;
        m.failures = 0;
    } else {
        ++m.failures;
        m.retry = k_retry;
    }
    return false;
}

void SkydotAi::steer(Mind& m, SkydotActor& actor, double seconds) {
    using ai::Procedure;
    if (m.step >= m.steps.size()) {
        const auto pkg = ai::resolve_package(*root(), m.package);
        if (!m.steps.empty() && ai::repeats(pkg)) {
            m.step = 0;
            m.step_started = false;
        } else if (!m.steps.empty()) {
            // Done: the next package that applies takes over.
            m.completed[m.package] = clock_.days;
            m.think = 0.0;
            actor.set_wander(false);
        }
        return;
    }
    const ai::Step& s = m.steps[m.step];
    if (!m.step_started) {
        m.step_started = true;
        m.step_time = 0.0;
        m.target = step_spot(m, s);
        if (s.procedure == Procedure::patrol && m.target) {
            // Patrol markers: the start, then its linked references in turn.
            m.patrol.clear();
            m.patrol_next = 0;
            std::set<std::uint32_t> seen;
            for (auto r = m.target->ref; r != 0 && !seen.contains(r) && m.patrol.size() < 32; r = linked_ref(r, 0)) {
                seen.insert(r);
                if (auto at = ref_spot(r); at && at->space == m.target->space) {
                    m.patrol.push_back(at->position);
                }
            }
        }
    }
    m.step_time += seconds;
    const bool timed_out = s.seconds > 0.0 && m.step_time >= s.seconds;
    switch (s.procedure) {
    case Procedure::find:
        do_find(m, s);
        next_step(m);
        return;
    case Procedure::unlock_doors:
    case Procedure::lock_doors:
        set_doors_locked(m.target.value_or(actor_spot(m)), s.procedure == Procedure::lock_doors);
        next_step(m);
        return;
    case Procedure::wait:
    case Procedure::other:
        actor.set_wander(false);
        if (timed_out) {
            next_step(m);
        }
        return;
    case Procedure::travel:
        actor.set_wander(false);
        if (!m.target || go(m, actor, *m.target, std::max(m.target->radius, k_arrive), seconds)) {
            next_step(m);
        }
        return;
    case Procedure::sandbox:
    case Procedure::wander:
    case Procedure::guard: {
        if (!m.target) {
            actor.set_wander(true);
        } else {
            const double radius = std::max(m.target->radius, k_min_sandbox);
            const bool there = m.target->space == m.space &&
                               horizontal(m.last, m.target->position) <= radius + 150.0;
            if (!there && !(actor.get_wander() && m.target->space == m.space &&
                            horizontal(m.last, m.target->position) <= radius * 1.5)) {
                actor.set_wander(false);
                go(m, actor, *m.target, radius, seconds);
            } else if (!actor.get_wander()) {
                actor.set_home(SkydotWorld::skyrim_position(m.target->position));
                actor.set_wander_radius(radius * SkydotWorld::UNIT_SCALE);
                actor.set_wander(true);
            }
        }
        if (timed_out) {
            actor.set_wander(false);
            next_step(m);
        }
        return;
    }
    case Procedure::patrol:
        actor.set_wander(false);
        if (m.patrol.empty()) {
            return;
        }
        if (go(m, actor, Spot{m.target->space, m.patrol[m.patrol_next], 0.0F, 0.0, 0}, k_arrive, seconds)) {
            m.walks = 0;
            if (++m.patrol_next >= m.patrol.size()) {
                m.patrol_next = 0;
            }
        }
        return;
    default: // Sleep, sit, eat, idle markers, activate, hold position: go and stay.
        actor.set_wander(false);
        if (m.target) {
            go(m, actor, *m.target, std::max(m.target->radius, k_stand_reach), seconds);
        }
        if (timed_out) {
            next_step(m);
        }
        return;
    }
}

// ---- for tools and tests ---------------------------------------------------

Dictionary SkydotAi::get_actor_state(std::int64_t ref) {
    Dictionary out;
    Mind* m = mind(static_cast<std::uint32_t>(ref));
    if (m == nullptr) {
        return out;
    }
    choose(*m);
    out["npc"] = static_cast<std::int64_t>(m->npc);
    out["package"] = static_cast<std::int64_t>(m->package);
    const auto pkg = ai::resolve_package(*root(), m->package);
    out["package_editor_id"] = String::utf8(pkg.editor_id().c_str());
    out["template"] = pkg.tree != nullptr && pkg.tree->editor_id() != nullptr
                          ? String::utf8(pkg.tree->editor_id()->c_str())
                          : String();
    Array steps;
    for (const auto& s : m->steps) {
        steps.push_back(String(ai::procedure_name(s.procedure)));
    }
    out["steps"] = steps;
    out["step"] = static_cast<std::int64_t>(m->step);
    out["procedure"] = m->step < m->steps.size() ? String(ai::procedure_name(m->steps[m->step].procedure)) : String();
    out["attached"] = node_of(*m) != nullptr;
    out["space"] = static_cast<std::int64_t>(actor_spot(*m).space);
    if (m->target) {
        Dictionary t;
        t["space"] = static_cast<std::int64_t>(m->target->space);
        t["position"] = m->target->position;
        t["radius"] = m->target->radius;
        t["ref"] = static_cast<std::int64_t>(m->target->ref);
        out["target"] = t;
    }
    out["door"] = static_cast<std::int64_t>(m->door);
    return out;
}

Array SkydotAi::get_packages(std::int64_t ref) {
    Array out;
    Mind* m = mind(static_cast<std::uint32_t>(ref));
    if (m == nullptr) {
        return out;
    }
    Host host(*this, *m);
    for (const auto id : ai::package_list(*root(), m->npc, m->ref)) {
        const auto pkg = ai::resolve_package(*root(), id);
        Dictionary d;
        d["id"] = static_cast<std::int64_t>(id);
        if (!pkg) {
            d["missing"] = true;
            out.push_back(d);
            continue;
        }
        d["editor_id"] = String::utf8(pkg.editor_id().c_str());
        d["template"] = pkg.tree != nullptr && pkg.tree->editor_id() != nullptr
                            ? String::utf8(pkg.tree->editor_id()->c_str())
                            : String();
        d["hour"] = static_cast<std::int64_t>(pkg.own->hour());
        d["duration"] = static_cast<std::int64_t>(pkg.own->duration());
        d["day_of_week"] = static_cast<std::int64_t>(pkg.own->day_of_week());
        d["scheduled"] = ai::schedule_active(*pkg.own, clock_);
        d["conditions_pass"] = ai::conditions_pass(pkg.own->conditions(), host, m->ref);
        Array conditions;
        if (const auto* list = pkg.own->conditions()) {
            for (const auto* c : *list) {
                Dictionary cd;
                cd["function"] = static_cast<std::int64_t>(c->function());
                cd["type"] = static_cast<std::int64_t>(c->type());
                cd["param1"] = static_cast<std::int64_t>(c->param1());
                cd["param2"] = static_cast<std::int64_t>(c->param2());
                cd["run_on"] = static_cast<std::int64_t>(c->run_on());
                cd["compare_to"] = static_cast<double>(c->value());
                cd["value"] = host.function_value(*c, c->run_on() == 2 ? c->reference() : m->ref);
                conditions.push_back(cd);
            }
        }
        d["conditions"] = conditions;
        d["chosen"] = id == m->package;
        out.push_back(d);
    }
    return out;
}

Dictionary SkydotAi::get_destination(std::int64_t ref) {
    Dictionary out;
    Mind* m = mind(static_cast<std::uint32_t>(ref));
    if (m == nullptr) {
        return out;
    }
    choose(*m);
    if (auto spot = settled_spot(*m)) {
        out["space"] = static_cast<std::int64_t>(spot->space);
        out["position"] = spot->position;
        out["radius"] = spot->radius;
        out["ref"] = static_cast<std::int64_t>(spot->ref);
    }
    return out;
}

} // namespace skydot
