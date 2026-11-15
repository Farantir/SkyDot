# SPDX-License-Identifier: GPL-3.0-or-later
#
# The level-up attribute choice as levelupmenu.swf lays it out (LevelUpMenuObj at the stage's (640, 423): the
# framed prompt "You leveled up!  Choose an attribute to advance:" and the buttons Magicka, Health, Stamina side by
# side; the chosen button plays its "over" frame). Drawn through UiDraw from the pack's movie; SkillsView calls it
# while a level is banked.
class_name LevelUpDraw
extends RefCounted

const SWF := "levelupmenu"
const STAGE := Vector2(1280.0, 720.0)
const ORIGIN := Vector2(640.0, 423.0)
const SPRITE := "LevelUpMenuObj"
## The buttons from left to right, by their instance name in the movie.
const BUTTONS := ["MagickaButton", "HealthButton", "StaminaButton"]
const ORDER: Array[int] = [Progression.MAGICKA, Progression.HEALTH, Progression.STAMINA]
const TEXTS := {"MagickaButton": "Magicka", "HealthButton": "Health", "StaminaButton": "Stamina"}
# Measured against the game at 1100x800 (its scripts stretch the frame; skycompare-data/ui-step4/side_levelup.png):
const FRAME_STRETCH := 1.31  # the frame's height, about its centre
const FRAME_SHIFT := -3.0
const PROMPT_SHIFT := -6.4
const BUTTON_SHIFT := 9.0
const PROMPT := "You leveled up!  Choose an attribute to advance:"


static func available() -> bool:
	return UiAssets.available() and UiAssets.exported(SWF, SPRITE) >= 0


## Draw the dialog on `canvas` over its whole rect, `chosen` being the attribute under the cursor.
static func draw(canvas: CanvasItem, chosen: int) -> void:
	var size: Vector2 = canvas.get_rect().size
	var fit := minf(size.x / STAGE.x, size.y / STAGE.y)
	var stage := Transform2D(0.0, Vector2(fit, fit), 0.0, (size - STAGE * fit) * 0.5)
	var root := stage * Transform2D(0.0, ORIGIN)
	var id := UiAssets.exported(SWF, SPRITE)
	for entry: Dictionary in UiAssets.display_list(SWF, id, 0):
		var name: String = entry.get("name", "")
		var xform := root * UiDraw.matrix(entry["matrix"])
		var child := int(entry["character"])
		if name in BUTTONS:
			xform = root * Transform2D(0.0, Vector2(0.0, BUTTON_SHIFT)) * UiDraw.matrix(entry["matrix"])
			var over := ORDER[BUTTONS.find(name)] == chosen
			var frame := maxi(UiAssets.label_frame(SWF, child, "over" if over else "up"), 0)
			UiDraw.character(canvas, SWF, child, xform, Color.WHITE, null, Rect2(), frame)
			for part: Dictionary in UiAssets.display_list(SWF, child, frame):
				if part.get("name", "") == "textField":
					UiDraw.field(canvas, SWF, int(part["character"]), xform * UiDraw.matrix(part["matrix"]), TEXTS[name],
						Color.WHITE if over else Color(0.0, 0.0, 0.0, 0.0))
		elif name == "ItemTextField":
			UiDraw.field(canvas, SWF, child, root * Transform2D(0.0, Vector2(0.0, PROMPT_SHIFT)) * UiDraw.matrix(entry["matrix"]), PROMPT)
		else:
			var centre := UiDraw.matrix(entry["matrix"]) * UiAssets.bounds(SWF, child).get_center()
			xform = root * Transform2D(0.0, Vector2(0.0, FRAME_SHIFT)) * Transform2D(Vector2(1, 0), Vector2(0, FRAME_STRETCH), Vector2(0.0, centre.y * (1.0 - FRAME_STRETCH))) * UiDraw.matrix(entry["matrix"])
			UiDraw.character(canvas, SWF, child, xform, _tint(entry))


static func _tint(entry: Dictionary) -> Color:
	var c: Array = entry.get("color", [])
	return Color(c[0], c[1], c[2], c[3]) if c.size() >= 4 else Color.WHITE
