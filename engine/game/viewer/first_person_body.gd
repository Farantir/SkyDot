# SPDX-License-Identifier: GPL-3.0-or-later
#
# The player's own arms and weapon in the first person (GAMEPLAY-PLAN.md, 6c),
# shared by both shells. A Node3D the shell adds under the camera's parent and
# calls `follow()` on every frame.
#
# Flat mode (the viewer): the first-person skeleton (`skeletonfirst.hkx`) in
# game units under a z-up node, as an actor's; the skin's first-person body
# and hands for the slots nothing worn covers and the worn armor's
# first-person models skinned onto it (the plan's `parts_1st`, from the
# ARMA's MOD4/MOD5); the weapon model (WEAP `first_person_model` if it has
# one) on the `Weapon` bone. The `Camera1st [Cam1]` bone is put at the camera
# (position only: the clips animate the arms relative to it) and the body
# turns with the camera, so it follows the pitch. The casting art of a readied hand's
# spell (Magic.hand_art_changed) is built on that hand's magic node
# (`NPC L MagicNode [LMag]`, `NPC R MagicNode [RMag]`), and Magic's clips (hands
# readied, charging, released) play over Combat's until it gives the arms back.
# Drawn as at ARMS_FOV whatever
# the camera's field of view (a view-space scale). It plays the clips Combat
# names (equip, idle, attacks) and is hidden while the weapon is sheathed, as
# in Skyrim's first person.
#
# Hand mode (the VR shell, ADR 0006): no arms; only the weapon model, in the
# controller node. `update_hand` sweeps the blade through Combat when its tip
# is faster than SWING_SPEED.
class_name FirstPersonBody
extends Node3D

const SKELETON := "meshes/actors/character/_1stperson/characterassets/skeletonfirst.hkx"
const CLIPS := "meshes/actors/character/_1stperson/animations/"
const UNITS_NODE := "bethconv_z_up_to_y_up"
const CAMERA_BONE := "Camera1st [Cam1]"
const WEAPON_BONE := "Weapon"
const MAGIC_BONES := {Magic.LEFT: "NPC L MagicNode [LMag]", Magic.RIGHT: "NPC R MagicNode [RMag]"}
const PLAYER_NPC := 7
const PLAYER_REF := 0x14
## Seconds a clip blends into the previous pose.
const BLEND := 0.1
## Placeholder: metres per second the blade's tip must move to count as a swing.
const SWING_SPEED := 2.5
## Placeholder: the blade's length from the hand, metres, for the sweep (VR).
const BLADE_LENGTH := 0.9
## Align the camera bone's orientation with the camera too (else position only).
const ALIGN_ORIENTATION := false
## Vertical field of view, degrees, the arms are drawn with whatever the
## camera's is, as Skyrim draws them with its own first-person FOV. Skyrim SE's
## default, measured with SkyrimRemote (vertical 50.53 at 1100x800, the same at
## 16:9: it keeps the height); its INI fDefault1stPersonFOV is not in the data.
const ARMS_FOV := 50.53

var _session: GameSession
var _camera: Node3D
var _hand: Node3D  # hand mode: the controller the weapon is held in
var _units: Node3D
var _skeleton: Skeleton3D
var _player: AnimationPlayer
var _library := AnimationLibrary.new()
var _cam_bone := -1
var _weapon_bone := -1
var _weapon_node: Node3D
var _shown := false
var _weapon_shown := false  # the weapon is out (Combat)
var _magic_shown := false  # the hands are readied (Magic)
var _left_hand: Node3D  # hand mode: the left controller, which carries the left hand's art
var _art_nodes := {}  # Magic.LEFT / RIGHT -> the casting art model's holder
var _last_tip := Vector3.ZERO
var _has_tip := false
var _warned := {}
var _built := false


## `camera` is what the body hangs in front of (flat mode); `hand`, if given,
## is the controller that holds the weapon (hand mode, no arms).
func _init(session: GameSession, camera: Node3D, hand: Node3D = null, left_hand: Node3D = null) -> void:
	_session = session
	_camera = camera
	_hand = hand
	_left_hand = left_hand
	name = "FirstPersonBody"
	visible = false
	_session.weapon_state_changed.connect(_on_state)
	_session.attack_started.connect(_on_attack)
	_session.items_changed.connect(rebuild)
	_session.magic.ready_changed.connect(_on_magic_ready)
	_session.magic.hand_clip.connect(_on_magic_clip)
	_session.magic.hand_art_changed.connect(_on_art)


func _ready() -> void:
	_apply_state(_session.weapon_state(), _session.combat.idle_clip())
	_magic_shown = _session.magic.is_ready()


## Build the skeleton, the parts and the weapon again (the equipment changed).
## Nothing is built until the weapon is first drawn: a sheathed body is hidden.
func rebuild() -> void:
	if not _built:
		return
	_build()


func _build() -> void:
	_built = true
	for child in get_children():
		remove_child(child)
		child.free()
	if _weapon_node != null and _hand != null:
		_weapon_node.free()
	_art_nodes.clear()
	_skeleton = null
	_player = null
	_units = null
	_weapon_node = null
	_cam_bone = -1
	_weapon_bone = -1
	_library = AnimationLibrary.new()
	if _hand == null:
		_build_arms()
	_build_weapon()
	if _player != null:
		_player.add_animation_library("", _library)
	_apply_state(_session.weapon_state(), _session.combat.idle_clip())
	var art: Dictionary = _session.magic.hand_art()
	for hand: int in art:
		_on_art(hand, art[hand])


## Every frame, after the camera moved: put the body so the camera bone is at
## the camera and the arms face the way it looks.
func follow() -> void:
	if _skeleton == null or _camera == null or _cam_bone < 0:
		return
	var rest := _skeleton.get_bone_global_rest(_cam_bone)
	var offset := _units.transform.basis * rest.origin
	var basis := _camera.global_transform.basis
	if ALIGN_ORIENTATION:
		# The camera bone's own axes against the camera's.
		var bone_basis := _units.transform.basis.orthonormalized() * rest.basis.orthonormalized()
		basis = basis * bone_basis.inverse()
	var placed := Transform3D(basis, _camera.global_position - basis * offset)
	global_transform = _arms_projection() * placed


## The view-space scale that makes the arms look as at ARMS_FOV under the
## camera's own field of view: x and y about the camera's axis by the ratio of
## the two tangents, depth unchanged (identity when the FOVs agree).
func _arms_projection() -> Transform3D:
	var camera := _camera as Camera3D
	if camera == null or camera.projection != Camera3D.PROJECTION_PERSPECTIVE:
		return Transform3D.IDENTITY
	var k := tan(deg_to_rad(camera.fov) / 2.0) / tan(deg_to_rad(ARMS_FOV) / 2.0)
	if camera.keep_aspect != Camera3D.KEEP_HEIGHT:
		# The fov is horizontal: compare at the viewport's aspect.
		var size := camera.get_viewport().get_visible_rect().size
		var aspect := size.x / maxf(size.y, 1.0)
		k = tan(deg_to_rad(camera.fov) / 2.0) / (tan(deg_to_rad(ARMS_FOV) / 2.0) * aspect)
	if absf(k - 1.0) < 0.001:
		return Transform3D.IDENTITY
	var view := camera.global_transform.orthonormalized()
	return view * Transform3D(Basis.from_scale(Vector3(k, k, 1.0)), Vector3.ZERO) * view.affine_inverse()


## Hand mode, every frame: the weapon follows the controller (it is its
## child); a swing faster than SWING_SPEED sweeps the blade into Combat.
func update_hand(delta: float) -> void:
	if _hand == null or delta <= 0.0:
		return
	if _session.weapon_state() != Combat.State.DRAWN:
		_has_tip = false
		return
	var base := _hand.global_position
	var tip := base - _hand.global_transform.basis.z * BLADE_LENGTH
	if _has_tip and tip.distance_to(_last_tip) / delta > SWING_SPEED:
		_session.combat.hit_segment(base, tip)
	_last_tip = tip
	_has_tip = true


# ---- building

func _build_arms() -> void:
	var pack := _session.pack
	var skeleton := SkydotAnimation.build_skeleton(pack.get_bytes(SKELETON))
	if skeleton == null:
		_warn_once("skeleton", "first person: the skeleton %s does not build" % SKELETON)
		return
	skeleton.name = "Skeleton"
	_units = Node3D.new()
	_units.name = UNITS_NODE
	_units.transform = _units_transform()
	add_child(_units)
	_units.add_child(skeleton)
	_skeleton = skeleton
	_cam_bone = skeleton.find_bone(CAMERA_BONE)
	_weapon_bone = skeleton.find_bone(WEAPON_BONE)
	if _cam_bone < 0:
		_warn_once("cam", "first person: the skeleton has no %s bone" % CAMERA_BONE)
	var plan := _session.world.get_npc_plan(PLAYER_NPC, PLAYER_REF)
	if not String(plan.get("missing", "")).is_empty():
		_warn_once("plan", "first person: no plan for the player: %s" % plan["missing"])
	var moved := 0
	for part: String in plan.get("parts_1st", PackedStringArray()):
		var model := _session.world.build_model(part)
		if model == null:
			_warn_once(part, "first person: cannot build %s" % part)
			continue
		add_child(model)
		moved += SkydotAnimation.attach_skinned(model, skeleton)
		remove_child(model)
		model.free()
	if moved == 0:
		_warn_once("parts", "first person: no body parts for the player (%s)" % [plan.get("parts_1st")])
	# The body skin takes the player's tone, as an actor's does.
	var tone: Color = plan.get("skin_tone", Color.WHITE)
	var headless := DisplayServer.get_name() == "headless"  # no shader parameters to read
	for node in skeleton.get_children():
		var mesh := node as MeshInstance3D
		if mesh == null or mesh.mesh == null:
			continue
		for surface in mesh.mesh.get_surface_count():
			var material := mesh.get_surface_override_material(surface) as ShaderMaterial
			if not headless and material != null and material.get_shader_parameter("use_skin_tint") == true:
				var own: ShaderMaterial = material.duplicate()
				own.set_shader_parameter("skin_tint", Vector3(tone.r, tone.g, tone.b))
				mesh.set_surface_override_material(surface, own)
		mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_player = AnimationPlayer.new()
	_player.name = "AnimationPlayer"
	add_child(_player)


## The weapon model on the Weapon bone (flat) or in the hand (VR).
func _build_weapon() -> void:
	var form := _session.player_weapon()
	if form == 0:
		return
	var info := _session.world.get_weapon(form)
	var path: String = info.get("first_person_model", "")
	if path.is_empty():
		path = info.get("model", "")
	if path.is_empty():
		return
	var model := _session.world.build_model(path)
	if model == null:
		_warn_once(path, "first person: cannot build the weapon %s" % path)
		return
	_set_shadows_off(model)
	if _hand != null:
		_weapon_node = Node3D.new()
		_weapon_node.name = "Weapon"
		_weapon_node.add_child(model)
		_hand.add_child(_weapon_node)
		_weapon_node.visible = _weapon_shown
		return
	if _skeleton == null or _weapon_bone < 0:
		model.free()
		return
	# The model carries its own z-up node; the bone is already in game units,
	# so the holder undoes this body's units node above it.
	var attachment := BoneAttachment3D.new()
	attachment.name = "WeaponAttachment"
	attachment.bone_name = WEAPON_BONE
	_skeleton.add_child(attachment)
	var holder := Node3D.new()
	holder.transform = _units_transform().affine_inverse()
	attachment.add_child(holder)
	holder.add_child(model)
	_weapon_node = attachment


static func _units_transform() -> Transform3D:
	return Transform3D(Basis(Vector3.RIGHT, -PI / 2).scaled(Vector3.ONE * SkydotWorld.unit_scale()), Vector3.ZERO)


static func _set_shadows_off(node: Node) -> void:
	for mesh in node.find_children("*", "MeshInstance3D", true, false):
		(mesh as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF


# ---- playing

func _on_state(state: int, clip: String) -> void:
	_apply_state(state, clip)


func _on_attack(clip: String, _power: bool) -> void:
	var speed: float = _session.combat.current_attack().get("speed", 1.0)
	_play(clip, false, speed)


func _apply_state(state: int, clip: String) -> void:
	_weapon_shown = state != Combat.State.SHEATHED
	_show()
	if clip.is_empty():
		# Nothing of Combat's to show: the readied hands' idle, else rest.
		var idle := _session.magic.idle_clip()
		if idle != "" and _built:
			_play(idle, true, 1.0)
		elif _player != null:
			_player.stop()
		return
	_play(clip, state == Combat.State.DRAWN, 1.0)


## Weapon or readied hands out: the arms show (and are built on the first time).
func _show() -> void:
	_shown = _weapon_shown or _magic_shown
	if _shown and not _built:
		_build()
	if _hand == null:
		visible = _shown
	elif _weapon_node != null:
		_weapon_node.visible = _weapon_shown


func _on_magic_ready(is_ready: bool) -> void:
	_magic_shown = is_ready
	_show()


## Magic's clip for the arms; "" gives them back to Combat (its idle, or the hands' idle).
func _on_magic_clip(clip: String, loops: bool, speed: float) -> void:
	if not _built:
		return
	if clip.is_empty():
		_apply_state(_session.weapon_state(), _session.combat.idle_clip())
		if _session.weapon_state() == Combat.State.SHEATHED and not _magic_shown and _player != null:
			_player.stop()
		return
	_play(clip, loops, speed)


## The casting art of the spell in `hand` (Magic.LEFT, RIGHT) on its magic node; "" takes it off.
func _on_art(hand: int, model_path: String) -> void:
	if _art_nodes.has(hand):
		var old: Node = _art_nodes[hand]
		if is_instance_valid(old):
			old.get_parent().remove_child(old)
			old.free()
		_art_nodes.erase(hand)
	if model_path.is_empty() or not _built:
		return  # built with the arms (rebuild asks Magic again)
	var model := _session.world.build_model(model_path)
	if model == null:
		_warn_once("art" + model_path, "first person: cannot build the casting art %s" % model_path)
		return
	_set_shadows_off(model)
	if _hand != null:
		var controller := _hand if hand == Magic.RIGHT else _left_hand
		if controller == null:
			model.free()
			return
		var holder := Node3D.new()
		holder.name = "CastingArt"
		holder.add_child(model)
		controller.add_child(holder)
		_art_nodes[hand] = holder
		return
	var bone: String = MAGIC_BONES[hand]
	if _skeleton == null or _skeleton.find_bone(bone) < 0:
		_warn_once("artbone" + bone, "first person: the skeleton has no %s bone for the casting art" % bone)
		model.free()
		return
	var attachment := BoneAttachment3D.new()
	attachment.name = "CastingArt%d" % hand
	attachment.bone_name = bone
	_skeleton.add_child(attachment)
	var holder := Node3D.new()
	holder.transform = _units_transform().affine_inverse()
	attachment.add_child(holder)
	holder.add_child(model)
	_art_nodes[hand] = attachment


## Play first-person clip `clip` (a name without path or extension); a loop for
## the idle.
func _play(clip: String, loops: bool, speed: float) -> void:
	if _player == null or _skeleton == null:
		return
	if not _library.has_animation(clip):
		var bytes := _session.pack.get_bytes(CLIPS + clip + ".hkx")
		if bytes.is_empty():
			_warn_once("clip" + clip, "first person: no clip %s" % clip)
			return
		var animation := SkydotAnimation.build_clip(bytes, _skeleton, "%s/Skeleton" % UNITS_NODE)
		if animation == null:
			_warn_once("clip" + clip, "first person: clip %s does not build" % clip)
			return
		animation.loop_mode = Animation.LOOP_LINEAR if loops else Animation.LOOP_NONE
		_library.add_animation(clip, animation)
	_player.speed_scale = speed
	_player.play(clip, BLEND)


func _warn_once(key: String, text: String) -> void:
	if not _warned.has(key):
		_warned[key] = true
		print(text)
