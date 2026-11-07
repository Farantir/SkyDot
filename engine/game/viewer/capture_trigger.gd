# SPDX-License-Identifier: GPL-3.0-or-later
#
# skycompare's player triggers (--trigger cast:left|right|dual|shout, attack:left|right|dual|bash) with
# --hold auto|tap|SECONDS: the player's buttons go down and up as a finger would press them, with
# whatever the hands and the voice slot hold (the shot's "player" block put it there). The game job of
# skycompare (SkyrimRemote's @press and @action) does the same in Skyrim; both resolve `auto` alike:
# a concentration spell and a block are held for the whole capture (and TAIL more), a charged spell
# until it is charged (its charge time and a quarter second), a weapon attack and a shout are tapped
# (one frame down). A number holds that long (a charge, a power attack, more words of a shout).
class_name CaptureTrigger
extends RefCounted

## Seconds a bash's block is up before the attack button goes down, and stays up after it.
const BASH_LEAD := 0.3
## Seconds held past a charge time, so the spell is surely charged when the button goes up.
const CHARGE_MARGIN := 0.25
## Frames to wait for the hands to be drawn before pressing anyway.
const READY_FRAMES := 300
## Seconds a held (auto) concentration spell or block stays down past the capture, so its last frame is
## still held (skycompare's HOLD_TAIL).
const TAIL := 0.5

var _session: GameSession
var _trigger: String
var _hold: String
var _cap: float
var _presses: Array[Dictionary] = []  # {button, start, seconds, down: bool, up: bool, held}
var _time := 0.0
var _waited := 0


func _init(session: GameSession, trigger: String, hold: String, cap: float) -> void:
	_session = session
	_trigger = trigger
	_hold = hold
	_cap = cap + TAIL


## Whether `trigger` is one of the player triggers this class presses.
static func handles(trigger: String) -> bool:
	return trigger in ViewerSettings.TRIGGERS


## True once the hands are drawn (spells ready, weapon drawn), or after READY_FRAMES frames.
func is_ready() -> bool:
	if _trigger == "cast:shout":
		return true
	_waited += 1
	var magic := _session.magic
	var spells := magic.hands_with_spells()
	var spells_ok := spells.is_empty() or magic.ready_state() == Magic.Ready.READY
	var weapon_ok := (_session.combat.player_weapon() == 0 and not spells.is_empty()) \
		or _session.combat.weapon_state() == Combat.State.DRAWN
	return (spells_ok and weapon_ok) or _waited > READY_FRAMES


## The buttons go down (the first frame of the series): the presses are planned. Returns a log line.
func fire() -> String:
	match _trigger:
		"cast:right", "attack:right":
			_plan("right", 0.0, _seconds_for(Magic.RIGHT))
		"cast:left", "attack:left":
			_plan("left", 0.0, _seconds_for(Magic.LEFT))
		"cast:dual", "attack:dual":
			_plan("left", 0.0, _seconds_for(Magic.LEFT))
			_plan("right", 0.0, _seconds_for(Magic.RIGHT))
		"cast:shout":
			_plan("shout", 0.0, _held(0.0))
		"attack:bash":
			var strike := _held(0.0)
			_plan("left", 0.0, BASH_LEAD * 2.0 + strike)
			_plan("right", BASH_LEAD, strike)
	var parts: PackedStringArray = []
	for press in _presses:
		parts.append("%s %.2f s from %.2f s" % [press["button"], press["seconds"], press["start"]])
	var papyrus := _session.papyrus
	var player := SkydotPapyrus.PLAYER_REF
	var costs: PackedStringArray = []
	for hand in [Magic.LEFT, Magic.RIGHT]:
		if _session.magic.spell_in(hand) != 0:
			costs.append("%.0f" % papyrus.spell_cost(player, _session.magic.spell_in(hand)))
	var line := "%s (hold %s): %s; hands %s, weapon %s; Magicka %.0f/%.0f, spell cost %s" % [_trigger, _hold,
		", ".join(parts), Magic.Ready.keys()[_session.magic.ready_state()], _session.combat.state_name(),
		_session.actor_value(player, "Magicka"), _session.actor_value_max(player, "Magicka"), "/".join(costs)]
	step(0.0)
	return line


## Every frame of the series after `fire`: buttons go down at their start and up after their seconds
## (at least one frame later).
func step(delta: float) -> void:
	_time += delta
	for press in _presses:
		if not press["down"] and _time >= press["start"]:
			press["down"] = true
			press["at"] = _time
			_button(press["button"], true)
		elif press["down"] and not press["up"] and _time > press["at"] and _time - press["at"] >= press["seconds"]:
			press["up"] = true
			_button(press["button"], false)


func _plan(button: String, start: float, seconds: float) -> void:
	_presses.append({"button": button, "start": start, "seconds": seconds, "down": false, "up": false, "at": 0.0})


## How long the attack button of `hand` is held: `--hold` as given, or by what the hand holds (auto).
func _seconds_for(hand: int) -> float:
	if _hold != "auto":
		return _held(0.0)
	var spell := _session.magic.spell_in(hand)
	if spell != 0:
		var info := _session.magic.info_of(spell)
		if int(info.get("casting_type", 1)) == Magic.CAST_CONCENTRATION:
			return _cap
		return float(info.get("charge_time", 0.0)) + CHARGE_MARGIN
	if hand == Magic.LEFT:
		var left := int(_session.equipment().get("left", 0))
		var category := String(_session.papyrus.item_info(left).get("category", "")) if left != 0 else ""
		if left == 0 or category == "armor":
			return _cap  # a block (a shield or bare hands) is held
	return 0.0


## --hold as seconds: tap is 0, auto is `fallback`.
func _held(fallback: float) -> float:
	if _hold == "tap":
		return 0.0
	if _hold == "auto":
		return fallback
	return maxf(_hold.to_float(), 0.0)


func _button(button: String, down: bool) -> void:
	match button:
		"right":
			if down:
				_session.attack_pressed()
			else:
				_session.attack_released()
		"left":
			if down:
				_session.attack_left_pressed()
			else:
				_session.attack_left_released()
		"shout":
			if down:
				print("--trigger: voice key down: ", _session.voice_pressed())
			else:
				_session.voice_released()
	print("--trigger: %s %s at %.3f s" % [button, "down" if down else "up", _time])
