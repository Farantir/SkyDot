# SPDX-License-Identifier: GPL-3.0-or-later
#
# Dialogue on the real SE pack (SKYDOT_SSE_PACK): the greeting and the topic
# list Lucan Valerius, Gerdur and Alvor give in Riverwood, as the engine works
# them out (dialogue_topics_debug, no side effects), with the start-game quests
# running, before and after MQ102 is set to a stage. Headless; skips without
# the pack.
extends SceneTree

var failures := 0
var _session: GameSession


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
		print("smoke_dialogue: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _describe(npc_name: String, ref: int) -> Dictionary:
	var result := _session.papyrus.dialogue_topics_debug(ref)
	var lines: Array[String] = []
	for line: Dictionary in result["lines"]:
		lines.append(line["text"])
	var topics: Array[String] = []
	for topic: Dictionary in result["topics"]:
		topics.append(topic["text"])
	print("%s 0x%X greeting=%s topics=%s" % [npc_name, ref, lines, topics])
	return {"lines": lines, "topics": topics}


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
	var quest := _session.world.find_quest("MQ102")
	expect(quest != 0, "MQ102 exists")
	var stages: Array[int] = []
	for stage: Dictionary in _session.world.get_quest(quest)["stages"]:
		stages.append(stage["index"])
	print("MQ102 stages: ", stages)
	var npcs := {}
	for name in ["LucanValerius", "Gerdur", "Alvor"]:
		var npc := _session.world.find_npc(name)
		var ref := _session.world.find_actor_of(npc)
		expect(ref != 0, name + " is placed")
		npcs[name] = ref
	var generic := ["What can you tell me about Riverwood?"]

	# No extra stage. The lists are what the engine produces; they still need
	# comparing with the original game (README of the step).
	var lucan := _describe("LucanValerius", npcs["LucanValerius"])
	expect(lucan["lines"] == ["I don't know what you overheard, but the Riverwood Trader is still open. Feel free to shop."],
		"Lucan's greeting before MS13")
	expect(lucan["topics"] == ["Did something happen?", "Do you sell spells?", "What have you got for sale?"],
		"Lucan's topics before MS13 (the merchant's barter topic hangs on IsCommandedActor)")
	var gerdur := _describe("Gerdur", npcs["Gerdur"])
	expect(gerdur["lines"] == ["New to Riverwood? If you're looking for work, go see Hod at the mill."], "Gerdur's greeting")
	expect(gerdur["topics"] == generic + ["Is there somewhere I can buy fresh supplies?"], "Gerdur's topics")
	var alvor := _describe("Alvor", npcs["Alvor"])
	expect(alvor["lines"] == ["Ain't every day we get visitors in Riverwood."], "Alvor's greeting")
	# Alvor sells but does not train: smithing training is Balimund's (JobTrainerSmithingFaction).
	expect(alvor["topics"] == generic + ["Can I use your forge?", "What have you got for sale?"], "Alvor's topics")

	# MQ102 (Before the Storm) at stage 10 changes nothing for the three.
	expect(_session.papyrus.set_stage(quest, 10), "MQ102 stage 10 is set")
	expect(_describe("Gerdur", npcs["Gerdur"])["topics"] == gerdur["topics"], "MQ102 leaves Gerdur's topics")
	expect(_describe("Alvor", npcs["Alvor"])["topics"] == alvor["topics"], "MQ102 leaves Alvor's topics")
	expect(_describe("LucanValerius", npcs["LucanValerius"])["topics"] == lucan["topics"], "MQ102 leaves Lucan's topics")

	# MS13 (the Golden Claw) stage 10: Lucan greets about the thieves and no
	# longer asks about the break-in.
	var ms13 := _session.world.find_quest("MS13")
	expect(_session.papyrus.set_stage(ms13, 10), "MS13 stage 10 is set")
	lucan = _describe("LucanValerius", npcs["LucanValerius"])
	expect(lucan["lines"] == ["Show those thieves not to steal from Lucan Valerius!"], "Lucan's greeting during MS13")
	expect(lucan["topics"] == ["Do you sell spells?", "What have you got for sale?"], "Lucan's topics during MS13")
	expect(_describe("Gerdur", npcs["Gerdur"])["topics"] == gerdur["topics"], "Gerdur is unaffected")

	# The side effects of really talking: Say Once, said topics, fragments.
	var opened := _session.papyrus.dialogue_open(npcs["LucanValerius"])
	expect(opened["lines"].size() >= 1 and opened["topics"].size() == 2, "dialogue_open speaks and lists")
	expect(_session.papyrus.dialogue_is_open(), "the dialogue is open")
	var spells: Dictionary = _session.papyrus.dialogue_choose(opened["topics"][0]["id"])
	expect(spells["lines"].size() >= 1, "a topic is answered: %s" % [spells["lines"]])
	_session.papyrus.dialogue_close()
	expect(not _session.papyrus.dialogue_is_open(), "the dialogue closes")
	# An INFO's fragment script attaches to the INFO and its function runs
	# (Skyrim.esm's INFO 0x1362F has an end fragment; one without has none).
	expect(_session.papyrus.dialogue_fragment(0x1362F, true, npcs["LucanValerius"]), "an end fragment runs")
	expect(not _session.papyrus.dialogue_fragment(0x1362F, false, npcs["LucanValerius"]), "and no begin fragment")
	_session.papyrus.update(0.5)
	print("smoke_dialogue: failures=%d" % failures)
	quit(failures)
