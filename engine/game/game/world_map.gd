# SPDX-License-Identifier: GPL-3.0-or-later
#
# The world map's rules (the game layer; ui/map_view.gd draws the map, viewer/map_scene.gd its 3D view):
#   map_of()      the worldspace whose map applies (WRLD map_world, PNAM "use map data") and its MNAM data
#   markers()     the map world's markers with their flags now (XMRK FNAM over what scripts and discovery set,
#                 SkydotPapyrus get_map_marker_flags): 1 visible, 2 can travel to; shown_markers() those the map
#                 shows (visible, not disabled)
#   step()        discovery, every DISCOVERY_SECONDS over the markers not yet found (cached per map, dropped on a
#                 load): a marker not yet discovered (no "can travel" flag) whose XRDS radius (else
#                 iMapMarkerRevealDistance) the player comes within is discovered: flags 0 -> 3 (measured with
#                 SkyrimRemote @marker, skycompare-data/map-research.md section 5: Riverwood XRDS 4500 at 4402, Bleak
#                 Falls Barrow 5000 at 4909, South Skybound Watch without XRDS at 917), "<NAME> DISCOVERED"
#                 (sDiscoveredText) shown; Game.FastTravel requests are taken here
#   refusal()     why fast travel to a marker is refused, the game's HUD notification texts (sNoFastTravel*);
#                 overencumbered is GameSession.overencumbered
#   travel()      fast travel: the fade (fFastTravelFadeSecs 0.5), the arrival at the marker's linked heading marker
#                 (XLKR) with its heading, else on the marker as placed, in its worldspace (measured: Windhelm); game time passes as distance / (8 x fFastTravelSpeedMult)
#                 seconds (measured: Riverwood -> Whiterun 39845 units, 1.38 h) and the actors recover over it as a
#                 wait does (Rest.RECOVERY_PER_HOUR)
#   custom marker (Place Marker) and the last exterior position, saved (WMAP).
# Not checked yet (no systems behind them): hostile actors near but not fighting, guards pursuing (alarm), health
# damage over time; followers and horses do not come along.
class_name WorldMap
extends RefCounted

## iMapMarkerRevealDistance when the pack lacks it (Skyrim.esm: 1000).
const REVEAL_DISTANCE := 1000.0
## Fast travel's speed: game units per game second at fFastTravelSpeedMult 1 (map-research.md section 6, measured
## on three trips: 8.02, 8.03, 8.13). fFastTravelSpeedMult is an exe setting (not in the ESMs): 1.
const TRAVEL_UNITS_PER_SECOND := 8.0
const SPEED_MULT := 1.0
## fFastTravelFadeSecs:General.
const FADE_SECONDS := 0.5
const VISIBLE := 1
const CAN_TRAVEL := 2
## TNAM type of the player's own marker and the player's position in map.swf's IconTypes.
const PLAYER_SET_TYPE := 65
const YOU_ARE_HERE_TYPE := 66
## World DATA "Can't Fast Travel".
const WORLD_NO_TRAVEL := 0x02
const WMAP_VERSION := 1
## Seconds between two discovery checks of step().
const DISCOVERY_SECONDS := 0.25

const FALLBACK := {
	"sDiscoveredText": "%s DISCOVERED",
	"sFastTravelConfirm": "Fast travel to %s?",
	"sNoFastTravelScriptBlock": "Fast travel is currently unavailable from this location.",
	"sNoFastTravelOverencumbered": "You cannot fast travel while overencumbered.",
	"sNoFastTravelCombat": "You cannot fast travel during combat!",
	"sNoFastTravelHostileActorsNear": "You cannot fast travel when enemies are nearby.",
	"sNoFastTravelInAir": "You cannot fast travel while jumping or falling.",
	"sNoFastTravelCell": "You cannot fast travel from this location.",
	"sNoFastTravelDefault": "You cannot fast travel from here.",
	"sNoFastTravelUndiscovered": "You cannot fast travel to an undiscovered location.",
	"sPlayerSetMarkerName": "Custom Destination",
	"sPlaceMarkerUndiscovered": "You have not discovered this location yet.\nPlace marker?",
	"sMoveMarkerQuestion": "Do you want to move your marker or remove it?",
}

## A marker was discovered (its name).
signal discovered(ref: int, name: String)

## The player's own marker: {world, position (game units)}, {} when none.
var custom_marker := {}
## Where the player last stood outside: {world, position}; the map opens there from an interior.
var last_exterior := {}

var _session: GameSession
var _markers := {}  # world -> Array of the data's marker dictionaries
var _worlds := {}  # world id -> list_worlds entry
var _undiscovered := {}  # map world -> its data markers without "can travel" when last checked
var _discovery_clock := 0.0


func _init(session: GameSession) -> void:
	_session = session
	for w: Dictionary in session.world.list_worlds():
		_worlds[int(w["id"])] = w


## The list_worlds entry of `world`, {} when unknown.
func world_info(world: int) -> Dictionary:
	return _worlds.get(world, {})


## The worldspace whose map shows `world` (itself, or the ancestor whose map it uses), 0 when it has none.
func map_world(world: int) -> int:
	return int(world_info(world).get("map_world", 0))


## The markers on `world`'s map: the data's entries (ref, type, name, position, radius, arrival...) of every worldspace
## shown on it (the city worlds keep the capitals' markers: Whiterun's 0x162CE is in WhiterunWorld, at Tamriel's
## coordinates; their ONAM is scale 1, offset 0), with "flags" now and "world" the one each is in.
func markers(world: int) -> Array:
	var out := []
	for m: Dictionary in _data_markers(world):
		var entry := m.duplicate()
		var flags := _session.papyrus.get_map_marker_flags(int(m["ref"]))
		entry["flags"] = flags if flags >= 0 else int(m["flags"])
		out.append(entry)
	return out


## The markers the map shows: markers() that are visible (flag 1) and not disabled.
func shown_markers(world: int) -> Array:
	var out := []
	for m: Dictionary in markers(world):
		if int(m["flags"]) & VISIBLE and not _session.papyrus.is_ref_disabled(int(m["ref"])):
			out.append(m)
	return out


func _data_markers(world: int) -> Array:
	if not _markers.has(world):
		var list := []
		for id: int in _worlds:
			if map_world(id) == world:
				for m: Dictionary in _session.world.get_map_markers(id):
					m["world"] = id
					list.append(m)
		_markers[world] = list
	return _markers[world]


## A marker by reference, with "map_world" the world whose map shows it; {} when it is none.
func marker(ref: int) -> Dictionary:
	for world: int in _worlds:
		if map_world(world) != world:
			continue
		for m: Dictionary in markers(world):
			if int(m["ref"]) == ref:
				m["map_world"] = world
				return m
	return {}


## Where the map opens: the player's world and eye (the first-person camera: @mapcam's target was the camera's
## position, 124 units over the arrival marker at Whiterun) outside, the last exterior position inside (game units);
## {} when there is no map to show.
func map_place() -> Dictionary:
	var world := _session.streamer.world_id
	if world != 0 and map_world(world) != 0:
		return {"world": map_world(world), "position": SkydotWorld.godot_to_skyrim(_session.viewpoint_position())}
	if not last_exterior.is_empty() and map_world(int(last_exterior["world"])) != 0:
		return {"world": map_world(int(last_exterior["world"])), "position": last_exterior["position"]}
	return {}


## A load: the map's own state comes from the save (adopt), the rest is gone.
func reset() -> void:
	custom_marker = {}
	last_exterior = {}
	_undiscovered.clear()


## Each frame of play: the last exterior position, discovery, script fast travel.
func step(delta: float, feet: Vector3) -> void:
	var world := _session.streamer.world_id
	if world != 0:
		last_exterior["world"] = world
		last_exterior["position"] = _eye(feet)
		_discovery_clock += delta
		if _discovery_clock >= DISCOVERY_SECONDS:
			_discovery_clock = 0.0
			_discover(map_world(world), SkydotWorld.godot_to_skyrim(feet))
	var request := _session.papyrus.take_fast_travel_request()
	if request != 0:
		var m := marker(request)
		if not m.is_empty():
			travel(m)
		else:
			print("FastTravel: 0x%08X is no map marker" % request)


static func _eye(feet: Vector3) -> Vector3:
	return SkydotWorld.godot_to_skyrim(feet + Vector3(0.0, GameSession.EYE_HEIGHT, 0.0))


func _discover(world: int, at: Vector3) -> void:
	if world == 0:
		return
	var reveal := float(_session.world.get_gmst_int("iMapMarkerRevealDistance", int(REVEAL_DISTANCE)))
	var list := _undiscovered_on(world)
	var i := 0
	while i < list.size():
		var m: Dictionary = list[i]
		var ref := int(m["ref"])
		var flags := _session.papyrus.get_map_marker_flags(ref)
		if flags < 0 or flags & CAN_TRAVEL:
			list.remove_at(i)  # discovered meanwhile (a script, the map's own)
			continue
		var radius := float(m["radius"]) if float(m["radius"]) > 0.0 else reveal
		if not _session.papyrus.is_ref_disabled(ref) and (m["position"] as Vector3).distance_to(at) <= radius:
			discover(ref, String(m["name"]))
			list.remove_at(i)
			continue
		i += 1


## The data markers of `world`'s map not discovered when last checked (built on first use after a load).
func _undiscovered_on(world: int) -> Array:
	if not _undiscovered.has(world):
		var list := []
		for m: Dictionary in _data_markers(world):
			var flags := _session.papyrus.get_map_marker_flags(int(m["ref"]))
			if flags >= 0 and flags & CAN_TRAVEL == 0:
				list.append(m)
		_undiscovered[world] = list
	return _undiscovered[world]


## Discover a marker: flags 0 -> 3 (visible, travel), the HUD's "<NAME> DISCOVERED".
func discover(ref: int, name: String) -> void:
	_session.papyrus.set_map_marker_flags(ref, _session.papyrus.get_map_marker_flags(ref) | VISIBLE | CAN_TRAVEL)
	print("map: discovered 0x%08X %s" % [ref, name])
	if _session.journal != null:
		_session.journal.count("Locations Discovered")
	discovered.emit(ref, name)  # "<NAME> DISCOVERED": the HUD's quest update (HudView), else the shell's note


## The notification that refuses fast travel to `m` now, "" when the player may travel.
func refusal(m: Dictionary) -> String:
	if int(m.get("flags", 0)) & CAN_TRAVEL == 0:
		return text("sNoFastTravelUndiscovered")
	if not _session.papyrus.is_fast_travel_enabled():
		return text("sNoFastTravelScriptBlock")
	var world := _session.streamer.world_id
	if world != 0 and int(world_info(world).get("flags", 0)) & WORLD_NO_TRAVEL:
		return text("sNoFastTravelCell")
	if world == 0 and _session.place.cell_id != 0 and _session.world.get_cell(_session.place.cell_id).get("cant_travel_from_here", false):
		return text("sNoFastTravelCell")
	if _session.rest != null and _session.rest.fighting():
		return text("sNoFastTravelCombat")
	if _session.player_in_air():
		return text("sNoFastTravelInAir")
	if _session.overencumbered():
		return text("sNoFastTravelOverencumbered")
	return ""


## Game hours a fast travel from `from` to `to` (game units) takes: the distance on the map (x, y; Riverwood ->
## Whiterun's 39845 units are that, 40001 in 3D).
static func travel_hours(from: Vector3, to: Vector3) -> float:
	return Vector2(from.x, from.y).distance_to(Vector2(to.x, to.y)) / (TRAVEL_UNITS_PER_SECOND * SPEED_MULT) / 3600.0


## Fast travel to marker `m` (a markers() entry): behind the fade the time passes and the player arrives.
func travel(m: Dictionary) -> void:
	# The arrival's worldspace: the linked heading marker's (Whiterun's 0xC07EF is in WhiterunWorld: the game's
	# autosave after that trip was named WhiterunWorld); without one the marker's own (measured with SkyrimRemote: Windhelm's
	# marker, which has none, put the player on the marker itself, heading 0 as it is placed, in WindhelmWorld).
	var world := int(m.get("world", 0))
	if int(m.get("arrival_ref", 0)) != 0 and m.has("arrival_position"):
		var cell := _session.world.get_ref_cell(int(m["arrival_ref"]))
		if cell != 0 and int(_session.world.get_cell(cell).get("world", 0)) != 0:
			world = int(_session.world.get_cell(cell)["world"])
	var pos: Vector3 = m.get("arrival_position", m["position"])
	var rot: Vector3 = m.get("arrival_rotation", m.get("rotation", Vector3.ZERO))
	var arrival := SkydotWorld.skyrim_transform(pos, Vector3(0.0, 0.0, rot.z), 1.0)
	var from: Vector3 = SkydotWorld.godot_to_skyrim(_session.player_feet())
	if _session.streamer.world_id == 0 and not last_exterior.is_empty():
		from = last_exterior["position"] - Vector3(0.0, 0.0, GameSession.EYE_HEIGHT / SkydotWorld.unit_scale())
	var hours := travel_hours(from, pos)
	var door := {
		"ref": int(m["ref"]), "arrival": arrival, "destination_interior": false, "destination_world": world,
		"destination_cell": 0, "destination": -1,
	}
	print("map: fast travel to 0x%08X %s, %.2f h" % [int(m["ref"]), String(m["name"]), hours])
	_session.transition.go(door, FADE_SECONDS, _pass_time.bind(hours))


func _pass_time(hours: float) -> void:
	var scale := _session.clock.get_time_scale()
	_session.clock.shift(hours)
	if scale > 0.0:
		_session.papyrus.recover(hours * Rest.RECOVERY_PER_HOUR / scale)


## The player's own marker (Place Marker) at `position` (game units) of `world`; {} removes it.
func place_custom(world: int, position: Vector3) -> void:
	custom_marker = {"world": world, "position": position}


func remove_custom() -> void:
	custom_marker = {}


func text(setting: String) -> String:
	return _session.world.get_gmst_string(setting, FALLBACK.get(setting, ""))


func save_bytes() -> PackedByteArray:
	return var_to_bytes({"custom": custom_marker, "exterior": last_exterior})


## A WMAP section's bytes as a state for adopt; {} when it is not one.
static func decode(bytes: PackedByteArray) -> Dictionary:
	var state: Variant = bytes_to_var(bytes)
	return state if state is Dictionary else {}


func adopt(state: Dictionary) -> void:
	custom_marker = state.get("custom", {})
	last_exterior = state.get("exterior", {})
	_undiscovered.clear()
