# SPDX-License-Identifier: GPL-3.0-or-later
#
# What a GameSession is opened with: the pack, what is built, how the world
# streams, the time, and the runs that drive themselves. Defaults are the
# viewer's. A shell fills it from its own options (the viewer: ViewerSettings.
# game_config()); a test sets the few fields it cares about.
class_name GameConfig
extends RefCounted

## The pack directory.
var pack := ""

# What is built.
var materials := true
var effects := true
var grass := true
var all_light_shadows := false  # every placed light casts shadows
var collision := true
var navigation := true
var actors := true
var wander := true
var ai := true
var combat := true  # actors that see an enemy fight (SkydotAi.set_combat)
var has_tiling := false
var tiling := 0.0  # land texture repeats per cell
var foliage_bias := 0.5  # mip levels blurrier for alpha-tested trees and grass
var shadows := true  # the sun's, outside

# How the world streams.
var radius := 2  # cells around the viewpoint built in full
var build_budget_usec := 8000  # per frame for streaming cells in
var lod := true
var lod_split := 1.5
var large_refs := true
var large_ref_radius := 5
var has_tree_distance := false
var tree_distance := 0.0
var preload_doors := true  # build the place behind a near load door ahead
var preload_distance := 15.0  # metres

# Time and weather.
var time := 12.0  # hour to start at
var time_scale := 20.0  # game seconds per second
var still := false  # time stands still (screenshots and benchmarks)
var weather := ""  # keep this one; empty: they take turns

# Quests.
var quests := true
var set_stage := ""  # EDID:STAGE[,EDID:STAGE...]

# Doors and runs that drive themselves.
var pick_locks := false
var activate: Array[int] = []  # references to activate in turn
var damage: Array[Vector2] = []  # DamageObject calls (ref, amount) after the activations
var activate_wait := 0  # frames between two of them
