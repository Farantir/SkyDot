# SPDX-License-Identifier: GPL-3.0-or-later
#
# What the item list's sources share: which rows are listed (non-playable items
# never) and the text of a row. Pure functions over the rows `GameSession.inventory` returns.
class_name ItemRows
extends RefCounted

## Soul sizes by number (SLGM SOUL).
const SOULS: Array[String] = ["", "petty", "lesser", "common", "greater", "grand"]


## Whether a list shows `row`: playable, and with a name (the game hides the nameless, such as a
## dragon's blood effect armors).
static func is_listed(row: Dictionary) -> bool:
	return row["playable"] and row["name"] != ""


## The rows of `rows` a list may show: playable and named.
static func visible(rows: Array) -> Array:
	return rows.filter(is_listed)


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
