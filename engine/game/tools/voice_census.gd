# SPDX-License-Identifier: GPL-3.0-or-later
#
# The oracle of the voice file stem rule: derives the stem of every INFO
# response in the pack's world.fb and compares the set with the stems of all
# sound/voice/ vpaths of the pack (the voice type folder is ignored), per
# plugin. Also reports, for INFOs with a speaker (ANAM), the share whose full
# path, voice type folder included, exists.
#
#   SKYDOT_SSE_PACK=<pack> godot4.7 --headless --path game --script res://tools/voice_census.gd
#
# Exit status 1 when less than 98 % of the archive's stems are matched. About 1.3 %
# are INFOs world.fb does not hold at all, ~20 have odd suffixes.
extends SceneTree

func _initialize() -> void:
    var dir := OS.get_environment("SKYDOT_SSE_PACK")
    var pack := SkydotPack.new()
    if dir == "" or pack.open(dir) != OK:
        printerr("voice_census: no pack (SKYDOT_SSE_PACK)")
        quit(1)
        return
    var papyrus := SkydotPapyrus.new()
    if papyrus.setup(pack, pack.open_world()) != OK:
        printerr("voice_census: no world")
        quit(1)
        return
    var paths := PackedStringArray()
    var index := FileAccess.open(dir.path_join("vpath.idx"), FileAccess.READ)
    while not index.eof_reached():
        var path := index.get_line().get_slice("\t", 0)
        if path.begins_with("sound/voice/"):
            paths.append(path)
    var result: Dictionary = papyrus.voice_census(paths)
    var total_archive := 0
    var total_matched := 0
    var plugins: Dictionary = result["plugins"]
    for plugin: String in plugins:
        var row: Dictionary = plugins[plugin]
        total_archive += row["archive"]
        total_matched += row["matched"]
        print("%-20s archive stems %6d matched %6d (%.2f %%)  derived %6d in archive %6d (%.2f %%)" % [
            plugin, row["archive"], row["matched"], 100.0 * row["matched"] / maxf(1, row["archive"]),
            row["derived"], row["derived_found"], 100.0 * row["derived_found"] / maxf(1, row["derived"])])
        for sample: String in row["derived_missing_samples"]:
            print("    derived, not in archive: ", sample)
    var share := 100.0 * total_matched / maxf(1, total_archive)
    print("archive stems matched: %d / %d (%.2f %%)" % [total_matched, total_archive, share])
    print("INFOs with a speaker: %d, speaker with a voice type: %d, full path in the archive: %d (%.2f %% of voiced)" % [
        result["speakers"], result["speakers_voiced"], result["speakers_found"],
        100.0 * result["speakers_found"] / maxf(1, result["speakers_voiced"])])
    for sample: String in result["speaker_missing_samples"]:
        print("    speaker path missing: ", sample)
    quit(0 if share >= 98.0 else 1)
