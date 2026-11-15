# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where the item list's rows come from: GameSession's getters, turned into the rows ItemListModel takes
# (inventory and container sides, the magic menu's spells, shouts, scrolls and effects, the favorites popup).
# This is the only place that reads the game for the list; the model and the view never do.
class_name ItemListSources
extends RefCounted

## The magic skills' actor value index (Alteration 18 ... Restoration 22) -> tab id.
const SCHOOLS := {18: "alteration", 19: "conjuration", 20: "destruction", 21: "illusion", 22: "restoration"}
const HOURS_PER_DAY := 24.0


## What `owner` carries as list rows (the playable, named ones), each a row of kind "item".
static func inventory(session: GameSession, owner := SkydotPapyrus.PLAYER_REF) -> Array:
	return from_items(session.inventory(owner), session)


## What a trader would deal in, as rows with a `price`. There is no barter flow yet (no prices from
## fBarterMin/fBarterMax and Speechcraft), so the price is the item's value.
static func barter(session: GameSession, owner: int) -> Array:
	var out := inventory(session, owner)
	for row: Dictionary in out:
		row["price"] = int(row["value"])
	return out


## The list rows of GameSession.inventory rows `items`; with `session`, a weapon's or armor's base Damage or
## Armor rating (the game shows them with skill and perks; those are not applied yet).
static func from_items(items: Array, session: GameSession = null) -> Array:
	var out: Array = []
	for source: Dictionary in ItemRows.visible(items):
		var row := source.duplicate()
		row["kind"] = "item"
		row["tab"] = ItemListRegistry.item_tab(String(source["category"]))
		row["label"] = ItemRows.label(source)
		var weight := float(source["weight"])
		row["vw"] = float(source["value"]) / weight if weight > 0.0 else 0.0
		if session != null:
			var info := session.item_info(int(source["form"]))
			if source["category"] in ["weapon", "ammo"]:
				row["damage"] = roundi(float(info.get("damage", 0.0)))
			elif source["category"] == "armor":
				row["armor"] = roundi(float(info.get("armor_rating", 0.0)))
		out.append(row)
	return out


## The spells, powers, shouts, scrolls and active effects of the player as rows.
static func magic(session: GameSession) -> Array:
	var out: Array = []
	var left := session.spell_in_hand(Magic.LEFT)
	var right := session.spell_in_hand(Magic.RIGHT)
	var power := session.spell_in_hand(Magic.POWER)
	var magicka := session.actor_value(SkydotPapyrus.PLAYER_REF, "Magicka")
	for info: Dictionary in session.known_spells():
		var form := int(info["form"])
		var row := {"form": form, "name": info["name"], "label": info["name"], "kind": "spell", "cost": int(info["cost"]),
			"concentration": int(info["casting_type"]) == Magic.CAST_CONCENTRATION,
			"left": form == left, "right": form == right, "affordable": float(info["cost"]) <= magicka}
		if int(info["type"]) >= 2:
			row["kind"] = "power"
			row["right"] = false
			row["left"] = false
			row["power"] = form == power
			row["equipped"] = form == power
			row["cooldown_hours"] = float(info["cooldown"]) * HOURS_PER_DAY
			row["tab"] = "powers"
		else:
			row["equipped"] = form == left or form == right
			row["tab"] = SCHOOLS.get(int(info["school"]), "other")
		out.append(row)
	for info: Dictionary in session.known_shouts():
		out.append({"form": int(info["shout"]), "name": info["name"], "label": info["name"], "kind": "shout", "tab": "shouts",
			"words": info["words"], "description": info["description"], "usable": int(info["usable"]), "power": bool(info["equipped"]),
			"equipped": bool(info["equipped"]), "affordable": true})
	for item: Dictionary in session.inventory():
		if item["category"] == "scroll" and item["playable"]:
			var form := int(item["form"])
			out.append({"form": form, "name": item["name"], "label": ItemRows.label(item), "kind": "scroll", "tab": "scrolls",
				"count": int(item["count"]), "left": form == left, "right": form == right, "equipped": form == left or form == right,
				"affordable": true})
	for effect: Dictionary in session.player_effects():
		if bool(effect["active"]) and String(effect["name"]) != "":
			out.append({"form": int(effect["mgef"]), "name": effect["name"], "label": effect["name"], "kind": "effect", "tab": "effects",
				"magnitude": float(effect["magnitude"]), "remaining": float(effect["remaining"]), "affordable": true})
	return out


## The favorites popup: every favorite item, spell, scroll and shout, in one list.
static func favorites(session: GameSession) -> Array:
	var out: Array = []
	for row: Dictionary in inventory(session) + magic(session):
		if row["kind"] == "item" and row["category"] == "scroll":
			continue  # magic() lists the scrolls
		if row["kind"] != "effect" and session.favorites.has(int(row["form"])):
			row["tab"] = "all"
			out.append(row)
	return out
