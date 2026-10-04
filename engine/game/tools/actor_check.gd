# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds an actor from a pack the way the engine does: a skeleton from its
# animation asset, body parts moved onto it, clips sampled into an
# AnimationPlayer. Prints what it built; with --out, also renders a contact
# sheet per clip (needs a window).
#
#   godot4.7 --path game --script res://tools/actor_check.gd -- --pack <pack> \
#       [--clips mt_walkforward,mt_runforward] [--out <dir>]
extends SceneTree

const CHARACTER := "meshes/actors/character/"
const PARTS := ["malebody_1.nif", "malehands_1.nif", "malefeet_1.nif", "malehead.nif"]
const COLS := 6
const CELL := Vector2i(320, 400)


func _initialize() -> void:
	var argv := OS.get_cmdline_user_args()
	var args := {"--clips": "mt_walkforward,mt_runforward,mt_idle", "--out": ""}
	for i in range(0, argv.size() - 1):
		args[argv[i]] = argv[i + 1]
	if not args.has("--pack"):
		printerr("actor_check: --pack is required")
		quit(2)
		return
	var pack := SkydotPack.new()
	if pack.open(args["--pack"]) != OK:
		printerr("actor_check: ", pack.get_error())
		quit(1)
		return

	var actor := Node3D.new()
	actor.name = "Actor"
	root.add_child(actor)
	var units := Node3D.new()
	units.name = "bethconv_z_up_to_y_up"
	units.transform = Transform3D(Basis(Vector3.RIGHT, -PI / 2).scaled(Vector3.ONE * SkydotWorld.unit_scale()), Vector3.ZERO)
	actor.add_child(units)
	var skeleton: Skeleton3D = SkydotAnimation.build_skeleton(pack.get_bytes(CHARACTER + "character assets/skeleton.hkx"))
	if skeleton == null:
		quit(1)
		return
	skeleton.name = "Skeleton"
	units.add_child(skeleton)
	print("skeleton: ", skeleton.get_bone_count(), " bones")

	var meshes := 0
	for part in PARTS:
		var model: Node = pack.load_scene(CHARACTER + "character assets/" + part).instantiate()
		if model == null:
			printerr("actor_check: cannot load ", part)
			continue
		actor.add_child(model)
		meshes += SkydotAnimation.attach_skinned(model, skeleton)
		var missing := SkydotAnimation.get_last_missing_bones()
		if missing != 0:
			printerr("actor_check: ", part, ": ", missing, " binds name bones the skeleton lacks")
		actor.remove_child(model)
		model.free()
	print("body: ", meshes, " skinned meshes moved onto the skeleton")

	var player := AnimationPlayer.new()
	actor.add_child(player)
	var library := AnimationLibrary.new()
	for clip in args["--clips"].split(","):
		var vpath: String = CHARACTER + "animations/male/" + clip + ".hkx"
		if not pack.has(vpath):
			vpath = CHARACTER + "animations/" + clip + ".hkx"
		var bytes := pack.get_bytes(vpath)
		var info: Dictionary = SkydotAnimation.describe(bytes)
		var anim: Animation = SkydotAnimation.build_clip(bytes, skeleton, "bethconv_z_up_to_y_up/Skeleton")
		if anim == null:
			printerr("actor_check: cannot build ", vpath)
			quit(1)
			return
		anim.loop_mode = Animation.LOOP_LINEAR
		library.add_animation(clip, anim)
		print("clip %s: %.2f s, %d frames, %d tracks, markers %s" % [clip, anim.length, info["clips"][0]["frames"], anim.get_track_count(), anim.get_marker_names()])
	player.add_animation_library("", library)

	# Feet must move and stay near the floor; report the left foot's travel.
	var foot := skeleton.find_bone("NPC L Foot [Lft ]")
	for clip in library.get_animation_list():
		player.play(clip)
		var lowest := INF
		var highest := -INF
		var first := Vector3.ZERO
		var travel := 0.0
		var anim := library.get_animation(clip)
		for i in 12:
			player.seek(anim.length * i / 12.0, true)
			var p := (units.transform * _global_pose(skeleton, foot)).origin
			if i == 0:
				first = p
			travel = max(travel, p.distance_to(first))
			lowest = min(lowest, p.y)
			highest = max(highest, p.y)
		print("  %s: left foot moves up to %.2f m, height %.2f..%.2f m" % [clip, travel, lowest, highest])

	if args["--out"] == "":
		quit(0)
		return
	await _render(actor, player, library, args["--out"])
	quit(0)


## A bone's pose in skeleton space from the local poses, which an animation
## seek sets at once (the skeleton's own global poses update at frame end).
func _global_pose(skeleton: Skeleton3D, bone: int) -> Transform3D:
	var t := skeleton.get_bone_pose(bone)
	var parent := skeleton.get_bone_parent(bone)
	while parent >= 0:
		t = skeleton.get_bone_pose(parent) * t
		parent = skeleton.get_bone_parent(parent)
	return t


func _render(actor: Node3D, player: AnimationPlayer, library: AnimationLibrary, out_dir: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	var cam := Camera3D.new()
	cam.fov = 40
	root.add_child(cam)
	cam.look_at_from_position(Vector3(3.2, 1.0, 0.6), Vector3(0, 0.85, 0), Vector3.UP)
	cam.current = true
	var sun := DirectionalLight3D.new()
	root.add_child(sun)
	sun.look_at_from_position(Vector3(2, 3, 2), Vector3.ZERO, Vector3.UP)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.32, 0.36, 0.42)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.5, 0.5, 0.5)
	root.add_child(env)
	var floor := MeshInstance3D.new()
	floor.mesh = PlaneMesh.new()
	floor.mesh.size = Vector2(4, 4)
	root.add_child(floor)
	DisplayServer.window_set_size(CELL)
	for i in 30:
		await process_frame
	for clip in library.get_animation_list():
		var anim := library.get_animation(clip)
		var sheet := Image.create(CELL.x * COLS, CELL.y * 2, false, Image.FORMAT_RGBA8)
		for i in COLS * 2:
			player.play(clip)
			player.seek(anim.length * i / float(COLS * 2), true)
			player.pause()
			await process_frame
			await RenderingServer.frame_post_draw
			var img := root.get_texture().get_image()
			img.convert(Image.FORMAT_RGBA8)
			sheet.blit_rect(img, Rect2i(Vector2i.ZERO, CELL), Vector2i((i % COLS) * CELL.x, (i / COLS) * CELL.y))
		sheet.save_png(out_dir.path_join(clip + ".png"))
		print("wrote ", out_dir.path_join(clip + ".png"))
