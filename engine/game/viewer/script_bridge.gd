# SPDX-License-Identifier: GPL-3.0-or-later
#
# Between SkydotPapyrus and the scene: what scripts enable, disable, open,
# animate, push and say shows up in the built nodes and on screen, and what
# the player or a script activates (locks, load doors, plain doors, scripts)
# is done here. Also the --activate runs: activate references in turn, each
# in the place the previous one led to.
class_name ScriptBridge
extends RefCounted

const REACH := 2.6  # metres the camera can activate from

## --activate: every reference was activated; the run may end soon.
signal activations_finished
## --activate: a reference never appeared; the run is over.
signal failed(text: String)

## After the start-game quests have started: quest changes print from then on.
var quests_ready := false
var _activations: Array[int] = []  # --activate: refs still to activate
var _wait := 0  # frames the first of them has been missing
var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _host: Node3D  # the scene's root, where picking and waking clutter start
var _camera: Camera3D
var _settings: ViewerSettings
var _place: Place
var _streamer: SkydotStreamer
var _debug: DebugOverlay
var _transition: PlaceTransition


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, host: Node3D, camera: Camera3D,
		settings: ViewerSettings, place: Place, streamer: SkydotStreamer, debug: DebugOverlay,
		transition: PlaceTransition) -> void:
	_world = world
	_papyrus = papyrus
	_host = host
	_camera = camera
	_settings = settings
	_place = place
	_streamer = streamer
	_debug = debug
	_transition = transition
	_activations.assign(settings.activate)
	_papyrus.enable_changed.connect(_on_enable_changed)
	_papyrus.play_animation.connect(_on_play_animation)
	_papyrus.havok_impulse.connect(_on_havok_impulse)
	_papyrus.motion_type_changed.connect(_on_motion_type)
	_papyrus.activate_requested.connect(_on_activate_requested)
	_papyrus.open_changed.connect(_on_open_changed)
	_papyrus.lock_changed.connect(_on_lock_changed)
	_papyrus.message.connect(_on_message)
	_papyrus.quest_started.connect(_on_quest_started)
	_papyrus.quest_stage.connect(_on_quest_stage)
	_papyrus.objective_changed.connect(_on_objective_changed)
	_papyrus.effect_shader.connect(_on_effect_shader)
	_papyrus.trigger.connect(_on_trigger)


## Attach the scripts of what was just built (OnInit once, OnLoad each time),
## including model-less ones such as triggers, and show script-made changes to
## what is enabled.
func scripts_loaded(root: Node, cell_id: int) -> void:
	if cell_id != 0:
		_papyrus.attach_cell(cell_id)
	var scripted := _papyrus.attach_built(root)
	if scripted > 0:
		print("scripts: %d references in %s" % [scripted, root.name])
	# Every change scripts have made so far: apply those to this cell's
	# references in one walk over it, not a search of the scene for each.
	var changes := _papyrus.get_disabled_changes()
	if not changes.is_empty():
		for node in Place.ref_nodes(root):
			var ref: int = node.get_meta("skydot_ref")
			if changes.has(ref):
				_show_ref(node, not changes[ref])


func _on_activate_requested(ref: int, _activator: int, _default_only: bool) -> void:
	var node := find_ref_node(ref)
	call_deferred("activate", _world.get_ref_cell(ref), ref, node, true)


func _on_open_changed(ref: int, open: bool) -> void:
	var node := find_ref_node(ref)
	print("0x%08X %s by script" % [ref, "opened" if open else "closed"])
	if node != null:
		node.set_meta("skydot_open", open)
		_on_play_animation(ref, "Open" if open else "Close")


func _on_lock_changed(ref: int, locked: bool) -> void:
	print("0x%08X %s by script" % [ref, "locked" if locked else "unlocked"])


func _on_message(text: String, _box: bool) -> void:
	_debug.note(text)


func _on_quest_started(quest: int) -> void:
	if quests_ready:
		print("quest started: ", _debug.quest_name(quest))


## Only stages with journal text reach the screen, as in the game.
func _on_quest_stage(quest: int, stage: int, text: String) -> void:
	if text != "":
		_debug.note("%s: %s" % [_debug.quest_name(quest), text])
	elif quests_ready:
		print("%s: stage %d" % [_debug.quest_name(quest), stage])


func _on_objective_changed(quest: int, index: int, state: String, text: String) -> void:
	_debug.note("%s objective %d %s: %s" % [_debug.quest_name(quest), index, state, text])


func _on_effect_shader(shader: int, ref: int, playing: bool) -> void:
	print("effect shader 0x%08X %s on 0x%08X (not drawn yet)" % [shader, "plays" if playing else "stops", ref])


func _on_trigger(ref: int, _actor: int, entered: bool) -> void:
	print("%s trigger 0x%08X" % ["entered" if entered else "left", ref])


func _on_enable_changed(ref: int, enabled: bool) -> void:
	print("0x%08X %s by script" % [ref, "enabled" if enabled else "disabled"])
	var node := find_ref_node(ref)
	if node != null:
		if not enabled:
			_wake_around(node)
		_show_ref(node, enabled)
	elif enabled and _in_place(ref):
		# Initially disabled references are not built with their cell.
		var holder := _world.build_ref(_world.get_ref_cell(ref), ref)
		if holder != null:
			_place.add(holder)


## Whether the place on screen is where reference `ref` is: its interior, or
## its worldspace. A script may enable a reference elsewhere (the Cidhna Mine
## silver ore, while outside); that is not drawn here, where its interior
## coordinates would put it in the sky.
func _in_place(ref: int) -> bool:
	var cell := _world.get_ref_cell(ref)
	var info := _world.get_cell(cell)
	if info.is_empty():
		return false
	if info["interior"]:
		return _streamer.world_id == 0 and cell == _place.cell_id
	return _streamer.world_id != 0 and info["world"] == _streamer.world_id


## Enable or disable a built reference: shown and solid, or neither (a
## disabled node's bodies leave the physics space).
func _show_ref(node: Node, enabled: bool) -> void:
	node.visible = enabled
	node.process_mode = Node.PROCESS_MODE_INHERIT if enabled else Node.PROCESS_MODE_DISABLED


## Clutter resting on or against `node` falls once it is gone or moves.
func _wake_around(node: Node) -> void:
	var bounds := Place.mesh_bounds(node)
	if bounds.size != Vector3.ZERO:
		SkydotWorld.wake_clutter(_host, bounds.get_center(), bounds.size.length() / 2 + 0.5)


func _clutter_body(ref: int) -> SkydotDynamicBody:
	var node := find_ref_node(ref)
	if node == null:
		return null
	var bodies := node.find_children("*", "SkydotDynamicBody", true, false)
	return bodies[0] if not bodies.is_empty() else null


## ApplyHavokImpulse: Skyrim's direction, Havok's magnitude (Havok units are
## metres here, so it applies as is).
func _on_havok_impulse(ref: int, direction: Vector3, magnitude: float) -> void:
	var body := _clutter_body(ref)
	if body == null:
		print("0x%08X has no movable body for an impulse" % ref)
		return
	body.wake()
	var godot_direction := SkydotWorld.skyrim_position(direction).normalized()
	body.apply_central_impulse(godot_direction * magnitude)


## SetMotionType: the moving types release clutter, keyframed and fixed hold
## it. Static models cannot be made to move.
func _on_motion_type(ref: int, motion_type: int) -> void:
	var body := _clutter_body(ref)
	if body == null:
		print("0x%08X has no movable body for motion type %d" % [ref, motion_type])
		return
	if motion_type in [4, 5]:
		body.freeze = true
	else:
		body.wake()


func _on_play_animation(ref: int, animation: String) -> void:
	var node := find_ref_node(ref)
	var animator := node.get_node_or_null("SkydotAnimator") if node != null else null
	if animator == null or not animator.play(animation):
		print("0x%08X has no %s animation" % [ref, animation])
		return
	_wake_around(node)
	# PlayAnimationAndWait waits for a text key or the clip's end.
	if not animator.has_meta("skydot_notifies"):
		animator.set_meta("skydot_notifies", true)
		animator.text_key.connect(func(_clip: String, key: String) -> void:
			_papyrus.notify_animation_event(ref, key))
		animator.finished.connect(func(clip: String) -> void:
			_papyrus.notify_animation_event(ref, clip))


## What activating reference `ref` in `cell` does. `node` is its model, if
## built. `parent` activations ignore the parent-only flag. Returns false if
## the reference could not be found.
func activate(cell: int, ref: int, node: Node, force: bool, parent := false) -> bool:
	var info := _world.get_ref_info(cell, ref)
	if info.is_empty():
		return false
	var label := "0x%08X %s (%s)" % [ref, info["editor_id"], info["type"]]
	if info["parent_activate_only"] and not parent:
		_debug.note(label + " only responds to its activate parents")
		return true
	var level := _papyrus.get_lock_level(ref)  # -1: not locked; 0 is Novice
	if level >= 0 and not force and not _settings.pick_locks:
		_debug.note(label + " is locked (level %d, %s); Shift+F opens it anyway" % [level, _lock_name(level)])
		return true
	if level >= 0:
		_papyrus.set_locked(ref, false)
	if _papyrus.activate(ref) > 0:
		print(label, " runs ", ", ".join(_papyrus.get_scripts(ref).map(func(s): return s["name"])))
	for child in _world.get_activate_children(ref):
		_host.get_tree().create_timer(child["delay"]).timeout.connect(func() -> void:
			activate(child["cell"], child["ref"], find_ref_node(child["ref"]), true, true))
	# A script that blocks activation handles it alone.
	if _papyrus.is_activation_blocked(ref):
		_debug.note(label + ": activation blocked, only its scripts ran")
		return true
	if info["door"] != null:
		print(label, " leads to 0x%08X" % info["door"]["destination"])
		_transition.go(info["door"])
		return true
	if info["type"] == "DOOR" and node != null:
		var animator := node.get_node_or_null("SkydotAnimator")
		if animator != null:
			var open: bool = not node.get_meta("skydot_open", false)
			if animator.play("Open" if open else "Close"):
				node.set_meta("skydot_open", open)
				node.remove_meta("skydot_opened_by")  # an actor no longer closes it
				_papyrus.set_open_state(ref, 1 if open else 3)
				print(label, " opens" if open else " closes")
				return true
	if _papyrus.get_scripts(ref).is_empty():
		_debug.note(label + ": nothing happens")
	return true


## XLOC's level as the game names it.
func _lock_name(level: int) -> String:
	if level >= 255:
		return "requires a key"
	for step in [[100, "Master"], [75, "Expert"], [50, "Adept"], [25, "Apprentice"]]:
		if level >= step[0]:
			return step[1]
	return "Novice"


## The model of reference `ref` among what is built, or null.
func find_ref_node(ref: int) -> Node:
	var pending: Array = _place.nodes.duplicate()
	pending.append_array(_streamer.get_loaded_cells())
	while not pending.is_empty():
		var node = pending.pop_back()
		if node == null or not is_instance_valid(node) or node.is_queued_for_deletion() or node == _streamer.lod:
			continue
		for child in node.get_children():
			if child.get_meta("skydot_ref", 0) == ref:
				return child
			if not child.has_meta("skydot_ref") and child.get_child_count() > 0:
				pending.append(child)
	return null


func has_activations() -> bool:
	return not _activations.is_empty()


## --activate: activate the next reference once its model is built; quit
## when all are done or one never appears.
func run_activation() -> void:
	if _activations.is_empty():
		return
	var ref: int = _activations[0]
	var node := find_ref_node(ref)
	if node == null:
		_wait += 1
		if _wait > 600:
			failed.emit("reference 0x%08X never appeared" % ref)
		return
	_wait = 0
	_activations.pop_front()
	activate(node.get_meta("skydot_cell"), ref, node, false)
	if _activations.is_empty():
		activations_finished.emit()


func activate_in_view(force: bool) -> void:
	var from := _camera.global_position
	var to := from - _camera.global_transform.basis.z * REACH
	var hit := _world.pick_ref(_host, from, to)
	if hit.is_empty():
		_debug.note("nothing to activate")
		return
	activate(hit["cell"], hit["ref"], hit["node"], force)
