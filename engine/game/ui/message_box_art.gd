# SPDX-License-Identifier: GPL-3.0-or-later
#
# A message box as messagebox.swf and its MessageBox class lay it out (ADR 0010 step 4): the message centred in a
# field 350 wide, a divider, the buttons in a row (each as wide as its text plus 29, 25 apart), the background a
# 9-slice sized to the wider of buttons + 40 and the widest line + 60, centred on the screen; the chosen button
# has the movie's selection indicator under its text. Used by the death screen; any other box (Debug.MessageBox)
# can use it. Static: `layout` says where everything is, `draw` draws it. Without the pack's menus
# `available()` is false and the caller keeps its own look.
class_name MessageBoxArt
extends RefCounted

const SWF := "messagebox"
const CENTRE := Vector2(640.05, 360.15)
const STAGE := Vector2(1280.0, 720.0)
const TEXT_WIDTH := 350.0
const TEXT_SIZE := 22.0
const WIDTH_MARGIN := 20.0
const HEIGHT_MARGIN := 30.0
const SPACER := 10.0
const INDICATOR_WIDTH := 25.0
const INDICATOR_HEIGHT := 5.0
## A button's text field centre below its origin and the field's height when it holds one line (the movie's
## ButtonText at y -12.65, 55.1 high, vertically centred on its text).
const BUTTON_TEXT_CENTRE := 14.9
const BUTTON_PAD := 4.0
const BUTTON_GAP := 25.0
const LINE_LEADING := 2.0
## Measured against the game (1100x800): the buttons sit lower and the divider higher than the script's arithmetic gives.
const BUTTON_DROP := 8.5
const DIVIDER_RISE := 10.5
## The game's box is taller than the script's arithmetic gives (measured at 1100x800); the text keeps its place.
const EXTRA_HEIGHT := 6.3


static func available() -> bool:
	return UiAssets.available() and UiAssets.exported(SWF, "MessageBoxButton") >= 0


## Glyph scale (the x scale: the game stretches menus to the window, glyphs keep x).
static func scale_for(area: Vector2) -> float:
	return area.x / STAGE.x


## Stage pixels to view pixels: the stage stretched to the window.
static func _to_view(area: Vector2, stage: Vector2) -> Vector2:
	return UiDraw.stage_fill(area, STAGE) * stage


static func _size(area: Vector2, stage_size: Vector2) -> Vector2:
	return stage_size * Vector2(area.x / STAGE.x, area.y / STAGE.y)


## Where everything goes, in view pixels: {background: Rect2, divider: Rect2, lines: [{text, at (baseline left of
## the line's box), width}], buttons: [{text, rect (hit area), text_at, indicator: Rect2}], px (font size)}.
static func layout(area: Vector2, text: String, buttons: Array) -> Dictionary:
	var scale := scale_for(area)
	var font := _font()
	var px := int(round(TEXT_SIZE * scale))
	var line_h := font.get_height(px) / scale + LINE_LEADING
	var lines := UiDraw.wrap(font, text, px, (TEXT_WIDTH - 4.0) * scale)
	var widest := 0.0
	for line in lines:
		widest = maxf(widest, font.get_string_size(line, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x / scale)
	var message_h := lines.size() * line_h + 4.0
	var widths: Array[float] = []
	var total := 0.0
	for label in buttons:
		var w := font.get_string_size(label, HORIZONTAL_ALIGNMENT_LEFT, -1, px).x / scale + BUTTON_PAD + INDICATOR_WIDTH
		widths.append(w)
		total += w
	total += BUTTON_GAP * maxi(buttons.size() - 1, 0)
	var button_h := line_h + BUTTON_PAD  # the text field once the movie has fitted it to one line
	var bg_w := maxf(total + WIDTH_MARGIN * 2.0, widest + 60.0)
	var bg_h := message_h + button_h + HEIGHT_MARGIN * 2.0 + SPACER + EXTRA_HEIGHT
	var top := -bg_h / 2.0
	var out := {"px": px, "scale": scale, "lines": [], "buttons": []}
	out["background"] = Rect2(_to_view(area, CENTRE + Vector2(-bg_w / 2.0, top)), _size(area, Vector2(bg_w, bg_h)))
	var message_top := top + HEIGHT_MARGIN + EXTRA_HEIGHT / 2.0
	for i in lines.size():
		var at := _to_view(area, CENTRE + Vector2(-TEXT_WIDTH / 2.0, message_top + 2.0 + i * line_h)) \
			+ Vector2(0.0, font.get_ascent(px))
		out["lines"].append({"text": lines[i], "at": at, "width": (TEXT_WIDTH - 4.0) * scale})
	var button_centre := bg_h / 2.0 - HEIGHT_MARGIN - button_h / 2.0 + BUTTON_DROP
	var divider_y := button_centre - BUTTON_DROP - button_h / 2.0 - SPACER / 2.0 - 2.0 - DIVIDER_RISE
	out["divider"] = Rect2(_to_view(area, CENTRE + Vector2(-(bg_w - WIDTH_MARGIN * 2.0) / 2.0, divider_y - 1.15)),
		_size(area, Vector2(bg_w - WIDTH_MARGIN * 2.0, 2.3)))
	var x := -total / 2.0
	for i in buttons.size():
		var centre := Vector2(x + widths[i] / 2.0, button_centre)
		var rect := Rect2(_to_view(area, CENTRE + centre - Vector2(widths[i] / 2.0, button_h / 2.0)),
			_size(area, Vector2(widths[i], button_h)))
		var label_w := font.get_string_size(buttons[i], HORIZONTAL_ALIGNMENT_LEFT, -1, px).x
		var text_at := Vector2(rect.get_center().x - label_w * 0.5, rect.get_center().y) \
			+ Vector2(0.0, (font.get_ascent(px) - font.get_descent(px)) * 0.5)
		var indicator_w := (widths[i] - BUTTON_PAD) * scale + 4.0 * scale
		var indicator := Rect2(rect.get_center() + Vector2(-indicator_w / 2.0, -14.6 * scale * 0.5),
			Vector2(indicator_w, 14.6 * scale))
		out["buttons"].append({"text": buttons[i], "rect": rect, "text_at": text_at, "indicator": indicator})
		x += widths[i] + BUTTON_GAP
	return out


## Draw the box: `buttons` are the labels, `selected` the chosen one (-1 none). Returns the hit rectangles of the
## buttons (view pixels) for the mouse.
static func draw(canvas: CanvasItem, area: Vector2, text: String, buttons: Array, selected: int) -> Array[Rect2]:
	var boxes: Array[Rect2] = []
	var look := layout(area, text, buttons)
	var scale: float = look["scale"]
	var px: int = look["px"]
	var font := _font()
	_nine_slice(canvas, 11, look["background"], scale)
	_stretch(canvas, 13, look["divider"], scale)
	for line: Dictionary in look["lines"]:
		_text(canvas, font, line["text"], line["at"], px, HORIZONTAL_ALIGNMENT_CENTER, line["width"])
	for i in buttons.size():
		var button: Dictionary = look["buttons"][i]
		boxes.append(button["rect"])
		if i == selected and buttons.size() > 1:  # the game shows no indicator on a lone button
			_nine_slice(canvas, 5, button["indicator"], scale)
		_text(canvas, font, button["text"], button["text_at"], px, HORIZONTAL_ALIGNMENT_LEFT, -1.0)
	return boxes


static func _text(canvas: CanvasItem, font: Font, text: String, at: Vector2, px: int, align: HorizontalAlignment,
		width: float) -> void:
	canvas.draw_string_outline(font, at, text, align, width, px, 3, Color(0, 0, 0, 0.8))
	canvas.draw_string(font, at, text, align, width, px, Color.WHITE)


## A sprite with a scaling grid drawn into `rect`.
static func _nine_slice(canvas: CanvasItem, id: int, rect: Rect2, scale: float) -> void:
	var texture := UiAssets.texture(SWF, id, scale)
	var grid := UiAssets.scaling_grid(SWF, id)
	var bounds := UiAssets.bounds(SWF, id)
	if texture == null:
		return
	if grid.size == Vector2.ZERO:
		canvas.draw_texture_rect(texture, rect, false)
		return
	var box := StyleBoxTexture.new()
	box.texture = texture
	box.texture_margin_left = (grid.position.x - bounds.position.x) * scale
	box.texture_margin_top = (grid.position.y - bounds.position.y) * scale
	box.texture_margin_right = (bounds.end.x - grid.end.x) * scale
	box.texture_margin_bottom = (bounds.end.y - grid.end.y) * scale
	canvas.draw_style_box(box, rect)


static func _stretch(canvas: CanvasItem, id: int, rect: Rect2, scale: float) -> void:
	var texture := UiAssets.texture(SWF, id, scale)
	if texture != null:
		canvas.draw_texture_rect(texture, rect, false)


static func _font() -> Font:
	var font := UiAssets.font("$EverywhereMediumFont")
	return font if font != null else ThemeDB.fallback_font
