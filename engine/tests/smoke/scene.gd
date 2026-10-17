# SPDX-License-Identifier: GPL-3.0-or-later
#
# A scene on the real SE pack (SKYDOT_SSE_PACK): the quest
# DialogueRiverwoodSceneGerdurHod starts, its scene RiverwoodSceneGerdurHod
# (Gerdur and Hod talking, five dialogue phases) begins, plays its five lines
# in order through the game layer's speaker queues, each with a voice that
# exists, and ends. Time is simulated: the game layer's frames are stepped by
# hand, no actors are built (the scene accepts living placed actors, as
# set_scene_actors_need_3d(false) says). Headless; skips without the pack.
extends SceneTree

const QUEST := 0xD2C61
const SCENE := 0xD2C62
const GERDUR_NPC := 0x1347C
const HOD_NPC := 0x1347D
const TOPICS := [0xD2BA9, 0xD2BA8, 0xD2BA7, 0xD2BA6, 0xD2B9B]

var failures := 0
var _session: GameSession
var _started: Array = []
var _ended := 0
var _subtitles: Array = []


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false

	func place_at(_at: Vector3, new_yaw: float, new_pitch: float) -> void:
		yaw = new_yaw
		pitch = new_pitch

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return PackedByteArray()

	func load_section(_bytes: PackedByteArray) -> bool:
		return true


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_scene: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.preload_doors = false
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, viewpoint, FakePlayer.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	_session.papyrus.set_scene_actors_need_3d(false)
	_session.speech.line_started.connect(func(speaker: int, line: Dictionary, seconds: float) -> void:
		if line.has("scene"):
			_started.append({"speaker": speaker, "line": line, "seconds": seconds}))
	_session.speech.line_ended.connect(func(_speaker: int, line: Dictionary) -> void:
		if line.has("scene"):
			_ended += 1)
	_session.subtitle_line.connect(func(speaker: int, text: String, seconds: float) -> void:
		_subtitles.append([speaker, text, seconds]))

	var gerdur := _session.world.find_actor_of(GERDUR_NPC)
	var hod := _session.world.find_actor_of(HOD_NPC)
	var speakers := [gerdur, hod, gerdur, hod, gerdur]
	expect(gerdur != 0 and hod != 0, "Gerdur and Hod are placed")
	expect(not _session.papyrus.scene_is_playing(SCENE), "the scene is not playing before its quest")
	expect(_session.papyrus.start_quest(QUEST), "the quest starts")
	var time := 0.0
	while time < 180.0 and (_session.papyrus.scene_is_playing(SCENE) or time == 0.0):
		_session.update_scenes(0.1)
		_session.speech.update(0.1)
		time += 0.1
	print("scene finished after %.1f simulated seconds" % time)
	expect(not _session.papyrus.scene_is_playing(SCENE), "the scene has ended")
	expect(_started.size() == 5, "five lines were spoken, got %d" % _started.size())
	for i in mini(_started.size(), 5):
		var entry: Dictionary = _started[i]
		var line: Dictionary = entry["line"]
		var voice: String = line["voice"]
		var stream := _session.sounds.load_stream(voice)
		expect(entry["speaker"] == speakers[i], "line %d is spoken by 0x%X, not 0x%X" % [i + 1, speakers[i], entry["speaker"]])
		expect(line["topic"] == TOPICS[i], "line %d belongs to topic 0x%X, not 0x%X" % [i + 1, TOPICS[i], line["topic"]])
		expect(stream != null and stream.get_length() > 0.5, "line %d's voice %s loads" % [i + 1, voice])
		expect(is_equal_approx(entry["seconds"], stream.get_length() if stream != null else -1.0), "line %d lasts as long as its voice" % [i + 1])
		print("line %d 0x%X topic 0x%X: \"%s\" %s %.2f s" % [i + 1, entry["speaker"], line["topic"], line["text"], voice, entry["seconds"]])
	expect(_ended == _started.size(), "every line ended")
	expect(_subtitles.size() == _started.size(), "every line was subtitled")
	print("smoke_scene: failures=%d" % failures)
	quit(failures)
