# SPDX-License-Identifier: GPL-3.0-or-later
#
# --spawn 0xNPC_[,0x...]: put those actors in front of the player after the load (for tests and
# shots), each facing him, side by side about DISTANCE metres ahead (--spawn-distance: another distance). The form is an NPC_ or a
# leveled NPC list (LVLN: resolved when it is placed, so it brings the spells and gear of the NPC it
# picks); they are placed through PlacedRefs like a summon, so the AI takes them up and they are as
# hostile as their data says (aggression, factions). Shared by the flat viewer and the VR shell.
class_name SpawnActors
extends RefCounted

const DISTANCE := 3.5
const SPACING := 1.2
const MASK := 1 | 2 | 8


## Place `forms` in front of the player; the references (0 for a form that is no actor).
static func spawn(session: GameSession, rig: PlayerRig, forms: Array[int], distance := DISTANCE) -> Array[int]:
	var refs: Array[int] = []
	var feet := rig.player.global_position
	var forward := -rig.camera.global_transform.basis.z
	forward.y = 0.0
	if forward.length() < 0.01:
		forward = Vector3.FORWARD
	forward = forward.normalized()
	var right := forward.cross(Vector3.UP).normalized()
	var space := rig.camera.get_world_3d().direct_space_state
	for i in forms.size():
		var side := (float(i) - (forms.size() - 1) / 2.0) * SPACING
		var at := feet + forward * distance + right * side
		var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(at + Vector3.UP * 2.0, at - Vector3.UP * 3.0, MASK))
		if not hit.is_empty():
			at = hit["position"]
		var here := SkydotWorld.godot_to_skyrim(at)
		var to := SkydotWorld.godot_to_skyrim(feet) - here
		var ref := session.papyrus.spawn_actor(forms[i], session.place.cell_id, here, atan2(to.x, to.y))
		print("--spawn: 0x%08X %s at Skyrim (%.0f, %.0f, %.0f), the player at (%.0f, %.0f, %.0f)" % [forms[i],
			"is 0x%08X" % ref if ref != 0 else "is no actor", here.x, here.y, here.z,
			SkydotWorld.godot_to_skyrim(feet).x, SkydotWorld.godot_to_skyrim(feet).y, SkydotWorld.godot_to_skyrim(feet).z])
		refs.append(ref)
	return refs
