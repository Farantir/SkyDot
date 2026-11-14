# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sound of where the listener is (engine/docs/sound.md): the acoustic
# space's ambient loop, the sounds of the regions and of the weather. No
# viewer, camera, input or overlay in here; the listener comes from Sounds.
#
# - Acoustic space: inside a placed ASPC box (the innermost one) that space,
#   else the cell's (CELL XCAS; an exterior cell's where the listener stands).
#   Its SNAM loop plays non-positionally; its RDAT region's sounds play while
#   inside.
# - Regions (exteriors): the regions of the worldspace whose areas hold the
#   listener, by sound priority; one with the override flag hides those below
#   it. An RDSA entry plays when its weather flags hold the current weather's
#   class (bit 0 pleasant, 1 cloudy, 2 rainy, 3 snowy) and its descriptor's
#   conditions pass (most birds and insects are gated by GetCurrentTime). A
#   loop (chance 1) plays while that holds; a one-shot is rolled every second
#   with its chance and plays at a random point around the listener.
# - Weather (exteriors): the current weather's SNAM loops (default,
#   precipitation, wind); thunder (type 3) is not played.
# A sound that starts or stops fades over FADE seconds, so walking from one
# space or region into the next crossfades.
class_name Ambience
extends Node

## Seconds a starting or stopping ambient sound fades (estimated, see
## OPEN-TASKS.md: the game's own fade is not measured).
const FADE := 2.0
## How often the listener's place and the one-shots are checked, seconds.
const TICK := 1.0
## A one-shot of a region plays this far from the listener, as a fraction of
## its output model's range from min to max (estimated).
const ONE_SHOT_SPAN := 0.5
const WEATHER_THUNDER := 3

var _world: SkydotWorld
var _sounds: Sounds
## () -> {"world": worldspace id or 0, "interior": cell id or 0}
var where: Callable
## () -> the current weather id, 0 if none.
var weather: Callable
var enabled := true
var _spaces := []  # [{anchor: Node3D, half: Vector3 (Godot, local), space: ASPC id}]
var _cell_space := {}  # cell id -> ASPC id (0 none)
var _space := -1  # the acoustic space now
var _space_loop := 0  # its ambient loop's instance
var _loops := {}  # key "r<region>:<sound>" / "w<sound>" -> instance
var _wait := 0.0
var _one_shot_root: Node3D


func _init(world: SkydotWorld, sounds: Sounds) -> void:
	_world = world
	_sounds = sounds
	name = "Ambience"


func _ready() -> void:
	_one_shot_root = Node3D.new()
	_one_shot_root.name = "OneShots"
	add_child(_one_shot_root)


## A cell was built: remember its placed acoustic spaces (ASPC references
## with an XPRM box). `cell` is its node; the boxes go when it does.
func cell_loaded(cell: Node3D, cell_id: int) -> void:
	for ref in _world.get_refs(cell_id):
		if ref["disabled"] or _world.get_acoustic_space(ref["base"]).is_empty():
			continue
		var info := _world.get_ref_info(cell_id, ref["id"])
		var primitive: Variant = info.get("primitive")
		if not primitive is Dictionary:
			continue
		var anchor := Node3D.new()
		anchor.name = "AcousticSpace_%08X" % ref["id"]
		anchor.transform = ref["transform"]
		cell.add_child(anchor)
		# Half extents (as triggers read XPRM), in the anchor's unscaled frame.
		var half := SkydotWorld.skyrim_position(primitive["bounds"]).abs()
		_spaces.append({"anchor": anchor, "half": half, "space": int(ref["base"])})


func _process(delta: float) -> void:
	_wait -= delta
	if _wait > 0.0:
		return
	_wait = TICK
	if not enabled or not where.is_valid():
		_set_space(0)
		_keep({})
		return
	var ear: Variant = _sounds.ear_position()
	if ear == null:
		return
	var place: Dictionary = where.call()
	var interior := int(place.get("interior", 0))
	var world_id := int(place.get("world", 0))
	var space := _space_at(ear, interior, world_id)
	_set_space(space)
	var wanted := {}  # key -> [sound, chance]
	var weather_class := 1
	var weather_id := 0
	if interior == 0 and weather.is_valid():
		weather_id = weather.call()
		var c := _world.get_weather_classification(weather_id) & 0xF
		weather_class = c if c != 0 else 1
	var regions: Array = []
	if space != 0:
		var r := _world.get_region_sound_data(_world.get_acoustic_space(space).get("region", 0))
		if not r.is_empty():
			regions.append(r)
	if interior == 0 and world_id != 0:
		var at := SkydotWorld.godot_to_skyrim(ear)
		var found := _world.get_region_sounds(world_id, at.x, at.y)
		found.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return a["priority"] > b["priority"])
		for r: Dictionary in found:
			regions.append(r)
			if r["override"]:
				break
	for r: Dictionary in regions:
		for s: Dictionary in r["sounds"]:
			if interior != 0 or int(s["flags"]) & weather_class != 0:
				wanted["r%d:%d" % [r["id"], s["sound"]]] = [int(s["sound"]), float(s["chance"])]
	if weather_id != 0:
		for s: Dictionary in _world.get_weather_sounds(weather_id):
			if int(s["type"]) != WEATHER_THUNDER:
				wanted["w%d" % s["sound"]] = [int(s["sound"]), 1.0]
	_keep(wanted)


## The acoustic space at `ear` (Godot space): the innermost placed box that
## holds it, else the cell's.
func _space_at(ear: Vector3, interior: int, world_id: int) -> int:
	var best := 0
	var best_volume := INF
	var alive := []
	for s: Dictionary in _spaces:
		# Validity first: a typed assignment of a freed node is a script error.
		if not is_instance_valid(s["anchor"]):
			continue
		var anchor: Node3D = s["anchor"]
		if not anchor.is_inside_tree():
			continue
		alive.append(s)
		var local: Vector3 = anchor.global_transform.affine_inverse() * ear
		var half: Vector3 = s["half"] * SkydotWorld.unit_scale()
		if absf(local.x) <= half.x and absf(local.y) <= half.y and absf(local.z) <= half.z:
			var volume := half.x * half.y * half.z
			if volume < best_volume:
				best_volume = volume
				best = s["space"]
	_spaces = alive
	if best != 0:
		return best
	var cell := interior
	if cell == 0 and world_id != 0:
		var at := SkydotWorld.godot_to_skyrim(ear)
		cell = _world.get_exterior_cell(world_id, floori(at.x / 4096.0), floori(at.y / 4096.0))
	if cell == 0:
		return 0
	if not _cell_space.has(cell):
		_cell_space[cell] = int(_world.get_cell(cell).get("acoustic_space", 0))
	return _cell_space[cell]


func _set_space(space: int) -> void:
	if space == _space:
		return
	_space = space
	if _space_loop != 0:
		_sounds.stop(_space_loop, FADE)
		_space_loop = 0
	var ambient := int(_world.get_acoustic_space(space).get("ambient", 0)) if space != 0 else 0
	if ambient != 0:
		_space_loop = _sounds.play_ambient(ambient, null, FADE)
	print("ambience: acoustic space %s" % (_world.get_acoustic_space(space).get("editor_id", "none")
		if space != 0 else "none"))


## Keep the loops of `wanted` playing (starting the new ones, fading out the
## others) and roll its one-shots.
func _keep(wanted: Dictionary) -> void:
	for key: String in _loops.keys():
		var instance: int = _loops[key]
		var entry: Variant = wanted.get(key)
		var keep: bool = entry != null and _sounds.is_playing(instance) \
			and _sounds.descriptor_passes(entry[0], 0)
		if not keep:
			_sounds.stop(instance, FADE)
			_loops.erase(key)
	for key: String in wanted:
		var sound: int = wanted[key][0]
		var chance: float = wanted[key][1]
		var descriptor := _world.get_sound_descriptor(sound)
		if descriptor.is_empty():
			continue
		if int(descriptor["looping"]) != 0:
			if not _loops.has(key) and _sounds.descriptor_passes(sound, 0):
				var instance := _sounds.play_ambient(sound, null, FADE)
				if instance != 0:
					_loops[key] = instance
		elif randf() < chance * TICK:
			_one_shot(descriptor)


## A region's one-shot at a random point around the listener (non-positional
## if its output model does not place it).
func _one_shot(descriptor: Dictionary) -> void:
	var model := _world.get_sound_output_model(descriptor["output_model"])
	var node: Node3D = null
	var ear: Variant = _sounds.ear_position()
	if not model.is_empty() and int(model["flags"]) & Sounds.FLAG_ATTENUATES != 0 and ear != null:
		var near := float(model["min_distance"])
		var far := near + (float(model["max_distance"]) - near) * ONE_SHOT_SPAN
		var angle := randf() * TAU
		var distance := randf_range(near, far) * _sounds.range_scale * SkydotWorld.unit_scale()
		node = Node3D.new()
		_one_shot_root.add_child(node)
		node.global_position = (ear as Vector3) + Vector3(cos(angle), randf_range(0.0, 0.3), sin(angle)) * distance
		get_tree().create_timer(30.0).timeout.connect(node.queue_free)
	_sounds.play_ambient(descriptor["id"], node, 0.0)
