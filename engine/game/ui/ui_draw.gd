# SPDX-License-Identifier: GPL-3.0-or-later
#
# Draws a character of a game movie (UiAssets) on a CanvasItem the way its display list places it: shapes as
# textures of their SVGs at their full transform, sprites through their children at the frame asked for
# (UiAssets.display_list steps the timeline), masks (a clip depth) as rectangles. It runs no ActionScript, so
# what a script decides is the caller's, through a Look:
#   frames      character id -> frame: a sprite drawn at that frame instead of 0 (a meter's fade, a compass)
#   fills       instance name -> 0 to 1, for the meters: a sprite with "Full" and "Empty" labels is drawn at
#               the frame between them, as Components.Meter.SetPercent does (gotoAndStop of the percent's
#               frame); one not named there stands at 0 (Empty: a penalty meter with no penalty)
#   moves       instance name -> Vector2: its position in its parent, as a script's _x/_y would set it
#
# Assumes no rotation or skew where masks are: they are the axis-aligned bounds of their shapes.
class_name UiDraw
extends RefCounted


## Translation keys whose English text is not the key (interface/translate_english.txt).
const TRANSLATED := {"Player Gold": "Gold"}


class Look:
	var frames := {}
	var fills := {}
	var moves := {}
	## Draw the movie's text fields (edit_text): off by default. `texts` instance name (or "#id" of the character) -> text (a field inside a
	## named sprite can be keyed "Parent/field"); a field not named keeps its authored text, "$Key" shown as "Key".
	var draw_texts := false
	var texts := {}
	## instance name -> frame: a named sprite (a button's "over") drawn at that frame.
	var named_frames := {}
	## Instance names not drawn (a placeholder skin the game's script replaces).
	var hidden := {}
	var colours := {}  # instance name -> Color for a text field
	## Glyphs stretched with the stage (field's `stretch`), as the game draws them.
	var stretch_texts := false


## Draw `id` of `swf` at `frame` with `xform` (movie pixels to the canvas) times `modulate`; `clip`, if not
## empty, is a rectangle in the character's own pixels that it is drawn inside.
static func character(canvas: CanvasItem, swf: String, id: int, xform: Transform2D, modulate := Color.WHITE,
		look: Look = null, clip := Rect2(), frame := 0, name := "") -> void:
	var entry := UiAssets.character(swf, id)
	if look != null and look.hidden.has(name):
		return
	match entry.get("type", ""):
		"shape":
			_shape(canvas, swf, id, xform, modulate, clip)
		"sprite":
			_sprite(canvas, swf, entry, xform, modulate, look if look != null else Look.new(), clip, frame, name)
		"edit_text":
			if look != null and look.draw_texts:
				var text: String = look.texts.get("#%d" % id, look.texts.get(name, _plain(String(entry.get("text", "")))))
				if text != "":
					field(canvas, swf, id, xform, text, look.colours.get(name, Color(0.0, 0.0, 0.0, 0.0)), look.stretch_texts)


## A field's authored text without its markup; "$Key" (a translation key) as "Key".
static func _plain(text: String) -> String:
	var re := RegEx.new()
	re.compile("<[^>]*>")
	text = re.sub(text, "", true).strip_edges()
	text = text.substr(1) if text.begins_with("$") else text
	return TRANSLATED.get(text, text)


static func matrix(values: Array) -> Transform2D:
	return Transform2D(Vector2(values[0], values[1]), Vector2(values[2], values[3]), Vector2(values[4], values[5]))


static func _tint(values: Array) -> Color:
	return Color(values[0], values[1], values[2], values[3]) if values.size() == 8 else Color.WHITE


static func _shape(canvas: CanvasItem, swf: String, id: int, xform: Transform2D, modulate: Color, clip: Rect2) -> void:
	var bounds := UiAssets.bounds(swf, id)
	if bounds.size == Vector2.ZERO or modulate.a <= 0.0:
		return
	var raster := maxf(xform.x.length(), xform.y.length())
	var texture := UiAssets.texture(swf, id, raster)
	if texture == null:
		return
	var shown := bounds
	if clip.size != Vector2.ZERO:
		shown = bounds.intersection(clip)
		if shown.size.x <= 0.0 or shown.size.y <= 0.0:
			return
	var per_px := Vector2(texture.get_width(), texture.get_height()) / bounds.size
	var source := Rect2((shown.position - bounds.position) * per_px, shown.size * per_px)
	canvas.draw_set_transform_matrix(xform)
	canvas.draw_texture_rect_region(texture, shown, source, modulate)
	canvas.draw_set_transform_matrix(Transform2D.IDENTITY)


static func _sprite(canvas: CanvasItem, swf: String, entry: Dictionary, xform: Transform2D, modulate: Color,
		look: Look, clip: Rect2, frame: int, name: String) -> void:
	var id := int(entry["id"])
	if look.named_frames.has(name):
		frame = int(look.named_frames[name])
	elif look.frames.has(id):
		frame = int(look.frames[id])
	elif not look.fills.is_empty():
		var empty := UiAssets.label_frame(swf, id, "Empty")
		if empty > 0:
			frame = int(round((1.0 - clampf(float(look.fills.get(name, 0.0)), 0.0, 1.0)) * empty))
	var list := UiAssets.display_list(swf, id, frame)
	var i := 0
	while i < list.size():
		var placed: Dictionary = list[i]
		var local := _placed(placed, look)
		if not placed.has("clip_depth"):
			var child_name: String = placed.get("name", "")
			if look.texts.has(name + "/" + child_name):
				child_name = name + "/" + child_name
			character(canvas, swf, int(placed["character"]), xform * local, modulate * _tint(placed.get("color", [])),
				look, _into(local, clip), 0, child_name)
			i += 1
			continue
		# A mask: the depths up to clip_depth are drawn inside its bounds.
		var mask := local * UiAssets.bounds(swf, int(placed["character"]))
		if clip.size != Vector2.ZERO:
			mask = mask.intersection(clip)
		i += 1
		while i < list.size() and int(list[i]["depth"]) <= int(placed["clip_depth"]):
			var inner: Dictionary = list[i]
			var inner_local := _placed(inner, look)
			character(canvas, swf, int(inner["character"]), xform * inner_local,
				modulate * _tint(inner.get("color", [])), look, _into(inner_local, mask), 0, inner.get("name", ""))
			i += 1


static func _placed(placed: Dictionary, look: Look) -> Transform2D:
	var local := matrix(placed["matrix"])
	var name: String = placed.get("name", "")
	if name != "" and look.moves.has(name):
		local.origin = look.moves[name]
	return local


## A clip rectangle of the parent's space in the child's (placed with `local`).
static func _into(local: Transform2D, clip: Rect2) -> Rect2:
	if clip.size == Vector2.ZERO:
		return clip
	return (local.affine_inverse() * clip).abs()


## A text field (edit_text) of a movie, drawn in its box as the movie places it: `xform` is movie pixels to the canvas
## (the field's own placement included). Font, size and alignment are the field's; `colour` replaces its colour when
## given. Text gets a dark outline (the movies' DropShadow). Returns the drawn size of the text in canvas pixels.
## `stretch`: the glyphs are stretched by the transform's y/x scale ratio, as Scaleform's exact-fit stage draws them
## (measured in the journal's game captures at 1100x800: glyphs 1.29 times as tall as the x scale makes them);
## without it they keep the x scale's proportions.
static func field(canvas: CanvasItem, swf: String, id: int, xform: Transform2D, text: String,
		colour := Color(0.0, 0.0, 0.0, 0.0), stretch := false) -> Vector2:
	var entry := UiAssets.character(swf, id)
	if entry.get("type", "") != "edit_text":
		return Vector2.ZERO
	var font := UiAssets.font(String(entry.get("font", "")))
	if font == null:
		font = ThemeDB.fallback_font
	var scale := xform.get_scale().x
	var size_px := maxi(int(round(float(entry.get("height", 20.0)) * scale)), 6)
	var box := UiAssets.bounds(swf, id)
	var left := xform * Vector2(box.position.x + 2.0, 0.0)
	var width := (box.size.x - 4.0) * scale
	var align := HORIZONTAL_ALIGNMENT_LEFT
	match String(entry.get("align", "left")):
		"center":
			align = HORIZONTAL_ALIGNMENT_CENTER
		"right":
			align = HORIZONTAL_ALIGNMENT_RIGHT
	var base := xform * Vector2(0.0, box.position.y + 2.0 + float(entry.get("height", 20.0)) * 0.9)
	var tint := colour if colour.a > 0.0 else Color(String(entry.get("color", "#ffffffff")))
	var ratio := xform.get_scale().y / maxf(scale, 0.0001) if stretch else 1.0
	if ratio != 1.0:
		canvas.draw_set_transform(Vector2.ZERO, 0.0, Vector2(1.0, ratio))
	canvas.draw_string_outline(font, Vector2(left.x, base.y / ratio), text, align, width, size_px, 3, Color(0.0, 0.0, 0.0, 0.8))
	canvas.draw_string(font, Vector2(left.x, base.y / ratio), text, align, width, size_px, tint)
	if ratio != 1.0:
		canvas.draw_set_transform_matrix(Transform2D.IDENTITY)
	return font.get_string_size(text, align, width, size_px)


## Movie pixels of a `stage`-sized movie to a canvas `size`, the way the game shows its menus: the stage is stretched to
## the window (not letterboxed), so the x and y scales differ off 16:9; glyphs keep the x scale (see `field`).
static func stage_fill(size: Vector2, stage: Vector2) -> Transform2D:
	return Transform2D(Vector2(size.x / stage.x, 0.0), Vector2(0.0, size.y / stage.y), Vector2.ZERO)
