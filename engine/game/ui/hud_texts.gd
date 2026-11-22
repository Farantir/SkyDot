# SPDX-License-Identifier: GPL-3.0-or-later
#
# The HUD's texts as hudmenu.swf and its scripts do them (ADR 0010 step 4): the notification queue
# (HUDMenu.ShowMessage -> the Messages class: at most four lines, a new one at the top pushing the others down
# 30 px over 15 frames, the oldest fading out over its clip's own frames), the activation rollover (button,
# verb, name, rule, weight and value) and the general subtitle. Positions come from the movie's manifest and
# the locks of HUDMenu.InitExtensions (HudGame.placed). Pure drawing and a small simulation, no game state: the
# caller hands in what to show. A view (HudView, SubtitlesView) owns one and calls `step` each frame.
class_name HudTexts
extends RefCounted

const SWF := "hudmenu"
const BASE := "HUDMovieBaseInstance/"
const FPS := 24.0
## Messages.MAX_SHOWN, Y_SPACING (frames of 2 px), END_ANIM_FRAME as the movie sets them.
const MAX_SHOWN := 4
const Y_SPACING := 15
const END_FRAME := 80
## The MessageText clip: frames 0-13 fade in (it stops there), "FadeOut" starts at 14.
const FADE_OUT_FRAME := 14
const MESSAGE_SIZE := 24.0
const MESSAGE_BOX := Vector2(327.6, 59.7)
const NAME_SIZE := 24.0
## Menus stretch their stage to the window as the game does (UiDraw.stage_fill): positions follow the x and y scales,
## glyphs the x scale.
const SAFE := HudGame.SAFE_ZONE

## The info line's labels and numbers look bigger in the game than the movie's sizes (15, 24) give: measured.
const INFO_SIZE := 18.0
const VALUE_SIZE := 28.0
## The two lines' pitch and the bar, measured against the game (stage pixels).
const LINE_PITCH := 31.5
const BAR_HEIGHT := 2.0
const SUBTITLE_SIZE := 22.0
const GREY := Color("999999")
const SHADOW := Color(0.0, 0.0, 0.0, 0.8)
const NAME_DROP := 1.5
const INFO_DROP := 3.0
const BAR_DROP := 0.0
## The message block sits this much lower than its lock at the top left gives (measured at 1100x800).
const MESSAGE_DROP := 16.5

## AnimatedLetter (QuestUpdateBaseInstance.AnimatedLetter_mc): the quest-update line, "<NAME> DISCOVERED" among them.
const LETTER_CLIP := "AnimatedLetter_mc"
const LETTER_SPACE := 15.0  # AnimatedLetter.SpaceWidth
const LETTER_END := 104.0  # AnimatedLetter.EndPosition
const LETTER_TRIM := 5.0  # getLineMetrics(0).width - 5

var _queue: Array[String] = []
## The quest-update letters shown: {text, x (in AnimatedLetter_mc's parent), frame (the letter clip's)}.
var _letters: Array = []
var _letter_queue: Array[String] = []
## {text, frame (float, the clip's), y}
var _shown: Array = []
var _animating := false  # a new message is pushing the others down
var _fading := false  # the oldest is fading out: the next waits its turn (measured: a new one still comes in)
var _shift := 0
var _clock := 0.0


## Whether the pack can draw these texts.
static func available() -> bool:
	return HudGame.available()


## QuestNotification.ShowNotification with a text (AnimatedLetter.ShowQuestUpdate): the letters of `text` upper-cased,
## laid out centred on -EndPosition, one more appearing each frame, each playing the letter clip from StartAnim.
func show_quest_update(text: String) -> void:
	if not text.is_empty():
		_letter_queue.append(text.to_upper())


func has_quest_update() -> bool:
	return not _letters.is_empty() or not _letter_queue.is_empty()


func _start_letters(text: String) -> void:
	var font := _font()
	var px := _letter_px()
	var widths: Array[float] = []
	var total := 0.0
	for i in text.length():
		var w := font.get_string_size(text[i], HORIZONTAL_ALIGNMENT_LEFT, -1, px).x - LETTER_TRIM
		if text[i] == " " or w <= 0.0:
			w = LETTER_SPACE
		widths.append(w)
		total += w
	var start := -0.5 * total - LETTER_END
	var spacing := 0.0
	var old := 0.0
	_letters.clear()
	for i in text.length():
		var x := start + spacing + old * 0.5 + widths[i] * 0.5
		spacing = x - start
		old = widths[i]
		_letters.append({"text": text[i], "x": x, "frame": -float(i)})  # ShowLetter: one a frame


## The letter's field size in the letter clip's own pixels (LetterTextInstance's 55).
func _letter_px() -> int:
	return 55


func _letter_frames() -> int:
	return int(UiAssets.character(SWF, UiAssets.exported(SWF, LETTER_CLIP)).get("frames", 1))


## The letters, drawn as the letter clip at each one's frame over the quest update base.
func draw_quest_update(canvas: CanvasItem, area: Vector2) -> void:
	if _letters.is_empty():
		return
	var base := _place(area, BASE + "QuestUpdateBaseInstance")
	var clip := UiAssets.child(SWF, int(UiAssets.instance(SWF, BASE + "QuestUpdateBaseInstance").get("id", -1)), LETTER_CLIP)
	var id := UiAssets.exported(SWF, LETTER_CLIP)
	if clip.is_empty() or id < 0:
		return
	var placed := UiDraw.matrix(clip["matrix"])
	for letter: Dictionary in _letters:
		var frame := int(letter["frame"])
		if frame < 0:
			continue
		var look := UiDraw.Look.new()
		look.draw_texts = true
		look.stretch_texts = true
		look.texts["LetterTextInstance"] = String(letter["text"])
		var at := Transform2D(placed.x, placed.y, Vector2(float(letter["x"]), placed.origin.y))
		UiDraw.character(canvas, SWF, id, base * at, Color.WHITE, look, Rect2(), frame + 1)


## HUDMenu.ShowMessage: queue a notification.
func show_message(text: String) -> void:
	if not text.is_empty():
		_queue.append(text)


func has_messages() -> bool:
	return not _queue.is_empty() or not _shown.is_empty()


## The messages as they are on screen: [{text, y, alpha}], newest first. For tests.
func lines() -> Array:
	var out := []
	for i in range(_shown.size() - 1, -1, -1):
		var entry: Dictionary = _shown[i]
		out.append({"text": entry["text"], "y": entry["y"], "alpha": _alpha(entry)})
	return out


## Advance the queue by `delta` seconds (the movie's frames are 1/24 s).
func step(delta: float) -> void:
	_clock += delta * FPS
	while _clock >= 1.0:
		_clock -= 1.0
		_frame()


## One movie frame: the clips advance, then Messages.Update runs.
func _frame() -> void:
	if _letters.is_empty() and not _letter_queue.is_empty():
		_start_letters(_letter_queue.pop_front())
	elif not _letters.is_empty():
		var last := _letter_frames() - 1
		var done := true
		for letter: Dictionary in _letters:
			letter["frame"] = float(letter["frame"]) + 1.0
			done = done and int(letter["frame"]) >= last
		if done:
			_letters.clear()
	for entry: Dictionary in _shown:
		var frame: float = entry["frame"]
		if frame < FADE_OUT_FRAME - 1 or entry["fading"]:
			entry["frame"] = frame + 1.0
	# the onEnterFrame of a new message: the others move 2 px a frame while ySpacing runs
	if _animating and _shift < Y_SPACING:
		for i in range(_shown.size() - 1):
			_shown[i]["y"] += 2.0
		_shift += 1
		if _shift == Y_SPACING:
			_animating = false
			if _queue.is_empty() or _shown.size() == MAX_SHOWN:
				_fade_oldest()
	_update()


func _update() -> void:
	var queued := not _queue.is_empty()
	if queued and not _animating and _shown.size() < MAX_SHOWN:
		_shown.append({"text": _queue.pop_front(), "frame": 0.0, "y": 0.0, "fading": false})
		_animating = true
		_shift = 0
	var i := 0
	while i < _shown.size():
		if float(_shown[i]["frame"]) + 1.0 >= END_FRAME:  # _currentFrame is 1-based
			_shown.remove_at(i)
			_fading = false
		else:
			i += 1
	if _queue.is_empty() and not _animating and not _fading and not _shown.is_empty():
		_fade_oldest()


func _fade_oldest() -> void:
	if _shown.is_empty():
		return
	var oldest: Dictionary = _shown[0]
	if oldest["fading"]:
		return
	oldest["fading"] = true
	_fading = true
	oldest["frame"] = float(FADE_OUT_FRAME)


## The MessageText clip's alpha at its frame (its colour transform on TextFieldClip).
func _alpha(entry: Dictionary) -> float:
	var frame := int(entry["frame"])
	var list := UiAssets.display_list(SWF, UiAssets.exported(SWF, "MessageText"), frame)
	for placed: Dictionary in list:
		if placed.get("name", "") == "TextFieldClip":
			var color: Array = placed.get("color", [])
			return float(color[3]) if color.size() > 3 else 1.0
	return 1.0


func draw_messages(canvas: CanvasItem, area: Vector2) -> void:
	var xform := _place(area, BASE + "MessagesBlock", "TL")
	var font := _font()
	var scale := xform.get_scale().x
	var ys := xform.get_scale().y
	for entry: Dictionary in _shown:
		var alpha := _alpha(entry)
		if alpha <= 0.0:
			continue
		var origin := xform * Vector2(2.4, 2.4 + MESSAGE_DROP + float(entry["y"]))
		var text: String = entry["text"]
		var size := MESSAGE_SIZE
		var lines := _wrap(font, text, size * scale, (MESSAGE_BOX.x - 4.0) * scale)
		while lines.size() * size * 1.17 > MESSAGE_BOX.y - 2.0 and size > 10.0:  # textAutoSize "shrink"
			size -= 1.0
			lines = _wrap(font, text, size * scale, (MESSAGE_BOX.x - 4.0) * scale)
		var px := int(round(size * scale))
		for k in lines.size():
			var at := origin + Vector2(0.0, k * size * 1.17 * ys + font.get_ascent(px))
			_draw(canvas, font, lines[k], at, px, Color(1, 1, 1, alpha), HORIZONTAL_ALIGNMENT_LEFT, -1.0)


## The activation rollover: `prompt` as GameSession.activation_target gives it, `key` the activate key's name.
func draw_rollover(canvas: CanvasItem, area: Vector2, prompt: Dictionary, key: String, lock_text := "") -> void:
	if prompt.is_empty():
		return
	var name_xform := _field(area, "RolloverNameInstance", 0.0, NAME_DROP)
	var font := _font()
	var scale := name_xform.get_scale().x
	var px := int(round(NAME_SIZE * scale))
	var width := (1280.0 - 4.0) * scale
	var verb: String = prompt["verb"]
	var line_pitch := LINE_PITCH * name_xform.get_scale().y
	var top := name_xform.origin
	var verb_at := top + Vector2(0.0, font.get_ascent(px))
	_draw(canvas, font, verb, verb_at, px, Color.WHITE, HORIZONTAL_ALIGNMENT_CENTER, width)
	var name_at := verb_at + Vector2(0.0, line_pitch)
	_draw(canvas, font, prompt["name"], name_at, px, Color.WHITE, HORIZONTAL_ALIGNMENT_CENTER, width)
	if not lock_text.is_empty():
		_draw(canvas, font, lock_text, name_at + Vector2(0.0, line_pitch), int(px * 0.8), Color(0.85, 0.8, 0.6),
			HORIZONTAL_ALIGNMENT_CENTER, width)
	# ActivateButton_tf: right aligned, ending 3 px before the first line starts (HUDMenu.SetCrosshairTarget)
	var verb_width := font.get_string_size(verb, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x
	var line_left := top.x + (width - verb_width) * 0.5
	_draw_key(canvas, area, key, Vector2(line_left - 7.0 * scale, top.y), scale)
	var weight: float = prompt.get("weight", -1.0)
	var value: int = prompt.get("value", -1)
	if weight < 0.0 and value < 0:
		return
	var bar := _field(area, "GrayBarInstance", 0.0, BAR_DROP)
	canvas.draw_rect(Rect2(bar.origin, Vector2(258.4 * bar.get_scale().x, BAR_HEIGHT * bar.get_scale().y)), Color(0.75, 0.75, 0.75, 0.85))
	_draw_info(canvas, area, font, weight, value)


## RolloverInfoText: "WEIGHT <b>0.3</b>      VALUE <b>2</b>", the labels small and grey, the numbers big and white.
func _draw_info(canvas: CanvasItem, area: Vector2, font: Font, weight: float, value: int) -> void:
	var xform := _field(area, "RolloverInfoInstance", 0.0, INFO_DROP)
	var scale := xform.get_scale().x
	var small := int(round(INFO_SIZE * scale))
	var big := int(round(VALUE_SIZE * scale))
	var bold := UiAssets.font("$EverywhereBoldFont")
	if bold == null:
		bold = font
	var parts := []
	if weight >= 0.0:
		parts.append(["WEIGHT", _number(weight)])
	if value >= 0:
		parts.append(["VALUE", str(value)])
	var total := 0.0
	var gap := 2.0 * scale
	var gap_between := 22.0 * scale
	for part: Array in parts:
		total += font.get_string_size(part[0], HORIZONTAL_ALIGNMENT_LEFT, -1, small).x + gap \
			+ bold.get_string_size(part[1], HORIZONTAL_ALIGNMENT_LEFT, -1, big).x + gap_between
	total -= gap_between
	var x := xform.origin.x + (658.0 * scale - total) * 0.5
	var baseline := xform.origin.y + bold.get_ascent(big)
	for part: Array in parts:
		_draw(canvas, font, part[0], Vector2(x, baseline), small, GREY, HORIZONTAL_ALIGNMENT_LEFT, -1.0)
		x += font.get_string_size(part[0], HORIZONTAL_ALIGNMENT_LEFT, -1, small).x + gap
		_draw(canvas, bold, part[1], Vector2(x, baseline), big, Color.WHITE, HORIZONTAL_ALIGNMENT_LEFT, -1.0)
		x += bold.get_string_size(part[1], HORIZONTAL_ALIGNMENT_LEFT, -1, big).x + gap_between


## The weight as GlobalFunc.RoundDecimal(w, 1) prints it: "0.3", "12", "1.5".
static func _number(weight: float) -> String:
	var rounded := roundf(weight * 10.0) / 10.0
	return str(int(rounded)) if is_equal_approx(rounded, floorf(rounded)) else "%.1f" % rounded


## The key's art (an exported sprite named like the key: "E", "Tab", ...), else its name in a plain cap.
func _draw_key(canvas: CanvasItem, area: Vector2, key: String, right_top: Vector2, scale: float) -> void:
	if key.is_empty():
		return
	var id := UiAssets.exported(SWF, key)
	var size := 26.0 * scale
	var at := right_top + Vector2(-size, 0.0)
	if id >= 0:
		UiDraw.character(canvas, SWF, id, Transform2D(Vector2(scale, 0.0), Vector2(0.0, scale), at))
		return
	var font := _font()
	var px := int(round(NAME_SIZE * scale * 0.8))
	canvas.draw_string(font, at + Vector2(0.0, font.get_ascent(px)), key, HORIZONTAL_ALIGNMENT_LEFT, -1, px)


## The general subtitle: the SubtitleTextHolder's field, locked to the bottom as InitExtensions does.
func draw_subtitle(canvas: CanvasItem, area: Vector2, text: String) -> void:
	if text.is_empty():
		return
	var found := UiAssets.instance(SWF, BASE + "SubtitleTextHolder/textField")
	if found.is_empty():
		return
	var holder := _place(area, BASE + "SubtitleTextHolder", "B")
	var field: Dictionary = UiAssets.instance(SWF, BASE + "SubtitleTextHolder/textField")
	var offset: Vector2 = (field["xform"] as Transform2D).origin - (UiAssets.instance(SWF, BASE + "SubtitleTextHolder")["xform"] as Transform2D).origin
	var xform := holder * Transform2D(0.0, offset)
	var scale := xform.get_scale().x
	var font := _font()
	var px := int(round(SUBTITLE_SIZE * scale))
	var width := (800.0 - 4.0) * scale
	var lines := _wrap(font, text, px, width)
	# verticalAutoSize "bottom": the last line ends at the field's bottom (62.5 high)
	var bottom := xform.origin.y + 62.5 * xform.get_scale().y
	var pitch := font.get_height(px) + 2.0 * scale
	for k in lines.size():
		var y := bottom - (lines.size() - 1 - k) * pitch - font.get_descent(px)
		_draw(canvas, font, lines[k], Vector2(xform.origin.x, y), px, Color.WHITE, HORIZONTAL_ALIGNMENT_CENTER, width)


## An instance's transform in the view: the stage stretched to the window, its origin moved as GlobalFunc.Lock(`lock`)
## moves it (the whole stage is visible, so the safe zone is the only inset).
static func _place(area: Vector2, path: String, lock := "") -> Transform2D:
	var found := UiAssets.instance(SWF, path)
	if found.is_empty():
		return Transform2D()
	var xform: Transform2D = found["xform"]
	if lock.contains("T"):
		xform.origin.y = SAFE.y
	if lock.contains("B"):
		xform.origin.y = HudGame.STAGE.y - SAFE.y
	if lock.contains("L"):
		xform.origin.x = SAFE.x
	if lock.contains("R"):
		xform.origin.x = HudGame.STAGE.x - SAFE.x
	return UiDraw.stage_fill(area, HudGame.STAGE) * xform


## A field's transform in the view, moved down by `drop` stage pixels.
func _field(area: Vector2, instance: String, _unused: float, drop: float) -> Transform2D:
	var xform := _place(area, BASE + instance)
	xform.origin += Vector2(0.0, drop * xform.get_scale().y)
	return xform


static func _font() -> Font:
	var font := UiAssets.font("$EverywhereMediumFont")
	return font if font != null else ThemeDB.fallback_font


func _draw(canvas: CanvasItem, font: Font, text: String, at: Vector2, px: int, colour: Color,
		align: HorizontalAlignment, width: float) -> void:
	canvas.draw_string_outline(font, at, text, align, width, px, 3, Color(SHADOW, SHADOW.a * colour.a))
	canvas.draw_string(font, at, text, align, width, px, colour)


static func _wrap(font: Font, text: String, px: float, width: float) -> Array[String]:
	var lines: Array[String] = []
	var current := ""
	for word in text.split(" "):
		var trial := word if current.is_empty() else current + " " + word
		if font.get_string_size(trial, HORIZONTAL_ALIGNMENT_LEFT, -1, int(px)).x > width and not current.is_empty():
			lines.append(current)
			current = word
		else:
			current = trial
	lines.append(current)
	return lines
