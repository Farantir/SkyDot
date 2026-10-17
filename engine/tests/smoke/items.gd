# SPDX-License-Identifier: GPL-3.0-or-later
#
# Start inventories, items and equipment on the real SE pack
# (SKYDOT_SSE_PACK): what a Riverwood chest and the Bleak Falls Barrow urns and
# chests hold when first looked at, exact for the fixed seed (the reference
# decides, see data/leveled.hpp), the same on every run and after a save; taking
# it all; equipping iron gear. Headless; skips without the pack.
extends SceneTree

const PLAYER := 0x14
const GOLD := 0xF
# Riverwood Trader: a personal chest and the merchant's chest.
const TRADER_CHEST := 0x0005A3B6
const MERCHANT_CHEST := 0x00078C0B
# Bleak Falls Barrow: a large urn, an empty one, a Draugr chest.
const URN := 0x0003906E
const EMPTY_URN := 0x00039005
const DRAUGR_CHEST := 0x0003A7C1
const IRON_SWORD := 0x00012EB7
const IRON_ARMOR := 0x00012E49
const IRON_SHIELD := 0x00012EB6
const IRON_BATTLEAXE := 0x00013980
const IRON_DAGGER := 0x0001397E

var failures := 0

func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_items: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)
	print("smoke_items: failures=", failures)
	quit(failures)

func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)

func _vm(pack: SkydotPack) -> SkydotPapyrus:
	var vm := SkydotPapyrus.new()
	vm.setup(pack, pack.open_world())
	return vm

## form -> count of an owner's items.
func _contents(vm: SkydotPapyrus, owner: int) -> Dictionary:
	var out := {}
	for row in vm.inventory(owner):
		out[int(row["form"])] = int(row["count"])
	return out

func _describe(vm: SkydotPapyrus, owner: int) -> String:
	var parts := PackedStringArray()
	for row in vm.inventory(owner):
		parts.append("%s x%d (0x%08X)" % [row["name"], row["count"], row["form"]])
	return ", ".join(parts)

func _run(pack_dir: String) -> void:
	var pack := SkydotPack.new()
	expect(pack.open(pack_dir) == OK, "the pack opens: " + pack.get_error())
	var vm := _vm(pack)

	# Exact contents for the fixed seed. A change to the resolver or to the
	# data shows here: look at the printed contents, then repin them.
	var expected := {
		TRADER_CHEST: {0xA199B: 1, GOLD: 6, 0x3AD68: 1, 0x33761: 1},
		URN: {0x877AB: 1},
		DRAUGR_CHEST: {GOLD: 25, IRON_BATTLEAXE: 1, 0xA44BE: 1},
		EMPTY_URN: {},
	}
	for ref in [TRADER_CHEST, MERCHANT_CHEST, URN, DRAUGR_CHEST, EMPTY_URN]:
		print("smoke_items: 0x%08X holds %s" % [ref, _describe(vm, ref)])
	for ref in expected:
		expect(_contents(vm, ref) == expected[ref],
			"0x%08X holds %s, expected %s" % [ref, _contents(vm, ref), expected[ref]])
	expect(_contents(vm, MERCHANT_CHEST).has(GOLD), "the merchant's chest holds gold")
	expect(not _contents(vm, URN).is_empty() and not _contents(vm, TRADER_CHEST).is_empty(),
		"the urn and the chest are not empty")

	# The same in a second VM, and after a save and a load.
	var again := _vm(pack)
	for ref in expected:
		expect(_contents(again, ref) == expected[ref], "a second VM resolves 0x%08X the same" % ref)
	var bytes := vm.save_state()
	var loaded := _vm(pack)
	expect(loaded.load_state(bytes) == OK, "the save loads: " + loaded.get_last_error())
	for ref in expected:
		expect(_contents(loaded, ref) == expected[ref], "0x%08X is the same after a load" % ref)

	# The equip slot ids the rules are written against.
	expect(vm.item_info(IRON_SWORD)["equip_slot"] == 0x13F42 and vm.item_info(IRON_SWORD)["weapon_type"] == 1,
		"an iron sword is RightHand, a sword")
	expect(vm.item_info(IRON_DAGGER)["weapon_type"] == 2, "a dagger")
	expect(vm.item_info(IRON_BATTLEAXE)["equip_slot"] == 0x13F45 and vm.item_info(IRON_BATTLEAXE)["weapon_type"] == 6,
		"a battleaxe is BothHands, a two-handed axe")
	expect(vm.item_info(IRON_SHIELD)["equip_slot"] == 0x141E8, "a shield is Shield")
	expect(vm.item_info(IRON_ARMOR)["category"] == "armor" and vm.item_info(IRON_ARMOR)["armor_type"] == 1,
		"iron armor is heavy armor")
	expect(vm.item_info(0x877AB)["category"] == "jewelry", "the garnet ring is jewelry")
	expect(vm.item_info(0xA44BE)["category"] == "scroll", "a scroll")

	# Take it all.
	for row in vm.inventory(DRAUGR_CHEST):
		expect(vm.transfer(DRAUGR_CHEST, PLAYER, int(row["form"]), int(row["count"])) == int(row["count"]),
			"all of " + String(row["name"]) + " moves")
	expect(vm.inventory(DRAUGR_CHEST).is_empty(), "the chest is empty")
	expect(_contents(vm, PLAYER) == expected[DRAUGR_CHEST], "the player has what the chest had")
	expect(_contents(_vm(pack), DRAUGR_CHEST) == expected[DRAUGR_CHEST], "(a fresh VM still sees it full)")

	# Equip.
	for form in [IRON_SWORD, IRON_ARMOR, IRON_SHIELD, IRON_DAGGER]:
		vm.add_item(PLAYER, form, 1)
	expect(vm.equip(PLAYER, IRON_SWORD) and vm.equip(PLAYER, IRON_ARMOR) and vm.equip(PLAYER, IRON_SHIELD),
		"sword, armor and shield equip")
	var equipment := vm.equipment(PLAYER)
	expect(equipment["right"] == IRON_SWORD and equipment["left"] == IRON_SHIELD
		and equipment["worn"] == PackedInt64Array([IRON_ARMOR]), "worn: %s" % [equipment])
	expect(vm.equip(PLAYER, IRON_BATTLEAXE), "the battleaxe equips")
	equipment = vm.equipment(PLAYER)
	expect(equipment["right"] == IRON_BATTLEAXE and equipment["left"] == 0, "and takes both hands: %s" % [equipment])
	var wearing := vm.inventory(PLAYER).filter(func(row: Dictionary) -> bool: return row["equipped"])
	expect(wearing.size() == 2, "the inventory shows the axe and the armor as equipped")
	var after := _vm(pack)
	expect(after.load_state(vm.save_state()) == OK, "saves with equipment")
	expect(after.equipment(PLAYER) == equipment, "the equipment survives a load")
