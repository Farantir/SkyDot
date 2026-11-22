# SPDX-License-Identifier: GPL-3.0-or-later
#
# --open-map OPS: the world map opened after the load, then OPS in order (";" between them), for shots and tests:
#   on               only open it
#   zoom=F           the zoom fraction F (0 in .. 1 out), as the triggers set it
#   pan=DX:DY        the camera moved DX east and DY north (game units) as a pan does (z kept, clamped)
#   discover=0xREF   discover that marker (flags 3); discover=all every marker of the map
#   custom=X:Y       the custom marker at (X, Y) on the land
#   prompt=0xREF     the click on that marker (its question)
#   answer=N         the question's button N (0 Yes)
#   block            Game.EnableFastTravel(false)
#   cursor=X:Y       the cursor at view pixel (X, Y) (default the middle)
class_name MapShot
extends RefCounted


static func apply(items: ItemMenu, ops: String) -> void:
	var view := items.map_view
	var session: GameSession = view._session
	if not view.open():
		print("--open-map: no world map here")
		return
	for op in ops.split(";", false):
		var parts := op.strip_edges().split("=")
		var value := parts[1] if parts.size() > 1 else ""
		match parts[0]:
			"on":
				pass
			"zoom":
				view.scene.camera.zoom_by(float(value) - view.scene.camera.zoom)
			"pan":
				var d := value.split(":")
				view.scene.camera.move_to(Vector2(view.scene.camera.position.x + float(d[0]), view.scene.camera.position.y + float(d[1])))
			"discover":
				for m: Dictionary in session.world_map.markers(view.scene.world):
					if value == "all" or int(m["ref"]) == value.hex_to_int():
						session.papyrus.set_map_marker_flags(int(m["ref"]), 3)
			"custom":
				var c := value.split(":")
				var x := float(c[0])
				var y := float(c[1])
				session.world_map.place_custom(view.scene.world, Vector3(x, y, view.scene.camera.height_at.call(x, y)))
			"prompt":
				view.step_now()
				for s: Dictionary in view.shown:
					if int(s["ref"]) == value.hex_to_int():
						view.cursor = view.fill() * (s["at"] as Vector2)
						view.click(view.cursor)
			"answer":
				view.answer(int(value))
			"block":
				session.papyrus.set_fast_travel_enabled(false)
			"cursor":
				var p := value.split(":")
				view.cursor = Vector2(float(p[0]), float(p[1]))
			_:
				print("--open-map: unknown op ", op)
		view.step_now()
	print("map: ", view.describe())
