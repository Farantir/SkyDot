# SPDX-License-Identifier: GPL-3.0-or-later
#
# Casting: the rules of the hands and the voice, for the player and NPCs, over a fake
# CastingData that answers from dictionaries and logs what was paid or changed.
extends "gd_test.gd"

const P := 0x14
const NPC := 0x1234
const FIREBOLT := 0x100   # fire and forget, charge 0.5, costs 20
const FLAMES := 0x101     # concentration
const SCROLL := 0x102     # a SCRL item
const POWER_SPELL := 0x103
const SHOUT := 0x200

class FakeData extends CastingData:
	var equipped := {}      # "caster:hand" -> spell
	var infos := {}         # spell -> cast_info
	var dead := {}          # caster -> true
	var magickas := {}      # caster -> float
	var costs := {}         # spell -> float
	var cooldowns := {}     # spell -> float
	var items := {}         # "caster:item" -> true
	var shouts := {}        # shout -> shout_info
	var recovery := {}      # caster -> float
	var gmsts := {}
	var budget := INF       # seconds of concentration the payer can still afford
	var pay_refusal := ""   # a fire-and-forget payment is refused with this
	var log: Array = []

	func equipped_spell(caster: int, hand: int) -> int:
		return int(equipped.get("%d:%d" % [caster, hand], 0))

	func unequip_spell(caster: int, hand: int) -> void:
		log.append(["unequip", caster, hand])
		equipped.erase("%d:%d" % [caster, hand])

	func cast_info(spell: int) -> Dictionary:
		return infos.get(spell, {"found": false})

	func is_dead(caster: int) -> bool:
		return dead.has(caster)

	func magicka(caster: int) -> float:
		return float(magickas.get(caster, 100.0))

	func spell_cost(_caster: int, spell: int) -> float:
		return float(costs.get(spell, 0.0))

	func power_cooldown(_caster: int, spell: int) -> float:
		return float(cooldowns.get(spell, 0.0))

	func carried(caster: int, item: int) -> bool:
		return items.has("%d:%d" % [caster, item])

	func prepare(caster: int) -> void:
		log.append(["prepare", caster])

	func pay_spell(caster: int, spell: int, seconds: float) -> Dictionary:
		log.append(["pay", caster, spell, seconds])
		if seconds > 0.0:
			if budget < seconds - 1e-9:
				return {"ok": false, "reason": "not enough magicka", "cost": 0.0}
			budget -= seconds
			return {"ok": true, "reason": "", "cost": seconds * 10.0}
		if pay_refusal != "":
			return {"ok": false, "reason": pay_refusal, "cost": 0.0}
		return {"ok": true, "reason": "", "cost": float(costs.get(spell, 0.0))}

	func break_invisibility(caster: int) -> void:
		log.append(["break", caster])

	func shout_info(_caster: int, shout: int) -> Dictionary:
		return shouts.get(shout, {"found": false})

	func voice_recovery(caster: int) -> float:
		return float(recovery.get(caster, 0.0))

	func gmst_float(name: String, fallback: float) -> float:
		return float(gmsts.get(name, fallback))

	func count(kind: String) -> int:
		var n := 0
		for entry in log:
			if entry[0] == kind:
				n += 1
		return n

var data: FakeData
var casting: Casting

func before_each() -> void:
	data = FakeData.new()
	data.infos[FIREBOLT] = {"found": true, "is_scroll": false, "is_item": false, "casting_type": 1, "charge_time": 0.5}
	data.infos[FLAMES] = {"found": true, "is_scroll": false, "is_item": false, "casting_type": 2, "charge_time": 0.0}
	data.infos[SCROLL] = {"found": true, "is_scroll": true, "is_item": true, "casting_type": 1, "charge_time": 0.0}
	data.infos[POWER_SPELL] = {"found": true, "is_scroll": false, "is_item": false, "casting_type": 1, "charge_time": 0.0}
	data.costs[FIREBOLT] = 20.0
	data.shouts[SHOUT] = {"found": true, "usable": 3}
	casting = Casting.new(data)

func _equip(caster: int, hand: int, spell: int) -> void:
	data.equipped["%d:%d" % [caster, hand]] = spell

func _hold(hand: int, spell: int, caster := P) -> void:
	_equip(caster, hand, spell)
	expect(casting.press(caster, hand)["ok"], "pressed")

# ---- ports of the C++ [hand] cases

func test_fire_and_forget_charges_and_casts_on_release() -> void:
	_hold(Casting.RIGHT, FIREBOLT)
	expect_eq(casting.phase_of(P, Casting.RIGHT), "charging", "charging")
	expect_eq(casting.step(P, Casting.RIGHT, 0.25)["event"], "", "no event yet")
	expect_eq(casting.step(P, Casting.RIGHT, 0.0)["progress"], 0.5, "half charged")
	var full := casting.step(P, Casting.RIGHT, 0.3)
	expect_eq(full["event"], "charged", "charged")
	expect_eq(casting.phase_of(P, Casting.RIGHT), "charged", "phase charged")
	expect_eq(casting.step(P, Casting.RIGHT, 0.3)["event"], "", "announced once")
	var r := casting.release(P, Casting.RIGHT)
	expect_eq(r["event"], "cast", "cast")
	expect(absf(float(r["seconds"]) - 0.85) < 1e-6, "seconds held")
	expect_eq(casting.phase_of(P, Casting.RIGHT), "", "idle after release")
	expect_eq(casting.release(P, Casting.RIGHT)["event"], "", "a second release does nothing")

func test_release_before_charge_casts_nothing_and_pays_nothing() -> void:
	_hold(Casting.RIGHT, FIREBOLT)
	casting.step(P, Casting.RIGHT, 0.4)
	var r := casting.release(P, Casting.RIGHT)
	expect_eq(r["event"], "", "nothing")
	expect(not r["paid"], "not paid")
	expect_eq(data.count("pay"), 0, "no payment")
	expect_eq(data.count("break"), 0, "invisibility stays")
	expect(not casting.is_casting(P, Casting.RIGHT), "idle")

func test_charge_time_zero_casts_at_once() -> void:
	_hold(Casting.POWER, POWER_SPELL)
	expect_eq(casting.release(P, Casting.POWER)["event"], "cast", "cast at once")

func test_concentration_pays_and_ticks_until_released_or_out_of_magicka() -> void:
	data.budget = 0.25
	_hold(Casting.LEFT, FLAMES)
	var t1 := casting.step(P, Casting.LEFT, 0.1)
	expect_eq(t1["event"], "tick", "tick")
	expect(absf(float(t1["cost"]) - 1.0) < 1e-6, "the tick's cost")
	expect_eq(casting.step(P, Casting.LEFT, 0.1)["event"], "tick", "second tick")
	var out := casting.step(P, Casting.LEFT, 0.1)
	expect_eq(out["event"], "stop", "cannot pay: stop")
	expect_eq(out["reason"], "out of magicka", "reason")
	expect(not casting.is_casting(P, Casting.LEFT), "the hand is removed")
	expect_eq(casting.release(P, Casting.LEFT)["event"], "", "already over")
	data.budget = 10.0
	_hold(Casting.LEFT, FLAMES)
	casting.step(P, Casting.LEFT, 0.5)
	expect_eq(casting.release(P, Casting.LEFT)["event"], "stop", "released: stop")
	expect_eq(casting.step(P, Casting.LEFT, 0.5)["event"], "", "idle does not pay")

# ---- press

func test_press_refusals_in_order() -> void:
	expect_eq(casting.press(P, 1)["reason"], "no spell", "nothing equipped")
	_equip(P, 1, 0x999)
	expect_eq(casting.press(P, 1)["reason"], "no spell", "spell unknown to cast_info")
	_equip(P, 1, FIREBOLT)
	data.dead[P] = true
	data.cooldowns[FIREBOLT] = 1.0
	data.magickas[P] = 0.0
	expect_eq(casting.press(P, 1)["reason"], "dead", "dead before cooldown and magicka")
	data.dead.clear()
	expect_eq(casting.press(P, 1)["reason"], "cooldown", "cooldown before magicka")
	data.cooldowns.clear()
	data.magickas[P] = 19.0
	expect_eq(casting.press(P, 1)["reason"], "not enough magicka", "cost 20, magicka 19")
	data.magickas[P] = 20.0
	var ok := casting.press(P, 1)
	expect(ok["ok"] and ok["spell"] == FIREBOLT and not ok["concentration"] and not ok["is_scroll"], "enough magicka: %s" % [ok])
	expect(data.count("prepare") >= 1, "the caster is primed")

func test_press_concentration_needs_some_magicka_not_the_cost() -> void:
	_equip(P, 0, FLAMES)
	data.costs[FLAMES] = 50.0
	data.magickas[P] = 1.0
	var ok := casting.press(P, 0)
	expect(ok["ok"] and ok["concentration"], "magicka above 0 is enough")
	casting.cancel(P, 0)
	data.magickas[P] = 0.0
	expect_eq(casting.press(P, 0)["reason"], "not enough magicka", "no magicka")

func test_free_cost_spell_needs_no_magicka() -> void:
	_equip(P, 1, POWER_SPELL)
	data.magickas[P] = 0.0
	expect(casting.press(P, 1)["ok"], "cost 0")

func test_a_scroll_not_carried_is_unequipped() -> void:
	_equip(P, 1, SCROLL)
	var r := casting.press(P, 1)
	expect_eq(r["reason"], "not carried", "not carried")
	expect_eq(data.equipped_spell(P, 1), 0, "the hand is emptied")
	expect_eq(data.count("unequip"), 1, "unequipped once")
	_equip(P, 1, SCROLL)
	data.items["%d:%d" % [P, SCROLL]] = true
	data.magickas[P] = 0.0
	var ok := casting.press(P, 1)
	expect(ok["ok"] and ok["is_scroll"], "carried: pressed, no magicka needed")

func test_a_spell_scroll_needs_no_item() -> void:
	data.infos[0x104] = {"found": true, "is_scroll": true, "is_item": false, "casting_type": 1, "charge_time": 0.0}
	_equip(P, 1, 0x104)
	expect(casting.press(P, 1)["ok"], "a SPEL of scroll type")

func test_pressing_a_held_hand_is_a_refusal() -> void:
	_hold(1, FIREBOLT)
	casting.step(P, 1, 0.2)
	expect_eq(casting.press(P, 1)["reason"], "held", "held")
	expect(absf(float(casting.step(P, 1, 0.0)["progress"]) - 0.4) < 1e-6, "the state was not reset")

# ---- release and payment

func test_release_after_charge_pays_once_and_breaks_invisibility() -> void:
	_hold(1, FIREBOLT)
	casting.step(P, 1, 0.6)
	var r := casting.release(P, 1)
	expect(r["event"] == "cast" and r["paid"] and r["spell"] == FIREBOLT and not r["is_scroll"], "cast and paid: %s" % [r])
	expect_eq(r["cost"], 20.0, "the cost")
	expect_eq(data.count("pay"), 1, "paid once")
	expect_eq(data.count("break"), 1, "invisibility broken once")
	expect_eq(data.log[data.log.size() - 2], ["break", P], "broken before the payment")
	expect_eq(data.log[data.log.size() - 1], ["pay", P, FIREBOLT, 0.0], "paid for one cast")

func test_scroll_release_pays_nothing() -> void:
	data.items["%d:%d" % [P, SCROLL]] = true
	_hold(1, SCROLL)
	var r := casting.release(P, 1)
	expect(r["event"] == "cast" and r["paid"] and r["is_scroll"], "cast: %s" % [r])
	expect_eq(data.count("pay"), 0, "nothing paid")
	expect_eq(data.count("break"), 0, "invisibility stays")

func test_release_refused_for_magicka() -> void:
	_hold(1, FIREBOLT)
	casting.step(P, 1, 0.6)
	data.pay_refusal = "not enough magicka"
	var r := casting.release(P, 1)
	expect_eq(r["event"], "cast", "still the cast event")
	expect(not r["paid"], "not paid")
	expect_eq(r["reason"], "not enough magicka", "with its reason")
	expect(not casting.is_casting(P, 1), "the hand is removed")

# ---- concentration landings

func test_concentration_first_tick_and_landings_every_tenth() -> void:
	_hold(0, FLAMES)
	var first := casting.step(P, 0, 0.016)
	expect(first["event"] == "tick" and first["first"] and first["land"], "first tick lands: %s" % [first])
	var landings := 0
	var total := 0.0
	for dt in [0.016, 0.03, 0.05, 0.016, 0.07, 0.016, 0.2, 0.016, 0.016]:
		var s := casting.step(P, 0, dt)
		expect(not s["first"], "only the first tick is first")
		total += dt
		if s["land"]:
			landings += 1
	# the timer restarts at 0 on each landing: 0.016+0.03+0.05 (0.096) no, +0.016 (0.112) lands;
	# 0.07 no, +0.016 no, +0.2 lands (0.286); 0.016 no, 0.016 no.
	expect_eq(landings, 2, "landings across uneven dt")
	expect_eq(data.count("pay"), 10, "paid every step")
	expect_eq(data.count("break"), 0, "concentration does not break invisibility")
	expect(absf(float(casting.step(P, 0, 0.1)["cost"]) - 1.0) < 1e-6, "cost per step")

func test_concentration_of_a_scroll_never_lands() -> void:
	data.infos[0x105] = {"found": true, "is_scroll": true, "is_item": false, "casting_type": 2, "charge_time": 0.0}
	data.infos[0x105]["casting_type"] = Casting.CAST_CONCENTRATION
	_equip(P, 0, 0x105)
	expect(casting.press(P, 0)["ok"], "pressed")
	expect(not casting.step(P, 0, 0.2)["land"], "no landing for a scroll")

# ---- step guards

func test_step_on_dead_or_unequipped() -> void:
	_hold(0, FLAMES)
	data.dead[P] = true
	var s := casting.step(P, 0, 0.1)
	expect(s["event"] == "stop" and s["reason"] == "dropped", "dead while concentrating: stop")
	expect(not casting.is_casting(P, 0), "removed")
	data.dead.clear()
	_hold(1, FIREBOLT)
	data.equipped.erase("%d:%d" % [P, 1])
	expect_eq(casting.step(P, 1, 0.1)["event"], "", "unequipped while charging: no event")
	expect(not casting.is_casting(P, 1), "removed")
	_hold(0, FLAMES)
	data.equipped.erase("%d:%d" % [P, 0])
	expect_eq(casting.step(P, 0, 0.1)["event"], "stop", "unequipped while concentrating: stop")
	expect_eq(data.count("pay"), 0, "nothing paid")

func test_step_without_a_hand_does_nothing() -> void:
	var s := casting.step(P, 1, 0.1)
	expect_eq(s["event"], "", "no event")
	expect_eq(data.log.size(), 0, "no data asked")

# ---- two hands, two casters

func test_dual_cast_hands_are_independent() -> void:
	_hold(0, FIREBOLT)
	_hold(1, FIREBOLT)
	expect_eq(casting.hands_of(P).size(), 2, "two hands")
	casting.step(P, 0, 0.6)
	expect_eq(casting.phase_of(P, 0), "charged", "left charged")
	expect_eq(casting.phase_of(P, 1), "charging", "right still charging")
	casting.step(P, 1, 0.6)
	expect_eq(casting.release(P, 0)["event"], "cast", "left casts")
	expect_eq(casting.phase_of(P, 1), "charged", "right unaffected")
	expect_eq(casting.release(P, 1)["event"], "cast", "right casts")
	expect_eq(data.count("pay"), 2, "both paid")

func test_an_npc_beside_the_player() -> void:
	_hold(1, FIREBOLT)
	_hold(1, FIREBOLT, NPC)
	casting.step(NPC, 1, 0.6)
	expect_eq(casting.phase_of(P, 1), "charging", "the player is unaffected")
	var r := casting.release(NPC, 1)
	expect(r["event"] == "cast" and r["paid"], "the NPC casts")
	expect_eq(data.log[data.log.size() - 1], ["pay", NPC, FIREBOLT, 0.0], "paid by the NPC")
	expect(casting.is_casting(P, 1), "the player's hand is still down")
	casting.cancel_caster(NPC)
	expect_eq(casting.hands_of(NPC), [], "no hands")

# ---- power

func test_cast_power() -> void:
	expect_eq(casting.cast_power(P)["reason"], "no power", "no power")
	_equip(P, Casting.POWER, POWER_SPELL)
	data.dead[P] = true
	expect_eq(casting.cast_power(P)["reason"], "dead", "dead")
	data.dead.clear()
	var r := casting.cast_power(P)
	expect(r["ok"] and r["spell"] == POWER_SPELL and r["info"]["found"], "cast: %s" % [r])
	expect_eq(data.count("break"), 1, "invisibility broken")
	expect_eq(data.count("pay"), 1, "paid")
	data.pay_refusal = "cooldown"
	var refused := casting.cast_power(P)
	expect(not refused["ok"] and refused["reason"] == "cooldown", "refused: %s" % [refused])

# ---- pay

func test_pay_scroll_is_free() -> void:
	var r := casting.pay(P, SCROLL, data.infos[SCROLL])
	expect(r["ok"] and r["cost"] == 0.0, "free")
	expect_eq(data.log.size(), 0, "nothing asked")

# ---- shouts

func test_shout_thresholds_by_words() -> void:
	var three := casting.shout_begin(P, SHOUT)
	expect(three["ok"] and three["usable"] == 3 and absf(float(three["threshold"]) - 0.9) < 1e-6 and not three["fire"], "three words: %s" % [three])
	casting.shout_end(P)
	data.shouts[SHOUT]["usable"] = 2
	var two := casting.shout_begin(P, SHOUT)
	expect(absf(float(two["threshold"]) - 0.2) < 1e-6 and not two["fire"], "two words: %s" % [two])
	casting.shout_end(P)
	data.shouts[SHOUT]["usable"] = 1
	var one := casting.shout_begin(P, SHOUT)
	expect(one["ok"] and one["threshold"] == 0.0 and one["fire"], "one word fires at once: %s" % [one])
	expect(casting.shout_state(P)["fired"], "fired")
	var rel := casting.shout_release(P)
	expect(not rel["fire"], "release after firing says nothing")
	expect(not casting.is_shouting(P), "cleared")

func test_shout_words_by_held_time() -> void:
	data.shouts[SHOUT]["usable"] = 3
	expect_eq(casting.shout_words(P, SHOUT, 0.0), 1, "0 s: one")
	expect_eq(casting.shout_words(P, SHOUT, 0.19), 1, "just below fShoutTime1: one")
	expect_eq(casting.shout_words(P, SHOUT, 0.2), 2, "exactly fShoutTime1 is two")
	expect_eq(casting.shout_words(P, SHOUT, 0.5), 2, "0.5 s: two")
	expect_eq(casting.shout_words(P, SHOUT, 0.89), 2, "just below fShoutTime2: two")
	expect_eq(casting.shout_words(P, SHOUT, 0.9), 3, "exactly fShoutTime2 is three")
	expect_eq(casting.shout_words(P, SHOUT, 99.0), 3, "an NPC's long hold: three")
	data.gmsts["fShoutTime1"] = 0.6
	data.gmsts["fShoutTime2"] = 1.0
	expect_eq(casting.shout_words(P, SHOUT, 0.5), 1, "the GMSTs move the thresholds")
	expect_eq(casting.shout_words(P, SHOUT, 1.0), 3, "and the third")

func test_shout_words_are_limited_to_the_usable_ones() -> void:
	data.shouts[SHOUT]["usable"] = 2
	expect_eq(casting.shout_words(P, SHOUT, 5.0), 2, "three held, two usable")
	expect_eq(casting.shout_words(P, SHOUT, 0.0), 1, "one held")
	data.shouts[SHOUT]["usable"] = 1
	expect_eq(casting.shout_words(P, SHOUT, 5.0), 1, "one usable")
	expect_eq(casting.shout_words(P, 0x999, 5.0), 3, "unknown shout: the native refuses it")

func test_shout_gmst_overrides() -> void:
	data.gmsts["fShoutTime2"] = 1.5
	expect_eq(casting.shout_begin(P, SHOUT)["threshold"], 1.5, "GMST read")

func test_shout_fires_on_step_at_threshold() -> void:
	casting.shout_begin(P, SHOUT)
	var s := casting.shout_step(P, 0.5)
	expect(not s["fire"] and s["held"] == 0.5, "held so far")
	s = casting.shout_step(P, 1.0)
	expect(s["fire"] and absf(float(s["held"]) - 0.9) < 1e-6, "fires, held clamped: %s" % [s])
	expect(not casting.shout_step(P, 1.0)["fire"], "fires once")
	expect(casting.is_shouting(P), "still down until the key goes up")
	var rel := casting.shout_release(P)
	expect(not rel["fire"], "already said")
	expect(not casting.is_shouting(P), "cleared")

func test_shout_release_before_threshold_fires_with_held_so_far() -> void:
	casting.shout_begin(P, SHOUT)
	casting.shout_step(P, 0.4)
	var rel := casting.shout_release(P)
	expect(rel["fire"] and absf(float(rel["held"]) - 0.4) < 1e-6, "fires with 0.4: %s" % [rel])
	expect(not casting.is_shouting(P), "cleared")
	expect(not casting.shout_release(P)["fire"], "nothing when not shouting")

func test_shout_refusals_in_order() -> void:
	data.dead[P] = true
	data.recovery[P] = 5.0
	data.shouts[SHOUT]["usable"] = 0
	_equip(P, 1, FIREBOLT)
	expect_eq(casting.shout_begin(P, SHOUT)["reason"], "dead", "dead first")
	data.dead.clear()
	casting.press(P, 1)
	expect_eq(casting.shout_begin(P, SHOUT)["reason"], "casting", "then casting")
	casting.cancel(P, 1)
	expect_eq(casting.shout_begin(P, 0x999)["reason"], "unknown shout", "then unknown")
	expect_eq(casting.shout_begin(P, SHOUT)["reason"], "no word", "then no word, before recovering")
	data.shouts[SHOUT]["usable"] = 3
	expect_eq(casting.shout_begin(P, SHOUT)["reason"], "recovering", "then recovering")
	data.recovery.clear()
	expect(casting.shout_begin(P, SHOUT)["ok"], "begins")
	var again := casting.shout_begin(P, SHOUT)
	expect(again["ok"] and not again["fire"], "pressed again while shouting: no-op")
	expect_eq(casting.shout_state(P)["held"], 0.0, "state kept")

func test_press_refused_while_shouting_and_shout_refused_while_casting() -> void:
	_equip(P, 1, FIREBOLT)
	casting.shout_begin(P, SHOUT)
	expect_eq(casting.press(P, 1)["reason"], "shouting", "no cast while shouting")
	expect(casting.press(NPC, 1)["reason"] != "shouting", "another caster is not shouting")
	casting.shout_end(P)
	expect(casting.press(P, 1)["ok"], "pressed after the shout ended")
	expect_eq(casting.shout_begin(P, SHOUT)["reason"], "casting", "no shout while casting")
	expect_eq(casting.shout_begin(NPC, SHOUT)["ok"], true, "an NPC may shout")

func test_shout_state_is_a_copy() -> void:
	casting.shout_begin(P, SHOUT)
	var copy := casting.shout_state(P)
	copy["held"] = 99.0
	expect_eq(casting.shout_state(P)["held"], 0.0, "the copy is detached")
	expect_eq(casting.shout_state(NPC), {}, "no state for another caster")

# ---- info cache and lifecycle

func test_info_is_cached() -> void:
	casting.info_of(FIREBOLT)
	data.infos[FIREBOLT] = {"found": false}
	expect(casting.info_of(FIREBOLT)["found"], "cached")
	casting.clear_info_cache()
	expect(not casting.info_of(FIREBOLT)["found"], "cleared")

func test_reset_drops_everything_but_the_cache() -> void:
	_hold(1, FIREBOLT)
	_hold(1, FIREBOLT, NPC)
	casting.shout_begin(0x777, SHOUT)
	var before := data.log.size()
	casting.reset()
	expect(not casting.is_casting(P, 1) and not casting.is_casting(NPC, 1), "no hands")
	expect(not casting.is_shouting(0x777), "no shout")
	expect_eq(data.log.size(), before, "silent: nothing asked of the data")
	data.infos[FIREBOLT] = {"found": false}
	expect(casting.info_of(FIREBOLT)["found"], "the cache stays")

func test_on_place_left_keeps_the_player() -> void:
	_hold(1, FIREBOLT)               # the player's hand, an NPC's shout
	casting.shout_begin(NPC, SHOUT)
	var before := data.log.size()
	casting.on_place_left()
	expect(casting.is_casting(P, 1), "the player's hand stays")
	expect(not casting.is_shouting(NPC), "the NPC's shout is gone")
	expect_eq(data.log.size(), before, "silent")
	casting.reset()
	_hold(1, FIREBOLT, NPC)          # an NPC's hand, the player's shout
	casting.shout_begin(P, SHOUT)
	casting.on_place_left()
	expect(not casting.is_casting(NPC, 1), "the NPC's hand is gone")
	expect(casting.is_shouting(P), "the player's shout stays")
