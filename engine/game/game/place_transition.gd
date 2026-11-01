# SPDX-License-Identifier: GPL-3.0-or-later
#
# Going through a load door: fade to black, travel on a black frame, wait
# until the new place is built (outside: the cells in range and the LOD) and a
# few frames are drawn (the first frame of a new place is slow), fade back.
# Without a person at the keyboard (or headless) it travels at once. It only
# keeps the fade as a number (`fade`, `fade_changed`); drawing it is the shell's
# (the flat viewer's FadeLayer, the VR shell's quad on the head).
class_name PlaceTransition
extends RefCounted

## The fade changed: 0 clear, 1 black. Also back to 0 when a transition is cancelled.
signal fade_changed(alpha: float)

const FADE_SECONDS := 0.35
const SETTLE_FRAMES := 3  # drawn after the place is built
const TIMEOUT := 15.0  # seconds; fades in even if streaming never ends

enum Phase { NONE, OUT, TRAVEL, WAIT, IN }

## False for runs that drive themselves: doors travel at once, without a fade.
var interactive := true
var phase := Phase.NONE
## How black the screen is during a door transition: 0 clear, 1 black.
var fade := 0.0:
	set(value):
		if value != fade:
			fade = value
			fade_changed.emit(value)
var _door := {}  # the door being gone through
var _wait := 0.0  # seconds in WAIT
var _frames := 0  # frames since the place was built
var _place: Place
var _streamer: SkydotStreamer


func _init(place: Place, streamer: SkydotStreamer) -> void:
	_place = place
	_streamer = streamer


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


## Drop a door transition half way (a loaded save replaces the place it was
## going to): nothing travels afterwards and the screen is clear.
func cancel() -> void:
	phase = Phase.NONE
	_door = {}
	_wait = 0.0
	_frames = 0
	fade = 0.0


## A save was loaded or the game restarted: a door transition half way is dropped. Silent.
func reset() -> void:
	cancel()


## The place was left: the transition itself leaves it, so it must go on.
func on_place_left() -> void:
	pass


## The fade, a step per frame while is_fading.
func step(delta: float) -> void:
	match phase:
		Phase.OUT:
			fade = minf(1.0, fade + delta / FADE_SECONDS)
			if fade >= 1.0:
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
			fade = maxf(0.0, fade - delta / FADE_SECONDS)
			if fade <= 0.0:
				phase = Phase.NONE


## Arrive through a load door: its XTEL gives the spot and the facing.
## What was built ahead for the place behind it is used.
func travel(door: Dictionary) -> void:
	var arrival: Transform3D = door["arrival"]
	var eye := arrival.origin + Vector3(0, GameSession.EYE_HEIGHT, 0)
	var forward := -arrival.basis.z
	forward.y = 0.0
	var target := eye + (forward.normalized() if forward.length() > 0.001 else Vector3.FORWARD)
	var prepared := _streamer.take_prepared(door)
	if door["destination_interior"]:
		_place.enter_interior(door["destination_cell"], eye, target, prepared.root if prepared != null else null)
	elif door["destination_world"] != 0:
		_place.enter_exterior(door["destination_world"], eye, target, prepared)
	else:
		print("door 0x%08X leads nowhere this pack knows" % door["ref"])
