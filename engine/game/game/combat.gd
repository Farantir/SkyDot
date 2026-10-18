# SPDX-License-Identifier: GPL-3.0-or-later
#
# Melee combat for the player and, through `npc_attack`, for any actor
# (GAMEPLAY-PLAN.md, 6c; SYSTEMS.md P2, C1). No camera, input or scene of its
# own: it knows a pointer node (the viewpoint, or a VR controller) and the
# player's feet, both given by the session, and the built actors under the
# host.
#
# The player's weapon has four states (sheathed, drawing, drawn, sheathing)
# and a clip set by the right-hand weapon's type: `1hm_` for swords, daggers,
# axes and maces, `2hm_` for two-handed swords, `2hw_` for two-handed axes and
# hammers, `h2h_` without a weapon. Bows, crossbows and staves are not
# implemented: they log once and play as hands. The clips are the
# first-person project's (`meshes/animationdata/firstperson.txt`, whose clips
# list the text keys: Skyrim keeps `HitFrame`, `weaponSwing` and
# `SoundPlay.*` there, not in the clips). The shell plays the clip a signal
# names; this class only keeps the time.
#
# A light attack alternates right and left; an attack held longer than
# fInitialPowerAttackDelay becomes a power attack. At the clip's `HitFrame`
# the hit test runs: the nearest living actor within weapon reach x
# fCombatDistance (plus its radius) of the viewpoint, inside the strike angle
# of the race's attack, in line of sight, takes `SkydotPapyrus.apply_hit`.
# Then the impact: the weapon's impact data set, the target race's material,
# the sound and the model of that impact.
#
# Actors (6g): `npc_attack(attacker, power, target)` turns the attacker to its
# target, plays the third-person clip on it (SkydotActor.play_action) and tests
# against every living built actor but the attacker, and the player, the target
# first. `npc_draw(ref, drawn)` sets the combat stance by the weapon in the
# right hand (a bow for an archer: SkydotPapyrus.ranged_weapon), plays the
# equip or unequip clip and moves the weapon model between sheath and hand
# (ActorGear) halfway through it.
extends RefCounted
class_name Combat

enum State { SHEATHED, DRAWING, DRAWN, SHEATHING }

## The weapon state changed (or an attack ended): the new state and the clip
## the first-person body plays (without path or extension, e.g. "1hm_equip");
## looping for DRAWN (the idle) and SHEATHED (empty: nothing is shown).
signal weapon_state_changed(state: int, clip: String)
## The player's attack began: its clip and whether it is a power attack.
## `current_attack()` says the rest (left hand, playback speed, hit time).
signal attack_started(clip: String, power: bool)
## `target` took a hit (the dictionary `SkydotPapyrus.apply_hit` returns) at
## `point` (engine space).
signal hit_landed(target: int, result: Dictionary, point: Vector3)
## An actor's attack began (`npc_attack`): the attacker, the third-person clip
## (a pack path) and whether it is a power attack. The clip already plays on
## the attacker's node; the signal is for shells and the AI.
signal npc_attack_started(attacker: int, clip: String, power: bool)
## An actor drew or sheathed (`npc_draw`).
signal npc_drawn_changed(ref: int, drawn: bool)
## An arrow left an actor's bow (`npc_shoot`): the shooter and its target.
signal npc_arrow(attacker: int, target: int)

const FIRST_PERSON_PROJECT := "meshes/animationdata/firstperson.txt"
const FIRST_PERSON_CLIPS := "meshes/actors/character/_1stperson/animations/"
const THIRD_PERSON_CLIPS := "meshes/actors/character/animations/"
const PROJECTS := "meshes/animationdata/"

## Game settings and their fallbacks (the values of the base game's data).
const GMST_POWER_DELAY := "fInitialPowerAttackDelay"
const POWER_DELAY_FALLBACK := 0.3
const GMST_COMBAT_DISTANCE := "fCombatDistance"
const COMBAT_DISTANCE_FALLBACK := 141.0

## Placeholder: the hit time of a clip the project does not list, as a share
## of its length.
const HIT_FRACTION_FALLBACK := 0.4
## Placeholder: the strike angle (degrees, half the cone) when the race has no
## attack of the event. The data's 50 and 35 (attackStart, power attacks).
const STRIKE_ANGLE_LIGHT := 50.0
const STRIKE_ANGLE_POWER := 35.0
## Placeholder: the player's body for NPC attacks (SkydotPlayer's defaults).
const PLAYER_RADIUS := 0.3
const PLAYER_HEIGHT := 1.83
## An NPC's eyes in a share of its height.
const HEAD_FRACTION := 0.93
## Where on a body a hit lands, as a share of its height.
const CHEST_FRACTION := 0.65
## Placeholder: seconds a segment sweep (VR) cannot hit the same actor again.
const SWEEP_COOLDOWN := 0.5
## Placeholder: seconds the impact model stays at least, and at most.
const IMPACT_MIN_SECONDS := 0.5
const IMPACT_MAX_SECONDS := 3.0

## The weapon model changes hands this far into the equip or unequip clip: a
## placeholder for the clip's own weapon text key (not read yet).
const GEAR_SWAP_FRACTION := 0.4
## WEAP animation type -> the third-person equip clip's prefix (`1hm_equip`).
const EQUIP_PREFIX := {1: "1hm", 2: "dag", 3: "axe", 4: "mac", 5: "2hm", 6: "2hw", 7: "bow"}
## WEAP animation type -> the stance (SkydotActor.set_stance).
const STANCE_PREFIX := {1: "1hm", 2: "1hm", 3: "1hm", 4: "1hm", 5: "2hm", 6: "2hw", 7: "bow"}
## What a creature without a weapon stands in when it fights.
const CREATURE_STANCE := "combat"
## PLACEHOLDER: where in the draw clip the arrow leaves when the clip has no release key.
const BOW_RELEASE_FRACTION := 0.85
## Bow draw clips, in the order tried (human project); the arrow leaves at the end.
const BOW_CLIPS := ["bow_drawlight", "bow_drawnock_quickshot", "bow_drawnock"]
const GMST_BOW_SPREAD := "fBowNPCSpreadAngle"
const BOW_SPREAD_FALLBACK := 4.0
const GRAVITY := 9.81
## A shot leaves this far above the feet (fraction of the height) and this far ahead (metres).
const SHOT_HEIGHT_FRACTION := 0.75
const SHOT_AHEAD := 0.5
const EVENT_BOW := "bowAttack"
const PLAYER_REF := 0x14
const PLAYER_NPC := 7
const EVENT_LIGHT := "attackStart"
const EVENT_LIGHT_LEFT := "attackStartLeftHand"
const EVENT_POWER := "attackPowerStartInPlace"
const FLAGS_POWER := 4

var _world: SkydotWorld
var _pack: SkydotPack
var _papyrus: SkydotPapyrus
var _host: Node3D
var _sounds: Sounds
var _pointer: Callable  # () -> Node3D: where attacks start and look from
var _feet: Callable  # () -> Vector3: the player's feet, engine space
var _warn := WarnOnce.new("combat: ")

var _state := State.SHEATHED
var _state_time := 0.0
var _state_length := 0.0
var _weapon := {}  # the right-hand weapon (SkydotWorld.get_weapon), {} for hands
var _weapon_form := 0
var _weapon_extra := 0
var _prefix := "h2h_"
var _left_next := false
var _attack := {}  # the attack under way (current_attack)
var _held := false  # the attack button is down
var _held_time := 0.0
var _held_fired := false  # the held press already became a power attack
var _npc_attacks: Array[Dictionary] = []
var _effects: Array[Dictionary] = []  # impact models: {node, left}
var _cooldowns := {}  # ref -> seconds, segment sweeps
var _clock := 0.0
var _timings := {}  # "project|stem" -> timing dictionary
var _projects := {}  # project path -> {lowercase clip name: annotations}
var _races := {}  # race id -> get_race
var _plans := {}  # actor ref -> get_actor_plan (race, behaviour)
var _player_plan := {}
var _shots: Array[Dictionary] = []  # arrows still to leave: {attacker, target, bow, ammo, left}
var _rng := RandomNumberGenerator.new()
## Where arrows fly (the session's Projectiles).
var projectiles: Projectiles
var _locomotions := {}  # behaviour path -> get_locomotion
var _drawn := {}  # actor ref -> bool, the actors that fight with their weapon out
var _swaps: Array[Dictionary] = []  # models still to change hands: {ref, left, drawn}
## The weapons on the third-person skeletons.
var gear: ActorGear


func _init(world: SkydotWorld, pack: SkydotPack, papyrus: SkydotPapyrus, host: Node3D, sounds: Sounds,
		pointer: Callable, feet: Callable) -> void:
	_world = world
	_pack = pack
	_papyrus = papyrus
	_host = host
	_sounds = sounds
	_pointer = pointer
	_feet = feet
	gear = ActorGear.new(world, papyrus)
	on_items_changed()


# ---- the player's weapon

## Draw the weapon if it is sheathed, sheathe it if it is drawn. Ignored
## while drawing or sheathing.
func toggle_weapon() -> void:
	if _state == State.SHEATHED:
		_begin(State.DRAWING, _prefix + "equip", "weaponDraw")
	elif _state == State.DRAWN:
		_attack = {}
		_begin(State.SHEATHING, _prefix + "unequip", "weaponSheathe")


func weapon_state() -> int:
	return _state


func state_name() -> String:
	return State.keys()[_state]


## The right-hand weapon's form, 0 for bare hands.
func player_weapon() -> int:
	return _weapon_form


func clip_prefix() -> String:
	return _prefix


## The clip the first-person body plays for `_state` at rest.
func idle_clip() -> String:
	return _prefix + "idle" if _state == State.DRAWN else ""


## The attack under way: {clip, power, left, speed, hit_time, length, window,
## time, hit}; {} between attacks.
func current_attack() -> Dictionary:
	return _attack


## The weapon's reach from the viewpoint in metres, without the target's radius.
func reach_metres() -> float:
	return _reach_metres(_weapon, _player_race())


## The equipment changed (or was loaded): the weapon in hand and the clip set
## follow it. While drawn the idle changes.
func on_items_changed() -> void:
	var equipment := _papyrus.equipment(PLAYER_REF)
	var form: int = equipment["right"]
	var extra: int = equipment["right_extra"]
	var info := _world.get_weapon(form) if form != 0 else {}
	var type := int(info.get("weapon_type", 0))
	var prefix := "h2h_"
	match type:
		1, 2, 3, 4:
			prefix = "1hm_"
		5:
			prefix = "2hm_"
		6:
			prefix = "2hw_"
		7, 8, 9:
			_warn.once("type%d" % type, "weapon type %d (bow, staff, crossbow) is not implemented: hands" % type)
			info = {}
			form = 0
			extra = 0
		_:
			pass
	var changed := prefix != _prefix or form != _weapon_form
	_weapon = info
	_weapon_form = form
	_weapon_extra = extra
	_prefix = prefix
	if changed:
		_player_plan = {}
		if _state == State.DRAWN:
			_attack = {}
			weapon_state_changed.emit(_state, idle_clip())
		elif _state == State.DRAWING or _state == State.SHEATHING:
			_state = State.SHEATHED
			weapon_state_changed.emit(_state, "")


# ---- the player's attacks

## The attack button went down. A sheathed weapon is drawn instead.
func attack_pressed() -> void:
	if _state == State.SHEATHED:
		toggle_weapon()
		return
	if _state != State.DRAWN:
		return
	_held = true
	_held_time = 0.0
	_held_fired = false


## The button went up: a light attack, unless a held press already began a
## power attack.
func attack_released() -> void:
	if not _held:
		return
	_held = false
	if not _held_fired:
		attack(false)


## Begin an attack now: a power attack, or the next light one (right and left
## in turn). `left` picks the hand of a light attack. False if the weapon is
## not drawn or an attack is still too early to follow.
func attack(power := false, left := false) -> bool:
	if _state != State.DRAWN:
		return false
	if not _attack.is_empty() and _attack["time"] < _attack["window"]:
		return false
	var use_left := left or (not power and _left_next)
	var stem := _clip_for(power, use_left)
	var timing := _timing(FIRST_PERSON_PROJECT, FIRST_PERSON_CLIPS, stem)
	var speed := maxf(float(_weapon.get("speed", 1.0)), 0.1)
	_attack = {"clip": stem, "power": power, "left": use_left, "speed": speed, "time": 0.0, "hit": false,
		"hit_time": timing["hit"], "length": timing["stop"], "window": timing["window"],
		"sounds": timing["sounds"], "sounds_done": 0}
	if not power:
		_left_next = not use_left
	attack_started.emit(stem, power)
	return true


## Sweep a blade segment (VR: the weapon's base and tip, engine space) against
## the actors; the first one in reach takes a hit as a light attack. The shell
## calls this when the tip's speed passes its threshold.
func hit_segment(base: Vector3, tip: Vector3) -> Dictionary:
	if _state != State.DRAWN:
		return {}
	var best := {}
	var best_distance := INF
	for candidate in _candidates(PLAYER_REF):
		if _cooldowns.get(candidate["ref"], 0.0) > _clock:
			continue
		var distance := _segment_distance(base, tip, candidate)
		if distance <= 0.0 and distance < best_distance:
			best = candidate
			best_distance = distance
	if best.is_empty():
		return {}
	_cooldowns[best["ref"]] = _clock + SWEEP_COOLDOWN
	var point := _body_point(best)
	var result := _deliver(PLAYER_REF, best["ref"], _weapon_form, _weapon_extra, EVENT_LIGHT, 0, point, _player_race(), _weapon)
	return {"target": best["ref"], "result": result, "point": point}


# ---- actors that attack

## `attacker` (a built actor) swings: it turns to `target` (the player or
## another built actor), plays its third-person clip, and after the clip's
## HitFrame tests from its head along its facing against every living built
## actor but itself and the player, `target` first. Combat AI (A9) calls this.
## False if it has no node or is dead.
func npc_attack(attacker: int, power := false, target := PLAYER_REF) -> bool:
	var node := _world.find_ref_node(attacker) as Node3D
	if node == null or not node.is_inside_tree() or _papyrus.is_actor_dead(attacker):
		return false
	_use_weapon(attacker, false)
	var plan := _actor_plan(attacker)
	var behaviour: String = plan.get("behaviour", "")
	var equipment := _papyrus.equipment(attacker)
	var form: int = equipment["right"]
	var info := _world.get_weapon(form) if form != 0 else {}
	var prefix := _prefix_of(int(info.get("weapon_type", 0)))
	if prefix == "":
		info = {}
		form = 0
		prefix = "h2h_"
	var stem := prefix + ("attackpower" if power else "attackright")
	var project := PROJECTS + behaviour.get_file().get_basename() + ".txt"
	var dir := (behaviour.get_base_dir() + "/animations/") if behaviour != "" else THIRD_PERSON_CLIPS
	var timing := _timing(project, dir, stem)
	var speed := maxf(float(info.get("speed", 1.0)), 0.1)
	_face(node, target)
	var clip := _clip_file(behaviour, stem, dir)
	if node.has_method("play_action"):
		node.play_action(clip, speed)
	_npc_attacks.append({"attacker": attacker, "target": target, "power": power, "weapon": form, "info": info,
		"extra": int(equipment["right_extra"]), "time": 0.0, "hit": false, "speed": speed,
		"hit_time": timing["hit"], "length": timing["stop"]})
	npc_attack_started.emit(attacker, clip, power)
	return true


## `attacker` shoots at `target`: the bow it carries (SkydotPapyrus.ranged_weapon) goes in
## its hands with the best arrows, it turns to the target and plays the bow's draw clip, and
## at the clip's release key (BOW_RELEASE_FRACTION of it without one) an arrow leaves through
## Projectiles with the AMMO's PROJ speed and gravity and fBowNPCSpreadAngle of spread. An
## arrow that enters an actor's body calls apply_hit with the bow and the arrow. False without
## a bow, a built living actor or Projectiles.
func npc_shoot(attacker: int, target := PLAYER_REF) -> bool:
	var node := _world.find_ref_node(attacker) as Node3D
	if node == null or not node.is_inside_tree() or _papyrus.is_actor_dead(attacker) or projectiles == null:
		return false
	var ranged := _papyrus.ranged_weapon(attacker)
	if ranged.is_empty() or int(ranged.get("ammo", 0)) == 0:
		_warn.once("noshot%d" % attacker, "0x%08X has no bow and arrows to shoot with" % attacker)
		return false
	_use_weapon(attacker, true)
	_face(node, target)
	var behaviour: String = _actor_plan(attacker).get("behaviour", "")
	var length := 0.0
	var release := 0.0
	for stem in BOW_CLIPS:
		var clip := _clip_file(behaviour, stem, "")
		if clip != "" and node.has_method("play_action"):
			length = node.play_action(clip, 1.0)
			if length > 0.0:
				release = length * BOW_RELEASE_FRACTION
				for note: Dictionary in _project_notes(PROJECTS + behaviour.get_file().get_basename() + ".txt", stem):
					if String(note["text"]).to_lower().contains("release"):
						release = float(note["time"])
				break
	_shots.append({"attacker": attacker, "target": target, "bow": int(ranged["weapon"]),
		"ammo": int(ranged["ammo"]), "left": minf(release, maxf(length, 0.0))})
	return true


## Put the weapon `ranged` (the bow with its arrows) or the melee weapon in `ref`'s hands, as
## the game's AI does when the fight changes, and show it. No clip plays.
func _use_weapon(ref: int, ranged: bool) -> void:
	var right: int = _papyrus.equipment(ref)["right"]
	var want := 0
	var ammo := 0
	if ranged:
		var bow := _papyrus.ranged_weapon(ref)
		want = int(bow.get("weapon", 0))
		ammo = int(bow.get("ammo", 0))
	else:
		want = _papyrus.melee_weapon(ref)
	if want == 0 or want == right:
		return
	_papyrus.equip(ref, want)
	if ammo != 0:
		_papyrus.equip(ref, ammo)
	gear.refresh(ref)
	var node := _world.find_ref_node(ref)
	if node != null and node.has_method("set_stance") and _drawn.get(ref, false):
		var info := _world.get_weapon(want)
		node.set_stance(STANCE_PREFIX.get(int(info.get("weapon_type", 0)), CREATURE_STANCE))


## `ref` draws its weapon (true) or sheathes it: the combat stance of the weapon
## in its right hand (a carried bow if it has no other weapon, `combat` for a
## creature), the equip or unequip clip on it, and the weapon model in hand or
## on its sheath halfway through the clip. False if it is not a built living actor.
func npc_draw(ref: int, drawn: bool) -> bool:
	var node := _world.find_ref_node(ref) as Node3D
	if node == null or not node.is_inside_tree() or _papyrus.is_actor_dead(ref):
		return false
	if _drawn.get(ref, false) == drawn:
		return true
	_drawn[ref] = drawn
	var info := npc_weapon(ref)
	var type := int(info.get("weapon_type", 0))
	var behaviour: String = _actor_plan(ref).get("behaviour", "")
	var length := 0.0
	if node.has_method("set_stance"):
		var stance: String = STANCE_PREFIX.get(type, CREATURE_STANCE)
		node.set_stance(stance if drawn else "")
		if EQUIP_PREFIX.has(type):
			var clip := _clip_file(behaviour, EQUIP_PREFIX[type] + ("_equip" if drawn else "_unequip"), "")
			if clip != "":
				length = node.play_action(clip, 1.0)
	if length > 0.0:
		_swaps.append({"ref": ref, "left": length * GEAR_SWAP_FRACTION, "drawn": drawn})
	else:
		gear.set_drawn(ref, drawn)
	npc_drawn_changed.emit(ref, drawn)
	return true


## Whether `ref` fights with its weapon out (`npc_draw`).
func npc_is_drawn(ref: int) -> bool:
	return _drawn.get(ref, false)


## The weapon `ref` fights with, as SkydotWorld.get_weapon: the right hand's, else the bow it
## carries, else {} (hands or claws).
func npc_weapon(ref: int) -> Dictionary:
	var form: int = _papyrus.equipment(ref)["right"]
	if form != 0:
		return _world.get_weapon(form)
	var ranged := _papyrus.ranged_weapon(ref)
	return _world.get_weapon(int(ranged["weapon"])) if not ranged.is_empty() else {}


## The animation file of the actor's project clip `stem` (any case), else
## `dir` + stem + ".hkx" (or "" when `dir` is).
func _clip_file(behaviour: String, stem: String, dir: String) -> String:
	if behaviour != "":
		if not _locomotions.has(behaviour):
			_locomotions[behaviour] = _world.get_locomotion(behaviour)
		var clips: Dictionary = _locomotions[behaviour].get("clips", {})
		if clips.has(stem.to_lower()):
			return clips[stem.to_lower()]["file"]
	return dir + stem + ".hkx" if dir != "" else ""


## Turn `node` to face the actor (or the player) `target`.
func _face(node: Node3D, target: int) -> void:
	if not node.has_method("face"):
		return
	var at: Vector3
	if target == PLAYER_REF:
		at = _feet.call()
	else:
		var other := _world.find_ref_node(target) as Node3D
		if other == null:
			return
		at = other.global_position
	var offset := at - node.global_position
	if Vector2(offset.x, offset.z).length() > 0.01:
		node.face(atan2(-offset.x, -offset.z))


# ---- the frame

## Advance the weapon, the attacks and the impact models by `delta` seconds.
func step(delta: float) -> void:
	_clock += delta
	_step_state(delta)
	_step_attack(delta)
	_step_npc_attacks(delta)
	_step_swaps(delta)
	_step_shots(delta)
	_step_effects(delta)


func _step_state(delta: float) -> void:
	if _state == State.DRAWING or _state == State.SHEATHING:
		_state_time += delta
		if _state_time >= _state_length:
			_state = State.DRAWN if _state == State.DRAWING else State.SHEATHED
			_left_next = false
			weapon_state_changed.emit(_state, idle_clip())


func _step_attack(delta: float) -> void:
	if _held and not _held_fired and _state == State.DRAWN:
		_held_time += delta
		if _held_time >= _power_delay() and (_attack.is_empty() or _attack["time"] >= _attack["window"]):
			_held_fired = true
			attack(true)
	if _attack.is_empty():
		return
	var before: float = _attack["time"]
	_attack["time"] = before + delta * _attack["speed"]
	var now: float = _attack["time"]
	for entry: Array in _attack["sounds"]:
		if entry[0] > before and entry[0] <= now:
			_sounds.play_named(String(entry[1]).trim_prefix("SoundPlay."), null, 0)
	if not _attack["hit"] and now >= _attack["hit_time"]:
		_attack["hit"] = true
		_player_strike(_attack["power"], _attack["left"])
	if now >= _attack["length"]:
		_attack = {}
		if _state == State.DRAWN:
			weapon_state_changed.emit(_state, idle_clip())


func _step_shots(delta: float) -> void:
	for shot in _shots.duplicate():
		shot["left"] -= delta
		if shot["left"] <= 0.0:
			_shots.erase(shot)
			_release(shot)


## The arrow leaves: toward the target's chest, the drop at that range made up for, a random
## spread of up to fBowNPCSpreadAngle degrees about the aim.
func _release(shot: Dictionary) -> void:
	var attacker: int = shot["attacker"]
	var node := _world.find_ref_node(attacker) as Node3D
	if node == null or not node.is_inside_tree() or _papyrus.is_actor_dead(attacker):
		return
	var target: int = shot["target"]
	var chest: Vector3
	if target == PLAYER_REF:
		chest = _feet.call() + Vector3.UP * PLAYER_HEIGHT * CHEST_FRACTION
	else:
		var other := _world.find_ref_node(target) as Node3D
		if other == null:
			return
		chest = other.global_position + Vector3.UP * float(other.get("height")) * CHEST_FRACTION
	var height: float = node.get_height() if node.has_method("get_height") else 1.8
	var origin := node.global_position + Vector3.UP * height * SHOT_HEIGHT_FRACTION
	var ammo_data := _world.get_ammo(shot["ammo"])
	var proj := _world.get_projectile(int(ammo_data.get("projectile", 0)))
	if proj.is_empty():
		_warn.once("noproj%d" % shot["ammo"], "ammo 0x%08X has no projectile" % shot["ammo"])
		return
	var speed: float = float(proj["speed"]) * SkydotWorld.unit_scale()
	var aim := chest - origin
	var flight := aim.length() / maxf(speed, 1.0)
	aim.y += 0.5 * GRAVITY * float(proj["gravity"]) * flight * flight
	var direction := aim.normalized()
	var spread := _world.get_gmst_float(GMST_BOW_SPREAD, BOW_SPREAD_FALLBACK)
	var tilt := deg_to_rad(spread) * _rng.randf()
	var axis := direction.cross(Vector3.UP).normalized().rotated(direction, _rng.randf() * TAU)
	direction = direction.rotated(axis, tilt)
	origin += direction * SHOT_AHEAD
	print("combat: arrow 0x%X -> 0x%X (ammo 0x%X, bow 0x%X): %.1f m/s, spread %.2f of %.1f deg" % [
		attacker, target, shot["ammo"], shot["bow"], speed, rad_to_deg(tilt), spread])
	npc_arrow.emit(attacker, target)
	projectiles.shoot(shot["ammo"], origin, direction, proj, ammo_data,
		func(from: Vector3, to: Vector3) -> Dictionary: return segment_hit(attacker, from, to),
		func(ref: int, point: Vector3) -> void:
			_deliver(attacker, ref, shot["bow"], 0, EVENT_BOW, 0, point, _race(int(_actor_plan(attacker).get("race", 0))),
				_world.get_weapon(shot["bow"]), shot["ammo"]))


## The first living actor but `attacker` (the player too) whose body the segment crosses:
## {ref, point} or {}.
func segment_hit(attacker: int, from: Vector3, to: Vector3) -> Dictionary:
	for candidate in _candidates(attacker):
		var feet: Vector3 = candidate["feet"]
		var top := feet + Vector3.UP * float(candidate["height"])
		var steps := maxi(int(from.distance_to(to) / 0.2), 1)
		for i in steps + 1:
			var p := from.lerp(to, float(i) / steps)
			var q := Geometry3D.get_closest_point_to_segment(p, feet, top)
			if p.distance_to(q) <= float(candidate["radius"]):
				return {"ref": candidate["ref"], "point": p}
	return {}


func _step_swaps(delta: float) -> void:
	for swap in _swaps.duplicate():
		swap["left"] -= delta
		if swap["left"] <= 0.0:
			_swaps.erase(swap)
			gear.set_drawn(swap["ref"], swap["drawn"])


func _step_npc_attacks(delta: float) -> void:
	var finished: Array[Dictionary] = []
	for attack_data in _npc_attacks:
		attack_data["time"] += delta * attack_data["speed"]
		if not attack_data["hit"] and attack_data["time"] >= attack_data["hit_time"]:
			attack_data["hit"] = true
			_npc_strike(attack_data)
		if attack_data["time"] >= attack_data["length"]:
			finished.append(attack_data)
	for attack_data in finished:
		_npc_attacks.erase(attack_data)


func _step_effects(delta: float) -> void:
	for effect in _effects.duplicate():
		effect["left"] -= delta
		if effect["left"] <= 0.0 or not is_instance_valid(effect["node"]):
			if is_instance_valid(effect["node"]):
				effect["node"].queue_free()
			_effects.erase(effect)


# ---- hits

## The viewpoint's hit test at the HitFrame of a player attack.
func _player_strike(power: bool, left: bool) -> void:
	var pointer: Node3D = _pointer.call()
	var origin := pointer.global_position
	var forward := -pointer.global_transform.basis.z
	var race := _player_race()
	var event := EVENT_POWER if power else (EVENT_LIGHT_LEFT if left else EVENT_LIGHT)
	var hit := _find_target(PLAYER_REF, origin, forward, _weapon, race, event, power)
	if hit.is_empty():
		return
	var flags := SkydotPapyrus.HIT_POWER if power else 0
	_deliver(PLAYER_REF, hit["ref"], _weapon_form, _weapon_extra, event, flags, hit["point"], race, _weapon)


func _npc_strike(data: Dictionary) -> void:
	var attacker: int = data["attacker"]
	var node := _world.find_ref_node(attacker) as Node3D
	if node == null or not node.is_inside_tree() or _papyrus.is_actor_dead(attacker):
		return
	var height: float = node.get_height() if node.has_method("get_height") else 1.8
	var origin := node.global_position + Vector3.UP * height * HEAD_FRACTION
	var forward := -node.global_transform.basis.z
	var plan := _actor_plan(attacker)
	var race := _race(int(plan.get("race", 0)))
	var power: bool = data["power"]
	var event := EVENT_POWER if power else EVENT_LIGHT
	var hit := _find_target(attacker, origin, forward, data["info"], race, event, power, data["target"])
	if hit.is_empty():
		return
	_deliver(attacker, hit["ref"], data["weapon"], data["extra"], event,
		SkydotPapyrus.HIT_POWER if power else 0, hit["point"], race, data["info"])


## The nearest living target within reach, inside the strike angle and in
## line of sight: {ref, point} or {}. `prefer` (an actor ref, 0 for none) wins
## over a nearer one when it is a candidate too.
func _find_target(attacker: int, origin: Vector3, forward: Vector3, weapon: Dictionary, race: Dictionary,
		event: String, power: bool, prefer := 0) -> Dictionary:
	var reach := _reach_metres(weapon, race)
	var half_angle := deg_to_rad(_strike_angle(race, event, power))
	var space := _host.get_world_3d().direct_space_state
	var best := {}
	var best_distance := INF
	for candidate in _candidates(attacker):
		var closest := _closest_on_body(origin, candidate)
		var offset: Vector3 = closest - origin
		var distance := offset.length() - float(candidate["radius"])
		if distance > reach:
			continue
		if distance > 0.0 and forward.angle_to(offset) > half_angle:
			continue
		var query := PhysicsRayQueryParameters3D.create(origin, closest,
			SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
		if not space.intersect_ray(query).is_empty():
			continue
		if candidate["ref"] == prefer:
			return {"ref": candidate["ref"], "point": _body_point(candidate)}
		if distance < best_distance:
			best_distance = distance
			best = {"ref": candidate["ref"], "point": _body_point(candidate)}
	return best


## Everything an attack by `attacker` can hit: the built, living actors but
## itself, and the player for an NPC's attack. [{ref, feet, radius, height}]
func _candidates(attacker: int) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	if attacker != PLAYER_REF and not _papyrus.is_actor_dead(PLAYER_REF):
		out.append({"ref": PLAYER_REF, "feet": _feet.call(), "radius": PLAYER_RADIUS, "height": PLAYER_HEIGHT})
	for node in _host.find_children("*", "SkydotActor", true, false):
		var actor := node as SkydotActor
		if actor == null or not actor.has_meta("skydot_ref") or not actor.is_inside_tree():
			continue
		var ref: int = actor.get_meta("skydot_ref")
		if ref == attacker or _papyrus.is_actor_dead(ref):
			continue
		out.append({"ref": ref, "feet": actor.global_position, "radius": actor.radius, "height": actor.height})
	return out


## The point of the body's axis closest to `origin`.
static func _closest_on_body(origin: Vector3, candidate: Dictionary) -> Vector3:
	var feet: Vector3 = candidate["feet"]
	var height: float = candidate["height"]
	return Vector3(feet.x, clampf(origin.y, feet.y, feet.y + height), feet.z)


static func _body_point(candidate: Dictionary) -> Vector3:
	var feet: Vector3 = candidate["feet"]
	var height: float = candidate["height"]
	return feet + Vector3.UP * (height * CHEST_FRACTION)


## How far the segment is from the body's cylinder, metres; zero or less when
## it touches (the axis segment's distance minus the radius).
static func _segment_distance(a: Vector3, b: Vector3, candidate: Dictionary) -> float:
	var feet: Vector3 = candidate["feet"]
	var height: float = candidate["height"]
	var top := feet + Vector3.UP * height
	var best := INF
	var steps := 8
	for i in steps + 1:
		var p := a.lerp(b, float(i) / steps)
		var q := Geometry3D.get_closest_point_to_segment(p, feet, top)
		best = minf(best, p.distance_to(q))
	return best - float(candidate["radius"])


## Apply the hit, tell the shells, and leave the impact.
func _deliver(attacker: int, target: int, weapon: int, extra: int, event: String, flags: int, point: Vector3,
		attacker_race: Dictionary, weapon_info: Dictionary, ammo := 0) -> Dictionary:
	var result := _papyrus.apply_hit(attacker, target, weapon, extra, event, flags, ammo)
	if result.has("error"):
		return result
	_impact(weapon_info, attacker_race, target, point)
	hit_landed.emit(target, result, point)
	return result


## Weapon IPDS (the attacker race's for hands) -> the target race's material
## -> IPCT: its sound on the target, its model at the hit point.
func _impact(weapon_info: Dictionary, attacker_race: Dictionary, target: int, point: Vector3) -> void:
	var data_set := int(weapon_info.get("impact_data_set", 0))
	if data_set == 0:
		data_set = int(attacker_race.get("impact_data_set", 0))
	var race := _race(int(_actor_plan(target).get("race", 0)))
	var material := int(race.get("impact_material", 0))
	if data_set == 0 or material == 0:
		_warn.once("noimpact%d_%d" % [data_set, material], "no impact for data set 0x%X on material 0x%X" % [data_set, material])
		return
	var impact := _world.get_impact(data_set, material)
	if impact.is_empty():
		_warn.once("noimpact%d_%d" % [data_set, material], "data set 0x%X has no impact for material 0x%X" % [data_set, material])
		return
	var sound := int(impact["sound1"])
	if sound != 0:
		_sounds.play(sound, target)
	var model: String = impact["model"]
	if model.is_empty():
		return
	var node := _world.build_model(model)
	if node == null:
		_warn.once("model" + model, "impact model %s cannot be built" % model)
		return
	if node.find_children("*", "MeshInstance3D", true, false).is_empty():
		_warn.once("model" + model, "impact model %s has no mesh (particles do not render yet)" % model)
	_host.add_child(node)
	node.global_position = point
	_effects.append({"node": node, "left": clampf(float(impact["duration"]), IMPACT_MIN_SECONDS, IMPACT_MAX_SECONDS)})


# ---- clips and timing

## The clip stem of an attack: the power attack falls back to a light one
## where the set has none (hands).
func _clip_for(power: bool, left: bool) -> String:
	var names: Array[String] = []
	if power:
		names.append(_prefix + "attackpower")
	names.append(_prefix + ("attackleft" if left else "attackright"))
	names.append(_prefix + "attackright")
	for stem in names:
		if _pack.has(FIRST_PERSON_CLIPS + stem + ".hkx"):
			return stem
	return names[names.size() - 1]


## The times of a clip in seconds of its own: {hit, window, stop, length,
## sounds: [[time, text]]}. From the project's text keys (`HitFrame`,
## `AttackWinStart`, `attackStop`, `SoundPlay.*`), else from the length.
func _timing(project: String, dir: String, stem: String) -> Dictionary:
	var key := project + "|" + dir + stem
	if _timings.has(key):
		return _timings[key]
	var length := 1.0
	var info: Dictionary = {}
	var bytes := _pack.get_bytes(dir + stem + ".hkx")
	if bytes.size() > 0:
		info = SkydotAnimation.describe(bytes)
		if info.has("clips") and not info["clips"].is_empty():
			length = float(info["clips"][0]["duration"])
	var notes := _project_notes(project, stem)
	var out := {"hit": -1.0, "window": -1.0, "stop": -1.0, "length": length, "sounds": []}
	for note: Dictionary in notes:
		var text := String(note["text"]).to_lower()
		var time := float(note["time"])
		if text == "hitframe" and out["hit"] < 0.0:
			out["hit"] = time
		elif text.begins_with("attackwinstart") and out["window"] < 0.0:
			out["window"] = time
		elif (text == "attackstop" or text == "powerattackstop") and out["stop"] < 0.0:
			out["stop"] = time
		elif text.begins_with("soundplay."):
			out["sounds"].append([time, note["text"]])
	if out["hit"] < 0.0:
		out["hit"] = length * HIT_FRACTION_FALLBACK
		if stem.contains("attack"):
			_warn.once("nohit" + stem, "clip %s has no HitFrame: hit at %.0f%% of its length" % [stem, HIT_FRACTION_FALLBACK * 100.0])
	if out["stop"] < 0.0:
		out["stop"] = length
	if out["window"] < 0.0:
		out["window"] = out["stop"] * 0.6
	_timings[key] = out
	return out


## The text keys of project clip `stem` (any case; an NPC variant too).
func _project_notes(project: String, stem: String) -> Array:
	if not _projects.has(project):
		var table := {}
		if _pack.has(project):
			var info := SkydotAnimation.describe(_pack.get_bytes(project))
			for clip: Dictionary in info.get("project_clips", []):
				table[String(clip["name"]).to_lower()] = clip["annotations"]
		_projects[project] = table
	var table: Dictionary = _projects[project]
	for name in [stem, stem + "npc"]:
		if table.has(name):
			return table[name]
	return []


## Begin DRAWING or SHEATHING: its length is the clip's, to the annotation
## `done` or the end.
func _begin(state: int, stem: String, done: String) -> void:
	var timing := _timing(FIRST_PERSON_PROJECT, FIRST_PERSON_CLIPS, stem)
	_state = state
	_state_time = 0.0
	_state_length = float(timing["length"])
	for note: Dictionary in _project_notes(FIRST_PERSON_PROJECT, stem):
		if String(note["text"]).to_lower() in ["weapequip_out", "unequip_out"]:
			_state_length = minf(_state_length, float(note["time"]))
	if not _pack.has(FIRST_PERSON_CLIPS + stem + ".hkx"):
		_state_length = 0.0
		_warn.once("noclip" + stem, "no first-person clip %s (%s)" % [stem, done])
	weapon_state_changed.emit(state, stem)


func _power_delay() -> float:
	return _world.get_gmst_float(GMST_POWER_DELAY, POWER_DELAY_FALLBACK)


func _reach_metres(weapon: Dictionary, race: Dictionary) -> float:
	var units: float
	if weapon.is_empty():
		units = float(race.get("unarmed_reach", 96.0))  # the race's reach is in game units
	else:
		units = float(weapon["reach"]) * _world.get_gmst_float(GMST_COMBAT_DISTANCE, COMBAT_DISTANCE_FALLBACK)
	return units * SkydotWorld.unit_scale()


## Half the cone of the race's attack of `event`, degrees.
func _strike_angle(race: Dictionary, event: String, power: bool) -> float:
	for attack_data: Dictionary in race.get("attacks", []):
		if String(attack_data["event"]).to_lower() == event.to_lower():
			return float(attack_data["strike_angle"])
	return STRIKE_ANGLE_POWER if power else STRIKE_ANGLE_LIGHT


static func _prefix_of(type: int) -> String:
	match type:
		0:
			return "h2h_"
		1, 2, 3, 4:
			return "1hm_"
		5:
			return "2hm_"
		6:
			return "2hw_"
	return ""


func _race(id: int) -> Dictionary:
	if id == 0:
		return {}
	if not _races.has(id):
		_races[id] = _world.get_race(id)
	return _races[id]


func _player_race() -> Dictionary:
	if _player_plan.is_empty():
		_player_plan = _world.get_npc_plan(PLAYER_NPC, PLAYER_REF)
	return _race(int(_player_plan.get("race", 0)))


func _actor_plan(ref: int) -> Dictionary:
	if ref == PLAYER_REF:
		_player_race()
		return _player_plan
	if not _plans.has(ref):
		_plans[ref] = _world.get_actor_plan(ref)
	return _plans[ref]
