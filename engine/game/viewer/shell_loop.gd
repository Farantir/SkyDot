# SPDX-License-Identifier: GPL-3.0-or-later
#
# What both shells (the flat viewer, the VR shell) do the same around a GameSession: the
# commands every player has, the signals of the game (messages, falling through the world,
# leaving a place, death), the start options, and the frame's order. A shell makes one, hands
# it its camera rig, its input device and a way to show notes, and keeps only what is its own:
# the camera and device, panels or layers, shots, benchmark, debug overlay, XR.
#
#   frame():  start options' waits, streamer hold, rig track and follow, aiming options, the
#             shell's device step, GameSession.step, quit countdown, activations. True when
#             nothing else keeps the player from being driven.
#   drive():  paralysis to the controls, then the controls' movement.
#
# Keys that are not about the player's body but are not shared (the viewer's debug keys) come
# back through `command`.
class_name ShellLoop
extends RefCounted

## An action the shell handles itself (the loop did not).
signal command(action: StringName, shift: bool)
## I and M: the shell opens (or closes) its menu.
signal inventory_requested
signal magic_requested
signal skills_requested
## The player died; the controls are blocked. The shell shows the death: its camera fall or fade.
signal died(killer: int)
## Each frame of the quit countdown, after it counted down (0: the last frame of the run).
signal quit_countdown(frames_left: int)
## The run ends now (after --save-to).
signal quitting

## The start and test options that act on the game, and their steps.
var options: StartOptions
## Frames until quitting after --activate or --quit-after; -1 not counting.
var quit_in := -1
## Whether start() entered a worldspace (not an interior).
var in_world := false

var _host: Node
var _session: GameSession
var _rig: PlayerRig
var _controls: PlayerInput
var _settings: ViewerSettings
var _image_space: SkydotImageSpace
var _note: Callable  # (text: String): where the shell shows a note


func _init(host: Node, session: GameSession, rig: PlayerRig, controls: PlayerInput, settings: ViewerSettings,
		image_space: SkydotImageSpace, note: Callable) -> void:
	_host = host
	_session = session
	_rig = rig
	_controls = controls
	_settings = settings
	_image_space = image_space
	_note = note
	options = StartOptions.new(settings, session, rig)
	controls.command.connect(_on_command)
	controls.attack_changed.connect(_on_attack_changed)
	controls.attack_left_changed.connect(_on_attack_left_changed)
	controls.voice_changed.connect(_on_voice_changed)
	session.message.connect(_on_message)
	session.activations_finished.connect(_on_activations_finished)
	session.place_left.connect(image_space.reset_adaptation)
	session.player_died.connect(_on_player_died)
	rig.fell_through.connect(_on_fell_through)


## Start the game and enter the place the options name. False if it does not (a failure is
## reported); the shell then shows its menus and begins its shots or benchmark.
func start() -> bool:
	var settings := _settings
	if not _session.start_game():
		return false
	for entry in settings.damage_actor:
		_session.papyrus.damage_actor_value(entry["ref"], entry["av"], entry["amount"])
	in_world = not settings.world.is_empty()
	if in_world:
		if not settings.has_at:
			fail("--world needs --at X,Y,Z")
			return false
		var at := SkydotWorld.skyrim_position(settings.at)
		var target = SkydotWorld.skyrim_position(settings.target) if settings.has_target else null
		if not _session.enter_world(settings.world, at, target):
			return false
		_controls.fly_speed = 10.0
	else:
		var entered := false
		if settings.has_at and settings.has_target:
			entered = _session.enter_cell(settings.cell, SkydotWorld.skyrim_position(settings.at),
				SkydotWorld.skyrim_position(settings.target))
		else:
			entered = _session.enter_cell(settings.cell,
				SkydotWorld.skyrim_position(settings.at) if settings.has_at else null, null)
		if not entered:
			return false
	if settings.has_look:
		_rig.apply_look(settings.look_yaw, settings.look_pitch)
	if not settings.load_path.is_empty():
		_session.load_game(settings.load_path)
	_session.give(settings.give)
	options.apply()
	_session.start_quests(settings.start_quests)
	if settings.talk != 0 and not _session.dialogue_open(settings.talk):
		print("--talk: 0x%08X cannot talk" % settings.talk)
	if settings.open_container != 0 and not _session.activate_ref(settings.open_container):
		print("--open-container: 0x%08X cannot be activated" % settings.open_container)
	return true


## Count down to the quit from the start when --quit-after is given and nothing waits to be
## activated (otherwise the countdown starts when the activations are done).
func arm_quit() -> void:
	if _settings.quit_after >= 0 and not _session.has_activations():
		quit_in = _settings.quit_after


## One frame of the game, in this order. `device_step` (Callable(delta)) is what the shell's input
## device does after the player and camera are placed and before the game steps (the weapon
## follows the camera or the hand); `hold_streaming` holds the streamer's work (a benchmark).
## False when the shell must not drive the player this frame: the run is quitting or an
## --activate is running.
func frame(delta: float, device_step: Callable, hold_streaming := false) -> bool:
	options.step(delta)
	_session.streamer.hold_player(_rig.player, _session.is_fading())
	_rig.track()
	_rig.follow()
	options.aim_step()
	if device_step.is_valid():
		device_step.call(delta)
	_session.step(delta, _rig.player.global_position, hold_streaming)
	if quit_in >= 0:
		quit_in -= 1
		quit_countdown.emit(quit_in)
		if quit_in < 0:
			if not _settings.save_to.is_empty():
				_session.save_game(_settings.save_to)
			quitting.emit()
			_host.get_tree().quit(0)
		return false
	if _session.has_activations():
		_session.run_activation()
		return false
	return true


## The controls move the player (not when paralyzed).
func drive(delta: float) -> void:
	_controls.paralyzed = not _session.player_state.can(PlayerState.MOVE)
	_controls.apply(delta)


## The player chose to load after dying: the newest save, or the start place again. False (and
## said) when there is neither; the shell then keeps its death screen.
func reload_after_death() -> bool:
	if not _session.reload_or_restart():
		printerr("death: nothing to load and no start place")
		return false
	_controls.blocked = false
	return true


## Whether the player's keys and mouse count at all (false in runs that drive themselves).
func input_enabled() -> bool:
	return _controls.enabled


## A run that cannot go on: say so and quit with an error.
func fail(message: String) -> void:
	push_error(message)
	printerr(message)
	_host.get_tree().quit(1)


## A key that is not about the player's body. The keys every shell has are handled here, the
## menus are the shell's (signals), anything else is the shell's `command`.
func _on_command(action: StringName, shift: bool) -> void:
	match action:
		PlayerInput.ACTIVATE:
			_session.activate_in_view(shift)  # Shift opens locks
		PlayerInput.READY_WEAPON:
			_session.toggle_weapon()
		PlayerInput.QUICK_SAVE:
			_session.save_game(SaveService.QUICKSAVE)
		PlayerInput.QUICK_LOAD:
			_session.load_game(SaveService.QUICKSAVE)
		PlayerInput.TOGGLE_FLY:
			_note.call("flying" if _rig.toggle_fly() else "walking")
		PlayerInput.CHANGE_TIME:
			var time_text := _session.clock.shift(-1.0 if shift else 1.0)
			if not time_text.is_empty():
				_note.call(time_text)
		PlayerInput.NEXT_WEATHER:
			if _session.clock.has_weather():
				_session.clock.weather.next_weather()
		PlayerInput.INVENTORY:
			inventory_requested.emit()
		PlayerInput.MAGIC:
			magic_requested.emit()
		PlayerInput.SKILLS:
			skills_requested.emit()
		_:
			command.emit(action, shift)


func _on_attack_changed(pressed: bool) -> void:
	if pressed:
		_session.attack_pressed()
	else:
		_session.attack_released()


func _on_attack_left_changed(pressed: bool) -> void:
	if pressed:
		_session.attack_left_pressed()
	else:
		_session.attack_left_released()


func _on_voice_changed(pressed: bool) -> void:
	if pressed:
		var voice := _session.voice_pressed()
		if not voice["ok"]:
			_note.call("voice: " + String(voice["reason"]))
	else:
		_session.voice_released()


func _on_message(text: String, _box: bool) -> void:
	_note.call(text)


func _on_fell_through() -> void:
	_note.call("fell through the world: flying (V walks)")


func _on_activations_finished() -> void:  # let scripts run first
	quit_in = _settings.quit_after if _settings.quit_after >= 0 else 5


func _on_player_died(killer: int) -> void:
	_controls.blocked = true
	died.emit(killer)
