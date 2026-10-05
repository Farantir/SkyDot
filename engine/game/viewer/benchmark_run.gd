# SPDX-License-Identifier: GPL-3.0-or-later
#
# --benchmark SECONDS: the camera flies east at --fly-speed m/s while frame
# times are collected; the statistics print when the time is up.
class_name BenchmarkRun
extends RefCounted

var _seconds_left := 0.0
var _fly_speed := 20.0  # m/s
var _frame_times: Array[float] = []  # ms


## Start (the viewer calls frame every frame from now on).
func begin(seconds: float, fly_speed: float) -> void:
	_seconds_left = seconds
	_fly_speed = fly_speed


func is_running() -> bool:
	return _seconds_left > 0.0


## One frame: fly on and note the time. Returns true when the run is over,
## after printing its statistics. `slowest_streaming_usec` is the streamer's.
func frame(delta: float, player: SkydotPlayer, slowest_streaming_usec: int) -> bool:
	_frame_times.append(delta * 1000.0)
	player.teleport(player.global_position + Vector3(_fly_speed * delta, 0, 0))
	_seconds_left -= delta
	if _seconds_left > 0.0:
		return false
	var sorted := _frame_times.duplicate()
	sorted.sort()
	var pick := func(q: float) -> float: return sorted[int(q * (sorted.size() - 1))]
	print("benchmark: %d frames, median %.1f ms, p99 %.1f ms, max %.1f ms, over 33 ms: %d, slowest streaming step %.1f ms"
		% [sorted.size(), pick.call(0.5), pick.call(0.99), sorted[-1],
		   sorted.filter(func(t: float) -> bool: return t > 33.0).size(), slowest_streaming_usec / 1000.0])
	return true
