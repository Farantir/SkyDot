# SPDX-License-Identifier: GPL-3.0-or-later
#
# MenuHost (viewer/menu_host.gd) and the loop's rules around it (ShellLoop): one menu at a time, which menus pause
# the world, who blocks the controls, that a load closes every menu, and the journal settings it applies. The
# session is never opened (the fake VM and world); menus whose movie the test has no pack for are marked open directly.
extends "gd_test.gd"
const Fakes := preload("fakes.gd")

const SPEAKER := 0xFF000042


class StubSession extends GameSession:
	var dead := false

	func is_player_dead() -> bool:
		return dead

	func spell_in_hand(_hand: int) -> int:
		return 0  # no Magic in this session

	func reload_or_restart() -> bool:
		dead = false
		return true


var _session: StubSession
var _controls: PlayerInput
var _loop: ShellLoop
var _host: MenuHost


func before_each() -> void:
	var settings := ViewerSettings.from_arguments([])
	var rig := PlayerRig.new(settings, SkydotImageSpace.new())
	add_child(rig.camera)
	add_child(rig.player)
	_session = StubSession.new()
	_session.clock = SkydotClock.new()
	_session.clock.setup(10.0, 20.0, false)
	_session.papyrus = Fakes.papyrus()
	_session.world = Fakes.world()
	_session.player_state = PlayerState.new(PlayerStateData.new(_session.papyrus))
	_session.speech = Speech.new(null, func(_ref: int) -> Node: return null)
	_session.rest = Rest.new(_session)
	_session.world_map = WorldMap.new(_session)
	add_child(_session)
	_session.add_child(_session.speech)
	_controls = PlayerInput.new(rig)
	_controls.enabled = false  # no mouse capture in a test
	add_child(_controls)
	_loop = ShellLoop.new(self, _session, rig, _controls, settings, SkydotImageSpace.new(), func(_text: String) -> void: pass)
	var layer := CanvasLayer.new()
	add_child(layer)
	_host = MenuHost.new(_session, _controls, layer.add_child)
	_host.attach(_loop)


## Open `view` as its own open() would, without the movie or the game it draws.
func _mark_open(view: GameMenu) -> void:
	view._open = true
	view.engaged.emit(true)


func _open_views() -> Array[String]:
	var out: Array[String] = []
	for view in _host.views:
		if view.is_open():
			out.append(view.get_script().get_global_name())
	return out


func _talk() -> void:
	_session._talk_speaker = SPEAKER
	_session.dialogue_topics.emit([])


func _end_talk() -> void:
	_session._talk_speaker = 0
	_session.dialogue_closed.emit()


func test_one_menu_at_a_time() -> void:
	_host.open_inventory()
	expect_eq(_open_views(), ["InventoryView"], "the inventory opens")
	_host.open_magic()
	_host.open_favorites()
	_host.open_tween()
	_host.toggle_skills()
	_host.open_wait()
	expect_eq(_open_views(), ["InventoryView"], "nothing opens over it")
	_host.inventory_view.close()
	_session.container_opened.emit(0x1234)
	_host.open_inventory()
	expect_eq(_open_views(), ["ContainerView"], "nor over a container")


func test_the_map_does_not_open_over_a_container() -> void:
	_session.container_opened.emit(0x1234)
	expect_eq(_open_views(), ["ContainerView"], "the container's menu opens")
	_loop.map_requested.emit()
	_host.open_map()
	expect_eq(_open_views(), ["ContainerView"], "the map stays shut")


func test_a_container_replaces_the_item_menus() -> void:
	_host.open_inventory()
	_session.container_opened.emit(0x1234)
	expect_eq(_open_views(), ["ContainerView"], "the inventory closed for the container")


func test_no_menu_of_the_player_opens_in_a_dialogue() -> void:
	_talk()
	_loop.inventory_requested.emit()
	_loop.magic_requested.emit()
	_loop.favorites_requested.emit()
	_loop.tween_requested.emit()
	_loop.skills_requested.emit()
	_session.container_opened.emit(0x1234)
	expect(not _host.is_open(), "no menu opened")


func test_every_menu_but_training_pauses_the_world() -> void:
	expect(not _loop.is_paused(), "nothing open: the world runs")
	for view in _host.views:
		_mark_open(view)
		expect_eq(_loop.is_paused(), view != _host.training_view, "paused under %s" % view.get_script().get_global_name())
		view.close(true)
	expect(not _loop.is_paused(), "all closed: the world runs")


func test_the_dialogue_does_not_pause_the_world() -> void:
	_talk()
	expect(not _loop.is_paused(), "the world runs in a dialogue")
	expect(_controls.blocked, "the player stands still")


func test_closing_training_in_a_dialogue_keeps_the_controls_blocked() -> void:
	_talk()
	_mark_open(_host.training_view)
	expect(_controls.blocked, "blocked in the training menu")
	_host.training_view.close()
	expect(_controls.blocked, "the dialogue still holds them")
	_end_talk()
	expect(not _controls.blocked, "free once the dialogue is over")


func test_the_dialogue_closing_closes_training() -> void:
	_talk()
	_mark_open(_host.training_view)
	expect(_host.service_open(), "the dialogue's keys are quiet under training")
	_end_talk()
	expect(not _host.training_view.is_open() and not _host.service_open(), "training closed with the dialogue")


func test_closing_a_menu_keeps_the_dead_blocked() -> void:
	_host.open_inventory()
	_session.dead = true
	_session.player_died.emit(7)
	_host.inventory_view.close()
	expect(_controls.blocked, "the dead player stays blocked")
	_loop.update_blocked()
	expect(_controls.blocked, "also when worked out again")
	expect(_loop.reload_after_death(), "the reload works")
	expect(not _controls.blocked, "free after the reload")


func test_a_menu_blocks_and_closing_it_frees() -> void:
	_host.open_inventory()
	expect(_controls.blocked, "blocked while open")
	_host.inventory_view.close()
	expect(not _controls.blocked, "free when closed")


func test_a_load_closes_every_menu() -> void:
	_mark_open(_host.skills_view)
	_session._reset_game_state()
	expect(not _host.is_open(), "the menus are closed")
	expect(not _controls.blocked, "the controls are free")


func test_no_save_while_dead() -> void:
	_session.dead = true
	expect(not _session.save_game("user://test_dead.skydot"), "refused")


func test_invert_y_in_the_journal_settings_reaches_the_controls() -> void:
	var system := _host.journal_view.system
	system.settings_index = 0
	system.option_index = 0  # $Invert Y
	system._change_option(1)
	expect(_controls.invert_look_y, "checked: the right stick looks down")
	system._change_option(1)
	expect(not _controls.invert_look_y, "unchecked again")
