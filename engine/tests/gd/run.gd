# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one GDScript test file headless:
#   godot --headless --path engine/game --script engine/tests/gd/run.gd -- <test_file.gd>
# Every method test_* of the file (in declaration order) is one test. Exit code:
# the number of failed tests; 1 if the file has no tests or does not load;
# 77 if every test skipped. Kept trivial on purpose: a script error in
# _initialize would leave the process running, so tests are called via call().
extends SceneTree

const TIMEOUT_SECONDS := 60.0

var _mutex := Mutex.new()
var _errors: Array[String] = []
var _logger: _ErrorLogger

class _ErrorLogger extends Logger:
    var runner: Object
    func _log_error(function: String, file: String, line: int, code: String, rationale: String,
            _editor_notify: bool, _error_type: int, _backtraces: Array[ScriptBacktrace]) -> void:
        runner.call("_on_error", "%s (%s:%d)" % [rationale if rationale != "" else code, file, line])

func _on_error(text: String) -> void:
    _mutex.lock()
    _errors.append(text)
    _mutex.unlock()

func _initialize() -> void:
    var timer := create_timer(TIMEOUT_SECONDS)
    timer.timeout.connect(func() -> void:
        printerr("FAIL: watchdog, the run took longer than %d s" % int(TIMEOUT_SECONDS))
        quit(1))
    _logger = _ErrorLogger.new()
    _logger.runner = self
    OS.add_logger(_logger)
    _run(OS.get_cmdline_user_args())

func _run(args: PackedStringArray) -> void:
    if args.is_empty():
        printerr("FAIL: usage: run.gd -- <test_file.gd>")
        quit(1)
        return
    var script: Variant = load(args[0]) if args[0].begins_with("res://") else _load_file(args[0])
    if not script is GDScript or not script.can_instantiate():
        printerr("FAIL: %s does not load" % args[0])
        quit(1)
        return
    var names: Array[String] = []
    for m: Dictionary in script.get_script_method_list():
        if String(m["name"]).begins_with("test_"):
            names.append(m["name"])
    if names.is_empty():
        printerr("FAIL: %s has no test_* methods" % args[0])
        quit(1)
        return
    var failed := 0
    var skipped := 0
    for test_name in names:
        var t: Node = script.new()
        root.add_child(t)
        t.set("runner", self)
        _mutex.lock()
        _errors.clear()
        _mutex.unlock()
        if t.has_method("before_each"):
            await t.call("before_each")
        await t.call(test_name)
        if t.has_method("after_each"):
            await t.call("after_each")
        var reasons: Array[String] = []
        reasons.append_array(t.get("failures"))
        _mutex.lock()
        var errs := _errors.duplicate()
        _mutex.unlock()
        var expected: Array = t.get("expected_errors")
        for e: String in errs:
            var ok := false
            for p: String in expected:
                if e.contains(p):
                    ok = true
            if not ok:
                reasons.append("error: " + e)
        for p: String in expected:
            var seen := false
            for e: String in errs:
                if e.contains(p):
                    seen = true
            if not seen:
                reasons.append("expected error not seen: " + p)
        if t.get("skip_reason") != "" and reasons.is_empty():
            skipped += 1
            print("SKIP %s: %s" % [test_name, t.get("skip_reason")])
        elif reasons.is_empty():
            print("PASS ", test_name)
        else:
            failed += 1
            print("FAIL %s: %s" % [test_name, "; ".join(reasons)])
        t.queue_free()
    print("%d tests, %d failed, %d skipped" % [names.size(), failed, skipped])
    if failed == 0 and skipped == names.size():
        if OS.get_environment("SKYDOT_TESTPACK_REQUIRE") != "":
            printerr("FAIL: every test skipped but SKYDOT_TESTPACK_REQUIRE is set")
            quit(1)
            return
        quit(77)
        return
    quit(failed)

func _load_file(path: String) -> Variant:
    var src := FileAccess.get_file_as_string(path)
    if src == "":
        return null
    var s := GDScript.new()
    s.source_code = src
    s.resource_path = ProjectSettings.globalize_path(path)
    if s.reload() != OK:
        return null
    return s
