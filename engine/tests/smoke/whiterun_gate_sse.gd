# SPDX-License-Identifier: GPL-3.0-or-later
#
# ForceGreet on the real SE pack (SKYDOT_SSE_PACK): the Whiterun gate guard 0xDC9E4 (non-persistent, placed in
# WhiterunWorld) runs WhiterunGateGuardForcegreet 0xD198B while the player is within 2000 of its wait marker
# 0xD1986 outside the gate. He walks out to the player and opens his topic 0xD197A; the Persuade answer sets
# stage 10 of 0xD1984, which unlocks the gate 0x1B1F1. Headless; skips without the pack.
extends SceneTree

const GUARD := 0xDC9E4
const GATE := 0x1B1F1
const QUEST := 0xD1984
const PERSUADE := 0xD1978
const MARKER := Vector3(19138.0, -7147.2, -3584.0)
const GATE_AT := Vector3(19312.0, -7424.0, -3520.0)
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _viewpoint: Camera3D
var _topics: Array = []


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_whiterun_gate_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func _steps(count: int, feet: Vector3) -> void:
	_viewpoint.position = SkydotWorld.skyrim_position(feet) + Vector3(0, GameSession.EYE_HEIGHT, 0)
	for i in count:
		_session.step(STEP, SkydotWorld.skyrim_position(feet))
		await process_frame


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.preload_doors = false
	config.grass = false
	_viewpoint = Camera3D.new()
	root.add_child(_viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	root.add_child(_session)
	expect(_session.open(config, _viewpoint, PlayerBody.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	_session.dialogue_topics.connect(func(list: Array) -> void: _topics = list)
	expect(_session.papyrus.get_lock_level(GATE) >= 0, "the gate 0x%X is locked" % GATE)
	var outside := MARKER + (MARKER - GATE_AT).normalized() * 800.0
	expect(_session.enter_world("Tamriel", SkydotWorld.skyrim_position(outside) + Vector3(0, GameSession.EYE_HEIGHT, 0), null),
		"Tamriel entered outside the gate")
	var frames := 0
	while not _session.is_dialogue_open() and frames < 60 * 90:
		await _steps(1, outside)
		frames += 1
	expect(_session.is_dialogue_open(), "the guard greets the player (%.0f s; %s, %.1f m away)" % [frames / 60.0,
		_session.ai.get_actor_state(GUARD).get("procedure", "?"), _session.distance_to_player(GUARD)])
	expect(_session.get("_talk_speaker") == GUARD, "the speaker is the guard 0x%X" % GUARD)
	var waited := 0
	while _topics.is_empty() and waited < 60 * 30:
		await _steps(1, outside)
		_session.dialogue_skip()
		waited += 1
	var index := -1
	for i in _topics.size():
		if int(_topics[i]["id"]) == PERSUADE:
			index = i
	expect(index >= 0, "the Persuade topic 0x%X is offered (%s)" % [PERSUADE, _topics.map(func(t): return t["id"])])
	if index >= 0:
		_session.dialogue_choose(index)
		waited = 0
		while _session.papyrus.get_lock_level(GATE) >= 0 and waited < 60 * 30:
			await _steps(1, outside)
			_session.dialogue_skip()
			waited += 1
		var state: Dictionary = _session.papyrus.get_quest_state(QUEST)
		expect(10 in state.get("stages_done", []), "stage 10 of 0x%X is done (%s)" % [QUEST, state])
		expect(_session.papyrus.get_lock_level(GATE) < 0, "the gate 0x%X is unlocked" % GATE)
	print("smoke_whiterun_gate_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)
