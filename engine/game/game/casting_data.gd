# SPDX-License-Identifier: GPL-3.0-or-later
#
# What Casting asks the data (ARCHITECTURE.md 8.2, ADR 0009): narrow questions over
# SkydotPapyrus and SkydotWorld, and no rule of its own. The equipped spell of a hand, its
# cast_info, whether the caster is dead, its Magicka, what a spell costs, a power's cooldown,
# whether an item is carried, `prime` (the actor's magic is set up), paying a spell, ending
# invisibility, a shout's info, the voice recovery and the GMSTs. A native object cannot be
# faked, so Casting's unit tests (tests/gd/test_casting.gd) extend this class and answer from
# dictionaries. A mod does not override this; it overrides Casting's rule methods.
class_name CastingData
extends RefCounted

const PLAYER_REF := 0x14

var _papyrus: SkydotPapyrus
var _world: SkydotWorld


func _init(papyrus: SkydotPapyrus = null, world: SkydotWorld = null) -> void:
	_papyrus = papyrus
	_world = world


## The spell or scroll in `hand` (0 left, 1 right, 2 power) of `caster`; 0 if none.
func equipped_spell(caster: int, hand: int) -> int:
	return int(_papyrus.equipped_spell(caster, hand))


## Empty `hand` of `caster`.
func unequip_spell(caster: int, hand: int) -> void:
	_papyrus.unequip_spell(caster, hand)


## SkydotPapyrus.cast_info of `spell` ({found: false} if unknown; `is_item` tells a scroll
## item from a SPEL).
func cast_info(spell: int) -> Dictionary:
	return _papyrus.cast_info(spell)


func is_dead(caster: int) -> bool:
	if caster == PLAYER_REF:
		return _papyrus.is_player_dead()
	return _papyrus.is_actor_dead(caster)


func magicka(caster: int) -> float:
	return _papyrus.get_actor_value(caster, "Magicka")


func spell_cost(caster: int, spell: int) -> float:
	return _papyrus.spell_cost(caster, spell)


## Game days until the power may be used again; 0 if it may.
func power_cooldown(caster: int, spell: int) -> float:
	return _papyrus.power_cooldown(caster, spell)


## Whether `caster` carries at least one `item`.
func carried(caster: int, item: int) -> bool:
	return _papyrus.get_item_count(caster, item) > 0


## Set up the caster's magic (its spells and active effects) before a cast asks about it.
func prepare(caster: int) -> void:
	_papyrus.prepare_actor_magic(caster)


## SkydotPapyrus.pay_spell: {ok, reason, cost}. `seconds` 0 pays one cast.
func pay_spell(caster: int, spell: int, seconds: float) -> Dictionary:
	return _papyrus.pay_spell(caster, spell, seconds)


func break_invisibility(caster: int) -> void:
	_papyrus.break_invisibility(caster)


## SkydotPapyrus.shout_info: {found, usable, ...}.
func shout_info(caster: int, shout: int) -> Dictionary:
	return _papyrus.shout_info(caster, shout)


## Seconds until `caster`'s voice recovers; 0 if it has.
func voice_recovery(caster: int) -> float:
	return _papyrus.voice_recovery(caster)


func gmst_float(name: String, fallback: float) -> float:
	return _world.get_gmst_float(name, fallback)
