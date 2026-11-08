# SPDX-License-Identifier: GPL-3.0-or-later
#
# Furniture use on a real pack, without the game around it: for an actor and a furniture reference,
# the idle the IdleChooser picks for ActionActivate (0x13009) under stated facts, and the clips the
# actor's behaviour graph plays for the chosen events (ActorGraph), with the root motion of the
# playing clip. Drives nothing.
#
#   godot4.7 --headless --path game --script res://tools/furniture_check.gd -- --pack <pack> \
#       [--case spider|draugr|sigrid|all]
extends SceneTree

const ACTIVATE := 0x13009
var _world: SkydotWorld
var _pack: SkydotPack
var _fur := SkydotFurniture.new()


func _initialize() -> void:
	var argv := OS.get_cmdline_user_args()
	var args := {}
	for i in range(0, argv.size() - 1):
		args[argv[i]] = argv[i + 1]
	_pack = SkydotPack.new()
	if _pack.open(args.get("--pack", "")) != OK:
		printerr("furniture_check: ", _pack.get_error())
		quit(1)
		return
	_world = _pack.open_world()
	_fur.setup(_world)
	var which: String = args.get("--case", "all")
	if which in ["spider", "all"]:
		_spider()
	if which in ["draugr", "all"]:
		_draugr()
	if which in ["sigrid", "all"]:
		_sigrid()
	quit(0)


func _info(ref: int) -> Dictionary:
	return _world.get_ref_info(_world.get_ref_cell(ref), ref)


func _behaviour(actor: int) -> String:
	return _world.get_actor_plan(actor).get("behaviour", "")


# The animation type of the furniture's first marker (1 sit, 2 sleep, 3 lean), 0 if it has none.
func _anim_type(furniture: int) -> int:
	var base: Dictionary = _world.get_base(_info(furniture)["base"])
	var model := _world.build_model(base.get("model", ""))
	if model == null:
		return 0
	var markers := _fur.get_markers(model)
	model.free()
	print("   markers of %s: %s" % [base.get("model", ""), markers])
	return markers[0]["animation_type"] if markers.size() > 0 else 0


func _choose(actor: int, furniture: int, facts: Dictionary) -> Dictionary:
	var choice := _fur.choose_idle(ACTIVATE, _fur.get_graph_behaviour(), actor, furniture, facts)
	print("   facts %s -> %s %s %s%s" % [facts, choice.get("editor_id", "-"), choice.get("event", "(none)"), "0x%X" % choice.get("idle", 0),
		"  unknown functions %s" % [choice["unknown"]] if choice.get("unknown", []).size() > 0 else ""])
	return choice


func _show(label: String, out: Dictionary) -> void:
	print("   %s: events %s changes %s playing %s" % [label, out.get("events"), out.get("changes"), out.get("playing")])


# Send `event` (entering: the graph may route it into the furniture states), then advance in `step`
# seconds until nothing plays / `seconds` pass.
func _play(event: String, seconds: float, step: float = 0.25, entering: bool = false) -> void:
	_show("send " + event, _fur.graph_enter(event) if entering else _fur.graph_send(event))
	var t := 0.0
	var last: Dictionary = _fur.graph_playing()
	while t < seconds:
		var out := _fur.graph_advance(step)
		t += step
		var now: Dictionary = out["playing"]
		if not out["events"].is_empty() or not out["changes"].is_empty() or now.get("clip", "") != last.get("clip", ""):
			print("   t=%.2f events %s changes %s playing %s" % [t, out["events"], out["changes"], now.get("clip", "(none)") if now.is_empty() else "%s %.2f/mode %d" % [now["clip"], now["time"], now["mode"]]])
		last = now
		if now.is_empty() and t > 0.5:
			break


func _open(actor: int) -> bool:
	var behaviour := _behaviour(actor)
	print("== actor 0x%X behaviour %s" % [actor, behaviour])
	if not _fur.open_graph(behaviour):
		print("   no graph: ", _fur.get_missing())
		return false
	print("   root graph ", _fur.get_graph_behaviour())
	return true


func _spider() -> void:
	var actor := 0x3A1E1
	var furniture := 0x46BEF
	if not _open(actor):
		return
	var type := _anim_type(furniture)
	print("-- enter (placed in it on load)")
	_show("start", _fur.graph_start())
	var enter := _choose(actor, furniture, {"quick": 1, "anim_type": type, "sleeping": 3})
	_play(enter.get("event", ""), 2.0, 0.25, true)
	print("-- exit (waking, GetSleeping 4)")
	var exit := _choose(actor, furniture, {"quick": 0, "anim_type": type, "sleeping": 4})
	_play(exit.get("event", ""), 12.0)
	for t in [0.0, 1.15, 2.3]:
		print("   root motion PodExit.HKX at %.2f: %s" % [t, _fur.root_motion("PodExit.HKX", t)])


# The placed reference of base `base` nearest to the actor.
func _ref_of_base(actor: int, base: int) -> int:
	var cell := _world.get_ref_cell(actor)
	var at: Vector3 = _info(actor)["position"]
	var best := 0
	var best_d := 1.0e20
	for entry in _world.get_refs(cell):
		if entry["base"] != base:
			continue
		var d := (_info(entry["id"])["position"] as Vector3).distance_to(at)
		if d < best_d:
			best_d = d
			best = entry["id"]
	print("   ref of base 0x%X nearest 0x%X: 0x%X at %.1f" % [base, actor, best, best_d])
	return best


func _draugr() -> void:
	var actor := 0x380B5
	var furniture := _ref_of_base(actor, 0x46B81)
	if not _open(actor):
		return
	var type := _anim_type(furniture)
	_fur.graph_start()
	print("-- enter instantly")
	var enter := _choose(actor, furniture, {"quick": 1, "anim_type": type, "sleeping": 3})
	_play(enter.get("event", ""), 3.0, 0.25, true)
	print("-- exit")
	var exit := _choose(actor, furniture, {"quick": 0, "anim_type": type, "sleeping": 4})
	_play(exit.get("event", ""), 12.0)


func _nearest_furniture(cell: int, pos: Vector3) -> int:
	var best := 0
	var best_d := 1.0e20
	for entry in _world.get_refs(cell):
		var info := _world.get_ref_info(cell, entry["id"])
		if info.get("type", "") != "FURN":
			continue
		var d := (info["position"] as Vector3).distance_to(pos)
		if d < best_d:
			best_d = d
			best = entry["id"]
	print("   nearest FURN to %s: 0x%X %s at %.1f" % [pos, best, _info(best).get("editor_id", ""), best_d])
	return best


func _sigrid() -> void:
	var actor := 0x13483
	if not _open(actor):
		return
	var cell := _world.find_cell("RiverwoodAlvorsHouse")
	for item in [["chair", Vector3(-697, -210, 242)], ["bed", Vector3(-221, -184, 240)]]:
		var furniture := _nearest_furniture(cell, item[1])
		var type := _anim_type(furniture)
		var sit := type == 1
		print("-- %s 0x%X" % [item[0], furniture])
		# The entry is the marker's entry bit the actor uses (1 front, 2 behind, 4 right, 8 left, 0x10 up).
		for entry in [1, 4, 8]:
			var down := {"quick": 0, "anim_type": type, "sitting": 2 if sit else 0, "sleeping": 0 if sit else 2, "entry": entry}
			var walk := _choose(actor, furniture, down)
			var instant := _choose(actor, furniture, {"quick": 1, "anim_type": type, "sitting": 0, "sleeping": 0, "entry": entry})
			var up := {"quick": 0, "anim_type": type, "sitting": 4 if sit else 0, "sleeping": 0 if sit else 4, "entry": entry}
			var leave := _choose(actor, furniture, up)
			if entry != 4 and entry != 8:
				continue
			for event in [walk.get("event", ""), instant.get("event", "")]:
				if event == "":
					continue
				_open(actor)
				_fur.graph_start()
				print("   -- enter ", event)
				_play(event, 4.0, 0.25, true)
				if leave.has("event"):
					print("   in furniture: ", _fur.graph_in_furniture())
					_play(leave["event"], 8.0)
					print("   in furniture after the exit: ", _fur.graph_in_furniture())
