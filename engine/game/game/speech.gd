# SPDX-License-Identifier: GPL-3.0-or-later
#
# Spoken lines: a dialogue line (the dictionary `papyrus.dialogue_open` and
# `dialogue_choose` return: text, seconds, voice) plays its voice file on an
# AudioStreamPlayer3D at its speaker's head, a child of the speaker's built
# node. One queue per speaker: a line starts when the speaker's previous one
# is over. A line lasts as long as its voice; without one (no voice type, the
# file missing or unreadable) it lasts `line["seconds"]`, the text timer
# max(2 s, 0.07 s/char) the engine computed. The player's dialogue and, later,
# scenes use the same queue. No viewer, camera, input or overlay in here.
#
# The log, per voiced line: `speech: 0x… voice <vpath> <seconds> s start <time>`
# and `… end <time>`, or `speech: 0x… voice missing <vpath>`; times are the
# game clock (the sum of `update` deltas), then `wall` (`Time.get_ticks_msec`), in seconds. The lines' clock is
# `update(delta)`, called once a frame.
class_name Speech
extends Node

## A line begins / is over (stopped lines too). `line` is the dictionary given
## to `say`; `seconds` is how long it lasts.
signal line_started(speaker: int, line: Dictionary, seconds: float)
signal line_ended(speaker: int, line: Dictionary)

## Where a head is above the speaker's origin, metres.
const HEAD_HEIGHT := 1.6
## Voices fade out past this, metres.
const MAX_DISTANCE := 30.0
const UNIT_SIZE := 3.0

var _sounds: Sounds
var _find_ref: Callable  # reference id -> its built node, or null
var _clock := 0.0  # game seconds: the sum of update(delta)
var _queues := {}  # speaker -> Array of entries; the first is playing


func _init(sounds: Sounds, find_ref: Callable) -> void:
	_sounds = sounds
	_find_ref = find_ref
	name = "Speech"


## Queue `line` for `speaker` (a reference id). Returns how many seconds it
## lasts once it plays; it starts at once if the speaker is silent.
func say(speaker: int, line: Dictionary) -> float:
	var entry := {"line": line, "player": null, "left": 0.0, "seconds": _seconds_of(line)}
	var queue: Array = _queues.get(speaker, [])
	queue.append(entry)
	_queues[speaker] = queue
	if queue.size() == 1:
		_start(speaker, entry)
	return entry["seconds"]


## Is `speaker` speaking or has lines waiting?
func is_speaking(speaker: int) -> bool:
	return _queues.has(speaker)


## Cut the speaker's current line short; the next one starts.
func skip(speaker: int) -> void:
	if _queues.has(speaker):
		_end(speaker)


## Stop the speaker and drop what is queued.
func stop(speaker: int) -> void:
	if _queues.has(speaker):
		(_queues[speaker] as Array).resize(1)  # the first is the one that has started
		_end(speaker)


## Advance the lines' clocks.
func update(delta: float) -> void:
	_clock += delta
	for speaker: int in _queues.keys():
		var queue: Array = _queues.get(speaker, [])
		if queue.is_empty():
			continue
		queue[0]["left"] -= delta
		while not queue.is_empty() and queue[0]["left"] <= 0.0:
			_end(speaker)
			queue = _queues.get(speaker, [])


func _seconds_of(line: Dictionary) -> float:
	var voice := _stream_of(line)
	if voice != null and voice.get_length() > 0.0:
		return voice.get_length()
	return float(line["seconds"])


## The decoded voice of `line`, null if it has none (logged when the file was
## named but is not usable). Cached by Sounds.
func _stream_of(line: Dictionary) -> AudioStream:
	var vpath: String = line.get("voice", "")
	if vpath.is_empty():
		return null
	return _sounds.load_stream(vpath)


func _start(speaker: int, entry: Dictionary) -> void:
	var line: Dictionary = entry["line"]
	var vpath: String = line.get("voice", "")
	var stream := _stream_of(line)
	entry["left"] = entry["seconds"]
	if stream != null and stream.get_length() > 0.0:
		var node: Node = _find_ref.call(speaker)
		var player := AudioStreamPlayer3D.new()
		player.stream = stream
		player.max_distance = MAX_DISTANCE
		player.unit_size = UNIT_SIZE
		player.position = Vector3(0.0, HEAD_HEIGHT, 0.0)
		(node if node != null else self).add_child(player)
		player.play()
		entry["player"] = player
		print("speech: 0x%08X voice %s %.2f s start %.2f wall %.2f" % [speaker, vpath, entry["seconds"], _clock, _now()])
	elif not vpath.is_empty():
		print("speech: 0x%08X voice missing %s" % [speaker, vpath])
	line_started.emit(speaker, line, entry["seconds"])


## The current line of `speaker` is over; start the next, if any.
func _end(speaker: int) -> void:
	var queue: Array = _queues[speaker]
	var entry: Dictionary = queue.pop_front()
	var player: Node = entry["player"]
	if player != null:
		if is_instance_valid(player):
			player.stop()
			player.queue_free()
		print("speech: 0x%08X voice %s %.2f s end %.2f wall %.2f" % [speaker, entry["line"]["voice"], entry["seconds"], _clock, _now()])
	if queue.is_empty():
		_queues.erase(speaker)
	line_ended.emit(speaker, entry["line"])
	if _queues.has(speaker):
		_start(speaker, _queues[speaker][0])


func _now() -> float:
	return Time.get_ticks_msec() / 1000.0
