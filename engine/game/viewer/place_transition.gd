# SPDX-License-Identifier: GPL-3.0-or-later
#
# Going through a load door: fade to black, travel on a black frame, wait
# until the new place is built (outside: the cells in range and the LOD) and a
# few frames are drawn (the first frame of a new place is slow), fade back.
# Without a person at the keyboard (or headless) it travels at once. A layer
# over everything, with the black rectangle on it.
class_name PlaceTransition
extends CanvasLayer

const FADE_SECONDS := 0.35
const SETTLE_FRAMES := 3  # drawn after the place is built
const TIMEOUT := 15.0  # seconds; fades in even if streaming never ends

enum Phase { NONE, OUT, TRAVEL, WAIT, IN }

## False for runs that drive themselves: doors travel at once, without a fade.
var interactive := true
var phase := Phase.NONE
var _fade: ColorRect  # black over everything during a door transition
var _door := {}  # the door being gone through
var _wait := 0.0  # seconds in WAIT
var _frames := 0  # frames since the place was built
var _place: Place
var _preloader: DoorPreloader
var _streamer: WorldStreamer


func _init(place: Place, preloader: DoorPreloader, streamer: WorldStreamer) -> void:
	_place = place
	_preloader = preloader
	_streamer = streamer
	layer = 100  # over the notes
	_fade = ColorRect.new()
	_fade.color = Color(0, 0, 0, 0)
	_fade.set_anchors_preset(Control.PRESET_FULL_RECT)
	_fade.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_fade.visible = false
	add_child(_fade)


func is_fading() -> bool:
	return phase != Phase.NONE


## Go through `door`: behind a fade if someone is watching, else at once.
func go(door: Dictionary) -> void:
	if interactive and DisplayServer.get_name() != "headless":
		begin_fade(door)
	else:
		travel(door)


## Go through `door` behind a fade to black (step).
func begin_fade(door: Dictionary) -> void:
	if phase != Phase.NONE:
		return
	_door = door
	phase = Phase.OUT
	_fade.visible = true


## The fade, a step per frame while is_fading.
func step(delta: float) -> void:
	match phase:
		Phase.OUT:
			_fade.color.a = minf(1.0, _fade.color.a + delta / FADE_SECONDS)
			if _fade.color.a >= 1.0:
				phase = Phase.TRAVEL  # this frame draws black first
		Phase.TRAVEL:
			travel(_door)
			_door = {}
			phase = Phase.WAIT
			_wait = 0.0
			_frames = 0
		Phase.WAIT:
			_wait += delta
			var built := _streamer.world_id == 0 or (not _streamer.streaming and not _streamer.lod_busy)
			if built:
				_frames += 1
			if _frames > SETTLE_FRAMES or _wait > TIMEOUT:
				phase = Phase.IN
		Phase.IN:
			_fade.color.a = maxf(0.0, _fade.color.a - delta / FADE_SECONDS)
			if _fade.color.a <= 0.0:
				phase = Phase.NONE
				_fade.visible = false


## Arrive through a load door: its XTEL gives the spot and the facing.
## What was built ahead for the place behind it is used.
func travel(door: Dictionary) -> void:
	var arrival: Transform3D = door["arrival"]
	var eye := arrival.origin + Vector3(0, PlayerRig.EYE_HEIGHT, 0)
	var forward := -arrival.basis.z
	forward.y = 0.0
	var target := eye + (forward.normalized() if forward.length() > 0.001 else Vector3.FORWARD)
	var prepared := _preloader.take(door)
	if door["destination_interior"]:
		_place.enter_interior(door["destination_cell"], eye, target, prepared.root if prepared != null else null)
	elif door["destination_world"] != 0:
		_place.enter_exterior(door["destination_world"], eye, target, prepared)
	else:
		print("door 0x%08X leads nowhere this pack knows" % door["ref"])
