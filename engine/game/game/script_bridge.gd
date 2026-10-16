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
## Something to tell the user: a script's message or a quest note, what an
## activation says.
signal message(text: String, box: bool)
signal quest_started(quest: int)
signal quest_stage(quest: int, stage: int, text: String)
signal objective_changed(quest: int, index: int, state: String, text: String)

## After the start-game quests have started: quest changes print from then on.
var quests_ready := false
var _activations: Array[int] = []  # --activate: refs still to activate
var _wait := 0  # frames the first of them has been missing
var _gap := 0  # frames to pass before the next of them (--activate-wait)
var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _host: Node3D  # the place's root, where picking and waking clutter start
var _viewpoint: Camera3D
var _config: GameConfig
var _place: Place
var _streamer: SkydotStreamer
var _transition: PlaceTransition
var _sounds: Sounds
var _damages: Array[Vector2] = []


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, host: Node3D, viewpoint: Camera3D,
		config: GameConfig, place: Place, streamer: SkydotStreamer, transition: PlaceTransition,
		sounds: Sounds) -> void:
	_world = world
	_papyrus = papyrus
	_host = host
	_viewpoint = viewpoint
	_config = config
	_place = place
	_streamer = streamer
	_transition = transition
	_sounds = sounds
	_activations.assign(config.activate)
	_damages.assign(config.damage)
	_papyrus.enable_changed.connect(_on_enable_changed)
	_papyrus.ref_moved.connect(_on_ref_moved)
	_papyrus.play_animation.connect(_on_play_animation)
	_papyrus.havok_impulse.connect(_on_havok_impulse)
	_papyrus.motion_type_changed.connect(_on_motion_type)
	_papyrus.activation_outcome.connect(_on_activation_outcome)
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
	_resume_graphs(root)
	if cell_id != 0:
		_sounds.cell_loaded(root as Node3D, cell_id, Place.ref_nodes(root))
		_papyrus.attach_cell(cell_id)
	var scripted := _papyrus.attach_built(root)
	if scripted > 0:
		print("scripts: %d references in %s" % [scripted, root.name])
	# Every change scripts have made so far: apply those to this cell's
	# references in one walk over it, not a search of the scene for each.
	var changes := _papyrus.get_disabled_changes()
	var moves := _papyrus.get_moved_changes()
	if not changes.is_empty() or not moves.is_empty():
		for node in Place.ref_nodes(root):
			var ref: int = node.get_meta("skydot_ref")
			if changes.has(ref):
				_show_ref(node, not changes[ref])
			if moves.has(ref):
				_move_ref(node as Node3D, moves[ref][0], moves[ref][1])


## A cell went away (streamed out): its scripted references unload.
func scripts_unloaded(cell_id: int) -> void:
	_report_unloaded(_papyrus.detach_cell(cell_id))


## The place on screen is about to go: everything loaded unloads.
func place_left() -> void:
	_report_unloaded(_papyrus.detach_all())


func _report_unloaded(count: int) -> void:
	if count > 0:
		print("scripts: %d references unloaded" % count)


## Put the behaviour graphs of what was just built where the saves left them
## (an opened gate stays open), before scripts see the references, and keep
## the store current when a graph changes or its cell unloads.
func _resume_graphs(root: Node) -> void:
	for node in Place.ref_nodes(root):
		var player := node.find_child("SkydotBehavior", true, false)
		if player == null:
			continue
		var ref: int = node.get_meta("skydot_ref")
		var bytes := _papyrus.get_animation_state(ref)
		if not bytes.is_empty() and not player.restore_snapshot(bytes):
			print("0x%08X: its saved graph state does not fit the graph, so it starts over" % ref)
		player.state_changed.connect(_record_graph.bind(ref, player))
		player.leaving.connect(_record_graph.bind(ref, player))


func _record_graph(ref: int, player: Node) -> void:
	_papyrus.set_animation_state(ref, player.get_snapshot())


## Record every live graph (for a save).
func record_graphs() -> void:
	for player in Engine.get_main_loop().get_nodes_in_group("skydot_behaviors"):
		_record_graph(player.get_ref(), player)


## The live graphs are about to be replaced by a loaded save: what they hold
## is stale and must not reach the store when their cell unloads.
func retire_graphs() -> void:
	for player in Engine.get_main_loop().get_nodes_in_group("skydot_behaviors"):
		player.retire()


func _on_open_changed(ref: int, open: bool) -> void:
	var node := find_ref_node(ref)
	print("0x%08X %s by script" % [ref, "opened" if open else "closed"])
	if node != null:
		node.set_meta("skydot_open", open)
		_on_play_animation(ref, "Open" if open else "Close")


func _on_lock_changed(ref: int, locked: bool) -> void:
	print("0x%08X %s by script" % [ref, "locked" if locked else "unlocked"])


func _on_message(text: String, box: bool) -> void:
	message.emit(text, box)


## A quest's name as the game shows it, else its editor id.
func quest_name(quest: int) -> String:
	var info := _world.get_quest(quest)
	var title: String = info.get("name", "")
	return title if title != "" else info.get("editor_id", "0x%08X" % quest)


func _on_quest_started(quest: int) -> void:
	if quests_ready:
		print("quest started: ", quest_name(quest))
	quest_started.emit(quest)


## Only stages with journal text reach the screen, as in the game.
func _on_quest_stage(quest: int, stage: int, text: String) -> void:
	if text != "":
		message.emit("%s: %s" % [quest_name(quest), text], false)
	elif quests_ready:
		print("%s: stage %d" % [quest_name(quest), stage])
	quest_stage.emit(quest, stage, text)


func _on_objective_changed(quest: int, index: int, state: String, text: String) -> void:
	message.emit("%s objective %d %s: %s" % [quest_name(quest), index, state, text], false)
	objective_changed.emit(quest, index, state, text)


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


## ObjectReference.Reset: the built node goes to the position and rotation
## (Skyrim space, radians), keeping its scale. A reference not built moves
## in the VM's view only.
func _on_ref_moved(ref: int, position: Vector3, rotation: Vector3) -> void:
	print("0x%08X moved by script" % ref)
	_move_ref(find_ref_node(ref) as Node3D, position, rotation)


## Put a built node at a position and rotation (Skyrim space, radians),
## keeping its scale.
func _move_ref(node: Node3D, position: Vector3, rotation: Vector3) -> void:
	if node != null:
		node.transform = _world.skyrim_transform(position, rotation, node.scale.x)


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
	if node == null:
		print("0x%08X is not built, so it plays no %s animation" % [ref, animation])
	elif not _play_event(node, ref, animation):
		var graph := node.find_child("SkydotBehavior", true, false)
		if graph != null:
			print("0x%08X: its behaviour graph (state %s) and its clips ignore %s" % [ref, graph.get_state(), animation])
		else:
			print("0x%08X has no %s animation" % [ref, animation])


## The one way an animation event reaches a placed model: its behaviour
## graph, else the NIF clip of that name. What the model then raises (a text
## key, the clip's end, a graph event) releases scripts waiting in
## PlayAnimationAndWait. False if nothing played.
func _play_event(node: Node, ref: int, event: String) -> bool:
	_connect_notifies(node, ref)
	if not SkydotBehavior.play_event(node, event):
		return false
	_wake_around(node)
	return true


func _connect_notifies(node: Node, ref: int) -> void:
	if node.has_meta("skydot_notifies"):
		return
	node.set_meta("skydot_notifies", true)
	var animator := node.get_node_or_null("SkydotAnimator")
	if animator != null:
		animator.text_key.connect(func(_clip: String, key: String) -> void:
			_papyrus.notify_animation_event(ref, key))
		animator.finished.connect(func(clip: String) -> void:
			_papyrus.notify_animation_event(ref, clip))
	var player := node.find_child("SkydotBehavior", true, false)
	if player != null:
		player.event_raised.connect(func(name: String) -> void:
			_papyrus.notify_animation_event(ref, name))


## The player's activation of reference `ref`: SkydotPapyrus.activate_ref
## decides (parent-only, locks, scripts, activate children) and this applies
## what it asks of the scene. `force` picks locks. Returns false if the
## reference could not be found.
func activate(_cell: int, ref: int, _node: Node, force: bool) -> bool:
	var outcome := _papyrus.activate_ref(ref, SkydotPapyrus.PLAYER_REF, false,
		SkydotPapyrus.ACTIVATE_PLAYER, force or _config.pick_locks)
	if outcome["kind"] == "missing":
		return false
	_apply_outcome(outcome, force)
	return true


## A script's Activate or an activate child, once the VM has run it.
func _on_activation_outcome(_ref: int, outcome: Dictionary) -> void:
	_apply_outcome(outcome, false)


## Do what an activation outcome asks of the scene and say what it noted.
func _apply_outcome(outcome: Dictionary, picked := false) -> void:
	var ref: int = outcome["ref"]
	var label: String = outcome.get("label", "")
	var node := find_ref_node(ref)
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
			if node != null and _play_event(node, ref, outcome["event"]):
				return
			# The graph will not raise ObjectActivated: the use counts now.
			if outcome.get("deferred", false):
				_papyrus.complete_activation(ref)
		"toggle_open":
			if node != null and _toggle_open(node, ref, label):
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


## The model of reference `ref` among what is built, or null.
func find_ref_node(ref: int) -> Node:
	return _world.find_ref_node(ref)


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
	var node := find_ref_node(ref)
	if node == null:
		_wait += 1
		if _wait > 600:
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


func activate_in_view(force: bool) -> void:
	var from := _viewpoint.global_position
	var to := from - _viewpoint.global_transform.basis.z * REACH
	var hit := _world.pick_ref(_host, from, to)
	if hit.is_empty():
		message.emit("nothing to activate", false)
		return
	activate(hit["cell"], hit["ref"], hit["node"], force)
