# SPDX-License-Identifier: GPL-3.0-or-later
#
# The pack the session opened, for readers without a session at hand: the menus' assets (ui/ui_assets.gd listens)
# and the control map. GameSession hands it over with `use` when it opens one.
class_name PackFiles
extends RefCounted

static var pack: SkydotPack
static var _listeners: Array[Callable] = []


## Hand over the pack the session opened (null forgets it); the listeners hear of a new one.
static func use(next: SkydotPack) -> void:
	if next == pack:
		return
	pack = next
	for listener: Callable in _listeners:
		listener.call(next)


## `listener` (pack: SkydotPack) hears of each pack handed over from now on.
static func listen(listener: Callable) -> void:
	_listeners.append(listener)


## Member `member` of the ZIP asset at `path`; empty without a pack or that file.
static func zip_file(path: String, member: String) -> PackedByteArray:
	return pack.get_zip_file(path, member) if pack != null else PackedByteArray()
