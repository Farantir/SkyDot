# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game's Sleep/Wait Menu (sleepwaitmenu.swf): "Wait how long?" on the Wait key (T), "Rest how long?" when a bed is
# activated, over the world (the item menus' image space). Drawn from the movie through UiDraw and driven as its
# ActionScript does (SleepWaitMenu.handleInput):
#   HoursSlider   gfx.controls.Slider as placed (minimum 1, maximum 24, value 1, no snapping, offsets 2 and 40): left
#                 and right move it by 1 (snapInterval), the wheel by its delta, Page Up / Page Down and the gamepad's
#                 shoulders by 4; a click or drag on the track puts the thumb there. HoursText shows floor(value).
#   Enter / Tab   OK (the hours) / Cancel; the buttons are the movie's MouseButtons with the CrossPlatformButtons
#                 art ("Enter", "Tab"); a click on them does the same
#   OK            controls disabled (slider and track at their "disabled" frames, the thumb no longer moves), then
#                 the rest runs (Rest.step: an hour per Rest.HOUR_SECONDS, the game's time standing still between
#                 them) while HoursText counts down and CurrentTime follows; a sleep fades the world to black under
#                 the menu and back after it. The menu closes when the rest is over.
# The game refuses a rest before the menu opens (Rest.refusal: a HUD message instead).
class_name SleepWaitView
extends GameMenu

const SWF := "sleepwaitmenu"
const MENU := "SleepWaitMenu_mc"
## The HoursSlider's parameters as the movie places it (PlaceObject clip actions).
const MINIMUM := 1.0
const MAXIMUM := 24.0
const OFFSET_LEFT := 2.0
const OFFSET_RIGHT := 40.0
## SleepWaitMenu.handleInput: Page Up / Page Down move the slider by this.
const PAGE_STEP := 4.0
## How long the world takes to fade to black behind a sleep, and back (an hour of the rest).
const HOUR_SECONDS := Rest.HOUR_SECONDS

## The slider's value (Slider.value, a float; the hours are its floor).
var value := MINIMUM
var sleeping := false
## The rest under way: hours still to pass (0 when the menu waits for a choice).
var hours_left: int:
	get:
		return _session.rest.hours_left if _resting and _session.rest != null else 0

var _session: GameSession
## Whether this menu began the rest under way.
var _resting := false
## How black the world is behind a sleep (0 to 1); it fades back after the menu closes.
var _fade := 0.0
var _dragging := false
## The value the thumb stood at when the rest began (a disabled slider's thumb stays).
var _thumb_value := MINIMUM


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	visible = false


## Whether the pack has the movie.
static func available() -> bool:
	return UiAssets.available() and not UiAssets.instance(SWF, MENU).is_empty()


## The menu for a wait (`bed` 0) or a sleep in `bed`. A rest the game refuses shows its message instead.
func open(sleep: bool, bed := 0) -> void:
	if _open or _session.rest == null or _session.rest.is_resting():
		return
	var refusal := _session.rest.refusal(sleep, bed)
	if not refusal.is_empty():
		_session.message.emit(refusal, false)
		return
	_open = true
	sleeping = sleep
	value = MINIMUM
	_resting = false
	_dragging = false
	visible = true
	queue_redraw()
	engaged.emit(true)


## Closing does not stop a rest under way: Rest.step finishes it.
func _closed() -> void:
	_resting = false
	visible = _fade > 0.0


## The hours chosen (getSliderValue: the floor of the slider's value).
func hours() -> int:
	return int(floor(value))


## OK (onOKPress): the controls are disabled and the rest begins.
func accept() -> void:
	if not _open or hours_left > 0:
		return
	_thumb_value = value
	_session.rest.begin(sleeping, hours())
	_resting = true
	queue_redraw()


func set_value(to: float) -> void:
	if hours_left > 0:
		return
	value = clampf(to, MINIMUM, MAXIMUM)
	queue_redraw()


func _process(delta: float) -> void:
	if _resting and not _session.rest.is_resting():
		_resting = false
		close()
	if _open and _resting:
		if sleeping:
			_fade = minf(1.0, _fade + delta / HOUR_SECONDS)
		value = float(hours_left)
		queue_redraw()
	elif not _open and _fade > 0.0:
		_fade = maxf(0.0, _fade - delta / HOUR_SECONDS)
		visible = _fade > 0.0
		queue_redraw()


func _menu_input(event: InputEvent) -> void:
	if hours_left == 0:
		if event is InputEventMouseMotion:
			if _dragging:
				_drag_to(event.position)
			return
		if event.is_action_pressed(&"ui_right", true):
			set_value(value + 1.0)
		elif event.is_action_pressed(&"ui_left", true):
			set_value(value - 1.0)
		elif event is InputEventKey and event.pressed and event.keycode == KEY_PAGEUP:
			set_value(value + PAGE_STEP)
		elif event is InputEventKey and event.pressed and event.keycode == KEY_PAGEDOWN:
			set_value(value - PAGE_STEP)
		elif event is InputEventJoypadButton and event.pressed and event.button_index == JOY_BUTTON_RIGHT_SHOULDER:
			set_value(value + PAGE_STEP)
		elif event is InputEventJoypadButton and event.pressed and event.button_index == JOY_BUTTON_LEFT_SHOULDER:
			set_value(value - PAGE_STEP)
		elif event.is_action_pressed(&"ui_accept"):
			accept()
		elif event.is_action_pressed(&"ui_cancel"):
			close()
		elif event is InputEventMouseButton and event.pressed:
			_click(event)
		elif event is InputEventMouseButton and not event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
			_dragging = false
	_consume(event)


func _click(event: InputEventMouseButton) -> void:
	match event.button_index:
		MOUSE_BUTTON_WHEEL_UP:
			set_value(value + 1.0)
		MOUSE_BUTTON_WHEEL_DOWN:
			set_value(value - 1.0)
		MOUSE_BUTTON_LEFT:
			if _button_rect("AcceptMouseButton").has_point(event.position):
				accept()
			elif _button_rect("CancelMouseButton").has_point(event.position):
				close()
			elif _slider_rect().has_point(event.position):
				_dragging = true
				_drag_to(event.position)


## The slider's value under `point` (Slider.trackPress / doDrag: the track's width less the offsets).
func _drag_to(point: Vector2) -> void:
	var slider := _placed("HoursSlider")
	if slider.is_empty():
		return
	var local := (slider["xform"] as Transform2D).affine_inverse() * point
	var track := _track_width() - OFFSET_LEFT - OFFSET_RIGHT
	set_value((local.x - OFFSET_LEFT) / track * (MAXIMUM - MINIMUM) + MINIMUM)


# ---- drawing

## A child of the menu clip: {id, xform (to this control), matrix} or empty.
func _placed(name: String) -> Dictionary:
	var menu := UiAssets.instance(SWF, MENU)
	if menu.is_empty():
		return {}
	var root: Transform2D = fill() * (menu["xform"] as Transform2D)
	for placed: Dictionary in UiAssets.character(SWF, int(menu["id"])).get("display", []):
		if placed.get("name", "") == name:
			return {"id": int(placed["character"]), "xform": root * UiDraw.matrix(placed["matrix"])}
	return {}


## Slider.__width: the component's width as placed (its track's).
func _track_width() -> float:
	var slider := _placed("HoursSlider")
	return UiAssets.bounds(SWF, int(slider["id"])).size.x if not slider.is_empty() else 0.0


func _slider_rect() -> Rect2:
	var slider := _placed("HoursSlider")
	return (slider["xform"] as Transform2D) * UiAssets.bounds(SWF, int(slider["id"])) if not slider.is_empty() else Rect2()


func _button_rect(name: String) -> Rect2:
	var rect := _placed("ButtonRect")
	var child := UiAssets.child(SWF, int(rect["id"]), name) if not rect.is_empty() else {}
	if child.is_empty():
		return Rect2()
	var xform: Transform2D = (rect["xform"] as Transform2D) * UiDraw.matrix(child["matrix"])
	var art := _button_art(name)
	var bounds := UiAssets.bounds(SWF, int(child["character"]))
	var art_width := UiAssets.bounds(SWF, art).size.x if art >= 0 else 0.0
	return xform * Rect2(bounds.position - Vector2(art_width, 0.0), bounds.size + Vector2(art_width, 0.0))


func _button_art(name: String) -> int:
	return UiAssets.exported(SWF, "Enter" if name.begins_with("Accept") else "Tab")


func _draw() -> void:
	if not UiAssets.available():
		return
	if _fade > 0.0:
		draw_rect(Rect2(Vector2.ZERO, size), Color(0.0, 0.0, 0.0, _fade))
	if not _open:
		return
	var menu := UiAssets.instance(SWF, MENU)
	if menu.is_empty():
		return
	var look := JournalView.look()
	look.hidden["ButtonRect"] = true
	look.texts["QuestionInstance"] = JournalView.translate("$Rest how long?" if sleeping else "$Wait how long?")
	look.texts["HoursText"] = str(hours())
	look.texts["CurrentTime"] = _time_text()
	var slider_id := -1
	var slider := _placed("HoursSlider")
	if not slider.is_empty():
		slider_id = int(slider["id"])
	# Slider.updateThumb: the thumb at the value along the track less the offsets, centred; a disabled slider
	# (the rest under way) keeps its thumb where it was and shows its "disabled" frames.
	var thumb := UiAssets.child(SWF, slider_id, "thumb") if slider_id >= 0 else {}
	if not thumb.is_empty():
		var thumb_id := int(thumb["character"])
		var width := UiAssets.bounds(SWF, thumb_id).size.x
		var at := value if hours_left == 0 else _thumb_value
		var x := (float(at) - MINIMUM) / (MAXIMUM - MINIMUM) * (_track_width() - OFFSET_LEFT - OFFSET_RIGHT) - width / 2.0 + OFFSET_LEFT
		look.moves["thumb"] = Vector2(x, UiDraw.matrix(thumb["matrix"]).origin.y)
		if hours_left > 0:
			look.named_frames["thumb"] = maxi(UiAssets.label_frame(SWF, thumb_id, "disabled"), 0)
			var track := UiAssets.child(SWF, slider_id, "track")
			if not track.is_empty():
				look.named_frames["track"] = maxi(UiAssets.label_frame(SWF, int(track["character"]), "disabled"), 0)
	UiDraw.character(self, SWF, int(menu["id"]), fill() * (menu["xform"] as Transform2D), Color.WHITE, look)
	_draw_buttons()


## ButtonRect's mouse buttons (the PC with keyboard and mouse shows them, their labels in the movie's grey, #ffffff66,
## as the game draws them), with the keys' art right-aligned before the button, centred on its height
## (CrossPlatformButtons.RefreshArt), the label in the button's text field.
func _draw_buttons() -> void:
	var rect := _placed("ButtonRect")
	if rect.is_empty():
		return
	for entry: Array in [["AcceptMouseButton", "$Yes"], ["CancelMouseButton", "$No"]]:
		var child := UiAssets.child(SWF, int(rect["id"]), entry[0])
		if child.is_empty():
			continue
		var xform: Transform2D = (rect["xform"] as Transform2D) * UiDraw.matrix(child["matrix"])
		var button_id := int(child["character"])
		var height := UiAssets.bounds(SWF, button_id).size.y
		var art := _button_art(entry[0])
		if art >= 0:
			var art_bounds := UiAssets.bounds(SWF, art)
			UiDraw.character(self, SWF, art, xform * Transform2D(0.0, Vector2(-art_bounds.size.x - art_bounds.position.x,
				(height - art_bounds.size.y) * 0.5 - art_bounds.position.y)))
		var field := UiAssets.child(SWF, button_id, "textField")
		if not field.is_empty():
			var field_id := int(field["character"])
			UiDraw.field(self, SWF, field_id, xform * UiDraw.matrix(field["matrix"]), JournalView.translate(entry[1]),
				Color(String(UiAssets.character(SWF, field_id).get("color", "#ffffffff"))), true)


## SetCurrentTime: the date and hour without the year.
func _time_text() -> String:
	if _session.clock == null or _session.world == null:
		return ""
	return TweenView.format_date(_session.clock.get_game_days(), _session.clock.get_hour(),
		func(setting: String) -> String: return _session.world.get_gmst_string(setting, ""), false)
