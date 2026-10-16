// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/papyrus.hpp"

#include "data/coordinates.hpp"
#include "actors/behavior_player.hpp"
#include "build/ref_nodes.hpp"
#include "physics/collision.hpp"
#include "vm/activation.hpp"
#include "vm/natives_common.hpp"
#include "vm/natives_world.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;
using godot::Vector3;

namespace skydot {

namespace {

std::string utf8(const String& s) {
    const auto bytes = s.utf8();
    return {bytes.get_data(), static_cast<std::size_t>(bytes.length())};
}

String to_godot(const std::string& s) {
    return String::utf8(s.c_str(), static_cast<int>(s.size()));
}

bool scalar_type(std::string_view type) {
    return type == "int" || type == "float" || type == "bool" || type == "string";
}

// XPRM types (world.fbs).
constexpr std::uint32_t k_box = 1;
constexpr std::uint32_t k_sphere = 2;

using natives::k_closed;

/// 2 added quests, aliases and globals; 1 is still read.
constexpr std::int32_t k_save_format = 2;

} // namespace

void SkydotPapyrus::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("setup", "pack", "world"), &SkydotPapyrus::setup);
    godot::ClassDB::bind_method(D_METHOD("attach", "cell", "ref"), &SkydotPapyrus::attach);
    godot::ClassDB::bind_method(D_METHOD("attach_built", "root"), &SkydotPapyrus::attach_built);
    godot::ClassDB::bind_method(D_METHOD("attach_cell", "cell"), &SkydotPapyrus::attach_cell);
    godot::ClassDB::bind_method(D_METHOD("activate_ref", "ref", "activator", "default_only", "source", "force"),
                                &SkydotPapyrus::activate_ref);
    godot::ClassDB::bind_method(D_METHOD("is_3d_loaded", "ref"), &SkydotPapyrus::is_3d_loaded_i);
    godot::ClassDB::bind_method(D_METHOD("get_scheduled_count"), &SkydotPapyrus::get_scheduled_count);
    BIND_CONSTANT(ACTIVATE_PLAYER);
    BIND_CONSTANT(ACTIVATE_SCRIPT);
    BIND_CONSTANT(ACTIVATE_PARENT);
    godot::ClassDB::bind_method(D_METHOD("census", "classes"), &SkydotPapyrus::census);
    godot::ClassDB::bind_method(D_METHOD("activate", "ref", "activator"), &SkydotPapyrus::activate,
                                DEFVAL(PLAYER_REF));
    godot::ClassDB::bind_method(D_METHOD("add_item", "owner", "item", "count"), &SkydotPapyrus::add_item);
    godot::ClassDB::bind_method(D_METHOD("send_event", "ref", "event", "args"),
                                &SkydotPapyrus::send_event);
    godot::ClassDB::bind_method(D_METHOD("complete_activation", "ref"), &SkydotPapyrus::complete_activation);
    godot::ClassDB::bind_method(D_METHOD("notify_animation_event", "ref", "event"),
                                &SkydotPapyrus::notify_animation_event);
    godot::ClassDB::bind_method(D_METHOD("notify_sound_finished", "instance"),
                                &SkydotPapyrus::notify_sound_finished);
    godot::ClassDB::bind_method(D_METHOD("update_actor", "actor", "position"),
                                &SkydotPapyrus::update_actor);
    godot::ClassDB::bind_method(D_METHOD("update", "seconds"), &SkydotPapyrus::update);
    godot::ClassDB::bind_method(D_METHOD("call_global", "cls", "fn", "args"),
                                &SkydotPapyrus::call_global);
    godot::ClassDB::bind_method(D_METHOD("create_instance", "cls"), &SkydotPapyrus::create_instance);
    godot::ClassDB::bind_method(D_METHOD("call_method", "form", "cls", "fn", "args"),
                                &SkydotPapyrus::call_method);
    godot::ClassDB::bind_method(D_METHOD("take_result", "thread"), &SkydotPapyrus::take_result);
    godot::ClassDB::bind_method(D_METHOD("get_variable", "form", "cls", "name"),
                                &SkydotPapyrus::get_variable);
    godot::ClassDB::bind_method(D_METHOD("get_scripts", "ref"), &SkydotPapyrus::get_scripts);
    godot::ClassDB::bind_method(D_METHOD("is_ref_disabled", "ref"), &SkydotPapyrus::is_ref_disabled);
    godot::ClassDB::bind_method(D_METHOD("get_disabled_changes"),
                                &SkydotPapyrus::get_disabled_changes);
    godot::ClassDB::bind_method(D_METHOD("is_activation_blocked", "ref"),
                                &SkydotPapyrus::is_activation_blocked);
    godot::ClassDB::bind_method(D_METHOD("get_lock_level", "ref"), &SkydotPapyrus::get_lock_level);
    godot::ClassDB::bind_method(D_METHOD("set_locked", "ref", "locked"), &SkydotPapyrus::set_locked);
    godot::ClassDB::bind_method(D_METHOD("get_open_state", "ref"), &SkydotPapyrus::get_open_state);
    godot::ClassDB::bind_method(D_METHOD("set_open_state", "ref", "state"),
                                &SkydotPapyrus::set_open_state);
    godot::ClassDB::bind_method(D_METHOD("get_trigger_count", "ref"),
                                &SkydotPapyrus::get_trigger_count);
    godot::ClassDB::bind_method(D_METHOD("get_triggers"), &SkydotPapyrus::get_triggers);
    godot::ClassDB::bind_method(D_METHOD("start_game_enabled_quests"),
                                &SkydotPapyrus::start_game_enabled_quests);
    godot::ClassDB::bind_method(D_METHOD("start_quest", "quest"), &SkydotPapyrus::start_quest);
    godot::ClassDB::bind_method(D_METHOD("stop_quest", "quest"), &SkydotPapyrus::stop_quest);
    godot::ClassDB::bind_method(D_METHOD("set_stage", "quest", "stage"), &SkydotPapyrus::set_stage);
    godot::ClassDB::bind_method(D_METHOD("get_quest_state", "quest"), &SkydotPapyrus::get_quest_state);
    godot::ClassDB::bind_method(D_METHOD("get_running_quests"), &SkydotPapyrus::get_running_quests);
    godot::ClassDB::bind_method(D_METHOD("get_alias_ref", "quest", "alias"),
                                &SkydotPapyrus::get_alias_ref);
    godot::ClassDB::bind_method(D_METHOD("get_global_value", "global"),
                                &SkydotPapyrus::get_global_value);
    godot::ClassDB::bind_method(D_METHOD("set_global_value", "global", "value"),
                                &SkydotPapyrus::set_global_value);
    godot::ClassDB::bind_method(D_METHOD("set_animation_state", "ref", "bytes"),
                                &SkydotPapyrus::set_animation_state);
    godot::ClassDB::bind_method(D_METHOD("get_animation_state", "ref"), &SkydotPapyrus::get_animation_state);
    godot::ClassDB::bind_method(D_METHOD("remove_placed", "ref"), &SkydotPapyrus::remove_placed);
    godot::ClassDB::bind_method(D_METHOD("forget_ref", "ref"), &SkydotPapyrus::forget_ref);
    godot::ClassDB::bind_method(D_METHOD("damage_ref", "ref", "amount"), &SkydotPapyrus::damage_ref);
    godot::ClassDB::bind_method(D_METHOD("ref_built", "ref"), &SkydotPapyrus::ref_built);
    godot::ClassDB::bind_method(D_METHOD("get_ref_base", "ref"), &SkydotPapyrus::get_ref_base);
    godot::ClassDB::bind_method(D_METHOD("save_state"), &SkydotPapyrus::save_state);
    godot::ClassDB::bind_method(D_METHOD("load_state", "bytes"), &SkydotPapyrus::load_state);
    godot::ClassDB::bind_method(D_METHOD("get_last_error"), &SkydotPapyrus::get_last_error);
    godot::ClassDB::bind_method(D_METHOD("get_error_count"), &SkydotPapyrus::get_error_count);
    godot::ClassDB::bind_method(D_METHOD("get_instruction_count"),
                                &SkydotPapyrus::get_instruction_count);
    godot::ClassDB::bind_method(D_METHOD("get_thread_count"), &SkydotPapyrus::get_thread_count);
    godot::ClassDB::bind_method(D_METHOD("get_time"), &SkydotPapyrus::get_time);
    using godot::PropertyInfo;
    ADD_SIGNAL(godot::MethodInfo("trace", PropertyInfo(Variant::STRING, "text")));
    ADD_SIGNAL(godot::MethodInfo("message", PropertyInfo(Variant::STRING, "text"),
                                 PropertyInfo(Variant::BOOL, "box")));
    ADD_SIGNAL(godot::MethodInfo("error", PropertyInfo(Variant::STRING, "text")));
    ADD_SIGNAL(godot::MethodInfo("enable_changed", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::BOOL, "enabled")));
    ADD_SIGNAL(godot::MethodInfo("play_animation", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::STRING, "animation")));
    ADD_SIGNAL(godot::MethodInfo("havok_impulse", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::VECTOR3, "direction"),
                                 PropertyInfo(Variant::FLOAT, "magnitude")));
    // Weapon.Fire: the weapon, the source reference and the ammo (0 for None).
    ADD_SIGNAL(godot::MethodInfo("fire_weapon", PropertyInfo(Variant::INT, "weapon"),
                                 PropertyInfo(Variant::INT, "source"),
                                 PropertyInfo(Variant::INT, "ammo")));
    // Sound.Play and PlayAndWait: the SNDR or SOUN, the source reference (0 for
    // None) and the playback instance; StopInstance and SetInstanceVolume name
    // an instance.
    ADD_SIGNAL(godot::MethodInfo("play_sound", PropertyInfo(Variant::INT, "sound"),
                                 PropertyInfo(Variant::INT, "source"),
                                 PropertyInfo(Variant::INT, "instance")));
    ADD_SIGNAL(godot::MethodInfo("stop_sound", PropertyInfo(Variant::INT, "instance")));
    ADD_SIGNAL(godot::MethodInfo("sound_volume", PropertyInfo(Variant::INT, "instance"),
                                 PropertyInfo(Variant::FLOAT, "volume")));
    // ObjectReference.Reset: the reference goes to a position (Skyrim space,
    // game units) and rotation (radians) the game layer applies.
    ADD_SIGNAL(godot::MethodInfo("ref_moved", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::VECTOR3, "position"),
                                 PropertyInfo(Variant::VECTOR3, "rotation")));
    // PlaceAtMe: a runtime reference of `base` in `cell` at a position (Skyrim
    // space) and rotation (radians) for the game layer to build; ref_deleted
    // when it goes (Delete, an effect that ended, a loaded save).
    ADD_SIGNAL(godot::MethodInfo("ref_placed", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::INT, "base"), PropertyInfo(Variant::INT, "cell"),
                                 PropertyInfo(Variant::VECTOR3, "position"),
                                 PropertyInfo(Variant::VECTOR3, "rotation"),
                                 PropertyInfo(Variant::FLOAT, "scale"), PropertyInfo(Variant::BOOL, "disabled")));
    ADD_SIGNAL(godot::MethodInfo("ref_deleted", PropertyInfo(Variant::INT, "ref")));
    // A destructible reference opened a stage with a model: the game layer
    // swaps to it ("" and stage 0: back to the base's own model).
    ADD_SIGNAL(godot::MethodInfo("destruction_model", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::STRING, "model"), PropertyInfo(Variant::INT, "stage")));
    ADD_SIGNAL(godot::MethodInfo("motion_type_changed", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::INT, "motion_type")));
    // A script's or an activate parent's activation, once the VM ran it: the
    // outcome as activate_ref describes it.
    ADD_SIGNAL(godot::MethodInfo("activation_outcome", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::DICTIONARY, "outcome")));
    ADD_SIGNAL(godot::MethodInfo("open_changed", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::BOOL, "open")));
    ADD_SIGNAL(godot::MethodInfo("lock_changed", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::BOOL, "locked")));
    ADD_SIGNAL(godot::MethodInfo("effect_shader", PropertyInfo(Variant::INT, "shader"),
                                 PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::BOOL, "playing")));
    ADD_SIGNAL(godot::MethodInfo("trigger", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::INT, "actor"),
                                 PropertyInfo(Variant::BOOL, "entered")));
    ADD_SIGNAL(godot::MethodInfo("quest_started", PropertyInfo(Variant::INT, "quest")));
    ADD_SIGNAL(godot::MethodInfo("quest_stopped", PropertyInfo(Variant::INT, "quest")));
    ADD_SIGNAL(godot::MethodInfo("quest_stage", PropertyInfo(Variant::INT, "quest"),
                                 PropertyInfo(Variant::INT, "stage"),
                                 PropertyInfo(Variant::STRING, "text")));
    ADD_SIGNAL(godot::MethodInfo("objective_changed", PropertyInfo(Variant::INT, "quest"),
                                 PropertyInfo(Variant::INT, "index"),
                                 PropertyInfo(Variant::STRING, "state"),
                                 PropertyInfo(Variant::STRING, "text")));
    BIND_CONSTANT(PLAYER_REF);
    BIND_CONSTANT(ALIAS_HANDLE_BASE);
}

SkydotPapyrus::SkydotPapyrus() : vm_(std::make_unique<vm::Vm>()) { install_scheduled_handler(); }

/// Host calls scheduled on the VM's clock: the activations of Activate and of
/// activate children.
void SkydotPapyrus::install_scheduled_handler() {
    vm_->set_scheduled_handler([this](const std::string& kind, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (kind == "activate") {
            const Dictionary outcome = activate_ref(a, b, (c & 0x100u) != 0, c & 0xFFu, false);
            emit_signal("activation_outcome", static_cast<std::int64_t>(a), outcome);
        }
    });
}

void SkydotPapyrus::reset() {
    attached_.clear();
    attached_cells_.clear();
    states_.clear();
    pending_activations_.clear();
    triggers_.clear();
    actors_.clear();
    next_form_ = 0xFF000000u;
    quests_.clear();
    quest_scripts_.clear();
    alias_handles_.clear();
    handle_alias_.clear();
    alias_fill_.clear();
    globals_.clear();
    next_alias_handle_ = static_cast<std::uint32_t>(ALIAS_HANDLE_BASE);
    quest_info_.clear();
}

godot::Error SkydotPapyrus::setup(const godot::Ref<SkydotPack>& pack,
                                  const godot::Ref<SkydotWorld>& world) {
    if (pack.is_null() || world.is_null()) {
        return godot::ERR_INVALID_PARAMETER;
    }
    pack_ = pack;
    world_ = world;
    vm_ = std::make_unique<vm::Vm>();
    install_scheduled_handler();
    reset();
    // Scripts keep their virtual paths: class Foo is scripts/foo.pex.
    vm_->set_loader([this](const std::string& key) -> std::optional<std::vector<std::uint8_t>> {
        const auto bytes = pack_->get_bytes(to_godot("scripts/" + key + ".pex"));
        if (bytes.is_empty()) {
            return std::nullopt;
        }
        return std::vector<std::uint8_t>(bytes.ptr(), bytes.ptr() + bytes.size());
    });
    vm_->set_log([this](const std::string& message) {
        godot::UtilityFunctions::push_warning(String("Papyrus: ") + to_godot(message));
        emit_signal("error", to_godot(message));
    });
    bind_natives();
    return godot::OK;
}

namespace {
Vector3 vec3(const double (&v)[3]) {
    return Vector3(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
}
} // namespace

// ---- the world as scripts see it ----------------------------------------------

Dictionary SkydotPapyrus::ref_info(std::uint32_t ref) const {
    if (world_.is_null()) {
        return {};
    }
    if (const auto* placed = vm_->placed().find(ref)) {
        return placed_info(*placed);
    }
    return world_->get_ref_info(world_->get_ref_cell(ref), ref);
}

bool SkydotPapyrus::disabled(std::uint32_t ref, int depth) const {
    if (const auto it = states_.disabled.find(ref); it != states_.disabled.end()) {
        return it->second;
    }
    const Dictionary info = ref_info(ref);
    // Follow a script-changed enable parent; otherwise the world's initial state.
    const auto parent = static_cast<std::uint32_t>(static_cast<std::int64_t>(info.get("enable_parent", 0)));
    if (parent != 0 && depth < 16 && world_.is_valid() && world_->get_ref_cell(parent) != 0) {
        return disabled(parent, depth + 1) != static_cast<bool>(info.get("enable_opposite", false));
    }
    return static_cast<bool>(info.get("disabled", false));
}

void SkydotPapyrus::set_disabled(std::uint32_t ref, bool value) {
    // The reference and its enable children, whose state follows it.
    std::vector<std::pair<std::uint32_t, bool>> before{{ref, disabled(ref)}};
    for (std::size_t i = 0; i < before.size() && world_.is_valid() && before.size() < 4096; ++i) {
        for (const std::int64_t child : world_->get_enable_children(before[i].first)) {
            const auto id = static_cast<std::uint32_t>(child);
            if (!states_.disabled.contains(id)) {
                before.emplace_back(id, disabled(id));
            }
        }
    }
    states_.disabled[ref] = value;
    for (const auto& [id, was] : before) {
        if (const bool now = disabled(id); now != was) {
            emit_signal("enable_changed", static_cast<std::int64_t>(id), !now);
        }
    }
}

std::uint32_t SkydotPapyrus::lock_holder(std::uint32_t ref) const {
    // The two doors of a load door share one lock, and the plugin stores it
    // on one side only (Faendal's house: the inside door). A door without a
    // lock of its own answers with its partner's.
    const Dictionary info = ref_info(ref);
    if (info.get("lock", Variant()).get_type() == Variant::DICTIONARY) {
        return ref;
    }
    const Variant door = info.get("door", Variant());
    if (door.get_type() != Variant::DICTIONARY) {
        return ref;
    }
    const auto other =
        static_cast<std::uint32_t>(static_cast<std::int64_t>(Dictionary(door).get("destination", 0)));
    if (other != 0 && ref_info(other).get("lock", Variant()).get_type() == Variant::DICTIONARY) {
        return other;
    }
    return ref;
}

bool SkydotPapyrus::locked(std::uint32_t ref) const {
    ref = lock_holder(ref);
    if (const auto it = states_.locked.find(ref); it != states_.locked.end()) {
        return it->second;
    }
    // A reference with XLOC starts locked.
    return ref_info(ref).get("lock", Variant()).get_type() == Variant::DICTIONARY;
}

Vector3 SkydotPapyrus::rotation_of(std::uint32_t ref) const {
    if (const auto it = states_.moved.find(ref); it != states_.moved.end()) {
        return vec3(it->second.rotation);
    }
    return ref_info(ref).get("rotation", Vector3());
}

Vector3 SkydotPapyrus::position_of(std::uint32_t ref) const {
    if (const auto it = states_.moved.find(ref); it != states_.moved.end()) {
        return vec3(it->second.position);
    }
    if (const auto it = actors_.find(ref); it != actors_.end()) {
        return it->second;
    }
    const Dictionary info = ref_info(ref);
    if (info.is_empty() && world_.is_valid()) {
        return world_->get_actor(ref).get("position", Vector3()); // a placed actor
    }
    return info.get("position", Vector3());
}

/// The world as natives ask about it: the Dictionaries behind it stay private
/// to SkydotPapyrus.
struct SkydotPapyrus::Access final : vm::WorldAccess {
    explicit Access(const SkydotPapyrus& owner) : owner_(owner) {}

    std::uint32_t base_form(std::uint32_t ref) const override {
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(owner_.ref_info(ref).get("base", 0)));
    }
    std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const override {
        const Array links = owner_.ref_info(ref).get("links", Array());
        for (std::int64_t i = 0; i < links.size(); ++i) {
            const Dictionary link = links[i];
            if (static_cast<std::int64_t>(link["keyword"]) == keyword) {
                return static_cast<std::uint32_t>(static_cast<std::int64_t>(link["target"]));
            }
        }
        return 0;
    }
    std::uint32_t parent_cell(std::uint32_t ref) const override {
        return owner_.cell_of(ref);
    }
    bool cell_interior(std::uint32_t cell) const override {
        return !owner_.world_.is_null() && static_cast<bool>(owner_.world_->get_cell(cell).get("interior", false));
    }
    bool cell_attached(std::uint32_t cell) const override { return owner_.attached_cells_.contains(cell); }
    bool has_keyword(std::uint32_t form, std::uint32_t keyword) const override {
        if (owner_.world_.is_null()) {
            return false;
        }
        // A reference answers for its base; anything else is taken for a base.
        const auto placed = static_cast<std::uint32_t>(static_cast<std::int64_t>(owner_.ref_info(form).get("base", 0)));
        const godot::PackedInt64Array keywords = owner_.world_->get_base_keywords(placed != 0 ? placed : form);
        return keywords.has(static_cast<std::int64_t>(keyword));
    }
    std::uint32_t find_keyword(const std::string& editor_id) const override {
        return owner_.world_.is_null() ? 0 : static_cast<std::uint32_t>(owner_.world_->find_keyword(to_godot(editor_id)));
    }
    bool form_list(std::uint32_t list, std::vector<std::uint32_t>& forms) const override {
        const Dictionary info = owner_.world_.is_null() ? Dictionary() : owner_.world_->get_form_list(list);
        if (info.is_empty()) {
            return false;
        }
        const godot::PackedInt64Array ids = info["forms"];
        for (const std::int64_t id : ids) {
            forms.push_back(static_cast<std::uint32_t>(id));
        }
        return true;
    }
    bool message(std::uint32_t id, vm::MessageText& out) const override {
        const Dictionary info = owner_.world_.is_null() ? Dictionary() : owner_.world_->get_message(id);
        if (info.is_empty()) {
            return false;
        }
        out.title = utf8(info["title"]);
        out.text = utf8(info["text"]);
        out.box = info["box"];
        const Array buttons = info["buttons"];
        for (std::int64_t i = 0; i < buttons.size(); ++i) {
            out.buttons.push_back(utf8(buttons[i]));
        }
        return true;
    }
    std::int32_t player_level() const override { return 1; }

private:
    const SkydotPapyrus& owner_;
};

void SkydotPapyrus::bind_world_natives() {
    access_ = std::make_unique<Access>(*this);
    vm::bind_world_natives(*vm_, *access_);
}

void SkydotPapyrus::bind_natives() {
    // One registration per class, each in vm/natives_<class>.cpp.
    using Registration = void (SkydotPapyrus::*)();
    static constexpr Registration table[] = {
        &SkydotPapyrus::bind_debug_natives,
        &SkydotPapyrus::bind_utility_natives,
        &SkydotPapyrus::bind_game_natives,
        &SkydotPapyrus::bind_form_natives,
        &SkydotPapyrus::bind_objectreference_natives,
        &SkydotPapyrus::bind_effectshader_natives,
        &SkydotPapyrus::bind_weapon_natives,
        &SkydotPapyrus::bind_sound_natives,
        &SkydotPapyrus::bind_quest_natives,
        &SkydotPapyrus::bind_noop_natives,
        &SkydotPapyrus::bind_world_natives,
        &SkydotPapyrus::bind_runtime_natives,
    };
    for (const Registration registration : table) {
        (this->*registration)();
    }
}

// ---- values -------------------------------------------------------------------

vm::Value SkydotPapyrus::to_value(const Variant& v, std::string_view type, std::uint32_t owner_quest) {
    switch (v.get_type()) {
    case Variant::INT: {
        const auto n = static_cast<std::int64_t>(v);
        if (!type.empty() && !scalar_type(type) && !type.ends_with("[]")) {
            // What a property names has its scripts, loaded or not: in the
            // game such references are persistent, and quests exist before
            // they start.
            const auto form = static_cast<std::uint32_t>(n);
            attach_quest_scripts(form, true);
            if (!attached_.contains(form) && world_.is_valid()) {
                if (const auto cell = world_->get_ref_cell(form); cell != 0) {
                    attach(cell, form);
                }
            }
            return vm::Value::object(static_cast<std::uint32_t>(n), vm_->load_class(type));
        }
        return vm::Value::integer(static_cast<std::int32_t>(n));
    }
    case Variant::FLOAT: return vm::Value::floating(static_cast<float>(static_cast<double>(v)));
    case Variant::BOOL: return vm::Value::boolean(static_cast<bool>(v));
    case Variant::STRING:
    case Variant::STRING_NAME: return vm::Value::string(utf8(v));
    case Variant::ARRAY: {
        const Array list = v;
        const auto element = type.ends_with("[]") ? type.substr(0, type.size() - 2) : std::string_view{};
        auto out = std::make_shared<vm::Array>();
        for (std::int64_t i = 0; i < list.size(); ++i) {
            out->push_back(to_value(list[i], element, owner_quest));
        }
        return vm::Value::make_array(out);
    }
    case Variant::DICTIONARY: {
        // A quest alias: {form, alias}; form 0 is the quest whose script this is.
        const Dictionary d = v;
        auto quest = static_cast<std::uint32_t>(static_cast<std::int64_t>(d.get("form", 0)));
        if (quest == 0) {
            quest = owner_quest;
        }
        const auto alias = static_cast<std::int64_t>(d.get("alias", -1));
        if (quest == 0 || alias < 0) {
            return {};
        }
        attach_quest_scripts(quest, true);
        return vm::Value::object(alias_handle(quest, static_cast<std::uint32_t>(alias)),
                                 vm_->load_class(type.empty() ? std::string_view("Alias") : type));
    }
    default:
        return {};
    }
}

Variant SkydotPapyrus::to_variant(const vm::Value& v) const {
    switch (v.kind()) {
    case vm::Kind::none: return {};
    case vm::Kind::integer: return v.i();
    case vm::Kind::floating: return v.f();
    case vm::Kind::boolean: return v.b();
    case vm::Kind::string: return to_godot(v.s());
    case vm::Kind::object: return static_cast<std::int64_t>(v.form());
    case vm::Kind::array: {
        Array out;
        for (const auto& e : *v.array()) {
            out.push_back(to_variant(e));
        }
        return out;
    }
    }
    return {};
}

// ---- attaching and events -------------------------------------------------

std::int64_t SkydotPapyrus::attach(std::int64_t cell, std::int64_t ref) {
    const auto form = static_cast<std::uint32_t>(ref);
    if (world_.is_null() || !attached_.insert(form).second) {
        return 0;
    }
    const Dictionary info = world_->get_ref_info(cell, ref);
    const std::int64_t count = attach_scripts(form, info.get("scripts", Array()), 0);
    if (count != 0) {
        register_trigger(form, info);
        vm_->send_event(form, "OnInit", {});
    }
    return count;
}

std::int64_t SkydotPapyrus::attach_scripts(std::uint32_t form, const Array& scripts,
                                           std::uint32_t owner_quest) {
    // Base scripts first; a reference's entry of the same name overrides its
    // status and property values.
    std::vector<std::string> order;
    std::map<std::string, std::pair<bool, Dictionary>> merged; // removed, properties
    for (std::int64_t i = 0; i < scripts.size(); ++i) {
        const Dictionary s = scripts[i];
        const std::string name = utf8(s["name"]);
        const std::string key = vm::to_lower(name);
        auto [it, fresh] =
            merged.try_emplace(key, std::pair<bool, Dictionary>{false, Dictionary()});
        if (fresh) {
            order.push_back(name);
        }
        it->second.first = s["removed"];
        it->second.second.merge(s["properties"], true);
    }
    std::int64_t count = 0;
    for (const auto& name : order) {
        const auto& [removed, properties] = merged[vm::to_lower(name)];
        if (removed) {
            continue;
        }
        auto* instance = vm_->attach(form, name);
        if (instance == nullptr) {
            continue;
        }
        const Array keys = properties.keys();
        for (std::int64_t k = 0; k < keys.size(); ++k) {
            const std::string pname = vm::to_lower(utf8(keys[k]));
            std::string type;
            for (const auto* c = instance->cls; c != nullptr && type.empty(); c = c->parent()) {
                if (const auto* p = c->property(pname)) {
                    type = p->type;
                }
            }
            if (type.empty() ||
                !vm_->set_property(instance, pname, to_value(properties[keys[k]], type, owner_quest))) {
                vm_->error(name + ": no property " + pname);
            }
        }
        ++count;
    }
    return count;
}

std::int64_t SkydotPapyrus::attach_built(godot::Node* root) {
    if (root == nullptr) {
        return 0;
    }
    std::int64_t scripted = 0;
    std::vector<godot::Node*> pending{root};
    while (!pending.empty()) {
        godot::Node* node = pending.back();
        pending.pop_back();
        for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
            godot::Node* child = node->get_child(i);
            if (!child->has_meta("skydot_ref")) {
                pending.push_back(child);
                continue;
            }
            const std::int64_t ref = child->get_meta("skydot_ref");
            attach(child->get_meta("skydot_cell"), ref);
            attached_cells_.insert(static_cast<std::uint32_t>(static_cast<std::int64_t>(child->get_meta("skydot_cell"))));
            if (!vm_->instances(static_cast<std::uint32_t>(ref)).empty()) {
                register_phantoms(static_cast<std::uint32_t>(ref),
                                  godot::Object::cast_to<godot::Node3D>(child));
            }
            // The game sends these as a reference's cell attaches, before OnLoad.
            dispatch(static_cast<std::uint32_t>(ref), "OnCellAttach", {});
            dispatch(static_cast<std::uint32_t>(ref), "OnCellLoad", {});
            dispatch(static_cast<std::uint32_t>(ref), "OnLoad", {});
            if (!vm_->instances(static_cast<std::uint32_t>(ref)).empty()) {
                ++scripted;
            }
        }
    }
    return scripted;
}

std::int64_t SkydotPapyrus::attach_cell(std::int64_t cell) {
    if (world_.is_null()) {
        return 0;
    }
    attached_cells_.insert(static_cast<std::uint32_t>(cell));
    std::int64_t count = 0;
    const auto refs = world_->get_scripted_refs(cell);
    for (std::int64_t i = 0; i < refs.size(); ++i) {
        count += attach(cell, refs[i]) > 0 ? 1 : 0;
    }
    return count;
}

Array SkydotPapyrus::census(const godot::PackedStringArray& classes) {
    std::vector<std::string> names;
    for (const String& name : classes) {
        names.push_back(utf8(name));
    }
    Array out;
    for (const auto& entry : vm_->census(names)) {
        Dictionary row;
        row["class"] = to_godot(entry.cls);
        row["function"] = to_godot(entry.fn);
        row["sites"] = static_cast<std::int64_t>(entry.sites);
        row["bound"] = entry.bound;
        row["ignored"] = entry.ignored;
        out.push_back(row);
    }
    return out;
}

std::int64_t SkydotPapyrus::activate(std::int64_t ref, std::int64_t activator) {
    return static_cast<std::int64_t>(dispatch(
        static_cast<std::uint32_t>(ref), "OnActivate",
        {vm_->object(static_cast<std::uint32_t>(activator), "ObjectReference")}));
}

bool SkydotPapyrus::is_3d_loaded(std::uint32_t ref) const {
    return ref_nodes::find(ref) != nullptr && !disabled(ref);
}

// The rules are in vm/activation.hpp; this runs them and what follows.
Dictionary SkydotPapyrus::activate_ref(std::int64_t ref, std::int64_t activator, bool default_only,
                                       std::int64_t source, bool force) {
    Dictionary out;
    out["ref"] = ref;
    out["kind"] = "missing";
    out["note"] = String();
    out["scripts"] = 0;
    const auto form = static_cast<std::uint32_t>(ref);
    const Dictionary info = ref_info(form);
    if (info.is_empty()) {
        return out;
    }
    const String label = String("0x%08X %s (%s)") % Array::make(ref, info["editor_id"], info["type"]);
    out["label"] = label;
    ActivationRequest request;
    request.source = static_cast<ActivationSource>(std::clamp<std::int64_t>(source, 0, 2));
    request.parent_activate_only = static_cast<bool>(info.get("parent_activate_only", false));
    request.lock_level = static_cast<int>(get_lock_level(ref));
    request.force = force;
    switch (activation_gate(request)) {
    case ActivationGate::parent_only:
        out["kind"] = "parent_only";
        out["note"] = label + String(" only responds to its activate parents");
        return out;
    case ActivationGate::locked:
        out["kind"] = "locked";
        out["lock_level"] = request.lock_level;
        out["note"] = String("%s is locked (level %d, %s)") %
                      Array::make(label, request.lock_level, String::utf8(lock_level_name(request.lock_level).c_str()));
        return out;
    case ActivationGate::pass:
        break;
    }
    if (activation_unlocks(request)) {
        set_locked(ref, false);
    }
    // A lever with a behaviour graph activates when the graph says so: the
    // scripts and activate children wait for its ObjectActivated.
    if (activation_waits_for_graph(utf8(String(info["type"])), request.source, graph_raises_activation(form))) {
        if (!pending_activations_.begin(form, {activator != 0 ? activator : PLAYER_REF, default_only})) {
            out["kind"] = "busy";
            out["note"] = label + String(" is still being used");
            return out;
        }
        out["kind"] = "furniture";
        out["event"] = "LeverPushStart";
        out["deferred"] = true;
        out["note"] = label + String(": nothing happens");
        return out;
    }
    if (!run_activation(form, label, activator, default_only, out)) {
        return out;
    }
    out["kind"] = "none";
    const Variant door = info.get("door", Variant());
    if (activation_picks_up(utf8(String(info["type"])))) {
        // Loose items go into the activator's inventory (XCNT is not in the
        // reference info yet: one of them).
        const auto count = static_cast<std::int64_t>(info.get("count", 1));
        vm_->inventory().add(static_cast<std::uint32_t>(activator != 0 ? activator : PLAYER_REF),
                             static_cast<std::uint32_t>(static_cast<std::int64_t>(info["base"])), std::max<std::int64_t>(count, 1));
        set_disabled(form, true);
        out["kind"] = "pickup";
        out["note"] = label + String(": picked up");
        return out;
    }
    if (door.get_type() == Variant::DICTIONARY) {
        out["kind"] = "door";
        out["door"] = door;
    } else if (String(info["type"]) == "FURN") {
        // A lever is furniture: using it pushes it (the player's push
        // animation, which sends Lever_PlayerPush in the game, does not exist).
        out["kind"] = "furniture";
        out["event"] = "LeverPushStart";
    } else if (String(info["type"]) == "DOOR") {
        out["kind"] = "toggle_open";
    }
    out["note"] = label + String(": nothing happens"); // if the host can do nothing with `kind`
    return out;
}

// OnActivate, the blocked check and the activate children: what an activation
// does once it is going ahead. False if scripts blocked it (`out` says so).
bool SkydotPapyrus::run_activation(std::uint32_t form, const String& label, std::int64_t activator,
                                   bool default_only, Dictionary& out) {
    if (!default_only) {
        out["scripts"] = activate(form, activator != 0 ? activator : PLAYER_REF);
    }
    // A script that blocks activation handles it alone, and its activate
    // children stay quiet.
    if (is_activation_blocked(form)) {
        out["kind"] = "blocked";
        out["note"] = label + String(": activation blocked, only its scripts ran");
        return false;
    }
    // Activate children run on the VM's clock, activated by the parent: trap
    // scripts read the parent's trigger type from their activator (BladeTrap
    // casts it to TrapTriggerBase; a pull chain's type 2 stops the blades).
    const Array children = world_->get_activate_children(form);
    for (std::int64_t i = 0; i < children.size(); ++i) {
        const Dictionary child = children[i];
        vm_->schedule(static_cast<double>(child.get("delay", 0.0)), "activate",
                      static_cast<std::uint32_t>(static_cast<std::int64_t>(child["ref"])),
                      form, static_cast<std::uint32_t>(ActivationSource::parent));
    }
    return true;
}

bool SkydotPapyrus::graph_raises_activation(std::uint32_t form) const {
    godot::Node* node = ref_nodes::find(form);
    if (node == nullptr) {
        return false;
    }
    const auto* graph = godot::Object::cast_to<SkydotBehavior>(node->find_child("SkydotBehavior", true, false));
    return graph != nullptr && graph->raises_event("ObjectActivated");
}

void SkydotPapyrus::complete_activation(std::int64_t ref) {
    const auto form = static_cast<std::uint32_t>(ref);
    const auto taken = pending_activations_.take(form);
    if (!taken) {
        return;
    }
    const PendingActivations::Entry pending = *taken;
    Dictionary out;
    out["ref"] = ref;
    out["kind"] = "none";
    out["scripts"] = 0;
    out["note"] = String();
    const Dictionary info = ref_info(form);
    out["label"] = String("0x%08X %s (%s)") % Array::make(ref, info.get("editor_id", ""), info.get("type", ""));
    run_activation(form, out["label"], pending.activator, pending.default_only, out);
    emit_signal("activation_outcome", ref, out);
}

void SkydotPapyrus::add_item(std::int64_t owner, std::int64_t item, std::int64_t count) {
    vm_->inventory().add(static_cast<std::uint32_t>(owner), static_cast<std::uint32_t>(item), count);
}

std::int64_t SkydotPapyrus::send_event(std::int64_t ref, const String& event, const Array& args) {
    std::vector<vm::Value> values;
    for (std::int64_t i = 0; i < args.size(); ++i) {
        values.push_back(to_value(args[i], args[i].get_type() == Variant::INT ? "objectreference" : ""));
    }
    return static_cast<std::int64_t>(dispatch(static_cast<std::uint32_t>(ref), utf8(event), values));
}

void SkydotPapyrus::notify_animation_event(std::int64_t ref, const String& event) {
    if (event == String("ObjectActivated")) {
        complete_activation(ref);
    }
    vm_->notify("anim:" + std::to_string(static_cast<std::uint32_t>(ref)) + ":" + utf8(event));
}

void SkydotPapyrus::notify_sound_finished(std::int64_t instance) {
    vm_->notify("sound:" + std::to_string(instance));
}

// ---- triggers -------------------------------------------------------------

// A box's bounds are half extents: xEdit shows XPRM bounds doubled. A sphere's
// radius is its x. Neither is checked against the game yet.
void SkydotPapyrus::register_trigger(std::uint32_t ref, const Dictionary& info) {
    const Variant primitive = info.get("primitive", Variant());
    if (primitive.get_type() != Variant::DICTIONARY) {
        return;
    }
    const Dictionary p = primitive;
    const auto type = static_cast<std::uint32_t>(static_cast<std::int64_t>(p["type"]));
    if (type != k_box && type != k_sphere) {
        return;
    }
    const Vector3 r = info.get("rotation", Vector3());
    // As SkydotWorld::skyrim_transform: clockwise angles, Z first, then Y, X.
    const godot::Basis rx(Vector3(1, 0, 0), -r.x);
    const godot::Basis ry(Vector3(0, 1, 0), -r.y);
    const godot::Basis rz(Vector3(0, 0, 1), -r.z);
    const auto scale = static_cast<godot::real_t>(static_cast<double>(info.get("scale", 1.0)));
    Trigger t;
    t.origin = info.get("position", Vector3());
    t.to_local = (rx * ry * rz).transposed();
    t.half = Vector3(p["bounds"]) * scale;
    t.type = type;
    if (const auto old = triggers_.find(ref); old != triggers_.end()) {
        t.volumes = std::move(old->second.volumes);
    }
    triggers_[ref] = std::move(t);
}

bool SkydotPapyrus::contains(const Volume& v, const Vector3& p) {
    const Vector3 local = v.to_local.xform(skydot::skyrim_position(p));
    if (v.sphere) {
        return local.length() <= v.half.x;
    }
    return std::abs(local.x) <= v.half.x && std::abs(local.y) <= v.half.y &&
           std::abs(local.z) <= v.half.z;
}

// The model's phantoms (see physics/collision.hpp) become volumes of the ref
// that carries the model. `node` is the built reference; its transform is the
// placement, like an XPRM trigger's.
void SkydotPapyrus::register_phantoms(std::uint32_t ref, godot::Node3D* node) {
    if (node == nullptr || !node->has_meta(k_phantoms_meta)) {
        return;
    }
    const Variant meta = node->get_meta(k_phantoms_meta);
    if (meta.get_type() != Variant::ARRAY) {
        return;
    }
    std::vector<Volume> volumes;
    const godot::Array list = meta;
    for (std::int64_t i = 0; i < list.size(); ++i) {
        if (list[i].get_type() != Variant::DICTIONARY) {
            continue;
        }
        const Dictionary d = list[i];
        Volume v;
        v.to_local = (node->get_transform() * godot::Transform3D(d["transform"])).affine_inverse();
        v.half = d["half"];
        v.sphere = String(d["kind"]) == "sphere";
        volumes.push_back(v);
    }
    if (volumes.empty()) {
        return;
    }
    auto [it, fresh] = triggers_.try_emplace(ref);
    if (fresh) {
        it->second.primitive = false;
    }
    it->second.volumes = std::move(volumes);
}

bool SkydotPapyrus::contains(const Trigger& t, const Vector3& p) {
    for (const Volume& v : t.volumes) {
        if (contains(v, p)) {
            return true;
        }
    }
    if (!t.primitive) {
        return false;
    }
    const Vector3 local = t.to_local.xform(p - t.origin);
    if (t.type == k_sphere) {
        return local.length() <= t.half.x;
    }
    return std::abs(local.x) <= t.half.x && std::abs(local.y) <= t.half.y &&
           std::abs(local.z) <= t.half.z;
}

void SkydotPapyrus::update_actor(std::int64_t actor, const Vector3& position) {
    const auto form = static_cast<std::uint32_t>(actor);
    actors_[form] = position;
    for (auto& [ref, t] : triggers_) {
        const bool in = contains(t, position);
        const bool was = t.inside.contains(form);
        if (in == was) {
            continue;
        }
        // A disabled trigger notices no one (the E3 demo's set-stage boxes on
        // the way to Bleak Falls would start its quest and swap the barrow door).
        if (in && disabled(ref)) {
            continue;
        }
        if (in) {
            t.inside.insert(form);
        } else {
            t.inside.erase(form);
        }
        emit_signal("trigger", static_cast<std::int64_t>(ref), actor, in);
        dispatch(ref, in ? "OnTriggerEnter" : "OnTriggerLeave", {vm_->object(form, "ObjectReference")});
    }
}

std::int64_t SkydotPapyrus::get_trigger_count(std::int64_t ref) const {
    const auto it = triggers_.find(static_cast<std::uint32_t>(ref));
    return it != triggers_.end() ? static_cast<std::int64_t>(it->second.inside.size()) : 0;
}

Array SkydotPapyrus::get_triggers() const {
    Array out;
    for (const auto& [ref, t] : triggers_) {
        Dictionary entry;
        entry["ref"] = static_cast<std::int64_t>(ref);
        entry["type"] = static_cast<std::int64_t>(t.type);
        entry["origin"] = t.origin;
        entry["half"] = t.half;
        entry["inside"] = static_cast<std::int64_t>(t.inside.size());
        out.push_back(entry);
    }
    return out;
}

// ---- state the game layer reads and sets ----------------------------------

std::int64_t SkydotPapyrus::get_lock_level(std::int64_t ref) const {
    const auto form = lock_holder(static_cast<std::uint32_t>(ref));
    if (!locked(form)) {
        return -1;
    }
    const Variant lock = ref_info(form).get("lock", Variant());
    return lock.get_type() == Variant::DICTIONARY ? static_cast<std::int64_t>(Dictionary(lock)["level"]) : 0;
}

void SkydotPapyrus::set_locked(std::int64_t ref, bool value) {
    const auto form = lock_holder(static_cast<std::uint32_t>(ref));
    const bool was = locked(form);
    states_.locked[form] = value;
    if (was != value) {
        emit_signal("lock_changed", ref, value);
        if (form != static_cast<std::uint32_t>(ref)) {
            emit_signal("lock_changed", static_cast<std::int64_t>(form), value);
        }
    }
}

std::int64_t SkydotPapyrus::get_open_state(std::int64_t ref) const {
    const auto form = static_cast<std::uint32_t>(ref);
    if (const auto it = states_.open.find(form); it != states_.open.end()) {
        return it->second;
    }
    return String(ref_info(form).get("type", String())) == "DOOR" ? k_closed : 0;
}

void SkydotPapyrus::set_open_state(std::int64_t ref, std::int64_t state) {
    states_.open[static_cast<std::uint32_t>(ref)] = state;
}

bool SkydotPapyrus::is_activation_blocked(std::int64_t ref) const {
    return states_.blocked.contains(static_cast<std::uint32_t>(ref));
}

Dictionary SkydotPapyrus::get_disabled_changes() const {
    Dictionary out;
    for (const auto& [ref, value] : states_.disabled) {
        out[static_cast<std::int64_t>(ref)] = value;
    }
    return out;
}

bool SkydotPapyrus::is_ref_disabled(std::int64_t ref) const {
    return world_.is_valid() && disabled(static_cast<std::uint32_t>(ref));
}

// ---- saving ---------------------------------------------------------------

void SkydotPapyrus::set_animation_state(std::int64_t ref, const godot::PackedByteArray& bytes) {
    const auto* data = bytes.ptr();
    vm_->set_animation_state(static_cast<std::uint32_t>(ref),
                             data != nullptr ? std::vector<std::uint8_t>(data, data + bytes.size())
                                             : std::vector<std::uint8_t>{});
}

godot::PackedByteArray SkydotPapyrus::get_animation_state(std::int64_t ref) const {
    godot::PackedByteArray out;
    const auto& all = vm_->animation_states();
    if (const auto it = all.find(static_cast<std::uint32_t>(ref)); it != all.end()) {
        out.resize(static_cast<std::int64_t>(it->second.size()));
        std::copy(it->second.begin(), it->second.end(), out.ptrw());
    }
    return out;
}

godot::PackedByteArray SkydotPapyrus::save_state() const {
    Dictionary state;
    state["format"] = k_save_format;
    const auto vm_bytes = vm_->save();
    godot::PackedByteArray vm_packed;
    vm_packed.resize(static_cast<std::int64_t>(vm_bytes.size()));
    std::copy(vm_bytes.begin(), vm_bytes.end(), vm_packed.ptrw());
    state["vm"] = vm_packed;
    Array attached;
    for (const auto ref : attached_) {
        attached.push_back(static_cast<std::int64_t>(ref));
    }
    state["attached"] = attached;
    Dictionary disabled;
    for (const auto& [ref, value] : states_.disabled) {
        disabled[static_cast<std::int64_t>(ref)] = value;
    }
    state["disabled"] = disabled;
    Array blocked;
    for (const auto ref : states_.blocked) {
        blocked.push_back(static_cast<std::int64_t>(ref));
    }
    state["blocked"] = blocked;
    Dictionary open;
    for (const auto& [ref, value] : states_.open) {
        open[static_cast<std::int64_t>(ref)] = value;
    }
    state["open"] = open;
    Dictionary locks;
    for (const auto& [ref, value] : states_.locked) {
        locks[static_cast<std::int64_t>(ref)] = value;
    }
    state["locked"] = locks;
    Dictionary inside;
    for (const auto& [ref, t] : triggers_) {
        Array actors;
        for (const auto a : t.inside) {
            actors.push_back(static_cast<std::int64_t>(a));
        }
        inside[static_cast<std::int64_t>(ref)] = actors;
    }
    state["inside"] = inside;
    Dictionary actors;
    for (const auto& [ref, position] : actors_) {
        actors[static_cast<std::int64_t>(ref)] = position;
    }
    state["actors"] = actors;
    state["next_form"] = static_cast<std::int64_t>(next_form_);
    Dictionary quests;
    for (const auto& [quest, q] : quests_) {
        Dictionary entry;
        entry["running"] = q.running;
        entry["started"] = q.started;
        entry["completed"] = q.completed;
        entry["failed"] = q.failed;
        entry["active"] = q.active;
        entry["stage"] = q.stage;
        Array done;
        for (const auto stage : q.done) {
            done.push_back(stage);
        }
        entry["done"] = done;
        Dictionary objectives;
        for (const auto& [index, bits] : q.objectives) {
            objectives[index] = bits;
        }
        entry["objectives"] = objectives;
        quests[static_cast<std::int64_t>(quest)] = entry;
    }
    state["quests"] = quests;
    Array quest_scripts;
    for (const auto quest : quest_scripts_) {
        quest_scripts.push_back(static_cast<std::int64_t>(quest));
    }
    state["quest_scripts"] = quest_scripts;
    Array handles; // [quest, alias, handle, fill] each
    for (const auto& [key, handle] : alias_handles_) {
        const auto fill = alias_fill_.find(handle);
        handles.push_back(Array::make(static_cast<std::int64_t>(key.first), static_cast<std::int64_t>(key.second),
                                      static_cast<std::int64_t>(handle),
                                      static_cast<std::int64_t>(fill != alias_fill_.end() ? fill->second : 0)));
    }
    state["aliases"] = handles;
    Dictionary globals;
    for (const auto& [global, value] : globals_) {
        globals[static_cast<std::int64_t>(global)] = value;
    }
    state["globals"] = globals;
    state["next_alias_handle"] = static_cast<std::int64_t>(next_alias_handle_);
    return godot::UtilityFunctions::var_to_bytes(state);
}

godot::Error SkydotPapyrus::load_state(const godot::PackedByteArray& bytes) {
    const auto fail = [this](const String& why) {
        last_error_ = why;
        godot::UtilityFunctions::push_error(String("SkydotPapyrus: ") + why);
        return godot::ERR_FILE_CORRUPT;
    };
    if (world_.is_null()) {
        return fail("setup has not run");
    }
    // Objects are not allowed in: a save is data.
    const Variant decoded = godot::UtilityFunctions::bytes_to_var(bytes);
    if (decoded.get_type() != Variant::DICTIONARY) {
        return fail("not a script save");
    }
    const Dictionary state = decoded;
    const auto format = static_cast<std::int64_t>(state.get("format", -1));
    if (format != k_save_format && format != 1) {
        return fail("script save format " + String(Variant(state.get("format", -1))) +
                    " is not one this engine reads");
    }
    const auto typed = [&](const char* key, Variant::Type type) {
        return state.get(key, Variant()).get_type() == type;
    };
    for (const auto& [key, type] :
         {std::pair{"vm", Variant::PACKED_BYTE_ARRAY}, {"attached", Variant::ARRAY},
          {"disabled", Variant::DICTIONARY}, {"blocked", Variant::ARRAY}, {"open", Variant::DICTIONARY},
          {"locked", Variant::DICTIONARY}, {"inside", Variant::DICTIONARY}, {"actors", Variant::DICTIONARY},
          {"next_form", Variant::INT}}) {
        if (!typed(key, type)) {
            return fail(String("script save lacks ") + key);
        }
    }
    if (format >= 2) {
        for (const auto& [key, type] :
             {std::pair{"quests", Variant::DICTIONARY}, {"quest_scripts", Variant::ARRAY},
              {"aliases", Variant::ARRAY}, {"globals", Variant::DICTIONARY},
              {"next_alias_handle", Variant::INT}}) {
            if (!typed(key, type)) {
                return fail(String("script save lacks ") + key);
            }
        }
    }
    const godot::PackedByteArray packed = state["vm"];
    const std::vector<std::uint8_t> vm_bytes(packed.ptr(), packed.ptr() + packed.size());
    std::string why;
    std::vector<std::uint32_t> gone;
    for (const auto& [id, placed] : vm_->placed().refs()) {
        gone.push_back(id);
    }
    reset();
    for (const auto id : gone) {
        emit_signal("ref_deleted", static_cast<std::int64_t>(id));
    }
    if (!vm_->load(vm_bytes, why)) {
        return fail(to_godot(why));
    }
    const auto ref_of = [](const Variant& v) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(v)); };
    const Array attached = state["attached"];
    for (std::int64_t i = 0; i < attached.size(); ++i) {
        const auto ref = ref_of(attached[i]);
        attached_.insert(ref);
        if (!vm_->instances(ref).empty()) {
            register_trigger(ref, ref_info(ref));
        }
    }
    const auto each = [](const Dictionary& d, auto&& fn) {
        const Array keys = d.keys();
        for (std::int64_t i = 0; i < keys.size(); ++i) {
            fn(keys[i], d[keys[i]]);
        }
    };
    each(state["disabled"], [&](const Variant& k, const Variant& v) { states_.disabled[ref_of(k)] = v; });
    const Array blocked = state["blocked"];
    for (std::int64_t i = 0; i < blocked.size(); ++i) {
        states_.blocked.insert(ref_of(blocked[i]));
    }
    each(state["open"], [&](const Variant& k, const Variant& v) { states_.open[ref_of(k)] = v; });
    each(state["locked"], [&](const Variant& k, const Variant& v) { states_.locked[ref_of(k)] = v; });
    each(state["inside"], [&](const Variant& k, const Variant& v) {
        if (const auto it = triggers_.find(ref_of(k)); it != triggers_.end() && v.get_type() == Variant::ARRAY) {
            const Array actors = v;
            for (std::int64_t i = 0; i < actors.size(); ++i) {
                it->second.inside.insert(ref_of(actors[i]));
            }
        }
    });
    each(state["actors"], [&](const Variant& k, const Variant& v) {
        if (v.get_type() == Variant::VECTOR3) {
            actors_[ref_of(k)] = v;
        }
    });
    next_form_ = ref_of(state["next_form"]);
    if (format >= 2) {
        each(state["quests"], [&](const Variant& k, const Variant& v) {
            if (v.get_type() != Variant::DICTIONARY) {
                return;
            }
            const Dictionary d = v;
            QuestState q;
            q.running = d.get("running", false);
            q.started = d.get("started", false);
            q.completed = d.get("completed", false);
            q.failed = d.get("failed", false);
            q.active = d.get("active", false);
            q.stage = static_cast<std::int32_t>(static_cast<std::int64_t>(d.get("stage", 0)));
            const Variant done = d.get("done", Array());
            if (done.get_type() == Variant::ARRAY) {
                const Array list = done;
                for (std::int64_t i = 0; i < list.size(); ++i) {
                    q.done.insert(static_cast<std::int32_t>(static_cast<std::int64_t>(list[i])));
                }
            }
            const Variant objectives = d.get("objectives", Dictionary());
            if (objectives.get_type() == Variant::DICTIONARY) {
                each(objectives, [&](const Variant& index, const Variant& bits) {
                    q.objectives[static_cast<std::int32_t>(static_cast<std::int64_t>(index))] =
                        static_cast<std::uint8_t>(static_cast<std::int64_t>(bits));
                });
            }
            quests_[ref_of(k)] = std::move(q);
        });
        const Array quest_scripts = state["quest_scripts"];
        for (std::int64_t i = 0; i < quest_scripts.size(); ++i) {
            quest_scripts_.insert(ref_of(quest_scripts[i]));
        }
        const Array handles = state["aliases"];
        for (std::int64_t i = 0; i < handles.size(); ++i) {
            if (handles[i].get_type() != Variant::ARRAY || Array(handles[i]).size() != 4) {
                return fail("script save has a malformed alias");
            }
            const Array a = handles[i];
            const auto key = std::pair{ref_of(a[0]), ref_of(a[1])};
            alias_handles_[key] = ref_of(a[2]);
            handle_alias_[ref_of(a[2])] = key;
            alias_fill_[ref_of(a[2])] = ref_of(a[3]);
        }
        each(state["globals"], [&](const Variant& k, const Variant& v) { globals_[ref_of(k)] = v; });
        next_alias_handle_ = ref_of(state["next_alias_handle"]);
    }
    // The kept placed references are built again.
    for (const auto& [id, placed] : vm_->placed().refs()) {
        emit_signal("ref_placed", static_cast<std::int64_t>(id), static_cast<std::int64_t>(placed.base),
                    static_cast<std::int64_t>(placed.cell), vec3(placed.position), vec3(placed.rotation),
                    placed.scale, disabled(id));
    }
    last_error_ = String();
    return godot::OK;
}

String SkydotPapyrus::get_last_error() const { return last_error_; }

// ---- tests and inspection -------------------------------------------------

void SkydotPapyrus::update(double seconds) { vm_->update(seconds); }

std::int64_t SkydotPapyrus::call_global(const String& cls, const String& fn, const Array& args) {
    std::vector<vm::Value> values;
    for (std::int64_t i = 0; i < args.size(); ++i) {
        values.push_back(to_value(args[i], ""));
    }
    return static_cast<std::int64_t>(vm_->call_global(utf8(cls), utf8(fn), std::move(values)));
}

std::int64_t SkydotPapyrus::create_instance(const String& cls) {
    const auto form = next_form_++;
    return vm_->attach(form, utf8(cls)) != nullptr ? form : 0;
}

std::int64_t SkydotPapyrus::call_method(std::int64_t form, const String& cls, const String& fn,
                                        const Array& args) {
    std::vector<vm::Value> values;
    for (std::int64_t i = 0; i < args.size(); ++i) {
        values.push_back(to_value(args[i], ""));
    }
    return static_cast<std::int64_t>(vm_->call_method(
        vm_->instance(static_cast<std::uint32_t>(form), utf8(cls)), utf8(fn), std::move(values)));
}

Dictionary SkydotPapyrus::take_result(std::int64_t thread) {
    Dictionary out;
    const auto result = vm_->take_result(static_cast<std::uint64_t>(thread));
    out["done"] = result.has_value();
    if (result) {
        out["value"] = to_variant(*result);
    }
    return out;
}

Variant SkydotPapyrus::get_variable(std::int64_t form, const String& cls, const String& name) const {
    auto* instance = vm_->instance(static_cast<std::uint32_t>(form), utf8(cls));
    return instance != nullptr ? to_variant(vm_->get_variable(instance, utf8(name))) : Variant();
}

Array SkydotPapyrus::get_scripts(std::int64_t ref) const {
    Array out;
    for (const auto* i : vm_->instances(static_cast<std::uint32_t>(ref))) {
        Dictionary entry;
        entry["name"] = to_godot(i->cls->name());
        entry["state"] = to_godot(i->state());
        out.push_back(entry);
    }
    return out;
}

std::int64_t SkydotPapyrus::get_error_count() const { return static_cast<std::int64_t>(vm_->errors()); }
std::int64_t SkydotPapyrus::get_instruction_count() const {
    return static_cast<std::int64_t>(vm_->instructions());
}
std::int64_t SkydotPapyrus::get_thread_count() const {
    return static_cast<std::int64_t>(vm_->thread_count());
}
double SkydotPapyrus::get_time() const { return vm_->time(); }

} // namespace skydot
