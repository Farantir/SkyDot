# SPDX-License-Identifier: GPL-3.0-or-later
#
# GameSession's player gates (ARCHITECTURE.md 8.2): which player actions a paralysed player cannot
# do. The session is never opened; Combat and Magic are GDScript stubs that log the calls reaching
# them, the papyrus is a never-set-up native (alive, no shout equipped).
extends "gd_test.gd"
const Fakes := preload("fakes.gd")


class FakeData extends PlayerStateData:
	var dead := false
	var broken := 0

	func is_dead() -> bool:
		return dead

	func break_invisibility() -> void:
		broken += 1


class StubMagic extends Magic:
	var calls: Array[String] = []
	var left_spell := 1
	var right_spell := 1

	func _init() -> void:
		super(null, null, null, null, null, null, Callable(), null, null)

	func spell_in(hand: int) -> int:
		return left_spell if hand == Magic.LEFT else right_spell

	func hands_with_spells() -> Array[int]:
		return [Magic.LEFT, Magic.RIGHT]

	func hand_pressed(hand: int) -> void:
		calls.append("hand_pressed %d" % hand)

	func toggle_ready(_silent := false) -> void:
		calls.append("toggle_ready")

	func set_ready(on: bool, _silent := false) -> void:
		calls.append("set_ready %s" % on)

	func cast_power() -> Dictionary:
		calls.append("cast_power")
		return {"ok": true}

	func shout_begin(shout: int) -> Dictionary:
		calls.append("shout_begin %d" % shout)
		return {"ok": true}


class StubCombat extends Combat:
	var calls: Array[String] = []

	func _init(host: Node3D) -> void:
		super(Fakes.world(), SkydotPack.new(), Fakes.papyrus(), host, Sounds.new(Fakes.world(), null, Callable(), Callable()),
				Callable(), Callable(), ActorRegistry.new(host))

	func toggle_weapon() -> void:
		calls.append("toggle_weapon")

	func attack_pressed() -> void:
		calls.append("attack_pressed")

	func attack(power := false, _left := false) -> bool:
		calls.append("attack %s" % power)
		return true


var _session: GameSession
var _magic: StubMagic
var _combat: StubCombat
var _host: Node3D
var _data: FakeData


func before_each() -> void:
	_host = Node3D.new()
	add_child(_host)
	_session = GameSession.new()
	add_child(_session)
	_session.papyrus = Fakes.papyrus()
	_data = FakeData.new(_session.papyrus)
	_session.player_state = PlayerState.new(_data)
	_magic = StubMagic.new()
	_combat = StubCombat.new(_host)
	_session.magic = _magic
	_session.combat = _combat


func test_free_player_reaches_magic_and_combat() -> void:
	_session.toggle_weapon()
	_session.attack_left_pressed()
	_session.attack_pressed()
	expect(_magic.calls.has("toggle_ready"), "the hands are readied")
	expect(_magic.calls.has("hand_pressed %d" % Magic.LEFT), "left hand cast")
	expect(_magic.calls.has("hand_pressed %d" % Magic.RIGHT), "right hand cast")


func test_paralysed_toggle_weapon_does_nothing() -> void:
	_session.player_state.set_paralyzed(true)
	_session.toggle_weapon()
	expect_eq(_combat.calls, [] as Array[String], "combat calls")
	expect_eq(_magic.calls, [] as Array[String], "magic calls")


func test_paralysed_attack_pressed_does_nothing() -> void:
	_session.player_state.set_paralyzed(true)
	_session.attack_pressed()
	expect_eq(_combat.calls, [] as Array[String], "combat calls")
	expect_eq(_magic.calls, [] as Array[String], "magic calls")


func test_paralysed_attack_left_pressed_does_nothing() -> void:
	_session.player_state.set_paralyzed(true)
	_session.attack_left_pressed()
	expect_eq(_magic.calls, [] as Array[String], "magic calls")


func test_activate_ref_refused_while_paralysed() -> void:
	_session.player_state.set_paralyzed(true)
	expect_eq(_session.activate_ref(0x1234), false, "activate_ref")


func test_paralysed_power_and_voice_power_are_refused() -> void:
	_session.player_state.set_paralyzed(true)
	expect_eq(_session.cast_power(), {"ok": false, "reason": "paralyzed"}, "cast_power")
	expect_eq(_session.voice_pressed(), {"ok": false, "reason": "paralyzed"}, "voice power")
	expect_eq(_magic.calls, [] as Array[String], "magic calls")


func test_paralysed_attack_is_refused() -> void:
	_session.player_state.set_paralyzed(true)
	expect_eq(_session.attack(), false, "attack")
	expect_eq(_combat.calls, [] as Array[String], "combat calls")


func test_dead_player_is_refused() -> void:
	_data.dead = true
	_session.toggle_weapon()
	_session.attack_pressed()
	_session.attack_left_pressed()
	expect_eq(_session.attack(), false, "attack")
	expect_eq(_session.cast_power(), {"ok": false, "reason": "dead"}, "cast_power")
	expect_eq(_session.voice_pressed(), {"ok": false, "reason": "dead"}, "voice")
	expect_eq(_session.activate_ref(0x1234), false, "activate_ref")
	expect_eq(_session.is_player_dead(), true, "is_player_dead")
	expect_eq(_combat.calls, [] as Array[String], "combat calls")
	expect_eq(_magic.calls, [] as Array[String], "magic calls")
	expect_eq(_data.broken, 0, "invisibility kept")


func test_acting_breaks_invisibility() -> void:
	_session.attack_pressed()
	expect_eq(_data.broken, 1, "attack_pressed")
	_session.attack_left_pressed()
	expect_eq(_data.broken, 2, "attack_left_pressed")
	_session.cast_spell(0x100)
	expect_eq(_data.broken, 3, "cast_spell")
