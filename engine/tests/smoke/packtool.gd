# SPDX-License-Identifier: GPL-3.0-or-later
#
# The pack tool (game/packtool/). Always: argument building and parsing. With a
# bethconv binary ($SKYDOT_BETHCONV, else a converter build next to this
# checkout): a real `convert --json` of an empty Data folder through
# BethconvCli's pipes, `target`/`info`/`cell` queries on the result, and the
# whole tool built headless with its settings in the scratch folder. No game
# data is needed: an empty folder converts to an empty pack.
extends SceneTree

var failures := 0
var cli := BethconvCli.new()
var scratch := ""
var events: Array = []
var exit_code := -1
var answers := {}
var tool: Node = null
var step := 0
var deadline := 0


func _init() -> void:
    _pure_checks()
    var binary := OS.get_environment("SKYDOT_BETHCONV")
    if cli.locate(binary).is_empty():
        if OS.get_environment("SKYDOT_BETHCONV_REQUIRE") != "":
            printerr("smoke_packtool: no bethconv and SKYDOT_BETHCONV_REQUIRE is set")
            quit(1)
            return
        print("smoke_packtool: pure checks only, failures=", failures,
            " (SKIP live part: no bethconv; set SKYDOT_BETHCONV)")
        quit(failures if failures != 0 else 77)
        return
    print("smoke_packtool: using ", cli.path)
    scratch = OS.get_environment("SKYDOT_SCRATCH")
    if scratch.is_empty():
        scratch = OS.get_user_data_dir()
    scratch = scratch.path_join("packtool")
    _remove_tree(scratch)
    DirAccess.make_dir_recursive_absolute(scratch.path_join("Data"))
    cli.event.connect(func(e: Dictionary) -> void: events.append(e))
    cli.finished.connect(func(code: int) -> void: exit_code = code)


func expect(ok: bool, what: String) -> void:
    if not ok:
        failures += 1
        printerr("FAIL: ", what)


func _pure_checks() -> void:
    var args := BethconvCli.convert_args({"data": "/d", "out": "/o"})
    expect(args == PackedStringArray(["convert", "--json", "--data", "/d", "-o", "/o"]),
        "minimal convert args: %s" % args)
    args = BethconvCli.convert_args({"data": "/d", "out": "/o", "mo2": "/m", "profile": "P 1",
        "store": "loose", "prune": true, "max_texture": 1024, "encode": "bc7", "filter": "meshes/",
        "limit": 50})
    expect(args == PackedStringArray(["convert", "--json", "--data", "/d", "-o", "/o", "--mo2", "/m",
        "--profile", "P 1", "--store", "loose", "--prune", "--max-texture-size", "1024",
        "--encode-uncompressed", "bc7",
        "--filter", "meshes/", "--limit", "50"]),
        "full convert args: %s" % args)
    expect(BethconvCli.parse_document("noise\n{\"a\": 1}\n") == {"a": 1.0}, "last JSON line is taken")
    expect(BethconvCli.parse_document("no json").is_empty(), "no JSON gives {}")
    expect(PackToolUi.pack_name("se") == "se", "pack name without a profile")
    expect(PackToolUi.pack_name("vr", "FUS RO DAH (Basic + Appearance)") == "vr-fus-ro-dah-basic-appearance",
        "pack name from a profile: " + PackToolUi.pack_name("vr", "FUS RO DAH (Basic + Appearance)"))
    expect(BethconvCli.format_bytes(3.5 * 1024 * 1024 * 1024) == "3.5 GiB", "GiB formatting")


func _process(_delta: float) -> bool:
    if cli.path.is_empty():
        return false
    var now := Time.get_ticks_msec()
    match step:
        0:
            cli.query(PackedStringArray(["target", scratch.path_join("pack"), "--json"]),
                func(doc: Dictionary) -> void: answers["target"] = doc)
            deadline = now + 30000
            step = 1
        1:
            if answers.has("target"):
                var doc: Dictionary = answers["target"]
                expect(not doc.has("error"), "target answers: %s" % doc)
                expect(doc.get("exists") == false, "the pack folder does not exist yet")
                expect(doc.get("blob", {}).get("verdict") in ["ok", "warn"], "a blob may go there")
                var started := cli.start(BethconvCli.convert_args(
                    {"data": scratch.path_join("Data"), "out": scratch.path_join("pack")}))
                expect(started, "convert starts")
                expect(not cli.start(PackedStringArray(["detect"])), "a second job is refused")
                deadline = now + 60000
                step = 2
            elif now > deadline:
                return _give_up("target")
        2:
            if exit_code >= 0:
                _check_convert()
                cli.query(PackedStringArray(["info", scratch.path_join("pack"), "--json"]),
                    func(doc: Dictionary) -> void: answers["info"] = doc)
                cli.query(PackedStringArray(["cell", scratch.path_join("pack"), "--worlds", "--json"]),
                    func(doc: Dictionary) -> void: answers["worlds"] = doc)
                cli.query(PackedStringArray(["no-such-command"]),
                    func(doc: Dictionary) -> void: answers["bad"] = doc)
                deadline = now + 30000
                step = 3
            elif now > deadline:
                cli.cancel()
                return _give_up("convert")
        3:
            if answers.has("info") and answers.has("worlds") and answers.has("bad"):
                _check_queries()
                tool = load("res://packtool/pack_tool.tscn").instantiate()
                tool.settings_path = scratch.path_join("packtool.cfg")
                root.add_child(tool)
                deadline = now + 30000
                step = 4
            elif now > deadline:
                return _give_up("queries")
        4:
            # Detection has answered once the install list was refreshed.
            if tool.get("_convert")._install_option.item_count > 0:
                var f: Dictionary = tool.get("_convert").form()
                expect(f.has("data") and f.has("out"), "the convert form has data and out")
                tool.queue_free()
                cli.shutdown()
                print("smoke_packtool: failures=", failures)
                quit(failures)
                return true
            elif now > deadline:
                return _give_up("the tool's detection")
    return false


func _check_convert() -> void:
    expect(exit_code == 0, "convert exits 0, got %d" % exit_code)
    var kinds := events.map(func(e: Dictionary) -> String: return e.get("event", ""))
    expect(kinds.has("start") and kinds.back() == "done", "start ... done: %s" % [kinds])
    var phases := {}
    for e in events:
        if e.get("event") == "progress":
            phases[e["phase"]] = true
    for phase in ["mount", "merge", "assets", "finish"]:
        expect(phases.has(phase), "progress reports " + phase)
    var done: Dictionary = events.back() if not events.is_empty() else {}
    expect(done.get("forms", -1) == 0, "an empty folder has no forms")
    expect(done.get("json_version", 0) == 1, "json_version 1")


func _check_queries() -> void:
    var info: Dictionary = answers["info"]
    expect(not info.has("error"), "info answers: %s" % info)
    var input: Dictionary = info.get("manifest", {}).get("input", {})
    expect(input.get("kind") == "data", "the manifest records a Data folder input")
    expect(input.get("data", "") == scratch.path_join("Data"), "and which one: %s" % input)
    expect(info.get("blob") is Dictionary and info["blob"]["stale_bytes"] == 0, "nothing stale")
    expect(answers["worlds"].get("worlds") == [], "no worldspaces: %s" % answers["worlds"])
    expect(answers["bad"].has("error"), "a failing command reports an error")


func _give_up(what: String) -> bool:
    failures += 1
    printerr("FAIL: timed out waiting for ", what)
    cli.shutdown()
    quit(failures)
    return true


func _remove_tree(path: String) -> void:
    var dir := DirAccess.open(path)
    if dir == null:
        return
    for sub in dir.get_directories():
        _remove_tree(path.path_join(sub))
    for name in dir.get_files():
        dir.remove(name)
    DirAccess.remove_absolute(path)
