// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ranges the VM hands ids out of, and the allocator that keeps to them.
// Form ids made at run time share the 0xFF...... space with the game's own
// runtime references, so each kind owns a range that no other touches;
// running out of a range is an error (allocate() returns 0), never an
// overlap. A range that is nearly used up says so once (`take_pressure_warning`)
// for its owner to log; ids are never reused (a Value, a saved reference or a
// script may still hold a forgotten one, and a free list would change the
// save format). Sound instances are playback numbers, not forms, with a range of
// their own. Nothing here touches Godot.
#pragma once

#include <cstdint>

namespace skydot::vm {

/// An inclusive range of ids.
struct IdRange {
    std::uint32_t first;
    std::uint32_t last;
    [[nodiscard]] constexpr bool contains(std::uint32_t id) const noexcept { return id >= first && id <= last; }
};

namespace id_ranges {
/// Objects `create_instance` makes for tests (4,096).
inline constexpr IdRange test_instances{0xFF000000u, 0xFF000FFFu};
/// References PlaceAtMe makes (runtime_refs.hpp).
inline constexpr IdRange runtime_refs{0xFF001000u, 0xFF6FFFFFu};
/// Potions and poisons that alchemy creates (created_forms.hpp), 1,048,576.
inline constexpr IdRange created_forms{0xFF700000u, 0xFF7FFFFFu};
/// Handles for quest aliases (quest_states.hpp).
inline constexpr IdRange alias_handles{0xFF800000u, 0xFFEFFFFFu};
/// Instances of active magic effects (magic/active_effects.hpp), also the handles of the script
/// objects an effect's scripts make: 1,048,575.
inline constexpr IdRange effect_instances{0xFFF00000u, 0xFFFFFFFEu};
/// Sound playback instances: Sound.Play's and the game layer's own. Fits the
/// Int a script gets.
inline constexpr IdRange sound_instances{1u, 0x7FFFFFFFu};
} // namespace id_ranges

class IdAllocator {
public:
    explicit constexpr IdAllocator(IdRange range) noexcept : range_(range), next_(range.first) {}

    /// The next id, or 0 when the range is used up.
    [[nodiscard]] std::uint32_t allocate() noexcept {
        if (next_ > range_.last) {
            return 0;
        }
        return static_cast<std::uint32_t>(next_++);
    }
    /// The id allocate() hands out next (last + 1 once used up).
    [[nodiscard]] std::uint32_t next() const noexcept { return static_cast<std::uint32_t>(next_); }
    [[nodiscard]] bool exhausted() const noexcept { return next_ > range_.last; }
    /// How many ids were handed out (or noted as used), and how many the range holds.
    [[nodiscard]] std::uint64_t used() const noexcept { return next_ - range_.first; }
    [[nodiscard]] std::uint64_t capacity() const noexcept {
        return static_cast<std::uint64_t>(range_.last) - range_.first + 1;
    }
    /// True once, the first time it is asked after 90 % of the range is used: the owner logs a warning. It
    /// rearms with `clear` and `restore` (a load), so a game that is still above 90 % warns once more.
    [[nodiscard]] bool take_pressure_warning() noexcept {
        if (warned_ || used() * 10 < capacity() * 9) {
            return false;
        }
        warned_ = true;
        return true;
    }
    [[nodiscard]] const IdRange& range() const noexcept { return range_; }
    /// Continue from `next` (a saved state); false, and no change, if it is
    /// outside the range.
    bool restore(std::uint32_t next) noexcept {
        if (next < range_.first || (next > range_.last && next != range_.last + std::uint64_t{1})) {
            return false;
        }
        next_ = next;
        warned_ = false;
        return true;
    }
    /// An id handed out elsewhere (a loaded one): later ids lie above it.
    void note_used(std::uint32_t id) noexcept {
        if (range_.contains(id) && id >= next_) {
            next_ = id + std::uint64_t{1};
        }
    }
    void clear() noexcept {
        next_ = range_.first;
        warned_ = false;
    }

private:
    IdRange range_;
    std::uint64_t next_;
    bool warned_ = false;
};

} // namespace skydot::vm
