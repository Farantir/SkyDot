extends "gd_test.gd"
# UiDraw's texts: wrap (words into lines no wider than the width, measured with the font) and the menus' date.

const PX := 16


func _width(text: String) -> float:
	return ThemeDB.fallback_font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, PX).x


func test_wrap_breaks_between_words() -> void:
	var font := ThemeDB.fallback_font
	var width := maxf(_width("alpha beta"), _width("gamma delta")) + 1.0
	var lines := UiDraw.wrap(font, "alpha beta gamma delta", PX, width)
	expect_eq(lines, ["alpha beta", "gamma delta"] as Array[String], "two words a line")
	for line in lines:
		expect(_width(line) <= width, "no line wider than the width: " + line)


func test_wrap_keeps_a_long_word_whole() -> void:
	var lines := UiDraw.wrap(ThemeDB.fallback_font, "a extraordinarily b", PX, _width("a b"))
	expect_eq(lines, ["a", "extraordinarily", "b"] as Array[String], "a word wider than the line stands alone")


func test_wrap_short_and_empty_text() -> void:
	expect_eq(UiDraw.wrap(ThemeDB.fallback_font, "one line", PX, 1000.0), ["one line"] as Array[String], "fits in one")
	expect_eq(UiDraw.wrap(ThemeDB.fallback_font, "", PX, 1000.0), [""] as Array[String], "empty text is one empty line")


func test_menu_date() -> void:
	var names := {"sDaySunday": "Sundas", "sDayMonday": "Morndas", "sMonthAugust": "Last Seed",
		"sMonthSeptember": "Heartfire", "sMonthJanuary": "Morning Star"}
	var gmst := func(setting: String) -> String: return names.get(setting, setting)
	expect_eq(UiDraw.format_date(0.33, 8.02, gmst), "Sundas, 8:01 AM, 17th of Last Seed, 4E 201", "the start")
	expect_eq(UiDraw.format_date(1.0, 13.5, gmst), "Morndas, 1:30 PM, 18th of Last Seed, 4E 201", "a day on")
	expect_eq(UiDraw.format_date(15.0, 0.0, gmst), "Morndas, 12:00 AM, 1st of Heartfire, 4E 201", "into the next month")
	expect_eq(UiDraw.format_date(140.0, 12.0, gmst), "Sundas, 12:00 PM, 4th of Morning Star, 4E 202", "into the next year")
