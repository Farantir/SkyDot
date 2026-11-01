// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/papyrus.hpp"

#include "data/actors.hpp"
#include "data/coordinates.hpp"
#include "data/fb_search.hpp"
#include "data/scripts.hpp"
#include "perks/entry_points.hpp"
#include "skydot_formats/flags.hpp"
#include "world_generated.h"
#include "actors/behavior_player.hpp"
#include "physics/collision.hpp"
#include "vm/activation.hpp"
#include "vm/natives_actor.hpp"
#include "vm/papyrus_common.hpp"
#include "vm/natives_families.hpp"
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
    godot::ClassDB::bind_method(D_METHOD("spell_cost", "caster", "spell"), &SkydotPapyrus::spell_cost);
    godot::ClassDB::bind_method(D_METHOD("set_blocking", "actor", "blocking"), &SkydotPapyrus::set_blocking);
    godot::ClassDB::bind_method(D_METHOD("spend_magicka", "caster", "amount"), &SkydotPapyrus::spend_magicka);
    godot::ClassDB::bind_method(D_METHOD("apply_item_effects", "actor", "form", "extra"),
                                &SkydotPapyrus::apply_item_effects, DEFVAL(0));
    godot::ClassDB::bind_method(D_METHOD("spells_of", "actor"), &SkydotPapyrus::spells_of);
    godot::ClassDB::bind_method(D_METHOD("add_spell", "actor", "spell"), &SkydotPapyrus::add_spell);
    godot::ClassDB::bind_method(D_METHOD("remove_spell", "actor", "spell"), &SkydotPapyrus::remove_spell);
    godot::ClassDB::bind_method(D_METHOD("has_spell", "actor", "spell"), &SkydotPapyrus::has_spell);
    godot::ClassDB::bind_method(D_METHOD("equip_spell", "actor", "spell", "hand"), &SkydotPapyrus::equip_spell);
    godot::ClassDB::bind_method(D_METHOD("equipped_spell", "actor", "hand"), &SkydotPapyrus::equipped_spell);
    godot::ClassDB::bind_method(D_METHOD("unequip_spell", "actor", "hand"), &SkydotPapyrus::unequip_spell);
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
    godot::ClassDB::bind_method(D_METHOD("cast_shout", "actor", "shout", "words", "target"),
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
    godot::ClassDB::bind_method(D_METHOD("finish_dying", "ref"), &SkydotPapyrus::finish_dying);
    godot::ClassDB::bind_method(D_METHOD("faction_rank", "ref", "faction"), &SkydotPapyrus::faction_rank);
    godot::ClassDB::bind_method(D_METHOD("get_runtime_cell", "ref"), &SkydotPapyrus::get_runtime_cell);
    godot::ClassDB::bind_method(D_METHOD("get_item_count", "owner", "item"), &SkydotPapyrus::get_item_count);
    godot::ClassDB::bind_method(D_METHOD("get_actor_value", "ref", "name"), &SkydotPapyrus::get_actor_value);
    godot::ClassDB::bind_method(D_METHOD("get_base_actor_value", "ref", "name"), &SkydotPapyrus::get_base_actor_value);
    godot::ClassDB::bind_method(D_METHOD("get_actor_value_max", "ref", "name"),
                                &SkydotPapyrus::get_actor_value_max);
    godot::ClassDB::bind_method(D_METHOD("damage_actor_value", "ref", "name", "amount"),
                                &SkydotPapyrus::damage_actor_value);
    godot::ClassDB::bind_method(D_METHOD("set_actor_value", "ref", "name", "value"),
                                &SkydotPapyrus::set_actor_value);
    godot::ClassDB::bind_method(
        D_METHOD("hit_facts", "attacker", "target", "weapon", "extra", "attack_event", "flags", "ammo"),
        &SkydotPapyrus::hit_facts, DEFVAL(0));
    godot::ClassDB::bind_method(
        D_METHOD("land_hit", "attacker", "target", "weapon", "extra", "damage", "attack_event", "flags", "breakdown"),
        &SkydotPapyrus::land_hit);
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
    godot::ClassDB::bind_method(D_METHOD("get_player_progress"), &SkydotPapyrus::get_player_progress);
    godot::ClassDB::bind_method(D_METHOD("set_skill_xp", "skill_av", "xp"), &SkydotPapyrus::set_skill_xp);
    godot::ClassDB::bind_method(D_METHOD("set_player_xp", "xp"), &SkydotPapyrus::set_player_xp);
    godot::ClassDB::bind_method(D_METHOD("set_player_level", "level"), &SkydotPapyrus::set_player_level);
    godot::ClassDB::bind_method(D_METHOD("set_perk_points", "points"), &SkydotPapyrus::set_perk_points);
    godot::ClassDB::bind_method(D_METHOD("set_trainings", "n"), &SkydotPapyrus::set_trainings);
    godot::ClassDB::bind_method(D_METHOD("set_legendary", "skill_av", "n"), &SkydotPapyrus::set_legendary);
    godot::ClassDB::bind_method(D_METHOD("mark_book_read", "book"), &SkydotPapyrus::mark_book_read);
    godot::ClassDB::bind_method(D_METHOD("is_book_read", "book"), &SkydotPapyrus::is_book_read);
    godot::ClassDB::bind_method(D_METHOD("add_perk", "ref", "perk"), &SkydotPapyrus::add_perk);
    godot::ClassDB::bind_method(D_METHOD("remove_perk", "ref", "perk"), &SkydotPapyrus::remove_perk);
    godot::ClassDB::bind_method(D_METHOD("perk_rank", "ref", "perk"), &SkydotPapyrus::perk_rank);
    godot::ClassDB::bind_method(D_METHOD("get_perks", "ref"), &SkydotPapyrus::get_perks);
    godot::ClassDB::bind_method(D_METHOD("perk_conditions_met", "perk", "ref"), &SkydotPapyrus::perk_conditions_met);
    godot::ClassDB::bind_method(D_METHOD("perk_entry", "entry_point", "owner", "subjects", "value", "extra"),
                                &SkydotPapyrus::perk_entry, DEFVAL(godot::Dictionary()));
    godot::ClassDB::bind_method(D_METHOD("perk_entry_spells", "entry_point", "owner", "subjects", "extra"),
                                &SkydotPapyrus::perk_entry_spells, DEFVAL(godot::Dictionary()));
    godot::ClassDB::bind_method(D_METHOD("perk_entry_texts", "entry_point", "owner", "subjects", "extra"),
                                &SkydotPapyrus::perk_entry_texts, DEFVAL(godot::Dictionary()));
    godot::ClassDB::bind_method(D_METHOD("set_progression_handler", "handler"), &SkydotPapyrus::set_progression_handler);
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
    godot::ClassDB::bind_method(D_METHOD("forget_store_sizes"), &SkydotPapyrus::forget_store_sizes);
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
    register_forgets();
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
    vm_->set_scheduled_handler([this](const vm::ScheduledCall& call) {
        if (const auto* a = std::get_if<vm::ScheduledActivate>(&call)) {
            const Dictionary outcome = activate_ref(a->target, a->activator, a->default_only, a->source, false);
            emit_signal("activation_outcome", static_cast<std::int64_t>(a->target), outcome);
        }
    });
}

void SkydotPapyrus::reset() {
    if (world_.is_valid()) {
        world_->placement().clear_enabled();
    }
    attached_.clear();
    attached_cells_.clear();
    loaded_.clear();
    states_.clear();
    actors_.clear();
    clear_magic();
    item_.clear();
    created_.clear();
    dialogue_.reset();
    progression_.clear();
    blocking_.clear();
    scene_.reset();
    activation_.reset();
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
    world_->set_worn_provider(item_.worn_provider());
    vm_ = std::make_unique<vm::Vm>();
    register_forgets();
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

static Vector3 vec3_of_xyz(const vm::Xyz& p) {
    return Vector3(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
}

bool SkydotPapyrus::ref_record(std::uint32_t ref, vm::RefInfo& out) const {
    if (world_.is_null()) {
        return false;
    }
    out = vm::RefInfo{};
    if (const auto* placed = vm_->placed().find(ref)) {
        out.base = placed->base;
        out.cell = placed->cell;
        std::copy(std::begin(placed->position), std::end(placed->position), out.placement.position.begin());
        std::copy(std::begin(placed->rotation), std::end(placed->rotation), out.placement.rotation.begin());
        out.placement.scale = placed->scale;
        out.placement.base = placed->base;
        if (const auto* base = world_->data().base_ptr(placed->base)) {
            out.type = WorldData::type_name(base->type());
            if (base->editor_id() != nullptr) {
                out.editor_id = base->editor_id()->str();
            }
        }
        return true;
    }
    WorldData::RefRecord record;
    if (!world_->data().ref_record(ref, record)) {
        return false;
    }
    const auto widen = [](const std::array<float, 3>& v) {
        return vm::Xyz{static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2])};
    };
    out.base = record.base;
    out.cell = record.cell;
    out.type = std::move(record.type);
    out.editor_id = std::move(record.editor_id);
    out.actor = record.actor;
    out.parent_activate_only = record.parent_activate_only;
    out.placement.position = widen(record.position);
    out.placement.rotation = widen(record.rotation);
    out.placement.disabled = record.initially_disabled;
    out.placement.scale = static_cast<double>(record.scale);
    out.placement.base = record.base;
    out.locked = record.locked;
    out.lock_level = record.lock_level;
    out.lock_key = record.lock_key;
    out.load_door = record.load_door;
    out.door_destination = record.door_destination;
    for (const auto& link : record.links) {
        out.links.push_back({link.keyword, link.target});
    }
    return true;
}

std::int64_t SkydotPapyrus::get_ref_base(std::int64_t ref) const {
    vm::RefInfo info;
    return ref_record(static_cast<std::uint32_t>(ref), info) ? static_cast<std::int64_t>(info.base) : 0;
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
            if (world_.is_valid() && world_->data().actor_ptr(id) != nullptr) {
                world_->placement().set_enabled(id, !now); // the cell builder builds an actor a script enabled
            }
            emit_signal("enable_changed", static_cast<std::int64_t>(id), !now);
        }
    }
}

std::uint32_t SkydotPapyrus::lock_holder(std::uint32_t ref) const {
    // The two doors of a load door share one lock, and the plugin stores it
    // on one side only (Faendal's house: the inside door). A door without a
    // lock of its own answers with its partner's.
    vm::RefInfo info;
    if (!ref_record(ref, info) || info.locked || !info.load_door) {
        return ref;
    }
    const auto other = info.door_destination;
    vm::RefInfo partner;
    if (other != 0 && ref_record(other, partner) && partner.locked) {
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
    vm::RefInfo info;
    return ref_record(ref, info) && info.locked;
}

Vector3 SkydotPapyrus::rotation_of(std::uint32_t ref) const {
    if (const auto it = states_.moved.find(ref); it != states_.moved.end()) {
        return vec3_of(it->second.rotation);
    }
    vm::RefInfo info;
    return ref_record(ref, info) ? vec3_of_xyz(info.placement.rotation) : Vector3();
}

Vector3 SkydotPapyrus::position_of(std::uint32_t ref) const {
    if (const auto it = states_.moved.find(ref); it != states_.moved.end()) {
        return vec3_of(it->second.position);
    }
    // An actor on screen reports where it is (the AI, a few times a second); one that is not, or lies dead,
    // is where the placement says (its walk off screen, its corpse place).
    const ActorPlacement::Place* moved = world_.is_valid() ? world_->placement().moved_place(ref) : nullptr;
    if (const vm::Vec3* actor = triggers_.actor_position(ref); actor != nullptr && (moved == nullptr || !actors_.is_dead(ref))) {
        return Vector3(actor->x, actor->y, actor->z);
    }
    if (moved != nullptr) {
        return moved->position;
    }
    vm::RefInfo info;
    if (!ref_record(ref, info)) {
        return world_.is_valid() ? Vector3(world_->get_actor(ref).get("position", Vector3())) : Vector3();
    }
    return vec3_of_xyz(info.placement.position);
}

static Vector3 vec3_from(const vm::Xyz& p) {
    return Vector3(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
}

static vm::Xyz xyz_from(const Vector3& p) {
    return {static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z)};
}

/// What a perk's ability and quest effects do: Actor.AddSpell / RemoveSpell and Quest.SetStage.
struct PerkEffects final : perks::EffectSink {
    explicit PerkEffects(SkydotPapyrus& owner) : owner_(owner) {}
    void add_ability(std::uint32_t actor, std::uint32_t spell) override { owner_.add_spell(actor, spell); }
    void remove_ability(std::uint32_t actor, std::uint32_t spell) override { owner_.remove_spell(actor, spell); }
    void set_stage(std::uint32_t quest, std::uint16_t stage) override { owner_.set_stage(quest, stage); }
    SkydotPapyrus& owner_;
};

/// The world as natives ask about it: the Dictionaries behind it stay private
/// to SkydotPapyrus.
struct SkydotPapyrus::Access final : vm::WorldAccess, vm::WorldEvents, vm::SoundHost, vm::QuestHost, vm::ReferenceHost, vm::ActorHost,
                             vm::SceneRuntime::Host, vm::DialogueRuntime::Host, vm::ProgressionRuntime::Host, vm::CraftingRuntime::Host, vm::CombatRuntime::Host,
                             vm::ActivationRuntime::Host, vm::ItemRuntime::Host, vm::MagicRuntime::Host {
    explicit Access(SkydotPapyrus& owner) : owner_(owner) {}

    /// The typed world data, null without a world.
    const WorldData* world() const { return owner_.world_.is_valid() ? &owner_.world_->data() : nullptr; }

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
        return static_cast<std::uint32_t>(owner_.world_->get_form_from_file(id, String::utf8(plugin.c_str())));
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

    // ---- vm::WorldEvents: each one is the signal it was, with the same arguments.

    void trace(const std::string& text) override {
        const String shown = natives::to_godot(text);
        godot::UtilityFunctions::print(String("[Papyrus] ") + shown);
        owner_.emit_signal("trace", shown);
    }
    void message(const std::string& text, bool box, bool help) override {
        const String shown = natives::to_godot(text);
        godot::UtilityFunctions::print(String(box ? "[Message] " : help ? "[Help] " : "[Notification] ") + shown);
        owner_.emit_signal("message", shown, box);
    }

    void effect_shader(std::uint32_t shader, std::uint32_t target, bool play) override {
        owner_.emit_signal("effect_shader", static_cast<std::int64_t>(shader), static_cast<std::int64_t>(target), play);
    }

    void fire_weapon(std::uint32_t weapon, std::uint32_t source, std::uint32_t ammo) override {
        owner_.emit_signal("fire_weapon", static_cast<std::int64_t>(weapon), static_cast<std::int64_t>(source),
                           static_cast<std::int64_t>(ammo));
    }

    void play_sound(std::uint32_t sound, std::uint32_t source, std::int64_t instance) override {
        owner_.emit_signal("play_sound", static_cast<std::int64_t>(sound), static_cast<std::int64_t>(source), instance);
    }
    void stop_sound(std::int64_t instance) override { owner_.emit_signal("stop_sound", instance); }
    void sound_volume(std::int64_t instance, double volume) override {
        owner_.emit_signal("sound_volume", instance, volume);
    }

    // ---- vm::SoundHost

    std::int64_t new_sound_instance() override { return owner_.new_sound_instance(); }

    void animation_events_wanted(std::uint32_t sender) override {
        owner_.emit_signal("animation_events_wanted", static_cast<std::int64_t>(sender));
    }

    // ---- vm::QuestHost

    void attach_quest_scripts(std::uint32_t quest, bool on_init) override {
        owner_.attach_quest_scripts(quest, on_init);
    }
    vm::QuestStates& quests() override { return owner_.quests_; }
    bool start_quest(std::int64_t quest) override { return owner_.start_quest(quest); }
    void stop_quest(std::int64_t quest) override { owner_.stop_quest(quest); }
    bool set_stage(std::int64_t quest, std::int64_t stage) override { return owner_.set_stage(quest, stage); }
    void set_objective(std::uint32_t quest, std::int32_t index, std::uint8_t bit, bool on) override {
        owner_.set_objective(quest, index, bit, on);
    }
    bool owning_quest(std::uint32_t info, std::uint32_t& quest) const override {
        return owner_.dialogue_.owning_quest(info, quest);
    }
    bool alias_info(std::uint32_t quest, std::int64_t id, vm::AliasInfo& out) const override {
        const Array aliases = owner_.quest_info(quest).get("aliases", Array());
        for (std::int64_t i = 0; i < aliases.size(); ++i) {
            const Dictionary a = aliases[i];
            if (static_cast<std::int64_t>(a["id"]) == id) {
                out.name = natives::utf8(a["name"]);
                out.location = static_cast<bool>(a["location"]);
                return true;
            }
        }
        return false;
    }
    std::uint32_t alias_handle(std::uint32_t quest, std::uint32_t alias) override {
        return owner_.alias_handle(quest, alias);
    }
    double global_value(std::int64_t global) const override { return owner_.get_global_value(global); }
    void set_global_value(std::int64_t global, double value) override { owner_.set_global_value(global, value); }

    void ref_moved(std::uint32_t ref, const vm::Xyz& position, const vm::Xyz& rotation) override {
        owner_.emit_signal("ref_moved", static_cast<std::int64_t>(ref), vec3_from(position), vec3_from(rotation));
    }
    void play_animation(std::uint32_t ref, const std::string& animation) override {
        owner_.emit_signal("play_animation", static_cast<std::int64_t>(ref), natives::to_godot(animation));
    }
    void havok_impulse(std::uint32_t ref, const vm::Xyz& direction, double magnitude) override {
        owner_.emit_signal("havok_impulse", static_cast<std::int64_t>(ref), vec3_from(direction), magnitude);
    }
    void push_actor(std::uint32_t source, std::uint32_t target, double force) override {
        owner_.emit_signal("push_actor", static_cast<std::int64_t>(source), static_cast<std::int64_t>(target), force);
    }
    void knock_area(std::uint32_t source, double magnitude, double radius) override {
        owner_.emit_signal("knock_area", static_cast<std::int64_t>(source), magnitude, radius);
    }
    void motion_type_changed(std::uint32_t ref, std::int32_t motion_type) override {
        owner_.emit_signal("motion_type_changed", static_cast<std::int64_t>(ref),
                           static_cast<std::int64_t>(motion_type));
    }
    void open_changed(std::uint32_t ref, bool open) override {
        owner_.emit_signal("open_changed", static_cast<std::int64_t>(ref), open);
    }
    void visual_effect(std::uint32_t ref, std::uint32_t art, std::uint32_t target, double duration, bool play) override {
        owner_.emit_signal("visual_effect", static_cast<std::int64_t>(ref), static_cast<std::int64_t>(art),
                           static_cast<std::int64_t>(target), duration, play);
    }
    void sub_graph_animation(std::uint32_t ref, const std::string& animation) override {
        owner_.emit_signal("sub_graph_animation", static_cast<std::int64_t>(ref), natives::to_godot(animation));
    }

    // ---- vm::ActorHost

    vm::ActorStates& actor_states() override { return owner_.actors_; }
    conditions::ConditionWorld& conditions() override { return owner_.condition_world(); }
    // ---- vm::ActivationRuntime::Host
    vm::ReferenceHost& references() override { return *this; }
    std::uint32_t placed_actor_base(std::uint32_t ref) const override {
        const auto* actor = world() != nullptr ? world()->actor_ptr(ref) : nullptr;
        return actor != nullptr ? actor->base() : 0;
    }
    bool graph_raises_activation(std::uint32_t ref) const override { return owner_.graph_raises_activation(ref); }
    std::int64_t send_activate(std::uint32_t ref, std::int64_t activator) override { return owner_.activate(ref, activator); }
    void schedule_activation(double delay, std::uint32_t child, std::uint32_t parent) override {
        owner_.vm_->schedule(
            delay, vm::ScheduledActivate{child, parent, static_cast<std::uint8_t>(ActivationSource::parent), false});
    }
    vm::ItemAccess& items() override { return owner_.item_; }
    std::optional<double> global_value_of(std::uint32_t id) const override { return owner_.get_global_value(id); }
    void forget_ref(std::uint32_t ref) override { owner_.forget_ref(ref); }

    // ---- vm::MagicRuntime::Host
    std::uint32_t dialogue_npc(std::uint32_t ref) const override { return owner_.dialogue_npc(ref); }
    std::uint32_t place_summon_ref(const vm::PlacedRef& placed) override { return owner_.place_ref(placed, false); }
    std::vector<std::uint32_t> triggered_actors() const override {
        std::vector<std::uint32_t> out;
        for (const auto& [actor, position] : owner_.triggers_.actors()) {
            out.push_back(actor);
        }
        return out;
    }
    void magic_trace(const std::string& line) override {
        owner_.emit_signal("trace", natives::to_godot(line));
        godot::UtilityFunctions::print(natives::to_godot(line));
    }
    void magic_notification(const std::string& text) override {
        godot::UtilityFunctions::print(natives::to_godot("[Notification] " + text));
        owner_.emit_signal("message", natives::to_godot(text), false);
    }
    void effect_event(const magic::EffectView& v, bool started) override {
        owner_.emit_signal("magic_effect_changed", static_cast<std::int64_t>(v.target), static_cast<std::int64_t>(v.mgef),
                           static_cast<std::int64_t>(v.archetype), started, static_cast<std::int64_t>(v.id));
        owner_.emit_signal("magic_effect_event", effect_event_dictionary(v, started));
    }
    void attach_effect_scripts(std::uint32_t id, std::uint32_t mgef) override {
        const auto* root = world() != nullptr ? world()->root() : nullptr;
        const auto* record = root != nullptr ? lookup(root->magic_effects(), mgef) : nullptr;
        owner_.attach_scripts(id, script_list(record != nullptr ? record->scripts() : nullptr, false), 0);
    }

    // ---- vm::ItemRuntime::Host
    vm::Vm& vm() override { return *owner_.vm_; }
    void dispatch_event(std::uint32_t ref, std::string_view event, const std::vector<vm::Value>& args) override {
        owner_.dispatch(ref, event, args);
    }
    void sync_worn_magic(std::uint32_t owner) override { owner_.sync_worn_magic(owner); }
    void worn_changed(std::uint32_t owner) override { owner_.emit_signal("worn_changed", static_cast<std::int64_t>(owner)); }
    std::optional<bool> apply_consumed(std::uint32_t owner, std::uint32_t form, std::uint32_t extra) override {
        const auto applied = owner_.apply_item_effects(owner, form, extra);
        if (String(applied.get("reason", String())) == "poison") {
            return static_cast<bool>(owner_.poison_weapon(owner, form, static_cast<std::int64_t>(extra)).get("ok", false));
        }
        return std::nullopt;
    }
    void note_eaten(std::uint32_t form) override {
        if (auto* logic = owner_.crafting_logic()) {
            logic->note_eaten(form);
        }
    }
    void item_trace(const std::string& line) override { owner_.emit_signal("trace", natives::to_godot(line)); }

    // ---- vm::CraftingRuntime::Host
    vm::Inventory& inventory() override { return owner_.vm_->inventory(); }
    vm::ItemExtras& item_extras() override { return owner_.item_.states().extras(); }
    vm::CreatedForms& created_forms() override { return owner_.created_; }
    std::unique_ptr<conditions::ConditionWorld> new_condition_world() override {
        return std::make_unique<PapyrusConditionWorld>(&owner_);
    }

    void announce(const std::string& line) override {
        const String shown = natives::to_godot(line);
        owner_.emit_signal("trace", shown);
        godot::UtilityFunctions::print(shown);
    }

    // ---- vm::DialogueRuntime::Host
    vm::ActorHost& actor_host() override { return *this; }
    std::uint32_t base_of(std::uint32_t ref) const override { return owner_.dialogue_npc(ref); }

    // ---- vm::MagicRuntime::Host: the perks' entry points for spell cost, magnitude and duration
    double perk_entry(std::uint8_t ep, std::uint32_t owner, std::array<std::uint32_t, 3> subjects,
                      double value) const override {
        const auto* root = world() != nullptr ? world()->root() : nullptr;
        if (root == nullptr || ep >= perks::k_entry_point_count) {
            return value;
        }
        const perks::EntryPoints points(*root, owner_.condition_world(),
                                        [this](std::uint32_t ref) { return owner_.progression_.perks_of(ref); });
        perks::EntryContext ctx;
        ctx.owner = owner;
        ctx.subjects = subjects;
        ctx.worn = owner_.item_.worn_armor_of(owner);
        return points.apply(static_cast<perks::EntryPoint>(ep), ctx, value);
    }

    // ---- vm::MagicRuntime::Host: the abilities of an actor's base perks are part of its spell list
    std::vector<std::uint32_t> perk_abilities(std::uint32_t ref) override {
        std::vector<std::uint32_t> out;
        const auto* root = world() != nullptr ? world()->root() : nullptr;
        if (root == nullptr) {
            return out;
        }
        for (const auto& [perk, rank] : owner_.progression_.base_perks(ref)) {
            const auto* p = lookup(root->perks(), perk);
            if (p == nullptr || p->effects() == nullptr || rank == 0) {
                continue;
            }
            for (const auto* e : *p->effects()) {
                if (e->kind() == bethconv::pack::wfb::PerkEffectKind::ability && e->spell() != 0 &&
                    static_cast<unsigned>(e->rank()) + 1U == rank) {
                    out.push_back(e->spell());
                }
            }
        }
        return out;
    }

    // ---- vm::ProgressionRuntime::Host: a perk's ability and quest effects (perks/entry_points.hpp)
    void perk_gained(std::uint32_t ref, std::uint32_t perk) override {
        if (const auto* root = world() != nullptr ? world()->root() : nullptr) {
            PerkEffects sink{owner_};
            perks::perk_added(*root, ref, perk, sink);
        }
    }
    void perk_lost(std::uint32_t ref, std::uint32_t perk) override {
        if (const auto* root = world() != nullptr ? world()->root() : nullptr) {
            PerkEffects sink{owner_};
            perks::perk_removed(*root, ref, perk, sink);
        }
    }

    // ---- vm::SceneRuntime::Host
    const bethconv::pack::wfb::World* world_root() const override { return world() != nullptr ? world()->root() : nullptr; }
    vm::WorldAccess& world_access() override { return *this; }
    vm::QuestHost& quest_host() override { return *this; }
    conditions::ConditionWorld& condition_world() override { return owner_.condition_world(); }
    void attach_scene_scripts(std::uint32_t scene) override {
        if (const auto* s = world() != nullptr ? lookup(world()->root()->scenes(), scene) : nullptr) {
            owner_.attach_scripts(scene, script_list(s->scripts(), false), s->quest());
        }
    }
    void package_signal(std::uint32_t actor, std::uint32_t package, bool push) override {
        owner_.emit_signal("scene_package", static_cast<std::int64_t>(actor), static_cast<std::int64_t>(package), push);
    }
    void log(const std::string& text) override { godot::UtilityFunctions::print(natives::to_godot(text)); }

    bool location_loaded(std::uint32_t location) override { return owner_.location_loaded(location); }
    void evaluate_package(std::uint32_t ref) override { owner_.evaluate_package(ref); }
    void set_last_damager(std::uint32_t killer) override { owner_.combat_.set_last_damager(killer); }

    // ---- vm::ReferenceHost

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
        owner_.move_ref_to(ref, target, vec3_from(offset), match_rotation);
    }
    void relocate_ref(std::uint32_t ref, const vm::Xyz& position, const vm::Xyz& rotation) override {
        owner_.relocate_ref(ref, 0, vec3_from(position), vec3_from(rotation));
    }
    void send_event(std::uint32_t ref, std::string_view event, const std::vector<vm::Value>& args) override {
        owner_.dispatch(ref, event, args);
    }
    void resolve_owner(std::uint32_t owner) const override { owner_.item_.resolve_owner(owner); }
    std::vector<vm::Inventory::Taken> remove_item_split(std::uint32_t owner, std::uint32_t form,
                                                        std::int64_t count) override {
        return owner_.item_.remove_item_split(owner, form, count);
    }

private:
    SkydotPapyrus& owner_;
};

void SkydotPapyrus::AccessDeleter::operator()(Access* access) const { delete access; }

std::unique_ptr<vm::MagicRuntime> SkydotPapyrus::new_magic(const bethconv::pack::wfb::World& root) {
    return std::make_unique<vm::MagicRuntime>(*access_, root, item_, combat_);
}

void SkydotPapyrus::bind_item_natives() { item_.bind_natives(*vm_, *access_); }

void SkydotPapyrus::bind_activations() {
    // Activations work before `setup` too: the access object is made now and again by bind_natives.
    if (!access_) {
        access_.reset(new Access(*this));
    }
    activation_.bind(*access_);
    item_.bind(*access_);
}

void SkydotPapyrus::bind_debug_natives() { vm::bind_debug_natives(*vm_, *access_, *access_); }
void SkydotPapyrus::bind_utility_natives() { vm::bind_utility_natives(*vm_); }
void SkydotPapyrus::bind_effectshader_natives() { vm::bind_effectshader_natives(*vm_, *access_); }
void SkydotPapyrus::bind_weapon_natives() { vm::bind_weapon_natives(*vm_, *access_); }
void SkydotPapyrus::bind_sound_natives() { vm::bind_sound_natives(*vm_, *access_, *access_); }
void SkydotPapyrus::bind_form_natives() { vm::bind_form_natives(*vm_, *access_, *access_); }
void SkydotPapyrus::bind_game_natives() { vm::bind_game_natives(*vm_, *access_); }
void SkydotPapyrus::bind_quest_natives() { vm::bind_quest_natives(*vm_, *access_, *access_); }
void SkydotPapyrus::bind_objectreference_natives() { vm::bind_objectreference_natives(*vm_, *access_, *access_, *access_); }
void SkydotPapyrus::bind_actor_extra_natives() { vm::bind_actor_extra_natives(*vm_, *access_, *access_, *access_); }
void SkydotPapyrus::bind_noop_natives() { vm::bind_noop_natives(*vm_); }

void SkydotPapyrus::bind_world_natives() {
    vm::bind_world_natives(*vm_, *access_);
    actors_.set_source([this](std::uint32_t ref, vm::ActorBase& out) { return access_->actor_base(ref, out); });
    if (world_.is_valid()) {
        const WorldData& data = world_->data();
        actors_.set_regen_delays(
            static_cast<double>(data.gmst_float("fDamagedMagickaRegenDelay",
                                                static_cast<float>(vm::ActorStates::k_damaged_magicka_regen_delay))),
            static_cast<double>(data.gmst_float("fDamagedStaminaRegenDelay",
                                                static_cast<float>(vm::ActorStates::k_damaged_stamina_regen_delay))));
        actors_.set_combat_regen(
            static_cast<double>(data.gmst_float("fCombatHealthRegenRateMult",
                                                static_cast<float>(vm::ActorStates::k_combat_health_regen_mult))),
            static_cast<double>(data.gmst_float("fCombatMagickaRegenRateMult",
                                                static_cast<float>(vm::ActorStates::k_combat_magicka_regen_mult))),
            static_cast<double>(data.gmst_float("fCombatStaminaRegenRateMult",
                                                static_cast<float>(vm::ActorStates::k_combat_stamina_regen_mult))));
    }
    // Who fights is SkydotAi's (the hooks are set later; read when ticking).
    actors_.set_in_combat([this](std::uint32_t ref) { return combat_.in_combat(ref); });
}

void SkydotPapyrus::bind_actor_natives() {
    vm::bind_actor_natives(*vm_, actors_, [this](std::uint32_t ref, std::uint32_t av, double amount) {
        if (ref == k_player_ref && av == vm::av::k_dragon_souls && amount > 0.0) {
            dragon_soul_absorbed();
        }
    });
    combat_.bind_natives(*vm_, *access_, *access_);
}

std::int64_t SkydotPapyrus::send_get_up(std::int64_t ref) {
    return static_cast<std::int64_t>(dispatch(static_cast<std::uint32_t>(ref), "OnGetUp", {vm::Value{}}));
}

std::int64_t SkydotPapyrus::combat_state_changed(std::int64_t ref, std::int64_t target, std::int64_t state) {
    const vm::Value who = target != 0 ? vm_->object(static_cast<std::uint32_t>(target), "Actor") : vm::Value{};
    return static_cast<std::int64_t>(dispatch(static_cast<std::uint32_t>(ref), "OnCombatStateChanged",
                                              {who, vm::Value::integer(static_cast<std::int32_t>(state))}));
}

void SkydotPapyrus::send_on_death(std::uint32_t ref) {
    const std::uint32_t killer_ref = actors_.killer_of(ref);
    dispatch(ref, "OnDeath", {killer_ref != 0 ? vm_->object(killer_ref, "Actor") : vm::Value{}});
}

bool SkydotPapyrus::finish_dying(std::int64_t ref) {
    const auto id = static_cast<std::uint32_t>(ref);
    if (!actors_.finish_dying(id)) {
        return false;
    }
    send_on_death(id);
    return true;
}

void SkydotPapyrus::flush_deaths() {
    for (const auto ref : actors_.take_deaths()) {
        magic_actor_died(ref);
        emit_signal("actor_died", static_cast<std::int64_t>(ref));
        item_.give_death_item(ref);
        // Every death, whoever dealt it (a hit, a spell, a shout, a script): OnDying at once; OnDeath
        // when the actor has finished dying, which for a body that takes time (a death clip, a
        // crash) is when the game layer says it is at rest (`finish_dying`), or after a timeout.
        const std::uint32_t killer_ref = actors_.killer_of(ref);
        const vm::Value killer = killer_ref != 0 ? vm_->object(killer_ref, "Actor") : vm::Value{};
        dispatch(ref, "OnDying", {killer});
        if (combat_.hooks().defers_death && combat_.hooks().defers_death(ref)) {
            actors_.defer_death(ref, k_dying_timeout);
        } else {
            dispatch(ref, "OnDeath", {killer});
        }
    }
    for (const auto ref : actors_.take_due_deaths()) {
        send_on_death(ref);
    }
}

double SkydotPapyrus::get_actor_value(std::int64_t ref, const String& name) const {
    const auto av = vm::av::index_of(utf8(name));
    return av ? actors_.get(static_cast<std::uint32_t>(ref), *av) : 0.0;
}

double SkydotPapyrus::get_base_actor_value(std::int64_t ref, const String& name) const {
    const auto av = vm::av::index_of(utf8(name));
    return av ? actors_.get_base(static_cast<std::uint32_t>(ref), *av) : 0.0;
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
        if (combat_.player_dead() && static_cast<std::uint32_t>(ref) == k_player_ref && *av == vm::av::k_health &&
            amount > 0.0) {
            combat_.player_restored(); // brought back (a restart)
        }
    }
}

void SkydotPapyrus::bind_natives() {
    // One registration per class, each in vm/natives_<class>.cpp.
    access_.reset(new Access(*this));
    activation_.bind(*access_);
    item_.bind(*access_);
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
        &SkydotPapyrus::bind_actor_natives,
        &SkydotPapyrus::bind_actor_extra_natives,
        &SkydotPapyrus::bind_magic_natives,
        &SkydotPapyrus::bind_item_natives,
        &SkydotPapyrus::bind_runtime_natives,
    };
    for (const Registration registration : table) {
        (this->*registration)();
    }
    dialogue_.bind(*access_);
    progression_.bind(*access_);
    progression_.clear();
    crafting_.bind(*access_);
    scene_.bind_natives(*vm_, *access_, dialogue_);
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
    return SkydotWorld::ref_node(ref) != nullptr && !disabled(ref);
}

void SkydotPapyrus::add_item(std::int64_t owner, std::int64_t item, std::int64_t count) {
    item_.resolve_owner(static_cast<std::uint32_t>(owner));
    vm_->inventory().add(static_cast<std::uint32_t>(owner), static_cast<std::uint32_t>(item), count);
}

std::int64_t SkydotPapyrus::get_item_count(std::int64_t owner, std::int64_t item) const {
    item_.resolve_owner(static_cast<std::uint32_t>(owner));
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
    vm::RefInfo info;
    return ref_record(form, info) && info.locked ? static_cast<std::int64_t>(info.lock_level) : 0;
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
    vm::RefInfo info;
    return ref_record(form, info) && info.type == "DOOR" ? k_closed : 0;
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
    if (combat_.check_player_death()) {
        emit_signal("player_died", static_cast<std::int64_t>(combat_.last_damager()));
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
    if (test_ids_.take_pressure_warning()) {
        godot::UtilityFunctions::push_warning("Papyrus: the ids create_instance hands out are 90 % used");
    }
    return vm_->attach(form, utf8(cls)) != nullptr ? form : 0;
}

std::int64_t SkydotPapyrus::new_sound_instance() {
    const auto instance = sound_ids_.allocate();
    if (instance == 0) {
        godot::UtilityFunctions::push_error("Papyrus: the sound instance ids are used up");
    } else if (sound_ids_.take_pressure_warning()) {
        godot::UtilityFunctions::push_warning("Papyrus: the sound instance ids are 90 % used");
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
