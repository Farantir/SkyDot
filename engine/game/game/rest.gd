# SPDX-License-Identifier: GPL-3.0-or-later
#
# Waiting and sleeping, the Sleep/Wait menu's rules (ui/sleep_wait_view.gd draws and times the menu):
#   refusal()   why the player may not rest now, as the game words it (GMSTs sNoWait* / sNoSleep*): enemies fight
#               the player, the player is in the air, a bed someone else owns (vm/ownership.hpp: the bed's owner, an
#               inn's rented bed is the player's through RentRoomScript's SetActorOwner)
#   begin()     the clock stands still between the hours (in the game no time passes between them); a sleep sends
#               OnSleepStart to the scripts that called RegisterForSleep
#   hour()      one hour passes: the clock (the AI and the weather follow it), the actors recover as two hours of
#               play would let them (RECOVERY_PER_HOUR, measured), the journal's Hours Waiting / Hours Slept count
#   finish()    the clock runs again; a sleep sends OnSleepStop (PlayerSleepQuest gives Rested or Well Rested)
# Not checked (no system for them yet): trespassing, being asked to leave, guards pursuing, taking health damage,
# cells the player may not wait in.
class_name Rest
extends RefCounted

## The game's messages and their English text (Skyrim.esm), as the sNoWait/sNoSleep GMSTs.
const FALLBACK := {
	"sNoWaitHostileActorsNear": "You cannot wait when enemies are nearby.",
	"sNoSleepHostileActorsNear": "You cannot sleep when enemies are nearby.",
	"sNoWaitInAir": "You cannot wait in the air.",
	"sNoSleepInAir": "You cannot sleep in the air.",
	"sNoSleepInOwnedBed": "You cannot sleep in an owned bed.",
}

## An hour of a wait or a sleep lets the actors regenerate as RECOVERY_PER_HOUR / time scale seconds of play would
## (the real time two game hours take). Measured with SkyrimRemote
## (2026-11-18, HealRate 0.01 and 0.1: +3.6 Health an hour at time scale 20, +7.2 at 10; 0.01 a second in play).
const RECOVERY_PER_HOUR := 7200.0
## A furniture marker's animation type for sleeping (BSFurnitureMarker: 1 sit, 2 sleep, 4 lean).
const SLEEP_MARKER := 2

var _session: GameSession
## The actors fighting the player (SkydotAi's combat state, target the player).
var _foes := {}
var _sleeping := false
var _resting := false
var _time_scale := 0.0


func _init(session: GameSession) -> void:
	_session = session


## Whether a built furniture model is a bed, as the game tells one: one of its furniture markers (the NIF's
## BSFurnitureMarkerNode, the converter's `furniture_markers` extra) is for sleeping (animation type 2; 1 sits,
## 4 leans). The game's beds carry no keyword that says so.
static func is_bed(model: Node, depth := 0) -> bool:
	if model == null:
		return false
	if model.has_meta("extras"):
		var extras: Variant = model.get_meta("extras")
		var block: Variant = extras.get("bethconv", {}) if extras is Dictionary else {}
		var markers: Variant = block.get("furniture_markers") if block is Dictionary else null
		if markers is Array:
			for marker: Variant in markers:
				if marker is Dictionary and int(marker.get("animation_type", 0)) == SLEEP_MARKER:
					return true
			return false
	if depth < 3:
		for child in model.get_children():
			if is_bed(child, depth + 1):
				return true
	return false


## A load: nobody fights the player any more.
func reset() -> void:
	_foes.clear()


func is_resting() -> bool:
	return _resting


## An actor's combat state toward `target` changed (0 none): the foes of the player.
func on_combat_state(ref: int, target: int, state: int) -> void:
	if target == SkydotPapyrus.PLAYER_REF and state != 0:
		_foes[ref] = true
	else:
		_foes.erase(ref)


## The message that refuses a rest (a wait, or a sleep in `bed`), or "" when the player may rest.
func refusal(sleeping: bool, bed := 0) -> String:
	var prefix := "sNoSleep" if sleeping else "sNoWait"
	for foe: int in _foes.keys():
		if _session.papyrus.is_actor_dead(foe):
			_foes.erase(foe)
	if not _foes.is_empty():
		return _text(prefix + "HostileActorsNear")
	if _session.player_in_air():
		return _text(prefix + "InAir")
	if sleeping and bed != 0:
		var owner := _session.papyrus.get_owner(bed)
		if int(owner["owner"]) != 0 and not owner["player"]:
			return _text("sNoSleepInOwnedBed")
	return ""


## The rest of `hours` begins.
func begin(sleeping: bool, hours: int) -> void:
	_resting = true
	_sleeping = sleeping
	_time_scale = _session.clock.get_time_scale()
	_session.clock.set_time_scale(0.0)
	if sleeping:
		var now := _session.clock.get_game_days()
		_session.papyrus.sleep_start(now, now + hours / 24.0)


## One hour of the rest passes.
func hour() -> void:
	_session.clock.shift(1.0)
	if _time_scale > 0.0:
		_session.papyrus.recover(RECOVERY_PER_HOUR / _time_scale)
	if _session.journal != null:
		_session.journal.count("Hours Slept" if _sleeping else "Hours Waiting")


## Whether a rest under way must stop: an enemy began fighting the player.
func interrupted() -> bool:
	return _resting and not _foes.is_empty()


## The rest is over (`interrupted`: before its hours were done).
func finish(interrupted_early: bool) -> void:
	if not _resting:
		return
	_resting = false
	_session.clock.set_time_scale(_time_scale)
	if _sleeping:
		_session.papyrus.sleep_stop(interrupted_early)


func _text(setting: String) -> String:
	return _session.world.get_gmst_string(setting, FALLBACK.get(setting, ""))
