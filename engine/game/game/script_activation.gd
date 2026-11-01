# SPDX-License-Identifier: GPL-3.0-or-later
#
# Activation: what the player or a script activates (locks, load doors, plain
# doors, benches, containers, scripts) is decided by SkydotPapyrus.activate_ref
# and applied to the scene here. Also the --activate and --damage runs: refs in
# turn, each in the place the previous one led to.
class_name ScriptActivation
extends RefCounted

const REACH := 2.6  # metres the camera can activate from
## Frames a reference may be missing before the --activate run gives up (10 s at 60 fps).
const MISSING_FRAMES := 600

## --activate: every reference was activated; the run may end soon.
signal activations_finished
## --activate: a reference never appeared; the run is over.
signal failed(text: String)
signal message(text: String, box: bool)
signal talk_requested(ref: int)
signal container_opened(ref: int)
signal crafting_opened(ref: int, bench_type: int)

var _activations: Array[int] = []  # --activate: refs still to activate
var _wait := 0  # frames the first of them has been missing
var _gap := 0  # frames to pass before the next of them (--activate-wait)
var _damages: Array[Vector2] = []
var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _host: Node3D
var _config: GameConfig
var _transition: PlaceTransition
var _sounds: Sounds
var _animation: ScriptAnimationEvents


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, host: Node3D, config: GameConfig,
		transition: PlaceTransition, sounds: Sounds, animation: ScriptAnimationEvents) -> void:
	_world = world
	_papyrus = papyrus
	_host = host
	_config = config
	_transition = transition
	_sounds = sounds
	_animation = animation
	_activations.assign(config.activate)
	_damages.assign(config.damage)


## The player's activation of reference `ref`: SkydotPapyrus.activate_ref
## decides (parent-only, locks, scripts, activate children) and this applies
## what it asks of the scene. `force` picks locks. Returns false if the
## reference could not be found.
func activate(_cell: int, ref: int, _node: Node, force: bool) -> bool:
	var outcome := _papyrus.activate_ref(ref, SkydotPapyrus.PLAYER_REF, false,
		SkydotPapyrus.ACTIVATE_PLAYER, force or _config.pick_locks)
	if outcome["kind"] == "missing":
		return false
	apply_outcome(outcome, force)
	return true


## A script's Activate or an activate child, once the VM has run it.
func on_activation_outcome(_ref: int, outcome: Dictionary) -> void:
	apply_outcome(outcome, false)


## Do what an activation outcome asks of the scene and say what it noted.
func apply_outcome(outcome: Dictionary, picked := false) -> void:
	var ref: int = outcome["ref"]
	var label: String = outcome.get("label", "")
	var node := _world.find_ref_node(ref)
	if outcome["scripts"] > 0:
		print(label, " runs ", ", ".join(_papyrus.get_scripts(ref).map(func(s): return s["name"])))
	match outcome["kind"]:
		"parent_only", "blocked", "busy":
			message.emit(outcome["note"], false)
			return
		"locked":
			message.emit(outcome["note"] + "; Shift+F opens it anyway", false)
			return
		"door":
			print(label, " leads to 0x%08X" % outcome["door"]["destination"])
			_transition.go(outcome["door"])
			return
		"furniture":
			if node != null and _animation.play_event(node, ref, outcome["event"]):
				return
			# The graph will not raise ObjectActivated: the use counts now.
			if outcome.get("deferred", false):
				_papyrus.complete_activation(ref)
		"toggle_open":
			if node != null and _toggle_open(node, ref, label):
				return
		"harvest":
			if node != null:
				SkydotSwitch.set_index(node, 1)
			# The ingredient is already in the inventory.
			var sound: int = outcome.get("harvest_sound", 0)
			if sound != 0 and int(outcome.get("harvest_count", 0)) > 0:
				_sounds.play(sound, ref)
			message.emit(outcome["note"], false)
			return
		"crafting":
			print(label, " is a bench of type ", outcome["bench_type"])
			crafting_opened.emit(ref, outcome["bench_type"])
			return
	if outcome["kind"] == "container":
		print(label, " opens")
		container_opened.emit(ref)
		return
	if outcome["kind"] == "none" and _papyrus.dialogue_can_talk(ref):
		talk_requested.emit(ref)
		return
	if _papyrus.get_scripts(ref).is_empty():
		message.emit(outcome["note"], false)


func _toggle_open(node: Node, ref: int, label: String) -> bool:
	var animator := node.get_node_or_null("SkydotAnimator")
	if animator == null:
		return false
	var open: bool = not node.get_meta("skydot_open", false)
	if not animator.play("Open" if open else "Close"):
		return false
	node.set_meta("skydot_open", open)
	node.remove_meta("skydot_opened_by")  # an actor no longer closes it
	_papyrus.set_open_state(ref, 1 if open else 3)
	print(label, " opens" if open else " closes")
	return true


func has_activations() -> bool:
	return not _activations.is_empty() or not _damages.is_empty()


## --activate: activate the next reference once its model is built; quit
## when all are done or one never appears.
func run_activation() -> void:
	if _gap > 0:
		_gap -= 1
		return
	if _activations.is_empty():
		_run_damage()
		return
	var ref: int = _activations[0]
	var node := _world.find_ref_node(ref)
	if node == null:
		_wait += 1
		if _wait == MISSING_FRAMES + 1:  # once
			failed.emit("reference 0x%08X never appeared" % ref)
		return
	_wait = 0
	_activations.pop_front()
	activate(node.get_meta("skydot_cell"), ref, node, false)
	_gap = _config.activate_wait
	if _activations.is_empty() and _damages.is_empty():
		activations_finished.emit()


## --damage: the next DamageObject, once the activations are done.
func _run_damage() -> void:
	var entry: Vector2 = _damages.pop_front()
	print("damage 0x%08X by %s" % [int(entry.x), entry.y])
	_papyrus.damage_ref(int(entry.x), entry.y)
	_gap = _config.activate_wait
	if _damages.is_empty():
		activations_finished.emit()


## Activates what the viewpoint looks at. False if there was nothing known to activate.
func activate_in_view(force: bool, pointer: Node3D) -> bool:
	var from := pointer.global_position
	var to := from - pointer.global_transform.basis.z * REACH
	var hit := _world.pick_ref(_host, from, to)
	if hit.is_empty():
		message.emit("nothing to activate", false)
		return false
	return activate(hit["cell"], hit["ref"], hit["node"], force)
