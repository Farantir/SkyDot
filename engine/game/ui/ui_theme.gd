# SPDX-License-Identifier: GPL-3.0-or-later
#
# The item menus' Theme from the game's own movies (ADR 0010), built at run time from UiAssets; the layout stays
# ItemListView's (a SkyUI-style table), only the look is the game's:
#
#   font        the font of the inventory list's entry text field (ItemsListEntry in inventorylists.swf,
#               "$EverywhereMediumFont" through fontconfig), its size relative to the 720-pixel stage, and the
#               row pitch from the entries' placement in ItemsList
#   colours     the entry text's colour for rows and the item card's label colour (its most used grey) for dim
#               text and column titles
#   panel       the item card's frame (the largest sprite in itemcard.swf with a scaling grid), as a
#               StyleBoxTexture whose margins are its 9-slice grid, at the alpha ITEM CARD BASE places the
#               card's background with
#   divider     the category list's divider line (inventorylists.swf), stretched; its colour (fainter) marks
#               the chosen row and tab
#
# Whatever the pack lacks keeps the value of ui/theme/menu_theme.tres, which is also the whole theme without ui
# assets.
class_name UiTheme
extends RefCounted

const LISTS := "inventory components/inventorylists"
const ITEMCARD := "inventory components/itemcard"
const TYPE := &"ItemListView"
const STAGE_HEIGHT := 720.0

static var _cache := {}  # "pack path|scale" -> Theme


## The item list's theme for a view `height` pixels tall: the game's look over `fallback`.
static func item_list(fallback: Theme, height: float) -> Theme:
	if not UiAssets.available():
		return fallback
	var scale := snappedf(maxf(height / STAGE_HEIGHT, 0.25), 0.05)
	var key := "%s|%.2f" % [fallback.resource_path, scale]
	if _cache.has(key):
		return _cache[key]
	var theme: Theme = fallback.duplicate()
	var entry := _list_entry()
	if not entry.is_empty():
		var font := UiAssets.font(entry["font"])
		if font != null:
			theme.default_font = font
		theme.set_constant(&"font_size_720", TYPE, int(round(float(entry["height"]))))
		theme.set_constant(&"row_pitch_720", TYPE, int(round(float(entry["pitch"]))))
		var colour := _colour(entry["color"])
		theme.set_color(&"normal", TYPE, colour)
		theme.set_color(&"selected", TYPE, colour)
	var grey := _card_label_colour()
	if grey != Color.TRANSPARENT:
		theme.set_color(&"dim", TYPE, grey)
		theme.set_color(&"header", TYPE, grey)
	var frame := _card_frame()
	if frame >= 0:
		var texture := UiAssets.texture(ITEMCARD, frame, scale)
		var grid := UiAssets.scaling_grid(ITEMCARD, frame)
		var bounds := UiAssets.bounds(ITEMCARD, frame)
		if texture != null:
			var box := StyleBoxTexture.new()
			box.texture = texture
			box.texture_margin_left = (grid.position.x - bounds.position.x) * scale
			box.texture_margin_top = (grid.position.y - bounds.position.y) * scale
			box.texture_margin_right = (bounds.end.x - grid.end.x) * scale
			box.texture_margin_bottom = (bounds.end.y - grid.end.y) * scale
			box.modulate_color = Color(1.0, 1.0, 1.0, _card_alpha())
			theme.set_stylebox(&"panel", TYPE, box)
	var rule := _list_rule()
	if rule >= 0:
		var texture := UiAssets.texture(LISTS, rule, scale)
		if texture != null:
			theme.set_icon(&"divider", TYPE, texture)
		# The line's colour marks the chosen row and tab too, faint as the line is.
		var line := _shape_colour(LISTS, rule)
		if line != Color.TRANSPARENT:
			theme.set_color(&"border", TYPE, line)
			theme.set_color(&"highlight", TYPE, Color(line, line.a * 0.4))
	_cache[key] = theme
	return theme


## The inventory list's entry text field: {font, height, color, pitch}.
static func _list_entry() -> Dictionary:
	var entry_id := UiAssets.exported(LISTS, "ItemsListEntry")
	var list_id := UiAssets.exported(LISTS, "ItemsList")
	for placed: Dictionary in UiAssets.character(LISTS, entry_id).get("display", []):
		var field := UiAssets.character(LISTS, int(placed["character"]))
		if field.get("type", "") == "edit_text" and field.has("font"):
			var ys: Array[float] = []
			for row: Dictionary in UiAssets.character(LISTS, list_id).get("display", []):
				if int(row["character"]) == entry_id:
					ys.append(float(row["matrix"][5]))
			ys.sort()
			var pitch := ys[1] - ys[0] if ys.size() > 1 else float(field["height"]) * 1.6
			return {"font": field["font"], "height": field["height"], "color": field.get("color", "#ffffffff"), "pitch": pitch}
	return {}


## The alpha ITEM CARD BASE places its card background with (0.6 in the game's itemcard.swf), 1 if none.
static func _card_alpha() -> float:
	var base := UiAssets.exported(ITEMCARD, "ITEM CARD BASE")
	for placed: Dictionary in UiAssets.character(ITEMCARD, base).get("display", []):
		if int(placed["depth"]) == 1 and (placed.get("color", []) as Array).size() == 8:
			return float(placed["color"][3])
	return 1.0


## The grey most of the item card's text fields use (its labels), or transparent.
static func _card_label_colour() -> Color:
	var counts := {}
	for character: Dictionary in UiAssets.manifest(ITEMCARD).get("characters", []):
		if character.get("type", "") != "edit_text" or not character.has("color"):
			continue
		var colour := _colour(character["color"])
		if colour.a > 0.5 and colour.s < 0.05 and colour.v > 0.3 and colour.v < 0.9:
			counts[character["color"]] = int(counts.get(character["color"], 0)) + 1
	var best := ""
	for hex: String in counts:
		if best == "" or counts[hex] > counts[best]:
			best = hex
	return _colour(best) if best != "" else Color.TRANSPARENT


## The item card's frame: its largest sprite with a scaling grid.
static func _card_frame() -> int:
	var best := -1
	var area := 0.0
	for character: Dictionary in UiAssets.manifest(ITEMCARD).get("characters", []):
		if character.get("type", "") == "sprite" and character.has("scaling_grid") and character.has("bounds"):
			var a := float(character["bounds"][2]) * float(character["bounds"][3])
			if a > area:
				area = a
				best = int(character["id"])
	return best


## The list's rule: the widest thin shape of inventorylists.swf (the category list's divider line).
static func _list_rule() -> int:
	var best := -1
	var width := 0.0
	for character: Dictionary in UiAssets.manifest(LISTS).get("characters", []):
		var b: Array = character.get("bounds", [])
		if character.get("type", "") == "shape" and b.size() == 4 and float(b[3]) < 4.0 and float(b[2]) > width:
			width = float(b[2])
			best = int(character["id"])
	return best


## The first solid fill of a shape's SVG (fill and fill-opacity), or transparent.
static func _shape_colour(swf: String, id: int) -> Color:
	var svg := UiAssets.file(swf, UiAssets.character(swf, id).get("svg", "")).get_string_from_utf8()
	var found := RegEx.create_from_string('fill="(#[0-9a-f]{6})"( fill-opacity="([0-9.]+)")?').search(svg)
	if found == null:
		return Color.TRANSPARENT
	var colour := Color.html(found.get_string(1))
	colour.a = found.get_string(3).to_float() if found.get_string(3) != "" else 1.0
	return colour


## "#rrggbbaa" (the manifest's colours).
static func _colour(hex: String) -> Color:
	return Color.html(hex) if hex.length() == 9 else Color.WHITE
