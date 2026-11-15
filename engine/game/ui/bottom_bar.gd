# SPDX-License-Identifier: GPL-3.0-or-later
#
# The bar under the item menus (BottomBar of "inventory components/bottombar.swf", its PlayerInfoCard): the
# inventory and container menus show the carry weight, the gold and the Health meter, the magic menu the
# Magicka meter. The bar is the game's own sprite (UiDraw draws the meters at their frame between Full and Empty,
# as Components.Meter does); the movie's text fields are drawn here with the fonts UiAssets read. InventoryMenu
# keeps the bar at the bottom of the screen (BottomBar_mc at y 645 of the 720 high stage) whatever the aspect, and
# the card sits at (503, 16) in it. Without the game's assets nothing is drawn.
#
# Not done: the bar's buttons (Button0-3, the key hints), the armor rating, damage and warmth labels of the
# other states (the item card shows Damage and Armor), the Level meter.
class_name BottomBar
extends Control

const SWF := "inventory components/bottombar"
const STAGE := Vector2(1280.0, 720.0)
const BAR_HEIGHT := 75.0  # the stage's height under BottomBar_mc's origin
const CARD := Vector2(503.0, 16.0)
const CARD_SPRITE := 329
const FIELDS := {
	"label": {"font": "$EverywhereMediumFont", "size": 16.0, "colour": Color("999999"), "box": Vector2(-2.0, 51.05)},
	"value": {"font": "$EverywhereBoldFont", "size": 20.0, "colour": Color.WHITE},
}

## "inventory" (also the container): carry weight, gold, Health. "magic": Magicka.
var kind := "inventory"
var carry_weight := 0.0
var carry_capacity := 0.0
var gold := 0
var health := Vector2.ZERO  # value, maximum
var magicka := Vector2.ZERO


func _init(bar_kind := "inventory") -> void:
	kind = bar_kind
	set_anchors_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE


## Whether the pack has the movie.
static func available() -> bool:
	return UiAssets.available() and UiAssets.exported(SWF, "BottomBar") >= 0


func _draw() -> void:
	if not available():
		return
	var fit := minf(size.x / STAGE.x, size.y / STAGE.y)
	var visible := Rect2((STAGE - size / fit) * 0.5, size / fit)
	var bar := Transform2D(0.0, Vector2(fit, fit), 0.0, (Vector2(0.0, visible.end.y - BAR_HEIGHT) - visible.position) * fit)
	var card := bar * Transform2D(0.0, CARD)
	var frame := UiAssets.label_frame(SWF, CARD_SPRITE, "Default" if kind == "inventory" else "Magic")
	var entries := UiAssets.display_list(SWF, CARD_SPRITE, maxi(frame, 0))
	if kind == "inventory":
		var gold_field := _entry(entries, "PlayerGoldValue")
		var weight_field := _entry(entries, "CarryWeightValue")
		_field(card, _entry(entries, "PlayerGoldLabel"), "Gold", "label")
		_field(card, gold_field, str(gold), "value")
		_field(card, _entry(entries, "CarryWeightLabel"), "Carry Weight", "label")
		_field(card, weight_field, "%d/%d" % [roundi(carry_weight), roundi(carry_capacity)], "value")
		_meter(card, _entry(entries, "HealthRect"), health)
	else:
		_meter(card, _entry(entries, "MagickaRect"), magicka)


func _entry(entries: Array, name: String) -> Dictionary:
	for entry: Dictionary in entries:
		if entry.get("name", "") == name:
			return entry
	return {}


func _meter(card: Transform2D, entry: Dictionary, pair: Vector2) -> void:
	if entry.is_empty():
		return
	var look := UiDraw.Look.new()
	look.fills["Meter_mc"] = clampf(pair.x / pair.y, 0.0, 1.0) if pair.y > 0.0 else 0.0
	UiDraw.character(self, SWF, int(entry["character"]), card * UiDraw.matrix(entry["matrix"]), Color.WHITE, look)


## A text field of the movie: right aligned in its box, `kind` of FIELDS says the font.
func _field(card: Transform2D, entry: Dictionary, text: String, style: String) -> void:
	if entry.is_empty():
		return
	var spec: Dictionary = FIELDS[style]
	var font := UiAssets.font(spec["font"])
	if font == null:
		font = get_theme_default_font()
	var bounds := UiAssets.bounds(SWF, int(entry["character"]))
	var xform := card * UiDraw.matrix(entry["matrix"])
	var scale := xform.get_scale().x
	var size_px := int(round(float(spec["size"]) * scale))
	var right := xform * Vector2(bounds.end.x - 2.0, 0.0)
	var top := xform * Vector2(0.0, 2.0 + float(spec["size"]) * 0.9)
	var width := bounds.size.x * scale
	draw_string_outline(font, Vector2(right.x - width, top.y), text, HORIZONTAL_ALIGNMENT_RIGHT, width, size_px, 3, Color(0, 0, 0, 0.8))
	draw_string(font, Vector2(right.x - width, top.y), text, HORIZONTAL_ALIGNMENT_RIGHT, width, size_px, spec["colour"])
