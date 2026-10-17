# SPDX-License-Identifier: GPL-3.0-or-later
#
# Combat data of world.fb 31 on the real SE pack (SKYDOT_SSE_PACK): the
# bandit 0x373A6's NPC_ record (AI values, combat style, crime faction), its
# combat style's multipliers, the factions it belongs to with their XNAM
# relations (BanditFaction 0x1BCC0, PlayerFaction 0xDB1) and the full FACT
# DATA flags. Headless; skips without the pack.
extends SceneTree

const BANDIT := 0x373A6
const PLAYER_FACTION := 0xDB1

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
		print("smoke_combat_data: SKIP (SKYDOT_SSE_PACK unset)")
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
	config.quests = false
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
	var world := _session.world
	var base: int = world.get_actor(BANDIT)["base"]
	var npc := world.get_npc(base)
	print("bandit npc: ", npc)
	expect(not npc.is_empty() and npc["aggression"] >= 1, "the bandit is aggressive: %s" % [npc.get("aggression")])
	expect(npc["combat_style"] != 0, "the bandit has a combat style")
	expect(npc["crime_faction"] == 0, "the bandit has no crime faction")
	var style := world.get_combat_style(npc["combat_style"])
	print("combat style: ", style)
	expect(not style.is_empty() and style["editor_id"] != "", "the combat style is found")
	expect(style["offensive_mult"] > 0.0 and style["defensive_mult"] > 0.0, "its offensive and defensive multipliers are set")
	expect(style["melee_score_mult"] > 0.0, "its melee score multiplier is set")
	expect(style["circle_mult"] >= 0.0 and style.has("hover_chance") and style.has("strafe_mult"), "its other groups are there")
	expect(world.get_combat_style(0x7FFFFFF).is_empty(), "an unknown style is empty")

	var bandit_faction := {}
	for rank in npc["factions"]:
		var fact := world.get_faction(rank["faction"])
		print("faction ", fact.get("editor_id", "?"), " flags ", fact.get("flags", 0), " relations ", fact.get("relations", []).size())
		if str(fact.get("editor_id", "")).to_lower() == "banditfaction":
			bandit_faction = fact
	expect(not bandit_faction.is_empty(), "the bandit is in BanditFaction")
	# XNAM: BanditFaction has no entry for PlayerFaction (the player is hostile to bandits by
	# their aggression); it is an ally of itself and an enemy of the White River Watch wolves.
	var to_self := {}
	var to_wolves := {}
	for relation in bandit_faction.get("relations", []):
		if relation["faction"] == bandit_faction["id"]:
			to_self = relation
		elif relation["faction"] == 0x86DDB:
			to_wolves = relation
	expect(bandit_faction["relations"].size() == 15, "BanditFaction has 15 relations")
	expect(to_self.get("reaction", -1) == 2, "BanditFaction is an ally of itself")
	expect(to_wolves.get("reaction", -1) == 1, "BanditFaction is an enemy of the wolves of White River Watch")
	# DATA keeps every bit: 0x8001 (bit 15 and the first flag).
	expect(bandit_faction["flags"] == 0x8001, "BanditFaction's flags are 0x8001: 0x%X" % bandit_faction["flags"])
	expect(world.get_faction(0x7FFFFFF).is_empty(), "an unknown faction is empty")
	var player_faction := world.get_faction(PLAYER_FACTION)
	expect(player_faction.get("editor_id", "") == "PlayerFaction" and player_faction["flags"] == 0x8000, "PlayerFaction is found with flags 0x8000")
	expect(player_faction["relations"].size() > 50, "PlayerFaction has many relations")
	print("smoke_combat_data: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)
