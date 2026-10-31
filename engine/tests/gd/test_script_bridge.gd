# SPDX-License-Identifier: GPL-3.0-or-later
extends "gd_test.gd"

const Fakes := preload("fakes.gd")

func test_missing_activation_fails_once() -> void:
	var config := GameConfig.new()
	config.activate.assign([0x123])
	var bridge := ScriptBridge.new(Fakes.world(), Fakes.papyrus(), Node3D.new(), Camera3D.new(),
			config, null, null, null, null)
	var failed := watch(bridge, "failed")
	for i in 700:
		bridge.run_activation()
	expect_eq(failed.size(), 1, "failed is emitted once")
