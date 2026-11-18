# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Sleep/Wait rules on the real SE pack (SKYDOT_SSE_PACK), against what the game does (SkyrimRemote captures,
# skycompare-data/ui-sleepwait, 2026-11-15): in the Sleeping Giant Inn the rentable bed 0x539DB belongs to the inn
# ("You cannot sleep in an owned bed." on activation); the player, damaged by 60 Health, waits an hour: the clock moves
# an hour, Hours Waiting counts 1 and Health recovers as in the game (HealRate 0.01: +3.6 an hour at time scale 20). Headless; skips without the pack.
extends SceneTree

const PLAYER := 0x14
const BED := 0x539DB

var failures := 0
var _session: GameSession


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_sleep_wait_sse: SKIP (SKYDOT_SSE_PACK unset)")
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
	config.time = 8.0
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	root.add_child(_session)
	expect(_session.open(config, viewpoint, PlayerBody.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	expect(_session.start_game(), "the game starts")
	expect(_session.enter_cell("RiverwoodSleepingGiantInn", SkydotWorld.skyrim_position(Vector3(-1092, -708.7, 0)), null),
		"the inn is entered")
	for i in 3:
		await process_frame

	var owner := _session.papyrus.get_owner(BED)
	print("bed owner: ", owner)
	expect(int(owner["owner"]) != 0 and not owner["player"], "the inn's bed is not the player's: %s" % [owner])
	expect(_session.rest.refusal(true, BED) == "You cannot sleep in an owned bed.", "the game's message for it")

	var view := SleepWaitView.new(_session)
	root.add_child(view)
	# The game's measurement (sw4): HealRate 0.01, 90 damage; an hour's wait at time scale 20 gives +3.6 Health.
	_session.papyrus.set_actor_value(PLAYER, "HealRate", 0.01)
	_session.papyrus.damage_actor_value(PLAYER, "Health", 90.0)
	var before := _session.actor_value(PLAYER, "Health")
	var hour := _session.clock.get_hour()
	view.open(false)
	view.accept()
	for i in 5:
		if view.is_open():
			view._process(SleepWaitView.HOUR_SECONDS)
	var after := _session.actor_value(PLAYER, "Health")
	print("an hour's wait: Health %.2f -> %.2f (the game: 10.10 -> 13.71), hour %.2f -> %.2f" % [before, after, hour,
		_session.clock.get_hour()])
	expect(not view.is_open() and absf(fposmod(_session.clock.get_hour() - hour, 24.0) - 1.0) < 0.01, "an hour passed")
	expect(_session.journal.value("Hours Waiting") == 1, "Hours Waiting counts it")
	expect(absf(after - before - 3.6) < 0.1, "Health recovered as in the game (+3.6): %+.2f" % (after - before))
	print("smoke_sleep_wait_sse: %s" % ("OK" if failures == 0 else "%d failures" % failures))
	quit(0 if failures == 0 else 1)
