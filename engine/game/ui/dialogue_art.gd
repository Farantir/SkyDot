# SPDX-License-Identifier: GPL-3.0-or-later
#
# The dialogue menu in dialoguemenu.swf's layout (ADR 0010 step 4): the soft dark panel behind the topics on the
# right, the topics in a list centred on the chosen one (DialogueCenteredList: 8 slots of 40 px in a 0.8-scaled
# holder, the chosen at slot 4 at full alpha and the others at 60 %, a topic not yet said white and said ones
# 0x606060), the speaker's name at the middle of the screen, the spoken line bottom aligned in an 800 px field, and
# the Goodbye button at the bottom right. Static drawing from view-model values: `draw(canvas, area, state)`.
# Without the pack's menus `available()` is false and DialogueView keeps its plain text.
class_name DialogueArt
extends RefCounted

const SWF := "dialoguemenu"
const ROOT := Vector2(599.55, 309.7)
const HOLDER := Vector2(148.1, -220.0)
const HOLDER_SCALE := 0.8
const LIST_AT := Vector2(50.0, 270.0)
const SLOTS := 8
const CENTRE_SLOT := 4
const PITCH := 40.0
const SELECTED_EXTRA := 5.0
const TEXT_SIZE := 24.0
const TEXT_DY := 5.75
const SELECTED_DY := 2.5
const SUBTITLE_AT := Vector2(-351.0, 267.0)
const SUBTITLE_SIZE := 22.0
const SUBTITLE_BOX := Vector2(800.0, 65.9)
const SPEAKER_AT := Vector2(-53.0, 126.2)
const SPEAKER_BOX := Vector2(200.0, 32.5)
const EXIT_AT := Vector2(345.0, 360.0)
const EXIT_SIZE := 20.0
const SAID := Color(0.376, 0.376, 0.376)
const GOODBYE := "Goodbye"


static func available() -> bool:
	return UiAssets.available() and UiAssets.exported(SWF, "TopicList") >= 0


## The stage fitted to `area` (the whole stage visible, centred).
static func fit(area: Vector2) -> Transform2D:
	var scale := minf(area.x / 1280.0, area.y / 720.0)
	return Transform2D(Vector2(scale, 0.0), Vector2(0.0, scale), area * 0.5 - Vector2(640.0, 360.0) * scale)


## `entries`: [{text, said}] the topics in order; `selected` the chosen index (entries.size() = Goodbye); `speaker`
## and `line` the NPC's name and the line being spoken; `with_topics` whether the topic list is up.
static func draw(canvas: CanvasItem, area: Vector2, speaker: String, line: String, entries: Array, selected: int,
		with_topics: bool, key: String = "") -> void:
	var stage := fit(area)
	var scale := stage.get_scale().x
	var font := _font()
	if with_topics:
		_draw_topics(canvas, stage, font, entries, selected, scale)
		_draw_exit(canvas, stage, font, selected >= entries.size(), key, scale)
	if not speaker.is_empty():
		var at := stage * (ROOT + SPEAKER_AT)
		_text(canvas, font, speaker, at + Vector2(0.0, font.get_ascent(_px(SUBTITLE_SIZE, scale))),
			_px(SUBTITLE_SIZE, scale), Color.WHITE, HORIZONTAL_ALIGNMENT_CENTER, (SPEAKER_BOX.x - 4.0) * scale)
	if not line.is_empty():
		_draw_line(canvas, stage, font, line, scale)


static func _draw_topics(canvas: CanvasItem, stage: Transform2D, font: Font, entries: Array, selected: int,
		scale: float) -> void:
	var holder := stage * Transform2D(Vector2(HOLDER_SCALE, 0.0), Vector2(0.0, HOLDER_SCALE), ROOT + HOLDER)
	_draw_instance(canvas, stage, "ListPanel")
	if entries.size() > 1:
		_draw_instance(canvas, stage, "ScrollIndicators")
	var px := _px(TEXT_SIZE * HOLDER_SCALE, scale)
	var centre := mini(selected, entries.size() - 1)
	for slot in SLOTS:
		var index := centre - CENTRE_SLOT + slot
		if index < 0 or index >= entries.size():
			continue
		var entry: Dictionary = entries[index]
		var chosen := slot == CENTRE_SLOT and selected < entries.size()
		var y := LIST_AT.y + PITCH * slot + (SELECTED_EXTRA if slot > CENTRE_SLOT else 0.0) \
			+ TEXT_DY + (SELECTED_DY if slot == CENTRE_SLOT else 0.0)
		var at := holder * Vector2(LIST_AT.x + 2.0, y)
		var colour: Color = SAID if entry.get("said", false) else Color.WHITE
		colour.a = 1.0 if chosen else 0.6
		_text(canvas, font, entry["text"], at + Vector2(0.0, font.get_ascent(px)), px, colour,
			HORIZONTAL_ALIGNMENT_LEFT, -1.0)


## The Goodbye button: its text (the movie's Button skin is a placeholder) with the key's art left of it.
static func _draw_exit(canvas: CanvasItem, stage: Transform2D, font: Font, chosen: bool, key: String,
		scale: float) -> void:
	var px := _px(EXIT_SIZE, scale)
	var at := stage * (ROOT + EXIT_AT + Vector2(2.0, 2.0))
	var colour := Color(1, 1, 1, 1.0 if chosen else 0.6)
	var width := font.get_string_size(GOODBYE, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x
	at.x -= width * 0.5 - 60.0 * scale
	var id := UiAssets.exported(SWF, key) if not key.is_empty() else -1
	if id >= 0:
		UiDraw.character(canvas, SWF, id, Transform2D(Vector2(scale, 0.0), Vector2(0.0, scale),
			at + Vector2(-30.0 * scale, -2.0 * scale)), Color(1, 1, 1, colour.a))
	_text(canvas, font, GOODBYE, at + Vector2(0.0, font.get_ascent(px)), px, colour, HORIZONTAL_ALIGNMENT_LEFT, -1.0)


## SubtitleText: centred in its 800 px field, the last line at the field's bottom.
static func _draw_line(canvas: CanvasItem, stage: Transform2D, font: Font, line: String, scale: float) -> void:
	var px := _px(SUBTITLE_SIZE, scale)
	var width := (SUBTITLE_BOX.x - 4.0) * scale
	var lines := _wrap(font, line, px, width)
	var top := stage * (ROOT + SUBTITLE_AT)
	var pitch := font.get_height(px) + 2.0 * scale
	var bottom := top.y + SUBTITLE_BOX.y * scale
	for k in lines.size():
		var y := bottom - (lines.size() - 1 - k) * pitch - font.get_descent(px)
		_text(canvas, font, lines[k], Vector2(top.x, y), px, Color.WHITE, HORIZONTAL_ALIGNMENT_CENTER, width)


## A child of the topic holder as the movie places it.
static func _draw_instance(canvas: CanvasItem, stage: Transform2D, name: String) -> void:
	var found := UiAssets.instance(SWF, "DialogueMenu_mc/TopicListHolder/" + name)
	if not found.is_empty():
		UiDraw.character(canvas, SWF, int(found["id"]), stage * (found["xform"] as Transform2D))


static func _px(size: float, scale: float) -> int:
	return maxi(int(round(size * scale)), 6)


static func _text(canvas: CanvasItem, font: Font, text: String, at: Vector2, px: int, colour: Color,
		align: HorizontalAlignment, width: float) -> void:
	canvas.draw_string_outline(font, at, text, align, width, px, 3, Color(0, 0, 0, 0.8 * colour.a))
	canvas.draw_string(font, at, text, align, width, px, colour)


static func _font() -> Font:
	var font := UiAssets.font("$EverywhereMediumFont")
	return font if font != null else ThemeDB.fallback_font


static func _wrap(font: Font, text: String, px: int, width: float) -> Array[String]:
	var lines: Array[String] = []
	var current := ""
	for word in text.split(" "):
		var trial := word if current.is_empty() else current + " " + word
		if font.get_string_size(trial, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x > width and not current.is_empty():
			lines.append(current)
			current = word
		else:
			current = trial
	lines.append(current)
	return lines
