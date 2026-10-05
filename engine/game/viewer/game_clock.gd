# SPDX-License-Identifier: GPL-3.0-or-later
#
# The time of day, asked of one place. It has two owners that take turns:
# outside, SkydotWeather runs it (sky, light, weather); inside, SkydotAi's own
# clock does. The clock hands the time from one to the other as places are
# left and entered, and every frame (sync), so the AI always knows the hour
# and a place is entered at the hour the last one was left at.
class_name GameClock
extends RefCounted

## Game seconds per second (--time-scale); 0 stops time.
var scale := 20.0
## The weather running outside; null inside. Bind it once it is set up.
var weather: SkydotWeather
var _ai: SkydotAi  # null with --ai off
var _hour := 12.0  # kept while inside, from the AI
var _day := 0
var _still := false  # screenshots and benchmarks: time stands still

## The hour of the day (0 to 24) and the day count, wherever time runs now.
var hour: float:
	get: return weather.hour if has_weather() else _hour
var day: int:
	get: return weather.day if has_weather() else _day


func _init(start_hour: float, time_scale: float, still: bool) -> void:
	_hour = start_hour
	scale = time_scale
	_still = still


## Inside, `ai` keeps the time; it starts at this clock's.
func attach_ai(ai: SkydotAi) -> void:
	_ai = ai
	ai.days = _day + _hour / 24.0
	ai.time_scale = 0.0 if _still else scale


func has_weather() -> bool:
	return weather != null and is_instance_valid(weather)


## Give a weather about to be set up the hour, day and speed.
func configure(node: SkydotWeather) -> void:
	node.hour = _hour
	node.day = _day
	node.time_scale = 0.0 if _still else scale


## `node` (configured, set up) now runs the time outside.
func bind_weather(node: SkydotWeather) -> void:
	weather = node


## The weather's place is left: its time is kept for the next one.
func release_weather() -> void:
	if has_weather():
		_hour = weather.hour
		_day = weather.day
	weather = null


## Every frame before the AI updates. Outside the weather keeps the time and
## the AI follows it; inside the AI's clock runs (stopped during a benchmark).
func sync(benchmarking: bool) -> void:
	if _ai == null:
		return
	if has_weather():
		_ai.time_scale = 0.0
		_ai.days = weather.day + weather.hour / 24.0
	else:
		_ai.time_scale = 0.0 if benchmarking else scale
		_hour = _ai.get_hour()
		_day = int(_ai.days)


## Move the time by `hours`. Returns "HH:MM" for the new time, or "" when
## nothing keeps the time (inside, with --ai off).
func shift(hours: float) -> String:
	if weather != null:
		weather.hour = fmod(weather.hour + hours + 24.0, 24.0)
		return _text(weather.hour)
	if _ai != null:
		_ai.days = maxf(0.0, _ai.days + hours / 24.0)
		_hour = _ai.get_hour()
		return _text(_hour)
	return ""


## The time for a shot's JSON: hour, day and, outside, the speed.
func describe() -> Dictionary:
	if has_weather():
		return {"hour": weather.hour, "day": weather.day, "time_scale": weather.time_scale}
	return {"hour": _hour, "day": _day}  # inside: kept for when one leaves


func _text(at: float) -> String:
	return "%02d:%02d" % [int(at), int(fmod(at, 1.0) * 60)]
