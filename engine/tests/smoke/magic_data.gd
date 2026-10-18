# SPDX-License-Identifier: GPL-3.0-or-later
#
# Magic data of world.fb 33 on the real SE pack (SKYDOT_SSE_PACK): the spells
# Flames (0x12FCD) and Healing (0x12FCC), their effects, the flame projectile,
# the player's spell list, a potion of healing (0x3EADE), a staff and an
# enchanted sword with their enchantments and charges. Headless; skips without
# the pack.
extends SceneTree

const FLAMES := 0x12FCD
const HEALING := 0x12FCC
const PLAYER_NPC := 0x7
const NORD_RACE := 0x13746
const POTION_HEALING := 0x3EADE
const STAFF_INCINERATE := 0x10FCF2
const DREMORA_FIRE_SWORD := 0x17009
const ARCHETYPE_VALUE_MODIFIER := 0
const AV_HEALTH := 24

var failures := 0
var _session: GameSession


class FakePlayer:
	extends RefCounted
	var yaw := 0.0
	var pitch := 0.0
	var flying := false

	func place_at(_at: Vector3, new_yaw: float, new_pitch: float) -> void:
		yaw = new_yaw
		pitch = new_pitch

	func set_view_distance(_metres: float) -> void:
		pass

	func save_section() -> PackedByteArray:
		return PackedByteArray()

	func load_section(_bytes: PackedByteArray) -> bool:
		return true


func _initialize() -> void:
	var pack_dir := OS.get_environment("SKYDOT_SSE_PACK")
	if pack_dir == "":
		print("smoke_magic_data: SKIP (SKYDOT_SSE_PACK unset)")
		quit(77)
		return
	_run(pack_dir)


func expect(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("FAIL: ", what)


func _run(pack_dir: String) -> void:
	await process_frame
	var config := GameConfig.new()
	config.pack = pack_dir
	config.navigation = false
	config.actors = false
	config.ai = false
	config.quests = false
	config.preload_doors = false
	var viewpoint := Camera3D.new()
	root.add_child(viewpoint)
	_session = GameSession.new()
	_session.interactive = false
	_session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
	root.add_child(_session)
	expect(_session.open(config, viewpoint, FakePlayer.new()), "the session opens")
	if _session.world == null:
		quit(1)
		return
	var world := _session.world

	# Flames: a concentration spell aimed at a target, with a flame projectile.
	var flames := world.get_spell(FLAMES)
	print("flames: ", flames)
	expect(flames.get("editor_id", "") == "Flames", "Flames is found")
	expect(flames["casting_type"] == 2, "Flames is a concentration spell: %s" % [flames.get("casting_type")])
	expect(flames["delivery"] == 2, "Flames is aimed: %s" % [flames.get("delivery")])
	expect(flames["type"] == 0 and flames["cost"] == 14, "Flames is a spell costing 14: %s" % [flames.get("cost")])
	expect(flames["half_cost_perk"] != 0 and flames["equip_slot"] != 0, "Flames has a half-cost perk and an equip slot")
	expect(flames["effects"].size() == 2, "Flames has two effects: %d" % flames["effects"].size())
	var fire := world.get_magic_effect(flames["effects"][0]["effect"])
	print("flames effect: ", fire)
	expect(fire.get("editor_id", "") == "FireDamageConcAimed", "its first effect is FireDamageConcAimed: %s" % [fire.get("editor_id")])
	expect(fire["archetype"] == ARCHETYPE_VALUE_MODIFIER, "which is a value modifier")
	expect(fire["actor_value"] == AV_HEALTH, "on Health: %s" % [fire.get("actor_value")])
	expect((fire["flags"] & 0x1) != 0 and (fire["flags"] & 0x4) != 0, "hostile and detrimental: 0x%X" % fire["flags"])
	expect(fire["casting_type"] == 2 and fire["delivery"] == 2, "concentration, aimed")
	expect(fire["magic_skill"] == 20, "Destruction (20): %s" % [fire.get("magic_skill")])
	expect(fire["resist_value"] == 41, "resisted by ResistFire (41): %s" % [fire.get("resist_value")])
	expect(fire["casting_light"] != 0 and fire["hit_shader"] != 0 and fire["casting_art"] != 0, "light, shader and art are set")
	expect(fire["sounds"].size() >= 2, "it has sounds: %d" % fire["sounds"].size())
	expect(fire["conditions"].size() == 0, "it has no conditions")
	var art := world.get_art_object(fire["casting_art"])
	print("casting art: ", art)
	expect(art.get("model", "").begins_with("meshes/") and art["model"].ends_with(".nif"), "its casting art has a model: %s" % [art.get("model")])
	var projectile := world.get_projectile(fire["projectile"])
	print("flames projectile: ", projectile)
	expect(projectile.get("type", 0) == 8, "Flames' projectile is a flame (type 8): %s" % [projectile.get("type")])
	expect(projectile.get("model", "") != "", "it has a model: %s" % [projectile.get("model")])
	expect(projectile["speed"] > 0.0 and projectile["range"] > 0.0 and projectile["relaunch_interval"] > 0.0, "speed, range and relaunch interval are set")
	var intense := world.get_magic_effect(flames["effects"][1]["effect"])
	expect(intense.get("archetype", -1) == 7, "its second effect is a Demoralize (7): %s" % [intense.get("archetype")])
	expect(flames["effects"][1]["conditions"].size() == 0 and intense["conditions"].size() > 0, "the perk condition sits on the second effect's MGEF")

	# Healing: a concentration spell on the caster, restoring Health.
	var healing := world.get_spell(HEALING)
	print("healing: ", healing)
	expect(healing.get("editor_id", "") == "Healing", "Healing is found")
	expect(healing["casting_type"] == 2 and healing["delivery"] == 0, "Healing is a concentration spell on the caster")
	var heal := world.get_magic_effect(healing["effects"][0]["effect"])
	print("healing effect: ", heal)
	expect(heal.get("archetype", -1) == ARCHETYPE_VALUE_MODIFIER and heal.get("actor_value", -1) == AV_HEALTH, "it restores Health with a value modifier")
	expect((heal["flags"] & 0x1) == 0, "and is not hostile")
	expect(heal["magic_skill"] == 22, "Restoration (22): %s" % [heal.get("magic_skill")])
	expect(healing["effects"][0]["magnitude"] > 0.0, "with a magnitude: %s" % [healing["effects"][0]["magnitude"]])
	expect(healing["effects"][1]["conditions"].size() > 0, "the second Healing effect has a condition")

	# The player lists both spells; so does the race's ability list or the NPC itself.
	var player := world.get_npc(PLAYER_NPC)
	print("player spells: ", player.get("spells"))
	expect(player.get("race", 0) == NORD_RACE, "the player is a Nord: 0x%X" % player.get("race", 0))
	expect(player["spells"].has(FLAMES) and player["spells"].has(HEALING), "NPC_ 0x7 lists Flames and Healing")
	var nord := world.get_race(NORD_RACE)
	expect(nord["spells"].size() > 0, "the Nord race has abilities: %d" % nord["spells"].size())
	var resist := world.get_spell(nord["spells"][0])
	expect(resist.get("type", -1) == 4 or world.get_spell(nord["spells"][0]).is_empty(), "its first entry is an ability (or a list)")

	# A potion of healing restores Health.
	var potion := world.get_item_effects(POTION_HEALING)
	print("potion effects: ", potion)
	expect(potion.size() == 1, "the potion has one effect")
	var restore := world.get_magic_effect(potion[0]["effect"])
	expect(restore.get("archetype", -1) == ARCHETYPE_VALUE_MODIFIER and restore.get("actor_value", -1) == AV_HEALTH, "it is a Restore Health value modifier: %s" % [restore.get("editor_id")])
	expect(potion[0]["magnitude"] == 50.0, "of 50 points: %s" % [potion[0]["magnitude"]])

	# Enchanted weapons: a staff and a sword.
	var staff := world.get_weapon(STAFF_INCINERATE)
	print("staff: ", staff)
	expect(staff.get("enchantment", 0) != 0 and staff["enchantment_amount"] > 0, "the staff has an enchantment and a charge: %s" % [staff.get("enchantment_amount")])
	var staff_ench := world.get_enchantment(staff["enchantment"])
	print("staff enchantment: ", staff_ench)
	expect(staff_ench.get("enchantment_type", 0) == 12, "a staff enchantment (12): %s" % [staff_ench.get("enchantment_type")])
	expect(staff_ench["effects"].size() > 0, "with effects")
	var sword := world.get_weapon(DREMORA_FIRE_SWORD)
	expect(sword.get("enchantment", 0) != 0 and sword["enchantment_amount"] > 0, "the sword has an enchantment and a charge: %s" % [sword.get("enchantment_amount")])
	var sword_ench := world.get_enchantment(sword["enchantment"])
	print("sword enchantment: ", sword_ench)
	expect(sword_ench.get("enchantment_type", 0) == 6, "an enchantment (6): %s" % [sword_ench.get("enchantment_type")])
	expect(sword_ench["effects"].size() > 0 and sword_ench["effects"][0]["magnitude"] > 0.0, "with a magnitude")
	expect(sword_ench["delivery"] == 1 and sword_ench["casting_type"] == 1, "touch, fire and forget: %s %s" % [sword_ench.get("delivery"), sword_ench.get("casting_type")])
	expect(world.get_spell(0x7FFFFFF).is_empty() and world.get_magic_effect(0x7FFFFFF).is_empty(), "unknown forms are empty")
	print("smoke_magic_data: ", "FAIL" if failures > 0 else "OK")
	quit(1 if failures > 0 else 0)
