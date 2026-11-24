# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game's Tween menu (Tab): four directions around a compass, Skills up, Magic left, Items right, Map down, over
# the world (the item menus' image space, MenuHost), with the bottom bar's level, level meter and date. Drawn from
# tweenmenu.swf (TweenMenu_mc: Selections_mc's frames None/Skills/Magic/Inventory/Map highlight a direction) and
# driven as its ActionScript does (TweenMenu.handleInput, onInputRectMouseOver/Click):
#   up/left/right/down   highlight that direction; the highlighted one again opens it
#   accept (Enter, E)    opens the highlighted one
#   cancel (Tab, Esc)    closes the menu (back to the game)
#   mouse                over a direction's input rect highlights it, a click opens it; the wheel does nothing
# Opening a direction closes this menu and opens the viewer's menu for it (`chosen`); the game keeps TweenMenu hidden
# under it, and Tab there returns to the game, the same as here. Map opens the world map (ui/map_view.gd).
# The skills header says LEVEL UP when a level-up is due (the movie's bLevelUp).
class_name TweenView
extends GameMenu
## A direction was opened: "skills", "magic", "inventory" or "map".
signal chosen(direction: String)

const SWF := "tweenmenu"
## Selections_mc's frame labels by selection index (TweenMenu.FrameToLabelMap).
const LABELS := ["None", "Skills", "Magic", "Inventory", "Map"]
const DIRECTIONS := ["", "skills", "magic", "inventory", "map"]
const INPUT_RECTS := {"SkillsInputRect": 1, "MagicInputRect": 2, "ItemsInputRect": 3, "MapInputRect": 4}

## The highlighted direction: 0 none, 1 Skills, 2 Magic, 3 Items, 4 Map.
var selection := 0

var _session: GameSession


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	visible = false


func open() -> void:
	if _open:
		return
	_open = true
	selection = 0
	visible = true
	queue_redraw()
	engaged.emit(true)


func _closed() -> void:
	visible = false


## Highlight `index`, or open it when it is highlighted already (TweenMenu.handleInput).
func press(index: int) -> void:
	if index == selection:
		open_selected()
	else:
		selection = index
		queue_redraw()


## Open the highlighted direction (OpenHighlightedMenu).
func open_selected() -> void:
	if selection <= 0:
		return
	var direction: String = DIRECTIONS[selection]
	close()
	chosen.emit(direction)


func _menu_input(event: InputEvent) -> void:
	var handled := true
	if event is InputEventMouseMotion:
		var over := _rect_at(event.position)
		if over > 0 and over != selection:
			selection = over
			queue_redraw()
		handled = false
	elif event is InputEventMouseButton:
		if event.pressed and event.button_index == MOUSE_BUTTON_LEFT and _rect_at(event.position) > 0:
			selection = _rect_at(event.position)
			open_selected()
	elif event.is_action_pressed(&"ui_up"):
		press(1)
	elif event.is_action_pressed(&"ui_left"):
		press(2)
	elif event.is_action_pressed(&"ui_right"):
		press(3)
	elif event.is_action_pressed(&"ui_down"):
		press(4)
	elif event.is_action_pressed(&"ui_accept"):
		open_selected()
	elif event.is_action_pressed(&"ui_cancel") or event.is_action_pressed(&"tween_menu"):
		close()
	else:
		handled = false
	_consume(event, handled)


## The direction whose input rect (the movie's invisible hit areas) holds `at` (this control's pixels), else 0.
func _rect_at(at: Vector2) -> int:
	for name: String in INPUT_RECTS:
		var placed := UiAssets.instance(SWF, "TweenMenu_mc/" + name)
		if placed.is_empty():
			continue
		var rect: Rect2 = (fill() * (placed["xform"] as Transform2D)) * UiAssets.bounds(SWF, int(placed["id"]))
		if rect.has_point(at):
			return INPUT_RECTS[name]
	return 0


func _draw() -> void:
	if not _open or not UiAssets.available():
		return
	var menu := UiAssets.instance(SWF, "TweenMenu_mc")
	if menu.is_empty():
		return
	var look := UiDraw.Look.new()
	look.draw_texts = true
	var selections := UiAssets.instance(SWF, "TweenMenu_mc/Selections_mc")
	if not selections.is_empty():
		look.frames[int(selections["id"])] = maxi(UiAssets.label_frame(SWF, int(selections["id"]), LABELS[selection]), 0)
	for name: String in INPUT_RECTS:
		look.hidden[name] = true
	var progression := _session.progression
	if progression != null:
		var progress := progression.data().progress()
		var level := int(progress["level"])
		look.texts["LevelNumberLabel"] = str(level)
		look.fills["LevelProgressBar"] = clampf(float(progress["xp"]) / maxf(progression.level_up_threshold(level), 1.0), 0.0, 1.0)
		if progression.level_up_ready():
			look.texts["SkillsText_mc/textField"] = "LEVEL UP"
	look.texts["DateText"] = UiDraw.date_text(_session)
	UiDraw.character(self, SWF, int(menu["id"]), fill() * (menu["xform"] as Transform2D), Color.WHITE, look)
