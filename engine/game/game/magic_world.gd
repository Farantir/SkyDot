# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the magic archetypes do in the scene (SYSTEMS.md 7a-4): the game layer's half of
# `SkydotPapyrus.magic_effect_event`. The rules live in the VM (ActiveEffects, `magic_overrides`)
# and the AI (SkydotAi obeys calm, fear, frenzy, paralysis, invisibility and commands); this
# builds what only a scene can:
#   light (12)      an OmniLight3D from the LIGH the MGEF names, held LIGHT_HEIGHT above the target
#   stagger (33)    SkydotAi.stagger; the player only logs it (PLACEHOLDER for a camera shake)
#   reanimate (22)  the corpse stands up (Corpses.stand_up) and SkydotAi.revive
#   bound (17)      the equipment changed (the hands and arms refresh); the weapon cannot be dropped
#   paralysis       on the player: no input, hands lowered (GameSession.player_paralyzed)
#   slow time (37)  Engine.time_scale while it holds on the player; the player's speeds are
#                   multiplied by 1/scale (PLACEHOLDER: gravity and the mouse look are not)
# Lines: `magic: light 0x.. on 0x..`, `magic: 0x.. staggers`, `magic: player paralyzed`.
class_name MagicWorld
extends Node

const PLAYER := 0
const LIGHT_HEIGHT := 2.4  # PLACEHOLDER: about 1 m above the shoulder
const ARCH_LIGHT := 12
const ARCH_BOUND := 17
const ARCH_REANIMATE := 22
const ARCH_STAGGER := 33
const ARCH_SLOW_TIME := 37

var _session: GameSession
var _lights := {}  # instance -> {node, target}
var _bound := {}  # weapon form -> instance (the player's)
var _paralyzed := false
var _scale := 1.0
var _speeds := {}  # the player's own speeds, while slowed


func _init(session: GameSession) -> void:
	_session = session
	name = "MagicWorld"
	_session.papyrus.magic_effect_event.connect(_on_event)


## The weapon forms the player holds from a bound weapon effect.
func is_bound(form: int) -> bool:
	return _bound.has(form)


func _on_event(info: Dictionary) -> void:
	var started: bool = info.get("started", false)
	var target := int(info.get("target", 0))
	match int(info.get("archetype", -1)):
		ARCH_LIGHT:
			if started:
				_light_start(int(info["instance"]), target, int(info.get("associated", 0)))
			else:
				_light_end(int(info["instance"]))
		ARCH_STAGGER:
			if started:
				_stagger(target, float(info.get("magnitude", 0.0)))
		ARCH_REANIMATE:
			if started:
				_reanimate(target)
		ARCH_BOUND:
			if target == SkydotPapyrus.PLAYER_REF:
				var weapon := int(info.get("associated", 0))
				if started:
					_bound[weapon] = int(info["instance"])
				else:
					_bound.erase(weapon)
				_session.items_changed.emit()


func _stagger(target: int, magnitude: float) -> void:
	if target == SkydotPapyrus.PLAYER_REF:
		print("magic: player staggers (%g)" % magnitude)  # PLACEHOLDER: no camera shake yet
		return
	if _session.ai != null:
		_session.ai.stagger(target, magnitude)


func _reanimate(target: int) -> void:
	if _session.ai == null:
		return
	if not _session.corpses.stand_up(target):
		print("magic: 0x%08X is not built, it rises when its place is built" % target)
		return
	_session.ai.revive(target)


# ---- light

func _light_start(instance: int, target: int, light: int) -> void:
	if _lights.has(instance):
		return
	var data: Dictionary = _session.world.get_base(light).get("light", {}) if light != 0 else {}
	var node := OmniLight3D.new()
	node.name = "MagicLight%d" % instance
	node.omni_range = maxf(float(data.get("radius", 3.0)), 1.0)
	node.light_color = data.get("color", Color(1.0, 0.9, 0.7))
	node.shadow_enabled = false
	_session.host.add_child(node)
	_lights[instance] = {"node": node, "target": target}
	_follow(node, target)
	print("magic: light 0x%08X on 0x%08X" % [light, target])


func _light_end(instance: int) -> void:
	if _lights.has(instance):
		var node: Node = _lights[instance]["node"]
		if is_instance_valid(node):
			node.queue_free()
		_lights.erase(instance)
		print("magic: light %d ends" % instance)


func lights() -> int:
	return _lights.size()


func _follow(node: OmniLight3D, target: int) -> void:
	var at := Vector3.ZERO
	if target == SkydotPapyrus.PLAYER_REF:
		at = _session.player_feet()
	else:
		var actor := _session.world.find_ref_node(target) as Node3D
		if actor == null or not actor.is_inside_tree():
			return
		at = actor.global_position
	node.global_position = at + Vector3.UP * LIGHT_HEIGHT


# ---- the frame

func step(_delta: float) -> void:
	for instance: int in _lights.keys():
		var entry: Dictionary = _lights[instance]
		if is_instance_valid(entry["node"]):
			_follow(entry["node"], entry["target"])
	var overrides: Dictionary = _session.papyrus.magic_overrides(SkydotPapyrus.PLAYER_REF)
	_set_paralyzed(bool(overrides.get("paralyzed", false)))
	_set_scale(float(overrides.get("time_scale", 1.0)))


func _set_paralyzed(value: bool) -> void:
	if value == _paralyzed:
		return
	_paralyzed = value
	_session.player_paralyzed = value
	if value:
		_session.lower_hands()
		print("magic: player paralyzed")
	else:
		print("magic: player moves again")


func _set_scale(scale: float) -> void:
	scale = clampf(scale, 0.1, 1.0)
	if is_equal_approx(scale, _scale):
		return
	var player := _session.player_object()
	if player != null and player.has_method("set_walk_speed"):
		if _scale == 1.0:
			_speeds = {"walk": player.get_walk_speed(), "run": player.get_run_speed(),
				"sprint": player.get_sprint_speed(), "jump": player.get_jump_height()}
		var inverse := 1.0 / scale
		if scale == 1.0 and not _speeds.is_empty():
			player.set_walk_speed(_speeds["walk"])
			player.set_run_speed(_speeds["run"])
			player.set_sprint_speed(_speeds["sprint"])
		elif not _speeds.is_empty():
			player.set_walk_speed(_speeds["walk"] * inverse)
			player.set_run_speed(_speeds["run"] * inverse)
			player.set_sprint_speed(_speeds["sprint"] * inverse)
	_scale = scale
	Engine.time_scale = scale
	print("magic: time scale %g" % scale)


## After a load: the lights and the bound weapons come from the effects that run now.
func on_loaded() -> void:
	for instance: int in _lights.keys():
		_light_end(instance)
	_bound.clear()
	var targets: Array[int] = [SkydotPapyrus.PLAYER_REF]
	for node in _session.host.find_children("*", "SkydotActor", true, false):
		if node.has_meta("skydot_ref"):
			targets.append(int(node.get_meta("skydot_ref")))
	for target in targets:
		for effect: Dictionary in _session.papyrus.active_effects(target):
			match int(effect.get("archetype", -1)):
				ARCH_LIGHT:
					_light_start(int(effect["instance"]), target, int(effect.get("associated", 0)))
				ARCH_BOUND:
					if target == SkydotPapyrus.PLAYER_REF:
						_bound[int(effect.get("associated", 0))] = int(effect["instance"])
	_set_paralyzed(false)
	_set_scale(1.0)
	step(0.0)
