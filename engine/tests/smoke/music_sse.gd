# SPDX-License-Identifier: GPL-3.0-or-later
#
# Music data on the real SE pack (SKYDOT_SSE_PACK), against Skyrim.esm's MUSC and MUST:
#   every track of every music type is known, and each file and finale is an Ogg sound asset in the pack;
#   MUSCombat's flags, priority, ducking and fade; a combat track's cue points and finale; the one loop;
#   a palette's tracks; the default objects DFMS and BTMS;
#   Whiterun interiors' XCMO, Tamriel's ZNAM (_NONE), WeatherPineForest's RDMO and the region music at Riverwood.
# Headless; skips without the pack.
extends SceneTree

const TAMRIEL := 0x3C
const RIVERWOOD_MARKER := 0x162A4
const PINE_FOREST_REGION := 0x2A72D

var failures := 0


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_music_sse: SKIP (SKYDOT_SSE_PACK unset)")
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
	_tracks(pack, world)
	_records(world)
	_places(world)
	print("smoke_music_sse: ", "OK" if failures == 0 else "%d failures" % failures)
	quit(0 if failures == 0 else 1)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)
	else:
		print("ok: ", what)


func _tracks(pack: SkydotPack, world: SkydotWorld) -> void:
	var types := world.get_music_types()
	expect(types.size() >= 64, "%d music types (64 in SE, 50 of them Skyrim.esm's)" % types.size())
	var seen := {}
	var unknown: Array[String] = []
	var missing: Array[String] = []
	var counts := {"single": 0, "palette": 0, "silent": 0, "unknown": 0}
	var pending: Array = []
	for id in types:
		pending.append_array(Array(world.get_music_type(id)["tracks"]))
	while not pending.is_empty():
		var id: int = pending.pop_back()
		if id == 0 or seen.has(id):
			continue
		seen[id] = true
		var t := world.get_music_track(id)
		if t.is_empty():
			unknown.append("0x%X" % id)
			continue
		counts[t["type"]] += 1
		pending.append_array(Array(t["tracks"]))
		for key in ["file", "finale"]:
			var vpath: String = t[key]
			if vpath != "" and not pack.has(vpath):
				missing.append(vpath)
		if t["type"] == "single" and t["file"] == "":
			missing.append("%s has no file" % t["editor_id"])
	expect(unknown.is_empty(), "every track a type names is a MUST: %s" % [unknown])
	expect(missing.is_empty(), "every track file and finale is in the pack: %s" % [missing.slice(0, 8)])
	expect(counts["single"] >= 200 and counts["palette"] >= 10 and counts["silent"] >= 5 and counts["unknown"] == 0,
		"track types: %s" % [counts])
	var day := world.get_music_track(world.find_music_track("MUSExploreDay09"))
	expect(day.get("file", "") == "music/explore/mus_explore_day_09.xwm" and day["has_conditions"],
		"MUSExploreDay09: %s, conditioned" % day.get("file", ""))
	var bytes := pack.get_bytes(day.get("file", ""))
	var ogg := AudioStreamOggVorbis.load_from_buffer(bytes) if bytes.size() > 4 else null
	expect(bytes.slice(0, 4).get_string_from_ascii() == "OggS" and ogg != null and ogg.get_length() > 60.0,
		"its file is an Ogg Vorbis stream of %.0f s" % (ogg.get_length() if ogg != null else 0.0))


func _records(world: SkydotWorld) -> void:
	var combat_id := world.find_music_type("MUSCombat")
	var combat := world.get_music_type(combat_id)
	expect(combat_id == 0x3418E and combat["cycle_tracks"] and combat["ducks_current_track"]
		and not combat["plays_one_selection"] and combat["priority"] == 2
		and is_equal_approx(combat["ducking_db"], 100.0) and is_equal_approx(combat["fade_duration"], 9.0)
		and combat["tracks"].size() == 8,
		"MUSCombat: flags 0x%X, priority %d, ducking %.2f dB, fade %.1f s, %d tracks" % [combat.get("flags", 0),
			combat.get("priority", 0), combat.get("ducking_db", 0.0), combat.get("fade_duration", 0.0),
			combat.get("tracks", []).size()])
	expect(world.get_default_object("BTMS") == combat_id, "BTMS (battle music) is MUSCombat")
	var dfms := world.get_music_type(world.get_default_object("DFMS"))
	expect(dfms.get("editor_id", "") == "MUSExploreTestb", "DFMS (default music) is %s" % dfms.get("editor_id", ""))
	var c06 := world.get_music_track(0x1740F)
	expect(c06.get("editor_id", "") == "MUSCombat06" and c06["type"] == "single"
		and c06["file"] == "music/combat/mus_combat_06.xwm"
		and c06["finale"] == "music/combat/mus_combat_06_finale.xwm"
		and c06["cue_points"].size() == 17 and is_equal_approx(c06["cue_points"][0], 8.23),
		"MUSCombat06: finale %s, %d cue points" % [c06.get("finale", ""), c06.get("cue_points", []).size()])
	var chant := world.get_music_track(world.find_music_track("MUSSovngardeChantLP"))
	expect(chant.get("loop_count", 0) == 90 and is_equal_approx(chant["loop_end"], 209.24),
		"MUSSovngardeChantLP loops 0..%.2f s %d times" % [chant.get("loop_end", 0.0), chant.get("loop_count", 0)])
	var palette := world.get_music_track(world.find_music_track("MUSExploreSnowNightPalette"))
	expect(palette.get("type", "") == "palette" and is_equal_approx(palette["duration"], 210.0)
		and is_equal_approx(palette["fade_out"], 15.0) and palette["tracks"].size() > 4 and palette["has_conditions"],
		"MUSExploreSnowNightPalette: %.0f s, fade %.0f s, %d tracks" % [palette.get("duration", 0.0),
			palette.get("fade_out", 0.0), palette.get("tracks", []).size()])
	var silent := world.get_music_track(0x137FA)
	expect(silent.get("type", "") == "silent" and is_equal_approx(silent["duration"], 30.0) and silent["file"] == "",
		"_MUSExploreSILENT30 is 30 s of silence")


func _places(world: SkydotWorld) -> void:
	var town := world.find_music_type("MUSTownTest")
	expect(town != 0 and world.get_cell_music(world.find_cell("WhiterunBreezehome")) == town,
		"Breezehome plays MUSTownTest (XCMO)")
	expect(world.get_cell_music(world.find_cell("WhiterunJail02")) == world.find_music_type("MUSDungeon"),
		"Whiterun's jail plays MUSDungeon")
	var tamriel := world.get_world_music(TAMRIEL)
	expect(tamriel != 0 and world.get_music_type(tamriel).get("editor_id", "") == "_NONE",
		"Tamriel's world music (ZNAM) is set: _NONE")
	var pine := world.find_music_type("MUSExploreForestPine")
	expect(pine != 0 and world.get_region_sound_data(PINE_FOREST_REGION).get("music", 0) == pine,
		"WeatherPineForest's RDMO is MUSExploreForestPine")
	var cell := world.get_ref_cell(RIVERWOOD_MARKER)
	var at: Vector3 = world.get_ref_info(cell, RIVERWOOD_MARKER).get("position", Vector3.ZERO)
	var found := false
	for r: Dictionary in world.get_region_music(TAMRIEL, at.x, at.y):
		found = found or r["music"] == pine
	expect(found, "Riverwood (%.0f, %.0f) is in a region with MUSExploreForestPine" % [at.x, at.y])
