# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the shared item list (ItemListModel, ItemListView) shows, as data: the tabs of each menu, the
# columns of each tab, the actions a row offers and the fields of its item card (ADR 0010). Views and the
# model read these tables and hard-code none of them, so later entries (SkyDot's own, mods) register the
# same way. Tabs, columns and actions are looked up by id; `register_*` adds or replaces one.
class_name ItemListRegistry
extends RefCounted

const FAVORITES := "favorites"

## Item categories (world.fbs ItemCategory names) by the inventory tab that lists them.
const ITEM_TABS := {
	"weapons": ["weapon", "ammo"],
	"apparel": ["armor", "clothing", "jewelry"],
	"potions": ["potion"],
	"scrolls": ["scroll"],
	"food": ["food"],
	"ingredients": ["ingredient"],
	"books": ["book"],
	"misc": ["misc", "soul_gem", "light"],
	"keys": ["key"],
}

## Tabs by menu, in order: {id, name, hidden_empty: hide it while it has no rows, all: lists every row}.
static var tabs: Dictionary = {
	"inventory": [
		{"id": FAVORITES, "name": "Favorites"}, {"id": "weapons", "name": "Weapons"}, {"id": "apparel", "name": "Apparel"},
		{"id": "potions", "name": "Potions"}, {"id": "scrolls", "name": "Scrolls"}, {"id": "food", "name": "Food"},
		{"id": "ingredients", "name": "Ingredients"}, {"id": "books", "name": "Books"}, {"id": "misc", "name": "Misc"},
		{"id": "keys", "name": "Keys"},
	],
	"magic": [
		{"id": FAVORITES, "name": "Favorites"}, {"id": "alteration", "name": "Alteration"},
		{"id": "conjuration", "name": "Conjuration"}, {"id": "destruction", "name": "Destruction"},
		{"id": "illusion", "name": "Illusion"}, {"id": "restoration", "name": "Restoration"},
		{"id": "other", "name": "Other", "hidden_empty": true}, {"id": "scrolls", "name": "Scrolls"},
		{"id": "shouts", "name": "Shouts", "hidden_empty": true}, {"id": "powers", "name": "Powers"},
		{"id": "effects", "name": "Active Effects"},
	],
	# A container, barter or gift side: what the inventory lists, with a tab for everything first.
	"container": [
		{"id": "all", "name": "All", "all": true}, {"id": "weapons", "name": "Weapons"}, {"id": "apparel", "name": "Apparel"},
		{"id": "potions", "name": "Potions"}, {"id": "scrolls", "name": "Scrolls"}, {"id": "food", "name": "Food"},
		{"id": "ingredients", "name": "Ingredients"}, {"id": "books", "name": "Books"}, {"id": "misc", "name": "Misc"},
		{"id": "keys", "name": "Keys"},
	],
	# The quick favorites popup: favorite items and spells together.
	"favorites": [{"id": "all", "name": "Favorites", "all": true}],
}
## Barter and gift lists are a container's: a menu id lists the tabs, columns and actions of another.
const SAME_AS := {"barter": "container", "gift": "container"}

## Columns by id: {id, title, numeric (sorts high to low first), sortable, width (a share of the table; 0 takes the
## rest), key (the row field to sort by; the id if absent)}.
static var columns: Dictionary = {
	"mark": {"id": "mark", "title": "", "sortable": false, "width": 0.05, "key": "favorite"},
	"name": {"id": "name", "title": "Name", "width": 0.0},
	"weight": {"id": "weight", "title": "Weight", "numeric": true, "width": 0.11},
	"value": {"id": "value", "title": "Value", "numeric": true, "width": 0.11},
	"vw": {"id": "vw", "title": "Value/Weight", "numeric": true, "width": 0.18},
	"damage": {"id": "damage", "title": "Damage", "numeric": true, "width": 0.11},
	"armor": {"id": "armor", "title": "Armor", "numeric": true, "width": 0.11},
	"price": {"id": "price", "title": "Price", "numeric": true, "width": 0.11},
	"count": {"id": "count", "title": "Count", "numeric": true, "width": 0.09},
	"cost": {"id": "cost", "title": "Cost", "numeric": true, "width": 0.14},
	"hands": {"id": "hands", "title": "Hand", "width": 0.09},
	"magnitude": {"id": "magnitude", "title": "Magnitude", "numeric": true, "width": 0.14},
	"remaining": {"id": "remaining", "title": "Time left", "numeric": true, "width": 0.14},
	"hotkey": {"id": "hotkey", "title": "Key", "width": 0.08},
	"kind": {"id": "kind", "title": "Type", "width": 0.12},
}

## The columns of a tab, by menu and tab id (the "*" entry is the default of the menu).
static var tab_columns: Dictionary = {
	"inventory": {
		"*": ["mark", "name", "weight", "value", "vw"],
		"weapons": ["mark", "name", "damage", "weight", "value", "vw"],
		"apparel": ["mark", "name", "armor", "weight", "value", "vw"],
	},
	"container": {
		"*": ["mark", "name", "weight", "value"],
		"weapons": ["mark", "name", "damage", "weight", "value"],
		"apparel": ["mark", "name", "armor", "weight", "value"],
	},
	# Barter: what a trader asks or pays. No barter flow exists yet, so `price` is the item's value.
	"barter": {
		"*": ["mark", "name", "weight", "price"],
		"weapons": ["mark", "name", "damage", "weight", "price"],
		"apparel": ["mark", "name", "armor", "weight", "price"],
	},
	"magic": {
		"*": ["mark", "name", "cost", "hands"],
		"favorites": ["mark", "name", "cost", "hands", "kind"],
		"scrolls": ["mark", "name", "hands", "count"],
		"shouts": ["mark", "name"],
		"powers": ["mark", "name", "cost"],
		"effects": ["name", "magnitude", "remaining"],
	},
	"favorites": {"*": ["name", "kind", "hotkey"]},
}

## Actions by id: {id, label, input (an InputMap action), menus, kinds (row kinds it applies to; none: all),
## when (a condition name for `applies`)}. A host runs an action when the view reports it.
static var actions: Dictionary = {
	"use": {"id": "use", "label": "equip", "input": &"ui_accept", "menus": ["inventory"], "kinds": ["item"]},
	"drop": {"id": "drop", "label": "drop", "input": &"menu_drop", "menus": ["inventory"], "kinds": ["item"]},
	"recharge": {"id": "recharge", "label": "recharge", "input": &"menu_charge", "menus": ["inventory"],
		"kinds": ["item"], "when": "rechargeable"},
	"favorite": {"id": "favorite", "label": "favorite", "input": &"menu_favorite", "menus": ["inventory", "magic"],
		"kinds": ["item", "spell", "power", "shout", "scroll"]},
	"equip_right": {"id": "equip_right", "label": "right hand", "input": &"ui_accept", "menus": ["magic"],
		"kinds": ["spell", "power", "scroll", "shout"]},
	"equip_left": {"id": "equip_left", "label": "left hand", "input": &"menu_equip_left", "menus": ["magic"],
		"kinds": ["spell", "power", "scroll", "shout"]},
	"unlock": {"id": "unlock", "label": "unlock word", "input": &"menu_drop", "menus": ["magic"], "kinds": ["shout"]},
	"move": {"id": "move", "label": "move stack", "input": &"ui_accept", "menus": ["container"]},
	"take_all": {"id": "take_all", "label": "take all", "input": &"menu_drop", "menus": ["container"], "global": true},
	"use_favorite": {"id": "use_favorite", "label": "use", "input": &"ui_accept", "menus": ["favorites"]},
	"equip_left_favorite": {"id": "equip_left_favorite", "label": "left hand", "input": &"menu_equip_left", "menus": ["favorites"],
		"kinds": ["spell", "scroll"]},
	"unfavorite": {"id": "unfavorite", "label": "remove", "input": &"menu_favorite", "menus": ["favorites"]},
}

## The card of a row, by row kind: [{label, field}]. `field_text` formats a field.
static var card_fields: Dictionary = {
	"item": [{"label": "Damage", "field": "damage"}, {"label": "Armor", "field": "armor"}, {"label": "Weight", "field": "weight"}, {"label": "Value", "field": "value"}, {"label": "Value/Weight", "field": "vw"},
		{"label": "Charge", "field": "charge"}, {"label": "Soul", "field": "soul"}],
	"scroll": [{"label": "Cost", "field": "cost"}, {"label": "Hand", "field": "hands"}, {"label": "Count", "field": "count"}],
	"spell": [{"label": "Cost", "field": "cost"}, {"label": "Hand", "field": "hands"}],
	"power": [{"label": "Cost", "field": "cost"}, {"label": "Wait", "field": "wait"}],
	"shout": [{"label": "Words", "field": "words_known"}],
	"effect": [{"label": "Magnitude", "field": "magnitude"}, {"label": "Time left", "field": "remaining"}],
}


## The tabs of `menu` (a barter or gift menu has its container's).
static func tabs_of(menu: String) -> Array:
	return tabs.get(menu, tabs.get(SAME_AS.get(menu, ""), []))


static func register_tab(menu: String, tab: Dictionary, at := -1) -> void:
	var list: Array = tabs.get(menu, [])
	if at < 0 or at > list.size():
		list.append(tab)
	else:
		list.insert(at, tab)
	tabs[menu] = list


static func register_column(column: Dictionary) -> void:
	columns[column["id"]] = column


static func register_action(action: Dictionary) -> void:
	actions[action["id"]] = action


## The column definitions of tab `tab` of `menu`.
static func columns_of(menu: String, tab: String) -> Array[Dictionary]:
	var by_tab: Dictionary = tab_columns.get(menu, tab_columns.get(SAME_AS.get(menu, ""), {}))
	var ids: Array = by_tab.get(tab, by_tab.get("*", ["name"]))
	var out: Array[Dictionary] = []
	for id: String in ids:
		if columns.has(id):
			out.append(columns[id])
	return out


## The inventory tab (id) that lists items of `category`; "misc" for one no tab names.
static func item_tab(category: String) -> String:
	for id: String in ITEM_TABS:
		if (ITEM_TABS[id] as Array).has(category):
			return id
	return "misc"


## The actions of `menu` that apply to `row` ({} for none, then only the global ones).
static func actions_for(menu: String, row: Dictionary) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for id: String in actions:
		var action: Dictionary = actions[id]
		if not (action["menus"] as Array).has(menu) and not (action["menus"] as Array).has(SAME_AS.get(menu, "")):
			continue
		if row.is_empty():
			if action.get("global", false):
				out.append(action)
			continue
		var kinds: Array = action.get("kinds", [])
		if not kinds.is_empty() and not kinds.has(row.get("kind", "item")):
			continue
		if action.has("when") and not _applies(String(action["when"]), row):
			continue
		out.append(action)
	return out


static func _applies(condition: String, row: Dictionary) -> bool:
	match condition:
		"rechargeable":
			return row.has("charge_max") and float(row["charge"]) < float(row["charge_max"])
	return true


## The label of `action` on `row` (equip, unequip or use for the primary item action; "unfavorite" when it is one).
static func action_label(action: Dictionary, row: Dictionary) -> String:
	match action["id"]:
		"use":
			if row.get("equipped", false):
				return "unequip"
			return "use" if row.get("category", "") in ["potion", "food", "ingredient", "book"] else "equip"
		"favorite":
			return "unfavorite" if row.get("favorite", false) else "favorite"
	return action["label"]


## The text of `field` of `row`, "" if the row has no such value.
static func field_text(field: String, row: Dictionary) -> String:
	match field:
		"name":
			return String(row.get("label", row.get("name", "")))
		"mark":
			return ("*" if row.get("favorite", false) else " ") + ("E" if row.get("equipped", false) else " ")
		"weight":
			return "%.1f" % float(row["weight"]) if row.has("weight") else ""
		"value":
			return str(int(row["value"])) if row.has("value") else ""
		"vw":
			return "%.1f" % float(row["vw"]) if row.has("vw") else ""
		"damage":
			return str(int(row["damage"])) if row.has("damage") else ""
		"armor":
			return str(int(row["armor"])) if row.has("armor") else ""
		"price":
			return str(int(row["price"])) if row.has("price") else ""
		"count":
			return str(int(row["count"])) if row.has("count") else ""
		"cost":
			if not row.has("cost"):
				return ""
			if int(row["cost"]) <= 0 and row.get("kind", "") == "power":
				return "-"
			return "%d%s" % [int(row["cost"]), "/s" if row.get("concentration", false) else ""]
		"hands":
			return ("L" if row.get("left", false) else "") + ("R" if row.get("right", false) else "")
		"wait":
			return "%d h" % ceili(float(row["cooldown_hours"])) if float(row.get("cooldown_hours", 0.0)) > 0.0 else "ready"
		"magnitude":
			var magnitude := float(row.get("magnitude", 0.0))
			return "%d" % roundi(magnitude) if absf(magnitude) >= 10.0 or is_equal_approx(magnitude, roundf(magnitude)) else "%.1f" % magnitude
		"remaining":
			var seconds := float(row.get("remaining", -1.0))
			if seconds < 0.0:
				return ""
			var s := ceili(seconds)
			return "%d s" % s if s < 60 else "%d:%02d" % [s / 60, s % 60]
		"hotkey":
			return str(int(row["hotkey"])) if int(row.get("hotkey", 0)) > 0 else ""
		"kind":
			return String(row.get("kind", "")).capitalize()
		"charge":
			return "%d / %d" % [int(row["charge"]), int(row["charge_max"])] if row.has("charge_max") else ""
		"soul":
			return String(ItemRows.SOULS[int(row["soul"])]) if int(row.get("soul", 0)) > 0 else ""
		"words_known":
			var known := 0
			for word: Dictionary in row.get("words", []):
				known += 1 if word["known"] else 0
			return "%d / %d" % [known, (row.get("words", []) as Array).size()] if row.has("words") else ""
	return ""
