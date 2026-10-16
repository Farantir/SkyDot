# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sound effects: what Sound.Play, behaviour graphs, NIF text keys and placed
# sound markers ask for. No viewer, camera, input or overlay in here, only the
# pack, the world and the nodes sounds play on, so a VR shell reuses it as it
# is.
#
# A sound is an SNDR (descriptor): a random one of its files, played through an
# AudioStreamPlayer3D that is a child of the source node, so it moves with it.
# Its SOPM (output model) gives the distances, in game units; one that does not
# attenuate with distance plays non-positionally (AudioStreamPlayer), as does a
# sound without a source. Every player is a child of its source node, so it is
# freed with it; only a sound without a source lives under this node. Frequency shift and variance become pitch, static
# attenuation and dB variance become volume. Positional volume follows the
# SOPM's curve: five volume percentages spaced evenly from the min to the max
# distance (before the min the first, past the max the sound is out of range).
# Out of range, a loop is held until the listener comes back and a one-shot does
# not start. The listener is `listener`, else the viewport's current camera.
# A descriptor that loops (LNAM 0x08) loops the whole clip; the envelope kinds (0x10, 0x20) play once for now.
# Compound descriptors (CNAM other than 0x1EEF540A) are not played: they pick
# other descriptors by conditions.
#
# Callers hand sounds by SNDR or SOUN id (Sound.Play), by editor id (a
# behaviour event `SoundPlay.<SNDR>` or a text key `SoundPlay.X` / `Sound: X`),
# or place them (cell_loaded: every SOUN reference of a cell plays, loops
# stopping when the cell's node leaves the tree).
class_name Sounds
extends Node

## A playback ended, was stopped, or never started: for Sound.PlayAndWait.
signal finished(instance: int)

const STANDARD := 0x1EEF540A  # SNDR CNAM: a plain descriptor
const FLAG_ATTENUATES := 0x1  # SOPM NAM1 flags: volume falls with distance
const LOOP := 0x08  # SNDR LNAM byte 1
## Instances the game layer starts itself count from here; Sound.Play's are
## numbered by SkydotPapyrus from 1.
const FIRST_OWN_INSTANCE := 1 << 20
## Without an output model: where a positional sound starts to fade and stops,
## game units, and the shape of the fade (most vanilla SOPMs).
const DEFAULT_MIN := 150.0
const DEFAULT_MAX := 2000.0
const DEFAULT_CURVE: Array[int] = [100, 50, 20, 5, 0]
const SILENT_DB := -80.0
const EVENT_PREFIX := "soundplay."
const KEY_PREFIX := "sound:"

var _world: SkydotWorld
var _pack: SkydotPack
var _find_ref: Callable  # reference id -> its built node, or null
var _streams := {}  # vpath -> AudioStreamWAV, or null if it cannot be played
var _looped := {}  # vpath -> the same stream, set to loop
var _by_name := {}  # lowercase editor id -> SNDR id, 0 if there is none
var _models := {}  # SOPM id -> its dictionary
## Where sounds are heard from; null: the viewport's current camera.
var listener: Node3D
var _live := {}  # instance -> {name, player, base_db, user_db, positional, loops, min, max, curve}
var _next_instance := FIRST_OWN_INSTANCE
var _warn := WarnOnce.new("sound: ")


func _init(world: SkydotWorld, pack: SkydotPack, find_ref: Callable) -> void:
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
	var id := _descriptor_id(editor_id)
	if id == 0:
		_warn.once("name" + editor_id.to_lower(), "no sound descriptor %s" % editor_id)
		_fail.call_deferred(instance)
		return instance
	_start(_world.get_sound_descriptor(id), node, ref, instance)
	return instance


## Sound.StopInstance.
func stop(instance: int) -> void:
	var entry: Dictionary = _live.get(instance, {})
	if entry.is_empty():
		return
	var player: Node = entry["player"]
	if is_instance_valid(player):
		player.stop()
		player.queue_free()
	_ended(instance)


## Sound.SetInstanceVolume: `volume` is a factor, 1 the sound's own level.
func set_volume(instance: int, volume: float) -> void:
	var entry: Dictionary = _live.get(instance, {})
	if entry.is_empty() or not is_instance_valid(entry["player"]):
		return
	entry["user_db"] = linear_to_db(maxf(volume, 0.0))
	_update(entry)


func is_playing(instance: int) -> bool:
	return _live.has(instance)


## Play what a node's animator and behaviour graph ask for: NIF text keys
## `SoundPlay.X` and `Sound: X`, graph events `SoundPlay.X`. X is an SNDR or
## SOUN editor id. Once per node.
func watch(node: Node, ref: int) -> void:
	if node.has_meta("skydot_sounds"):
		return
	node.set_meta("skydot_sounds", true)
	var animator := node.get_node_or_null("SkydotAnimator")
	if animator != null:
		animator.text_key.connect(func(_clip: String, key: String) -> void:
			_on_key(key, node, ref))
	var player := node.find_child("SkydotBehavior", true, false)
	if player != null:
		player.event_raised.connect(func(event: String) -> void:
			_on_key(event, node, ref))


## A cell was built: watch its references and start its sound markers.
## `ref_nodes` are the cell's built references.
func cell_loaded(cell: Node3D, cell_id: int, ref_nodes: Array) -> void:
	for node in ref_nodes:
		watch(node, node.get_meta("skydot_ref"))
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
		if _start(descriptor, anchor, ref["id"], _new_instance()):
			started += 1
	if started > 0:
		print("sounds: %d markers play in cell 0x%08X" % [started, cell_id])


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


func _new_instance() -> int:
	_next_instance += 1
	return _next_instance


## Start `descriptor` on `node`; false if nothing plays (the instance then ends
## at once).
func _start(descriptor: Dictionary, node: Node, ref: int, instance: int) -> bool:
	var edid: String = descriptor["editor_id"]
	var files: Array = descriptor["files"]
	if descriptor["descriptor_type"] != STANDARD:
		_warn.once("compound" + edid, "sound %s is a compound descriptor: not played" % edid)
	elif files.is_empty():
		_warn.once("nofiles" + edid, "sound %s has no files" % edid)
	else:
		var file: String = files[randi() % files.size()]
		var loops: bool = int(descriptor["looping"]) & LOOP != 0
		var stream := _stream(file, loops)
		if stream != null:
			return _play_stream(descriptor, stream, file, node, ref, instance)
	_fail.call_deferred(instance)
	return false


func _play_stream(descriptor: Dictionary, stream: AudioStreamWAV, file: String, node: Node,
		ref: int, instance: int) -> bool:
	var model := _model(descriptor["output_model"])
	var positional := node is Node3D and (model.is_empty() or int(model["flags"]) & FLAG_ATTENUATES != 0)
	var loops: bool = int(descriptor["looping"]) & LOOP != 0
	var range_min := DEFAULT_MIN if model.is_empty() else float(model["min_distance"])
	var range_max := DEFAULT_MAX if model.is_empty() else float(model["max_distance"])
	var curve: Array = DEFAULT_CURVE if model.is_empty() or model["curve"].size() != 5 \
		else Array(model["curve"])
	if positional and not loops and _distance(node as Node3D) > range_max:
		print("sound %s ref 0x%08X out of range" % [descriptor["editor_id"], ref])
		_fail.call_deferred(instance)
		return false
	var player: Node
	var shift := float(descriptor["frequency_shift"])
	var spread := float(descriptor["frequency_variance"])
	var pitch := clampf(1.0 + (shift + randf_range(-spread, spread)) / 100.0, 0.1, 4.0)
	var db := -float(descriptor["static_attenuation"]) / 100.0 \
		- randf_range(0.0, float(descriptor["db_variance"]))
	if positional:
		var p := AudioStreamPlayer3D.new()
		# Volume over distance is ours (_update); Godot only pans and cuts off.
		p.attenuation_model = AudioStreamPlayer3D.ATTENUATION_DISABLED
		p.max_distance = range_max * SkydotWorld.unit_scale()
		p.pitch_scale = pitch
		p.volume_db = db
		p.stream = stream
		player = p
	else:
		var p := AudioStreamPlayer.new()
		p.pitch_scale = pitch
		p.volume_db = db
		p.stream = stream
		player = p
	var entry := {"name": "%s ref 0x%08X" % [descriptor["editor_id"], ref], "player": player, "base_db": db, "user_db": 0.0, "positional": positional,
		"loops": loops, "min": range_min, "max": range_max, "curve": curve, "held": false, "at": 0.0}
	_live[instance] = entry
	player.finished.connect(_ended.bind(instance))
	player.tree_exiting.connect(_ended.bind(instance))
	(node if node != null else self).add_child(player)
	if positional and loops and _distance(node as Node3D) > range_max:
		entry["held"] = true  # out of range: starts when the listener comes near
		print("sound %s held, out of range" % entry["name"])
	else:
		player.play()
	_update(entry)
	print("sound %s %s ref 0x%08X %s" % [descriptor["editor_id"], file, ref, "3d" if positional else "2d"])
	return true


func _process(_delta: float) -> void:
	for entry: Dictionary in _live.values():
		if entry["positional"]:
			_update(entry)


## Volume from the distance curve; a loop out of range is held (stopped, its
## position kept) until the listener comes back.
func _update(entry: Dictionary) -> void:
	var player: Node = entry["player"]
	if not is_instance_valid(player) or not player.is_inside_tree():
		return
	var db: float = entry["base_db"] + entry["user_db"]
	if entry["positional"]:
		var distance := _distance(player as Node3D)
		var out_of_range: bool = distance > entry["max"]
		if entry["loops"] and out_of_range != entry["held"]:
			# Stopped, not paused: a stopped player is not mixed, and a 3D
			# player drops stream_paused when its playback starts a tick late.
			entry["held"] = out_of_range
			if out_of_range:
				entry["at"] = player.get_playback_position()
				player.stop()
			else:
				player.play(entry["at"])
			print("sound %s %s" % [entry["name"], "held, out of range" if out_of_range else "resumed"])
		var gain := 0.0 if out_of_range else _curve_gain(entry, distance)
		db += linear_to_db(gain) if gain > 0.0 else SILENT_DB
	player.volume_db = maxf(db, SILENT_DB)


## The SOPM curve at `distance` game units: a volume factor 0..1.
static func _curve_gain(entry: Dictionary, distance: float) -> float:
	var curve: Array = entry["curve"]
	var span: float = entry["max"] - entry["min"]
	if distance <= entry["min"] or span <= 0.0:
		return curve[0] / 100.0
	var at := minf((distance - entry["min"]) / span, 1.0) * 4.0
	var i := mini(int(at), 3)
	return lerpf(curve[i], curve[i + 1], at - i) / 100.0


## From the listener to `node`, game units; 0 without a listener.
func _distance(node: Node3D) -> float:
	var ear: Node3D = listener if is_instance_valid(listener) else null
	if ear == null and is_inside_tree():
		ear = get_viewport().get_camera_3d()
	if ear == null or not node.is_inside_tree():
		return 0.0
	return ear.global_position.distance_to(node.global_position) / SkydotWorld.unit_scale()


## A playing instance ended (or its node left the tree): once per instance.
func _ended(instance: int) -> void:
	var entry: Dictionary = _live.get(instance, {})
	if entry.is_empty():
		return
	_live.erase(instance)
	if is_instance_valid(entry["player"]) and not entry["player"].is_queued_for_deletion():
		entry["player"].queue_free()
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


## The clip of `vpath`, looping over its whole data if `loops`; null (warned once)
## when the pack lacks it or Godot cannot read it.
func _stream(vpath: String, loops: bool) -> AudioStreamWAV:
	if not _streams.has(vpath):
		var bytes := _pack.get_bytes(vpath)
		var stream: AudioStreamWAV = null
		if bytes.is_empty():
			_warn.once("file" + vpath, "sound file %s is not in the pack" % vpath)
		else:
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
		copy.loop_mode = AudioStreamWAV.LOOP_FORWARD
		copy.loop_begin = 0
		copy.loop_end = copy.data.size() / (width * (2 if copy.stereo else 1))
		_looped[vpath] = copy
	return _looped[vpath]


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
