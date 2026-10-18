# SPDX-License-Identifier: GPL-3.0-or-later
#
# The flat viewer's host of the death screen (ui/death_view.gd): a CanvasLayer over the
# window. When the player dies the controls stop and the camera lowers to the floor and rolls
# onto its side (the eyes of a body falling), then the view fades to black over 2 s and asks
# "Load the last save?": Enter or A loads the newest save, or starts the start place again
# when there is none; Esc quits.
class_name DeathMenu
extends CanvasLayer

## Seconds the camera takes to reach the floor, and how far it rolls (radians).
const FALL_SECONDS := 1.2
const ROLL := 1.4
## The camera's height above the floor when it lies there (metres).
const FLOOR_HEIGHT := 0.25

var view: DeathView
var _session: GameSession
var _rig: PlayerRig
var _input: PlayerInput
var _time := -1.0
var _eye_start := Vector3.ZERO


func _init(session: GameSession, rig: PlayerRig, input: PlayerInput) -> void:
	layer = 20
	_session = session
	_rig = rig
	_input = input
	view = DeathView.new(session)
	view.enabled = func() -> bool: return input.enabled
	view.load_chosen.connect(_load)
	view.quit_chosen.connect(func() -> void: get_tree().quit(0))
	add_child(view)
	session.player_died.connect(_on_died)


func is_dead() -> bool:
	return _time >= 0.0


func _on_died(_killer: int) -> void:
	_time = 0.0
	_input.blocked = true
	_eye_start = _rig.camera.global_position


func _process(delta: float) -> void:
	if _time < 0.0:
		return
	_time += delta
	var t := clampf(_time / FALL_SECONDS, 0.0, 1.0)
	var eased := t * t
	var floor_y := _rig.player.global_position.y + FLOOR_HEIGHT
	_rig.camera.global_position = Vector3(_eye_start.x, lerpf(_eye_start.y, floor_y, eased), _eye_start.z)
	_rig.camera.rotation = Vector3(_rig.pitch * (1.0 - eased), _rig.yaw, ROLL * eased)


func _load() -> void:
	if not _session.reload_or_restart():
		printerr("death: nothing to load and no start place")
		return
	_time = -1.0
	view.end()
	_input.blocked = false
