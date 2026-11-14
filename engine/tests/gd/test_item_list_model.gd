extends "gd_test.gd"
# ItemListModel (ui/item_list_model.gd) over fake rows: tab membership, the favorites tab, sort per column and
# per tab, the search filter, the cursor over new rows, the card, the registry's actions; and Favorites (the
# per-form flag and hotkeys, and its save round trip).

const SWORD := 0x100
const DAGGER := 0x101
const BOW := 0x102
const SHIRT := 0x103
const POTION := 0x104


func _item(form: int, name: String, tab: String, weight: float, value: int, extra := {}) -> Dictionary:
	var row := {"form": form, "extra": 0, "name": name, "label": name, "kind": "item", "tab": tab, "category": tab, "count": 1,
		"weight": weight, "value": value, "vw": float(value) / weight if weight > 0.0 else 0.0, "equipped": false}
	row.merge(extra, true)
	return row


func _rows() -> Array:
	return [
		_item(SWORD, "Iron Sword", "weapons", 9.0, 25),
		_item(DAGGER, "Dagger", "weapons", 2.0, 10),
		_item(BOW, "Hunting Bow", "weapons", 7.0, 50),
		_item(SHIRT, "Fine Clothes", "apparel", 1.0, 125),
		_item(POTION, "Potion of Health", "potions", 0.5, 50),
	]


func _model(favorites := Favorites.new()) -> ItemListModel:
	var model := ItemListModel.new("inventory", favorites)
	model.set_rows(_rows())
	return model


func _names(rows: Array) -> Array:
	return rows.map(func(row: Dictionary) -> String: return row["name"])


func test_tabs_are_the_registered_ones_in_order() -> void:
	expect_eq(_model().tab_names(), ["Favorites", "Weapons", "Apparel", "Potions", "Scrolls", "Food", "Ingredients", "Books", "Misc", "Keys"] as Array[String], "inventory tabs")
	var magic := ItemListModel.new("magic")
	expect_eq(magic.tab_names(), ["Favorites", "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Scrolls", "Powers", "Active Effects"] as Array[String], "magic tabs without the empty hidden ones")
	magic.set_rows([{"form": 1, "name": "Fus", "kind": "shout", "tab": "shouts"}])
	expect(magic.tab_names().has("Shouts") and not magic.tab_names().has("Other"), "a hidden tab shows once it has rows")


func test_a_tab_lists_its_rows_sorted_by_name() -> void:
	var model := _model()
	expect(model.select_tab("Weapons"), "select by name")
	expect_eq(_names(model.rows()), ["Dagger", "Hunting Bow", "Iron Sword"], "weapons by name")
	model.select_tab("apparel")
	expect_eq(_names(model.rows()), ["Fine Clothes"], "apparel by id")
	expect(not model.select_tab("Nope"), "an unknown tab is refused")


func test_sorting_by_column_toggles_and_is_per_tab() -> void:
	var model := _model()
	model.select_tab("Weapons")
	model.sort_by("value")
	expect_eq(_names(model.rows()), ["Hunting Bow", "Iron Sword", "Dagger"], "a number sorts high to low first")
	model.sort_by("value")
	expect_eq(_names(model.rows()), ["Dagger", "Iron Sword", "Hunting Bow"], "again reverses")
	model.sort_by("weight")
	expect_eq(_names(model.rows()), ["Iron Sword", "Hunting Bow", "Dagger"], "weight high to low")
	model.sort_by("name")
	expect_eq(_names(model.rows()), ["Dagger", "Hunting Bow", "Iron Sword"], "name ascending")
	model.sort_by("name")
	expect_eq(_names(model.rows()), ["Iron Sword", "Hunting Bow", "Dagger"], "name descending")
	model.sort_by("vw")
	expect_eq(_names(model.rows()), ["Hunting Bow", "Dagger", "Iron Sword"], "value per weight high to low")
	model.select_tab("Apparel")
	expect_eq(model.sort_state(), {"column": "name", "ascending": true}, "another tab keeps its own sort")
	model.sort_by("mark")
	expect_eq(model.sort_state(), {"column": "name", "ascending": true}, "the mark column does not sort")


func test_search_filters_the_tab_by_name() -> void:
	var model := _model()
	model.select_tab("Weapons")
	model.search = "BOW"
	expect_eq(_names(model.rows()), ["Hunting Bow"], "case-insensitive substring")
	model.search = "zzz"
	expect(model.rows().is_empty() and model.current().is_empty(), "nothing matches")
	model.search = ""
	expect_eq(model.rows().size(), 3, "cleared")
	model.search = "o"
	expect_eq(_names(model.rows_of_tab("Weapons")).size(), 3, "rows_of_tab ignores the search")


func test_favorites_flag_and_tab() -> void:
	var favorites := Favorites.new()
	var model := _model(favorites)
	model.select_tab("Favorites")
	expect(model.rows().is_empty(), "no favorites yet")
	var changes := watch(model, "changed")
	favorites.toggle(BOW)
	favorites.toggle(POTION)
	expect(changes.size() >= 2, "the model reports a change")
	expect_eq(_names(model.rows()), ["Hunting Bow", "Potion of Health"], "favorites from every tab")
	model.select_tab("Weapons")
	expect(model.rows().filter(func(row: Dictionary) -> bool: return row["favorite"]).size() == 1, "the flag is on the row")
	expect_eq(ItemListRegistry.field_text("mark", model.rows_of_tab("Weapons")[1]), "* ", "a favorite shows a star")
	favorites.toggle(BOW)
	model.select_tab("Favorites")
	expect_eq(_names(model.rows()), ["Potion of Health"], "unmarked")


func test_the_cursor_follows_its_row_over_new_rows() -> void:
	var model := _model()
	model.select_tab("Weapons")
	model.select(2)
	expect_eq(model.current()["name"], "Iron Sword", "cursor on Iron Sword")
	var rows := _rows()
	rows.insert(0, _item(0x200, "Axe", "weapons", 8.0, 30))
	model.set_rows(rows)
	expect_eq(model.current()["name"], "Iron Sword", "still on it with a row before it")
	model.set_rows([_item(DAGGER, "Dagger", "weapons", 2.0, 10)])
	expect_eq(model.current()["name"], "Dagger", "it was removed: the cursor is clamped")
	expect_eq(model.tab_name(), "Weapons", "and the tab stays")
	model.move(1)
	expect_eq(model.selected, 0, "moving wraps")


func test_card_and_actions() -> void:
	var model := _model()
	model.select_tab("Weapons")
	model.select(2)
	var card := model.card()
	expect(card.any(func(field: Dictionary) -> bool: return field["label"] == "Weight" and field["text"] == "9.0"), "card has the weight: %s" % [card])
	expect(not card.any(func(field: Dictionary) -> bool: return field["label"] == "Charge"), "and not what the row lacks")
	var ids := ItemListRegistry.actions_for("inventory", model.current()).map(func(a: Dictionary) -> String: return a["id"])
	expect(ids.has("use") and ids.has("drop") and ids.has("favorite") and not ids.has("recharge"), "inventory actions: %s" % [ids])
	var charged := _item(1, "Flame Sword", "weapons", 9.0, 90, {"charge": 100.0, "charge_max": 3000.0})
	expect(ItemListRegistry.actions_for("inventory", charged).any(func(a: Dictionary) -> bool: return a["id"] == "recharge"), "recharge on a weapon below its maximum")
	expect(ItemListRegistry.actions_for("magic", {"kind": "effect"}).is_empty(), "an active effect has no action")
	expect(ItemListRegistry.actions_for("container", {}).any(func(a: Dictionary) -> bool: return a["id"] == "take_all"), "take all needs no row")


func test_container_menu_has_an_all_tab_and_columns_follow_the_tab() -> void:
	var model := ItemListModel.new("container")
	model.set_rows(_rows())
	expect_eq(model.tab_names()[0], "All", "all first")
	expect_eq(model.rows().size(), 5, "it lists everything")
	var magic := ItemListModel.new("magic")
	magic.set_rows([{"form": 1, "name": "Flames", "kind": "spell", "tab": "destruction", "cost": 14}])
	magic.select_tab("Destruction")
	expect_eq(magic.columns().map(func(c: Dictionary) -> String: return c["id"]), ["mark", "name", "cost", "hands"], "spell columns")
	expect_eq(ItemListRegistry.columns_of("magic", "effects").map(func(c: Dictionary) -> String: return c["id"]), ["name", "magnitude", "remaining"], "effect columns")


func test_favorites_hotkeys_and_save_round_trip() -> void:
	var favorites := Favorites.new()
	favorites.assign(3, BOW)
	favorites.toggle(SWORD)
	expect(favorites.has(BOW) and favorites.has(SWORD), "marked")
	expect_eq(favorites.hotkey_of(BOW), 3, "hotkey of")
	expect_eq(favorites.form_at(3), BOW, "form at")
	favorites.assign(3, SWORD)
	expect_eq(favorites.form_at(3), SWORD, "a slot takes the new form")
	expect_eq(favorites.hotkey_of(BOW), 0, "and the old one loses it")
	favorites.assign(0, SWORD)
	expect_eq(favorites.form_at(3), 0, "slot 0 frees the hotkey")
	favorites.assign(5, SWORD)
	favorites.toggle(SWORD)
	expect_eq(favorites.form_at(5), 0, "unmarking frees the hotkey")
	favorites.assign(7, BOW)
	var restored := Favorites.new()
	restored.adopt(Favorites.decode(favorites.save_bytes()))
	expect(restored.has(BOW) and not restored.has(SWORD) and restored.form_at(7) == BOW, "round trip")
	expect(Favorites.decode(PackedByteArray([1, 2, 3])).is_empty() and Favorites.decode(var_to_bytes([1])).is_empty(), "garbage is refused")
	restored.adopt({})
	expect(restored.forms().is_empty() and restored.form_at(7) == 0, "an old save (no section) has none")
