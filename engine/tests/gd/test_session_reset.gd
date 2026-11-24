# SPDX-License-Identifier: GPL-3.0-or-later
#
# A load or a restart (GameSession._reset_game_state) drops the dialogue and the scenes' lines of the game before,
# and ends a rest under way. The session is never opened: a clock, the fake VM and world, no lifecycle parts.
extends "gd_test.gd"
const Fakes := preload("fakes.gd")

const SPEAKER := 0xFF000042

var _session: GameSession


func before_each() -> void:
	_session = GameSession.new()
	_session.clock = SkydotClock.new()
	_session.clock.setup(10.0, 20.0, false)
	_session.papyrus = Fakes.papyrus()
	_session.world = Fakes.world()
	_session.speech = Speech.new(null, func(_ref: int) -> Node: return null)
	_session.rest = Rest.new(_session)
	_session.world_map = WorldMap.new(_session)
	add_child(_session)
	_session.add_child(_session.speech)


func test_a_reset_drops_the_dialogue_and_the_scene_lines() -> void:
	_session._talk_speaker = SPEAKER
	_session._talk_lines = [{"info": 0x1234, "first": true, "last": true, "text": "Hello."}]
	_session._talk_current = {"info": 0x1233, "first": true, "last": true, "text": "Hi."}
	_session._talk_after = {"closed": false, "topics": []}
	_session._talk_topics = [{"id": 0x1235}]
	_session._scene_held = [{"speaker": SPEAKER, "scene": 1, "action": 2, "lines": []}]
	_session._scene_lines = {"1:2": 1}
	var closed := watch(_session, "dialogue_closed")
	_session._reset_game_state()
	expect(not _session.is_dialogue_open(), "no dialogue is open")
	expect(_session._talk_lines.is_empty() and _session._talk_current.is_empty() and _session._talk_after.is_empty()
		and _session._talk_topics.is_empty(), "no line or topic is left")
	expect(_session._scene_held.is_empty() and _session._scene_lines.is_empty(), "no scene line is left")
	expect_eq(closed.size(), 1, "the dialogue view hears it closed")


func test_a_reset_ends_a_rest() -> void:
	_session.rest.begin(false, 4)
	_session._reset_game_state()
	expect(not _session.rest.is_resting(), "the rest is over")
	expect_eq(_session.clock.get_time_scale(), 20.0, "the clock runs again")
