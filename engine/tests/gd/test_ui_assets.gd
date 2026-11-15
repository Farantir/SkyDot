extends "gd_test.gd"
# UiAssets (ui/ui_assets.gd) and UiTheme (ui/ui_theme.gd): fontconfig.txt parsing, the fallback without ui
# assets, and, when SKYDOT_SSE_PACK names a converted SE pack, fonts through fontconfig, the item list theme
# from the game's movies and the HUD meters' instances.

const FALLBACK := preload("res://ui/theme/menu_theme.tres")


func after_each() -> void:
	UiAssets.use_pack(null)


func test_fontconfig_lines() -> void:
	var parsed := UiAssets.parse_fontconfig('fontlib "Interface\\fonts_en.swf"\r\nmap "$EverywhereFont" = "Futura CondensedLight" Normal\r\n'
		+ 'map "$EverywhereBoldFont" = "Futura Condensed" Bold\r\nvalidNameChars "abc"\r\n')
	expect_eq(parsed["libs"], ["interface/fonts_en.swf"], "font libraries, as vpaths")
	expect_eq(parsed["map"]["$EverywhereFont"], ["Futura CondensedLight", "Normal"], "a name and its style")
	expect_eq(parsed["map"]["$EverywhereBoldFont"], ["Futura Condensed", "Bold"], "a bold one")
	expect_eq(parsed["map"].size(), 2, "other lines are not maps")


func test_no_pack_keeps_the_fallback() -> void:
	expect(not UiAssets.available(), "nothing without a pack")
	expect(UiTheme.item_list(FALLBACK, 720.0) == FALLBACK, "the theme resource stays")
	expect(UiAssets.font("$EverywhereFont") == null, "no font")


func _sse() -> SkydotPack:
	var dir := OS.get_environment("SKYDOT_SSE_PACK")
	if dir == "" or not FileAccess.file_exists(dir.path_join("manifest.json")):
		skip("SKYDOT_SSE_PACK unset")
		return null
	var pack := SkydotPack.new()
	if pack.open(dir) != OK:
		skip("the SE pack does not open (format)")
		return null
	UiAssets.use_pack(pack)
	if not UiAssets.available():
		skip("the SE pack has no ui assets")
		return null
	return pack


func test_game_fonts_and_theme() -> void:
	if _sse() == null:
		return
	var font := UiAssets.font("$EverywhereMediumFont")
	expect(font != null and font.get_font_name() == "Futura Condensed", "the list font through fontconfig")
	var bold := UiAssets.font("$EverywhereBoldFont")
	expect(bold != null and bold.get_font_style() & TextServer.FONT_BOLD != 0, "the bold one is bold")
	var theme := UiTheme.item_list(FALLBACK, 800.0)
	expect(theme != FALLBACK, "a theme of its own")
	expect(theme.default_font == font, "with the list entries' font")
	expect(theme.has_stylebox(&"panel", &"ItemListView"), "the item card's frame as the panel")
	var box := theme.get_stylebox(&"panel", &"ItemListView") as StyleBoxTexture
	expect(box != null and box.texture_margin_left > 10.0, "9-slice margins from the scaling grid")
	expect(theme.get_constant(&"font_size_720", &"ItemListView") == 20, "the entries' text size")
	expect(theme.has_icon(&"divider", &"ItemListView"), "the list's divider line")
	expect(UiTheme.item_list(FALLBACK, 800.0) == theme, "cached per scale")


func test_hud_meters() -> void:
	if _sse() == null:
		return
	for path in ["HUDMovieBaseInstance/Health/HealthMeter_mc", "HUDMovieBaseInstance/Magica/MagickaMeter_mc",
			"HUDMovieBaseInstance/Stamina/StaminaMeter_mc", "HUDMovieBaseInstance/Crosshair"]:
		var found := UiAssets.instance("hudmenu", path)
		expect(not found.is_empty(), path)
	var stamina := UiAssets.instance("hudmenu", "HUDMovieBaseInstance/Stamina/StaminaMeter_mc")
	expect((stamina["xform"] as Transform2D).x.x < 0.0, "the stamina meter is mirrored")
	expect(UiAssets.texture("hudmenu", int(stamina["id"]), 1.0) != null, "and draws")


func test_timeline_and_world_ui_data() -> void:
	var pack := _sse()
	if pack == null:
		return
	# Components.Meter: the fill slides under the mask from Full (frame 0) to Empty.
	var magicka := UiAssets.instance("hudmenu", "HUDMovieBaseInstance/Magica/MagickaMeter_mc")
	var id := int(magicka["id"])
	var empty := UiAssets.label_frame("hudmenu", id, "Empty")
	expect(empty > 0, "the meter has an Empty label")
	var full_x := 0.0
	var empty_x := 0.0
	for entry: Dictionary in UiAssets.display_list("hudmenu", id, 0):
		if int(entry["depth"]) == 3:
			full_x = float(entry["matrix"][4])
	for entry: Dictionary in UiAssets.display_list("hudmenu", id, empty):
		if int(entry["depth"]) == 3:
			empty_x = float(entry["matrix"][4])
	expect(full_x - empty_x > 300.0, "the fill moved left by its width: %s -> %s" % [full_x, empty_x])
	var world := pack.open_world()
	var imid := world.get_default_object("IMID")
	expect(imid != 0, "DOBJ IMID, kept from Skyrim.esm although later DOBJs do not list it")
	var imad := world.get_image_space_modifier(imid)
	expect(float(imad.get("dof_distance", 0.0)) == 128.0 and float(imad.get("dof_range", 0.0)) == 768.0,
		"the inventory's depth of field")
	var markers := world.get_map_markers(world.find_world("Tamriel"))
	var riverwood := markers.filter(func(m: Dictionary) -> bool: return m["name"] == "Riverwood")
	expect(riverwood.size() == 1, "Riverwood's map marker, by its name")
