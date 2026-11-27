# SPDX-License-Identifier: GPL-3.0-or-later
#
# The music (engine/docs/sound.md, "Music"). No viewer, camera, input or
# overlay in here; where the player is comes from `where` and Sounds' ear.
#
# - Choosing (choose(), every TICK and when the Papyrus stack changes): the
#   place's type (the location's, else the cell's XCMO, else the regions' RDMO
#   by region priority, else the world's ZNAM, else DFMS), combat (BTMS) while
#   an actor fights the player, and MusicType.Add's stack; the lowest priority
#   number wins, a tie goes to the later of place, combat, stack.
# - Playing: each type playing is a layer. A type that ducks plays over the
#   layers below, which go on ducked by its ducking dB; any other type
#   crossfades over its fade duration (abrupt: cuts). A layer picks a track:
#   cycle tracks in turn from a random start (maintain track order: from where
#   it was left), else at random; tracks whose conditions fail are passed over.
#   A palette plays its tracks in turn for its duration and fades out over its
#   fade-out; a silent track waits its duration. A layer that ends plays its
#   track's finale at the next cue point, else fades out. A plays-one-selection
#   type is taken off the stack after one track.
class_name Music
extends Node

## The chosen type changed: its MUSC id and where it came from ("location",
## "cell", "region", "world", "default", "combat", "script").
signal chosen(type: int, source: String)
## A track's file started (`finale` true for a finale).
signal track_started(track: int, file: String, finale: bool)

const BUS := &"Music"
## The menu's default ($Music in journal_system.gd).
const DEFAULT_VOLUME := 0.5
const TICK := 1.0
## PLACEHOLDER: seconds a layer takes to duck or come back up.
const DUCK_SECONDS := 1.0
## PLACEHOLDER: seconds before asking again when no track's conditions pass.
const RETRY := 5.0

class Layer:
	var id: int
	var type: Dictionary
	var player: AudioStreamPlayer
	var index := 0  # the next track of a cycling type
	var selections := 0
	var track := {}  # the single or silent track now, {} between tracks
	var palette := {}  # the palette whose tracks play, or {}
	var sub := 0  # its next track
	var palette_left := 0.0
	var wait := 0.0  # silence left
	var loops := 0
	var fade := 1.0  # the layer's crossfade, linear
	var fade_to := 1.0
	var fade_rate := 0.0
	var duck := 1.0  # under a ducking layer, linear
	var ending := false
	var finale_at := -1.0  # seconds into the track, -1 none
	var in_finale := false

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _sounds: Sounds
## () -> {"world": worldspace id or 0, "interior": cell id or 0} (Ambience's).
var where: Callable
## () -> whether an actor fights the player.
var in_combat: Callable
## Decode and play the tracks; off (--music off, headless tests): only choose.
var play_streams := false
var _layers: Array[Layer] = []
var _kept_index := {}  # MUSC id -> its next track (maintain track order)
var _cell_music := {}  # cell id -> [location music, cell music]
var _choice := 0
var _wait := 0.0


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, sounds: Sounds) -> void:
	_world = world
	_papyrus = papyrus
	_sounds = sounds
	name = "Music"


func _ready() -> void:
	if AudioServer.get_bus_index(BUS) < 0:
		AudioServer.add_bus()
		var bus := AudioServer.bus_count - 1
		AudioServer.set_bus_name(bus, BUS)
		AudioServer.set_bus_send(bus, &"Master")
	set_volume(DEFAULT_VOLUME)
	_papyrus.music_changed.connect(_on_stack_changed)


## The $Music slider, 0..1.
func set_volume(volume: float) -> void:
	AudioServer.set_bus_volume_db(AudioServer.get_bus_index(BUS), linear_to_db(volume) if volume > 0.0 else -80.0)


## [MUSC id, source] of the music that should play now.
func choose() -> Array:
	var best := _place_music()
	if in_combat.is_valid() and in_combat.call():
		best = _rank(best, [_world.get_default_object("BTMS"), "combat"])
	for id in _papyrus.get_music_stack():
		best = _rank(best, [id, "script"])
	return best


## The later candidate wins a tie (assumed).
func _rank(best: Array, candidate: Array) -> Array:
	var type := _world.get_music_type(candidate[0])
	if type.is_empty():
		return best
	return candidate if int(type["priority"]) <= int(_world.get_music_type(best[0]).get("priority", 0xFFFF)) else best


## The first that is set of location, cell, region, world, DFMS (assumed order).
func _place_music() -> Array:
	var place: Dictionary = where.call() if where.is_valid() else {}
	var cell := int(place.get("interior", 0))
	var world_id := int(place.get("world", 0))
	var ear: Variant = _sounds.ear_position()
	var at := SkydotWorld.godot_to_skyrim(ear) if ear != null else Vector3.ZERO
	if cell == 0 and world_id != 0 and ear != null:
		cell = _world.get_exterior_cell(world_id, floori(at.x / 4096.0), floori(at.y / 4096.0))
	if cell != 0:
		if not _cell_music.has(cell):
			var location := int(_world.get_cell(cell).get("location", 0))
			_cell_music[cell] = [_world.get_location_music(location) if location != 0 else 0, _world.get_cell_music(cell)]
		if _cell_music[cell][0] != 0:
			return [_cell_music[cell][0], "location"]
		if _cell_music[cell][1] != 0:
			return [_cell_music[cell][1], "cell"]
	if int(place.get("interior", 0)) == 0 and world_id != 0:
		var region := 0
		var priority := -1
		if ear != null:
			for r: Dictionary in _world.get_region_music(world_id, at.x, at.y):
				if int(r["priority"]) > priority:
					priority = int(r["priority"])
					region = int(r["music"])
		if region != 0:
			return [region, "region"]
		if _world.get_world_music(world_id) != 0:
			return [_world.get_world_music(world_id), "world"]
	return [_world.get_default_object("DFMS"), "default"]


func _on_stack_changed() -> void:
	var stack := _papyrus.get_music_stack()
	if not stack.is_empty() and _world.get_music_type(stack[-1]).get("doesnt_queue", false) \
			and choose()[0] != stack[-1]:
		_papyrus.call_method(stack[-1], "MusicType", "Remove", [])  # it cannot play now: dropped (assumed)
	_wait = 0.0


func _process(delta: float) -> void:
	_wait -= delta
	if _wait <= 0.0:
		_wait = TICK
		var c := choose()
		if c[0] != _choice:
			_choice = c[0]
			print("music: %s (%s)" % [_world.get_music_type(_choice).get("editor_id", "none"), c[1]])
			chosen.emit(_choice, c[1])
			if play_streams:
				_switch(_choice)
	for i in _layers.size():
		var below := 1.0
		for j in range(i + 1, _layers.size()):
			if _layers[j].type["ducks_current_track"]:
				below = minf(below, db_to_linear(-float(_layers[j].type["ducking_db"])))
		_layers[i].duck = move_toward(_layers[i].duck, below, delta / DUCK_SECONDS)
	for layer in _layers.duplicate():
		_advance(layer, delta)


func _switch(id: int) -> void:
	for i in range(_layers.size() - 1, -1, -1):
		if _layers[i].id == id and not _layers[i].ending:
			for above in _layers.slice(i + 1):
				_end(above, false, -1.0)
			return
	var type := _world.get_music_type(id)
	if type.is_empty():
		return
	var abrupt: bool = type["abrupt_transition"]
	var fade := 0.0 if abrupt else float(type["fade_duration"])
	if not type["ducks_current_track"]:
		for layer in _layers.duplicate():
			_end(layer, abrupt, fade)
	var layer := Layer.new()
	layer.id = id
	layer.type = type
	var count: int = type["tracks"].size()
	layer.index = _kept_index.get(id, 0) if type["maintain_track_order"] else (randi() % count if count > 0 else 0)
	layer.player = AudioStreamPlayer.new()
	layer.player.bus = BUS
	add_child(layer.player)
	if fade > 0.0 and not type["ducks_current_track"] and not _layers.is_empty():
		layer.fade = 0.0
		layer.fade_rate = 1.0 / fade
	_layers.append(layer)
	_next(layer)


## Stop a layer: cut, or its finale at the next cue point, else fade out over
## `fade` seconds (-1: its own fade duration).
func _end(layer: Layer, cut: bool, fade: float) -> void:
	if layer.ending:
		return
	layer.ending = true
	if cut:
		_free(layer)
		return
	if layer.track.get("finale", "") != "" and layer.player.playing:
		var at := layer.player.get_playback_position()
		layer.finale_at = layer.player.stream.get_length()
		for cue: float in layer.track["cue_points"]:
			if cue > at:
				layer.finale_at = cue
				break
		return
	fade = float(layer.type["fade_duration"]) if fade < 0.0 else fade
	if fade <= 0.0:
		_free(layer)
		return
	layer.fade_to = 0.0
	layer.fade_rate = layer.fade / fade


func _advance(layer: Layer, delta: float) -> void:
	layer.fade = move_toward(layer.fade, layer.fade_to, layer.fade_rate * delta)
	if layer.ending and layer.fade <= 0.0:
		_free(layer)
		return
	var palette_fade := 1.0
	if not layer.palette.is_empty():
		layer.palette_left -= delta
		var out := float(layer.palette["fade_out"])
		if out > 0.0:
			palette_fade = clampf(layer.palette_left / out, 0.0, 1.0)
	layer.player.volume_db = linear_to_db(maxf(layer.fade * layer.duck * palette_fade, 0.0001))
	var player := layer.player
	if layer.in_finale:
		if not player.playing:
			_free(layer)
		return
	if layer.finale_at >= 0.0:
		if not player.playing or player.get_playback_position() >= layer.finale_at:
			layer.in_finale = true
			_play(layer, layer.track["finale"], int(layer.track["id"]), true)
		return
	if not layer.palette.is_empty() and layer.palette_left <= 0.0:
		player.stop()
		layer.palette = {}
		_next(layer)
	elif layer.wait > 0.0:
		layer.wait -= delta
		if layer.wait <= 0.0:
			_next(layer)
	elif layer.loops > 0 and player.playing and player.get_playback_position() >= float(layer.track["loop_end"]) \
			and float(layer.track["loop_end"]) > 0.0:
		layer.loops -= 1
		player.seek(float(layer.track["loop_begin"]))
	elif not player.playing:
		if layer.loops > 0:
			layer.loops -= 1
			player.play(float(layer.track["loop_begin"]))
		else:
			_next(layer)


## The layer's next track: the palette's, else one of the type's.
func _next(layer: Layer) -> void:
	layer.track = {}
	if not layer.palette.is_empty() and _palette_next(layer):
		return
	layer.palette = {}
	if layer.selections > 0 and layer.type["plays_one_selection"]:
		_one_selection_done(layer)
		return
	var list: Array = layer.type["tracks"]
	for n in list.size():
		var i := randi() % list.size()
		if layer.type["cycle_tracks"]:
			i = layer.index % list.size()
			layer.index = i + 1
		var t := _usable(list[i])
		if t.is_empty():
			continue
		if t["type"] == "palette":
			layer.palette = t
			layer.sub = 0
			layer.palette_left = float(t["duration"])
			if not _palette_next(layer):
				layer.palette = {}
				continue
		else:
			_start(layer, t)
		layer.selections += 1
		return
	layer.wait = RETRY


## Start the palette's next usable track, in turn, wrapping; false if none.
func _palette_next(layer: Layer) -> bool:
	var subs: Array = layer.palette["tracks"]
	for n in subs.size():
		var t := _usable(subs[layer.sub % subs.size()])
		layer.sub += 1
		if not t.is_empty() and t["type"] != "palette":
			_start(layer, t)
			return true
	return false


## The track if it exists and its conditions pass, else {}.
func _usable(id: int) -> Dictionary:
	if id == 0:
		return {}
	var t := _world.get_music_track(id)
	if t.is_empty() or (t["has_conditions"] and not _papyrus.music_track_conditions_met(id)):
		return {}
	return t


func _start(layer: Layer, t: Dictionary) -> void:
	layer.track = t
	if t["type"] == "silent":
		layer.wait = maxf(float(t["duration"]), 0.1)
		return
	layer.loops = int(t["loop_count"])
	if not _play(layer, t["file"], int(t["id"]), false):
		layer.wait = RETRY


func _play(layer: Layer, file: String, track: int, finale: bool) -> bool:
	var stream := _sounds.load_stream(file, false)
	if stream == null:
		return false
	layer.player.stream = stream
	layer.player.play()
	print("music: %s %s" % ["finale" if finale else "track", file])
	track_started.emit(track, file, finale)
	return true


func _one_selection_done(layer: Layer) -> void:
	_free(layer)
	if layer.id in _papyrus.get_music_stack():
		_papyrus.call_method(layer.id, "MusicType", "Remove", [])  # assumed: the game drops it so
	else:
		_choice = 0  # not the stack's: chosen again, it plays again
	_wait = 0.0


func _free(layer: Layer) -> void:
	_kept_index[layer.id] = layer.index
	layer.player.queue_free()
	_layers.erase(layer)


## The layers playing, bottom first: [{type, track, finale, playing, volume_db}] (tests, overlay).
func layers() -> Array:
	var out := []
	for layer in _layers:
		out.append({"type": layer.id, "track": layer.track.get("editor_id", ""), "finale": layer.in_finale or
			layer.finale_at >= 0.0, "playing": layer.player.playing, "volume_db": layer.player.volume_db})
	return out
