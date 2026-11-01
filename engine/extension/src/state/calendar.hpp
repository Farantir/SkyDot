// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game calendar: days passed since the game started, as the hour, the
// day of the week, the month and the date. Pure arithmetic.
#pragma once

namespace skydot::vm {

/// Game time.
struct Calendar {
    /// Days passed since the game started (GameDaysPassed), with the time of
    /// day as the fraction.
    double days = 0.0;

    double hour() const;
    /// 0 Sundas (Sunday) … 6 Loredas (Saturday).
    int day_of_week() const;
    /// 0-based month and 1-based day of the month.
    int month() const;
    int date() const;
};

} // namespace skydot::vm
