## Load an animated GLB at runtime (as the engine loads pack meshes), play
## each animation and save a contact sheet of frames per clip.
##
##   godot4.7 --path DIR --script godot_frames.gd -- ACTOR.glb OUT_DIR
##
## Needs a window (renders through the GPU); DISPLAY=:0 on this machine.
## HKX_REST=1 instead saves rest.png (bind pose beside a 1.8 m pole) and
## prints where the meshes and the skeleton's head are.
extends SceneTree

const COLS := 6
const CELL := Vector2i(320, 400)


func _init() -> void:
	var args := OS.get_cmdline_user_args()
	var glb_path := args[0]
	var out_dir := args[1]
	DirAccess.make_dir_recursive_absolute(out_dir)

	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	if doc.append_from_file(glb_path, state) != OK:
		push_error("cannot read " + glb_path)
		quit(1)
		return
	var scene := doc.generate_scene(state)
	root.size = CELL
	root.add_child(scene)

	var cam := Camera3D.new()
	cam.fov = 40
	root.add_child(cam)
	# The actor stands at the origin, ~1.3 m tall, facing -Z after conversion
	# (game +Y forward). Look at it from the side so legs read clearly.
	cam.look_at_from_position(Vector3(3.2, 1.0, 0.6), Vector3(0, 0.85, 0), Vector3.UP)
	cam.current = true
	var sun := DirectionalLight3D.new()
	root.add_child(sun)
	sun.look_at_from_position(Vector3(2, 3, 2), Vector3.ZERO, Vector3.UP)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.32, 0.36, 0.42)
	env.environment.ambient_light_color = Color(0.5, 0.5, 0.5)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	root.add_child(env)
	var floor := MeshInstance3D.new()
	floor.mesh = PlaneMesh.new()
	floor.mesh.size = Vector2(4, 4)
	root.add_child(floor)

	DisplayServer.window_set_size(CELL)
	for i in 30:  # as the viewer does: early frames come back empty
		await process_frame
	if OS.get_environment("HKX_REST") != "":
		var pole := MeshInstance3D.new()
		pole.mesh = BoxMesh.new()
		pole.mesh.size = Vector3(0.05, 1.8, 0.05)
		pole.position = Vector3(0, 0.9, -0.6)
		root.add_child(pole)
		cam.look_at_from_position(Vector3(8, 0.5, 0), Vector3(0, 0, 0), Vector3.UP)
		await RenderingServer.frame_post_draw
		root.get_texture().get_image().save_png(out_dir.path_join("rest.png"))
		for m in scene.find_children("*", "MeshInstance3D", true, false):
			print(m.name, " aabb ", (m as MeshInstance3D).get_aabb(), " global ", (m as MeshInstance3D).global_transform.origin)
		for sk in scene.find_children("*", "Skeleton3D", true, false):
			var s3 := sk as Skeleton3D
			var i := s3.find_bone("NPC Head [Head]")
			print("skeleton ", s3.global_transform, " head global ", s3.global_transform * s3.get_bone_global_pose(i).origin)
		quit(0)
		return
	var player: AnimationPlayer = scene.find_children("*", "AnimationPlayer", true, false)[0]
	for anim_name in player.get_animation_list():
		var anim := player.get_animation(anim_name)
		var n := COLS * 2
		var sheet := Image.create(CELL.x * COLS, CELL.y * 2, false, Image.FORMAT_RGBA8)
		for i in n:
			var t := anim.length * i / float(n)
			player.play(anim_name)
			player.seek(t, true)
			player.pause()
			await process_frame
			await RenderingServer.frame_post_draw
			var img := root.get_texture().get_image()
			img.convert(Image.FORMAT_RGBA8)
			sheet.blit_rect(img, Rect2i(Vector2i.ZERO, CELL), Vector2i((i % COLS) * CELL.x, (i / COLS) * CELL.y))
		var out := out_dir.path_join(String(anim_name) + ".png")
		sheet.save_png(out)
		print("wrote ", out, " (", anim.length, " s, ", anim.get_track_count(), " tracks)")
	quit(0)
