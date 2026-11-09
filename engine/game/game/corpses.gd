# SPDX-License-Identifier: GPL-3.0-or-later
#
# What an actor's death looks like (GAMEPLAY-PLAN.md, 6d; SYSTEMS.md A7). No
# camera, input or overlay. When `SkydotPapyrus` says an actor died (or a
# place is built with a dead actor in it), its node stops (`SkydotActor.
# set_dead`: no AI, no walking, no animation, no collision) and the game's
# ragdoll takes its skeleton (`Ragdoll`: the bodies and constraints of the race's
# skeleton model, one builder for every race). A melee kill starts it from the
# pose with no push; a ranged or spell kill pushes it the way PushActorAway does,
# with the force of the fDeathForce* GMSTs for the hit's damage and no lift. When
# the body rests (or after MAX_SECONDS) the ragdoll is frozen into the bones'
# poses and freed, and the corpse place is stored (`SkydotPapyrus.set_corpse_place`:
# space, position in Skyrim units, facing), which saves keep: a dead actor built
# later (the cell entered again, a load) is placed there and lies down the same
# way, without a push. A skeleton model without a ragdoll logs `death: no
# ragdoll for <skeleton>` and lies on its side.
#
# A living actor can be knocked down too (`knock_down`: a shout's push, PushActorAway): the same
# ragdoll, its bodies set moving as the game does (a velocity, not an impulse: PUSH_*), but it does
# not die, is not a corpse (no corpse place, not pickable) and gets up GET_UP_DELAY after it rests:
# it stands where its root body lay, collision, animation and AI back (`push: 0x… gets up`). While
# it is down its node is "dead" for SkydotActor (nothing moves it) and the AI is held
# (`SkydotAi.hold`, which also ends a furniture use). A race with the "no knockdowns" flag is not
# knocked down. If it dies meanwhile, the ragdoll goes on as a corpse's. Paralysis (every actor
# whose paralysis takes effect) lies down the same way, with no push, until it ends.
#
# The corpse stays pickable for activation (Search): `pick_ref` finds a node
# by the `skydot_activatable` and `skydot_bounds` metadata, which this sets on
# the actor's node (its cylinder is not on a physics layer any more).
class_name Corpses
extends RefCounted

## Writes the simulated bodies' poses into their bones without the skeleton's scale (see `_lay_down`).
class RagdollPose:
	extends SkeletonModifier3D
	var bones: Array[PhysicalBone3D] = []

	func _process_modification_with_delta(_delta: float) -> void:
		var skeleton := get_skeleton()
		if skeleton == null:
			return
		for bone in bones:  # parents come before their children
			if is_instance_valid(bone) and bone.is_inside_tree():
				var index := skeleton.find_bone(bone.bone_name)
				if index >= 0:
					skeleton.set_bone_global_pose(index, Corpses.bone_pose(skeleton, bone))


## Keeps the animation's global poses of the bones with a body before the ragdoll's simulator writes
## over them (it is the skeleton's first modifier): the targets of the rigid-body controller.
class AnimatedPose:
	extends SkeletonModifier3D
	var indices: Array[int] = []
	var poses := {}  # bone index -> global pose (skeleton space)

	func _process_modification_with_delta(_delta: float) -> void:
		var skeleton := get_skeleton()
		if skeleton == null:
			return
		for index in indices:
			poses[index] = skeleton.get_bone_global_pose(index)


## Frozen when every body is slower than REST_SPEED (m/s) for REST_SECONDS (PLACEHOLDER: the game's
## rest test is not known; its ragdolls settled in 2 s (human) to 18 s (a dragon's limbs)), or after
## MAX_SECONDS (PLACEHOLDER).
const REST_SPEED := 0.1
const REST_SECONDS := 0.5
const MAX_SECONDS := 20.0
## PushActorAway, measured in game (skycompare-data/game-ragdoll, SUMMARY c: human, wolf, draugr and
## giant at force 5, 15 and 30 on flat ground): the game sets the ragdoll moving, whatever its mass,
## at PUSH_SPEED_PER_FORCE m/s per unit of force from the pusher to the target, PUSH_LIFT_PER_FORCE
## m/s up, turning at PUSH_SPIN_PER_FORCE rad/s per unit about the horizontal axis that tips it over
## (a fit of the bodies' first velocities, human F5/F15/F30: 0.37, 0.30, 0.26). Measured constants:
## no GMST matches them (fKnockbackDamageMult 5, fKnockbackForceMax 128, fKnockbackTime 0.3 are the
## stagger's; fGameplayImpulseMultBiped 0.04 and fProjectileKnockMultBiped 0.25 scale impulses of hits
## and projectiles, not PushActorAway's velocity; Skyrim.esm sets no fPushActorAway* GMST).
const PUSH_SPEED_PER_FORCE := 0.70
const PUSH_LIFT_PER_FORCE := 0.25
const PUSH_SPIN_PER_FORCE := 0.30
## A knocked-down actor gets up this long after its ragdoll rests (measured: get_up 0.3-0.6 s after
## the game's "queued", which comes at rest).
const GET_UP_DELAY := 0.3
## The killing hit's force: the GMSTs and their defaults (Skyrim.esm). Melee kills push nothing
## (measured); ranged kills use the ranged curve, spell kills the melee curve times
## fDeathForceSpellImpactMult. The game pushed 3 of 7 spell kills (cause not found): every one is
## pushed here (an open gap).
const DEATH_FORCE := {"fDeathForceDamageMin": 1.0, "fDeathForceDamageMax": 40.0, "fDeathForceForceMin": 4.0,
	"fDeathForceForceMax": 12.0, "fDeathForceRangedDamageMin": 1.0, "fDeathForceRangedDamageMax": 50.0,
	"fDeathForceRangedForceMin": 10.0, "fDeathForceRangedForceMax": 30.0, "fDeathForceSpellImpactMult": 2.0}
## RACE DATA flag "No Knockdowns" (xEdit's RACE definition): PushActorAway does not knock it down.
const RACE_NO_KNOCKDOWNS := 0x400000
## The magic effect archetype of a value modifier (a spell's damage).
const ARCH_VALUE_MODIFIER := 0
## Paralysis is looked at this often, seconds.
const PARALYSIS_POLL := 0.2
## A dead actor built in a place waits this long for the ground under it.
const GROUND_SECONDS := 10.0
## A frozen body's pickable box is its bones' box grown by this (metres).
const BOUNDS_MARGIN := 0.3
## The ground probes start this high above the point (metres) and reach this far down: a settled
## ragdoll, a quick "is there ground" test, and the floor under a point.
const PROBE_UP := 0.5
const PROBE_DOWN_SETTLE := 3.0
const PROBE_DOWN_GROUND := 2.0
const PROBE_DOWN_FAR := 200.0
## A horizontal distance (metres) below which a direction is none: a hit from straight above.
const HIT_MIN_DISTANCE := 0.001
## A pose coordinate beyond this (metres) is a broken skeleton and is ignored.
const MAX_COORDINATE := 1.0e4
## A body with no bones to measure is picked by this box (metres): a person standing.
const FALLBACK_BOUNDS := AABB(Vector3(-0.4, 0.0, -0.4), Vector3(0.8, 1.8, 0.8))
## The AI holds a dead or paralysed actor for this long (seconds): "until it stands again".
const HOLD_INDEFINITELY := 3600.0

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _host: Node3D
var actors: ActorRegistry
var _player_position: Callable  # () -> Vector3: where the player is, for the direction of its hits
var _warn := WarnOnce.new("death: ")
var _hits := {}  # target ref -> {from: where it came from, force: the death force it gives}
var _pending := {}  # ref -> seconds waited: dead, not yet lying down
var _live := {}  # ref -> true: died while built (not a body built dead): its death plays
var _bodies: Array[Dictionary] = []  # {ref, node, skeleton, ragdoll, bones, time, still, knocked, rested}
var _paralysed := {}  # ref -> true: the actors whose paralysis has been seen
var _paralysis_timer := 0.0
var _death_force := {}  # GMST -> value
## The AI to hold while an actor is down (the session sets it; null without one).
var ai: SkydotAi


func _init(world: SkydotWorld, papyrus: SkydotPapyrus, host: Node3D, player_position: Callable, actor_registry: ActorRegistry) -> void:
	actors = actor_registry
	_player_position = player_position
	_world = world
	_papyrus = papyrus
	_host = host
	for name: String in DEATH_FORCE:
		_death_force[name] = _world.get_gmst_float(name, DEATH_FORCE[name])
	_papyrus.magic_effect_event.connect(_on_magic_effect)


## A hit landed (Combat.hit_landed): a ranged hit gives a death its force (the fDeathForceRanged*
## curve of its damage) and direction; a melee hit none (measured: a melee kill does not push).
func on_hit(target: int, result: Dictionary, _point: Vector3) -> void:
	var ranged := int(result.get("weapon_type", 0)) in [7, 9] or int(result.get("ammo", 0)) != 0
	if not ranged:
		_hits.erase(target)
		return
	_note_hit(target, int(result.get("attacker", 0)), death_force(float(result.get("damage", 0.0)), true))


## A spell's damage landed (a value modifier with a caster): the death it causes is pushed by the
## melee curve of its magnitude times fDeathForceSpellImpactMult.
func _on_magic_effect(info: Dictionary) -> void:
	var caster := int(info.get("caster", 0))
	var target := int(info.get("target", 0))
	if not bool(info.get("started", false)) or int(info.get("archetype", -1)) != ARCH_VALUE_MODIFIER or caster == 0 or caster == target:
		return
	_note_hit(target, caster, death_force(float(info.get("magnitude", 0.0)), false) * float(_death_force["fDeathForceSpellImpactMult"]))


func _note_hit(target: int, attacker: int, force: float) -> void:
	var from := _player_position.call() as Vector3
	if attacker != SkydotPapyrus.PLAYER_REF:
		var node := _world.find_ref_node(attacker) as Node3D
		if node == null:
			return
		from = node.global_position
	_hits[target] = {"from": from, "force": force}


## The force a killing hit of `damage` gives (the fDeathForce* GMSTs: from ForceMin at DamageMin to
## ForceMax at DamageMax, held at the ends), `ranged` or melee.
func death_force(damage: float, ranged: bool) -> float:
	var key := "fDeathForceRanged" if ranged else "fDeathForce"
	var low := float(_death_force[key + "DamageMin"])
	var high := float(_death_force[key + "DamageMax"])
	var share := clampf((damage - low) / (high - low), 0.0, 1.0) if high > low else 1.0
	return lerpf(float(_death_force[key + "ForceMin"]), float(_death_force[key + "ForceMax"]), share)


## An actor died: its node lies down with the push of the last hit (if it gives one), from the
## next `step` (the hit that killed it is reported after the death).
func on_died(ref: int) -> void:
	for body in _bodies:
		if body["ref"] == ref and body["knocked"]:
			body["knocked"] = false  # it dies where it lies: its ragdoll goes on as a corpse's
			body["time"] = 0.0
			return
	_live[ref] = true
	_queue(ref)


## A place was built: the dead in it lie where their corpse place says, with
## no push, once there is ground under them.
func on_place_built(root: Node, _cell: int = 0) -> void:
	for node in root.find_children("*", "SkydotActor", true, false):
		if node.has_meta("skydot_ref"):
			var ref: int = node.get_meta("skydot_ref")
			if _papyrus.is_actor_dead(ref):
				_queue(ref)


func on_actor_arrived(ref: int) -> void:
	if _papyrus.is_actor_dead(ref):
		_queue(ref)


func _queue(ref: int) -> void:
	if not _pending.has(ref):
		_pending[ref] = 0.0


## Reanimate: the body gets up. Its ragdoll (running or frozen into the bones' poses) is
## taken away, the skeleton goes back to its rest pose, it is no longer pickable as a corpse and
## the actor node lives again (`SkydotActor.set_dead(false)`); the AI is told by
## `SkydotAi.revive`. False if the actor is not built.
func stand_up(ref: int) -> bool:
	_pending.erase(ref)
	_hits.erase(ref)
	for body in _bodies.duplicate():
		if body["ref"] == ref:
			(body["ragdoll"] as Ragdoll).stop()
			_bodies.erase(body)
	var node := _world.find_ref_node(ref) as SkydotActor
	if node == null or not node.is_inside_tree():
		return false
	for skeleton: Skeleton3D in node.find_children("*", "Skeleton3D", true, false):
		skeleton.reset_bone_poses()
	node.rotation.x = 0.0
	node.rotation.z = 0.0
	node.set_meta("skydot_activatable", false)
	node.remove_meta("skydot_bounds")
	node.set_dead(false)
	print("death: 0x%08X stands up" % ref)
	return true


# ---- knocked down

## `ref`, a living actor, is thrown away from `source` (an actor, or the player) with `force` (the
## PushActorAway force): it falls as a ragdoll and gets up later. False (and nothing happens) if it
## is dead, not built, already down, has no ragdoll, or its race has "no knockdowns" (not for
## `hold`). `hold`: it stays down until `release` (paralysis; `force` 0).
func knock_down(ref: int, source: int, force: float, hold := false) -> bool:
	if _papyrus.is_actor_dead(ref) or is_down(ref):
		return false
	var node := _world.find_ref_node(ref) as Node3D
	if node == null or not node.is_inside_tree():
		return false
	if not hold and int(_plan(ref).get("race_flags", 0)) & RACE_NO_KNOCKDOWNS != 0:
		return false
	var from: Vector3 = _player_position.call()
	if source != SkydotPapyrus.PLAYER_REF:
		var origin := _world.find_ref_node(source) as Node3D
		if origin != null:
			from = origin.global_position
	if not _lay_down(ref, {"from": from, "force": force, "lift": true}, true):
		return false
	if ai != null:
		ai.hold(ref, HOLD_INDEFINITELY)  # until it stands again
	for body in _bodies:
		if body["ref"] == ref:
			body["hold"] = hold
	print("push: 0x%08X pushed by 0x%08X (force %s)" % [ref, source, force])
	return true


## How a push of `force` along `away` (horizontal, unit) sets a ragdoll moving: {linear, spin}, the
## game's measured law (PUSH_*), the same for the player's pushes and the NPCs'.
static func push_motion(away: Vector3, force: float, lift: bool) -> Dictionary:
	var linear := away * PUSH_SPEED_PER_FORCE * force
	if lift:
		linear += Vector3.UP * PUSH_LIFT_PER_FORCE * force
	# The body tips over away from the pusher: about the horizontal axis across the push.
	var spin := Vector3.UP.cross(away) * PUSH_SPIN_PER_FORCE * force
	return {"linear": linear, "spin": spin}


## Paralysis (the effect's overrides, polled in `step`): the actor goes limp as a ragdoll with no push (the game's
## human: limp 0.14 s after the cast) and lies until the effect ends, then gets up as a knocked-down actor does.
## Every race the same (a dragon is not paralysed: the effect's conditions, GetIsRace, keep it off; the game
## freezes one paralysed by force, from data not found: SkyDot lets it go limp). The green shell of the effect
## (its EFSH) is not shown unless the visual effects show it.
func _poll_paralysis(delta: float) -> void:
	_paralysis_timer += delta
	if _paralysis_timer < PARALYSIS_POLL:
		return
	_paralysis_timer = 0.0
	var seen := {}
	for node in actors.all():
		if not node.has_meta("skydot_ref"):
			continue
		var ref: int = node.get_meta("skydot_ref")
		seen[ref] = true
		var on: bool = bool(_papyrus.magic_overrides(ref).get("paralyzed", false))
		if on == _paralysed.has(ref):
			continue
		if on:
			_paralysed[ref] = true
			knock_down(ref, 0, 0.0, true)
		else:
			_paralysed.erase(ref)
			for body in _bodies:
				if body["ref"] == ref:
					body["hold"] = false  # it gets up with the next step
	# An actor that is no longer built (its cell streamed out) is no longer marked: a rebuilt one stands.
	for ref: int in _paralysed.keys():
		if not seen.has(ref):
			_paralysed.erase(ref)


## A save was loaded or the game restarted: nothing of the game before it stays. The actor nodes belong to the
## place, which frees them; only the entries go. Silent.
func reset() -> void:
	_hits.clear()
	_pending.clear()
	_live.clear()
	_bodies.clear()
	_paralysed.clear()
	_paralysis_timer = 0.0


## The place was left: its actors are freed with it, so the bodies and the marks of paralysis go. Silent.
func on_place_left() -> void:
	reset()


## Whether `ref` lies knocked down (alive).
func is_down(ref: int) -> bool:
	for body in _bodies:
		if body["ref"] == ref and body["knocked"]:
			return true
	return false


## The knocked-down body stands where its root body lay: the ragdoll goes, the bones are at rest, the
## node is alive for SkydotActor and the AI again.
func _get_up(body: Dictionary) -> void:
	var ref: int = body["ref"]
	var node: SkydotActor = body["node"]
	var skeleton: Skeleton3D = body["skeleton"]
	var ragdoll: Ragdoll = body["ragdoll"]
	var at := node.global_position
	var root := _root_bone(ragdoll)
	if root != null:
		at = skeleton.global_transform * bone_pose(skeleton, root).origin
	ragdoll.stop()
	skeleton.reset_bone_poses()
	var query := PhysicsRayQueryParameters3D.create(at + Vector3.UP * PROBE_UP, at + Vector3.DOWN * PROBE_DOWN_SETTLE,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
	var hit := _host.get_world_3d().direct_space_state.intersect_ray(query)
	node.global_position = hit["position"] if not hit.is_empty() else Vector3(at.x, node.global_position.y, at.z)
	node.rotation.x = 0.0
	node.rotation.z = 0.0
	node.set_dead(false)
	if ai != null:
		ai.hold(ref, 0.0)
	print("push: 0x%08X gets up" % ref)


## The ragdoll's first body on a bone (the data's root, the centre of mass body of a skeleton).
static func _root_bone(ragdoll: Ragdoll) -> PhysicalBone3D:
	for bone in ragdoll.bones:
		if bone != null:
			return bone
	return null


## Number of ragdolls still simulating.
func simulating() -> int:
	return _bodies.size()


## The frame: freeze the ragdolls that rest or have run their time.
func step(delta: float) -> void:
	_poll_paralysis(delta)
	_start_pending(delta)
	for body in _bodies.duplicate():
		if not is_instance_valid(body["node"]) or not is_instance_valid(body["skeleton"]):
			_bodies.erase(body)
			continue
		body["time"] += delta
		if body.has("animated"):
			# Driven by its clip: not at rest until the clip has ended.
			(body["ragdoll"] as Ragdoll).drive((body["animated"] as AnimatedPose).poses, delta)
			body["still"] = 0.0
			if body["time"] < MAX_SECONDS:
				continue
			_end_drive(body)
		var moving := false
		for bone: PhysicalBone3D in body["bones"]:
			if bone.linear_velocity.length() > REST_SPEED:
				moving = true
		body["still"] = 0.0 if moving else body["still"] + delta
		if body["knocked"]:
			if body.get("hold", false):
				continue
			# At rest it is "queued"; it gets up GET_UP_DELAY later (or after MAX_SECONDS).
			if body["still"] >= REST_SECONDS + GET_UP_DELAY or body["time"] >= MAX_SECONDS:
				_get_up(body)
				_bodies.erase(body)
			continue
		if body["still"] >= REST_SECONDS or body["time"] >= MAX_SECONDS:
			_freeze(body)
			_bodies.erase(body)


# ---- lying down

## Start the ragdolls of the queued dead whose ground is there (or after
## GROUND_SECONDS: a cell whose collision never comes).
func _start_pending(delta: float) -> void:
	for ref: int in _pending.keys():
		_pending[ref] += delta
		var node := _world.find_ref_node(ref) as Node3D
		if node == null or not node.is_inside_tree():
			if _pending[ref] > GROUND_SECONDS:
				_pending.erase(ref)
			continue
		# A race with a death clip does not wait for ground: it may die in the air and falls first.
		var posed := node is SkydotActor and (node as SkydotActor).has_death_clip()
		if not posed and _pending[ref] < GROUND_SECONDS and not _has_ground(node.global_position):
			continue
		_pending.erase(ref)
		_lay_down(ref, _hits.get(ref, {}) if _live.has(ref) else {})
		_hits.erase(ref)


func _has_ground(at: Vector3) -> bool:
	var space := _host.get_world_3d().direct_space_state
	var query := PhysicsRayQueryParameters3D.create(at + Vector3.UP * PROBE_UP, at + Vector3.DOWN * PROBE_DOWN_GROUND,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
	return not space.intersect_ray(query).is_empty()


## `push` ({from, force, lift}, or empty): the push the ragdoll starts with (push_motion).
func _lay_down(ref: int, push: Dictionary, knocked := false) -> bool:
	var node := _world.find_ref_node(ref) as SkydotActor
	if node == null or not node.is_inside_tree() or node.is_dead():
		return false
	if not knocked and node.has_death_clip():
		return _pose_death(ref, node)
	if ai != null:
		ai.hold(ref, HOLD_INDEFINITELY)  # out of its furniture before the ragdoll takes the body
	node.set_dead(true)
	return _ragdoll_body(ref, node, push, knocked, false)


## The game's ragdoll of `node`'s skeleton from the pose it has (`posed`: it plays or holds a death
## clip; the animation is let go of and a body without a ragdoll stays as it is).
func _ragdoll_body(ref: int, node: SkydotActor, push: Dictionary, knocked: bool, posed: bool, controls := []) -> bool:
	var found := node.find_children("*", "Skeleton3D", true, false)
	var skeleton: Skeleton3D = found.front() if not found.is_empty() else null
	if posed and controls.is_empty():
		var anim := node.get_node_or_null("AnimationPlayer") as AnimationPlayer
		if anim != null:
			anim.active = false  # the skeleton keeps the pose; the simulation takes it from there
	var ragdoll := Ragdoll.make(skeleton, _ragdoll_data(ref))
	if ragdoll == null:
		var name := String(_plan(ref).get("skeleton_model", "no skeleton"))
		_warn.once(name, "no ragdoll for %s" % name)
		if knocked:
			node.set_dead(false)  # no body to throw: it only staggers
			if ai != null:
				ai.hold(ref, 0.0)
			return false
		if not posed:
			node.rotate_object_local(Vector3.BACK, PI / 2.0)  # on its side
		_finish(ref, node, skeleton)
		return true
	ragdoll.start()
	# Godot's simulator writes a body's pose into the bone through the skeleton's inverse transform,
	# scale included: under the actor's units node (about 0.0147) a bone's basis comes out 68 times
	# too large and the skin explodes into huge planes. This modifier runs after it (it is the
	# skeleton's next child) and writes the right pose (`bone_pose`: no scale) over it; it goes when
	# the simulator does.
	var fix := RagdollPose.new()
	for bone in ragdoll.bones:
		if bone != null:
			fix.bones.append(bone)
	skeleton.add_child(fix)
	ragdoll.simulator.tree_exiting.connect(func() -> void:
		fix.active = false
		fix.queue_free())
	if not push.is_empty() and float(push.get("force", 0.0)) > 0.0:
		var away: Vector3 = node.global_position - Vector3(push["from"])
		away.y = 0.0
		away = away.normalized() if away.length() >= HIT_MIN_DISTANCE else -node.global_transform.basis.z
		var motion := push_motion(away, float(push["force"]), bool(push.get("lift", false)))
		ragdoll.set_motion(motion["linear"], motion["spin"])
	if not knocked:
		_set_place(ref, node, node.global_position)
	var bones: Array[PhysicalBone3D] = []
	for bone in ragdoll.bones:
		if bone != null:
			bones.append(bone)
	var entry := {"ref": ref, "node": node, "skeleton": skeleton, "ragdoll": ragdoll, "bones": bones,
		"time": 0.0, "still": 0.0, "knocked": knocked}
	if not controls.is_empty() and ragdoll.set_drive(controls):
		# The clip goes on posing the skeleton; the rigid-body controller drives the bodies toward it.
		var animated := AnimatedPose.new()
		for i in ragdoll.driven:
			animated.indices.append(skeleton.find_bone(ragdoll.bones[i].bone_name))
		skeleton.add_child(animated)
		skeleton.move_child(animated, 0)
		entry["animated"] = animated
		print("death: 0x%08X drives %d of %d bodies with the clip" % [ref, ragdoll.driven.size(), bones.size()])
	_bodies.append(entry)
	var skeleton_name := String(_plan(ref).get("skeleton_model", "?"))
	_warn.once("ragdoll" + skeleton_name, "ragdoll for %s (%d bodies, %d joints)" % [skeleton_name, bones.size(), ragdoll.joints.size()])
	if not knocked:
		_mark_pickable(node, skeleton)
	return true


## The ragdoll data of `ref`: a placed actor's, else a runtime one's (a summon) through its base.
func _ragdoll_data(ref: int) -> Dictionary:
	var data := _world.get_ragdoll(ref)
	if data.is_empty():
		var base := int(_papyrus.get_ref_base(ref))
		if base != 0:
			data = _world.get_ragdoll(ref, base)
	return data


## The actor plan of `ref` (SkydotWorld.get_actor_plan), a runtime actor's through its base.
func _plan(ref: int) -> Dictionary:
	var plan := _world.get_actor_plan(ref)
	if plan.is_empty():
		var base := int(_papyrus.get_ref_base(ref))
		if base != 0:
			plan = _world.get_npc_plan(base, ref)
	return plan


## A race with a death clip (the dragon's mtdeathintro) plays it once and holds the last frame; a flyer
## crashes first. A body built dead is posed at the end of the clip directly. If the graph's state that
## plays the clip drives the ragdoll (SkydotWorld.get_ragdoll_controls), the ragdoll takes the body when
## the clip starts and is driven toward its pose (Ragdoll.drive) until it ends; otherwise it takes the
## body from the held pose (`SkydotActor.death_pose_done`). Either way it then rests limp, freezes and is
## stored as any creature's.
func _pose_death(ref: int, node: SkydotActor) -> bool:
	var rebuilt := not _live.has(ref)
	_live.erase(ref)
	node.death_pose_done.connect(_on_death_pose_done.bind(ref, node), CONNECT_ONE_SHOT)
	# The behaviour graph's state that plays the clip may drive the ragdoll with it (the game's
	# AnimateToRagdoll states: hkbKeyframeBonesModifier + hkbRigidBodyRagdollControlsModifier).
	var controls := _world.get_ragdoll_controls(ref, node.get_death_clip())
	if controls.is_empty():
		var base := int(_papyrus.get_ref_base(ref))
		if base != 0:
			controls = _world.get_ragdoll_controls(ref, node.get_death_clip(), base)
	if not rebuilt and not controls.is_empty():
		node.death_clip_started.connect(_on_death_clip_started.bind(ref, node, controls), CONNECT_ONE_SHOT)
	if not rebuilt:
		print("death: 0x%08X plays %s%s" % [ref, node.get_death_clip().get_file(), " after a crash" if node.is_flying() else ""])
	if not node.start_death(rebuilt):
		node.death_pose_done.disconnect(_on_death_pose_done.bind(ref, node))
		if node.death_clip_started.is_connected(_on_death_clip_started.bind(ref, node, controls)):
			node.death_clip_started.disconnect(_on_death_clip_started.bind(ref, node, controls))
		return false
	return true


## The death clip starts: the ragdoll takes the body now, driven toward the clip's pose until it ends.
func _on_death_clip_started(ref: int, node: SkydotActor, controls: Array) -> void:
	if is_instance_valid(node) and node.is_inside_tree():
		_ragdoll_body(ref, node, {}, false, true, controls)


## The body of `ref` the ragdoll simulates, or empty.
func _body_of(ref: int) -> Dictionary:
	for body in _bodies:
		if body["ref"] == ref:
			return body
	return {}


func _on_death_pose_done(ref: int, node: SkydotActor) -> void:
	if not is_instance_valid(node) or not node.is_inside_tree():
		return
	var space := _host.get_world_3d().direct_space_state
	var query := PhysicsRayQueryParameters3D.create(node.global_position + Vector3.UP * PROBE_UP,
		node.global_position + Vector3.DOWN * PROBE_DOWN_FAR, SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
	var hit := space.intersect_ray(query)
	var above: float = node.global_position.y - float(hit["position"].y) if not hit.is_empty() else INF
	print("death: 0x%08X holds the death pose, %.2f m above the ground" % [ref, above])
	# The VM knows an NPC only where it was placed: tell it where the body lies (a crashed flyer is far
	# from where it flew; scripts measure distances to it).
	_papyrus.update_actor(ref, SkydotWorld.godot_to_skyrim(node.global_position))
	# The creature has finished dying when the clip is done (the game's dragon: OnDeath ~2 s after the
	# kill, stage 80), not when the body that then collapses has come to rest.
	_papyrus.finish_dying(ref)
	# The clip is only the intro: the body goes limp and rests as other creatures' do, without a push.
	# Driven by the clip (the graph's state has a rigid-body controller) it is already a ragdoll;
	# otherwise it collapses from the clip's last pose.
	var driven := _body_of(ref)
	if not driven.is_empty() and driven.has("animated"):
		_end_drive(driven)
		return
	_ragdoll_body(ref, node, {}, false, true)


## The drive of a body ends (its clip did, or the body freezes): limp, the animation let go of.
func _end_drive(body: Dictionary) -> void:
	(body["ragdoll"] as Ragdoll).end_drive()
	var animated: AnimatedPose = body.get("animated")
	body.erase("animated")
	if is_instance_valid(animated):
		animated.queue_free()
	var anim := (body["node"] as Node).get_node_or_null("AnimationPlayer") as AnimationPlayer
	if anim != null:
		anim.active = false


## Freeze the ragdoll into the bones' poses, free it, store the corpse place.
func _freeze(body: Dictionary) -> void:
	var skeleton: Skeleton3D = body["skeleton"]
	var node: Node3D = body["node"]
	var ragdoll: Ragdoll = body["ragdoll"]
	var poses := {}
	for bone: PhysicalBone3D in body["bones"]:
		poses[skeleton.find_bone(bone.bone_name)] = bone_pose(skeleton, bone)
	var root := _root_bone(ragdoll)
	var root_at := skeleton.global_transform * (poses[skeleton.find_bone(root.bone_name)] as Transform3D).origin
	ragdoll.stop()
	_settle_poses(skeleton, poses)
	_set_place(body["ref"], node, root_at)
	_mark_pickable(node, skeleton)
	_papyrus.finish_dying(body["ref"])  # a body with a death clip: OnDeath comes now


## Local poses against parents worked out here (the skeleton's cached global poses are stale while it
## simulates): a bone with a body in `poses` (index -> global pose) takes the simulated pose, any other
## follows its parent with the pose it had. Returns the global poses (index -> Transform3D).
func _settle_poses(skeleton: Skeleton3D, poses: Dictionary) -> Dictionary:
	var globals := {}
	for index in skeleton.get_bone_count():  # parents come before their children
		var parent := skeleton.get_bone_parent(index)
		var parent_global: Transform3D = globals[parent] if parent >= 0 else Transform3D.IDENTITY
		if poses.has(index):
			globals[index] = poses[index]
			skeleton.set_bone_pose(index, parent_global.affine_inverse() * poses[index])
		else:
			globals[index] = parent_global * skeleton.get_bone_pose(index)
	return globals


## Where the simulation has the bone, in the skeleton's space and units: the
## body's transform (no scale: Godot takes it off) put back on the bone.
static func bone_pose(skeleton: Skeleton3D, bone: PhysicalBone3D) -> Transform3D:
	var inverse := skeleton.global_transform.affine_inverse()
	var body := bone.global_transform
	var placed := Transform3D(inverse.basis.orthonormalized() * body.basis, inverse * body.origin)
	return placed * bone.body_offset.affine_inverse()


## A frozen or lying body: store where it is.
func _finish(ref: int, node: Node3D, skeleton: Skeleton3D) -> void:
	_papyrus.finish_dying(ref)
	_set_place(ref, node, node.global_position)
	_mark_pickable(node, skeleton)


## The floor under `at` (`at` itself where there is none within 3 m).
func _floor_under(at: Vector3) -> Vector3:
	var space := _host.get_world_3d().direct_space_state
	var query := PhysicsRayQueryParameters3D.create(at + Vector3.UP * PROBE_UP, at + Vector3.DOWN * PROBE_DOWN_SETTLE,
		SkydotPlayer.LAYER_WORLD | SkydotPlayer.LAYER_TERRAIN)
	var hit := space.intersect_ray(query)
	return hit["position"] if not hit.is_empty() else at


## The corpse place: the floor under `at`, in Skyrim units, and the facing.
## A dead actor built again stands there and falls the same way.
func _set_place(ref: int, node: Node3D, at: Vector3) -> void:
	var floor_at := _floor_under(at)
	var cell: int = _world.get_actor(ref).get("cell", 0)
	_papyrus.set_corpse_place(ref, _world.get_cell_space(cell), SkydotWorld.godot_to_skyrim(floor_at), node.rotation.y)


## The dead stay pickable: a box round the skeleton's bones in the node's own space.
func _mark_pickable(node: Node3D, skeleton: Skeleton3D) -> void:
	var bounds := AABB()
	var any := false
	if skeleton != null:
		var inverse := node.global_transform.affine_inverse()
		for i in skeleton.get_bone_count():
			var local := skeleton.get_bone_global_pose(i).origin
			if absf(local.x) > MAX_COORDINATE or absf(local.y) > MAX_COORDINATE or absf(local.z) > MAX_COORDINATE:
				continue  # a bone parked far away for a prop
			var at := inverse * (skeleton.global_transform * local)
			bounds = AABB(at, Vector3.ZERO) if not any else bounds.expand(at)
			any = true
	if not any:
		bounds = FALLBACK_BOUNDS
	node.set_meta("skydot_bounds", bounds.grow(BOUNDS_MARGIN))
	node.set_meta("skydot_activatable", true)
