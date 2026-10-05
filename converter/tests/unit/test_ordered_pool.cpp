// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pool behind convert()'s threads: results in index order, memory bounded,
// an exception where a single thread would have raised it, and a destructor that
// does not wait for work nobody will read. The private header is included
// directly; nothing else in the tests needs src/.
#include "../../src/pack/ordered_pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>

using bethconv::pack::OrderedPool;

namespace {

using Pool = OrderedPool<std::size_t>;

constexpr auto weigh_one = [](const std::size_t&) -> std::uint64_t { return 1; };

} // namespace

TEST_CASE("pool results come back in index order whatever order they finish in", "[pool]") {
    constexpr std::size_t count = 3000; // more than the window, so slots are reused
    Pool pool(
        count, 8, Pool::Limits{.window = 64, .budget = 1ULL << 20},
        [](std::size_t index) {
            // Every few indexes take longer, so later ones finish first.
            if (index % 5 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            return index;
        },
        weigh_one);
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(pool.next() == i);
    }
}

TEST_CASE("pool finished results wait within the budget", "[pool]") {
    constexpr std::size_t count = 400;
    constexpr unsigned threads = 4;
    constexpr std::uint64_t budget = 8;
    std::atomic<std::size_t> produced{0};
    Pool pool(
        count, threads, Pool::Limits{.window = 1000, .budget = budget},
        [&](std::size_t index) {
            ++produced;
            return index;
        },
        weigh_one);

    // A slow consumer: the pool runs as far ahead as it may, no further. What
    // is ahead is what is held plus what each thread has finished and offers.
    std::size_t most_ahead = 0;
    for (std::size_t i = 0; i < count; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        most_ahead = std::max(most_ahead, produced.load() - i);
        REQUIRE(pool.next() == i);
    }
    CHECK(most_ahead <= budget + threads + 1);
}

TEST_CASE("pool a result over the budget still gets through", "[pool]") {
    Pool pool(
        50, 4, Pool::Limits{.window = 16, .budget = 10},
        [](std::size_t index) { return index; },
        [](const std::size_t&) -> std::uint64_t { return 1000; });
    for (std::size_t i = 0; i < 50; ++i) {
        REQUIRE(pool.next() == i);
    }
}

TEST_CASE("pool an exception comes out at its index, after the earlier results", "[pool]") {
    Pool pool(
        100, 4, Pool::Limits{},
        [](std::size_t index) -> std::size_t {
            if (index == 7) {
                throw std::runtime_error("seven");
            }
            return index;
        },
        weigh_one);
    for (std::size_t i = 0; i < 7; ++i) {
        REQUIRE(pool.next() == i);
    }
    CHECK_THROWS_AS(pool.next(), std::runtime_error);
}

TEST_CASE("pool destroyed before its results are read does not hang", "[pool]") {
    std::atomic<std::size_t> produced{0};
    {
        Pool pool(
            100000, 4, Pool::Limits{.window = 32, .budget = 1ULL << 20},
            [&](std::size_t index) {
                ++produced;
                return index;
            },
            weigh_one);
        REQUIRE(pool.next() == 0);
        REQUIRE(pool.next() == 1);
    }
    // The window kept it from running on: 2 read, up to 32 ahead of them.
    CHECK(produced.load() <= 2 + 32 + 4);
}
