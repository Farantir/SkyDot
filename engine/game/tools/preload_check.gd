# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the viewer, stands the player on the nearest load door, waits until
# the place behind it is prepared (or --wait seconds), goes through and
# times the transition: the frame that travels and the frames after it until
# nothing streams. Compare with the viewer's --preload-doors off.
#
#   godot4.7 --path game --script res://tools/preload_check.gd -- \
#       --pack <pack> --cell RiverwoodSleepingGiantInn [--wait 20] [viewer options]
#
# Prints "preload_check:" lines: the travel frame, the frames until nothing
# streams, then 120 settled frames. Exits 1 if no load door is in the place.
extends SceneTree

var _viewer  # the viewer scene, driven through its script variables
var _frames := 0
var _door := {}
var _waited := 0.0
var _wait := 20.0
var _travelled := false
var _after: Array[float] = []
var _settled: Array[float] = []
var _travel_ms := 0.0
var _last := 0


func _initialize() -> void:
	var argv := OS.get_cmdline_user_args()
	for i in range(0, argv.size() - 1):
		if argv[i] == "--wait":
			_wait = float(argv[i + 1])
	_viewer = load("res://viewer/cell_viewer.tscn").instantiate()
	root.add_child(_viewer)
	# The keyboard and mouse must not move the view during a measurement.
	_viewer._input = false
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE


func _process(delta: float) -> bool:
	_frames += 1
	var now := Time.get_ticks_usec()
	var frame_ms := (now - _last) / 1000.0 if _last != 0 else 0.0
	_last = now
	if _frames < 30:
		return false
	if _door.is_empty():
		var nearest := 0
		var best := INF
		var eye: Vector3 = _viewer._rig.player.global_position
		for ref in _viewer._preloader.load_doors:
			var d: float = _viewer._preloader.load_doors[ref].global_position.distance_to(eye)
			if d < best:
				best = d
				nearest = ref
		if nearest == 0:
			printerr("preload_check: no load door here")
			quit(1)
			return true
		_door = _viewer._world.get_door(nearest)
		_viewer._rig.player.teleport(_viewer._preloader.load_doors[nearest].global_position + Vector3(0, 0.1, 0))
		print("preload_check: door 0x%08X, %.1f m away, leads to %s" % [nearest, best,
			SkydotWorld.godot_to_skyrim(_door["arrival"].origin)])
		return false
	if not _travelled:
		_waited += delta
		var done: bool = _viewer._preloader.current != null and _viewer._preloader.current.done
		if not done and _waited < _wait:
			return false
		var started := Time.get_ticks_usec()
		_viewer._transition.travel(_door)
		_travel_ms = (Time.get_ticks_usec() - started) / 1000.0
		_travelled = true
		_last = Time.get_ticks_usec()
		return false
	if _settled.is_empty() and _after.size() < 300 and (_viewer._streamer.streaming or _after.size() < 30):
		_after.append(frame_ms)
		return false
	_settled.append(frame_ms)
	if _settled.size() < 120:
		return false
	_settled.sort()
	var worst := 0.0
	var over := 0
	for ms in _after:
		worst = max(worst, ms)
		if ms > 33.0:
			over += 1
	print("preload_check: travel %.0f ms, then %d frames, worst %.0f ms, %d over 33 ms" %
		[_travel_ms, _after.size(), worst, over])
	print("preload_check: frames ", ", ".join(_after.map(func(ms: float) -> String: return "%.0f" % ms)))
	print("preload_check: then %d frames, median %.1f ms, p90 %.1f ms" %
		[_settled.size(), _settled[_settled.size() / 2], _settled[_settled.size() * 9 / 10]])
	quit(0)
	return true
