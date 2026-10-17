# SPDX-License-Identifier: GPL-3.0-or-later
#
# The VR shell (ADR 0006): a second shell around the same GameSession as the
# viewer, a spike of the milestone-2 boundary. It reads the viewer's game
# options (ViewerSettings), puts the player in a VrRig, shows the HUD, the
# subtitles and the dialogue menu as panels (vr_panel.gd) and runs the frame.
#
#   godot4.7 --path game --xr-mode on res://vr/vr_shell.tscn -- \
#       --pack DIR --cell EDITOR_ID [--quit-after FRAMES] [--no-input]
#
# OpenXR is off in project.godot, so the flat viewer never touches XR;
# `--xr-mode on` starts it for this shell. With no runtime (or without the
# flag) it prints "vr: no OpenXR runtime, desktop fallback" and runs with the
# XRCamera3D as a plain camera: WASD and the mouse as in the viewer.
#
# Takes the options that put the player somewhere and run the game (--pack,
# --cell, --world, --at, --target, --look, --time, --load, --give,
# --start-quest, --talk, --activate, --quit-after, --no-input and the quality
# ones); --screenshot FILE saves the window's picture one frame before the end
# (desktop fallback only). The viewer's shot series, benchmark and debug overlay are the
# viewer's.
extends Node3D

const SESSION_SIGNALS: Array[StringName] = [&"session_begun", &"session_visible", &"session_focussed",
	&"session_stopping", &"session_loss_pending"]
const FALLBACK_LINE := "vr: no OpenXR runtime, desktop fallback"
## Panel pixels and widths (metres, 1.5 m from the head).
const HUD_PIXELS := Vector2i(1280, 720)
const HUD_WIDTH := 1.6
const MENU_PIXELS := Vector2i(1280, 800)
const MENU_WIDTH := 1.6

var _settings: ViewerSettings
var _session: GameSession
var _image_space: SkydotImageSpace
var _rig: VrRig
var _controls: PlayerInput
var _dialogue_panel: VrPanel
var _inventory: InventoryView
var _container: ContainerView
var _crafting: CraftingView
var _inventory_panel: VrPanel
var _container_panel: VrPanel
var _crafting_panel: VrPanel
var _xr: XRInterface
var _quit_in := -1  # frames until quitting after --activate


func _ready() -> void:
	_settings = ViewerSettings.from_arguments(OS.get_cmdline_user_args())
	if not _settings.error.is_empty():
		_fail(_settings.error)
		return
	var xr_on := _start_xr()
	_image_space = SkydotImageSpace.new()
	_rig = VrRig.new(_settings, _image_space)
	_rig.set_tracking(xr_on)
	add_child(_rig.origin)
	add_child(_rig.player)
	_session = GameSession.new()
	_session.name = "GameSession"
	_session.failed.connect(_fail)
	add_child(_session)
	_session.interactive = _settings.interactive
	if not _session.open(_settings.game_config(), _rig.camera, _rig):
		return
	# The right controller's aim is what activates; a plain camera looks.
	_session.set_pointer(_rig.right if xr_on else _rig.camera)
	get_viewport().msaa_3d = ViewerSettings.MSAA_STEPS[_settings.msaa_index]
	_create_components()
	_start_game()
	if _settings.quit_after >= 0 and not _session.has_activations():
		_quit_in = _settings.quit_after


## Start the OpenXR session if a runtime answers. True when one runs.
func _start_xr() -> bool:
	_xr = XRServer.find_interface("OpenXR")
	if _xr != null and (_xr.is_initialized() or _xr.initialize()):
		var viewport := get_viewport()
		viewport.use_xr = true
		# The runtime paces the frames.
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
		print("vr: OpenXR session started")
		if _xr is OpenXRInterface:
			for state in SESSION_SIGNALS:
				_xr.connect(state, _on_session_state.bind(state))
		return true
	print(FALLBACK_LINE)
	return false


## The session's states, logged. Connected to a method, and disconnected when
## the shell leaves the tree: a lambda left on the interface (it outlives the
## shell) corrupts the heap at exit.
func _on_session_state(state: StringName) -> void:
	print("vr: ", String(state).replace("_", " "))


func _exit_tree() -> void:
	if _xr is OpenXRInterface:
		for state in SESSION_SIGNALS:
			for connection in _xr.get_signal_connection_list(state):
				_xr.disconnect(state, connection["callable"])


func _create_components() -> void:
	_session.message.connect(func(text: String, _box: bool) -> void: print("note: ", text))
	_session.activations_finished.connect(func() -> void:
		_quit_in = _settings.quit_after if _settings.quit_after >= 0 else 5)
	_rig.fell_through.connect(func() -> void: print("note: fell through the world"))
	_rig.inventory_requested.connect(_on_inventory_requested)
	_controls = PlayerInput.new(_rig)
	_controls.enabled = _settings.interactive
	_controls.look_speed = _settings.stick_look_speed
	_controls.invert_look_y = _settings.invert_look_y
	_controls.command.connect(_on_command)
	add_child(_controls)
	if _settings.hud:
		var hud := HudView.new(_session, _key_label)
		hud.show_crosshair = not _rig.tracking
		_head_panel(hud, HUD_PIXELS, HUD_WIDTH, Vector3(0, -0.45, -1.5))
	if _settings.hud or not _settings.start_quests.is_empty():
		_head_panel(SubtitlesView.new(_session), HUD_PIXELS, HUD_WIDTH, Vector3(0, -0.75, -1.5))
	if _settings.hud or _settings.talk != 0:
		var dialogue := DialogueView.new(_session)
		dialogue.enabled = func() -> bool: return _controls.enabled
		dialogue.engaged.connect(_on_dialogue_engaged)
		_dialogue_panel = VrPanel.new(dialogue, MENU_PIXELS, MENU_WIDTH)
		_dialogue_panel.set_shown(false)
		add_child(_dialogue_panel)
	if _settings.hud or _settings.open_inventory or _settings.open_container != 0:
		_inventory = InventoryView.new(_session)
		_container = ContainerView.new(_session)
		_inventory_panel = _menu_panel(_inventory)
		_container_panel = _menu_panel(_container)
		_crafting = CraftingView.new(_session)
		_crafting_panel = _menu_panel(_crafting)
		_session.container_opened.connect(_on_container_opened)
		_session.crafting_opened.connect(_on_crafting_opened)
	if not _rig.tracking:
		_controls.capture_mouse()


## A panel locked to the head.
func _head_panel(view: Control, pixels: Vector2i, width: float, at: Vector3) -> void:
	var panel := VrPanel.new(view, pixels, width)
	panel.position = at
	_rig.camera.add_child(panel)


func _key_label(action: StringName) -> String:
	return "Trigger" if _rig.tracking and action == &"activate" else _controls.key_label(action)


## An item menu (ui/) on a panel put in front of the head when it opens.
func _menu_panel(view: Control) -> VrPanel:
	view.enabled = func() -> bool: return _controls.enabled
	view.key_label = _key_label
	view.engaged.connect(_on_menu_engaged.bind(view))
	var panel := VrPanel.new(view, MENU_PIXELS, MENU_WIDTH)
	panel.set_shown(false)
	add_child(panel)
	return panel


func _on_menu_engaged(open: bool, view: Control) -> void:
	var panel := _inventory_panel
	if view == _container:
		panel = _container_panel
	elif view == _crafting:
		panel = _crafting_panel
	_controls.blocked = open or _inventory.is_open() or _container.is_open() or _crafting.is_open()
	if open and not panel.visible:
		panel.recenter(_rig.camera)
	panel.set_shown(open)


## The left menu button: the inventory, or closes the menu that is open.
func _on_inventory_requested() -> void:
	if _inventory == null:
		print("vr: inventory (the HUD is off)")
		return
	if _inventory.is_open():
		_inventory.close()
	elif _container.is_open():
		_container.close()
	elif _crafting.is_open():
		_crafting.close()
	elif not _session.is_dialogue_open():
		_inventory.open()


## Activating a container opens its menu.
func _on_container_opened(ref: int) -> void:
	if _session.is_dialogue_open():
		return
	_inventory.close()
	_container.open(ref)


## Activating a workbench opens its menu.
func _on_crafting_opened(ref: int, bench_type: int) -> void:
	if _session.is_dialogue_open():
		return
	_inventory.close()
	_container.close()
	_crafting.open(ref, bench_type)


func _on_dialogue_engaged(open: bool) -> void:
	_controls.blocked = open
	if open and not _dialogue_panel.visible:
		_dialogue_panel.recenter(_rig.camera)
	_dialogue_panel.set_shown(open)


## The same entering as the viewer's, without shots and benchmark.
func _start_game() -> void:
	var settings := _settings
	if not _session.start_game():
		return
	if not settings.world.is_empty():
		if not settings.has_at:
			_fail("--world needs --at X,Y,Z")
			return
		var at := SkydotWorld.skyrim_position(settings.at)
		var target = SkydotWorld.skyrim_position(settings.target) if settings.has_target else null
		if not _session.enter_world(settings.world, at, target):
			return
		_session.give(settings.give)
		_session.start_quests(settings.start_quests)
		if settings.open_container != 0 and not _session.activate_ref(settings.open_container):
			print("--open-container: 0x%08X cannot be activated" % settings.open_container)
		if settings.has_look:
			_rig.apply_look(settings.look_yaw, settings.look_pitch)
		return
	var entered := false
	if settings.has_at and settings.has_target:
		entered = _session.enter_cell(settings.cell, SkydotWorld.skyrim_position(settings.at),
			SkydotWorld.skyrim_position(settings.target))
	else:
		entered = _session.enter_cell(settings.cell,
			SkydotWorld.skyrim_position(settings.at) if settings.has_at else null, null)
	if not entered:
		return
	if settings.has_look:
		_rig.apply_look(settings.look_yaw, settings.look_pitch)
	if not settings.load_path.is_empty():
		_session.load_game(settings.load_path)
	_session.give(settings.give)
	_session.start_quests(settings.start_quests)
	if settings.talk != 0 and not _session.dialogue_open(settings.talk):
		print("--talk: 0x%08X cannot talk" % settings.talk)
	if settings.open_container != 0 and not _session.activate_ref(settings.open_container):
		print("--open-container: 0x%08X cannot be activated" % settings.open_container)
	if settings.open_inventory and _inventory != null:
		_inventory.open()


func _process(delta: float) -> void:
	if _controls == null:
		return
	ImageSpaceSync.apply(get_viewport(), _settings, _session, _image_space)
	_session.streamer.hold_player(_rig.player, _session.is_fading())
	_rig.track()
	_rig.follow()
	_session.step(delta, _rig.player.global_position)
	if _quit_in >= 0:
		_quit_in -= 1
		if _quit_in == 0 and _settings.has_screenshot:
			_save_mirror(_settings.screenshot)
		if _quit_in < 0:
			if not _settings.save_to.is_empty():
				_session.save_game(_settings.save_to)
			print("vr: quit")
			get_tree().quit(0)
		return
	if _session.has_activations():
		_session.run_activation()
		return
	_controls.apply(delta)


## --screenshot: the window's picture one frame before the run ends (in the
## desktop fallback; the window of a running session stays black).
func _save_mirror(path: String) -> void:
	var image := get_viewport().get_texture().get_image()
	print("vr: mirror ", image.get_size(), " -> ", path, " ", image.save_png(path))


## A key that is not about the player's body; the few the shell has a use for.
func _on_command(action: StringName, shift: bool) -> void:
	match action:
		PlayerInput.ACTIVATE:
			_session.activate_in_view(shift)
		PlayerInput.QUICK_SAVE:
			_session.save_game(SaveService.QUICKSAVE)
		PlayerInput.QUICK_LOAD:
			_session.load_game(SaveService.QUICKSAVE)
		PlayerInput.TOGGLE_FLY:
			print("note: ", "flying" if _rig.toggle_fly() else "walking")


func _fail(message: String) -> void:
	push_error(message)
	printerr(message)
	get_tree().quit(1)
