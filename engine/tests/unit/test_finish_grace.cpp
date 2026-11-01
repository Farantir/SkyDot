// SPDX-License-Identifier: GPL-3.0-or-later
//
// The grace a finished effect's script objects get (magic/finish_grace.hpp): VM seconds, not frames.
#include "magic/finish_grace.hpp"

#include <catch2/catch_test_macros.hpp>

using skydot::magic::FinishGrace;

TEST_CASE("script objects of a finished effect wait VM seconds, however many updates they take", "[magic][forget]") {
    FinishGrace grace;
    grace.add(7);
    // Many short updates (a fast frame rate) do not shorten the wait to a few frames.
    for (int i = 0; i < 5; ++i) {
        CHECK(grace.tick(0.016).empty());
    }
    CHECK(grace.size() == 1);
    const auto due = grace.tick(0.1);
    REQUIRE(due.size() == 1);
    CHECK(due[0] == 7);
    CHECK(grace.size() == 0);
}

TEST_CASE("one long update ends the wait at once, and a stopped clock never does", "[magic][forget]") {
    FinishGrace grace;
    grace.add(1);
    CHECK(grace.tick(0.0).empty());
    CHECK(grace.tick(0.0).empty());
    grace.add(2);
    const auto due = grace.tick(FinishGrace::k_effect_finish_grace);
    CHECK(due == std::vector<std::uint32_t>{1, 2});
}

TEST_CASE("a forgotten id no longer waits", "[magic][forget]") {
    FinishGrace grace;
    grace.add(1);
    grace.add(2);
    grace.forget(1);
    CHECK(grace.size() == 1);
    CHECK(grace.tick(1.0) == std::vector<std::uint32_t>{2});
}
