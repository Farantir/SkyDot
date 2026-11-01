# SPDX-License-Identifier: GPL-3.0-or-later
#
# Between SkydotPapyrus and the scene: what scripts enable, disable, open,
# animate, push and say shows up in the built nodes and on screen, and what
# the player or a script activates (locks, load doors, plain doors, scripts)
# is done here. Also the --activate runs: activate references in turn, each
# in the place the previous one led to. It wires the signals of SkydotPapyrus to
# one class each for graph persistence (ScriptGraphs), activation
# (ScriptActivation), animation events (ScriptAnimationEvents) and clutter
# physics (ClutterPhysics), and keeps what is left: enabling, moving, quests.
class_name ScriptBridge
extends RefCounted

const REACH := ScriptActivation.REACH  # metres the camera can activate from

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
## The player activated an actor that can talk, and nothing else came of it.
signal talk_requested(ref: int)
## The player activated a container (a CONT, lock gate passed): its menu.
signal container_opened(ref: int)
## The player activated a workbench (a FURN with a bench type): its menu.
signal crafting_opened(ref: int, bench_type: int)

## After the start-game quests have started: quest changes print from then on.
var quests_ready := false
var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _viewpoint: Camera3D
var _place: Place
var _streamer: SkydotStreamer
var _sounds: Sounds
var _graphs: ScriptGraphs
var _clutter: ClutterPhysics
var _animation: ScriptAnimationEvents
var _activation: ScriptActivation


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, host: Node3D, viewpoint: Camera3D,
		config: GameConfig, place: Place, streamer: SkydotStreamer, transition: PlaceTransition,
		sounds: Sounds) -> void:
	_world = world
	_papyrus = papyrus
	_viewpoint = viewpoint
	_place = place
	_streamer = streamer
	_sounds = sounds
	_graphs = ScriptGraphs.new(papyrus)
	_clutter = ClutterPhysics.new(world, host)
	_animation = ScriptAnimationEvents.new(world, papyrus, _clutter)
	_activation = ScriptActivation.new(world, papyrus, host, config, transition, sounds, _animation)
	_activation.activations_finished.connect(activations_finished.emit)
	_activation.failed.connect(failed.emit)
	_activation.message.connect(message.emit)
	_activation.talk_requested.connect(talk_requested.emit)
	_activation.container_opened.connect(container_opened.emit)
	_activation.crafting_opened.connect(crafting_opened.emit)
	_papyrus.enable_changed.connect(_on_enable_changed)
	_papyrus.ref_moved.connect(_on_ref_moved)
	_papyrus.play_animation.connect(_animation.on_play_animation)
	_papyrus.animation_events_wanted.connect(_animation.on_animation_events_wanted)
	_papyrus.havok_impulse.connect(_clutter.on_havok_impulse)
	_papyrus.motion_type_changed.connect(_clutter.on_motion_type)
	_papyrus.activation_outcome.connect(_activation.on_activation_outcome)
	_papyrus.open_changed.connect(_on_open_changed)
	_papyrus.lock_changed.connect(_on_lock_changed)
	_papyrus.message.connect(_on_message)
	_papyrus.quest_started.connect(_on_quest_started)
	_papyrus.quest_stage.connect(_on_quest_stage)
	_papyrus.objective_changed.connect(_on_objective_changed)
	_papyrus.effect_shader.connect(_on_effect_shader)
	_papyrus.visual_effect.connect(_on_visual_effect)
	_papyrus.sub_graph_animation.connect(_animation.on_sub_graph_animation)
	_papyrus.trigger.connect(_on_trigger)


## Attach the scripts of what was just built (OnInit once, OnLoad each time),
## including model-less ones such as triggers, and show script-made changes to
## what is enabled.
func scripts_loaded(root: Node, cell_id: int) -> void:
	_graphs.resume(root)
	if cell_id != 0:
		_sounds.cell_loaded(root as Node3D, cell_id, Place.ref_nodes(root))
		_papyrus.attach_cell(cell_id)
	var scripted := _papyrus.attach_built(root)
	if scripted > 0:
		print("scripts: %d references in %s" % [scripted, root.name])
	show_harvests(root)
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


## Plants and trees harvested so far show their harvested model (child 1 of
## the model's switch node) in what was just built; the rest keep child 0. A
## respawned one is not listed, so a rebuild shows it whole again.
func show_harvests(root: Node) -> void:
	for ref in _papyrus.get_harvested_refs():
		var node := find_ref_node(ref)
		if node != null and (node == root or root.is_ancestor_of(node)):
			SkydotSwitch.set_index(node, 1)


## A cell went away (streamed out): its scripted references unload.
func scripts_unloaded(cell_id: int) -> void:
	_report_unloaded(_papyrus.detach_cell(cell_id))


## The place on screen is about to go: everything loaded unloads.
func place_left() -> void:
	_report_unloaded(_papyrus.detach_all())


func _report_unloaded(count: int) -> void:
	if count > 0:
		print("scripts: %d references unloaded" % count)


func _on_open_changed(ref: int, open: bool) -> void:
	var node := find_ref_node(ref)
	print("0x%08X %s by script" % [ref, "opened" if open else "closed"])
	if node != null:
		node.set_meta("skydot_open", open)
		_animation.on_play_animation(ref, "Open" if open else "Close")


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


func _on_visual_effect(effect: int, ref: int, target: int, seconds: float, playing: bool) -> void:
	if playing:
		print("visual effect 0x%08X plays on 0x%08X toward 0x%08X for %.1f s (placeholder)" % [effect, ref, target, seconds])
	else:
		print("visual effect 0x%08X stops on 0x%08X" % [effect, ref])


func _on_trigger(ref: int, actor: int, entered: bool) -> void:
	var info := _world.get_ref_info(_world.get_ref_cell(ref), ref)
	var at: Vector3 = info.get("position", Vector3.ZERO)
	print("%s trigger 0x%08X (actor 0x%X, trigger at %.0f, %.0f, %.0f)" % ["entered" if entered else "left", ref, actor, at.x, at.y, at.z])


func _on_enable_changed(ref: int, enabled: bool) -> void:
	print("0x%08X %s by script" % [ref, "enabled" if enabled else "disabled"])
	var node := find_ref_node(ref)
	if node != null:
		if not enabled:
			_clutter.wake_around(node)
		_show_ref(node, enabled)
	elif enabled and _place != null and _place.enable_actor(ref):
		# An actor the editor placed disabled: built now, with the scripts of its base (a far one has no cell
		# to attach them).
		var built := find_ref_node(ref)
		if built != null:
			var count := _papyrus.attach_built(built.get_parent() if built.get_parent() != null else built)
			print("0x%08X built after Enable: %d scripted, scripts %s" % [ref, count, _papyrus.get_scripts(ref).map(func(x: Dictionary) -> String: return x["name"])])
	elif enabled and _in_place(ref):
		# Initially disabled references are not built with their cell.
		var holder := _world.build_ref(_world.get_ref_cell(ref), ref)
		if holder != null:
			_place.add(holder)


## ObjectReference.Reset, MoveTo: the built node goes to the position and rotation
## (Skyrim space, radians), keeping its scale. A reference not built moves
## in the VM's view only. An actor goes through the AI: it forgets where it
## walked to, is built where it went if that is the place on screen, and its
## node is freed if it went elsewhere.
func _on_ref_moved(ref: int, position: Vector3, rotation: Vector3) -> void:
	print("0x%08X moved by script to (%.0f, %.0f, %.0f)" % [ref, position.x, position.y, position.z])
	var node := find_ref_node(ref) as Node3D
	if _place != null:
		var outcome := _place.relocate_actor(ref, position)
		if outcome == 0 or outcome == 2:
			return  # built there by the place, or gone with its node
	_move_ref(node, position, rotation)


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


## The model of reference `ref` among what is built, or null.
func find_ref_node(ref: int) -> Node:
	return _world.find_ref_node(ref)


## Record every live graph (for a save).
func record_graphs() -> void:
	_graphs.record_all()


## The live graphs are about to be replaced by a loaded save: what they hold
## is stale and must not reach the store when their cell unloads.
func retire_graphs() -> void:
	_graphs.retire_all()


## The player's activation of reference `ref` (see ScriptActivation.activate).
## Returns false if the reference could not be found.
func activate(cell: int, ref: int, node: Node, force: bool) -> bool:
	return _activation.activate(cell, ref, node, force)


func has_activations() -> bool:
	return _activation.has_activations()


## --activate: activate the next reference once its model is built; quit
## when all are done or one never appears.
func run_activation() -> void:
	_activation.run_activation()


## Activates what the viewpoint looks at. False if there was nothing known to activate.
func activate_in_view(force: bool, pointer: Node3D) -> bool:
	return _activation.activate_in_view(force, pointer)
