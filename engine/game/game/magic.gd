# SPDX-License-Identifier: GPL-3.0-or-later
#
# The player's casting (GAMEPLAY-PLAN.md, 7a-3; SYSTEMS.md A9). No camera, input or scene of
# its own, like Combat: it knows a pointer node (the viewpoint, or a VR controller) and the
# built actors under the host, both given by the session. The rules (cost, cooldown, the
# hands' charge and concentration, landing the effects) are SkydotPapyrus's; this class
# carries them out in the scene.
#
# Hands: a spell or scroll held in a hand (SkydotPapyrus.equip_spell; 0 left, 1 right, 2
# power) is readied with R (GameSession.toggle_weapon) and cast with that hand's attack
# action. The first press on a sheathed hand only readies it. Fire and forget: press, hold
# for the casting time ("charged"), release: the cost is paid and the spell delivered.
# Released early: nothing happens and nothing is paid. Concentration: every step pays for the
# time and lands the effects again (ten times a second) until the button goes up or the
# Magicka is gone. A power is cast at once by `cast_power` (the power key).
#
# Delivery (CastInfo.delivery): self lands on the player; touch lands on the nearest actor in
# front within a hand's reach; aimed launches the PROJ of the spell's costliest effect through
# Projectiles by its type (missile and lobber fly with their speed and gravity, flames leave as
# short bolts ten times a second, beams are a ray, cones a cone test) and lands where it
# arrives; target actor and target location land at the crosshair. An area (area_units)
# lands on every actor within it around the impact, the explosion model of the spell's
# effect (else its PROJ) appears there, and so does the effect's hit art. A scroll lands the
# same way and is used up by its first landing (an aimed scroll that meets nothing is not).
#
# Shells draw it through signals: `hand_clip` (the first-person clip to play, or "" to give the
# arms back to Combat), `ready_changed`, `hand_art_changed` (the casting art model on a hand).
class_name Magic
extends RefCounted

const LEFT := 0
const RIGHT := 1
const POWER := 2
const PLAYER_REF := 0x14

const FIRST_PERSON_PROJECT := "meshes/animationdata/firstperson.txt"
const FIRST_PERSON_CLIPS := "meshes/actors/character/_1stperson/animations/"

## PROJ types (DATA).
const PROJ_MISSILE := 1
const PROJ_LOBBER := 2
const PROJ_BEAM := 4
const PROJ_FLAME := 8
const PROJ_CONE := 0x10
## SPIT delivery.
const DELIVER_SELF := 0
const DELIVER_TOUCH := 1
const DELIVER_AIMED := 2
const DELIVER_TARGET_ACTOR := 3
const DELIVER_TARGET_LOCATION := 4
const CAST_CONCENTRATION := 2

## Seconds between two landings (and flame bolts) of a concentration spell.
const TICK_SECONDS := 0.1
## Placeholder: a touch spell's reach from the eyes in game units, and half its cone in degrees.
const TOUCH_REACH_UNITS := 128.0
const TOUCH_ANGLE := 50.0
## Placeholder: the reach of a target-at-crosshair spell in game units (info.range is the
## spell's own when it has one), and half its cone for an actor.
const TARGET_REACH_UNITS := 4000.0
const TARGET_ANGLE := 4.0
## A cone spell's half angle in degrees when its PROJ gives none, and its reach in game units.
const CONE_ANGLE_FALLBACK := 25.0
const CONE_REACH_UNITS := 600.0
## Where a shot leaves the hand: ahead of the eyes, to the side, below, metres.
const SHOT_AHEAD := 0.5
const SHOT_SIDE := 0.15
const SHOT_BELOW := 0.12
## How far the crosshair's ray looks for something to aim at, metres.
const AIM_DISTANCE := 60.0
## Seconds an explosion or hit art model stays.
const EFFECT_SECONDS := 2.0
const MASK := SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_CLUTTER | SkydotPlayer.LAYER_TERRAIN

enum Ready { SHEATHED, READYING, READY }

## The first-person arms should play `clip` (a name without path or extension; looping when
## `loops`) at `speed`; "" gives them back to Combat (its idle, or hidden).
signal hand_clip(clip: String, loops: bool, speed: float)
## The hands went to the ready or were lowered.
signal ready_changed(is_ready: bool)
## The casting art on a hand changed: `hand` (LEFT, RIGHT) and the model path ("" for none).
signal hand_art_changed(hand: int, model: String)
## A cast was paid and left the hand (a fire-and-forget spell's release, a concentration
## spell's start, a power): the hand, the spell, and cast_info's dictionary.
signal cast_started(hand: int, spell: int, info: Dictionary)
## A spell's effects landed on `target` at `point` (engine space).
signal spell_landed(hand: int, spell: int, target: int, point: Vector3, hostile: bool)
## A scroll was read and used up (the hands that held the last one are empty).
signal scroll_used(spell: int)
## The hand's cast was refused (not enough Magicka, cooldown, ...): hand, spell, reason.
signal cast_refused(hand: int, spell: int, reason: String)

var _world: SkydotWorld
var _pack: SkydotPack
var _papyrus: SkydotPapyrus
var _host: Node3D
var _combat: Combat
var _projectiles: Projectiles
var _pointer: Callable  # () -> Node3D: where casts start and look from
var _warn := WarnOnce.new("magic: ")

var _ready := Ready.SHEATHED
var _ready_time := 0.0
var _ready_length := 0.0
var _hands := {}  # hand -> {spell, info, phase, timer}: the buttons that are down
var _clip_left := 0.0  # seconds until a release clip is over and the idle returns
var _art := {LEFT: "", RIGHT: ""}  # the models last announced per hand
var _effects: Array[Dictionary] = []  # explosion and hit art models: {node, left}
var _lengths := {}  # clip stem -> seconds
var _clock := 0.0
var _info_cache := {}  # spell -> cast_info


func _init(world: SkydotWorld, pack: SkydotPack, papyrus: SkydotPapyrus, host: Node3D, combat: Combat,
		projectiles: Projectiles, pointer: Callable) -> void:
	_world = world
	_pack = pack
	_papyrus = papyrus
	_host = host
	_combat = combat
	_projectiles = projectiles
	_pointer = pointer


# ---- what the hands hold

## The spell or scroll in `hand` (0 for none).
func spell_in(hand: int) -> int:
	return int(_papyrus.equipped_spell(PLAYER_REF, hand))


## The hands (LEFT, RIGHT) that hold a spell or a scroll.
func hands_with_spells() -> Array[int]:
	var out: Array[int] = []
	for hand in [LEFT, RIGHT]:
		if spell_in(hand) != 0:
			out.append(hand)
	return out


func is_ready() -> bool:
	return _ready != Ready.SHEATHED


func ready_state() -> int:
	return _ready


## Whether `hand`'s button is down on a spell.
func is_casting(hand: int) -> bool:
	return _hands.has(hand)


## The phase of `hand`: "" (idle), "charging", "charged", "concentrating".
func phase_of(hand: int) -> String:
	return _hands[hand]["phase"] if _hands.has(hand) else ""


## cast_info of `spell`, cached ({} if unknown).
func info_of(spell: int) -> Dictionary:
	if not _info_cache.has(spell):
		_info_cache[spell] = _papyrus.cast_info(spell)
	return _info_cache[spell]


## The equipment changed (or a game was loaded): the hands' spells and their art follow it.
func on_items_changed() -> void:
	_info_cache.clear()
	for hand in _hands.keys():
		if spell_in(hand) != (_hands[hand]["spell"] as int):
			_papyrus.hand_release(PLAYER_REF, hand)
			_hands.erase(hand)
	if _ready != Ready.SHEATHED and hands_with_spells().is_empty():
		_lower(true)
	_refresh_art()
	if _ready == Ready.READY and _hands.is_empty():
		hand_clip.emit(idle_clip(), true, 1.0)


# ---- readying

## Ready the hands that hold spells, or lower them. `silent`: Combat plays the arms (a weapon
## drawn in the other hand).
func toggle_ready(silent := false) -> void:
	if _ready == Ready.SHEATHED:
		set_ready(true, silent)
	elif _ready == Ready.READY:
		set_ready(false, silent)


## Ready (true) or lower (false) the hands; ignored while they already are, or hold nothing.
func set_ready(on: bool, silent := false) -> void:
	if on == (_ready != Ready.SHEATHED):
		return
	if not on:
		_lower(silent)
		return
	if hands_with_spells().is_empty():
		return
	var stem := _hand_clip("equip", hands_with_spells())
	_ready = Ready.READYING
	_ready_time = 0.0
	_ready_length = _length_of(stem) if (stem != "" and not silent) else 0.0
	_refresh_art()
	ready_changed.emit(true)
	if silent or stem == "":
		_finish_ready()
	else:
		hand_clip.emit(stem, false, 1.0)


func _lower(silent: bool) -> void:
	for hand in _hands.keys():
		_papyrus.hand_release(PLAYER_REF, hand)
	_hands.clear()
	var was_ready := _ready != Ready.SHEATHED
	_ready = Ready.SHEATHED
	_clip_left = 0.0
	_refresh_art()
	if not was_ready:
		return
	ready_changed.emit(false)
	var stem := _hand_clip("unequip", hands_with_spells()) if not silent else ""
	hand_clip.emit(stem, false, 1.0)


func _finish_ready() -> void:
	_ready = Ready.READY
	_ready_time = 0.0
	hand_clip.emit(idle_clip() if _combat.weapon_state() != Combat.State.DRAWN else "", true, 1.0)


## The clip the arms idle in while the hands are ready ("" with no spell, or while Combat's
## weapon is drawn: its idle is the arms').
func idle_clip() -> String:
	if _ready != Ready.READY:
		return ""
	var held := hands_with_spells()
	if held.is_empty():
		return ""
	return _hand_clip("readyloop", [held[held.size() - 1]])


# ---- the attack buttons

## `hand`'s attack button went down. A sheathed hand is readied instead.
func hand_pressed(hand: int) -> void:
	if _papyrus.is_player_dead() or _hands.has(hand):
		return
	if spell_in(hand) == 0:
		return
	if _ready == Ready.SHEATHED:
		set_ready(true)
		return
	if _ready != Ready.READY:
		return
	var press := _papyrus.hand_press(PLAYER_REF, hand)
	if not press["ok"]:
		cast_refused.emit(hand, spell_in(hand), press["reason"])
		print("magic: hand %d refused: %s" % [hand, press["reason"]])
		return
	var spell := int(press["spell"])
	var info := info_of(spell)
	_hands[hand] = {"spell": spell, "info": info, "phase": "charging", "timer": TICK_SECONDS,
		"scroll": bool(press["is_scroll"]), "started": false}
	var conc := int(press["casting_type"]) == CAST_CONCENTRATION
	var stem := _hand_clip("concentration" if conc else "chargeloop", [hand], info)
	_clip_left = 0.0
	hand_clip.emit(stem, true, 1.0)


## The button went up: a charged fire-and-forget spell is cast, a concentration spell ends.
func hand_released(hand: int) -> void:
	if not _hands.has(hand):
		return
	var held: Dictionary = _hands[hand]
	var result := _papyrus.hand_release(PLAYER_REF, hand)
	_hands.erase(hand)
	var spell: int = held["spell"]
	var info: Dictionary = held["info"]
	var event: String = result["event"]
	if event == "cast":
		if _pay(hand, spell, info):
			_deliver(hand, spell, info, false)
		_after_release(hand, info, "release")
	elif event == "stop":
		_after_release(hand, info, "")
	else:
		_after_release(hand, info, "")


## Cast the power in the power slot now. {ok, reason}.
func cast_power() -> Dictionary:
	var spell := spell_in(POWER)
	if spell == 0:
		return {"ok": false, "reason": "no power"}
	if _papyrus.is_player_dead():
		return {"ok": false, "reason": "dead"}
	var info := info_of(spell)
	if not _pay(POWER, spell, info):
		return {"ok": false, "reason": "refused"}
	_deliver(POWER, spell, info, false)
	var stem := _hand_clip("selfrelease" if int(info.get("delivery", 0)) == DELIVER_SELF else "release", [RIGHT], info)
	if stem != "" and _ready != Ready.SHEATHED:
		_clip_left = _length_of(stem)
		hand_clip.emit(stem, false, 1.0)
	return {"ok": true, "reason": ""}


## Pay for a fire-and-forget cast (a scroll is paid for when it lands). False, with
## `cast_refused`, if it cannot.
func _pay(hand: int, spell: int, info: Dictionary) -> bool:
	if bool(info.get("is_scroll", false)):
		cast_started.emit(hand, spell, info)
		return true
	var paid := _papyrus.pay_spell(PLAYER_REF, spell, 0.0)
	if not paid["ok"]:
		cast_refused.emit(hand, spell, paid["reason"])
		print("magic: 0x%08X refused: %s" % [spell, paid["reason"]])
		return false
	cast_started.emit(hand, spell, info)
	return true


func _after_release(hand: int, info: Dictionary, kind: String) -> void:
	if _ready == Ready.SHEATHED:
		return
	if kind == "release":
		var stem := _hand_clip("selfrelease" if int(info.get("delivery", 0)) == DELIVER_SELF else "release", [hand], info)
		if stem != "":
			_clip_left = _length_of(stem)
			hand_clip.emit(stem, false, 1.0)
			return
	_play_idle()


func _play_idle() -> void:
	_clip_left = 0.0
	hand_clip.emit(idle_clip(), true, 1.0)


# ---- the frame

## Advance the readying, the held buttons and the effect models by `delta` seconds.
func step(delta: float) -> void:
	_clock += delta
	if _ready == Ready.READYING:
		_ready_time += delta
		if _ready_time >= _ready_length:
			_finish_ready()
	if _clip_left > 0.0:
		_clip_left -= delta
		if _clip_left <= 0.0 and _hands.is_empty():
			_play_idle()
	for hand in _hands.keys():
		if _hands.has(hand):
			_step_hand(hand, delta)
	for effect in _effects.duplicate():
		effect["left"] -= delta
		if effect["left"] <= 0.0 or not is_instance_valid(effect["node"]):
			if is_instance_valid(effect["node"]):
				effect["node"].queue_free()
			_effects.erase(effect)


func _step_hand(hand: int, delta: float) -> void:
	var held: Dictionary = _hands[hand]
	var step := _papyrus.hand_step(PLAYER_REF, hand, delta)
	match String(step["event"]):
		"charged":
			held["phase"] = "charged"
		"tick":
			if not held["started"]:
				held["started"] = true
				held["phase"] = "concentrating"
				cast_started.emit(hand, held["spell"], held["info"])
			held["timer"] += delta
			if held["timer"] >= TICK_SECONDS:
				held["timer"] = 0.0
				if held["scroll"]:
					return
				_deliver(hand, held["spell"], held["info"], true)
		"stop":
			held["phase"] = "stopped"
			print("magic: 0x%08X ends: out of Magicka" % held["spell"])
			_hands.erase(hand)
			_after_release(hand, held["info"], "")


# ---- delivery

## Send the spell where its delivery says. `again`: one tick of a concentration spell.
func _deliver(hand: int, spell: int, info: Dictionary, again: bool) -> void:
	var pointer: Node3D = _pointer.call()
	if pointer == null:
		return
	var eye := pointer.global_position
	var forward := -pointer.global_transform.basis.z
	match int(info.get("delivery", DELIVER_SELF)):
		DELIVER_SELF:
			_land(hand, spell, info, PLAYER_REF, eye)
		DELIVER_TOUCH:
			var hit := _combat.find_in_cone(PLAYER_REF, eye, forward, TOUCH_REACH_UNITS * SkydotWorld.unit_scale(), TOUCH_ANGLE)
			if hit.is_empty():
				return
			_impact(hand, spell, info, hit["ref"], hit["point"])
		DELIVER_AIMED:
			_launch(hand, spell, info, eye, forward, pointer, again)
		DELIVER_TARGET_ACTOR:
			var reach := maxf(float(info.get("range", 0.0)), TARGET_REACH_UNITS) * SkydotWorld.unit_scale()
			var hit := _combat.find_in_cone(PLAYER_REF, eye, forward, reach, TARGET_ANGLE)
			if hit.is_empty():
				_warn.once("notarget%d" % spell, "0x%08X found no actor at the crosshair" % spell)
				return
			_impact(hand, spell, info, hit["ref"], hit["point"])
		DELIVER_TARGET_LOCATION:
			_impact(hand, spell, info, 0, _aim_point(eye, forward))
		_:
			_warn.once("delivery%d" % spell, "0x%08X has delivery %s: not implemented" % [spell, info.get("delivery")])


## Aimed: the PROJ's type decides.
func _launch(hand: int, spell: int, info: Dictionary, eye: Vector3, forward: Vector3, pointer: Node3D, again: bool) -> void:
	var proj := _world.get_projectile(int(info.get("projectile", 0)))
	if proj.is_empty():
		_warn.once("noproj%d" % spell, "0x%08X is aimed but has no projectile: it lands at the crosshair" % spell)
		var ray := _ray(eye, forward, AIM_DISTANCE)
		_impact(hand, spell, info, ray["ref"], ray["point"])
		return
	var type := int(proj["type"])
	var reach := float(proj["range"]) * SkydotWorld.unit_scale()
	if reach <= 0.0:
		reach = AIM_DISTANCE
	match type:
		PROJ_BEAM:
			_warn.once("beam%d" % type, "beam projectiles are a ray without a model (0x%08X)" % int(proj["id"]))
			var ray := _ray(eye, forward, reach)
			_impact(hand, spell, info, ray["ref"], ray["point"])
		PROJ_CONE:
			_warn.once("cone%d" % type, "cone projectiles are a cone test without a model (0x%08X)" % int(proj["id"]))
			var angle: float = float(proj.get("cone_spread", 0.0))
			var hit := _combat.find_in_cone(PLAYER_REF, eye, forward, reach, angle if angle > 1.0 else CONE_ANGLE_FALLBACK)
			var ahead := eye + forward * minf(reach, 3.0)
			_impact(hand, spell, info, hit.get("ref", 0), hit.get("point", ahead))
		PROJ_MISSILE, PROJ_LOBBER, PROJ_FLAME:
			_shoot(hand, spell, info, proj, eye, forward, pointer, reach)
		_:
			_warn.once("projtype%d" % type, "projectile type 0x%X (barrier, arrow...) is flown as a missile" % type)
			_shoot(hand, spell, info, proj, eye, forward, pointer, reach)


func _shoot(hand: int, spell: int, info: Dictionary, proj: Dictionary, eye: Vector3, forward: Vector3,
		pointer: Node3D, reach: float) -> void:
	if _projectiles == null:
		return
	var right := pointer.global_transform.basis.x
	var side := -1.0 if hand == LEFT else (1.0 if hand == RIGHT else 0.0)
	var origin := eye + forward * SHOT_AHEAD + right * SHOT_SIDE * side - Vector3.UP * SHOT_BELOW
	var blocked := _host.get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(eye, origin, MASK))
	if not blocked.is_empty():
		origin = eye  # the hand is inside a wall: the shot leaves from the eyes
	var aim := _aim_point(eye, forward)
	var direction := (aim - origin).normalized()
	if direction.length() < 0.5:
		direction = forward
	var flame := int(proj["type"]) == PROJ_FLAME
	_projectiles.shoot_spell(origin, direction, proj,
		func(from: Vector3, to: Vector3) -> Dictionary: return _combat.segment_hit(PLAYER_REF, from, to),
		func(ref: int, point: Vector3) -> void: _impact(hand, spell, info, ref, point),
		reach, not flame)


## The first thing the crosshair's ray meets (an actor, else the world): {ref, point}; the point
## `distance` ahead when it meets nothing.
func _ray(eye: Vector3, forward: Vector3, distance: float) -> Dictionary:
	var far := eye + forward * distance
	var space := _host.get_world_3d().direct_space_state
	var query := PhysicsRayQueryParameters3D.create(eye, far, MASK)
	var hit := space.intersect_ray(query)
	var end: Vector3 = hit["position"] if not hit.is_empty() else far
	var body := _combat.segment_hit(PLAYER_REF, eye, end)
	if not body.is_empty():
		return {"ref": body["ref"], "point": body["point"]}
	return {"ref": 0, "point": end}


## Where the crosshair points: the world, else a long way ahead.
func _aim_point(eye: Vector3, forward: Vector3) -> Vector3:
	return _ray(eye, forward, AIM_DISTANCE)["point"]


## The spell arrived at `point`, having entered the actor `ref` (0: the world, or nothing).
func _impact(hand: int, spell: int, info: Dictionary, ref: int, point: Vector3) -> void:
	var area := float(info.get("area_units", 0.0)) * SkydotWorld.unit_scale()
	var targets: Array[int] = []
	if ref != 0:
		targets.append(ref)
	if area > 0.0:
		for body in _combat.actors_within(PLAYER_REF, point, area):
			if not targets.has(body["ref"]):
				targets.append(body["ref"])
	for i in targets.size():
		_land(hand, spell, info, targets[i], point, i == 0)
	_show_impact(info, point)


## Land the effects on `target` (PLAYER_REF: the caster). `consume`: a scroll is read and used up
## by this landing (the first of an area's), else only landed.
func _land(hand: int, spell: int, info: Dictionary, target: int, point: Vector3, consume := true) -> void:
	var result: Dictionary
	var at := 0 if target == PLAYER_REF else target
	if consume and bool(info.get("is_scroll", false)):
		result = _papyrus.cast_scroll(PLAYER_REF, spell, at)
		if result["ok"]:
			scroll_used.emit(spell)
	else:
		result = _papyrus.land_spell(PLAYER_REF, spell, at)
	if not result["ok"]:
		_warn.once("landfail%d_%s" % [spell, result["reason"]], "0x%08X does not land: %s" % [spell, result["reason"]])
		return
	spell_landed.emit(hand, spell, target, point, bool(info.get("hostile", false)))


# ---- effects of the spell in the scene

## The explosion model of the spell's effect (else of its PROJ) and its hit art at `point`.
func _show_impact(info: Dictionary, point: Vector3) -> void:
	var explosion := int(info.get("explosion", 0))
	if explosion == 0:
		explosion = int(_world.get_projectile(int(info.get("projectile", 0))).get("explosion", 0))
	if explosion != 0:
		var data := _world.get_explosion(explosion)
		var model: String = data.get("model", "")
		if model != "":
			_spawn(model, point)
		else:
			_warn.once("noexpl%d" % explosion, "explosion 0x%08X has no model" % explosion)
		if float(data.get("damage", 0.0)) > 0.0 or float(data.get("force", 0.0)) > 0.0:
			_warn.once("explphys", "explosion damage and force are not applied (the spell's effects are)")
	var hit_art := int(info.get("hit_art", 0))
	if hit_art != 0:
		var art := _world.get_art_object(hit_art)
		if art.get("model", "") != "":
			_spawn(art["model"], point)


func _spawn(model_path: String, point: Vector3) -> void:
	var node := _world.build_model(model_path)
	if node == null:
		_warn.once("model" + model_path, "%s cannot be built" % model_path)
		return
	_host.add_child(node)
	node.global_position = point
	_effects.append({"node": node, "left": EFFECT_SECONDS})


# ---- hand art

## The casting art of the spell in each hand, shown while the hands are ready.
func _refresh_art() -> void:
	for hand in [LEFT, RIGHT]:
		var path := ""
		var spell := spell_in(hand)
		if spell != 0 and _ready != Ready.SHEATHED:
			var art := int(info_of(spell).get("casting_art", 0))
			if art != 0:
				path = String(_world.get_art_object(art).get("model", ""))
				if path == "":
					_warn.once("noart%d" % art, "casting art 0x%08X has no model" % art)
		if path != _art[hand]:
			_art[hand] = path
			hand_art_changed.emit(hand, path)


## The casting art models now on the hands: {LEFT: path, RIGHT: path}, "" for none.
func hand_art() -> Dictionary:
	return _art.duplicate()


# ---- clips

## The first-person clip for `what` (equip, unequip, readyloop, chargeloop, concentration,
## release, selfrelease) of the hands `hands`, "" when the pack has none. The aimed forms are
## tried before the self forms for a spell with another delivery.
func _hand_clip(what: String, hands: Array, info := {}) -> String:
	var both := hands.size() > 1
	var self_cast := int(info.get("delivery", DELIVER_AIMED)) == DELIVER_SELF
	var hand: int = hands[hands.size() - 1] if not hands.is_empty() else RIGHT
	var prefix := "mlh" if hand == LEFT else "mrh"
	var names: Array[String] = []
	match what:
		"equip", "unequip":
			if both:
				names.append("mrh_and_mlh_" + what)
			names.append(prefix + "_" + what)
		"readyloop":
			names.append(prefix + "_readyloop")
			names.append(prefix + "_idle")
		"chargeloop":
			if self_cast:
				names.append(prefix + "_selfchargeloop")
			names.append(prefix + "_chargeloop")
			names.append("mrh_chargeloop")
		"concentration":
			if self_cast:
				names.append(prefix + "_selfconcentration")
			names.append(prefix + "_aimedconcentration")
			names.append(prefix + "_aimedcon")
			names.append(prefix + "_chargeloop")
		"release":
			names.append(prefix + "_release")
		"selfrelease":
			names.append(prefix + "_selfrelease")
			names.append(prefix + "_release")
	for stem in names:
		if _pack.has(FIRST_PERSON_CLIPS + stem + ".hkx"):
			return stem
	_warn.once("noclip" + what + prefix, "no first-person clip for %s of the %s hand" % [what, prefix])
	return ""


## A first-person clip's length in seconds (1 if it cannot be read).
func _length_of(stem: String) -> float:
	if _lengths.has(stem):
		return _lengths[stem]
	var length := 1.0
	var bytes := _pack.get_bytes(FIRST_PERSON_CLIPS + stem + ".hkx")
	if bytes.size() > 0:
		var info := SkydotAnimation.describe(bytes)
		if info.has("clips") and not info["clips"].is_empty():
			length = float(info["clips"][0]["duration"])
	_lengths[stem] = length
	return length
