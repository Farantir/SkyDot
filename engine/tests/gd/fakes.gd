# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stand-ins for the native objects the Game layer takes. They are not
# subclasses: a GDScript class cannot override a method of a native class (parse
# error), and typed calls would bypass the override anyway. A native object that
# was never set up is the fake: SkydotPapyrus has all its signals (emit them
# with emit_signal) and answers queries with neutral defaults, SkydotWorld is
# an empty world (find_ref_node returns null).
extends RefCounted

static func papyrus() -> SkydotPapyrus:
    return SkydotPapyrus.new()

static func world() -> SkydotWorld:
    return SkydotWorld.new()
