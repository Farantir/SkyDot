# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sound effects: what Sound.Play, behaviour graphs, NIF text keys and placed
# sound markers ask for. No viewer, camera, input or overlay in here, only the
# pack, the world and the nodes sounds play on, so a VR shell reuses it as it
# is. The game's data and what each field does: engine/docs/sound.md.
#
# A sound is an SNDR (descriptor). When it is asked for, the descriptors that
# are its alternates (their SNAM names it) and whose conditions pass play
# instead; a descriptor whose own conditions fail does not play. One of its
# files plays at random. Its SOPM (output model) says how:
# - type 0 (HRTF), attenuating: an AudioStreamPlayer3D on the source node, so
#   it moves with it and is panned;
# - type 1 (defined speaker output): an AudioStreamPlayer, the channels as the
#   speaker rows route them to the front pair; if it attenuates, the distance
#   to the source still scales it;
# - not attenuating, or without a source: an AudioStreamPlayer.
# Volume over distance is the SOPM's curve: five volume percentages spaced
# evenly from the min to the max distance (before the min the first, past the
# max the sound is out of range). Out of range, a loop is held until the
# listener comes back and a one-shot does not start. On top: the static
# attenuation and dB variance, and the category's volume (SNCT: static
# volumes up the parent chain times the menu sliders, `category_volumes`).
# Frequency shift and variance become pitch. The listener is `listener`, else
# the viewport's current camera.
# Looping (LNAM byte 1): 0x08 loops; the envelopes 0x10 and 0x20 ("LPM" files)
# play their start, loop between the file's sampler (smpl) loop points, and on
# stop finish the loop and play the tail. A 0x08 loop loops between the
# sampler points too (all vanilla loops span the whole file).
#
# Callers hand sounds by SNDR or SOUN id (Sound.Play), by editor id (a
# behaviour event `SoundPlay.<SNDR>` or a text key `SoundPlay.X` / `Sound: X`),
# or place them (cell_loaded: every SOUN reference of a cell plays, loops
# stopping when the cell's node leaves the tree). The ambience of where the
# listener is (acoustic spaces, region and weather sounds) is Ambience's.
class_name Sounds
extends Node

## A playback ended, was stopped, or never started: for Sound.PlayAndWait.
signal finished(instance: int)

const STANDARD := 0x1EEF540A  # SNDR CNAM: a plain descriptor (all vanilla ones)
const FLAG_ATTENUATES := 0x1  # SOPM NAM1 flags: volume falls with distance
const SPEAKER_OUTPUT := 1  # SOPM MNAM: defined speaker output
const LOOP := 0x08  # SNDR LNAM byte 1
const ENVELOPE_FAST := 0x10
const ENVELOPE_SLOW := 0x20
const MENU_CATEGORY := 0x2  # SNCT FNAM: a slider in the audio menu
## Without an output model: where a positional sound starts to fade and stops,
## game units, and the shape of the fade (most vanilla SOPMs).
const DEFAULT_MIN := 150.0
const DEFAULT_MAX := 2000.0
const DEFAULT_CURVE: Array[int] = [100, 50, 20, 5, 0]
const SILENT_DB := -80.0
## An HRTF (SOPM type 0) sound's level against Godot's 3D player at the same
## gain. Measured (skycompare sound series 2026-11-14): the Riverwood
## waterwheel loop at its min distance, game -43.7 dB RMS against SkyDot -30.9
## before categories; the Bannered Mare door, -38.5 against -27.3. Of those
## ~12 dB the category takes 1.8 (both are under SFX and Master, 0.81); the
## rest is this. A defined speaker output sound (the Bannered Mare interior
## loop, 2D) plays within 3 dB of its data, so it does not apply there.
## Measured on SOMMono01800 sounds only.
const HRTF_DB := -10.0
const EVENT_PREFIX := "soundplay."
const KEY_PREFIX := "sound:"
## How often a looping sound's conditions are asked again, and how long it
## fades when they change, seconds (estimated).
const RECHECK := 2.0
const GATE_FADE := 2.0

## A playing instance.
class Live:
	var name: String  # "<editor id> ref 0x..." for the log
	var edid: String
	var player: Node  # its AudioStreamPlayer or AudioStreamPlayer3D
	var source: Node3D  # what it is heard from when the distance counts, else null
	var base_db: float
	var user_db := 0.0  # set_volume's
	var distanced: bool
	var loops: bool
	var envelope: bool  # an envelope loop: stopping plays its tail
	var range_min: float
	var range_max: float
	var curve: Array  # the SOPM curve, five volumes in percent
	var held := false  # a loop out of range, stopped until the listener comes near
	var at := 0.0  # where a held loop resumes
	var fade: float
	var fade_target := 1.0
	var fade_rate: float
	var stop_at_zero := false
	var gated: int  # the SNDR whose conditions fade a loop in and out, 0 for none
	var ref: int

var _world: SkydotWorld
var _pack: SkydotPack
var _find_ref: Callable  # reference id -> its built node, or null
var _streams := {}  # vpath -> AudioStreamWAV, or null if it cannot be played
var _loop_points := {}  # vpath -> Vector2i(begin, end) frames of the sampler loop, or none
var _looped := {}  # vpath -> the same stream, set to loop
var _decoded := {}  # vpath -> AudioStream for load_stream, or null
var _by_name := {}  # lowercase editor id -> SNDR id, 0 if there is none
var _models := {}  # SOPM id -> its dictionary
var _alternates := {}  # SNDR id -> PackedInt64Array of the SNDRs that stand in for it
var _category_db := {}  # SNCT id -> dB
## Times every min and max distance (--sound-range): 1 is the game's.
var range_scale := 1.0
## Where sounds are heard from; null: the viewport's current camera.
var listener: Node3D
## The user's sliders of the menu categories, SNCT id -> 0..1; a category
## without an entry uses its default (SNCT UNAM).
var category_volumes := {}:
	set(value):
		category_volumes = value
		_category_db.clear()
## (sound, ref) -> whether the SNDR's conditions pass on that source; unset:
## every condition passes.
var conditions_met: Callable
var _live: Dictionary[int, Live] = {}  # instance -> its state
var _recheck := 0.0
## Told about each built cell (its placed acoustic spaces); may be null.
var ambience: Ambience
var _new_instance_id: Callable  # () -> a fresh instance id from the engine, 0 if used up
var _warn := WarnOnce.new("sound: ")


func _init(world: SkydotWorld, pack: SkydotPack, find_ref: Callable, new_instance_id: Callable) -> void:
	_new_instance_id = new_instance_id
	_world = world
	_pack = pack
	_find_ref = find_ref
	name = "Sounds"


## Sound.Play: `sound` is an SNDR or a SOUN, `source` the reference it plays
## on (0: nowhere in particular, so not positional). `instance` is the id the
## caller already handed out, or 0 for a new one. Returns the instance; its end
## (or failure to start) comes back as `finished`.
func play(sound: int, source: int, instance := 0) -> int:
	if instance == 0:
		instance = _new_instance()
		if instance == 0:
			return 0  # the engine's ids are used up (it logged the error)
	var node: Node = _find_ref.call(source) if source != 0 else null
	var descriptor := _descriptor_of(sound)
	if descriptor.is_empty():
		_warn.once("form%d" % sound, "sound 0x%08X is no sound descriptor or marker" % sound)
		_fail.call_deferred(instance)
		return instance
	_start(descriptor, node, source, instance)
	return instance


## Play the descriptor with this editor id on `node` (a built reference of id
## `ref`, or null).
func play_named(editor_id: String, node: Node, ref: int) -> int:
	var instance := _new_instance()
	if instance == 0:
		return 0
	var id := _descriptor_id(editor_id)
	if id == 0:
		_warn.once("name" + editor_id.to_lower(), "no sound descriptor %s" % editor_id)
		_fail.call_deferred(instance)
		return instance
	_start(_world.get_sound_descriptor(id), node, ref, instance)
	return instance


## Play SNDR `sound` for the ambience: on `node` (null: not positional),
## starting silent and fading in over `fade_in` seconds. Returns the instance,
## 0 if nothing plays (out of range, conditions, no file).
func play_ambient(sound: int, node: Node, fade_in: float) -> int:
	var descriptor := _world.get_sound_descriptor(sound)
	if descriptor.is_empty():
		return 0
	var instance := _new_instance()
	if instance == 0:
		return 0
	if not _start(descriptor, node, 0, instance, fade_in):
		return 0
	return instance if _live.has(instance) else 0


## Sound.StopInstance. An envelope loop finishes its loop and plays its tail
## first; `fade` > 0 fades out over that many seconds instead.
func stop(instance: int, fade := 0.0) -> void:
	var entry: Live = _live.get(instance)
	if entry == null:
		return
	var player := entry.player
	if fade > 0.0 and is_instance_valid(player):
		entry.fade_target = 0.0
		entry.fade_rate = 1.0 / fade
		entry.stop_at_zero = true
		return
	if entry.envelope and is_instance_valid(player) and player.playing and not entry.held:
		_exit_loop(entry)
		return
	if is_instance_valid(player):
		player.stop()
		player.queue_free()
	_ended(instance)


## Sound.SetInstanceVolume: `volume` is a factor, 1 the sound's own level.
func set_volume(instance: int, volume: float) -> void:
	var entry: Live = _live.get(instance)
	if entry == null or not is_instance_valid(entry.player):
		return
	entry.user_db = linear_to_db(maxf(volume, 0.0))
	_update(entry, 0.0)


func is_playing(instance: int) -> bool:
	return _live.has(instance)


## The descriptor's editor id of a playing instance, "" if none.
func instance_name(instance: int) -> String:
	var entry: Live = _live.get(instance)
	return entry.edid if entry != null else ""


## Play what a node's animator and behaviour graph ask for: NIF text keys
## `SoundPlay.X` and `Sound: X`, graph events `SoundPlay.X`. X is an SNDR or
## SOUN editor id. Once per node.
func watch(node: Node, ref: int) -> void:
	var animator := node.get_node_or_null("SkydotAnimator")
	var player := node.find_child("SkydotBehavior", true, false)
	var on_text_key := _on_text_key.bind(node, ref)
	var on_event := _on_key.bind(node, ref)
	# Once per node: the first connection tells (a swapped-in model is a new node and is watched).
	if animator != null:
		if animator.text_key.is_connected(on_text_key):
			return
	elif player != null and player.event_raised.is_connected(on_event):
		return
	if animator != null:
		animator.text_key.connect(on_text_key)
	if player != null:
		player.event_raised.connect(on_event)


## A cell was built: watch its references and start its sound markers.
## `ref_nodes` are the cell's built references.
func cell_loaded(cell: Node3D, cell_id: int, ref_nodes: Array) -> void:
	for node in ref_nodes:
		watch(node, node.get_meta("skydot_ref"))
	if ambience != null:
		ambience.cell_loaded(cell, cell_id)
	var started := 0
	for ref in _world.get_refs(cell_id):
		if ref["disabled"]:
			continue
		var marker := _world.get_sound_marker(ref["base"])
		if marker.is_empty():
			continue
		var anchor := Node3D.new()
		anchor.name = "SoundMarker_%08X" % ref["id"]
		anchor.transform = ref["transform"]
		cell.add_child(anchor)
		var descriptor := _world.get_sound_descriptor(marker["descriptor"])
		if descriptor.is_empty():
			_warn.once("marker%d" % ref["base"], "sound marker %s has no descriptor" % marker["editor_id"])
			continue
		var instance := _new_instance()
		if instance != 0 and _start(descriptor, anchor, ref["id"], instance):
			started += 1
	if started > 0:
		print("sounds: %d markers play in cell 0x%08X" % [started, cell_id])


func _on_text_key(_clip: String, key: String, node: Node, ref: int) -> void:
	_on_key(key, node, ref)


func _on_key(key: String, node: Node, ref: int) -> void:
	var text := key.strip_edges()
	var lower := text.to_lower()
	var wanted := ""
	if lower.begins_with(EVENT_PREFIX):
		wanted = text.substr(EVENT_PREFIX.length()).strip_edges()
	elif lower.begins_with(KEY_PREFIX):
		wanted = text.substr(KEY_PREFIX.length()).strip_edges()
	if wanted != "":
		play_named(wanted, node, ref)


## SNDR id of `sound` (an SNDR or a SOUN), as its dictionary; empty if neither.
func _descriptor_of(sound: int) -> Dictionary:
	var marker := _world.get_sound_marker(sound)
	return _world.get_sound_descriptor(marker["descriptor"] if not marker.is_empty() else sound)


## An SNDR by editor id, else a SOUN's descriptor (behaviour graphs name
## either: `SoundPlay.OBJPullBar` is an SNDR, `SoundPlay.OBJLeverNordic02Pull`
## the SOUN of OBJLeverNordic02PullSD). 0 if neither.
func _descriptor_id(editor_id: String) -> int:
	var key := editor_id.to_lower()
	if not _by_name.has(key):
		var id := _world.find_sound_descriptor(editor_id)
		if id == 0:
			id = _world.get_sound_marker(_world.find_sound_marker(editor_id)).get("descriptor", 0)
		_by_name[key] = id
	return _by_name[key]


## Any sound asset by vpath, told apart by its first bytes, not its extension:
## `RIFF` a WAV, `OggS` Ogg Vorbis (music), `SDVC` a voice container (its Ogg
## slice). Cached; null (warned once) when the pack lacks it or Godot cannot
## read it. The lip bytes of a voice are not used.
func load_stream(vpath: String) -> AudioStream:
	if _decoded.has(vpath):
		return _decoded[vpath]
	var stream: AudioStream = null
	var bytes := _pack.get_bytes(vpath)
	if bytes.is_empty():
		_warn.once("file" + vpath, "sound file %s is not in the pack" % vpath)
	elif bytes.slice(0, 4).get_string_from_ascii() == "RIFF":
		stream = _stream(vpath, false)
	else:
		stream = _ogg(bytes)
		if stream == null:
			_warn.once("file" + vpath, "sound file %s cannot be read" % vpath)
	_decoded[vpath] = stream
	return stream


## The Ogg Vorbis stream of plain Ogg bytes or of an `SDVC` container (u32
## version at 4, u32 Ogg offset at 8, u32 Ogg size at 12); null if neither.
static func _ogg(bytes: PackedByteArray) -> AudioStreamOggVorbis:
	var magic := bytes.slice(0, 4).get_string_from_ascii()
	if magic == "SDVC" and bytes.size() >= 24:
		var start := bytes.decode_u32(8)
		var size := bytes.decode_u32(12)
		if start < 24 or start + size > bytes.size():
			return null
		bytes = bytes.slice(start, start + size)
		magic = bytes.slice(0, 4).get_string_from_ascii()
	return AudioStreamOggVorbis.load_from_buffer(bytes) if magic == "OggS" else null


func _new_instance() -> int:
	return _new_instance_id.call()


## What plays when `descriptor` is asked for on `ref`: the first of its
## alternates whose conditions pass (and so on down their own alternates),
## else itself if its conditions pass; empty if nothing.
func _resolve(descriptor: Dictionary, ref: int, depth := 0) -> Dictionary:
	var id: int = descriptor["id"]
	if depth < 8:
		if not _alternates.has(id):
			_alternates[id] = _world.get_sound_alternates(id)
		for alt: int in _alternates[id]:
			var other := _world.get_sound_descriptor(alt)
			if not other.is_empty() and _passes(other, ref):
				return _resolve(other, ref, depth + 1)
	return descriptor if _passes(descriptor, ref) else {}


## Whether SNDR `sound` (or one of its alternates) would play on `ref` now.
func descriptor_passes(sound: int, ref: int) -> bool:
	var d := _world.get_sound_descriptor(sound)
	return not d.is_empty() and not _resolve(d, ref).is_empty()


func _passes(descriptor: Dictionary, ref: int) -> bool:
	if not descriptor.get("has_conditions", false) or not conditions_met.is_valid():
		return true
	return conditions_met.call(descriptor["id"], ref)


## Start `descriptor` on `node`; false if nothing plays (the instance then ends
## at once).
func _start(asked: Dictionary, node: Node, ref: int, instance: int, fade_in := 0.0) -> bool:
	var descriptor := _resolve(asked, ref)
	var gated_off := false
	if descriptor.is_empty() and int(asked["looping"]) != 0 and asked.get("has_conditions", false):
		# A loop whose conditions fail now starts silent and fades in when
		# they pass (the recheck in _process).
		descriptor = asked
		gated_off = true
	if descriptor.is_empty():
		print("sound %s ref 0x%08X: its conditions fail" % [asked["editor_id"], ref])
		_fail.call_deferred(instance)
		return false
	var edid: String = descriptor["editor_id"]
	var files: Array = descriptor["files"]
	if descriptor["descriptor_type"] != STANDARD:
		_warn.once("compound" + edid, "sound %s is a compound descriptor: not played" % edid)
	elif files.is_empty():
		_warn.once("nofiles" + edid, "sound %s has no files" % edid)
	else:
		var file: String = files[randi() % files.size()]
		var kind := int(descriptor["looping"])
		var stream := _stream(file, kind in [LOOP, ENVELOPE_FAST, ENVELOPE_SLOW])
		if stream != null:
			if not _play_stream(descriptor, stream, file, node, ref, instance, fade_in):
				return false
			if gated_off:
				(_live[instance] as Live).fade = 0.0
				(_live[instance] as Live).fade_target = 0.0
			return true
	_fail.call_deferred(instance)
	return false


func _play_stream(descriptor: Dictionary, stream: AudioStreamWAV, file: String, node: Node,
		ref: int, instance: int, fade_in: float) -> bool:
	var model := _model(descriptor["output_model"])
	var attenuates := model.is_empty() or int(model["flags"]) & FLAG_ATTENUATES != 0
	var distanced := node is Node3D and attenuates
	var panned := distanced and (model.is_empty() or int(model["type"]) != SPEAKER_OUTPUT)
	var kind := int(descriptor["looping"])
	var loops := kind in [LOOP, ENVELOPE_FAST, ENVELOPE_SLOW]
	var range_min := (DEFAULT_MIN if model.is_empty() else float(model["min_distance"])) * range_scale
	var range_max := (DEFAULT_MAX if model.is_empty() else float(model["max_distance"])) * range_scale
	var curve: Array = DEFAULT_CURVE if model.is_empty() or model["curve"].size() != 5 \
		else Array(model["curve"])
	if distanced and not loops and _distance(node as Node3D) > range_max:
		print("sound %s ref 0x%08X out of range" % [descriptor["editor_id"], ref])
		_fail.call_deferred(instance)
		return false
	var player: Node
	var shift := float(descriptor["frequency_shift"])
	var spread := float(descriptor["frequency_variance"])
	var pitch := clampf(1.0 + (shift + randf_range(-spread, spread)) / 100.0, 0.1, 4.0)
	var db := -float(descriptor["static_attenuation"]) / 100.0 \
		- randf_range(0.0, float(descriptor["db_variance"])) \
		+ category_db(int(descriptor["category"])) + _speaker_db(model, stream) \
		+ (HRTF_DB if panned else 0.0)
	if panned:
		var p := AudioStreamPlayer3D.new()
		# Volume over distance is ours (_update); Godot only pans and cuts off.
		p.attenuation_model = AudioStreamPlayer3D.ATTENUATION_DISABLED
		# Godot's air absorption low-pass took up to 10 dB more off a far
		# sound than the curve (the waterwheel series).
		p.attenuation_filter_db = 0.0
		p.max_distance = range_max * SkydotWorld.unit_scale()
		p.pitch_scale = pitch
		p.stream = stream
		player = p
	else:
		var p := AudioStreamPlayer.new()
		p.pitch_scale = pitch
		p.stream = stream
		player = p
	player.volume_db = SILENT_DB
	var entry := Live.new()
	entry.name = "%s ref 0x%08X" % [descriptor["editor_id"], ref]
	entry.edid = descriptor["editor_id"]
	entry.player = player
	entry.source = node as Node3D if distanced else null
	entry.base_db = db
	entry.distanced = distanced
	entry.loops = loops
	entry.envelope = kind != LOOP and loops
	entry.range_min = range_min
	entry.range_max = range_max
	entry.curve = curve
	entry.fade = 0.0 if fade_in > 0.0 else 1.0
	entry.fade_rate = 1.0 / fade_in if fade_in > 0.0 else 0.0
	entry.gated = int(descriptor["id"]) if loops and descriptor.get("has_conditions", false) else 0
	entry.ref = ref
	_live[instance] = entry
	player.finished.connect(_ended.bind(instance))
	player.tree_exiting.connect(_ended.bind(instance))
	(node if node != null else self).add_child(player)
	if distanced and loops and _distance(node as Node3D) > range_max:
		entry.held = true  # out of range: starts when the listener comes near
		print("sound %s held, out of range" % entry.name)
	else:
		player.play()
	_update(entry, 0.0)
	print("sound %s %s ref 0x%08X %s" % [descriptor["editor_id"], file, ref,
		"3d" if panned else "2d distance" if distanced else "2d"])
	return true


## The category's volume in dB: SNCT static volumes up the parent chain, times
## each menu category's slider (`category_volumes`, else the SNCT default).
func category_db(category: int) -> float:
	if category == 0:
		return 0.0
	if not _category_db.has(category):
		var gain := 1.0
		var id := category
		for depth in 16:
			var c := _world.get_sound_category(id)
			if c.is_empty():
				break
			gain *= float(c["static_volume"])
			if int(c["flags"]) & MENU_CATEGORY != 0:
				gain *= float(category_volumes.get(id, c["default_menu_volume"]))
			id = int(c["parent"])
			if id == 0:
				break
		_category_db[category] = linear_to_db(gain) if gain > 0.0 else SILENT_DB
	return _category_db[category]


## A defined speaker output's level on the front pair: the mono row's front
## left and right, or for a stereo file the left row's front left and the right
## row's front right (percent, averaged). 0 dB for an HRTF model or none.
static func _speaker_db(model: Dictionary, stream: AudioStreamWAV) -> float:
	if model.is_empty() or int(model["type"]) != SPEAKER_OUTPUT:
		return 0.0
	var rows: PackedByteArray = model.get("speaker_output", PackedByteArray())
	if rows.size() < 24:
		return 0.0
	var front := (rows[8] + rows[17]) / 2.0 if stream.stereo else (rows[0] + rows[1]) / 2.0
	return linear_to_db(front / 100.0) if front > 0.0 else SILENT_DB


func _process(delta: float) -> void:
	_recheck -= delta
	var recheck := _recheck <= 0.0
	if recheck:
		_recheck = RECHECK
	for instance: int in _live.keys():
		var entry: Live = _live.get(instance)
		if entry != null:
			if recheck and entry.gated != 0 and not entry.stop_at_zero:
				# A loop gated by conditions (a tavern's night loop) fades out and
				# back in as they change.
				var on := _passes({"id": entry.gated, "has_conditions": true}, entry.ref)
				if on != (entry.fade_target > 0.0):
					entry.fade_target = 1.0 if on else 0.0
					entry.fade_rate = 1.0 / GATE_FADE
			_update(entry, delta)
			if entry.stop_at_zero and entry.fade <= 0.0:
				stop(instance)


## Volume from the fade and the distance curve; a loop out of range is held
## (stopped, its position kept) until the listener comes back.
func _update(entry: Live, delta: float) -> void:
	var player := entry.player
	if not is_instance_valid(player) or not player.is_inside_tree():
		return
	if entry.fade != entry.fade_target:
		entry.fade = move_toward(entry.fade, entry.fade_target, entry.fade_rate * delta) \
			if entry.fade_rate > 0.0 else entry.fade_target
	var db := entry.base_db + entry.user_db
	if entry.fade < 1.0:
		db += linear_to_db(entry.fade) if entry.fade > 0.0 else SILENT_DB
	var source := entry.source
	if entry.distanced and is_instance_valid(source):
		var distance := _distance(source)
		var out_of_range := distance > entry.range_max
		if entry.loops and out_of_range != entry.held:
			# Stopped, not paused: a stopped player is not mixed, and a 3D
			# player drops stream_paused when its playback starts a tick late.
			entry.held = out_of_range
			if out_of_range:
				entry.at = player.get_playback_position()
				player.stop()
			else:
				player.play(entry.at)
			print("sound %s %s" % [entry.name, "held, out of range" if out_of_range else "resumed"])
		var gain := 0.0 if out_of_range else _curve_gain(entry, distance)
		db += linear_to_db(gain) if gain > 0.0 else SILENT_DB
	player.volume_db = maxf(db, SILENT_DB)


## The SOPM curve at `distance` game units: a volume factor 0..1.
static func _curve_gain(entry: Live, distance: float) -> float:
	var curve := entry.curve
	var span := entry.range_max - entry.range_min
	if distance <= entry.range_min or span <= 0.0:
		return curve[0] / 100.0
	var at := minf((distance - entry.range_min) / span, 1.0) * 4.0
	var i := mini(int(at), 3)
	return lerpf(curve[i], curve[i + 1], at - i) / 100.0


## From the listener to `node`, game units; 0 without a listener.
func _distance(node: Node3D) -> float:
	var ear: Variant = ear_position()
	if ear == null or not node.is_inside_tree():
		return 0.0
	return (ear as Vector3).distance_to(node.global_position) / SkydotWorld.unit_scale()


## Where the listener is (Godot space), null without one.
func ear_position() -> Variant:
	var ear: Node3D = listener if is_instance_valid(listener) else null
	if ear == null and is_inside_tree():
		ear = get_viewport().get_camera_3d()
	return ear.global_position if ear != null and ear.is_inside_tree() else null


## An envelope loop was stopped: let the current pass end at the loop end,
## then play the tail after it (XAudio2's ExitLoop).
func _exit_loop(entry: Live) -> void:
	var player := entry.player
	var stream: AudioStreamWAV = player.stream
	var rate := float(stream.mix_rate) * maxf(player.pitch_scale, 0.01)
	var position: float = player.get_playback_position()
	var to_end := maxf(stream.loop_end / rate - position, 0.0)
	var tail := _tail_of(stream)
	entry.envelope = false
	if tail == null:
		get_tree().create_timer(to_end).timeout.connect(func() -> void:
			if is_instance_valid(player):
				player.stop()
				player.queue_free())
		return
	get_tree().create_timer(to_end).timeout.connect(func() -> void:
		if is_instance_valid(player):
			player.stream = tail
			player.play())


## The frames after the loop end of a looping copy, as a one-shot; null if none.
static func _tail_of(stream: AudioStreamWAV) -> AudioStreamWAV:
	var width := (2 if stream.format == AudioStreamWAV.FORMAT_16_BITS else 1) * (2 if stream.stereo else 1)
	var start := stream.loop_end * width
	if start >= stream.data.size() - width:
		return null
	var tail := stream.duplicate() as AudioStreamWAV
	tail.data = stream.data.slice(start)
	tail.loop_mode = AudioStreamWAV.LOOP_DISABLED
	tail.loop_begin = 0
	tail.loop_end = 0
	return tail


## A playing instance ended (or its node left the tree): once per instance.
func _ended(instance: int) -> void:
	var entry: Live = _live.get(instance)
	if entry == null:
		return
	_live.erase(instance)
	if is_instance_valid(entry.player) and not entry.player.is_queued_for_deletion():
		entry.player.queue_free()
	finished.emit(instance)


## Nothing started: the caller still hears that the instance is over.
func _fail(instance: int) -> void:
	finished.emit(instance)


func _model(id: int) -> Dictionary:
	if id == 0:
		return {}
	if not _models.has(id):
		_models[id] = _world.get_sound_output_model(id)
	return _models[id]


## The clip of `vpath`; with `loops` a copy looping between the file's sampler
## loop points, or over its whole data without them. Null (warned once) when
## the pack lacks it or Godot cannot read it.
func _stream(vpath: String, loops: bool) -> AudioStreamWAV:
	if not _streams.has(vpath):
		var bytes := _pack.get_bytes(vpath)
		var stream: AudioStreamWAV = null
		if bytes.is_empty():
			_warn.once("file" + vpath, "sound file %s is not in the pack" % vpath)
		else:
			_loop_points[vpath] = _sampler_loop(bytes)
			stream = AudioStreamWAV.load_from_buffer(_without_trailer(bytes))
			if stream == null:
				_warn.once("file" + vpath, "sound file %s cannot be read" % vpath)
		_streams[vpath] = stream
	var plain: AudioStreamWAV = _streams[vpath]
	if plain == null or not loops:
		return plain
	if not _looped.has(vpath):
		var copy := plain.duplicate() as AudioStreamWAV
		var width := 2 if copy.format == AudioStreamWAV.FORMAT_16_BITS else 1
		var frames := copy.data.size() / (width * (2 if copy.stereo else 1))
		copy.loop_mode = AudioStreamWAV.LOOP_FORWARD
		copy.loop_begin = 0
		copy.loop_end = frames
		var points: Variant = _loop_points.get(vpath)
		if points is Vector2i and points.x < points.y and points.y <= frames:
			copy.loop_begin = points.x
			copy.loop_end = mini(points.y + 1, frames)  # the sampler's end frame is played
		_looped[vpath] = copy
	return _looped[vpath]


## The first sampler (smpl) loop of a RIFF: Vector2i(start, end) frames, or null.
static func _sampler_loop(bytes: PackedByteArray) -> Variant:
	var at := 12
	while at + 8 <= bytes.size():
		var size := bytes.decode_u32(at + 4)
		if bytes.slice(at, at + 4).get_string_from_ascii() == "smpl" and at + 8 + 60 <= bytes.size():
			if bytes.decode_u32(at + 8 + 28) > 0:
				return Vector2i(bytes.decode_u32(at + 8 + 36 + 8), bytes.decode_u32(at + 8 + 36 + 12))
			return null
		at += 8 + size + (size & 1)
	return null


## The RIFF up to the end of its data chunk. Godot's loader warns about the
## LIST and smpl chunks the game's files carry after it, and would pick up the
## sampler's loop points whatever the descriptor says.
func _without_trailer(bytes: PackedByteArray) -> PackedByteArray:
	var at := 12
	while at + 8 <= bytes.size():
		var size := bytes.decode_u32(at + 4)
		if bytes[at] == 0x64 and bytes[at + 1] == 0x61 and bytes[at + 2] == 0x74 and bytes[at + 3] == 0x61:  # "data"
			var out := bytes.slice(0, mini(at + 8 + size, bytes.size()))
			out.encode_u32(4, out.size() - 8)  # the RIFF size Godot checks the length against
			return out
		at += 8 + size + (size & 1)
	return bytes
