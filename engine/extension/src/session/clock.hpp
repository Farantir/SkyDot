// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotClock`: the time of day, asked of one place. It has two owners that
// take turns: outside, SkydotWeather runs it (sky, light, weather); inside,
// SkydotAi's own clock does. The clock hands the time from one to the other
// as places are left and entered, and every frame (`sync`), so the AI always
// knows the hour and a place is entered at the hour the last one was left at.
// The rules are session/game_time.hpp's; this applies them to the two.
#pragma once

#include "ai/ai.hpp"
#include "render/weather.hpp"
#include "save/save_binding.hpp"
#include "session/game_time.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

namespace skydot {

class SkydotClock : public godot::RefCounted {
    GDCLASS(SkydotClock, godot::RefCounted)

public:
    /// Start at `start_hour`, at `time_scale` game seconds per second (0 stops
    /// time); `still` (screenshots and benchmarks) starts the weather and the
    /// AI with time standing still.
    void setup(double start_hour, double time_scale, bool still);

    /// The hour of the day (0 to 24) and the day count, wherever time runs now.
    double get_hour() const;
    std::int64_t get_day() const;
    void set_time_scale(double scale) { time_.set_scale(scale); }
    double get_time_scale() const { return time_.scale(); }

    /// Inside, `ai` keeps the time; it starts at this clock's.
    void attach_ai(const godot::Ref<SkydotAi>& ai);

    /// The weather running outside; null inside.
    SkydotWeather* get_weather() const;
    bool has_weather() const { return get_weather() != nullptr; }
    /// Give a weather about to be set up the hour, day and speed.
    void configure(SkydotWeather* node) const;
    /// `node` (configured, set up) now runs the time outside.
    void bind_weather(SkydotWeather* node);
    /// The weather's place is left: its time is kept for the next one.
    void release_weather();

    /// Every frame before the AI updates. Outside the weather keeps the time
    /// and the AI follows it; inside the AI's clock runs (stopped during a
    /// benchmark).
    void sync(bool benchmarking);

    /// Move the time by `hours`. Returns "HH:MM" for the new time, or "" when
    /// nothing keeps the time (inside, with the AI off).
    godot::String shift(double hours);

    /// Days passed since the start; the fraction is the time of day. What
    /// Utility.GetCurrentGameTime and the GameDaysPassed global read.
    double get_game_days() const;
    /// Set the time to `days` passed (GameDaysPassed's SetValue), wherever it
    /// runs now. `set_hour` keeps the day and sets the hour (GameHour).
    void set_game_days(double days);
    void set_hour(double hour);

    /// The clock's part of a save (the CLCK section): days passed, the speed
    /// and the weather. `prepare_load` decodes into a pending state and
    /// touches nothing; `commit_load` applies it: the time to whoever runs it
    /// now, the weather to the one running or, for the one started next
    /// (`take_saved_weather`), when a place is entered.
    void write_section(const godot::Ref<SkydotSaveWriter>& writer) const;
    godot::Error prepare_load(const godot::Ref<SkydotSaveReader>& reader);
    godot::Error commit_load();
    void cancel_load() { pending_ = false; }
    godot::String get_last_error() const { return error_; }
    /// The weather a save named, once: 0 if none is waiting.
    std::int64_t take_saved_weather();

    /// The time for a shot's JSON: hour, day and, outside, the speed.
    godot::Dictionary describe() const;

protected:
    static void _bind_methods();

private:
    session::GameTime time_; ///< The time kept while inside, from the AI.
    godot::Ref<SkydotAi> ai_; ///< Null with the AI off.
    std::uint64_t weather_ = 0; ///< Instance id of the weather; 0 inside.
    std::int64_t saved_weather_ = 0; ///< A loaded save's weather, for the next one started.
    bool pending_ = false;
    double pending_days_ = 0.0;
    double pending_scale_ = 0.0;
    std::int64_t pending_weather_ = 0;
    godot::String error_;
};

} // namespace skydot
