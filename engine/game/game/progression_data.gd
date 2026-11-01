# SPDX-License-Identifier: GPL-3.0-or-later
#
# What Progression asks the data (ARCHITECTURE.md 8.2, ADR 0009): narrow questions over
# SkydotPapyrus and SkydotWorld, and no rule of its own. GMSTs with defaults, the AVIF
# use/improve values of a skill, the player's base and current actor values (set, restore),
# the progress dictionary (SkydotPapyrus.get_player_progress) and its setters, perks (PERK data,
# ranks, add, remove, the prerequisite conditions) and where a perk stands in a skill's perk
# tree. A native object cannot be faked, so Progression's unit tests (tests/gd/test_progression.gd)
# extend this class and answer from dictionaries. A mod does not override this; it overrides
# Progression's rule methods.
class_name ProgressionData
extends RefCounted

const PLAYER_REF := 0x14
const FIRST_SKILL := 6
const LAST_SKILL := 23

## Actor value names by index (the engine's table, state/actor_values.hpp), for the values the
## rules touch: the 18 skills, Health, Magicka, Stamina and CarryWeight.
const AV_NAMES := {
	6: "OneHanded", 7: "TwoHanded", 8: "Marksman", 9: "Block", 10: "Smithing", 11: "HeavyArmor",
	12: "LightArmor", 13: "Pickpocket", 14: "Lockpicking", 15: "Sneak", 16: "Alchemy",
	17: "Speechcraft", 18: "Alteration", 19: "Conjuration", 20: "Destruction", 21: "Illusion",
	22: "Restoration", 23: "Enchanting", 24: "Health", 25: "Magicka", 26: "Stamina",
	32: "CarryWeight",
}

var _papyrus: SkydotPapyrus
var _world: SkydotWorld
var _nodes := {}   # perk -> {av, node, tree}, built from the skills' perk trees on first use


func _init(papyrus: SkydotPapyrus = null, world: SkydotWorld = null) -> void:
	_papyrus = papyrus
	_world = world


func gmst_float(name: String, fallback: float) -> float:
	return _world.get_gmst_float(name, fallback)


func gmst_int(name: String, fallback: int) -> int:
	return _world.get_gmst_int(name, fallback)


func gmst_string(name: String, fallback: String) -> String:
	return _world.get_gmst_string(name, fallback)


## SkydotWorld.get_actor_value_info of a skill: {name, use_mult, use_offset, improve_mult,
## improve_offset, perk_tree: [{index, perk, links}], ...}; empty if unknown.
func skill_info(av: int) -> Dictionary:
	return _world.get_actor_value_info(av)


func base_av(av: int) -> float:
	return _papyrus.get_base_actor_value(PLAYER_REF, AV_NAMES.get(av, ""))


func current_av(av: int) -> float:
	return _papyrus.get_actor_value(PLAYER_REF, AV_NAMES.get(av, ""))


func set_base_av(av: int, value: float) -> void:
	_papyrus.set_actor_value(PLAYER_REF, AV_NAMES.get(av, ""), value)


func mod_base_av(av: int, delta: float) -> void:
	set_base_av(av, base_av(av) + delta)


## Fill `av` up to its maximum (Health, Magicka, Stamina).
func restore_av(av: int) -> void:
	var name: String = AV_NAMES.get(av, "")
	var missing := _papyrus.get_actor_value_max(PLAYER_REF, name) - _papyrus.get_actor_value(PLAYER_REF, name)
	if missing > 0.0:
		_papyrus.restore_actor_value(PLAYER_REF, name, missing)


## {skill_xp: [18], xp, level, perk_points, trainings, legendary: [18], books_read}.
func progress() -> Dictionary:
	return _papyrus.get_player_progress()


func set_skill_xp(av: int, xp: float) -> void:
	_papyrus.set_skill_xp(av, xp)


func set_xp(xp: float) -> void:
	_papyrus.set_player_xp(xp)


func set_level(level: int) -> void:
	_papyrus.set_player_level(level)


func set_perk_points(points: int) -> void:
	_papyrus.set_perk_points(points)


func set_legendary(av: int, count: int) -> void:
	_papyrus.set_legendary(av, count)


## SkydotWorld.get_perk: {name, playable, next_perk, conditions, ...}; empty if unknown.
func perk(id: int) -> Dictionary:
	return _world.get_perk(id)


func perk_rank(id: int) -> int:
	return int(_papyrus.perk_rank(PLAYER_REF, id))


func add_perk(id: int) -> void:
	_papyrus.add_perk(PLAYER_REF, id)


func remove_perk(id: int) -> void:
	_papyrus.remove_perk(PLAYER_REF, id)


## SkydotPapyrus.perk_entry for the player's perks (`extra` names the moment: advance_skill ...).
func perk_entry(entry_point: int, subjects: Array, value: float, extra := {}) -> float:
	return _papyrus.perk_entry(entry_point, PLAYER_REF, subjects, value, extra)


## Whether the PERK's own conditions (its prerequisites) pass for the player.
func perk_conditions_met(id: int) -> bool:
	return _papyrus.perk_conditions_met(id, PLAYER_REF)


## The perk tree of a skill: [{index, perk, links}]; node 0 is the root (perk 0).
func perk_tree(av: int) -> Array:
	return skill_info(av).get("perk_tree", [])


## Where `id` (any rank of a chain) stands: {av, node, tree}, the skill whose tree has the node
## whose rank chain holds it; empty if no tree does. Built once from the trees.
func perk_node(id: int) -> Dictionary:
	if _nodes.is_empty():
		for av in range(FIRST_SKILL, LAST_SKILL + 1):
			var tree := perk_tree(av)
			for node: Dictionary in tree:
				var p := int(node.get("perk", 0))
				var guard := 0
				while p != 0 and guard < 16:
					_nodes[p] = {"av": av, "node": node, "tree": tree}
					p = int(perk(p).get("next_perk", 0))
					guard += 1
		_nodes[-1] = {}
	return _nodes.get(id, {})


# ---- books and training

## SkydotPapyrus.item_info of a book: {category, book_flags, book_skill, book_spell, ...}.
func item_info(form: int) -> Dictionary:
	return _papyrus.item_info(form)


func book_read(book: int) -> bool:
	return _papyrus.is_book_read(book)


func mark_book_read(book: int) -> void:
	_papyrus.mark_book_read(book)


func has_spell(spell: int) -> bool:
	return _papyrus.has_spell(PLAYER_REF, spell)


func add_spell(spell: int) -> void:
	_papyrus.add_spell(PLAYER_REF, spell)


## The name of a form (a spell) for a message; empty if it has none.
func form_name(form: int) -> String:
	return String(_world.get_base(form).get("name", ""))


## Destroy one `form` from the player's inventory; false if it was not carried.
func take_item(form: int) -> bool:
	return _papyrus.remove_item(PLAYER_REF, form, 1) > 0


func set_trainings(count: int) -> void:
	_papyrus.set_trainings(count)


## The trainer's class data: {teaches (skill AV, -1 none), max_training_level, ...}; empty if it has none.
## `trainer` is a reference (its base's NPC_ is read) or an NPC_ base.
func trainer_class(trainer: int) -> Dictionary:
	var base := _papyrus.get_ref_base(trainer)
	var npc := _world.get_npc(base if base != 0 else trainer)
	return _world.get_class_data(int(npc.get("class", 0))) if not npc.is_empty() else {}


const GOLD := 0xF


func gold() -> int:
	return _papyrus.get_item_count(PLAYER_REF, GOLD)


## Move `amount` gold from the player to `trainer`; false if the player has too little.
func pay_gold(trainer: int, amount: int) -> bool:
	if gold() < amount:
		return false
	return _papyrus.transfer(PLAYER_REF, trainer, GOLD, amount, -1) >= amount
