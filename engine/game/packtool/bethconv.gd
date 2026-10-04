# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the `bethconv` command line for the pack tool. All conversion, detection
# and checking lives in bethconv; this only starts it and reads its JSON
# (converter/docs/cli-json.md).
#
#   query(args, callback)  one JSON document from a short command (detect, mo2,
#                          target, info, cell), run on a thread; callback(doc)
#                          gets {"error": ...} when the command fails
#   start(args)            a long command (convert --json): every stdout line
#                          is an event, stderr lines are log text; signals
#                          event, log_line and finished(exit_code)
#   cancel()               kills it; an interrupted convert leaves a pack the
#                          next run repairs (formats/pack-format.md)
class_name BethconvCli
extends RefCounted

signal event(data: Dictionary)
signal log_line(text: String)
signal finished(exit_code: int)

## How long `finished` waits for a process whose pipes have closed to exit, and
## how often it looks.
const EXIT_WAIT_MSEC := 5000
const EXIT_POLL_SECONDS := 0.02

## The binary in use; empty when none was found.
var path := ""

var _pid := -1
var _readers: Array[Thread] = []
var _queries: Array[Thread] = []


## Pick the binary: `preferred` if it exists, else one next to this program,
## else a converter build in the source tree (release first).
func locate(preferred: String = "") -> String:
	path = ""
	for candidate in candidates(preferred):
		if FileAccess.file_exists(candidate):
			path = candidate
			break
	return path


static func candidates(preferred: String = "") -> PackedStringArray:
	var exe := "bethconv.exe" if OS.get_name() == "Windows" else "bethconv"
	var out := PackedStringArray()
	if not preferred.is_empty():
		out.append(preferred)
	out.append(OS.get_executable_path().get_base_dir().path_join(exe))
	# Running from the repository: engine/game -> converter/build/<preset>.
	var repo := ProjectSettings.globalize_path("res://").path_join("../../converter/build").simplify_path()
	for preset in ["linux-release", "windows-release", "linux-debug-asan", "linux-debug"]:
		out.append(repo.path_join(preset).path_join("tools/bethconv-cli").path_join(exe))
	return out


func is_running() -> bool:
	return _pid > 0


## Run a command that prints one JSON document; `callback(doc)` is called on
## the main thread. Never blocks the caller.
func query(args: PackedStringArray, callback: Callable) -> void:
	if path.is_empty():
		callback.call({"error": "bethconv not found"})
		return
	var thread := Thread.new()
	_queries.append(thread)
	thread.start(_run_query.bind(path, args, callback, thread))


func _run_query(binary: String, args: PackedStringArray, callback: Callable, thread: Thread) -> void:
	var output := []
	# stderr too: a failing command explains itself there.
	var code := OS.execute(binary, args, output, true)
	var text: String = output[0] if not output.is_empty() else ""
	var doc := parse_document(text)
	if doc.is_empty():
		var lines := text.strip_edges().split("\n", false)
		var why: String = lines[lines.size() - 1] if not lines.is_empty() else "no output"
		doc = {"error": "bethconv %s failed (exit %d): %s" % [args[0] if not args.is_empty() else "",
			code, why]}
		# CLI11 exits 106 on an unknown subcommand or flag.
		if code == 106:
			doc["error"] += ". This bethconv predates the pack tool; build the converter again " \
				+ "or choose a newer binary under Settings."
	_finish_query.call_deferred(thread, callback, doc)


func _finish_query(thread: Thread, callback: Callable, doc: Dictionary) -> void:
	if thread.is_started():  # shutdown() may have joined it already
		thread.wait_to_finish()
	_queries.erase(thread)
	if callback.is_valid():
		callback.call(doc)


## The last JSON object in a command's stdout, or {} if there is none.
static func parse_document(text: String) -> Dictionary:
	var lines := text.strip_edges().split("\n", false)
	for i in range(lines.size() - 1, -1, -1):
		# Only candidate lines: parse_string logs an error for anything else.
		if not lines[i].begins_with("{"):
			continue
		var parsed = JSON.parse_string(lines[i])
		if parsed is Dictionary:
			return parsed
	return {}


## Start a long-running command. Returns false if one is already running or
## the process could not be started.
func start(args: PackedStringArray) -> bool:
	if is_running() or path.is_empty():
		return false
	var pipes := OS.execute_with_pipe(path, args, true)
	if pipes.is_empty():
		return false
	_pid = pipes["pid"]
	_readers.clear()
	for key in ["stdio", "stderr"]:
		var thread := Thread.new()
		_readers.append(thread)
		thread.start(_read_pipe.bind(pipes[key], key == "stdio"))
	return true


## Blocking reads, one thread per pipe: an unread stderr would fill up and
## stall the converter.
func _read_pipe(pipe: FileAccess, is_stdout: bool) -> void:
	while true:
		var line := pipe.get_line()
		var ended := pipe.eof_reached() or (pipe.get_error() != OK and line.is_empty())
		if not line.is_empty():
			_deliver.call_deferred(line, is_stdout)
		if ended:
			break
	_reader_done.call_deferred()


func _deliver(line: String, is_stdout: bool) -> void:
	if is_stdout and line.begins_with("{"):
		var parsed = JSON.parse_string(line)
		if parsed is Dictionary:
			event.emit(parsed)
			return
	log_line.emit(line)


func _reader_done() -> void:
	# Called once per pipe; the first call after both closed does the work.
	if _readers.is_empty():
		return
	for thread in _readers:
		if thread.is_alive():
			return
	for thread in _readers:
		thread.wait_to_finish()
	_readers.clear()
	# Both pipes closed: the process is exiting. Its code is a moment away.
	_await_exit(_pid, Time.get_ticks_msec() + EXIT_WAIT_MSEC)


## Poll with a timer instead of sleeping: this runs on the main thread, and the
## window must keep drawing until the process is gone (or `deadline` passes,
## then the code is whatever the OS reports). `_pid` stays set meanwhile, so
## the job still counts as running and cannot be started twice.
func _await_exit(pid: int, deadline: int) -> void:
	if _pid != pid:  # shutdown() gave up on this job
		return
	if OS.is_process_running(pid) and Time.get_ticks_msec() < deadline:
		var tree: SceneTree = Engine.get_main_loop()
		tree.create_timer(EXIT_POLL_SECONDS).timeout.connect(_await_exit.bind(pid, deadline))
		return
	_pid = -1
	finished.emit(OS.get_process_exit_code(pid))


func cancel() -> void:
	if is_running():
		OS.kill(_pid)


## Join the threads before the object goes away.
func shutdown() -> void:
	cancel()
	_pid = -1
	for thread in _readers + _queries:
		if thread.is_started():
			thread.wait_to_finish()
	_readers.clear()
	_queries.clear()


## Arguments for `convert --json` from the pack tool's form. Keys: data, out,
## and optionally list, mo2, profile, store ("blob"/"loose"), prune,
## max_texture (pixels, 0 for full size), encode ("keep", "bc7", "compact"),
## filter, limit, allow_slow.
static func convert_args(form: Dictionary) -> PackedStringArray:
	var args := PackedStringArray(["convert", "--json", "--data", form["data"], "-o", form["out"]])
	if not str(form.get("mo2", "")).is_empty():
		args.append_array(["--mo2", form["mo2"]])
		if not str(form.get("profile", "")).is_empty():
			args.append_array(["--profile", form["profile"]])
	if not str(form.get("list", "")).is_empty():
		args.append_array(["--list", form["list"]])
	if form.get("store", "blob") == "loose":
		args.append_array(["--store", "loose"])
	if form.get("prune", false):
		args.append("--prune")
	if int(form.get("max_texture", 0)) > 0:
		args.append_array(["--max-texture-size", str(int(form["max_texture"]))])
	if form.get("encode", "keep") != "keep":
		args.append_array(["--encode-uncompressed", form["encode"]])
	if not str(form.get("filter", "")).is_empty():
		args.append_array(["--filter", form["filter"]])
	if int(form.get("limit", 0)) > 0:
		args.append_array(["--limit", str(int(form["limit"]))])
	if form.get("allow_slow", false):
		args.append("--allow-slow-target")
	return args


## "1.2 GiB", "340.0 MiB", "12 KiB".
static func format_bytes(bytes: float) -> String:
	if bytes >= 1024.0 * 1024.0 * 1024.0:
		return "%.1f GiB" % (bytes / (1024.0 * 1024.0 * 1024.0))
	if bytes >= 1024.0 * 1024.0:
		return "%.1f MiB" % (bytes / (1024.0 * 1024.0))
	return "%d KiB" % int(ceil(bytes / 1024.0))
