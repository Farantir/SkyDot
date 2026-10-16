# SPDX-License-Identifier: GPL-3.0-or-later
#
# The game layer's save and load, round trip, on the test pack
# (converter/tools/testpack), through a GameSession built headless around a
# bare Camera3D and a stand-in player: no rig, no input, no overlay. References
# are placed,
# deleted, moved and disabled, a door is unlocked and opened, a sound plays and
# a script counts; the state is saved, changed further and loaded, and every
# piece is compared with what it was when saved. A corrupt save must leave the
# running game as it was.
#
# Checks that name a bug of CODE-REVIEW.md which is not fixed yet are in KNOWN:
# they print "KNOWN FAIL Bx" and do not fail the test. One that passes anyway
# prints "KNOWN NOW PASSES Bx" and fails it, so the list is kept up to date
# (take the bug out of KNOWN when its fix lands).
#
# Not covered: projectiles (the test pack has no PROJ or AMMO to fire; the
# Weapon.Fire path is only checked up to its signal in papyrus.gd), the AI's
# actors and the weather (an exterior; this runs inside), which B5 names too.
extends SceneTree

const CELL := "TestpackInterior"
const CUBE := 0x301  # base 0x100, at the origin
const CUBE_BASE := 0x100
const LEVER := 0x304  # base 0x104, TestpackLeverScript
const LEVER_BASE := 0x104
const HIDDEN := 0x302  # base 0x110, 180 up
const INSIDE_DOOR := 0x303  # shares the lock of the outside door
const OUTSIDE_DOOR := 0x213
const GAME_HOUR := 0x38
const GAME_DAYS := 0x39
const TIME_SCALE := 0x3A
const SAVE_PATH := "user://game_save_smoke.skydot"
const BAD_PATH := "user://game_save_smoke_bad.skydot"
const FOREIGN_PATH := "user://game_save_smoke_foreign.skydot"

## Bugs of CODE-REVIEW.md that are known to be there: their checks are
## expected to fail.
const KNOWN := ["B5"]  # B5: live sounds are not saved (the clock is)

var failures := 0
var known_failures := 0

var _session: GameSession
var _player: TestPlayer
var _world: SkydotWorld
var _papyrus: SkydotPapyrus
var _clock: SkydotClock
var _placed_log: Array = []  # ref_placed: [ref, base, position]
var _errors: Array[String] = []


func _initialize() -> void:
    var pack_dir := OS.get_environment("SKYDOT_TESTPACK")
    if pack_dir == "":
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("smoke_game_save: SKYDOT_TESTPACK is unset and SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        print("smoke_game_save: SKIP (SKYDOT_TESTPACK unset)")
        quit(77)
        return
    _run(pack_dir)  # a coroutine: the scene tree runs a frame first


func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)


## A check that belongs to bug `bug`. While the bug is in KNOWN it is expected
## to fail; passing then is a failure of its own.
func check(bug: String, ok: bool, what: String) -> void:
    if not KNOWN.has(bug):
        expect(ok, "%s: %s" % [bug, what])
    elif ok:
        failures += 1
        printerr("KNOWN NOW PASSES %s: %s" % [bug, what])
    else:
        known_failures += 1
        print("KNOWN FAIL %s: %s" % [bug, what])


func _near(a: float, b: float) -> bool:
    return absf(a - b) < 0.001


func _frames(count: int) -> void:
    for i in count:
        await process_frame
        _papyrus.update(0.0)


func _call(fn: String, args: Array) -> Variant:
    var thread := _papyrus.call_global("TestpackWorldScript", fn, args)
    _papyrus.update(0.0)
    var result := _papyrus.take_result(thread)
    expect(thread != 0 and result["done"], "TestpackWorldScript.%s finished" % fn)
    var value: Variant = result.get("value")
    return (value as int) & 0xFFFFFFFF if value is int and fn == "Spawn" else value  # form ids are unsigned


func _node_position(ref: int) -> Variant:
    var node := _world.find_ref_node(ref) as Node3D
    return node.global_position if node != null else null


func _run(pack_dir: String) -> void:
    await process_frame
    if not _build(pack_dir):
        _finish()
        return
    await _frames(2)
    _check_clock(14.0)

    # ---- the state at save time ------------------------------------------
    var kept := _call("Spawn", [CUBE, CUBE_BASE]) as int  # placed, kept
    var scripted := _call("Spawn", [CUBE, LEVER_BASE]) as int  # placed with a script, then deleted
    var doomed := _call("Spawn", [CUBE, CUBE_BASE]) as int  # placed, no script, deleted
    expect(kept != 0 and scripted != 0 and doomed != 0 and kept != scripted, "PlaceAtMe makes references")
    await _frames(3)
    expect(_world.find_ref_node(kept) != null, "a placed reference is built")
    expect(_papyrus.get_ref_base(kept) == CUBE_BASE and _papyrus.get_ref_base(scripted) == LEVER_BASE,
        "and has its base")
    expect(not _papyrus.get_scripts(scripted).is_empty(), "a placed lever gets the lever's script")

    _call("Remove", [scripted])
    _call("Remove", [doomed])
    await _frames(3)
    expect(_papyrus.get_ref_base(doomed) == 0 and _papyrus.get_ref_base(scripted) == 0, "Delete removes a placed reference")
    expect(_world.find_ref_node(doomed) == null, "and its node")
    check("B1", _papyrus.get_scripts(scripted).is_empty(),
        "a deleted reference has no script instances left: %s" % [_papyrus.get_scripts(scripted)])

    # Move: the cube goes to where HIDDEN is; PlaceAtMe from it shows where it is.
    var origin_probe := _probe(CUBE)
    _call("MoveTo", [CUBE, HIDDEN])
    await _frames(2)
    var moved_to := _probe(CUBE)
    expect(not moved_to.is_equal_approx(origin_probe), "Reset to a target moves the cube: %s -> %s" % [origin_probe, moved_to])
    var moved_node: Variant = _node_position(CUBE)
    expect(moved_node != null and _near(moved_node.y, SkydotWorld.skyrim_position(moved_to).y), "and its node")

    # Disable: a reference of the pack (no node: it is a light) and a placed one.
    _call("Hide", [HIDDEN])
    _call("Hide", [kept])
    await _frames(1)
    expect(_papyrus.is_ref_disabled(HIDDEN) and _papyrus.is_ref_disabled(kept), "Disable is recorded")
    expect(not (_world.find_ref_node(kept) as Node3D).visible, "and hides a placed reference")

    # The door pair: unlock and open it.
    # (The NPC's package may have unlocked it when the cell was entered.)
    _papyrus.set_locked(OUTSIDE_DOOR, true)
    expect(_papyrus.get_lock_level(OUTSIDE_DOOR) == 25 and _papyrus.get_lock_level(INSIDE_DOOR) == 25, "the doors are locked")
    _papyrus.set_locked(OUTSIDE_DOOR, false)
    _papyrus.set_open_state(INSIDE_DOOR, 1)
    var open_state: int = _papyrus.get_open_state(INSIDE_DOOR)
    var lock_state: int = _papyrus.get_lock_level(OUTSIDE_DOOR)
    expect(lock_state == -1, "a door is unlocked")

    # A script counts, a sound plays, the clock moves.
    _papyrus.activate(LEVER)
    _papyrus.update(0.0)
    var pulls: int = _papyrus.get_variable(LEVER, "TestpackLeverScript", "Pulls")
    expect(pulls == 1, "the lever counts a pull: %d" % pulls)
    var sounds: Sounds = _session.sounds
    var descriptor := _world.find_sound_descriptor("TestpackClickSD")
    expect(descriptor != 0, "the test pack has a sound descriptor")
    var cube_disabled := _papyrus.is_ref_disabled(CUBE)
    expect(cube_disabled, "the lever disabled its cube")
    var instance := sounds.play(descriptor, 0)
    await _frames(2)
    var sound_plays := sounds.is_playing(instance)
    print("sound instance %d plays at save time: %s" % [instance, sound_plays])
    expect(not _clock.shift(2.0).is_empty(), "the clock moves")
    _check_clock(16.0)
    var hour: float = _clock.get_hour()
    var camera_at: Vector3 = _player.position

    _session.save_game(SAVE_PATH)
    expect(FileAccess.file_exists(SAVE_PATH), "a save is written")

    # ---- change everything ------------------------------------------------
    _call("Remove", [kept])
    var extra := _call("Spawn", [CUBE, CUBE_BASE]) as int
    _call("MoveTo", [CUBE, 0])  # back where it was placed
    _papyrus.activate(LEVER)
    _papyrus.update(0.0)
    _papyrus.set_locked(OUTSIDE_DOOR, true)
    _papyrus.set_open_state(INSIDE_DOOR, 0)
    if sound_plays:
        sounds.stop(instance)
    _clock.shift(5.0)
    _player.position += Vector3(1, 0, 1)
    await _frames(3)

    # ---- a save that cannot be loaded leaves the game as it is ------------
    _bad_save()
    var before_pulls: int = _papyrus.get_variable(LEVER, "TestpackLeverScript", "Pulls")
    var before_lock: int = _papyrus.get_lock_level(OUTSIDE_DOOR)
    var before_base: int = _papyrus.get_ref_base(extra)
    var before_scripts := _papyrus.get_scripts(LEVER)
    expect(not _session.load_game("user://no_such_save.skydot"), "a missing file is refused")
    expect(not _session.load_game(BAD_PATH), "a save with a truncated VM section is refused")
    check("B6", _papyrus.get_variable(LEVER, "TestpackLeverScript", "Pulls") == before_pulls
            and _papyrus.get_lock_level(OUTSIDE_DOOR) == before_lock
            and _papyrus.get_ref_base(extra) == before_base and _papyrus.get_scripts(LEVER) == before_scripts,
        "a failed load leaves the VM as it was: Pulls %s (was %d), lock %d (was %d), placed base 0x%X (was 0x%X), lever scripts %s"
            % [_papyrus.get_variable(LEVER, "TestpackLeverScript", "Pulls"), before_pulls,
               _papyrus.get_lock_level(OUTSIDE_DOOR), before_lock, _papyrus.get_ref_base(extra), before_base,
               _papyrus.get_scripts(LEVER)])
    # A save of another pack (its HEAD names another world hash) is refused too.
    _foreign_save()
    expect(not _session.load_game(FOREIGN_PATH), "a save of another pack is refused")
    expect(_papyrus.get_variable(LEVER, "TestpackLeverScript", "Pulls") == before_pulls
            and _papyrus.get_ref_base(extra) == before_base, "and the running game is as it was")
    await _frames(1)

    # ---- load and compare -------------------------------------------------
    expect(_session.load_game(SAVE_PATH), "the save loads")
    await _frames(4)
    expect(_papyrus.get_error_count() == 0, "no script error on the way: %s" % _papyrus.get_last_error())
    expect(_papyrus.get_ref_base(kept) == CUBE_BASE and _world.find_ref_node(kept) != null,
        "the placed reference is back, built")
    expect(_papyrus.get_ref_base(extra) == 0 and _world.find_ref_node(extra) == null,
        "a reference placed after the save is gone")
    expect(_papyrus.get_ref_base(doomed) == 0 and _papyrus.get_ref_base(scripted) == 0
            and _world.find_ref_node(doomed) == null, "deleted references stay deleted")
    check("B1", _papyrus.get_scripts(scripted).is_empty(),
        "the save holds no script instance of a deleted reference: %s" % [_papyrus.get_scripts(scripted)])
    check("B2", _probe(CUBE).is_equal_approx(moved_to),
        "a reference moved by Reset(target) is still there after loading: PlaceAtMe from it lands at %s, was %s"
            % [_probe(CUBE), moved_to])
    expect(_papyrus.is_ref_disabled(HIDDEN) and _papyrus.is_ref_disabled(kept), "the disabled references are still disabled")
    expect(_papyrus.is_ref_disabled(CUBE), "and the cube the lever disabled before the save is disabled again")
    var kept_node := _world.find_ref_node(kept) as Node3D
    expect(kept_node != null and not kept_node.visible, "and the placed one is hidden")
    expect(_papyrus.get_lock_level(OUTSIDE_DOOR) == lock_state, "the door is unlocked again: %d" % _papyrus.get_lock_level(OUTSIDE_DOOR))
    expect(_papyrus.get_open_state(INSIDE_DOOR) == open_state, "and open: %d" % _papyrus.get_open_state(INSIDE_DOOR))
    expect(_papyrus.get_variable(LEVER, "TestpackLeverScript", "Pulls") == pulls, "the script's count is back")
    expect(_player.position.is_equal_approx(camera_at), "the player is where it was saved")
    expect(_near(_clock.get_hour(), hour), "B5: the time of day is back: %.2f, saved at %.2f" % [_clock.get_hour(), hour])
    expect(_near(_papyrus_time(), hour / 24.0), "B4: and scripts read it: %.4f days" % _papyrus_time())
    # SetValue on GameHour moves the clock, and the sky with it.
    _call("SetGlobal", [GAME_HOUR, 9.0])
    _check_clock(9.0)
    _call("SetGlobal", [TIME_SCALE, 0.0])
    expect(_clock.get_time_scale() == 0.0, "B4: SetValue on TimeScale stops the clock")
    if sound_plays:
        check("B5", sounds.is_playing(instance), "a sound playing at save time plays after loading")
    else:
        expect(false, "the sound should be playing at save time")
    _finish()


## What scripts read as the time (Utility.GetCurrentGameTime and the GameHour,
## GameDaysPassed and TimeScale globals) is the SkydotClock's: one game clock (B4).
func _check_clock(hour: float) -> void:
    expect(_near(_clock.get_hour(), hour), "B4: the clock is at %.2f h: %.2f" % [hour, _clock.get_hour()])
    expect(_near(_papyrus_time(), hour / 24.0), "B4: GetCurrentGameTime is %.4f days: %.4f" % [hour / 24.0, _papyrus_time()])
    expect(_near(_call("GlobalValue", [GAME_HOUR]) as float, hour), "B4: GameHour is %.2f: %s" % [hour, _call("GlobalValue", [GAME_HOUR])])
    expect(_near(_call("GlobalValue", [GAME_DAYS]) as float, hour / 24.0), "B4: GameDaysPassed follows the clock")
    expect(_near(_call("GlobalValue", [TIME_SCALE]) as float, _clock.get_time_scale()), "B4: TimeScale is the clock's")


func _papyrus_time() -> float:
    return _call("GameTime", []) as float


## A stand-in for a shell's player (GameSession's contract): it only keeps
## where it was put.
class TestPlayer:
    extends RefCounted
    var yaw := 0.0
    var pitch := 0.0
    var flying := false
    var position := Vector3.ZERO
    var view_distance := 0.0
    var loaded_position := Vector3.ZERO
    var loaded_yaw := 0.0
    var loaded_pitch := 0.0

    func place_at(at: Vector3, new_yaw: float, new_pitch: float) -> void:
        position = at
        yaw = new_yaw
        pitch = new_pitch

    func set_view_distance(metres: float) -> void:
        view_distance = metres

    func save_section() -> PackedByteArray:
        return var_to_bytes({"position": position, "yaw": yaw, "pitch": pitch})

    func load_section(bytes: PackedByteArray) -> bool:
        var state = bytes_to_var(bytes)
        if typeof(state) != TYPE_DICTIONARY or typeof(state.get("position")) != TYPE_VECTOR3:
            return false
        loaded_position = state["position"]
        loaded_yaw = state["yaw"]
        loaded_pitch = state["pitch"]
        return true


## The game, as a shell would make it but with no rig, no input, no overlay:
## a GameSession around a bare camera and a stand-in player.
func _build(pack_dir: String) -> bool:
    var config := GameConfig.new()
    config.pack = pack_dir
    config.time = 14.0
    config.navigation = false
    config.actors = false
    config.quests = false
    config.preload_doors = false
    _player = TestPlayer.new()
    var viewpoint := Camera3D.new()
    root.add_child(viewpoint)
    _session = GameSession.new()
    _session.interactive = false
    _session.failed.connect(func(text: String) -> void: expect(false, "the session fails: " + text))
    root.add_child(_session)
    expect(_session.open(config, viewpoint, _player), "the session opens on the test pack")
    if _session.world == null:
        return false
    _papyrus = _session.papyrus
    _clock = _session.clock
    _world = _session.world
    _papyrus.error.connect(func(text: String) -> void: _errors.append(text); printerr("vm error: ", text))
    _papyrus.ref_placed.connect(func(ref: int, base: int, _cell: int, position: Vector3, _r: Vector3, _s: float, _d: bool) -> void:
        _placed_log.append([ref, base, position]))
    expect(_session.start_game(), "the game starts")
    expect(_session.enter_cell(CELL, SkydotWorld.skyrim_position(Vector3(0, -100, 150)), null),
        "the interior is entered")
    return true


## Where PlaceAtMe from `ref` puts a reference (Skyrim space): the reference's
## position as the VM sees it. The probe is deleted again.
func _probe(ref: int) -> Vector3:
    var made := _call("Spawn", [ref, CUBE_BASE]) as int
    var position := Vector3.INF
    for entry in _placed_log:
        if entry[0] == made:
            position = entry[2]
    _call("Remove", [made])
    return position


## A save file that is whole (its checksum is right) but whose VM section is
## cut short: only decoding it finds out.
func _bad_save() -> void:
    var good := SkydotSaveReader.new()
    expect(good.open(ProjectSettings.globalize_path(SAVE_PATH)) == OK, "the good save opens")
    var writer := SkydotSaveWriter.new()
    for tag in ["HEAD", "VMST", "INVT", "ANIM", "FLST", "PLCD", "DEST", "RFST", "PAPY", "QUST", "PLYR"]:
        var bytes := good.get_section(tag)
        if tag == "VMST":
            bytes = bytes.slice(0, bytes.size() / 2)
        writer.add_section(tag, good.get_section_version(tag), bytes)
    expect(writer.write(ProjectSettings.globalize_path(BAD_PATH)) == OK, "the bad save is written")


## The good save with another pack's world hash in its HEAD.
func _foreign_save() -> void:
    var good := SkydotSaveReader.new()
    good.open(ProjectSettings.globalize_path(SAVE_PATH))
    var writer := SkydotSaveWriter.new()
    for tag in ["HEAD", "VMST", "INVT", "ANIM", "FLST", "PLCD", "DEST", "RFST", "PAPY", "QUST", "PLYR"]:
        var bytes := good.get_section(tag)
        if tag == "HEAD":
            var head := StreamPeerBuffer.new()
            var other_hash := "0".repeat(64).to_utf8_buffer()
            head.put_u32(3)
            head.put_data("0.1".to_utf8_buffer())
            head.put_u32(other_hash.size())
            head.put_data(other_hash)
            head.put_u32(1)
            head.put_u32(11)
            head.put_data("Other.esm".to_utf8_buffer() + PackedByteArray([0, 0]))
            bytes = head.data_array
        writer.add_section(tag, good.get_section_version(tag), bytes)
    writer.write(ProjectSettings.globalize_path(FOREIGN_PATH))


func _finish() -> void:
    DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE_PATH))
    DirAccess.remove_absolute(ProjectSettings.globalize_path(BAD_PATH))
    DirAccess.remove_absolute(ProjectSettings.globalize_path(FOREIGN_PATH))
    print("smoke_game_save: failures=%d, known failures=%d" % [failures, known_failures])
    quit(failures)
