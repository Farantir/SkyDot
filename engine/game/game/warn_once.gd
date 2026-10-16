# SPDX-License-Identifier: GPL-3.0-or-later
#
# A message printed once per key: the game layer's classes report a missing
# model or sound the first time they meet it, not on every frame after.
class_name WarnOnce
extends RefCounted

var _prefix: String
var _as_warning: bool
var _seen := {}


## `prefix` starts every message; `as_warning` sends it to push_warning
## instead of the log.
func _init(prefix := "", as_warning := false) -> void:
	_prefix = prefix
	_as_warning = as_warning


func once(key: String, text: String) -> void:
	if _seen.has(key):
		return
	_seen[key] = true
	if _as_warning:
		push_warning(_prefix + text)
	else:
		print(_prefix, text)
