# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the inventory and container views share: the categories of the item
# list, which rows each shows (non-playable items never), and the text of a
# row. Pure functions over the rows `GameSession.inventory` returns.
class_name ItemRows
extends RefCounted

## Menu categories, in order; each lists the item categories (world.fbs
## ItemCategory names) it shows. All shows every playable item.
const CATEGORIES: Array[Dictionary] = [
	{"name": "All", "items": []},
	{"name": "Weapons", "items": ["weapon", "ammo"]},
	{"name": "Apparel", "items": ["armor", "clothing", "jewelry"]},
	{"name": "Potions", "items": ["potion"]},
	{"name": "Scrolls", "items": ["scroll"]},
	{"name": "Food", "items": ["food"]},
	{"name": "Ingredients", "items": ["ingredient"]},
	{"name": "Books", "items": ["book"]},
	{"name": "Keys", "items": ["key"]},
	{"name": "Misc", "items": ["misc", "soul_gem", "light"]},
]


## Soul sizes by number (SLGM SOUL).
const SOULS: Array[String] = ["", "petty", "lesser", "common", "greater", "grand"]


## The playable rows of `rows` that category `index` shows.
static func visible(rows: Array, index := 0) -> Array:
	var wanted: Array = CATEGORIES[index]["items"]
	var out: Array = []
	for row: Dictionary in rows:
		if not row["playable"]:
			continue
		if wanted.is_empty() or wanted.has(row["category"]):
			out.append(row)
	return out


## "Iron Sword" or "Arrow (12)": a name with its count.
static func label(row: Dictionary) -> String:
	var text: String = row["name"] if row["name"] != "" else "0x%08X" % int(row["form"])
	if row.has("charge_max"):
		text += " [%d/%d]" % [int(row["charge"]), int(row["charge_max"])]
	elif int(row.get("soul", 0)) > 0:
		text += " [%s]" % SOULS[int(row["soul"])]
	return text + (" (%d)" % int(row["count"]) if int(row["count"]) > 1 else "")


static func weight_text(row: Dictionary) -> String:
	return "%.1f" % float(row["weight"])


## The total weight of `rows`, for the header.
static func total_weight(rows: Array) -> float:
	var total := 0.0
	for row: Dictionary in rows:
		total += float(row["weight"]) * int(row["count"])
	return total
