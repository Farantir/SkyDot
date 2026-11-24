# SPDX-License-Identifier: GPL-3.0-or-later
#
# The VR shell (ADR 0006): a second shell around the same GameSession as the
# viewer, a spike of the milestone-2 boundary. It reads the viewer's game
# options (ViewerSettings), puts the player in a VrRig, shows the HUD, the
# subtitles, the dialogue and the game's menus (MenuHost) as panels (vr_panel.gd) and runs the frame.
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
# --start-quest, --talk, --activate, --quit-after, --no-input, --open-map (opens the world map), the --open-* menu
# options (MenuHost) and the quality
# ones); --screenshot FILE saves the window's picture one frame before the end
# (desktop fallback only). The viewer's shot series, benchmark and debug overlay are the
# viewer's.
#
# What both shells do the same (the commands, the game's signals, the start options, the
# frame's order) is ShellLoop; this shell keeps the XR session, the rig, the panels, the
# weapon in the hand and the death fade.
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
var _loop: ShellLoop
var _dialogue_panel: VrPanel
var _hud_panel: VrPanel
var _menus: MenuHost  # the game's menus, each on a panel put in front of the head when it opens
var _panels: Dictionary = {}  # GameMenu -> VrPanel
var _death: DeathView
var _death_panel: VrPanel
var _death_fade: MeshInstance3D  # a black quad on the head: the death fade and the door fade (max of the two)
var _door_fade := 0.0  # PlaceTransition.fade
var _xr: XRInterface
var _body: FirstPersonBody  # the weapon in the right hand (no arms in VR)


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
	# Only multisampling: with a runtime the XR interface owns the render size, so the viewer's
	# --render-scale (3D scaling) and --aa (screen-space AA over a scaled image) stay out.
	get_viewport().msaa_3d = ViewerSettings.MSAA_STEPS[_settings.msaa_index]
	_create_components()
	_start_game()
	_loop.arm_quit()


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
	_rig.inventory_requested.connect(_on_inventory_requested)
	_rig.magic_requested.connect(_on_magic_requested)
	_rig.skills_requested.connect(_on_skills_requested)
	_rig.favorites_requested.connect(_on_favorites_requested)
	_controls = PlayerInput.new(_rig)
	_controls.enabled = _settings.interactive
	_controls.look_speed = _settings.stick_look_speed
	_controls.invert_look_y = _settings.invert_look_y
	add_child(_controls)
	_loop = ShellLoop.new(self, _session, _rig, _controls, _settings, _image_space,
		func(text: String) -> void: print("note: ", text))
	# The viewer's debug keys (loop.command) are not here.
	_loop.quit_countdown.connect(_on_quit_countdown)
	_loop.quitting.connect(func() -> void: print("vr: quit"))
	# Minimal combat (ADR 0006): the weapon model in the right controller; a
	# swing faster than FirstPersonBody.SWING_SPEED is the attack. Only the
	# desktop fallback has been run (the controller stands still there: the
	# mouse button attacks as in the viewer).
	_body = FirstPersonBody.new(_session, null, _rig.right, _rig.left)
	add_child(_body)
	_session.magic.hand_node = _body.magic_node
	_session.magic.player_bone = _body.bone_node
	if _settings.hud:
		var hud := HudView.new(_session, _key_label)
		hud.show_crosshair = not _rig.tracking
		_hud_panel = _head_panel(hud, HUD_PIXELS, HUD_WIDTH, Vector3(0, -0.45, -1.5))
	if _settings.hud or not _settings.start_quests.is_empty():
		_head_panel(SubtitlesView.new(_session), HUD_PIXELS, HUD_WIDTH, Vector3(0, -0.75, -1.5))
	if _settings.hud or _settings.talk != 0:
		var dialogue := DialogueView.new(_session)
		dialogue.enabled = func() -> bool: return _controls.enabled
		dialogue.engaged.connect(_on_dialogue_engaged)
		_dialogue_panel = VrPanel.new(dialogue, MENU_PIXELS, MENU_WIDTH)
		_dialogue_panel.set_shown(false)
		add_child(_dialogue_panel)
	if _settings.wants_menus():
		# The keys of the desktop fallback open them as in the viewer (MenuHost.attach); in the headset the
		# controller buttons ask (VrRig).
		_menus = MenuHost.new(_session, _controls, _place_menu, _key_label)
		_menus.frees_mouse = not _rig.tracking
		_menus.attach(_loop)
		_menus.engaged.connect(_on_menu_engaged)
		# The item's model is a real object beside the panel, not a picture in it (ADR 0006, 0010).
		_menus.inventory_view.preview().use_world_node(_panels[_menus.inventory_view],
			Vector3(MENU_WIDTH * 0.5 + 0.25, 0.0, 0.05))
		_menus.crafting_view.can_name = false  # no keyboard: an enchanted item keeps its default name
		# The world map: the origin floats up over the map world (MapScene.origin), the movie's overlay on a panel.
		if MapView.available():
			_menus.map_view.scene = MapScene.new(_session, _rig.camera)
			_menus.map_view.scene.origin = _rig.origin
			add_child(_menus.map_view.scene)
			_loop.paused = func() -> bool: return _menus.map_view.is_open()
	_make_death_screen()
	if not _rig.tracking:  # the mouse looks only in the desktop fallback (the viewer always captures it)
		_controls.capture_mouse()


## The death (ui/death_view.gd): a black quad on the head fades in over 2 s and a panel in front of
## the head asks "Load the last save?" (Enter, A or the trigger loads; Esc quits).
func _make_death_screen() -> void:
	_death = DeathView.new(_session)
	_death.enabled = func() -> bool: return _controls.enabled
	_death.load_chosen.connect(func() -> void:
		if _loop.reload_after_death():
			_death.end()
			_death_panel.set_shown(false))
	_death.quit_chosen.connect(func() -> void: get_tree().quit(0))
	_death_panel = VrPanel.new(_death, MENU_PIXELS, MENU_WIDTH)
	_death_panel.set_shown(false)
	add_child(_death_panel)
	var quad := QuadMesh.new()
	quad.size = Vector2(4.0, 4.0)
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.no_depth_test = true
	material.render_priority = 9
	material.albedo_color = Color(0, 0, 0, 0)
	quad.material = material
	_death_fade = MeshInstance3D.new()
	_death_fade.mesh = quad
	_death_fade.position = Vector3(0, 0, -0.3)
	_death_fade.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_death_fade.visible = false
	_rig.camera.add_child(_death_fade)
	# The door fade is a CanvasLayer in the flat viewer, which a headset does not draw: the quad shows it.
	_session.transition.fade_changed.connect(func(alpha: float) -> void:
		_door_fade = alpha
		_update_fade())
	_loop.died.connect(func(_killer: int) -> void:  # the loop has blocked the controls
		_death_panel.recenter(_rig.camera)
		_death_panel.set_shown(true))


## A panel locked to the head.
func _head_panel(view: Control, pixels: Vector2i, width: float, at: Vector3) -> VrPanel:
	var panel := VrPanel.new(view, pixels, width)
	panel.position = at
	_rig.camera.add_child(panel)
	return panel


func _key_label(action: StringName) -> String:
	return "Trigger" if _rig.tracking and action == &"activate" else _controls.key_label(action)


## A menu (ui/) on a panel, hidden until it opens (MenuHost's `place`).
func _place_menu(view: GameMenu) -> void:
	var panel := VrPanel.new(view, MENU_PIXELS, MENU_WIDTH)
	panel.set_shown(false)
	add_child(panel)
	_panels[view] = panel


## A menu opened or closed: its panel comes up in front of the head, or goes.
func _on_menu_engaged(view: GameMenu, open: bool) -> void:
	var panel: VrPanel = _panels[view]
	if open and not panel.visible:
		panel.recenter(_rig.camera)
	panel.set_shown(open)


## The left menu button: the inventory, or closes the menu that is open.
func _on_inventory_requested() -> void:
	if _menus == null:
		print("vr: inventory (the HUD is off)")
		return
	var open := _menus.open_view()
	if open != null:
		open.close()
	else:
		_menus.open_inventory()


## The right stick click: the skills screen, or closes it.
func _on_skills_requested() -> void:
	if _menus == null:
		print("vr: skills (the HUD is off)")
		return
	_menus.toggle_skills()


## The right grip: the favorites menu; in the inventory or magic menu it marks the item under the cursor.
func _on_favorites_requested() -> void:
	if _menus == null:
		return
	if _menus.inventory_view.is_open() or _menus.magic_view.is_open():
		var press := InputEventAction.new()
		press.action = &"menu_favorite"
		press.pressed = true
		Input.parse_input_event(press)
	elif _menus.favorites_view.is_open():
		_menus.favorites_view.close()
	else:
		_menus.open_favorites()


## The left Y button: the magic menu, or closes it.
func _on_magic_requested() -> void:
	if _menus == null:
		print("vr: magic (the HUD is off)")
		return
	if _menus.magic_view.is_open():
		_menus.magic_view.close()
	else:
		_menus.open_magic()


func _on_dialogue_engaged(open: bool) -> void:
	_controls.blocked = open
	if open and not _dialogue_panel.visible:
		_dialogue_panel.recenter(_rig.camera)
	_dialogue_panel.set_shown(open)


## The same entering as the viewer's (ShellLoop.start), then the menus the options ask for;
## no shots or benchmark.
func _start_game() -> void:
	var settings := _settings
	if not _loop.start():
		return
	if _menus == null:
		return
	if settings.open_map != "":
		print("vr: map ", "open" if _menus.map_view.open() else "has no map here")
	_menus.open_start_menus(settings, _loop.in_world)


## The death screen lives in a SubViewport, which gets no keys: hand it the ones the shell gets.
func _unhandled_input(event: InputEvent) -> void:
	if _death != null and _death.is_asking():
		_death.handle_input(event)


func _process(delta: float) -> void:
	if _controls == null:
		return
	ImageSpaceSync.apply(get_viewport(), _settings, _session, _image_space)
	if _menus != null:
		if _hud_panel != null:
			_hud_panel.visible = not _menus.hides_hud()
		_body.menu_hidden = _menus.hides_body()
	if _loop.frame(delta, _device_step):
		_loop.drive(delta)


## What this shell's device does each frame before the game steps: the weapon follows the
## hand (the viewer's follows the camera), and the death fade darkens.
func _device_step(delta: float) -> void:
	_body.update_hand(delta)
	_update_fade()


## The black quad: the stronger of the death fade and the door fade.
func _update_fade() -> void:
	if _death_fade == null:
		return
	var alpha := maxf(_door_fade, _death.fade if _death.is_showing() else 0.0)
	_death_fade.visible = alpha > 0.0
	((_death_fade.mesh as QuadMesh).material as StandardMaterial3D).albedo_color = Color(0, 0, 0, alpha)


## The last frame of --quit-after: the mirror of the window, while it can still be read.
func _on_quit_countdown(frames_left: int) -> void:
	if frames_left == 0 and _settings.has_screenshot:
		_save_mirror(_settings.screenshot)


## --screenshot: the window's picture one frame before the run ends (in the
## desktop fallback; the window of a running session stays black).
func _save_mirror(path: String) -> void:
	var image := get_viewport().get_texture().get_image()
	print("vr: mirror ", image.get_size(), " -> ", path, " ", image.save_png(path))


func _fail(message: String) -> void:
	push_error(message)
	printerr(message)
	get_tree().quit(1)
