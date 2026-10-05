// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs `produce(i)` for i = 0 .. count-1 on a fixed set of threads and hands the
// results to one consumer in index order, whatever order they finish in. This is
// how convert() runs read, hash and conversion on every core while the pack
// writer still sees the work list in order. Private to pack/.
//
// Memory stays bounded two ways: a thread starts index i only while i is less
// than `window` ahead of the consumer, and it does not hand over a result while
// the finished ones waiting weigh more than `budget`. The result the consumer is
// waiting for is always accepted, so a result heavier than the budget cannot
// stall the pool.
//
// An exception thrown by `produce` comes out of `next()` at that index, as it
// would have in a single thread; the later results are dropped.
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace bethconv::pack {

template <typename Result>
class OrderedPool {
public:
    struct Limits {
        std::size_t window = 1024;
        std::uint64_t budget = 256ULL << 20;
    };

    /// `weight` says what a finished result holds (bytes), for `Limits::budget`.
    /// `threads` of at least one.
    OrderedPool(std::size_t count, unsigned threads, Limits limits,
                std::function<Result(std::size_t)> produce,
                std::function<std::uint64_t(const Result&)> weight)
        : count_(count), window_(std::max<std::size_t>(limits.window, 1)),
          budget_(limits.budget), produce_(std::move(produce)), weight_(std::move(weight)),
          slots_(window_) {
        workers_.reserve(threads);
        for (unsigned t = 0; t < threads; ++t) {
            workers_.emplace_back([this] { run(); });
        }
    }

    ~OrderedPool() {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        room_.notify_all();
        workers_.clear(); // joins
    }

    OrderedPool(const OrderedPool&) = delete;
    OrderedPool& operator=(const OrderedPool&) = delete;

    /// The next result in index order; waits for it. At most `count` calls.
    [[nodiscard]] Result next() {
        std::unique_lock lock(mutex_);
        Slot& slot = slots_[consumed_ % window_];
        ready_.wait(lock, [&] { return slot.filled; });
        Slot taken = std::move(slot);
        slot = Slot{};
        held_ -= taken.weight;
        ++consumed_;
        lock.unlock();
        room_.notify_all();
        if (taken.error) {
            std::rethrow_exception(taken.error);
        }
        return std::move(*taken.value);
    }

private:
    struct Slot {
        bool filled = false;
        std::optional<Result> value;
        std::exception_ptr error;
        std::uint64_t weight = 0;
    };

    void run() {
        for (;;) {
            std::size_t index = 0;
            {
                std::unique_lock lock(mutex_);
                room_.wait(lock, [&] {
                    return stopping_ || next_ >= count_ || next_ < consumed_ + window_;
                });
                if (stopping_ || next_ >= count_) {
                    return;
                }
                index = next_++;
            }

            Slot done;
            done.filled = true;
            try {
                done.value.emplace(produce_(index));
                done.weight = weight_(*done.value);
            } catch (...) {
                done.value.reset();
                done.error = std::current_exception();
            }

            {
                std::unique_lock lock(mutex_);
                room_.wait(lock, [&] {
                    return stopping_ || index == consumed_ || held_ + done.weight <= budget_;
                });
                if (stopping_) {
                    return;
                }
                held_ += done.weight;
                slots_[index % window_] = std::move(done);
            }
            ready_.notify_one();
        }
    }

    const std::size_t count_;
    const std::size_t window_;
    const std::uint64_t budget_;
    const std::function<Result(std::size_t)> produce_;
    const std::function<std::uint64_t(const Result&)> weight_;

    std::mutex mutex_;
    std::condition_variable ready_; ///< The consumer's next slot is filled.
    std::condition_variable room_;  ///< The window moved or weight was released.
    std::vector<Slot> slots_;       ///< Ring over [consumed_, consumed_ + window_).
    std::size_t next_ = 0;          ///< The next index to start.
    std::size_t consumed_ = 0;
    std::uint64_t held_ = 0;
    bool stopping_ = false;

    std::vector<std::jthread> workers_; // last: joined first
};

} // namespace bethconv::pack
