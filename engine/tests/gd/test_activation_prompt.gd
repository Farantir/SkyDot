# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

const Fakes := preload("fakes.gd")

func test_lock_text_none_and_key() -> void:
    expect_eq(ActivationPrompt.lock_text(ActivationPrompt.NO_LOCK), "", "no lock")
    expect_eq(ActivationPrompt.lock_text(ActivationPrompt.REQUIRES_KEY), "Requires key", "key 255")
    expect_eq(ActivationPrompt.lock_text(300), "Requires key", "above 255")

func test_lock_text_levels() -> void:
    for c: Array in [[100, "Master"], [254, "Master"], [99, "Expert"], [75, "Expert"],
            [74, "Adept"], [50, "Adept"], [49, "Apprentice"], [25, "Apprentice"],
            [24, "Novice"], [1, "Novice"], [0, "Novice"]]:
        expect_eq(ActivationPrompt.lock_text(c[0]), c[1], "level %d" % c[0])

func test_fake_papyrus_is_usable() -> void:
    var gear := ActorGear.new(Fakes.world(), Fakes.papyrus())
    expect(gear != null, "ActorGear takes the fakes")
    var p := Fakes.papyrus()
    var errors := watch(p, "error")
    p.emit_signal("error", "boom")
    expect_eq(errors, [["boom"]], "watch records the emission")
