# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the skills screen shows and does (ui/skills_view.gd draws it; ADR 0010: a view never touches game
# state): the header (level, XP, perk points), the 18 skills in the menu's ring, the selected skill's perk tree
# as stars with positions, links and status, a cursor over the stars, and the actions (take a perk, make a skill
# legendary, level up) which ask for a confirmation first. Built over a session (`progression`, `level_up`,
# `take_perk`); tests give it a fake with the same names. Rules stay in Progression.
class_name SkillsModel
extends RefCounted

## The ring in the menu's order, by arc (UESP Skyrim:Skills): Warrior, Mage, Thief; actor value indexes.
const ARCS: Array = [
	{"name": "Warrior", "skills": [6, 7, 8, 9, 10, 11]},
	{"name": "Mage", "skills": [18, 19, 20, 21, 22, 23]},
	{"name": "Thief", "skills": [12, 15, 14, 13, 17, 16]},
]
const ATTRIBUTES: Array[int] = [Progression.HEALTH, Progression.MAGICKA, Progression.STAMINA]
const ATTRIBUTE_NAMES := {24: "Health", 25: "Magicka", 26: "Stamina"}
const FUNC_GET_BASE_ACTOR_VALUE := 277

var _session: Object
var _progression: Progression
var _data: ProgressionData

## {level, xp, xp_need, xp_fraction, perk_points, level_up_ready, banked (levels waiting)}.
var header := {}
## [{av, name, arc, level, base, fraction, legendary}] in ring order.
var skills: Array[Dictionary] = []
## Index into `skills`.
var selected := 0
## The selected skill's stars: [{index, pos (Vector2 in 0..1, y down), perk, name, description, rank, ranks,
## required_level, status ("taken", "available", "locked"), blocker, blocker_text, links (indexes)}].
var nodes: Array[Dictionary] = []
## The node index (NODE `index`) under the cursor, -1 for none.
var cursor := -1
## The action waiting for a yes: {kind ("perk", "legendary"), text}, or empty.
var pending := {}
## The last action's outcome text (a refusal), for the view.
var note := ""


func _init(session: Object) -> void:
	_session = session
	_progression = session.progression
	_data = _progression.data()
	refresh()


## True while a level is banked: the screen asks for an attribute before the trees.
func choosing_attribute() -> bool:
	return bool(header["level_up_ready"])


func refresh() -> void:
	_build_header()
	skills.clear()
	for arc: Dictionary in ARCS:
		for av: int in arc["skills"]:
			skills.append(_skill_entry(av, String(arc["name"])))
	selected = clampi(selected, 0, skills.size() - 1)
	_build_nodes()


func current_skill() -> Dictionary:
	return skills[selected]


func node(index: int) -> Dictionary:
	for n: Dictionary in nodes:
		if n["index"] == index:
			return n
	return {}


func current_node() -> Dictionary:
	return node(cursor)


## Previous (-1) or next (+1) skill around the ring.
func select_skill(step: int) -> void:
	selected = posmod(selected + step, skills.size())
	pending = {}
	note = ""
	_build_nodes()


## Move the cursor to the nearest star in direction `dir` (screen directions, y down); stays if none.
func move_cursor(dir: Vector2) -> void:
	var here := current_node()
	if here.is_empty() or dir == Vector2.ZERO:
		return
	var best := -1
	var best_score := INF
	for n: Dictionary in nodes:
		if n["index"] == cursor:
			continue
		var delta: Vector2 = n["pos"] - here["pos"]
		var along := delta.dot(dir.normalized())
		if along <= 0.0 or along < 0.35 * delta.length():
			continue
		var score := delta.length() + 1.5 * absf(delta.cross(dir.normalized()))
		if score < best_score:
			best_score = score
			best = n["index"]
	if best >= 0:
		cursor = best
	pending = {}


## Follow a link out of (forward) or into (back) the cursor's star.
func step_link(forward: bool) -> void:
	var here := current_node()
	if here.is_empty():
		return
	if forward:
		if not here["links"].is_empty():
			cursor = here["links"][0]
	else:
		for n: Dictionary in nodes:
			if cursor in n["links"]:
				cursor = n["index"]
				break
	pending = {}


## Ask to take the perk under the cursor. False (and a note) if it cannot be taken.
func ask_take() -> bool:
	note = ""
	var n := current_node()
	if n.is_empty() or n["status"] != "available":
		note = String(n.get("blocker_text", "")) if not n.is_empty() else ""
		return false
	pending = {"kind": "perk", "text": "Take %s?" % n["name"]}
	return true


## Ask to make the selected skill legendary (needs 100).
func ask_legendary() -> bool:
	note = ""
	if not can_legendary():
		return false
	pending = {"kind": "legendary", "text": "Make %s legendary?" % current_skill()["name"]}
	return true


func can_legendary() -> bool:
	return int(current_skill()["base"]) >= int(Progression.MAX_SKILL)


## Do the waiting action. Returns the rule's result Dictionary ({} if nothing waited).
func confirm() -> Dictionary:
	if pending.is_empty():
		return {}
	var kind: String = pending["kind"]
	pending = {}
	var result := {}
	if kind == "perk":
		result = _session.take_perk(int(current_node()["perk_to_take"]))
	elif kind == "legendary":
		result = _progression.make_legendary(int(current_skill()["av"]))
	refresh()
	return result


func cancel() -> void:
	pending = {}


## Spend a banked level on Health, Magicka or Stamina (level-up flow). Returns GameSession.level_up's result.
func choose_attribute(attribute_av: int) -> Dictionary:
	var result: Dictionary = _session.level_up(attribute_av)
	refresh()
	return result


func attribute_name(av: int) -> String:
	return ATTRIBUTE_NAMES.get(av, "")


# ---- building

func _build_header() -> void:
	var p := _data.progress()
	var level := int(p["level"])
	var need := _progression.level_up_threshold(level)
	var xp := float(p["xp"])
	var banked := 0
	var rest := xp
	var l := level
	while rest >= _progression.level_up_threshold(l) and banked < 100:
		rest -= _progression.level_up_threshold(l)
		l += 1
		banked += 1
	header = {"level": level, "xp": xp, "xp_need": need, "xp_fraction": clampf(xp / maxf(need, 1.0), 0.0, 1.0),
			"perk_points": int(p["perk_points"]), "level_up_ready": banked > 0, "banked": banked}


func _skill_entry(av: int, arc: String) -> Dictionary:
	var p := _data.progress()
	var base := int(_data.base_av(av))
	var need := _progression.skill_threshold(av, base)
	var fraction := 0.0
	if base < Progression.MAX_SKILL and need > 0.0:
		fraction = clampf(float(p["skill_xp"][av - ProgressionData.FIRST_SKILL]) / need, 0.0, 1.0)
	return {"av": av, "name": _progression.skill_name(av), "arc": arc, "level": int(_data.current_av(av)),
			"base": base, "fraction": fraction, "legendary": int(p["legendary"][av - ProgressionData.FIRST_SKILL])}


func _build_nodes() -> void:
	nodes.clear()
	var av: int = skills[selected]["av"]
	var tree := _data.perk_tree(av)
	var raw: Array[Dictionary] = []
	for t: Dictionary in tree:
		if int(t.get("perk", 0)) != 0:   # node 0 is the root, not a star
			raw.append(t)
	var lo := Vector2(INF, INF)
	var hi := Vector2(-INF, -INF)
	for t in raw:
		var q := _raw_pos(t)
		lo = Vector2(minf(lo.x, q.x), minf(lo.y, q.y))
		hi = Vector2(maxf(hi.x, q.x), maxf(hi.y, q.y))
	var span := Vector2(maxf(hi.x - lo.x, 0.001), maxf(hi.y - lo.y, 0.001))
	var shown := {}
	for t in raw:
		shown[int(t["index"])] = true
	for t in raw:
		var q := _raw_pos(t)
		var perk := int(t["perk"])
		var current := _progression.first_missing(perk)
		var chain := _chain(perk)
		var shown_perk := current if current != 0 else chain[chain.size() - 1]
		var info := _data.perk(shown_perk)
		var taken := 0
		for c: int in chain:
			if _data.perk_rank(c) > 0:
				taken += 1
		var required := _required_level(chain[0 if current == 0 else chain.find(current)], av)
		var status := "taken"
		var blocker := ""
		if current != 0:
			blocker = _progression.perk_blocker(current)
			status = "available" if blocker == "" else "locked"
		var links: Array = []
		for l in t.get("links", []):
			if shown.has(int(l)):
				links.append(int(l))
		# y grows upward in the data (later perks higher); the screen's y grows down.
		nodes.append({"index": int(t["index"]), "pos": Vector2((q.x - lo.x) / span.x, 1.0 - (q.y - lo.y) / span.y),
				"perk": shown_perk, "perk_to_take": current, "name": String(info.get("name", "")),
				"description": String(info.get("description", "")), "rank": taken, "ranks": chain.size(),
				"required_level": required, "status": status, "blocker": blocker,
				"blocker_text": blocker_text(blocker, required, av), "links": links})
	if cursor < 0 or node(cursor).is_empty():
		cursor = _start_node()


## The star the cursor starts on: the first available one, else the lowest.
func _start_node() -> int:
	var best := -1
	for n: Dictionary in nodes:
		if n["status"] == "available":
			return n["index"]
	var low := -1.0
	for n: Dictionary in nodes:
		if n["pos"].y > low:
			low = n["pos"].y
			best = n["index"]
	return best


## The star's place in the tree: the CK grid cell plus the node's x/y offset, in cell units (y grows upward).
func _raw_pos(t: Dictionary) -> Vector2:
	return Vector2(float(t.get("grid_x", 0)) + float(t.get("x", 0.0)), float(t.get("grid_y", 0)) + float(t.get("y", 0.0)))


## The perk ids of the chain starting at `perk`, ranks in order.
func _chain(perk: int) -> Array[int]:
	var out: Array[int] = []
	var p := perk
	while p != 0 and out.size() < 16:
		out.append(p)
		p = int(_data.perk(p).get("next_perk", 0))
	return out


## The skill level a perk's conditions ask for (GetBaseActorValue of the tree's skill), 0 if none.
func _required_level(perk: int, av: int) -> int:
	for c: Dictionary in _data.perk(perk).get("conditions", []):
		if int(c.get("function", 0)) == FUNC_GET_BASE_ACTOR_VALUE and int(c.get("param1", -1)) == av:
			return int(c.get("compare_to", 0.0))
	return 0


## Why a star cannot be taken, in words ("" when it can, or is taken).
func blocker_text(blocker: String, required: int, av: int) -> String:
	match blocker:
		"points":
			return "No perk points"
		"link":
			return "Take a perk it follows first"
		"rank":
			return "Take the previous rank first"
		"conditions":
			if required > 0 and int(_data.base_av(av)) < required:
				return "Requires %s %d" % [_progression.skill_name(av), required]
			return "Requirements not met"
		"playable":
			return "Cannot be taken"
	return ""


## What a level-up adds to the chosen attribute (iAVDhmsLevelUp).
func attribute_gain() -> int:
	return _data.gmst_int("iAVDhmsLevelUp", 10)
