extends "gd_test.gd"
# UiDraw.wrap: words into lines no wider than the width, measured with the font.

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
