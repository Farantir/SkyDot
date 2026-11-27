# SPDX-License-Identifier: GPL-3.0-or-later
#
# The music the player hears (Music, game/game/music.gd) on the real SE pack (SKYDOT_SSE_PACK), with streams on:
#   Breezehome chooses MUSTownTest (its XCMO) and a track's stream plays;
#   MusicType.Add of MUSReward (priority 2) wins over it, ducks it, and after its one track leaves the stack;
#   Tamriel at Riverwood chooses MUSExploreForestPine (its region);
#   in Bleak Falls Barrow a bandit fights the player: MUSCombat over MUSDungeon; once the foes are dead the
#   dungeon music comes back and the combat track ends with its finale.
# Headless (Godot's dummy audio driver plays the streams in real time); skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const RIVERWOOD_MARKER := 0x162A4
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _chosen: Array = []  # [editor id, source]
var _tracks: Array = []  # [file, finale]


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_music_play_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func _frames(count: int) -> void:
	for i in count:
		_camera.global_position = _feet + Vector3.UP * 1.6
		_session.papyrus.restore_actor_value(PLAYER, "Health", 100.0)
		_session.step(STEP, _feet)
		await physics_frame


## Steps until `done` or `seconds` of wall time (the streams play in real time).
func _until(done: Callable, seconds: float) -> bool:
	var end := Time.get_ticks_msec() + int(seconds * 1000.0)
	while not done.call() and Time.get_ticks_msec() < end:
		await _frames(1)
	return done.call()


func _last() -> String:
	return _chosen[-1][0] if not _chosen.is_empty() else ""


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = false
	config.preload_doors = false
	config.wander = false
	config.grass = false
	config.music = true
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	root.add_child(_session)
	expect(_session.open(config, _camera, PlayerBody.new()), "the session opens")
	if _session.world == null or _session.music == null:
		quit(1)
		return
	var world := _session.world
	var music := _session.music
	music.chosen.connect(func(type: int, source: String) -> void:
		_chosen.append([world.get_music_type(type).get("editor_id", ""), source]))
	music.track_started.connect(func(_track: int, file: String, finale: bool) -> void: _tracks.append([file, finale]))
	expect(_session.start_game(), "the game starts")

	# Breezehome: its XCMO, and a stream plays.
	expect(_session.enter_cell("WhiterunBreezehome", null, null), "Breezehome is entered")
	_feet = _camera.global_position
	await _until(func() -> bool: return _last() == "MUSTownTest" and not _tracks.is_empty(), 20.0)
	expect(_chosen.has(["MUSTownTest", "cell"]), "Breezehome chooses MUSTownTest (cell): %s" % [_chosen])
	expect(not _tracks.is_empty() and _tracks[0][0].begins_with("music/"), "a track starts: %s" % [_tracks])
	var player := music.get_child(0) as AudioStreamPlayer
	var start := player.get_playback_position() if player != null else 0.0
	await _until(func() -> bool: return false, 1.5)
	expect(player != null and player.playing and player.bus == Music.BUS
		and player.get_playback_position() > start + 0.5,
		"its stream plays on the Music bus (%.2f s -> %.2f s)" % [start, player.get_playback_position() if player else 0.0])

	# MusicType.Add from Papyrus: MUSReward (2) over MUSTownTest (48), ducking it; one track, then off the stack.
	var reward := world.find_music_type("MUSReward")
	_session.papyrus.call_method(reward, "MusicType", "Add", [])
	await _until(func() -> bool: return _last() == "MUSReward" and music.layers().size() == 2, 5.0)
	expect(_chosen[-1] == ["MUSReward", "script"], "MusicType.Add wins by priority: %s" % [_chosen])
	await _until(func() -> bool: return music.layers().size() == 2 and music.layers()[0]["volume_db"] < -15.0, 5.0)
	var layers := music.layers()
	expect(layers.size() == 2 and layers[0]["type"] == world.find_music_type("MUSTownTest") and layers[0]["playing"]
		and layers[0]["volume_db"] < -15.0, "the town music goes on under it, ducked 20 dB: %s" % [layers])
	await _until(func() -> bool: return _last() == "MUSTownTest", 90.0)
	expect(_last() == "MUSTownTest" and not _session.papyrus.get_music_stack().has(reward),
		"after one track MUSReward leaves the stack and the town music is back: %s" % [_chosen])

	# Tamriel at Riverwood: the region's music.
	var cell := world.get_ref_cell(RIVERWOOD_MARKER)
	var at: Vector3 = world.get_ref_info(cell, RIVERWOOD_MARKER).get("position", Vector3.ZERO)
	_feet = SkydotWorld.skyrim_position(at)
	expect(_session.enter_world("Tamriel", _feet + Vector3.UP * GameSession.EYE_HEIGHT, null), "Tamriel is entered")
	await _until(func() -> bool: return _last() == "MUSExploreForestPine", 30.0)
	expect(_chosen[-1] == ["MUSExploreForestPine", "region"], "Riverwood chooses MUSExploreForestPine (region): %s"
		% [_chosen.slice(-3)])

	# Combat in Bleak Falls Barrow, once its music plays.
	_session.ai.set_combat(false)
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"Bleak Falls Barrow is entered")
	var node := world.find_ref_node(BANDIT) as SkydotActor
	if node == null:
		expect(false, "the bandit is built")
		quit(1)
		return
	_session.ai.attach_built(node.get_parent())
	await _until(func() -> bool: return _last() == "MUSDungeon" and music.layers().size() == 1, 20.0)
	expect(_chosen[-1] == ["MUSDungeon", "cell"], "Bleak Falls Barrow chooses MUSDungeon (cell)")
	_session.ai.set_combat(true)
	var forward := -node.global_transform.basis.z
	forward.y = 0.0
	_feet = node.global_position + forward.normalized() * 6.0
	await _until(func() -> bool: return _last() == "MUSCombat", 20.0)
	expect(_chosen[-1] == ["MUSCombat", "combat"], "a fight switches to MUSCombat: %s" % [_chosen.slice(-3)])
	await _until(func() -> bool: return music.layers().size() == 2 and music.layers()[0]["volume_db"] < -60.0, 5.0)
	var layers2 := music.layers()
	expect(layers2.size() == 2 and layers2[1]["track"].begins_with("MUSCombat") and layers2[0]["volume_db"] < -60.0,
		"over the dungeon music, ducked out: %s" % [layers2])
	var tracks_before := _tracks.size()
	for actor in node.get_parent().find_children("*", "SkydotActor", true, false):
		var ref: int = actor.get_meta("skydot_ref", 0)
		if ref != 0 and ref != PLAYER and not _session.papyrus.is_actor_dead(ref):
			_session.kill_actor(ref)
	await _until(func() -> bool: return _last() == "MUSDungeon", 10.0)
	expect(_chosen[-1] == ["MUSDungeon", "cell"], "after the fight MUSDungeon is chosen again: %s" % [_chosen.slice(-3)])
	await _until(func() -> bool: return _tracks.slice(tracks_before).any(func(t: Array) -> bool: return t[1]), 40.0)
	var finales := _tracks.slice(tracks_before).filter(func(t: Array) -> bool: return t[1])
	expect(not finales.is_empty() and finales[0][0].contains("_finale"), "the combat track ends with its finale: %s"
		% [_tracks.slice(tracks_before)])
	await _until(func() -> bool: return music.layers().size() == 1, 60.0)
	expect(music.layers().size() == 1 and music.layers()[0]["type"] == world.find_music_type("MUSDungeon"),
		"then only the dungeon music plays: %s" % [music.layers()])
	print("smoke_music_play_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)
