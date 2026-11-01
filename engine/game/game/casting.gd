# SPDX-License-Identifier: GPL-3.0-or-later
#
# The rules of the hands (ARCHITECTURE.md 8.2, ADR 0009), for any caster, the player or an NPC.
# It owns, per (caster, hand), the attack button on a spell or scroll (charging, charged,
# concentrating) and, per caster, the voice key on a shout. It plays no clip, shows no art and
# delivers nothing: `Magic` and `NpcMagic` turn the Dictionaries it returns into that. Data comes
# from a CastingData (the tests give it a fake); no signals, no calls to Papyrus but through it.
#
# A hand: press (refused when the hand holds nothing, the caster is dead or shouts, a scroll is
# no longer carried, a power cools down, Magicka is short), step (fire and forget: "charged"
# once the casting time is held; concentration: pay `dt` seconds, "tick", with `land` every
# TICK_SECONDS and `first` on the first, "stop" when the payer refuses), release (a charged
# fire-and-forget hand "cast"s and is paid; a concentration hand "stop"s; released early:
# nothing, nothing paid). `cast_power` pays a power at once. A shout: `shout_begin` (refused
# while casting, recovering, with no word), `shout_step` (fires at the threshold), `shout_release`.
#
# A script mod extends this class and overrides a rule method (`refusal`, `charge_time`,
# `shout_threshold`, `pay`), or replaces the class whole (GameSession.casting_script).
# Lifecycle, silent: `reset()` drops every hand and shout, `on_place_left()` those of every
# caster but the player. The cast_info cache stays.
class_name Casting
extends RefCounted

const LEFT := 0
const RIGHT := 1
const POWER := 2
const PLAYER_REF := 0x14
const TICK_SECONDS := 0.1
## cast_info "casting_type" of a concentration spell.
const CAST_CONCENTRATION := 2
const GMST_SHOUT_TIME1 := "fShoutTime1"
const GMST_SHOUT_TIME2 := "fShoutTime2"
const SHOUT_TIME1_FALLBACK := 0.2
const SHOUT_TIME2_FALLBACK := 0.9

var _data: CastingData
var _hands := {}   # caster -> {hand -> {spell, info, conc, scroll, charge, held, charged, started, timer}}
var _shouts := {}  # caster -> {shout, held, usable, threshold, fired}
var _info_cache := {}


func _init(data: CastingData = null) -> void:
	_data = data


# ---- the hands

## `caster` pressed the attack button of `hand`.
## {ok, reason, spell, info, concentration, is_scroll}; the reason of a refusal is "held",
## "shouting", or what `refusal` says.
func press(caster: int, hand: int) -> Dictionary:
	var out := {"ok": false, "reason": "", "spell": 0, "info": {}, "concentration": false, "is_scroll": false}
	if is_casting(caster, hand):
		out["reason"] = "held"
		return out
	if is_shouting(caster):
		out["reason"] = "shouting"
		return out
	var spell := _data.equipped_spell(caster, hand)
	var info := info_of(spell) if spell != 0 else {}
	out["spell"] = spell
	out["info"] = info
	var reason := refusal(caster, hand, spell, info)
	if reason != "":
		out["reason"] = reason
		return out
	var conc := int(info.get("casting_type", 0)) == CAST_CONCENTRATION
	var scroll := bool(info.get("is_scroll", false))
	if not _hands.has(caster):
		_hands[caster] = {}
	_hands[caster][hand] = {"spell": spell, "info": info, "conc": conc, "scroll": scroll,
		"charge": charge_time(caster, info), "held": 0.0, "charged": false, "started": false,
		"timer": TICK_SECONDS}
	out["ok"] = true
	out["concentration"] = conc
	out["is_scroll"] = scroll
	return out


## Why `caster` cannot start `spell` (cast_info `info`) from `hand`; "" if it can. In order: no
## spell, dead, a scroll no longer carried (its hand is emptied), a power cooling down, too
## little Magicka (a concentration spell needs some, the others its cost).
func refusal(caster: int, hand: int, spell: int, info: Dictionary) -> String:
	if spell == 0 or not bool(info.get("found", false)):
		return "no spell"
	if _data.is_dead(caster):
		return "dead"
	_data.prepare(caster)
	if bool(info.get("is_scroll", false)):
		if bool(info.get("is_item", false)) and not _data.carried(caster, spell):
			_data.unequip_spell(caster, hand)
			return "not carried"
		return ""
	if _data.power_cooldown(caster, spell) > 0.0:
		return "cooldown"
	var magicka := _data.magicka(caster)
	if int(info.get("casting_type", 0)) == CAST_CONCENTRATION:
		if magicka <= 0.0:
			return "not enough magicka"
	else:
		var cost := _data.spell_cost(caster, spell)
		if cost > 0.0 and magicka < cost:
			return "not enough magicka"
	return ""


## Seconds a fire-and-forget spell is held before it can be released.
func charge_time(_caster: int, info: Dictionary) -> float:
	return float(info.get("charge_time", 0.0))


## Advance a held hand by `dt` seconds. {event: ""|"charged"|"tick"|"stop", land, first, cost,
## progress, spell, info, reason}. "tick": the time was paid (`cost`); `land` says the effects
## land again (every TICK_SECONDS, never for a scroll), `first` that the concentration started.
## "stop": reason "out of magicka" (the payer refused) or "dropped" (the spell was put away or
## the caster died); the hand is removed.
func step(caster: int, hand: int, dt: float) -> Dictionary:
	var out := {"event": "", "land": false, "first": false, "cost": 0.0, "progress": 0.0,
		"spell": 0, "info": {}, "reason": ""}
	var state := _state(caster, hand)
	if state.is_empty():
		return out
	var spell := _data.equipped_spell(caster, hand)
	out["spell"] = spell
	out["info"] = state["info"]
	if spell == 0 or _data.is_dead(caster):
		if bool(state["conc"]):
			out["event"] = "stop"
			out["reason"] = "dropped"
		cancel(caster, hand)
		return out
	if dt <= 0.0:
		out["progress"] = _progress(state)
		return out
	if bool(state["conc"]):
		var paid := _data.pay_spell(caster, spell, dt)
		if not bool(paid.get("ok", false)):
			out["event"] = "stop"
			out["reason"] = "out of magicka"
			cancel(caster, hand)
			return out
		out["cost"] = float(paid.get("cost", 0.0))
		state["held"] += dt
		out["event"] = "tick"
		if not state["started"]:
			state["started"] = true
			out["first"] = true
		state["timer"] += dt
		if state["timer"] >= TICK_SECONDS:
			state["timer"] = 0.0
			out["land"] = not bool(state["scroll"])
	else:
		state["held"] += dt
		if not state["charged"] and state["held"] >= state["charge"]:
			state["charged"] = true
			out["event"] = "charged"
		out["progress"] = _progress(state)
	return out


## The button went up. {event: "cast"|"stop"|"", seconds, spell, info, is_scroll, paid, reason,
## cost}. "cast": charged in time, paid through `pay` (`paid`, `reason`, `cost`; if it is
## false nothing is cast); "stop": a concentration spell ended; "": released early or no hand.
func release(caster: int, hand: int) -> Dictionary:
	var out := {"event": "", "seconds": 0.0, "spell": 0, "info": {}, "is_scroll": false,
		"paid": false, "reason": "", "cost": 0.0}
	var state := _state(caster, hand)
	if state.is_empty():
		return out
	cancel(caster, hand)
	out["seconds"] = state["held"]
	out["spell"] = state["spell"]
	out["info"] = state["info"]
	out["is_scroll"] = state["scroll"]
	if bool(state["conc"]):
		out["event"] = "stop"
	elif state["held"] >= state["charge"]:
		out["event"] = "cast"
		var paid := pay(caster, int(state["spell"]), state["info"])
		out["paid"] = paid["ok"]
		out["reason"] = paid["reason"]
		out["cost"] = paid["cost"]
	return out


## Pay for one fire-and-forget cast of `spell` (cast_info `info`): a scroll is free; any other
## ends the caster's invisibility and pays its cost. {ok, reason, cost}.
func pay(caster: int, spell: int, info: Dictionary) -> Dictionary:
	if bool(info.get("is_scroll", false)):
		return {"ok": true, "reason": "", "cost": 0.0}
	_data.break_invisibility(caster)
	var paid := _data.pay_spell(caster, spell, 0.0)
	return {"ok": bool(paid.get("ok", false)), "reason": String(paid.get("reason", "")),
		"cost": float(paid.get("cost", 0.0))}


## Cast the power in `caster`'s power slot now. {ok, reason, spell, info}.
func cast_power(caster: int) -> Dictionary:
	var spell := _data.equipped_spell(caster, POWER)
	var out := {"ok": false, "reason": "", "spell": spell, "info": {}}
	if spell == 0:
		out["reason"] = "no power"
		return out
	if _data.is_dead(caster):
		out["reason"] = "dead"
		return out
	var info := info_of(spell)
	out["info"] = info
	var paid := pay(caster, spell, info)
	out["ok"] = paid["ok"]
	out["reason"] = paid["reason"]
	return out


## Drop the hand without an event.
func cancel(caster: int, hand: int) -> void:
	if not _hands.has(caster):
		return
	_hands[caster].erase(hand)
	if _hands[caster].is_empty():
		_hands.erase(caster)


## Drop every hand and the shout of `caster`, silently.
func cancel_caster(caster: int) -> void:
	_hands.erase(caster)
	_shouts.erase(caster)


func is_casting(caster: int, hand: int) -> bool:
	return not _state(caster, hand).is_empty()


## "" (idle), "charging", "charged" (fire and forget, held long enough) or "concentrating".
func phase_of(caster: int, hand: int) -> String:
	var state := _state(caster, hand)
	if state.is_empty():
		return ""
	if bool(state["conc"]):
		return "concentrating"
	return "charged" if state["charged"] else "charging"


## The spell or scroll held in `hand` since the press; 0 if the hand is idle.
func spell_of(caster: int, hand: int) -> int:
	return int(_state(caster, hand).get("spell", 0))


## The hands of `caster` that are down.
func hands_of(caster: int) -> Array:
	return (_hands[caster] as Dictionary).keys() if _hands.has(caster) else []


# ---- the voice

## The voice key went down on `shout`. {ok, reason, usable, threshold, fire}; `fire` says one
## word is usable, so it is said now (the state is `fired`). Refused: "dead", "casting" (a hand
## is down), "unknown shout", "no word", "recovering". Pressed again while shouting: ok, no-op.
func shout_begin(caster: int, shout: int) -> Dictionary:
	var out := {"ok": false, "reason": "", "usable": 0, "threshold": 0.0, "fire": false}
	if is_shouting(caster):
		out["ok"] = true
		return out
	if _data.is_dead(caster):
		out["reason"] = "dead"
		return out
	if _hands.has(caster):
		out["reason"] = "casting"
		return out
	var info := _data.shout_info(caster, shout)
	if not bool(info.get("found", false)):
		out["reason"] = "unknown shout"
		return out
	var usable := int(info.get("usable", 0))
	out["usable"] = usable
	if usable <= 0:
		out["reason"] = "no word"
		return out
	if _data.voice_recovery(caster) > 0.0:
		out["reason"] = "recovering"
		return out
	var threshold := shout_threshold(caster, usable)
	var fire := usable == 1
	_shouts[caster] = {"shout": shout, "held": 0.0, "usable": usable, "threshold": threshold, "fired": fire}
	out["ok"] = true
	out["threshold"] = threshold
	out["fire"] = fire
	return out


## Seconds the voice key charges before it says `usable` words: fShoutTime2 for three, fShoutTime1
## for two, none for one.
func shout_threshold(_caster: int, usable: int) -> float:
	if usable >= 3:
		return _data.gmst_float(GMST_SHOUT_TIME2, SHOUT_TIME2_FALLBACK)
	if usable == 2:
		return _data.gmst_float(GMST_SHOUT_TIME1, SHOUT_TIME1_FALLBACK)
	return 0.0


## Advance the key by `dt`. {fire, held}: `fire` once, when `held` (clamped) reaches the threshold.
func shout_step(caster: int, dt: float) -> Dictionary:
	var state: Dictionary = _shouts.get(caster, {})
	if state.is_empty() or state["fired"]:
		return {"fire": false, "held": float(state.get("held", 0.0))}
	state["held"] += dt
	if state["held"] >= state["threshold"]:
		state["held"] = state["threshold"]
		state["fired"] = true
		return {"fire": true, "held": state["held"]}
	return {"fire": false, "held": state["held"]}


## The key went up: {fire, held}, `fire` if nothing was said yet (the words held so far); clears.
func shout_release(caster: int) -> Dictionary:
	var state: Dictionary = _shouts.get(caster, {})
	_shouts.erase(caster)
	if state.is_empty():
		return {"fire": false, "held": 0.0}
	return {"fire": not state["fired"], "held": state["held"]}


## Drop the shout without a word.
func shout_end(caster: int) -> void:
	_shouts.erase(caster)


func is_shouting(caster: int) -> bool:
	return _shouts.has(caster)


## {shout, held, usable, threshold, fired} (a copy); {} if the key is not down.
func shout_state(caster: int) -> Dictionary:
	return (_shouts[caster] as Dictionary).duplicate() if _shouts.has(caster) else {}


# ---- data and lifecycle

## cast_info of `spell`, cached ({} if unknown).
func info_of(spell: int) -> Dictionary:
	if not _info_cache.has(spell):
		_info_cache[spell] = _data.cast_info(spell)
	return _info_cache[spell]


func clear_info_cache() -> void:
	_info_cache.clear()


## A save was loaded or the game restarted: every hand and shout goes. Silent.
func reset() -> void:
	_hands.clear()
	_shouts.clear()


## The place was left: the hands and shouts of every caster but the player go. Silent.
func on_place_left() -> void:
	for caster in _hands.keys():
		if caster != PLAYER_REF:
			_hands.erase(caster)
	for caster in _shouts.keys():
		if caster != PLAYER_REF:
			_shouts.erase(caster)


func _state(caster: int, hand: int) -> Dictionary:
	if not _hands.has(caster):
		return {}
	return _hands[caster].get(hand, {})


func _progress(state: Dictionary) -> float:
	var charge: float = state["charge"]
	return 1.0 if charge <= 0.0 else minf(1.0, state["held"] / charge)
