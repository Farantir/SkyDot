// SPDX-License-Identifier: GPL-3.0-or-later
//
// Names the VM looks up: functions by state and name, properties by name. A
// name that is looked up on every call (an instruction's, an instance's state)
// is hashed once, when it is read or set, and carries its hash as a `Name`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::vm {

/// The hash lookups use for a lowercase name or state.
[[nodiscard]] constexpr std::size_t name_hash(std::string_view text) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ULL; // FNV-1a
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001B3ULL;
    }
    return static_cast<std::size_t>(h ^ (h >> 29));
}

/// A lowercase name with its hash. A plain string converts, hashing it.
struct Name {
    std::string_view text;
    std::size_t hash;

    constexpr Name(std::string_view t) noexcept : text(t), hash(name_hash(t)) {}
    constexpr Name(const char* t) noexcept : Name(std::string_view(t)) {}
    Name(const std::string& t) noexcept : Name(std::string_view(t)) {}
    constexpr Name(std::string_view t, std::size_t h) noexcept : text(t), hash(h) {}
};

/// The hash of a function's state and name together.
[[nodiscard]] constexpr std::size_t function_hash(const Name& state, const Name& name) noexcept {
    return name.hash ^ (state.hash * 0x9E3779B97F4A7C15ULL);
}

/// An open-addressed table from a hash to the pointer it was put under. The
/// key itself is whatever the pointee says it is: `find` and `put` take a
/// predicate telling whether a pointee is the one wanted.
template <class T>
class HashTable {
public:
    template <class Same>
    [[nodiscard]] const T* find(std::size_t hash, const Same& same) const {
        if (slots_.empty()) {
            return nullptr;
        }
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t i = hash & mask;; i = (i + 1) & mask) {
            const Slot& slot = slots_[i];
            if (slot.ptr == nullptr) {
                return nullptr;
            }
            if (slot.hash == hash && same(*slot.ptr)) {
                return slot.ptr;
            }
        }
    }

    /// Put `ptr` under `hash`, replacing the entry `same` calls the same.
    template <class Same>
    void put(std::size_t hash, const T* ptr, const Same& same) {
        if ((count_ + 1) * 2 > slots_.size()) {
            grow();
        }
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t i = hash & mask;; i = (i + 1) & mask) {
            Slot& slot = slots_[i];
            if (slot.ptr == nullptr) {
                slot = {hash, ptr};
                ++count_;
                return;
            }
            if (slot.hash == hash && same(*slot.ptr)) {
                slot.ptr = ptr;
                return;
            }
        }
    }

private:
    struct Slot {
        std::size_t hash = 0;
        const T* ptr = nullptr;
    };

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.empty() ? 8 : old.size() * 2, Slot{});
        const std::size_t mask = slots_.size() - 1;
        for (const Slot& s : old) {
            if (s.ptr == nullptr) {
                continue;
            }
            std::size_t i = s.hash & mask;
            while (slots_[i].ptr != nullptr) {
                i = (i + 1) & mask;
            }
            slots_[i] = s;
        }
    }

    std::vector<Slot> slots_;
    std::size_t count_ = 0;
};

} // namespace skydot::vm
