# SPDX-License-Identifier: GPL-3.0-or-later
#
# Base class of a test file: `extends "gd_test.gd"`, then write test_* methods
# (see README.md). The runner makes one instance per test and adds it to the
# root, so a test may add children and await process_frame.
extends Node

var failures: Array[String] = []
var expected_errors: Array[String] = []
var skip_reason := ""
var runner: Object

func expect(cond: bool, what: String) -> void:
    if not cond:
        failures.append(what)

func expect_eq(actual: Variant, expected: Variant, what: String) -> void:
    if typeof(actual) != typeof(expected) or actual != expected:
        failures.append("%s: got %s, expected %s" % [what, var_to_str(actual), var_to_str(expected)])

## The test passes only if an error containing `pattern` is logged during it;
## other logged errors still fail it.
func expect_errors(pattern: String) -> void:
    expected_errors.append(pattern)

## Marks the test skipped (it ends there when the caller returns right after).
func skip(reason: String) -> void:
    skip_reason = reason

## Records the arguments of every emission of obj's signal; one Array per emission.
func watch(obj: Object, signal_name: String) -> Array:
    var log: Array = []
    obj.connect(signal_name, func(...args: Array) -> void: log.append(args))
    return log

## The test pack's world, or null after skip() (SKYDOT_TESTPACK unset) or a
## failure (unset with SKYDOT_TESTPACK_REQUIRE, or the pack does not open).
func testpack_world() -> SkydotWorld:
    var dir := OS.get_environment("SKYDOT_TESTPACK")
    if dir == "" or not DirAccess.dir_exists_absolute(dir):
        # CI runs the gd tests before the smoke step converts the test pack.
        var why := "SKYDOT_TESTPACK unset" if dir == "" else "no test pack at " + dir
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            failures.append(why + " and SKYDOT_TESTPACK_REQUIRE is set")
        else:
            skip(why)
        return null
    var pack := SkydotPack.new()
    if pack.open(dir) != OK:
        failures.append("the test pack opens: " + pack.get_error())
        return null
    var world := pack.open_world()
    if world == null:
        failures.append("world.fb opens")
    set_meta("pack", pack)  # keeps the pack alive as long as the test
    return world
