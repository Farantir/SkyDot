// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/game_time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace skydot::session {

double days_of(double hour, std::int64_t day) { return static_cast<double>(day) + hour / 24.0; }

double hour_of(double days) { return (days - std::floor(days)) * 24.0; }

std::int64_t day_of(double days) { return static_cast<std::int64_t>(days); }

double shifted_hour(double hour, double hours) { return std::fmod(hour + hours + 24.0, 24.0); }

double shifted_days(double days, double hours) { return std::max(0.0, days + hours / 24.0); }

std::string hour_text(double hour) {
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d", static_cast<int>(hour),
                  static_cast<int>(std::fmod(hour, 1.0) * 60.0));
    return text;
}

} // namespace skydot::session
