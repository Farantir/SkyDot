# SPDX-License-Identifier: GPL-3.0-or-later
#
# The trainer's menu as trainingmenu.swf lays it out (TrainingMenuObj at the stage's (426, 460): the framed card with
# the skill name, its meter, the trainer's rank, "Times Trained This Level", cost and gold, and the Train / Exit
# buttons). Drawn through UiDraw from the pack's movie; TrainingView calls it. The movie's script, not the card,
# places it: see PLACE_SHIFT.
class_name TrainingDraw
extends RefCounted

const SWF := "trainingmenu"
const STAGE := Vector2(1280.0, 720.0)
const SPRITE := "TrainingMenu"
const ORIGIN := Vector2(426.1, 459.7)
## Extra movie pixels the game's script moves the card down (measured at 1100x800, side_training.png).
const PLACE_SHIFT := Vector2.ZERO
## The buttons' key icons (exports of the movie) left of their text, in the card.
const ICONS := {"AcceptButton": ["E", Vector2(135.0, 185.8)], "ExitButton": ["Tab", Vector2(290.0, 185.8)]}
const ICON_OFFSET := Vector2(-25.0, 1.0)


static func available() -> bool:
	return UiAssets.available() and UiAssets.exported(SWF, SPRITE) >= 0


## The trainer's rank as the card shows it, from the level the trainer trains up to.
static func rank(max_level: int) -> String:
	for step: Array in [[90, "Master"], [75, "Expert"], [50, "Journeyman"], [25, "Apprentice"]]:
		if max_level >= int(step[0]):
			return String(step[1])
	return "Novice"


## `model` is a TrainingModel; `over` which
## of "AcceptButton" / "ExitButton" is highlighted ("" none).
static func draw(canvas: CanvasItem, model: TrainingModel, over := "") -> void:
	var size: Vector2 = canvas.get_rect().size
	var stage := UiDraw.stage_fill(size, STAGE)
	var look := UiDraw.Look.new()
	look.draw_texts = true
	look.texts = {
		"#31": model.skill_name.to_upper(),
		"TrainerSkill": rank(model.max_level),
		"TimesTrained": "%d/%d" % [maxi(Progression.TRAININGS_PER_LEVEL - model.sessions_left, 0), Progression.TRAININGS_PER_LEVEL],
		"TrainCost": str(model.cost),
		"CurrentGold": str(model.gold),
		"AcceptButton/textField": "Train",
		"ExitButton/textField": "Exit",
	}
	look.fills = {"SkillMeter": clampf(float(model.level) / 100.0, 0.0, 1.0)}
	if over != "":
		var button := UiAssets.exported(SWF, "Button")
		look.named_frames[over] = maxi(UiAssets.label_frame(SWF, button, "over"), 0)
	look.hidden = {"border": true}
	var id := UiAssets.exported(SWF, SPRITE)
	UiDraw.character(canvas, SWF, id, stage * Transform2D(0.0, ORIGIN + PLACE_SHIFT), Color.WHITE, look)
	var card := stage * Transform2D(0.0, ORIGIN + PLACE_SHIFT)
	for button: String in ICONS:
		var icon := UiAssets.exported(SWF, String(ICONS[button][0]))
		if icon >= 0:
			UiDraw.character(canvas, SWF, icon, card * Transform2D(0.0, (ICONS[button][1] as Vector2) + ICON_OFFSET))
