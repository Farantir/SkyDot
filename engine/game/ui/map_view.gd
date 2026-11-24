# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game's world map (map.swf, MapMenu): the 3D view is MapScene's (the main camera above the map world's LOD),
# this draws what the movie draws over it and takes the Map Menu context's input (controlmap.txt).
#   markers     map.swf's MapMenu.CreateMarkers / RefreshMarkers: each visible marker (XMRK flag 1) as the clip
#               MapMarker.IconTypes names for its type, at its projected position; one not yet travelled to (no flag 2)
#               with its "<Icon>Undiscovered" clip over IconClip; the player as YouAreHereMarker turned by his heading;
#               the custom marker as PlayerSetMarker (sPlayerSetMarkerName). The marker under the cursor
#               (SetSelectedMarker: its HitAreaClip, 35 px) is drawn last at its "over" frame: its own name field
#               (the player's "Current Location") or, for the others, the DescriptionHolder with the name as Title.
#   bottom bar  Bottom locked to the bottom (Lock("B"), the safe zone): Local Map (L), Journal (J), Zoom (Wheel),
#               Current Location (E), Set Destination (M1) and the date (SetDateString)
#   input       W/A/S/D pan, the wheel and the triggers zoom, E back to the player, J the journal, Tab/Esc close,
#               P places the custom marker under the cursor, a click: on a discovered marker sFastTravelConfirm (Yes,
#               No, Place Marker), on one not discovered sPlaceMarkerUndiscovered (Yes, No), on the custom marker
#               sMoveMarkerQuestion (Move It, Leave It, Remove It), elsewhere the custom marker there (Set
#               Destination); the left stick pans as the keys do (not measured: in the game it moves a cursor).
#   travel      Yes: WorldMap.refusal's text as a HUD notification over the map, or the map fades to black
#               (fFastTravelFadeSecs) and closes for the travel.
# Not done: the local map (L; interiors open the world map at the last exterior position), quest markers, look mode,
# the game's own cursor art (the system's pointer shows).
class_name MapView
extends GameMenu
## J: the journal instead of the map.
signal journal_requested

const SWF := "map"
const ICON_TYPES := ["EmptyMarker", "CityMarker", "TownMarker", "SettlementMarker", "CaveMarker", "CampMarker",
	"FortMarker", "NordicRuinMarker", "DwemerMarker", "ShipwreckMarker", "GroveMarker", "LandmarkMarker",
	"DragonlairMarker", "FarmMarker", "WoodMillMarker", "MineMarker", "ImperialCampMarker", "StormcloakCampMarker",
	"DoomstoneMarker", "WheatMillMarker", "SmelterMarker", "StableMarker", "ImperialTowerMarker", "ClearingMarker",
	"PassMarker", "AltarMarker", "RockMarker", "LighthouseMarker", "OrcStrongholdMarker", "GiantCampMarker",
	"ShackMarker", "NordicTowerMarker", "NordicDwellingMarker", "DocksMarker", "ShrineMarker", "RiftenCastleMarker",
	"RiftenCapitolMarker", "WindhelmCastleMarker", "WindhelmCapitolMarker", "WhiterunCastleMarker",
	"WhiterunCapitolMarker", "SolitudeCastleMarker", "SolitudeCapitolMarker", "MarkarthCastleMarker",
	"MarkarthCapitolMarker", "WinterholdCastleMarker", "WinterholdCapitolMarker", "MorthalCastleMarker",
	"MorthalCapitolMarker", "FalkreathCastleMarker", "FalkreathCapitolMarker", "DawnstarCastleMarker",
	"DawnstarCapitolMarker", "DLC02MiraakTempleMarker", "DLC02RavenRockMarker", "DLC02StandingStonesMarker",
	"DLC02TelvanniTowerMarker", "DLC02ToSkyrimMarker", "DLC02ToSolstheimMarker", "DLC02CastleKarstaagMarker", "",
	"DoorMarker", "QuestTargetMarker", "QuestTargetDoorMarker", "MultipleQuestTargetMarker", "PlayerSetMarker",
	"YouAreHereMarker"]
## The bottom bar's buttons: [instance, key art export, label].
const BUTTONS := [["LocalMapButton", "L", "$Local Map"], ["JournalButton", "J", "$Journal"],
	["LeftButton", "Wheel", "$Zoom"], ["PlayerLocButton", "E", "$Current Location"],
	["RightButton", "Mouse1", "$Set Destination"]]
## SetSelectedMarker: the marker nearest the cursor within fMaxMarkerSelectionDist (0.003) of the squared distance in
## screen fractions (measured, map3/j15: Whiterun stayed selected out to 59.4 px of a 1085 px wide screen, 0.0547 =
## sqrt(0.003); 30 px below it too).
const SELECTION_DIST := 0.003
## The left stick's cursor speed: not measured (view heights a second).
const CURSOR_SPEED := 0.8
## The DescriptionHolder's last "Show" frame (Show 1 .. Hide 8).
const DESCRIPTION_FRAME := 7
## The marker clips' "over" transition: frames 10 to 19.
const OVER_FRAMES := 9
## The player's marker is drawn 1.5 times the movie's size (measured in game_map_default, 1100x800: its arrow and its
## "Current Location" 1.5 times what the clip gives at the stage's scale; the other icons are the clip's size).
const PLAYER_SCALE := 1.5

enum Prompt { NONE, TRAVEL, UNDISCOVERED, MOVE_MARKER }

## The 3D side (the shell's): without one the map does not open.
var scene: MapScene
## Where the cursor points (view pixels); the game starts it in the middle.
var cursor := Vector2.ZERO
var prompt := Prompt.NONE
var prompt_marker := {}
var prompt_choice := 0
## The marker under the cursor (an entry of `shown`), {} none.
var selected := {}
## What the last frame drew: [{ref, type, name, at (stage px), rotation, undiscovered, entry}].
var shown: Array = []

var _session: GameSession
var _moving_marker := false
var _travelling := false
var _boxes: Array[Rect2] = []
var _texts: HudTexts


func _init(session: GameSession) -> void:
	_session = session
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	visible = false
	session.message.connect(func(text: String, _box: bool) -> void:
		if _open and _texts != null:
			_texts.show_message(text))


static func available() -> bool:
	return UiAssets.available() and UiAssets.exported(SWF, "YouAreHereMarker") >= 0


## Open on the player (or the last exterior position). False when this place has no world map.
func open() -> bool:
	if _open or scene == null or _session.world_map == null:
		return false
	if not scene.open(_session.world_map.map_place()):
		return false
	_open = true
	_travelling = false
	_moving_marker = false
	prompt = Prompt.NONE
	cursor = size * 0.5
	_texts = HudTexts.new() if HudTexts.available() else null
	visible = true
	if enabled.call() and DisplayServer.get_name() != "headless":
		Input.warp_mouse(cursor)
	engaged.emit(true)
	_refresh()
	return true


func _closed() -> void:
	prompt = Prompt.NONE
	if scene != null:
		scene.close()
	visible = false


## A frame of the map without time passing (the camera placed, the markers projected): for MapShot.
func step_now() -> void:
	if _open:
		scene.step(0.0)
		_refresh()


func _process(delta: float) -> void:
	if not _open:
		return
	if _travelling:
		# The fade to black runs over the map; the travel itself once the world is back.
		_session.transition.step(delta)
		if _session.transition.phase != PlaceTransition.Phase.OUT:
			close()
		return
	if prompt == Prompt.NONE and enabled.call():
		_held_input(delta)
	scene.step(delta)
	if _texts != null:
		_texts.step(delta)
	_refresh()


## Held keys and sticks: pan and zoom.
func _held_input(delta: float) -> void:
	var pan := Vector2.ZERO
	for entry: Array in [[&"map_up", Vector2.UP], [&"map_down", Vector2.DOWN], [&"map_left", Vector2.LEFT], [&"map_right", Vector2.RIGHT]]:
		if InputMap.has_action(entry[0]) and Input.is_action_pressed(entry[0]):
			pan += entry[1]
	if pan != Vector2.ZERO:
		# Screen up is the map's north turned by the camera's yaw.
		scene.camera.pan(Vector2(pan.x, -pan.y).limit_length(1.0), delta)
	# The left stick moves the cursor (Map Menu "Cursor": a 0.5 s push panned nothing, measured); its speed,
	# fGamepadCursorSensitivity 2, is not measured: taken as CURSOR_SPEED of the view's height a second.
	var stick := Vector2(Input.get_joy_axis(0, JOY_AXIS_LEFT_X), Input.get_joy_axis(0, JOY_AXIS_LEFT_Y))
	if stick.length() > 0.2:
		cursor = (cursor + stick * CURSOR_SPEED * size.y * delta).clamp(Vector2.ZERO, size)
	# Edge panning: the cursor in the screen's outer band pans the map (measured).
	var edge := MapCamera.edge_pan(cursor, size)
	if edge != Vector2.ZERO:
		scene.camera.pan(edge.limit_length(1.0), delta, MapCamera.EDGE_FACTOR)
	# The right stick looks (Map Menu "Look").
	var look := Vector2(Input.get_joy_axis(0, JOY_AXIS_RIGHT_X), Input.get_joy_axis(0, JOY_AXIS_RIGHT_Y))
	if look.length() > 0.2:
		scene.camera.look(look.x * MapCamera.STICK_YAW_RATE * delta, look.y * MapCamera.STICK_PITCH_RATE * delta)
	var zoom := Input.get_joy_axis(0, JOY_AXIS_TRIGGER_LEFT) - Input.get_joy_axis(0, JOY_AXIS_TRIGGER_RIGHT)
	if absf(zoom) > 0.1:
		scene.camera.zoom_by(zoom * MapCamera.ZOOM_RATE * delta)


func _menu_input(event: InputEvent) -> void:
	if _travelling:
		return
	if event is InputEventMouseMotion:
		if prompt == Prompt.NONE and (event.button_mask & MOUSE_BUTTON_MASK_RIGHT) != 0:
			# MapLookMode (right mouse): the camera turns, the cursor stays.
			var rel: Vector2 = event.relative
			scene.camera.look(rel.x * MapCamera.LOOK_YAW_PER_COUNT, rel.y * MapCamera.LOOK_PITCH_PER_COUNT)
			return
		cursor = event.position
		if prompt != Prompt.NONE:
			_hover_prompt(event.position)
		return
	if prompt != Prompt.NONE:
		_prompt_input(event)
	elif _pressed(event, &"map_cancel"):
		close()
	elif _pressed(event, &"map_player_position"):
		scene.camera.recenter()
	elif _pressed(event, &"map_journal"):
		close()
		journal_requested.emit()
	elif _pressed(event, &"map_place_marker"):
		_set_destination(cursor)
	elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_UP:
		scene.camera.zoom_by(-MapCamera.WHEEL_STEP)
	elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
		scene.camera.zoom_by(MapCamera.WHEEL_STEP)
	elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		click(event.position)
	elif _pressed(event, &"map_click"):
		click(cursor)
	_consume(event)


static func _pressed(event: InputEvent, action: StringName) -> bool:
	return InputMap.has_action(action) and event.is_action_pressed(action)


## ClickCallback / MarkerClick: what a click at `at` (view pixels) does.
func click(at: Vector2) -> void:
	cursor = at
	_refresh()
	if _moving_marker:
		_moving_marker = false
		_set_destination(at)
		return
	if selected.is_empty():
		_set_destination(at)
		return
	var type := int(selected["type"])
	if type == WorldMap.YOU_ARE_HERE_TYPE:
		return
	if type == WorldMap.PLAYER_SET_TYPE:
		_ask(Prompt.MOVE_MARKER, selected)
	elif int(selected["entry"].get("flags", 0)) & WorldMap.CAN_TRAVEL:
		_ask(Prompt.TRAVEL, selected)
	else:
		_ask(Prompt.UNDISCOVERED, selected)


func _ask(kind: Prompt, marker: Dictionary) -> void:
	prompt = kind
	prompt_marker = marker
	prompt_choice = 0
	queue_redraw()


func prompt_text() -> String:
	var wm := _session.world_map
	match prompt:
		Prompt.TRAVEL:
			return wm.text("sFastTravelConfirm").replace("%s", String(prompt_marker["name"]))
		Prompt.UNDISCOVERED:
			return wm.text("sPlaceMarkerUndiscovered")
		Prompt.MOVE_MARKER:
			return wm.text("sMoveMarkerQuestion")
	return ""


func prompt_buttons() -> Array:
	var gmst := func(setting: String, fallback: String) -> String: return _session.world.get_gmst_string(setting, fallback)
	match prompt:
		Prompt.TRAVEL:
			return [gmst.call("sYes", "Yes"), gmst.call("sNo", "No"), gmst.call("sPlaceMarker", "Place Marker")]
		Prompt.UNDISCOVERED:
			return [gmst.call("sYes", "Yes"), gmst.call("sNo", "No")]
		Prompt.MOVE_MARKER:
			return [gmst.call("sMoveMarker", "Move It"), gmst.call("sLeaveMarker", "Leave It"), gmst.call("sRemoveMarker", "Remove It")]
	return []


func _prompt_input(event: InputEvent) -> void:
	var count := prompt_buttons().size()
	if event.is_action_pressed(&"ui_left", true):
		prompt_choice = (prompt_choice + count - 1) % count
	elif event.is_action_pressed(&"ui_right", true):
		prompt_choice = (prompt_choice + 1) % count
	elif event.is_action_pressed(&"ui_accept"):
		answer(prompt_choice)
	elif _pressed(event, &"map_cancel") or event.is_action_pressed(&"ui_cancel"):
		prompt = Prompt.NONE
	elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		for i in _boxes.size():
			if _boxes[i].has_point(event.position):
				answer(i)
				return
	queue_redraw()


func _hover_prompt(at: Vector2) -> void:
	for i in _boxes.size():
		if _boxes[i].has_point(at) and i != prompt_choice:
			prompt_choice = i
			queue_redraw()


## A prompt's button `index` was chosen.
func answer(index: int) -> void:
	var kind := prompt
	var marker := prompt_marker
	prompt = Prompt.NONE
	var wm := _session.world_map
	match kind:
		Prompt.TRAVEL:
			if index == 0:
				var refusal := wm.refusal(marker["entry"])
				if not refusal.is_empty():
					_session.message.emit(refusal, false)
				else:
					_travel(marker["entry"])
			elif index == 2:
				wm.place_custom(scene.world, marker["entry"]["position"])
		Prompt.UNDISCOVERED:
			if index == 0:
				wm.place_custom(scene.world, marker["entry"]["position"])
		Prompt.MOVE_MARKER:
			if index == 0:
				_moving_marker = true
			elif index == 2:
				wm.remove_custom()
	queue_redraw()


func _travel(entry: Dictionary) -> void:
	_session.world_map.travel(entry)
	if _session.transition.phase == PlaceTransition.Phase.OUT:
		_travelling = true  # the fade runs over the map (_process)
	else:
		close()  # travelled at once (no one watching)


## Set Destination / Place Marker: the custom marker on the land under `at`.
func _set_destination(at: Vector2) -> void:
	var point: Variant = scene.ground_point(at)
	if point != null:
		_session.world_map.place_custom(scene.world, point)
		_refresh()


# ---- markers

## The markers as the movie shows them this frame, and the one under the cursor.
func _refresh() -> void:
	shown.clear()
	if not _open:
		return
	var wm := _session.world_map
	var fill_inverse := fill().affine_inverse()
	for m: Dictionary in wm.markers(scene.world):
		var flags := int(m["flags"])
		if flags & WorldMap.VISIBLE == 0 or _session.papyrus.is_ref_disabled(int(m["ref"])):
			continue
		_add_shown(fill_inverse, m["position"], int(m["type"]), String(m["name"]), 0.0, flags & WorldMap.CAN_TRAVEL == 0, m)
	if not wm.custom_marker.is_empty() and int(wm.custom_marker["world"]) == scene.world:
		_add_shown(fill_inverse, wm.custom_marker["position"], WorldMap.PLAYER_SET_TYPE, wm.text("sPlayerSetMarkerName"),
			0.0, false, wm.custom_marker)
	# The player's arrow turns against his heading (measured: heading 58.7 deg, the arrow at 301 deg in
	# game_map_default); Godot's yaw is minus the heading. The map's up is the camera's yaw.
	var heading := rad_to_deg(_session.player_object().yaw) + rad_to_deg(scene.camera.yaw)
	_add_shown(fill_inverse, scene.camera.target, WorldMap.YOU_ARE_HERE_TYPE, UiDraw.translate("$Current Location"),
		heading, false, {})
	selected = {}
	var best := SELECTION_DIST
	var fill_xform := fill()
	for s: Dictionary in shown:
		var d: Vector2 = (fill_xform * (s["at"] as Vector2) - cursor) / size
		if d.length_squared() < best:
			best = d.length_squared()
			selected = s
	queue_redraw()


func _add_shown(fill_inverse: Transform2D, position: Vector3, type: int, name: String, rotation: float,
		undiscovered: bool, entry: Dictionary) -> void:
	var on_screen: Variant = scene.project(position)
	if on_screen == null:
		return
	shown.append({"ref": int(entry.get("ref", 0)), "type": type, "name": name, "at": fill_inverse * (on_screen as Vector2),
		"rotation": rotation, "undiscovered": undiscovered, "entry": entry})


# ---- drawing

func _draw() -> void:
	if not _open or not UiAssets.available():
		return
	var top := {}
	for s: Dictionary in shown:
		if s == selected:
			top = s
			continue
		_draw_marker(s, false)
	if not top.is_empty():
		_draw_marker(top, true)
	_draw_bottom_bar()
	if _texts != null:
		_texts.draw_messages(self, size)
	if prompt != Prompt.NONE and MessageBoxArt.available():
		_boxes = MessageBoxArt.draw(self, size, prompt_text(), prompt_buttons(), prompt_choice)


func _draw_marker(s: Dictionary, over: bool) -> void:
	var type := int(s["type"])
	var clip_name: String = ICON_TYPES[type] if type < ICON_TYPES.size() else ""
	var id := UiAssets.exported(SWF, clip_name) if clip_name != "" else -1
	if id < 0:
		return
	var player := type == WorldMap.YOU_ARE_HERE_TYPE
	var xform := fill() * Transform2D(0.0, s["at"])
	if player:
		xform = xform * Transform2D(0.0, Vector2(PLAYER_SCALE, PLAYER_SCALE), 0.0, Vector2.ZERO)
	# "over" plays 10 frames (the icon grows to 1.4 and turns opaque, the name fades in): its end.
	var frame := maxi(UiAssets.label_frame(SWF, id, "over"), 0) + OVER_FRAMES if over else 0
	var look := UiDraw.Look.new()
	var icon_clip := UiAssets.child(SWF, id, "Icon")
	if player and not icon_clip.is_empty():
		# Only the arrow turns (MapMenu keeps YouAreHereMarker = the marker's Icon); its name stays level.
		look.hidden["Icon"] = true
		UiDraw.character(self, SWF, int(icon_clip["character"]),
			xform * Transform2D(deg_to_rad(float(s["rotation"])), Vector2.ZERO) * UiDraw.matrix(icon_clip["matrix"]))
	var text_clip := UiAssets.child(SWF, id, "TextClip")
	if over and not text_clip.is_empty():
		look.draw_texts = true
		look.stretch_texts = true
		look.texts["MarkerNameField"] = String(s["name"])
	UiDraw.character(self, SWF, id, xform, Color.WHITE, look, Rect2(), frame)
	if s["undiscovered"]:
		# CreateMarkers: the "<Icon>Undiscovered" clip on IconClip (Castle Karstaag's export is misnamed in the
		# movie, so the game never finds it either).
		# It is IconClip's child, so it takes IconClip's placement and colour at the marker's frame (alpha 0.6 up).
		var overlay := UiAssets.exported(SWF, clip_name + "Undiscovered")
		if overlay >= 0:
			for placed: Dictionary in UiAssets.display_list(SWF, id, frame):
				if placed.get("name", "") == "IconClip":
					UiDraw.character(self, SWF, overlay, xform * UiDraw.matrix(placed["matrix"]), UiDraw._tint(placed.get("color", [])))
	if over and text_clip.is_empty():
		var holder := UiAssets.exported(SWF, "DescriptionHolder")
		if holder >= 0:
			var hold := UiDraw.text_look()
			hold.texts["Title"] = String(s["name"])
			hold.hidden["LineItem0"] = true
			UiDraw.character(self, SWF, holder, fill() * Transform2D(0.0, s["at"]), Color.WHITE, hold, Rect2(), DESCRIPTION_FRAME)


## Bottom, locked to the bottom less the safe zone (MapMenu.onResize: Lock("B")), its buttons' labels and key art
## (CrossPlatformButtons: the art right before the text) and the date.
func _draw_bottom_bar() -> void:
	var bar := UiAssets.instance(SWF, "Bottom")
	if bar.is_empty():
		return
	var xform := fill() * Transform2D(0.0, Vector2(0.0, STAGE.y - HudGame.SAFE_ZONE.y))
	var look := UiDraw.text_look()
	look.texts["DateText"] = UiDraw.date_text(_session)
	for b: Array in BUTTONS:
		look.texts[b[0] + "/textField"] = UiDraw.translate(b[2])
	look.hidden["border"] = true  # the buttons' placeholder skin
	UiDraw.character(self, SWF, int(bar["id"]), xform, Color.WHITE, look)
	for b: Array in BUTTONS:
		var button := UiAssets.child(SWF, int(bar["id"]), b[0])
		var art := UiAssets.exported(SWF, b[1])
		if button.is_empty() or art < 0:
			continue
		var at: Transform2D = xform * UiDraw.matrix(button["matrix"])
		var art_bounds := UiAssets.bounds(SWF, art)
		var height := UiAssets.bounds(SWF, int(button["character"])).size.y
		UiDraw.character(self, SWF, art, at * Transform2D(0.0, Vector2(-art_bounds.size.x - art_bounds.position.x - 1.0,
			(height - art_bounds.size.y) * 0.5 - art_bounds.position.y)))


## For a shot's "menu" block.
func describe() -> Dictionary:
	var out := scene.describe() if scene != null else {}
	out["selected"] = String(selected.get("name", ""))
	out["markers"] = shown.size()
	out["prompt"] = Prompt.keys()[prompt]
	return out
