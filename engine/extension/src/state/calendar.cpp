// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/calendar.hpp"

#include <array>
#include <cmath>
#include <utility>

namespace skydot::vm {
namespace {

/// The game starts on the 17th of Last Seed (month 7, 0-based).
constexpr int k_start_month = 7;
constexpr int k_start_date = 17;
/// The day of the week the game starts on, 0 Sundas … 6 Loredas. Morndas,
/// as the opening's date reads; not yet compared with GetDayOfWeek in game.
constexpr int k_start_day_of_week = 1;
constexpr std::array<int, 12> k_month_days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
/// Days since the start to (month, date).
std::pair<int, int> calendar(double days) {
    int month = k_start_month;
    int date = k_start_date;
    auto left = static_cast<long long>(std::floor(std::max(0.0, days)));
    while (left > 0) {
        const int rest = k_month_days[static_cast<std::size_t>(month)] - date;
        if (left <= rest) {
            date += static_cast<int>(left);
            break;
        }
        left -= rest + 1;
        month = (month + 1) % 12;
        date = 1;
    }
    return {month, date};
}

} // namespace

double Calendar::hour() const {
    const double frac = days - std::floor(days);
    return frac * 24.0;
}

int Calendar::day_of_week() const {
    const auto d = static_cast<long long>(std::floor(days)) + k_start_day_of_week;
    return static_cast<int>(((d % 7) + 7) % 7);
}

int Calendar::month() const {
    return calendar(days).first;
}

int Calendar::date() const {
    return calendar(days).second;
}

} // namespace skydot::vm
