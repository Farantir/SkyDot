# SPDX-License-Identifier: GPL-3.0-or-later
#
# The skills screen's chrome as statsmenu.swf lays it out over the 3D sky (StatsSky), drawn through UiDraw from the
# pack's movie with the stage stretched to the window as the game does:
#   - statsmenubase (StatsMenuBaseInstance at the stage's (640, 360)): the header (NAME, LEVEL and its meter, RACE),
#     the bottom bar (Magicka, Health, Stamina with values and meters), "Perks to increase: N" when points are
#     left, and the description card: frame "Skills" (the skill's description) or "Perks" (the perk's description,
#     a rule, then "SKILL level" and "REQUIRES level" as SetDescriptionCard writes them).
#   - the skill ring (AnimatedSkillText.SetAngle): SkillText_mc per skill at LocationsA by the camera's angle,
#     20 degrees a skill, the two either side of the middle scaled up to 175% as they reach it, "NAME level" (the
#     level in $EverywhereBoldFont 24, white) and ShortBar at the skill's progress. Hidden in a tree (HideRing).
#   - in a tree the perk names (StatsMenu.UpdatePerkText): PerkName at each star's place on the screen, scaled
#     (y * 165 + 10)%, only below MaxPerkNameHeight (115 of the 720 stage).
class_name StatsMenuDraw
extends RefCounted

const SWF := "statsmenu"
const STAGE := Vector2(1280.0, 720.0)
const BASE := "statsmenubase"
const ORIGIN := Vector2(640.15, 360.05)
## AnimatedSkillText: the ring's x places (LocationsA, in AnimatingSkillTextInstance at the stage's (1.0, 492.05)).
const LOCATIONS: Array[float] = [-150.0, -10.0, 130.0, 270.0, 410.0, 640.0, 870.0, 1010.0, 1150.0, 1290.0, 1430.0]
const RING_ORIGIN := Vector2(1.0, 492.05)
const SKILL_ANGLE := 20.0
const MAX_PERK_NAME_HEIGHT := 115.0
const LEVEL_COLOUR := Color.WHITE
const LABEL_COLOUR := Color("999999")


static func available() -> bool:
	return UiAssets.available() and UiAssets.exported(SWF, BASE) >= 0


## `info`: {name, race, level, xp_fraction, perk_points, health, magicka, stamina (Vector2 value, max)}.
## `skills`: SkillsModel.skills; `angle` the camera's turn in degrees (skill i at i * 20). In a tree (`perk` not
## empty: the star under the cursor, a SkillsModel node) `labels` are [{text, at (fractions of the screen)}].
static func draw(canvas: CanvasItem, info: Dictionary, skills: Array, selected: int, angle: float, perk: Dictionary,
		labels: Array) -> void:
	var size: Vector2 = canvas.get_rect().size
	var stage := UiDraw.stage_fill(size, STAGE)
	var in_tree := not perk.is_empty()
	var skill: Dictionary = skills[selected]
	var look := UiDraw.Look.new()
	look.draw_texts = true
	look.texts = {
		"FirstLastLabel": String(info.get("name", "")),
		"LevelNumberLabel": str(info.get("level", 1)),
		"RacevalueLabel": String(info.get("race", "")),
		"healthValue": _pair(info.get("health", Vector2.ZERO)),
		"magicValue": _pair(info.get("magicka", Vector2.ZERO)),
		"enduranceValue": _pair(info.get("stamina", Vector2.ZERO)),
		"CardNameTextInstance": "",
	}
	look.fills = {
		"animate": float(info.get("xp_fraction", 0.0)),
		"HealthMeter_mc": _fraction(info.get("health", Vector2.ZERO)),
		"MagickaMeter_mc": _fraction(info.get("magicka", Vector2.ZERO)),
		"StaminaMeter_mc": _fraction(info.get("stamina", Vector2.ZERO)),
	}
	look.hidden = {"CameraMovementInstance": true, "AnimatingSkillTextInstance": true, "SkillRequirementText": true,
		"LegendaryButtonInstance": true, "CardDescriptionTextInstance": true}
	var points := int(info.get("perk_points", 0))
	if points > 0:
		look.texts["AddPerkTextField"] = "%s %d" % [UiDraw._plain(String(UiAssets.character(SWF, 131).get("text", "$Perks to increase:"))), points]
	else:
		look.hidden["AddPerkTextInstance"] = true
	var base := UiAssets.exported(SWF, BASE)
	var frame := maxi(UiAssets.label_frame(SWF, base, "Perks" if in_tree else "Skills"), 0)
	var root := stage * Transform2D(0.0, ORIGIN)
	UiDraw.character(canvas, SWF, base, root, Color.WHITE, look, Rect2(), frame)
	_description(canvas, root, base, frame, String(perk.get("description", "")) if in_tree else String(skill.get("description", "")))
	if in_tree:
		_requirements(canvas, root, base, frame, skill, perk)
		_perk_names(canvas, stage, labels)
	else:
		_ring(canvas, stage, skills, angle)


static func _pair(v: Vector2) -> String:
	return "%d/%d" % [roundi(v.x), roundi(v.y)]


static func _fraction(v: Vector2) -> float:
	return clampf(v.x / v.y, 0.0, 1.0) if v.y > 0.0 else 0.0


## The ring as AnimatedSkillText.SetAngle places it.
static func _ring(canvas: CanvasItem, stage: Transform2D, skills: Array, angle: float) -> void:
	var count := skills.size()
	var at := fposmod(angle, SKILL_ANGLE * count)
	var base_index := int(floorf(at / SKILL_ANGLE))
	var t := fposmod(at, SKILL_ANGLE) / SKILL_ANGLE
	var half := int(floorf((LOCATIONS.size() - 2) / 2.0)) + 1
	var low := posmod(base_index - half, count)
	var text_id := UiAssets.exported(SWF, "SkillText_mc")
	for i in count:
		var slot := posmod(i - low, count) - 1
		if slot < 0 or slot > LOCATIONS.size() - 2:  # outside the window: SetAngle parks it at LocationsA[0], off the stage
			continue
		var x := LOCATIONS[slot] + (1.0 - t) * (LOCATIONS[slot + 1] - LOCATIONS[slot])
		var scale := 1.0
		if slot == 4:
			scale = 1.0 + 0.75 * (1.0 - t)
		elif slot == 5:
			scale = 1.0 + 0.75 * t
		_skill_text(canvas, stage * Transform2D(0.0, RING_ORIGIN + Vector2(x, 0.0)) * Transform2D(0.0, Vector2(scale, scale), 0.0, Vector2.ZERO),
			text_id, skills[i], 1.0 - (scale - 1.0) / 2.5)


## One SkillText_mc: the label "NAME level", ShortBar (its own y scale as SetAngle sets it) and the legendary icon.
static func _skill_text(canvas: CanvasItem, xform: Transform2D, id: int, skill: Dictionary, bar_y: float) -> void:
	for part: Dictionary in UiAssets.display_list(SWF, id, 0):
		var local := UiDraw.matrix(part["matrix"])
		match String(part.get("name", "")):
			"LabelInstance":
				_rich(canvas, int(part["character"]), xform * local, [
					[String(skill["name"]).to_upper() + " ", "", LABEL_COLOUR, 0.0],
					[str(skill["level"]), "$EverywhereBoldFont", LEVEL_COLOUR, 24.0]])
			"ShortBar":
				var look := UiDraw.Look.new()
				look.fills = {"ShortBar": float(skill.get("fraction", 0.0))}
				UiDraw.character(canvas, SWF, int(part["character"]), xform * local * Transform2D(0.0, Vector2(1.0, bar_y), 0.0, Vector2.ZERO),
					Color.WHITE, look, Rect2(), 0, "ShortBar")
			"LegendaryIconInfoInstance":
				if int(skill.get("legendary", 0)) > 0:
					var look := UiDraw.Look.new()
					look.draw_texts = true
					look.texts = {"LegendaryCountText": str(skill["legendary"]) if int(skill["legendary"]) > 1 else ""}
					UiDraw.character(canvas, SWF, int(part["character"]), xform * local, Color.WHITE, look)


## SkillRequirementText as SetDescriptionCard writes it: "SKILL level", and "REQUIRES level" when the perk asks for one.
static func _requirements(canvas: CanvasItem, root: Transform2D, base: int, frame: int, skill: Dictionary, perk: Dictionary) -> void:
	for entry: Dictionary in UiAssets.display_list(SWF, base, frame):
		if entry.get("name", "") != "DescriptionCardInstance":
			continue
		var card := root * UiDraw.matrix(entry["matrix"])
		for part: Dictionary in UiAssets.display_list(SWF, int(entry["character"]), 0):
			if part.get("name", "") != "SkillRequirementText":
				continue
			var parts := [[String(skill["name"]).to_upper() + " ", "", LABEL_COLOUR, 0.0], [str(skill["level"]), "", LEVEL_COLOUR, 0.0]]
			var required := int(perk.get("required_level", 0))
			if required > 0:  # shown whenever the perk asks for a level, met or not (the game's shots)
				parts.append(["          REQUIRES ", "", LABEL_COLOUR, 0.0])
				parts.append([str(required), "", LEVEL_COLOUR, 0.0])
			_rich(canvas, int(part["character"]), card * UiDraw.matrix(part["matrix"]), parts)


## CardDescriptionTextInstance: a multiline field, wrapped in its box and centred.
static func _description(canvas: CanvasItem, root: Transform2D, base: int, frame: int, text: String) -> void:
	for entry: Dictionary in UiAssets.display_list(SWF, base, frame):
		if entry.get("name", "") != "DescriptionCardInstance":
			continue
		var card := root * UiDraw.matrix(entry["matrix"])
		for part: Dictionary in UiAssets.display_list(SWF, int(entry["character"]), 0):
			if part.get("name", "") != "CardDescriptionTextInstance":
				continue
			var id := int(part["character"])
			var field := UiAssets.character(SWF, id)
			var xform := card * UiDraw.matrix(part["matrix"])
			var font := UiAssets.font(String(field.get("font", "")))
			if font == null:
				font = ThemeDB.fallback_font
			var height := float(field.get("height", 22.0))
			var scale := xform.get_scale().x
			var px := maxi(int(round(height * scale)), 6)
			var box := UiAssets.bounds(SWF, id)
			var at := Vector2((xform * Vector2(box.position.x + 2.0, 0.0)).x, (xform * Vector2(0.0, box.position.y + 2.0 + height * 0.9)).y)
			var width := (box.size.x - 4.0) * scale
			canvas.draw_multiline_string_outline(font, at, text, HORIZONTAL_ALIGNMENT_CENTER, width, px, -1, 3, Color(0.0, 0.0, 0.0, 0.8))
			canvas.draw_multiline_string(font, at, text, HORIZONTAL_ALIGNMENT_CENTER, width, px, -1, Color(String(field.get("color", "#ffffffff"))))


static func _perk_names(canvas: CanvasItem, stage: Transform2D, labels: Array) -> void:
	var clip := UiAssets.exported(SWF, "PerkName")
	if clip < 0:
		return
	var visible := maxi(UiAssets.label_frame(SWF, clip, "Visible"), 0)
	for label: Dictionary in labels:
		var at: Vector2 = label["at"]
		if at.y * STAGE.y <= MAX_PERK_NAME_HEIGHT:
			continue
		var scale := (at.y * 165.0 + 10.0) / 100.0
		var xform := stage * Transform2D(0.0, at * STAGE) * Transform2D(0.0, Vector2(scale, scale), 0.0, Vector2.ZERO)
		for holder: Dictionary in UiAssets.display_list(SWF, clip, visible):
			var inner := xform * UiDraw.matrix(holder["matrix"])
			for part: Dictionary in UiAssets.display_list(SWF, int(holder["character"]), 0):
				if part.get("name", "") == "NameText":
					UiDraw.field(canvas, SWF, int(part["character"]), inner * UiDraw.matrix(part["matrix"]), String(label["text"]))


## A text field drawn with runs of its own font or another ([text, font ("" the field's), colour, size (0 the field's)]),
## centred in its box as the field aligns; the movies' htmlText with <font> runs.
static func _rich(canvas: CanvasItem, id: int, xform: Transform2D, parts: Array) -> void:
	var entry := UiAssets.character(SWF, id)
	var own := UiAssets.font(String(entry.get("font", "")))
	if own == null:
		own = ThemeDB.fallback_font
	var height := float(entry.get("height", 20.0))
	var scale := xform.get_scale().x
	var box := UiAssets.bounds(SWF, id)
	var runs: Array = []
	var width := 0.0
	for p: Array in parts:
		var font: Font = own if String(p[1]) == "" else UiAssets.font(String(p[1]))
		if font == null:
			font = own
		var px := maxi(int(round((float(p[3]) if float(p[3]) > 0.0 else height) * scale)), 6)
		var w := font.get_string_size(String(p[0]), HORIZONTAL_ALIGNMENT_LEFT, -1, px).x
		runs.append([String(p[0]), font, px, p[2], w])
		width += w
	var left := xform * Vector2(box.position.x + 2.0, 0.0)
	var box_width := (box.size.x - 4.0) * scale
	var x := left.x
	match String(entry.get("align", "left")):
		"center":
			x += (box_width - width) * 0.5
		"right":
			x += box_width - width
	var y := (xform * Vector2(0.0, box.position.y + 2.0 + height * 0.9)).y
	for r: Array in runs:
		canvas.draw_string_outline(r[1], Vector2(x, y), r[0], HORIZONTAL_ALIGNMENT_LEFT, -1, r[2], 3, Color(0.0, 0.0, 0.0, 0.8))
		canvas.draw_string(r[1], Vector2(x, y), r[0], HORIZONTAL_ALIGNMENT_LEFT, -1, r[2], r[3])
		x += float(r[4])
