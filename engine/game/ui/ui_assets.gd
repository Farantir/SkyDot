# SPDX-License-Identifier: GPL-3.0-or-later
#
# The menus' own look, as the converter took it out of the game's movies (ADR 0010; formats/pack-format.md,
# "UI assets"): each interface/*.swf is a `ui` asset, a ZIP of TrueType fonts, an SVG per shape and sprite frame,
# PNGs and a manifest.json. This reads them through SkydotPack.get_zip_file and caches what it made: manifests,
# FontFiles, and Textures of SVGs rasterized at the scale asked for.
#
# Fonts are found as the game finds them: a movie's text field names "$EverywhereFont"; interface/fontconfig.txt
# (fontconfig_ru.txt etc. for the pack's language) maps it to a font name and style ("Futura CondensedLight"
# Normal) and lists the font libraries (fontlib "Interface\fonts_en.swf") that hold it.
#
# One pack at a time: GameSession hands it over with `use_pack` when it opens one. Without a pack, or with a pack
# from before the ui kind, `available()` is false and callers keep their fallback look (ui/theme/menu_theme.tres).
class_name UiAssets
extends RefCounted

static var _pack: SkydotPack
static var _manifests := {}  # swf vpath -> Dictionary (empty when missing)
static var _characters := {}  # swf vpath -> {id: character}
static var _textures := {}  # "swf|id|frame|scale" -> Texture2D (null when it does not draw)
static var _fonts := {}  # fontconfig name -> Font (null when missing)
static var _fontconfig := {}  # parsed fontconfig: {"libs": [vpath], "map": {name: [font, style]}}
static var _lists := {}  # "swf|id|frame" -> display list


## Hand over the pack the session opened (null forgets it).
static func use_pack(pack: SkydotPack) -> void:
	if pack == _pack:
		return
	_pack = pack
	_manifests.clear()
	_characters.clear()
	_textures.clear()
	_fonts.clear()
	_fontconfig.clear()
	_lists.clear()


## Whether the pack holds the menus' assets.
static func available() -> bool:
	return _pack != null and _pack.is_open() and _pack.get_kind("interface/fontconfig.txt") == "ui"


## The movie's vpath: "hudmenu" -> "interface/hudmenu.swf".
static func swf_path(swf: String) -> String:
	var path := swf.to_lower()
	if not path.ends_with(".swf"):
		path += ".swf"
	return path if path.begins_with("interface/") else "interface/" + path


## One file of a movie's bundle (or of a text asset), empty when missing.
static func file(swf: String, member: String) -> PackedByteArray:
	if _pack == null:
		return PackedByteArray()
	return _pack.get_zip_file(swf_path(swf) if not swf.ends_with(".txt") else swf, member)


static func manifest(swf: String) -> Dictionary:
	var path := swf_path(swf)
	if not _manifests.has(path):
		var text := file(path, "manifest.json").get_string_from_utf8() if available() else ""
		var parsed: Variant = JSON.parse_string(text) if text != "" else null
		_manifests[path] = parsed if parsed is Dictionary else {}
		var by_id := {}
		for character: Dictionary in (_manifests[path] as Dictionary).get("characters", []):
			by_id[int(character["id"])] = character
		_characters[path] = by_id
	return _manifests[path]


## A character of a movie (its manifest entry), empty when missing.
static func character(swf: String, id: int) -> Dictionary:
	manifest(swf)
	return (_characters.get(swf_path(swf), {}) as Dictionary).get(id, {})


## The character an export name stands for ("ItemCardList"), or -1.
static func exported(swf: String, name: String) -> int:
	return int(manifest(swf).get("exports", {}).get(name, -1))


## An instance by its path of instance names from the main timeline ("HUDMovieBaseInstance/Health/HealthMeter_mc"):
## {"id": character, "xform": Transform2D from its pixels to the stage's}, empty when a name is missing.
static func instance(swf: String, path: String) -> Dictionary:
	var display: Array = manifest(swf).get("root", {}).get("display", [])
	var xform := Transform2D.IDENTITY
	var id := -1
	for name in path.split("/"):
		var found := {}
		for placed: Dictionary in display:
			if placed.get("name", "") == name:
				found = placed
				break
		if found.is_empty():
			return {}
		var m: Array = found["matrix"]
		xform = xform * Transform2D(Vector2(m[0], m[1]), Vector2(m[2], m[3]), Vector2(m[4], m[5]))
		id = int(found["character"])
		display = character(swf, id).get("display", [])
	return {"id": id, "xform": xform}


## A sprite's display list at `frame` (0-based): frame 0's from the manifest, stepped through its timeline
## (places, moves and removes, as the player applies them). Entries as in the manifest's "display".
static func display_list(swf: String, id: int, frame: int) -> Array:
	var entry := character(swf, id)
	if frame <= 0 or not entry.has("timeline"):
		return entry.get("display", [])
	frame = mini(frame, int(entry.get("frames", 1)) - 1)
	var key := "%s|%d|%d" % [swf_path(swf), id, frame]
	if _lists.has(key):
		return _lists[key]
	var by_depth := {}
	for placed: Dictionary in entry.get("display", []):
		by_depth[int(placed["depth"])] = placed.duplicate()
	for event: Dictionary in entry["timeline"]:
		if int(event["f"]) > frame:
			break
		var depth := int(event["d"])
		if event.has("rm"):
			by_depth.erase(depth)
			continue
		var current: Dictionary = by_depth.get(depth, {})
		if not event.has("mv") or current.is_empty():
			if not event.has("c"):
				continue
			current = {"depth": depth, "matrix": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0]}
		else:
			current = current.duplicate()
		if event.has("c"):
			current["character"] = event["c"]
		if event.has("m"):
			current["matrix"] = event["m"]
		if event.has("x"):
			current["color"] = event["x"]
		if event.has("n"):
			current["name"] = event["n"]
		if event.has("k"):
			current["clip_depth"] = event["k"]
		by_depth[depth] = current
	var depths := by_depth.keys()
	depths.sort()
	var list := []
	for depth: int in depths:
		list.append(by_depth[depth])
	_lists[key] = list
	return list


## A named child of sprite `id` (its frame-0 display entry: character, matrix, ...), empty when missing.
static func child(swf: String, id: int, name: String) -> Dictionary:
	for placed: Dictionary in character(swf, id).get("display", []):
		if placed.get("name", "") == name:
			return placed
	return {}


## The frame a sprite's label stands for, or -1.
static func label_frame(swf: String, id: int, label: String) -> int:
	for entry: Dictionary in character(swf, id).get("labels", []):
		if entry["name"] == label:
			return int(entry["frame"])
	return -1


## The character's bounds in its own pixels (Rect2), zero when it has none.
static func bounds(swf: String, id: int) -> Rect2:
	var b: Array = character(swf, id).get("bounds", [])
	return Rect2(b[0], b[1], b[2], b[3]) if b.size() == 4 else Rect2()


## The 9-slice grid of a character (DefineScalingGrid) in its pixels, zero when it has none.
static func scaling_grid(swf: String, id: int) -> Rect2:
	var g: Array = character(swf, id).get("scaling_grid", [])
	return Rect2(g[0], g[1], g[2], g[3]) if g.size() == 4 else Rect2()


## The character drawn at `frame`, rasterized at `scale` pixels per movie pixel. The texture covers
## `bounds(swf, id)` (for frame 0; other frames' SVGs share the sprite's frame-0 bounds only roughly). Null if
## the character does not draw.
static func texture(swf: String, id: int, scale: float, frame := 0) -> Texture2D:
	scale = snappedf(maxf(scale, 0.05), 0.05)
	var key := "%s|%d|%d|%.2f" % [swf_path(swf), id, frame, scale]
	if _textures.has(key):
		return _textures[key]
	var entry := character(swf, id)
	var member: String = entry.get("svg", "")
	if frame != 0:
		member = (entry.get("frame_svgs", {}) as Dictionary).get(str(frame), "")
	var tex: Texture2D = null
	if member != "":
		var image := Image.new()
		if image.load_svg_from_buffer(file(swf, member), scale) == OK and not image.is_empty():
			tex = ImageTexture.create_from_image(image)
	_textures[key] = tex
	return tex


## The font a movie's text field names ("$EverywhereFont", or a font name), through fontconfig. Null if the
## pack has none.
static func font(name: String) -> Font:
	if _fonts.has(name):
		return _fonts[name]
	var result: Font = null
	if available():
		var config := _config()
		var wanted: Array = (config["map"] as Dictionary).get(name, [name, "Normal"])
		var bold: bool = String(wanted[1]) in ["Bold", "Demi"]
		var best := {}
		for lib: String in config["libs"]:
			for entry: Dictionary in manifest(lib).get("fonts", []):
				if entry.get("name", "") != wanted[0] or not entry.has("file"):
					continue
				if best.is_empty() or bool(entry.get("bold", false)) == bold:
					best = entry.duplicate()
					best["lib"] = lib
				if bool(entry.get("bold", false)) == bold:
					break
			if not best.is_empty() and bool(best.get("bold", false)) == bold:
				break
		if not best.is_empty():
			var bytes := file(best["lib"], best["file"])
			if not bytes.is_empty():
				var ttf := FontFile.new()
				ttf.data = bytes
				result = ttf
	_fonts[name] = result
	return result


## interface/fontconfig.txt for the pack's language: font libraries and the name map.
static func _config() -> Dictionary:
	if not _fontconfig.is_empty():
		return _fontconfig
	var suffix: String = {"russian": "_ru", "polish": "_pl", "japanese": "_ja", "chinese": "_cn"}.get(_language(), "")
	var path := "interface/fontconfig%s.txt" % suffix
	var text := file(path, path.get_file()).get_string_from_utf8()
	if text == "":
		text = file("interface/fontconfig.txt", "fontconfig.txt").get_string_from_utf8()
	_fontconfig = parse_fontconfig(text)
	return _fontconfig


## fontconfig.txt's `fontlib "Interface\x.swf"` and `map "$Name" = "Font" Style` lines.
static func parse_fontconfig(text: String) -> Dictionary:
	var libs: Array[String] = []
	var map := {}
	var quoted := RegEx.create_from_string("\"([^\"]*)\"")
	for raw in text.split("\n"):
		var line := raw.strip_edges()
		var parts := quoted.search_all(line)
		if line.begins_with("fontlib") and parts.size() == 1:
			libs.append(parts[0].get_string(1).replace("\\", "/").to_lower())
		elif line.begins_with("map") and parts.size() == 2:
			var style := line.substr(parts[1].get_end()).strip_edges()
			map[parts[0].get_string(1)] = [parts[1].get_string(1), style if style != "" else "Normal"]
	return {"libs": libs, "map": map}


static func _language() -> String:
	var path := _pack.get_path().path_join("manifest.json")
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	return String((parsed as Dictionary).get("language", "english")) if parsed is Dictionary else "english"
