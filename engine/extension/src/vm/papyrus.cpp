// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/papyrus.hpp"

#include "data/actors.hpp"
#include "data/coordinates.hpp"
#include "skydot_formats/flags.hpp"
#include "world_generated.h"
#include "actors/behavior_player.hpp"
#include "build/ref_nodes.hpp"
#include "physics/collision.hpp"
#include "vm/activation.hpp"
#include "vm/natives_actor.hpp"
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

using natives::k_closed;

} // namespace

void SkydotPapyrus::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("setup", "pack", "world"), &SkydotPapyrus::setup);
    godot::ClassDB::bind_method(D_METHOD("attach_clock", "clock"), &SkydotPapyrus::attach_clock);
    godot::ClassDB::bind_method(D_METHOD("attach", "cell", "ref"), &SkydotPapyrus::attach);
    godot::ClassDB::bind_method(D_METHOD("attach_built", "root"), &SkydotPapyrus::attach_built);
    godot::ClassDB::bind_method(D_METHOD("attach_cell", "cell"), &SkydotPapyrus::attach_cell);
    godot::ClassDB::bind_method(D_METHOD("detach_cell", "cell"), &SkydotPapyrus::detach_cell);
    godot::ClassDB::bind_method(D_METHOD("set_allowed_to_fly", "ref", "allowed"), &SkydotPapyrus::set_allowed_to_fly);
    godot::ClassDB::bind_method(D_METHOD("get_allowed_to_fly", "ref"), &SkydotPapyrus::allowed_to_fly);
    godot::ClassDB::bind_method(D_METHOD("detach_all"), &SkydotPapyrus::detach_all);
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
    godot::ClassDB::bind_method(D_METHOD("resolve", "owner"), &SkydotPapyrus::resolve);
    godot::ClassDB::bind_method(D_METHOD("item_info", "form"), &SkydotPapyrus::item_info);
    godot::ClassDB::bind_method(D_METHOD("inventory", "owner"), &SkydotPapyrus::inventory);
    godot::ClassDB::bind_method(D_METHOD("transfer", "from", "to", "form", "count", "extra"),
                                &SkydotPapyrus::transfer, DEFVAL(-1));
    godot::ClassDB::bind_method(D_METHOD("equip", "owner", "form", "extra"), &SkydotPapyrus::equip, DEFVAL(-1));
    godot::ClassDB::bind_method(D_METHOD("unequip", "owner", "form", "extra"), &SkydotPapyrus::unequip,
                                DEFVAL(-1));
    godot::ClassDB::bind_method(D_METHOD("equipment", "owner"), &SkydotPapyrus::equipment);
    godot::ClassDB::bind_method(D_METHOD("ranged_weapon", "owner"), &SkydotPapyrus::ranged_weapon);
    godot::ClassDB::bind_method(D_METHOD("melee_weapon", "owner"), &SkydotPapyrus::melee_weapon);
    godot::ClassDB::bind_method(D_METHOD("combat_state_changed", "ref", "target", "state"),
                                &SkydotPapyrus::combat_state_changed);
    godot::ClassDB::bind_method(D_METHOD("equipped_item_type", "owner", "hand"),
                                &SkydotPapyrus::equipped_item_type);
    godot::ClassDB::bind_method(
        D_METHOD("drop", "owner", "form", "count", "cell", "position", "rotation", "extra"),
        &SkydotPapyrus::drop, DEFVAL(-1));
    godot::ClassDB::bind_method(D_METHOD("recipes", "bench_ref"), &SkydotPapyrus::recipes);
    godot::ClassDB::bind_method(D_METHOD("cast_spell", "caster", "spell", "target", "seconds"),
                                &SkydotPapyrus::cast_spell, DEFVAL(0), DEFVAL(0.0));
    godot::ClassDB::bind_method(D_METHOD("pay_spell", "caster", "spell", "seconds"), &SkydotPapyrus::pay_spell,
                                DEFVAL(0.0));
    godot::ClassDB::bind_method(D_METHOD("land_spell", "caster", "form", "target"), &SkydotPapyrus::land_spell);
    godot::ClassDB::bind_method(D_METHOD("cast_info", "form"), &SkydotPapyrus::cast_info);
    godot::ClassDB::bind_method(D_METHOD("cast_scroll", "caster", "scroll", "target"), &SkydotPapyrus::cast_scroll);
    godot::ClassDB::bind_method(D_METHOD("apply_poison", "actor", "poison"), &SkydotPapyrus::apply_poison);
    godot::ClassDB::bind_method(D_METHOD("weapon_charge", "actor", "form", "extra"), &SkydotPapyrus::weapon_charge,
                                DEFVAL(-1));
    godot::ClassDB::bind_method(D_METHOD("hand_press", "actor", "hand"), &SkydotPapyrus::hand_press);
    godot::ClassDB::bind_method(D_METHOD("hand_step", "actor", "hand", "dt"), &SkydotPapyrus::hand_step);
    godot::ClassDB::bind_method(D_METHOD("hand_release", "actor", "hand"), &SkydotPapyrus::hand_release);
    godot::ClassDB::bind_method(D_METHOD("spell_cost", "caster", "spell"), &SkydotPapyrus::spell_cost);
    godot::ClassDB::bind_method(D_METHOD("spend_magicka", "caster", "amount"), &SkydotPapyrus::spend_magicka);
    godot::ClassDB::bind_method(D_METHOD("apply_item_effects", "actor", "form", "extra"),
                                &SkydotPapyrus::apply_item_effects, DEFVAL(0));
    godot::ClassDB::bind_method(D_METHOD("spells_of", "actor"), &SkydotPapyrus::spells_of);
    godot::ClassDB::bind_method(D_METHOD("add_spell", "actor", "spell"), &SkydotPapyrus::add_spell);
    godot::ClassDB::bind_method(D_METHOD("remove_spell", "actor", "spell"), &SkydotPapyrus::remove_spell);
    godot::ClassDB::bind_method(D_METHOD("has_spell", "actor", "spell"), &SkydotPapyrus::has_spell);
    godot::ClassDB::bind_method(D_METHOD("equip_spell", "actor", "spell", "hand"), &SkydotPapyrus::equip_spell);
    godot::ClassDB::bind_method(D_METHOD("equipped_spell", "actor", "hand"), &SkydotPapyrus::equipped_spell);
    godot::ClassDB::bind_method(D_METHOD("power_cooldown", "actor", "spell"), &SkydotPapyrus::power_cooldown);
    godot::ClassDB::bind_method(D_METHOD("shout_info", "actor", "shout"), &SkydotPapyrus::shout_info);
    godot::ClassDB::bind_method(D_METHOD("shouts_of", "actor"), &SkydotPapyrus::shouts_of);
    godot::ClassDB::bind_method(D_METHOD("equipped_shout", "actor"), &SkydotPapyrus::equipped_shout);
    godot::ClassDB::bind_method(D_METHOD("equip_shout", "actor", "shout"), &SkydotPapyrus::equip_shout);
    godot::ClassDB::bind_method(D_METHOD("teach_word", "actor", "word"), &SkydotPapyrus::teach_word);
    godot::ClassDB::bind_method(D_METHOD("unlock_word", "actor", "word"), &SkydotPapyrus::unlock_word);
    godot::ClassDB::bind_method(D_METHOD("unlock_word_with_soul", "actor", "word"),
                                &SkydotPapyrus::unlock_word_with_soul);
    godot::ClassDB::bind_method(D_METHOD("voice_recovery", "actor"), &SkydotPapyrus::voice_recovery);
    godot::ClassDB::bind_method(D_METHOD("set_voice_recovery", "actor", "seconds"), &SkydotPapyrus::set_voice_recovery);
    godot::ClassDB::bind_method(D_METHOD("cast_shout", "actor", "shout", "held_seconds", "target"),
                                &SkydotPapyrus::cast_shout, DEFVAL(0));
    godot::ClassDB::bind_method(D_METHOD("active_effects", "actor"), &SkydotPapyrus::active_effects);
    godot::ClassDB::bind_method(D_METHOD("magic_overrides", "ref"), &SkydotPapyrus::magic_overrides);
    godot::ClassDB::bind_method(D_METHOD("break_invisibility", "ref"), &SkydotPapyrus::break_invisibility);
    godot::ClassDB::bind_method(D_METHOD("dispel_bound_weapon", "actor", "weapon"),
                                &SkydotPapyrus::dispel_bound_weapon);
    godot::ClassDB::bind_method(D_METHOD("dispel_spell", "actor", "spell"), &SkydotPapyrus::dispel_spell);
    godot::ClassDB::bind_method(D_METHOD("dispel_all_spells", "actor"), &SkydotPapyrus::dispel_all_spells);
    godot::ClassDB::bind_method(D_METHOD("prepare_actor_magic", "actor"), &SkydotPapyrus::prepare_actor_magic);
    godot::ClassDB::bind_method(D_METHOD("get_unhandled_archetypes"), &SkydotPapyrus::get_unhandled_archetypes);
    godot::ClassDB::bind_method(D_METHOD("craft", "recipe", "count"), &SkydotPapyrus::craft);
    godot::ClassDB::bind_method(D_METHOD("temper_rows", "bench_ref"), &SkydotPapyrus::temper_rows);
    godot::ClassDB::bind_method(D_METHOD("temper", "recipe", "form", "extra"), &SkydotPapyrus::temper);
    godot::ClassDB::bind_method(D_METHOD("enchant_rows"), &SkydotPapyrus::enchant_rows);
    godot::ClassDB::bind_method(D_METHOD("learn_enchantment", "enchantment"), &SkydotPapyrus::learn_enchantment);
    godot::ClassDB::bind_method(D_METHOD("enchantment_fits", "item", "enchantment"), &SkydotPapyrus::enchantment_fits);
    godot::ClassDB::bind_method(D_METHOD("enchant_preview", "item", "item_extra", "enchantment", "gem", "gem_extra", "name"),
                                &SkydotPapyrus::enchant_preview);
    godot::ClassDB::bind_method(D_METHOD("enchant", "item", "item_extra", "enchantment", "gem", "gem_extra", "name"),
                                &SkydotPapyrus::enchant);
    godot::ClassDB::bind_method(D_METHOD("disenchant", "form", "extra"), &SkydotPapyrus::disenchant);
    godot::ClassDB::bind_method(D_METHOD("recharge_options", "item", "extra"), &SkydotPapyrus::recharge_options);
    godot::ClassDB::bind_method(D_METHOD("recharge", "item", "extra", "gem", "gem_extra"), &SkydotPapyrus::recharge);
    godot::ClassDB::bind_method(D_METHOD("fill_gem", "actor"), &SkydotPapyrus::fill_gem);
    godot::ClassDB::bind_method(D_METHOD("alchemy_rows"), &SkydotPapyrus::alchemy_rows);
    godot::ClassDB::bind_method(D_METHOD("alchemy_preview", "forms"), &SkydotPapyrus::alchemy_preview);
    godot::ClassDB::bind_method(D_METHOD("alchemy_mix", "forms"), &SkydotPapyrus::alchemy_mix);
    godot::ClassDB::bind_method(D_METHOD("harvested", "ref"), &SkydotPapyrus::harvested);
    godot::ClassDB::bind_method(D_METHOD("get_harvested_refs"), &SkydotPapyrus::get_harvested_refs);
    godot::ClassDB::bind_method(D_METHOD("send_event", "ref", "event", "args"),
                                &SkydotPapyrus::send_event);
    godot::ClassDB::bind_method(D_METHOD("complete_activation", "ref"), &SkydotPapyrus::complete_activation);
    godot::ClassDB::bind_method(D_METHOD("notify_animation_event", "ref", "event"),
                                &SkydotPapyrus::notify_animation_event);
    godot::ClassDB::bind_method(D_METHOD("get_animation_event_count"), &SkydotPapyrus::get_animation_event_count);
    godot::ClassDB::bind_method(D_METHOD("notify_sound_finished", "instance"),
                                &SkydotPapyrus::notify_sound_finished);
    godot::ClassDB::bind_method(D_METHOD("update_actor", "actor", "position"),
                                &SkydotPapyrus::update_actor);
    godot::ClassDB::bind_method(D_METHOD("update", "seconds"), &SkydotPapyrus::update);
    godot::ClassDB::bind_method(D_METHOD("call_global", "cls", "fn", "args"),
                                &SkydotPapyrus::call_global);
    godot::ClassDB::bind_method(D_METHOD("create_instance", "cls"), &SkydotPapyrus::create_instance);
    godot::ClassDB::bind_method(D_METHOD("new_sound_instance"), &SkydotPapyrus::new_sound_instance);
    godot::ClassDB::bind_method(D_METHOD("call_method", "form", "cls", "fn", "args"),
                                &SkydotPapyrus::call_method);
    godot::ClassDB::bind_method(D_METHOD("take_result", "thread"), &SkydotPapyrus::take_result);
    godot::ClassDB::bind_method(D_METHOD("get_variable", "form", "cls", "name"),
                                &SkydotPapyrus::get_variable);
    godot::ClassDB::bind_method(D_METHOD("get_scripts", "ref"), &SkydotPapyrus::get_scripts);
    godot::ClassDB::bind_method(D_METHOD("is_ref_disabled", "ref"), &SkydotPapyrus::is_ref_disabled);
    godot::ClassDB::bind_method(D_METHOD("get_moved_changes"), &SkydotPapyrus::get_moved_changes);
    godot::ClassDB::bind_method(D_METHOD("get_disabled_changes"),
                                &SkydotPapyrus::get_disabled_changes);
    godot::ClassDB::bind_method(D_METHOD("is_activation_blocked", "ref"),
                                &SkydotPapyrus::is_activation_blocked);
    godot::ClassDB::bind_method(D_METHOD("get_lock_level", "ref"), &SkydotPapyrus::get_lock_level);
    godot::ClassDB::bind_method(D_METHOD("set_locked", "ref", "locked"), &SkydotPapyrus::set_locked);
    godot::ClassDB::bind_method(D_METHOD("get_open_state", "ref"), &SkydotPapyrus::get_open_state);
    godot::ClassDB::bind_method(D_METHOD("is_actor_dead", "ref"), &SkydotPapyrus::is_actor_dead);
    godot::ClassDB::bind_method(D_METHOD("is_actor_ghost", "ref"), &SkydotPapyrus::is_actor_ghost);
    godot::ClassDB::bind_method(D_METHOD("faction_rank", "ref", "faction"), &SkydotPapyrus::faction_rank);
    godot::ClassDB::bind_method(D_METHOD("get_item_count", "owner", "item"), &SkydotPapyrus::get_item_count);
    godot::ClassDB::bind_method(D_METHOD("get_actor_value", "ref", "name"), &SkydotPapyrus::get_actor_value);
    godot::ClassDB::bind_method(D_METHOD("get_actor_value_max", "ref", "name"),
                                &SkydotPapyrus::get_actor_value_max);
    godot::ClassDB::bind_method(D_METHOD("damage_actor_value", "ref", "name", "amount"),
                                &SkydotPapyrus::damage_actor_value);
    godot::ClassDB::bind_method(D_METHOD("set_actor_value", "ref", "name", "value"),
                                &SkydotPapyrus::set_actor_value);
    godot::ClassDB::bind_method(
        D_METHOD("apply_hit", "attacker", "target", "weapon", "extra", "attack_event", "flags", "ammo"),
        &SkydotPapyrus::apply_hit, DEFVAL(0));
    godot::ClassDB::bind_method(D_METHOD("is_player_dead"), &SkydotPapyrus::is_player_dead);
    godot::ClassDB::bind_method(D_METHOD("get_corpse_place", "ref"), &SkydotPapyrus::get_corpse_place);
    godot::ClassDB::bind_method(D_METHOD("set_corpse_place", "ref", "space", "position", "rot_z"),
                                &SkydotPapyrus::set_corpse_place);
    godot::ClassDB::bind_method(D_METHOD("get_killer", "ref"), &SkydotPapyrus::get_killer);
    BIND_CONSTANT(HIT_POWER);
    BIND_CONSTANT(HIT_SNEAK);
    BIND_CONSTANT(HIT_BASH);
    BIND_CONSTANT(HIT_BLOCKED);
    godot::ClassDB::bind_method(D_METHOD("restore_actor_value", "ref", "name", "amount"),
                                &SkydotPapyrus::restore_actor_value);
    godot::ClassDB::bind_method(D_METHOD("set_open_state", "ref", "state"),
                                &SkydotPapyrus::set_open_state);
    godot::ClassDB::bind_method(D_METHOD("get_trigger_count", "ref"),
                                &SkydotPapyrus::get_trigger_count);
    godot::ClassDB::bind_method(D_METHOD("get_triggers"), &SkydotPapyrus::get_triggers);
    godot::ClassDB::bind_method(D_METHOD("start_game_enabled_quests"),
                                &SkydotPapyrus::start_game_enabled_quests);
    godot::ClassDB::bind_method(D_METHOD("dialogue_can_talk", "ref"), &SkydotPapyrus::dialogue_can_talk);
    godot::ClassDB::bind_method(D_METHOD("dialogue_open", "speaker"), &SkydotPapyrus::dialogue_open);
    godot::ClassDB::bind_method(D_METHOD("topic_line", "speaker", "topic"), &SkydotPapyrus::topic_line);
    godot::ClassDB::bind_method(D_METHOD("dialogue_choose", "topic"), &SkydotPapyrus::dialogue_choose);
    godot::ClassDB::bind_method(D_METHOD("dialogue_close"), &SkydotPapyrus::dialogue_close);
    godot::ClassDB::bind_method(D_METHOD("dialogue_is_open"), &SkydotPapyrus::dialogue_is_open);
    godot::ClassDB::bind_method(D_METHOD("scenes_update", "seconds"), &SkydotPapyrus::scenes_update);
    godot::ClassDB::bind_method(D_METHOD("scene_action_done", "scene", "action"), &SkydotPapyrus::scene_action_done);
    godot::ClassDB::bind_method(D_METHOD("scene_start", "scene", "force"), &SkydotPapyrus::scene_start);
    godot::ClassDB::bind_method(D_METHOD("scene_stop", "scene"), &SkydotPapyrus::scene_stop);
    godot::ClassDB::bind_method(D_METHOD("scene_is_playing", "scene"), &SkydotPapyrus::scene_is_playing);
    godot::ClassDB::bind_method(D_METHOD("set_scene_actors_need_3d", "need"), &SkydotPapyrus::set_scene_actors_need_3d);
    godot::ClassDB::bind_method(D_METHOD("voice_census", "vpaths"), &SkydotPapyrus::voice_census);
    godot::ClassDB::bind_method(D_METHOD("dialogue_topics_debug", "speaker"),
                                &SkydotPapyrus::dialogue_topics_debug);
    godot::ClassDB::bind_method(D_METHOD("dialogue_fragment", "info", "end", "speaker"),
                                &SkydotPapyrus::dialogue_fragment);
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
    godot::ClassDB::bind_method(D_METHOD("spawn_actor", "base", "cell", "position", "rotation_z"),
                                &SkydotPapyrus::spawn_actor);
    godot::ClassDB::bind_method(D_METHOD("forget_ref", "ref"), &SkydotPapyrus::forget_ref);
    godot::ClassDB::bind_method(D_METHOD("damage_ref", "ref", "amount"), &SkydotPapyrus::damage_ref);
    godot::ClassDB::bind_method(D_METHOD("ref_built", "ref"), &SkydotPapyrus::ref_built);
    godot::ClassDB::bind_method(D_METHOD("get_ref_base", "ref"), &SkydotPapyrus::get_ref_base);
    godot::ClassDB::bind_method(D_METHOD("write_sections", "writer"), &SkydotPapyrus::write_sections);
    godot::ClassDB::bind_method(D_METHOD("prepare_load", "reader"), &SkydotPapyrus::prepare_load);
    godot::ClassDB::bind_method(D_METHOD("commit_load"), &SkydotPapyrus::commit_load);
    godot::ClassDB::bind_method(D_METHOD("cancel_load"), &SkydotPapyrus::cancel_load);
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
    // A script registered for animation events of `ref`: the host connects what the model raises to
    // notify_animation_event (once per node).
    ADD_SIGNAL(godot::MethodInfo("animation_events_wanted", PropertyInfo(Variant::INT, "ref")));
    ADD_SIGNAL(godot::MethodInfo("play_animation", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::STRING, "animation")));
    ADD_SIGNAL(godot::MethodInfo("havok_impulse", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::VECTOR3, "direction"),
                                 PropertyInfo(Variant::FLOAT, "magnitude")));
    // ObjectReference.PushActorAway and KnockAreaEffect: the source, the actor pushed and the force; the
    // source, the magnitude and the radius in game units.
    ADD_SIGNAL(godot::MethodInfo("push_actor", PropertyInfo(Variant::INT, "source"),
                                 PropertyInfo(Variant::INT, "target"), PropertyInfo(Variant::FLOAT, "force")));
    ADD_SIGNAL(godot::MethodInfo("knock_area", PropertyInfo(Variant::INT, "source"),
                                 PropertyInfo(Variant::FLOAT, "magnitude"), PropertyInfo(Variant::FLOAT, "radius")));
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
    ADD_SIGNAL(godot::MethodInfo("scene_package", PropertyInfo(Variant::INT, "actor"),
                                 PropertyInfo(Variant::INT, "package"), PropertyInfo(Variant::BOOL, "push")));
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
    // A non-essential actor other than the player was brought to Health 0.
    ADD_SIGNAL(godot::MethodInfo("actor_died", PropertyInfo(Variant::INT, "ref")));
    ADD_SIGNAL(godot::MethodInfo("soul_trapped", PropertyInfo(Variant::INT, "actor"), PropertyInfo(Variant::BOOL, "filled"),
                                 PropertyInfo(Variant::INT, "gem"), PropertyInfo(Variant::INT, "soul")));
    // A magic effect started acting on `target` or finished (`instance` is its id, the handle of
    // its script object); `archetype` is the MGEF's. For presentation: light, art, a menu.
    ADD_SIGNAL(godot::MethodInfo("magic_effect_changed", PropertyInfo(Variant::INT, "target"),
                                 PropertyInfo(Variant::INT, "mgef"), PropertyInfo(Variant::INT, "archetype"),
                                 PropertyInfo(Variant::BOOL, "started"), PropertyInfo(Variant::INT, "instance")));
    // The same with everything the game layer needs: {started, instance, mgef, archetype, caster, target,
    // magnitude, duration, area, associated (the MGEF's item), summoned (a summon's ref)}.
    ADD_SIGNAL(godot::MethodInfo("magic_effect_event", PropertyInfo(Variant::DICTIONARY, "info")));
    // The player was brought to Health 0 (nobody dies in the VM for him): the killer, 0 if none.
    ADD_SIGNAL(godot::MethodInfo("player_died", PropertyInfo(Variant::INT, "killer")));
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
    // VisualEffect.Play / Stop: `effect` plays on `ref`, toward `target` (0 none) for `seconds` (0: until stopped).
    ADD_SIGNAL(godot::MethodInfo("visual_effect", PropertyInfo(Variant::INT, "effect"),
                                 PropertyInfo(Variant::INT, "ref"), PropertyInfo(Variant::INT, "target"),
                                 PropertyInfo(Variant::FLOAT, "seconds"), PropertyInfo(Variant::BOOL, "playing")));
    // The armor an actor wears changed (EquipItem, UnequipItem, UnequipAll): its parts are built again.
    ADD_SIGNAL(godot::MethodInfo("worn_changed", PropertyInfo(Variant::INT, "ref")));
    ADD_SIGNAL(godot::MethodInfo("sub_graph_animation", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::STRING, "name")));
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

SkydotPapyrus::SkydotPapyrus() : vm_(std::make_unique<vm::Vm>()) {
    install_scheduled_handler();
    bind_activations();
}

SkydotPapyrus::~SkydotPapyrus() {
    // The world outlives this object and must not call back into it.
    if (world_.is_valid()) {
        world_->set_worn_provider({});
    }
}

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
    loaded_.clear();
    states_.clear();
    actors_.clear();
    clear_magic();
    items_.clear();
    created_.clear();
    harvests_.clear();
    dialogue_states_.clear();
    dialogue_.reset();
    scenes_.reset();
    scene_scripts_.clear();
    info_quests_.clear();
    activations_->clear();
    triggers_.clear();
    test_ids_.clear();
    sound_ids_.clear();
    quests_.clear();
    quest_info_.clear();
}

godot::Error SkydotPapyrus::setup(const godot::Ref<SkydotPack>& pack,
                                  const godot::Ref<SkydotWorld>& world) {
    if (pack.is_null() || world.is_null()) {
        return godot::ERR_INVALID_PARAMETER;
    }
    pack_ = pack;
    world_ = world;
    world_->set_worn_provider(worn_provider());
    vm_ = std::make_unique<vm::Vm>();
    crafting_.reset(); // it refers to the old VM's inventory
    magic_.reset();    // the natives bound to it die with the old VM
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

std::uint32_t SkydotPapyrus::base_form_of(std::uint32_t ref) const {
    if (const auto* placed = vm_->placed().find(ref)) {
        return placed->base;
    }
    WorldData::RefEnable info;
    return world_.is_valid() && world_->data().ref_enable(ref, info) ? info.base : 0;
}

bool SkydotPapyrus::disabled(std::uint32_t ref, int depth) const {
    if (const auto it = states_.disabled.find(ref); it != states_.disabled.end()) {
        return it->second;
    }
    // A reference a script placed starts enabled; one of the world's follows
    // its enable parent, as scripts changed it, else the world's initial state.
    WorldData::RefEnable info;
    if (vm_->placed().find(ref) != nullptr || world_.is_null() || !world_->data().ref_enable(ref, info)) {
        return false;
    }
    if (info.enable_parent != 0 && depth < 16 && world_->data().cell_of_ref(info.enable_parent) != 0) {
        return disabled(info.enable_parent, depth + 1) != info.opposite;
    }
    return info.initially_disabled;
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
        form_of(Dictionary(door).get("destination", 0));
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
        return vec3_of(it->second.rotation);
    }
    return ref_info(ref).get("rotation", Vector3());
}

Vector3 SkydotPapyrus::position_of(std::uint32_t ref) const {
    if (const auto it = states_.moved.find(ref); it != states_.moved.end()) {
        return vec3_of(it->second.position);
    }
    if (const vm::Vec3* actor = triggers_.actor_position(ref)) {
        return Vector3(actor->x, actor->y, actor->z);
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

    /// The typed world data, null without a world.
    const WorldData* world() const { return owner_.world_.is_valid() ? &owner_.world_->data() : nullptr; }

    std::uint32_t base_form(std::uint32_t ref) const override {
        return owner_.base_form_of(ref);
    }
    std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const override {
        const Array links = owner_.ref_info(ref).get("links", Array());
        for (std::int64_t i = 0; i < links.size(); ++i) {
            const Dictionary link = links[i];
            if (static_cast<std::int64_t>(link["keyword"]) == keyword) {
                return form_of(link["target"]);
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
    std::int32_t player_level() const override { return 1; }
    double game_days() const override {
        return owner_.clock_.is_valid() ? owner_.clock_->get_game_days() : 0.0;
    }
    double time_scale() const override {
        return owner_.clock_.is_valid() ? owner_.clock_->get_time_scale() : 0.0;
    }

private:
    const SkydotPapyrus& owner_;
};

void SkydotPapyrus::bind_world_natives() {
    access_ = std::make_unique<Access>(*this);
    vm::bind_world_natives(*vm_, *access_);
    actors_.set_source([this](std::uint32_t ref, vm::ActorBase& out) { return access_->actor_base(ref, out); });
    if (world_.is_valid()) {
        const WorldData& data = world_->data();
        actors_.set_regen_delays(
            static_cast<double>(data.gmst_float("fDamagedMagickaRegenDelay",
                                                static_cast<float>(vm::ActorStates::k_damaged_magicka_regen_delay))),
            static_cast<double>(data.gmst_float("fDamagedStaminaRegenDelay",
                                                static_cast<float>(vm::ActorStates::k_damaged_stamina_regen_delay))));
    }
}

void SkydotPapyrus::bind_actor_natives() {
    vm::bind_actor_natives(*vm_, actors_, [this](std::uint32_t ref, std::uint32_t av, double amount) {
        if (ref == k_player_ref && av == vm::av::k_dragon_souls && amount > 0.0) {
            dragon_soul_absorbed();
        }
    });
    // Combat: the state lives in SkydotAi (hooks); without it nobody fights.
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    const auto self_state = [this](NativeCall& c) -> std::int64_t {
        return combat_hooks_.state ? combat_hooks_.state(c.self.form()) : 0;
    };
    vm_->bind("Actor", "IsInCombat", [self_state](NativeCall& c) {
        return NativeResult{.value = Value::boolean(self_state(c) != 0), .wait = -1.0};
    });
    vm_->bind("Actor", "GetCombatState", [self_state](NativeCall& c) {
        return NativeResult{.value = Value::integer(static_cast<std::int32_t>(self_state(c))), .wait = -1.0};
    });
    vm_->bind("Actor", "GetCombatTarget", [this](NativeCall& c) {
        const std::uint32_t target = combat_hooks_.target ? combat_hooks_.target(c.self.form()) : 0;
        if (target == 0) {
            return NativeResult{};
        }
        return NativeResult{.value = c.vm.object(target, "Actor"), .wait = -1.0};
    });
    vm_->bind("Actor", "StartCombat", [this](NativeCall& c) {
        if (combat_hooks_.start && !c.args.empty() && c.args[0].kind() == vm::Kind::object) {
            combat_hooks_.start(c.self.form(), c.args[0].form());
        }
        return NativeResult{};
    });
    // HasLOS(ObjectReference): a ray from the actor's eyes to the target (SkydotAi::has_los). Without
    // the AI nobody sees anything.
    vm_->bind("Actor", "HasLOS", [this](NativeCall& c) {
        const bool seen = combat_hooks_.has_los && !c.args.empty() && c.args[0].kind() == vm::Kind::object &&
                          combat_hooks_.has_los(c.self.form(), c.args[0].form());
        return NativeResult{.value = Value::boolean(seen), .wait = -1.0};
    });
    // Flight: the node's mover answers (SkydotAi hooks); a grounded or unbuilt actor does not fly.
    vm_->bind("Actor", "IsFlying", [this](NativeCall& c) {
        return NativeResult{.value = Value::boolean(combat_hooks_.is_flying && combat_hooks_.is_flying(c.self.form())),
                            .wait = -1.0};
    });
    vm_->bind("Actor", "GetFlyingState", [this](NativeCall& c) {
        const std::int64_t state = combat_hooks_.flying_state ? combat_hooks_.flying_state(c.self.form()) : 0;
        return NativeResult{.value = Value::integer(static_cast<std::int32_t>(state)), .wait = -1.0};
    });
    vm_->bind("Actor", "CanFlyHere", [this](NativeCall& c) {
        return NativeResult{.value = Value::boolean(combat_hooks_.can_fly_here && combat_hooks_.can_fly_here(c.self.form())),
                            .wait = -1.0};
    });
    vm_->bind("ObjectReference", "GetLength", [this](NativeCall& c) {
        const double length = combat_hooks_.length ? combat_hooks_.length(c.self.form()) : 0.0;
        return NativeResult{.value = Value::floating(static_cast<float>(length)), .wait = -1.0};
    });
    vm_->bind("Actor", "StopCombat", [this](NativeCall& c) {
        if (combat_hooks_.stop) {
            combat_hooks_.stop(c.self.form());
        }
        return NativeResult{};
    });
}

std::int64_t SkydotPapyrus::send_get_up(std::int64_t ref) {
    return static_cast<std::int64_t>(dispatch(static_cast<std::uint32_t>(ref), "OnGetUp", {vm::Value{}}));
}

std::int64_t SkydotPapyrus::combat_state_changed(std::int64_t ref, std::int64_t target, std::int64_t state) {
    const vm::Value who = target != 0 ? vm_->object(static_cast<std::uint32_t>(target), "Actor") : vm::Value{};
    return static_cast<std::int64_t>(dispatch(static_cast<std::uint32_t>(ref), "OnCombatStateChanged",
                                              {who, vm::Value::integer(static_cast<std::int32_t>(state))}));
}

void SkydotPapyrus::flush_deaths() {
    for (const auto ref : actors_.take_deaths()) {
        magic_actor_died(ref);
        emit_signal("actor_died", static_cast<std::int64_t>(ref));
        give_death_item(ref);
        // Every death, whoever dealt it (a hit, a spell, a shout, a script): OnDying, then OnDeath.
        const std::uint32_t killer_ref = actors_.killer_of(ref);
        const vm::Value killer = killer_ref != 0 ? vm_->object(killer_ref, "Actor") : vm::Value{};
        dispatch(ref, "OnDying", {killer});
        dispatch(ref, "OnDeath", {killer});
    }
}

double SkydotPapyrus::get_actor_value(std::int64_t ref, const String& name) const {
    const auto av = vm::av::index_of(utf8(name));
    return av ? actors_.get(static_cast<std::uint32_t>(ref), *av) : 0.0;
}

double SkydotPapyrus::get_actor_value_max(std::int64_t ref, const String& name) const {
    const auto av = vm::av::index_of(utf8(name));
    return av ? actors_.max(static_cast<std::uint32_t>(ref), *av) : 0.0;
}

void SkydotPapyrus::set_actor_value(std::int64_t ref, const String& name, double value) {
    if (const auto av = vm::av::index_of(utf8(name))) {
        actors_.set_base(static_cast<std::uint32_t>(ref), *av, value);
    }
}

void SkydotPapyrus::damage_actor_value(std::int64_t ref, const String& name, double amount) {
    if (const auto av = vm::av::index_of(utf8(name))) {
        actors_.damage(static_cast<std::uint32_t>(ref), *av, amount);
        flush_deaths();
        if (static_cast<std::uint32_t>(ref) == k_player_ref) {
            check_player_death();
        }
    }
}

void SkydotPapyrus::restore_actor_value(std::int64_t ref, const String& name, double amount) {
    if (const auto av = vm::av::index_of(utf8(name))) {
        actors_.restore(static_cast<std::uint32_t>(ref), *av, amount);
        if (player_dead_ && static_cast<std::uint32_t>(ref) == k_player_ref && *av == vm::av::k_health && amount > 0.0) {
            player_dead_ = false; // brought back (a restart)
            last_damager_ = 0;
        }
    }
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
        &SkydotPapyrus::bind_scene_natives,
        &SkydotPapyrus::bind_noop_natives,
        &SkydotPapyrus::bind_world_natives,
        &SkydotPapyrus::bind_actor_natives,
        &SkydotPapyrus::bind_actor_extra_natives,
        &SkydotPapyrus::bind_magic_natives,
        &SkydotPapyrus::bind_item_natives,
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
        auto quest = form_of(d.get("form", 0));
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
    std::set<std::uint32_t> visited;
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
            visited.insert(static_cast<std::uint32_t>(ref));
            attach(child->get_meta("skydot_cell"), ref);
            const auto ref_cell = form_of(child->get_meta("skydot_cell"));
            attached_cells_.insert(ref_cell);
            if (!vm_->instances(static_cast<std::uint32_t>(ref)).empty()) {
                loaded_[static_cast<std::uint32_t>(ref)] = ref_cell;
            }
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
    // A trigger volume has no model, so no node walked above: its scripts still get the load events
    // once (a word wall's trigger sets its link to the glow script in OnLoad).
    std::vector<std::uint32_t> volumes;
    for (const auto& [ref, cell] : loaded_) {
        if (!visited.contains(ref) && triggers_.triggers().contains(ref) && volume_loaded_.insert(ref).second) {
            volumes.push_back(ref);
        }
    }
    for (const std::uint32_t ref : volumes) {
        dispatch(ref, "OnCellAttach", {});
        dispatch(ref, "OnCellLoad", {});
        dispatch(ref, "OnLoad", {});
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
        if (!vm_->instances(static_cast<std::uint32_t>(refs[i])).empty()) {
            loaded_[static_cast<std::uint32_t>(refs[i])] = static_cast<std::uint32_t>(cell);
        }
    }
    return count;
}

std::int64_t SkydotPapyrus::detach_cell(std::int64_t cell) {
    const auto id = static_cast<std::uint32_t>(cell);
    attached_cells_.erase(id);
    std::vector<std::uint32_t> refs;
    for (const auto& [ref, in] : loaded_) {
        if (in == id) {
            refs.push_back(ref);
        }
    }
    std::int64_t kept = 0;
    for (const auto ref : refs) {
        if (!detaching_all_ && stays_loaded_ && stays_loaded_(ref)) {
            ++kept; // rescued by the streamer: it flies on, with its scripts
            continue;
        }
        loaded_.erase(ref);
        volume_loaded_.erase(ref);
        dispatch(ref, "OnCellDetach", {});
        dispatch(ref, "OnUnload", {});
        vm_->unregister_updates(ref);
        vm_->unregister_animation_events(ref);
    }
    return static_cast<std::int64_t>(refs.size()) - kept;
}

std::int64_t SkydotPapyrus::detach_all() {
    std::set<std::uint32_t> cells(attached_cells_);
    for (const auto& entry : loaded_) {
        cells.insert(entry.second);
    }
    std::int64_t count = 0;
    detaching_all_ = true;
    for (const auto cell : cells) {
        count += detach_cell(cell);
    }
    detaching_all_ = false;
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

void SkydotPapyrus::add_item(std::int64_t owner, std::int64_t item, std::int64_t count) {
    resolve_owner(static_cast<std::uint32_t>(owner));
    vm_->inventory().add(static_cast<std::uint32_t>(owner), static_cast<std::uint32_t>(item), count);
}

std::int64_t SkydotPapyrus::get_item_count(std::int64_t owner, std::int64_t item) const {
    resolve_owner(static_cast<std::uint32_t>(owner));
    return vm_->inventory().count(static_cast<std::uint32_t>(owner), static_cast<std::uint32_t>(item));
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
    // Scripts that registered for this event on this reference.
    const auto sender = static_cast<std::uint32_t>(ref);
    for (const auto* instance : vm_->send_animation_event(sender, utf8(event))) {
        const auto key = instance->cls->key() + ":" + vm::to_lower(utf8(event));
        if (animation_event_logged_.insert(key).second) {
            std::printf("papyrus: OnAnimationEvent %s from 0x%08X reaches %s\n", utf8(event).c_str(), sender,
                        instance->cls->name().c_str());
        }
        ++animation_events_delivered_;
    }
}

std::int64_t SkydotPapyrus::get_animation_event_count() const {
    return static_cast<std::int64_t>(animation_events_delivered_);
}

void SkydotPapyrus::notify_sound_finished(std::int64_t instance) {
    vm_->notify("sound:" + std::to_string(instance));
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

Dictionary SkydotPapyrus::get_moved_changes() const {
    Dictionary out;
    for (const auto& [ref, to] : states_.moved) {
        Array pair;
        pair.push_back(godot::Vector3(static_cast<float>(to.position[0]), static_cast<float>(to.position[1]),
                                      static_cast<float>(to.position[2])));
        pair.push_back(godot::Vector3(static_cast<float>(to.rotation[0]), static_cast<float>(to.rotation[1]),
                                      static_cast<float>(to.rotation[2])));
        out[static_cast<std::int64_t>(ref)] = pair;
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

String SkydotPapyrus::get_last_error() const { return last_error_; }

// ---- tests and inspection -------------------------------------------------

void SkydotPapyrus::check_player_death() {
    const double health = actors_.get(k_player_ref, vm::av::k_health);
    if (player_dead_) {
        // The dead do not regenerate: only a load or a restore brings him back.
        if (health > 0.0) {
            actors_.damage(k_player_ref, vm::av::k_health, health);
        }
        return;
    }
    if (health <= 0.0 && actors_.is_actor(k_player_ref)) {
        player_dead_ = true;
        godot::UtilityFunctions::print(String::utf8(
            (std::string("death: player killed by 0x") + [&] {
                char hex[16];
                std::snprintf(hex, sizeof hex, "%X", last_damager_);
                return std::string(hex);
            }()).c_str()));
        emit_signal("player_died", static_cast<std::int64_t>(last_damager_));
    }
}

void SkydotPapyrus::update(double seconds) {
    tick_magic(seconds);
    actors_.tick(seconds);
    vm_->update(seconds);
    flush_deaths();
    check_player_death();
}

std::int64_t SkydotPapyrus::call_global(const String& cls, const String& fn, const Array& args) {
    std::vector<vm::Value> values;
    for (std::int64_t i = 0; i < args.size(); ++i) {
        values.push_back(to_value(args[i], ""));
    }
    return static_cast<std::int64_t>(vm_->call_global(utf8(cls), utf8(fn), std::move(values)));
}

std::int64_t SkydotPapyrus::create_instance(const String& cls) {
    const auto form = test_ids_.allocate();
    if (form == 0) {
        godot::UtilityFunctions::push_error("Papyrus: the 4,096 ids create_instance hands out are used up");
        return 0;
    }
    return vm_->attach(form, utf8(cls)) != nullptr ? form : 0;
}

std::int64_t SkydotPapyrus::new_sound_instance() {
    const auto instance = sound_ids_.allocate();
    if (instance == 0) {
        godot::UtilityFunctions::push_error("Papyrus: the sound instance ids are used up");
    }
    return instance;
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
