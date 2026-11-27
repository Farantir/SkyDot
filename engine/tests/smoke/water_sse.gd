# SPDX-License-Identifier: GPL-3.0-or-later
#
# Water data on the real SE pack (SKYDOT_SSE_PACK), against Skyrim.esm:
#   the White River at Riverwood is cell water at XCLW -250; Skyrim.esm gives it RiverWaterFlowNE
#   (NAM0 current (2.54, 1.35, 0)), Update.esm's override the SE flow-map type RiverWaterFlow
#   (no NAM0 current); downstream, the placed Water1024RiverFlowNE 0x3AC84 in cell
#   (7, -6) reports its activator's water type and its XWCU currents (three entries, the first
#   the type's velocity); a creek ref whose XWCU kept an older velocity than its type's.
# Headless; skips without the pack.
extends SceneTree

const TAMRIEL := 0x3C
const RIVER_FLOW_NE := 0xE717C
const RIVER_FLOW := 0x01001232  # Update.esm
const WATER_RIVER_NE := 0xE717D  # ACTI Water1024RiverFlowNE
const WHITE_RIVER_REF := 0x3AC84
const CREEK_REF := 0x31333  # Water1024Creek: XWCU (0.2867, 0.4096), DefaultCreekWater NAM0 (0.2267, 0.3596)

var failures := 0


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_water_sse: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	var pack := SkydotPack.new()
	if pack.open(pack_dir) != OK:
		printerr("FAIL: the pack opens: ", pack.get_error())
		quit(1)
		return
	var world := pack.open_world()
	if world == null:
		printerr("FAIL: the world opens")
		quit(1)
		return
	_riverwood(world)
	_placed(world)
	print("smoke_water_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func near(a: Vector3, b: Vector3) -> bool:
	return a.distance_to(b) < 0.001


func _riverwood(world: SkydotWorld) -> void:
	var cell := world.get_exterior_cell(TAMRIEL, 4, -12)
	expect(cell != 0, "Riverwood's cell (4, -12)")
	expect(world.get_cell_water_type(cell) == RIVER_FLOW, "its water is Update.esm's RiverWaterFlow: %x" % world.get_cell_water_type(cell))
	var flow: Dictionary = world.get_water_velocity(RIVER_FLOW)
	expect(near(flow.get("linear", Vector3.ONE), Vector3.ZERO), "with no current of its own: %s" % flow)
	var info: Dictionary = world.get_cell(cell)
	expect(absf(float(info.get("water_height", 0.0)) - (-250.0 * SkydotWorld.unit_scale())) < 0.01,
		"at XCLW -250: %s" % info.get("water_height"))
	var v: Dictionary = world.get_water_velocity(RIVER_FLOW_NE)
	expect(near(v.get("linear", Vector3.ZERO), Vector3(2.54, 1.35, 0)), "RiverWaterFlowNE flows NE: %s" % v)
	expect(near(v.get("angular", Vector3.ONE), Vector3.ZERO), "without an angular current")


func _placed(world: SkydotWorld) -> void:
	var base: Dictionary = world.get_base(WATER_RIVER_NE)
	expect(String(base.get("model", "")) == "meshes/water/water1024.nif", "the activator's model: %s" % base.get("model"))
	expect(world.get_ref_water_type(WHITE_RIVER_REF) == RIVER_FLOW_NE, "0x3AC84's water type: %x" % world.get_ref_water_type(WHITE_RIVER_REF))
	var currents := world.get_ref_water_currents(WHITE_RIVER_REF)
	expect(currents.size() == 3, "three XWCU entries: %s" % [currents])
	if currents.size() == 3:
		expect(currents[0].distance_to(Vector4(2.54, 1.35, 0, 0)) < 0.001, "the first is the type's velocity: %s" % currents[0])
		expect(currents[1] == Vector4.ZERO and currents[2] == Vector4.ZERO, "the others are 0")
	var creek := world.get_ref_water_currents(CREEK_REF)
	expect(creek.size() == 3 and creek[0].distance_to(Vector4(0.2867, 0.4096, 0, 0)) < 0.001,
		"a creek keeps its own XWCU: %s" % [creek])
	expect(world.get_ref_water_type(0x162A4) == 0 and world.get_ref_water_currents(0x162A4).is_empty(),
		"the Riverwood map marker is not water")
