# SPDX-License-Identifier: GPL-3.0-or-later
#
# The weapons an actor carries, shown on its third-person skeleton
# (GAMEPLAY-PLAN.md, 6g): the right-hand weapon of the VM's equipment on the
# `WEAPON` bone while drawn and on its sheath bone by type while sheathed
# (`WeaponSword`, `WeaponAxe`, `WeaponMace`, `WeaponDagger`, `WeaponBack` for
# two-handers, `WeaponBow`), the shield on `SHIELD`, and the bow the actor
# carries (SkydotPapyrus.ranged_weapon) on `WeaponBow` while it is not the
# weapon in hand. Bones are found in any case. A model hangs on a
# BoneAttachment3D of the actor's skeleton; the holder undoes the units node
# above the skeleton (the model brings its own), as the first-person body does.
# No input, no camera: shells see only the nodes.
extends RefCounted
class_name ActorGear

## The bone of a drawn weapon, a shield, a drawn bow.
const HAND_BONE := "WEAPON"
const SHIELD_BONE := "SHIELD"
## A drawn bow sits in the left hand: the shield's node (a guess; check by eye).
const BOW_DRAWN_BONE := SHIELD_BONE
## Sheath bone by WEAP animation type.
const SHEATH_BONES := {1: "WeaponSword", 2: "WeaponDagger", 3: "WeaponAxe", 4: "WeaponMace",
	5: "WeaponBack", 6: "WeaponBack", 7: "WeaponBow"}
const NODE_PREFIX := "Gear_"

var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _drawn := {}  # ref -> bool
var _warn := WarnOnce.new("gear: ")


func _init(world: SkydotWorld, papyrus: SkydotPapyrus) -> void:
	_world = world
	_papyrus = papyrus


## Whether `ref` shows its weapon in hand.
func is_drawn(ref: int) -> bool:
	return _drawn.get(ref, false)


## Show `ref`'s weapon in hand (true) or on its sheath (false).
func set_drawn(ref: int, drawn: bool) -> void:
	_drawn[ref] = drawn
	refresh(ref)


## A place was built: the actors in it carry their weapons.
func on_place_built(root: Node, _cell: int = 0) -> void:
	for node in root.find_children("*", "SkydotActor", true, false):
		if node.has_meta("skydot_ref"):
			refresh(node.get_meta("skydot_ref"))


## Build the weapon nodes of `ref` again from its equipment and drawn state.
## True if the actor has a skeleton to hang them on.
func refresh(ref: int) -> bool:
	var node := _world.find_ref_node(ref) as Node3D
	if node == null or not node.is_inside_tree():
		return false
	var skeleton := node.find_child("Skeleton", true, false) as Skeleton3D
	if skeleton == null:
		return false
	for child in skeleton.get_children():
		if String(child.name).begins_with(NODE_PREFIX):
			skeleton.remove_child(child)
			child.free()
	var drawn := is_drawn(ref)
	var equipment := _papyrus.equipment(ref)
	var right: int = equipment["right"]
	var right_info := _world.get_weapon(right) if right != 0 else {}
	var ranged := _papyrus.ranged_weapon(ref)
	var bow_info := _world.get_weapon(int(ranged.get("weapon", 0))) if not ranged.is_empty() else {}
	# The bow in hand is the weapon of an actor that has no other.
	var bow_in_hand := drawn and right_info.is_empty() and not bow_info.is_empty()
	if not right_info.is_empty():
		_attach(skeleton, "Weapon", right_info, HAND_BONE if drawn else _sheath(right_info))
	if not bow_info.is_empty():
		_attach(skeleton, "Bow", bow_info, BOW_DRAWN_BONE if bow_in_hand else SHEATH_BONES[7])
	var left: int = equipment["left"]
	var left_info := _world.get_base(left) if left != 0 else {}
	if left != 0 and not left_info.is_empty():
		_attach(skeleton, "Shield", {"model": left_info.get("model", ""), "id": left}, SHIELD_BONE)
	return true


static func _sheath(info: Dictionary) -> String:
	return SHEATH_BONES.get(int(info.get("weapon_type", 0)), "WeaponSword")


## The index of the bone named `name` in any case, -1 if there is none.
static func find_bone(skeleton: Skeleton3D, name: String) -> int:
	var wanted := name.to_lower()
	for i in skeleton.get_bone_count():
		if skeleton.get_bone_name(i).to_lower() == wanted:
			return i
	return -1


func _attach(skeleton: Skeleton3D, label: String, info: Dictionary, bone_name: String) -> void:
	var path: String = info.get("model", "")
	var bone := find_bone(skeleton, bone_name)
	if path == "" or bone < 0:
		_warn.once("bone%s%s" % [label, bone_name], "%s: no model or no bone %s on %s" % [label, bone_name, skeleton.get_parent().get_parent().name])
		return
	var model := _world.build_model(path)
	if model == null:
		_warn.once("model" + path, "cannot build %s" % path)
		return
	var attachment := BoneAttachment3D.new()
	attachment.name = NODE_PREFIX + label
	attachment.bone_name = skeleton.get_bone_name(bone)
	skeleton.add_child(attachment)
	var holder := Node3D.new()
	holder.transform = _units_transform().affine_inverse()
	attachment.add_child(holder)
	holder.add_child(model)
	for mesh in model.find_children("*", "MeshInstance3D", true, false):
		(mesh as MeshInstance3D).cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON


## What the skeleton's parent does: z-up game units to metres and y-up.
static func _units_transform() -> Transform3D:
	return Transform3D(Basis(Vector3.RIGHT, -PI / 2).scaled(Vector3.ONE * SkydotWorld.unit_scale()), Vector3.ZERO)
