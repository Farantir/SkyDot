// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's time of day as the engine keeps it between places, and the rules
// for handing it from one owner to the other. Outside, SkydotWeather runs the
// time (an hour and a day count); inside, SkydotAi's clock does (the days
// passed since the start, the time of day as the fraction). The time is kept
// here while one is left and the other not yet started, so a place is entered
// at the hour the last one was left at. Nothing here touches Godot;
// SkydotClock (clock.hpp) applies it to the two.
#pragma once

#include <cstdint>
#include <string>

namespace skydot::session {

/// What SkydotAi's clock is set to: days passed (the time of day is the
/// fraction) and game seconds per second.
struct AiTime {
    double days = 0.0;
    double speed = 0.0;
};

/// What a SkydotWeather about to run the time is given.
struct WeatherTime {
    double hour = 0.0;
    std::int64_t day = 0;
    double speed = 0.0;
};

/// SkydotAi's days passed for an hour (0 to 24) of day number `day`.
double days_of(double hour, std::int64_t day);
/// The hour and the day number of days passed (the fraction is the time of
/// day, as ai::Clock reads it).
double hour_of(double days);
std::int64_t day_of(double days);
/// `hour` moved by `hours`, wrapped into 0 to 24 (the day count does not
/// change).
double shifted_hour(double hour, double hours);
/// `days` moved by `hours`; time does not run back past the start.
double shifted_days(double days, double hours);
/// "HH:MM".
std::string hour_text(double hour);

/// The time kept while a place is left, and the speed it runs at.
class GameTime {
public:
    GameTime() = default;
    /// `scale`: game seconds per second; `still`: screenshots and benchmarks,
    /// where time stands still whatever the scale is.
    GameTime(double hour, double scale, bool still) : hour_(hour), scale_(scale), still_(still) {}

    double hour() const { return hour_; }
    std::int64_t day() const { return day_; }
    double scale() const { return scale_; }
    void set_scale(double scale) { scale_ = scale; }

    /// The speed handed to whoever runs the time: 0 when `stopped`.
    double speed(bool stopped) const { return stopped ? 0.0 : scale_; }

    /// The AI starts keeping the time: it starts at this one.
    AiTime for_ai() const { return {days_of(hour_, day_), speed(still_)}; }
    /// A weather about to be set up is given this one.
    WeatherTime for_weather() const { return {hour_, day_, speed(still_)}; }

    /// The weather's place is left: its time is kept for the next one.
    void keep(double hour, std::int64_t day) {
        hour_ = hour;
        day_ = day;
    }
    /// Outside the AI follows the weather: its clock is set to the weather's
    /// and stopped.
    static AiTime follow_weather(double hour, std::int64_t day) { return {days_of(hour, day), 0.0}; }
    /// Inside the AI's clock runs (see speed); what it says is kept.
    void keep_days(double days) { keep(hour_of(days), day_of(days)); }

private:
    double hour_ = 12.0;
    std::int64_t day_ = 0;
    double scale_ = 20.0;
    bool still_ = false;
};

} // namespace skydot::session
