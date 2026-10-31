# SPDX-License-Identifier: GPL-3.0-or-later
#
# The lifecycle contract of the GDScript subsystems (GameSession._lifecycle): reset() drops all
# per-game state and frees the nodes the subsystem made, on_place_left() drops what is tied to the
# place's built nodes; both are silent. The state is filled through the private variables.
extends "gd_test.gd"

const Fakes := preload("fakes.gd")

var _host: Node3D


func before_each() -> void:
	_host = Node3D.new()
	add_child(_host)


## Every signal of `obj`, watched.
func _watch_all(obj: Object) -> Array:
	var heard := []
	for sig in obj.get_signal_list():
		if String(sig["name"]) in ["ready", "renamed", "tree_entered", "tree_exiting", "tree_exited", "child_entered_tree",
				"child_exiting_tree", "child_order_changed", "replacing_by", "editor_description_changed",
				"script_changed", "property_list_changed", "visibility_changed", "visibility_changed",
				"hidden", "draw", "item_rect_changed", "gui_input", "focus_entered", "focus_exited", "mouse_entered",
				"mouse_exited", "resized", "sort_children", "theme_changed", "minimum_size_changed"]:
			continue
		heard.append(watch(obj, sig["name"]))
	return heard


func _silent(heard: Array, what: String) -> void:
	for emissions: Array in heard:
		expect(emissions.is_empty(), "%s emits nothing" % what)


func _combat() -> Combat:
	var sounds := Sounds.new(Fakes.world(), null, Callable(), Callable())
	_host.add_child(sounds)
	return Combat.new(Fakes.world(), SkydotPack.new(), Fakes.papyrus(), _host, sounds,
		func() -> Node3D: return null, func() -> Vector3: return Vector3.ZERO)


func _fill_combat(c: Combat) -> Node:
	var art := Node3D.new()
	_host.add_child(art)
	c._npc_attacks.append({"attacker": 5})
	c._shots.append({"attacker": 5})
	c._swaps.append({"ref": 5})
	c._cooldowns[5] = 1.0
	c._effects.append({"node": art, "left": 1.0})
	c._attack = {"time": 0.0}
	c._held = true
	c._held_time = 0.4
	c._held_fired = true
	c._left_next = true
	c._drawn[5] = true
	c._plans[5] = {"race": 1}
	c._player_plan = {"race": 1}
	return art


func _combat_attack_state_empty(c: Combat, what: String) -> void:
	expect(c._npc_attacks.is_empty(), "%s: npc attacks" % what)
	expect(c._shots.is_empty(), "%s: shots" % what)
	expect(c._swaps.is_empty(), "%s: swaps" % what)
	expect(c._cooldowns.is_empty(), "%s: cooldowns" % what)
	expect(c._effects.is_empty(), "%s: effects" % what)
	expect(c._attack.is_empty(), "%s: attack" % what)
	expect(not c._held and c._held_time == 0.0 and not c._held_fired and not c._left_next, "%s: held press" % what)


func test_combat_reset() -> void:
	var c := _combat()
	var art := _fill_combat(c)
	var heard := _watch_all(c)
	c.reset()
	_combat_attack_state_empty(c, "reset")
	expect(c._drawn.is_empty() and c._plans.is_empty() and c._player_plan.is_empty(), "reset drops stance and plans")
	await get_tree().process_frame
	expect(not is_instance_valid(art), "the impact art is freed")
	_silent(heard, "Combat.reset")


func test_combat_on_place_left_keeps_the_stance() -> void:
	var c := _combat()
	var art := _fill_combat(c)
	var heard := _watch_all(c)
	c.on_place_left()
	_combat_attack_state_empty(c, "place left")
	expect_eq(c._drawn.size(), 1, "the stance stays")
	expect_eq(c._plans.size(), 1, "the plans stay")
	await get_tree().process_frame
	expect(not is_instance_valid(art), "the impact art is freed")
	_silent(heard, "Combat.on_place_left")


func test_corpses_both() -> void:
	for reset in [true, false]:
		var c := Corpses.new(Fakes.world(), Fakes.papyrus(), _host, func() -> Vector3: return Vector3.ZERO)
		var heard := _watch_all(c)
		c._hits[1] = Vector3.ONE
		c._pending[1] = 0.5
		c._live[1] = true
		c._bodies.append({"ref": 1, "node": null})
		c._paralysed[1] = true
		c._paralysis_timer = 0.1
		if reset:
			c.reset()
		else:
			c.on_place_left()
		expect(c._hits.is_empty() and c._pending.is_empty() and c._live.is_empty(), "hits, pending, live (%s)" % reset)
		expect(c._bodies.is_empty() and c._paralysed.is_empty(), "bodies, paralysed (%s)" % reset)
		expect_eq(c._paralysis_timer, 0.0, "paralysis timer (%s)" % reset)
		_silent(heard, "Corpses")


func test_paralysis_poll_forgets_actors_that_are_not_built() -> void:
	var c := Corpses.new(Fakes.world(), Fakes.papyrus(), _host, func() -> Vector3: return Vector3.ZERO)
	c._paralysed[0x1234] = true  # streamed out: no actor node of it in the host
	c._poll_paralysis(1.0)
	expect(c._paralysed.is_empty(), "an actor that is not built is not marked paralysed")


func test_npc_magic_and_shouts() -> void:
	var world := Fakes.world()
	var papyrus := Fakes.papyrus()
	var combat := _combat()
	var magic := Magic.new(world, SkydotPack.new(), papyrus, _host, combat, null, func() -> Node3D: return null)
	var speech := Speech.new(Sounds.new(world, null, Callable(), Callable()), Callable())
	for reset in [true, false]:
		var casts := NpcMagic.new(world, papyrus, magic, combat)
		var shouts := NpcShouts.new(world, papyrus, magic, combat, speech, SkydotPack.new())
		casts._casts[1] = {"phase": "charging"}
		shouts._breaths[1] = {"state": "wait"}
		var heard := _watch_all(casts) + _watch_all(shouts)
		if reset:
			casts.reset()
			shouts.reset()
		else:
			casts.on_place_left()
			shouts.on_place_left()
		expect(casts._casts.is_empty(), "casts (%s)" % reset)
		expect(shouts._breaths.is_empty(), "breaths (%s)" % reset)
		_silent(heard, "NpcMagic/NpcShouts")


func test_visual_effects_free_their_streams() -> void:
	for reset in [true, false]:
		var v := VisualEffects.new(Fakes.world(), _host, func() -> Vector3: return Vector3.ZERO)
		var particles := GPUParticles3D.new()
		_host.add_child(particles)
		v._streams["1:2"] = {"node": particles, "ref": 2, "target": 0, "left": 5.0}
		var heard := _watch_all(v)
		if reset:
			v.reset()
		else:
			v.on_place_left()
		expect_eq(v.stream_count(), 0, "streams (%s)" % reset)
		await get_tree().process_frame
		expect(not is_instance_valid(particles), "the particles are freed (%s)" % reset)
		_silent(heard, "VisualEffects")


func test_speech_stops_silently() -> void:
	for reset in [true, false]:
		var s := Speech.new(Sounds.new(Fakes.world(), null, Callable(), Callable()), Callable())
		_host.add_child(s)
		var voice := AudioStreamPlayer.new()
		s.add_child(voice)
		s._queues[1] = [{"line": {}, "seconds": 1.0, "player": voice}, {"line": {}, "seconds": 1.0, "player": null}]
		var heard := _watch_all(s)
		if reset:
			s.reset()
		else:
			s.on_place_left()
		expect(s._queues.is_empty(), "queues (%s)" % reset)
		await get_tree().process_frame
		expect(not is_instance_valid(voice), "the playing voice is freed (%s)" % reset)
		_silent(heard, "Speech")


func test_actor_gear_stance_survives_only_a_place_left() -> void:
	var g := ActorGear.new(Fakes.world(), Fakes.papyrus())
	g.set_drawn(5, true)
	g.on_place_left()
	expect(g.is_drawn(5), "the stance stays when the place is left")
	g.reset()
	expect(not g.is_drawn(5), "reset drops it")


func test_placed_refs() -> void:
	var p := PlacedRefs.new(Fakes.world(), Fakes.papyrus())
	_host.add_child(p)
	p._deleted[1] = true
	p._waiting.append([1, 2, 3, Vector3.ZERO, Vector3.ZERO, 1.0, false])
	var generation := p._generation
	p.on_place_left()
	expect_eq(p._waiting.size(), 1, "waiting references stay when the place is left")
	expect_eq(p._generation, generation, "the generation stays")
	p.reset()
	expect(p._deleted.is_empty() and p._waiting.is_empty(), "reset drops deleted and waiting")
	expect_eq(p._generation, generation + 1, "reset bumps the generation")


func test_place_transition_survives_a_place_left() -> void:
	var t := PlaceTransition.new(null, null)
	add_child(t)
	t.begin_fade({"ref": 1})
	t.on_place_left()
	expect(t.is_fading(), "the transition itself leaves the place: it goes on")
	t.reset()
	expect(not t.is_fading(), "reset cancels it")


func test_projectiles_free_their_shots() -> void:
	for reset in [true, false]:
		var p := Projectiles.new(null, null, Callable())
		_host.add_child(p)
		var arrow := Node3D.new()
		p.add_child(arrow)
		var shot := {"node": arrow, "age": 0.0, "stuck": false, "travelled": 0.0, "range": 1.0}
		p._live.append(shot)
		var heard := _watch_all(p)
		if reset:
			p.reset()
		else:
			p.on_place_left()
		expect(p._live.is_empty(), "live shots (%s)" % reset)
		await get_tree().process_frame
		expect(not is_instance_valid(arrow), "the arrow is freed (%s)" % reset)
		_silent(heard, "Projectiles")


func test_magic_frees_its_art() -> void:
	var combat := _combat()
	for reset in [true, false]:
		var m := Magic.new(Fakes.world(), SkydotPack.new(), Fakes.papyrus(), _host, combat, null, func() -> Node3D: return null)
		var art := Node3D.new()
		_host.add_child(art)
		m._effects.append({"node": art, "left": 1.0})
		m._streams[1] = {"left": 1.0}
		var heard := _watch_all(m)
		if reset:
			m.reset()
		else:
			m.on_place_left()
		expect(m._effects.is_empty() and m._streams.is_empty(), "effects and streams (%s)" % reset)
		await get_tree().process_frame
		expect(not is_instance_valid(art), "the art is freed (%s)" % reset)
		_silent(heard, "Magic")
