# SPDX-License-Identifier: GPL-3.0-or-later
#
# The player's death on the real SE pack (SKYDOT_SSE_PACK), headless (GAMEPLAY-PLAN.md, 6k):
# after a save, the campfire bandit 0x39FE4 kills the player (Health 0): `player_died` comes
# once with the killer, his attacks and actions stop, the AI ends its fight against him; the
# death view fades to black in 2 s, asks, and Enter reaches `load_chosen`; the reload brings
# Health back above 0 with the player alive again; with no save the start place starts
# again. Skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BANDIT := 0x39FE4
const SAVE := "user://player_death_test.skydot"
const STEP := 1.0 / 60.0

var failures := 0
var _session: GameSession
var _camera: Camera3D
var _feet := Vector3.ZERO
var _died: Array[int] = []
var _swings: Array[int] = []


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false
	var loaded_position := Vector3.ZERO
	var loaded_yaw := 0.0
	var loaded_pitch := 0.0

	func place_at(_at: Vector3, new_yaw: float, new_pitch: float) -> void:
		yaw = new_yaw
		pitch = new_pitch

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return var_to_bytes({"position": Vector3.ZERO, "yaw": 0.0, "pitch": 0.0})

	func load_section(bytes: PackedByteArray) -> bool:
		var state = bytes_to_var(bytes)
		loaded_position = state["position"]
		return true


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_player_death: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _frames(count: int) -> void:
	for i in count:
		_camera.global_position = _feet + Vector3.UP * 1.6
		_session.step(STEP, _feet)
		await physics_frame


func _until(done: Callable, limit: float) -> float:
	var time := 0.0
	while not done.call() and time < limit:
		await _frames(1)
		time += STEP
	return time if done.call() else -1.0


func _run(pack_dir: String) -> void:
	await process_frame
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE))
	var config := GameConfig.new()
	config.pack = pack_dir
	config.quests = false
	config.preload_doors = false
	config.wander = false
	_camera = Camera3D.new()
	root.add_child(_camera)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, _camera, FakePlayer.new()), "the session opens")
	if _session.world == null or _session.ai == null:
		quit(1)
		return
	var papyrus := _session.papyrus
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("BleakFallsBarrow01", SkydotWorld.skyrim_position(Vector3(5006.8, -893.3, -636)), null),
		"the interior is entered")
	_session.player_died.connect(func(killer: int) -> void: _died.append(killer))
	_session.attack_started.connect(func(_clip: String, _power: bool) -> void: _swings.append(1))
	var node := _session.world.find_ref_node(BANDIT) as Node3D
	expect(node != null, "the bandit is built")
	if node == null:
		quit(1)
		return
	_session.ai.attach_built(node.get_parent())
	await _frames(30)
	var forward := -node.global_transform.basis.z
	forward.y = 0.0
	_feet = node.global_position + forward.normalized() * 2.0
	await _frames(2)
	expect(not _session.is_player_dead() and papyrus.get_actor_value(PLAYER, "Health") > 0.0, "the player starts alive")
	expect(_session.saves.save(SAVE), "the game is saved")
	var health_before := papyrus.get_actor_value(PLAYER, "Health")

	# The bandit fights him until he is dead (apply_hit by the AI's attacks).
	var waited := await _until(func() -> bool: return _session.is_player_dead(), 120.0)
	print("the player died after ", waited, " s")
	expect(waited >= 0.0, "the bandit kills the player")
	expect(_died.size() == 1 and _died[0] in [BANDIT, 0x39FE2], "player_died came once with a bandit as the killer: %s" % [_died])
	expect(papyrus.get_actor_value(PLAYER, "Health") <= 0.0, "Health is 0")
	# No more actions: his weapon stays sheathed, his attacks do nothing, and the AI lets go of him.
	var swings := _swings.size()
	_session.toggle_weapon()
	_session.attack_pressed()
	expect(not _session.attack(false), "the dead player cannot attack")
	await _frames(30)
	expect(_swings.size() == swings and _session.weapon_state() == Combat.State.SHEATHED, "no swing, no drawn weapon")
	expect(_session.ai.get_combat_state(BANDIT) == 0, "the bandit's combat against the player ends")

	# The death view: black in 2 s, then the question; Enter chooses to load.
	var view := DeathView.new(_session)
	root.add_child(view)
	var chosen: Array[bool] = []
	view.load_chosen.connect(func() -> void: chosen.append(true))
	view.begin()
	view.step(1.0)
	expect(absf(view.fade - 0.5) < 0.01 and not view.is_asking(), "half black after 1 s")
	view.step(1.0)
	expect(view.fade == 1.0 and view.is_asking(), "black after 2 s, asking")
	var enter := InputEventAction.new()
	enter.action = "ui_accept"
	enter.pressed = true
	view._unhandled_input(enter)
	expect(chosen == [true], "Enter chooses to load")
	view.end()

	# Load the save made first: the player lives.
	expect(_session.saves.newest() != "", "there is a save")
	expect(_session.reload_or_restart(), "the last save loads")
	await _frames(5)
	expect(not _session.is_player_dead() and papyrus.get_actor_value(PLAYER, "Health") >= health_before - 0.01,
		"the player lives with Health %s" % papyrus.get_actor_value(PLAYER, "Health"))

	# With no save, the start place starts again.
	papyrus.damage_actor_value(PLAYER, "Health", 100000.0)
	await _frames(2)
	expect(_session.is_player_dead(), "dead again")
	for file in DirAccess.get_files_at("user://"):
		if file.ends_with(".skydot"):
			DirAccess.remove_absolute(ProjectSettings.globalize_path("user://" + file))
	expect(_session.saves.newest() == "", "no save is left")
	expect(_session.reload_or_restart(), "the start place starts again")
	await _frames(5)
	expect(not _session.is_player_dead() and papyrus.get_actor_value(PLAYER, "Health") > 0.0, "alive after the restart")
	print("smoke_player_death: failures=%d" % failures)
	quit(failures)
