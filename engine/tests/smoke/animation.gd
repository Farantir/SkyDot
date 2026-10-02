# SPDX-License-Identifier: GPL-3.0-or-later
#
# SkydotAnimation over the test pack's actor (converter/tools/testpack): a
# three-bone skeleton (root, spine at z 70, head 50 above it), a clip turning
# the head 90 degrees about Z over 10 frames with "FootLeft" at 0.1 s, and a
# cube skinned to the head, placed 120 units up. Needs the test pack.
extends SceneTree

const ACTOR := "meshes/testpack/actor/"

var failures := 0

func _init() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_animation: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_animation: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    _run(pack_dir)
    print("smoke_animation: failures=", failures)
    quit(failures)

func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)

func _run(pack_dir: String) -> void:
    var pack := SkydotPack.new()
    expect(pack.open(pack_dir) == OK, "the test pack opens: " + pack.get_error())
    expect(pack.get_kind(ACTOR + "skeleton.hkx") == "animation", "a .hkx is an animation asset")

    var skeleton_bytes := pack.get_bytes(ACTOR + "skeleton.hkx")
    var info: Dictionary = SkydotAnimation.describe(skeleton_bytes)
    expect(info.get("skeletons", []) == ["NPC Root [Root]"], "describe names the skeleton: %s" % info)
    expect(SkydotAnimation.describe(PackedByteArray([1, 2, 3])).has("error"), "garbage is described as an error")

    var skeleton: Skeleton3D = SkydotAnimation.build_skeleton(skeleton_bytes)
    if skeleton == null:
        expect(false, "the skeleton builds")
        return
    root.add_child(skeleton)
    skeleton.name = "Skeleton"
    expect(skeleton.get_bone_count() == 3, "three bones")
    var head := skeleton.find_bone("NPC Head [Head]")
    expect(head == 2 and skeleton.get_bone_parent(head) == 1, "the head hangs off the spine")
    expect(skeleton.get_bone_global_rest(head).origin.is_equal_approx(Vector3(0, 0, 120)), "the head rests at z 120")

    # The body: a cube skinned to the head, moved onto this skeleton.
    var body: Node = pack.load_scene(ACTOR + "body.nif").instantiate()
    root.add_child(body)
    expect(SkydotAnimation.attach_skinned(body, skeleton) == 1, "one skinned mesh moves onto the skeleton")
    expect(SkydotAnimation.get_last_missing_bones() == 0, "every bind names a bone of the skeleton")
    var meshes := skeleton.find_children("*", "MeshInstance3D", false, false)
    expect(meshes.size() == 1, "the mesh is now the skeleton's child")
    if meshes.size() == 1:
        var skin: Skin = (meshes[0] as MeshInstance3D).skin
        # At rest, mesh space lands where the NIF placed the shape: 120 up.
        var placed := skeleton.get_bone_global_rest(skeleton.find_bone(skin.get_bind_name(0))) * skin.get_bind_pose(0)
        expect(placed.origin.is_equal_approx(Vector3(0, 0, 120)), "the mesh sits 120 units up at rest: %s" % placed.origin)

    # The clip.
    var clip_bytes := pack.get_bytes(ACTOR + "turnhead.hkx")
    var anim: Animation = SkydotAnimation.build_clip(clip_bytes, skeleton, "Skeleton")
    if anim == null:
        expect(false, "the clip builds")
        return
    expect(is_equal_approx(anim.length, 10.0 / 30.0), "10 frames at 30 fps: %f" % anim.length)
    expect(anim.get_track_count() == 6, "position and rotation per bone: %d" % anim.get_track_count())
    expect(anim.has_marker("FootLeft") and is_equal_approx(anim.get_marker_time("FootLeft"), 0.1), "the annotation is a marker")
    expect(SkydotAnimation.build_clip(skeleton_bytes, skeleton, "Skeleton") == null, "a skeleton file has no clip")

    var player := AnimationPlayer.new()
    root.add_child(player)
    player.root_node = NodePath("..")
    var library := AnimationLibrary.new()
    library.add_animation("turn", anim)
    player.add_animation_library("", library)
    player.play("turn")
    player.seek(anim.length, true)
    var turned := skeleton.get_bone_pose_rotation(head)
    var want := Quaternion(Vector3(0, 0, 1), PI / 2)
    expect(absf(turned.dot(want)) > 0.9999, "the head has turned 90 degrees: %s" % turned)
    player.seek(0.0, true)
    expect(absf(skeleton.get_bone_pose_rotation(head).dot(Quaternion())) > 0.9999, "and starts straight")
