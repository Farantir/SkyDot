# SPDX-License-Identifier: GPL-3.0-or-later
#
# Draws a character of a game movie (UiAssets) on a CanvasItem the way its display list places it: shapes as
# textures of their SVGs at their full transform, sprites at frame 0 through their children, masks (a clip
# depth) as rectangles. Enough for the HUD's meters and crosshair; it runs no ActionScript, so whatever a script
# sets (a meter's fill, an alpha) is the caller's: `fill` stands for a meter's percent (Components.Meter moves
# the fill under its mask, Full at 0 to Empty at the last frame).
#
# Assumes no rotation or skew below the character drawn (true of the meters and the crosshair): masks are the
# axis-aligned bounds of their shapes.
class_name UiDraw
extends RefCounted


## Draw `id` of `swf` with `xform` (movie pixels to the canvas) times `modulate`. `scale` is the canvas pixels
## per movie pixel at `xform`, for the rasterization. `fill` (0 to 1) moves the masked part of a sprite that has
## Full and Empty labels; -1 leaves it as authored.
static func character(canvas: CanvasItem, swf: String, id: int, xform: Transform2D, modulate := Color.WHITE,
		fill := -1.0, clip := Rect2()) -> void:
	var entry := UiAssets.character(swf, id)
	match entry.get("type", ""):
		"shape":
			_shape(canvas, swf, id, xform, modulate, clip)
		"sprite":
			_sprite(canvas, swf, entry, xform, modulate, fill, clip)


static func _matrix(values: Array) -> Transform2D:
	return Transform2D(Vector2(values[0], values[1]), Vector2(values[2], values[3]), Vector2(values[4], values[5]))


static func _tint(values: Array) -> Color:
	return Color(values[0], values[1], values[2], values[3]) if values.size() == 8 else Color.WHITE


## `clip` is in the shape's own pixels (empty: none).
static func _shape(canvas: CanvasItem, swf: String, id: int, xform: Transform2D, modulate: Color, clip: Rect2) -> void:
	var bounds := UiAssets.bounds(swf, id)
	if bounds.size == Vector2.ZERO:
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
		fill: float, clip: Rect2) -> void:
	var meter := fill >= 0.0 and UiAssets.label_frame(swf, int(entry["id"]), "Empty") > 0
	var list: Array = entry.get("display", [])
	var i := 0
	while i < list.size():
		var placed: Dictionary = list[i]
		var local := _matrix(placed["matrix"])
		var tint := modulate * _tint(placed.get("color", []))
		if not placed.has("clip_depth"):
			character(canvas, swf, int(placed["character"]), xform * local, tint, fill, _into(local, clip))
			i += 1
			continue
		# A mask: the depths up to clip_depth are drawn inside its bounds.
		var mask := local * UiAssets.bounds(swf, int(placed["character"]))
		if clip.size != Vector2.ZERO:
			mask = mask.intersection(clip)
		var shift := Transform2D.IDENTITY
		if meter:
			shift.origin.x = -(1.0 - clampf(fill, 0.0, 1.0)) * mask.size.x
		i += 1
		while i < list.size() and int(list[i]["depth"]) <= int(placed["clip_depth"]):
			var inner: Dictionary = list[i]
			var inner_local := shift * _matrix(inner["matrix"])
			character(canvas, swf, int(inner["character"]), xform * inner_local, modulate * _tint(inner.get("color", [])),
				fill, _into(inner_local, mask))
			i += 1


## A clip rectangle of the parent's space in the child's (placed with `local`).
static func _into(local: Transform2D, clip: Rect2) -> Rect2:
	if clip.size == Vector2.ZERO:
		return clip
	var r := local.affine_inverse() * clip
	return r.abs()
