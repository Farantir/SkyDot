// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's time of day between places (session/game_time.hpp): how days
// passed and an hour of a day convert, how time moves by hours, the speed
// that goes to whoever runs it, and the hand-off between the weather outside
// and the AI's clock inside, as SkydotClock applies it.
#include "ai/packages.hpp"
#include "session/game_time.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using namespace skydot::session;

TEST_CASE("days passed and the time of day convert both ways", "[game_time]") {
    const double days = days_of(18.0, 3);
    CHECK(days == Approx(3.75));
    CHECK(hour_of(days) == Approx(18.0));
    CHECK(day_of(days) == 3);

    // The AI reads the hour from its clock the same way.
    for (const double d : {0.0, 0.25, 2.99, 7.5, 100.999}) {
        CHECK(hour_of(d) == Approx(skydot::ai::Clock{d}.hour()));
    }
}

TEST_CASE("the day rolls over at midnight", "[game_time]") {
    CHECK(day_of(2.999) == 2);
    CHECK(hour_of(2.999) == Approx(23.976));
    CHECK(day_of(3.0) == 3);
    CHECK(hour_of(3.0) == Approx(0.0));
    // 24 h of a day is the start of the next.
    CHECK(days_of(24.0, 2) == Approx(days_of(0.0, 3)));
}

TEST_CASE("setting the hour wraps into the day", "[game_time]") {
    CHECK(shifted_hour(12.0, 1.0) == Approx(13.0));
    CHECK(shifted_hour(23.5, 1.0) == Approx(0.5));
    CHECK(shifted_hour(0.5, -1.0) == Approx(23.5));
    CHECK(shifted_hour(0.0, -1.0) == Approx(23.0));
    CHECK(shifted_hour(6.0, 0.0) == Approx(6.0));
}

TEST_CASE("the AI's clock moves by hours but not back past the start", "[game_time]") {
    CHECK(shifted_days(1.0, 6.0) == Approx(1.25));
    CHECK(shifted_days(1.25, -6.0) == Approx(1.0));
    CHECK(shifted_days(0.01, -1.0) == 0.0);
    CHECK(shifted_days(0.0, -24.0) == 0.0);
}

TEST_CASE("the time is told as HH:MM", "[game_time]") {
    CHECK(hour_text(0.0) == "00:00");
    CHECK(hour_text(9.25) == "09:15");
    CHECK(hour_text(13.5) == "13:30");
    CHECK(hour_text(23.999) == "23:59");
}

TEST_CASE("a clock starts where it was told and runs at its scale", "[game_time]") {
    const GameTime t(8.5, 20.0, false);
    CHECK(t.hour() == 8.5);
    CHECK(t.day() == 0);
    CHECK(t.scale() == 20.0);
    CHECK(t.speed(false) == 20.0);
    CHECK(t.speed(true) == 0.0);
}

TEST_CASE("time scale 0 stops time for whoever runs it", "[game_time]") {
    const GameTime t(12.0, 0.0, false);
    CHECK(t.speed(false) == 0.0);
    CHECK(t.for_ai().speed == 0.0);
    CHECK(t.for_weather().speed == 0.0);
}

TEST_CASE("a still clock (screenshots, benchmarks) starts both owners stopped", "[game_time]") {
    const GameTime t(12.0, 20.0, true);
    CHECK(t.scale() == 20.0);
    CHECK(t.for_ai().speed == 0.0);
    CHECK(t.for_weather().speed == 0.0);

    const GameTime running(12.0, 20.0, false);
    CHECK(running.for_ai().speed == 20.0);
    CHECK(running.for_weather().speed == 20.0);
}

TEST_CASE("a weather about to run the time is given the kept one", "[game_time]") {
    GameTime t(6.0, 5.0, false);
    t.keep(22.0, 4);
    const WeatherTime w = t.for_weather();
    CHECK(w.hour == 22.0);
    CHECK(w.day == 4);
    CHECK(w.speed == 5.0);
    CHECK(t.for_ai().days == Approx(4.0 + 22.0 / 24.0));
}

TEST_CASE("outside the AI follows the weather, stopped", "[game_time]") {
    const AiTime at = GameTime::follow_weather(6.0, 2);
    CHECK(at.days == Approx(2.25));
    CHECK(at.speed == 0.0);
}

TEST_CASE("inside the AI's clock runs and what it says is kept", "[game_time]") {
    GameTime t(12.0, 20.0, false);
    t.keep_days(5.75);
    CHECK(t.hour() == Approx(18.0));
    CHECK(t.day() == 5);
    CHECK(t.speed(false) == 20.0);
    CHECK(t.speed(true) == 0.0); // benchmarking
    // Leaving the weather's place and entering one with the AI's clock: the
    // time is the one the weather left.
    t.keep(3.0, 9);
    CHECK(t.for_ai().days == Approx(9.125));
}
