// SPDX-License-Identifier: GPL-3.0-or-later
//
// What scripts changed about a reference, kept per reference and saved as the
// RFST section: enabled or disabled, the open state, locked or not, activation
// blocked, and where a script moved it to (Skyrim units, rotation in radians).
// Replaces the maps SkydotPapyrus kept for each of these. Nothing here touches
// Godot.
#pragma once

#include "save/bytes.hpp"

#include <vector>

#include <cstdint>
#include <map>
#include <set>

namespace skydot::vm {

struct MovedTo {
    double position[3] = {0.0, 0.0, 0.0};
    double rotation[3] = {0.0, 0.0, 0.0};
};

struct RefStates {
    static constexpr std::uint32_t k_section_version = 1;

    std::map<std::uint32_t, bool> disabled; ///< ref -> disabled (false: enabled by a script)
    std::map<std::uint32_t, std::int64_t> open; ///< GetOpenState values
    std::map<std::uint32_t, bool> locked;
    std::set<std::uint32_t> blocked;
    std::map<std::uint32_t, MovedTo> moved;

    /// Drop everything known about `ref`.
    void forget(std::uint32_t ref) {
        disabled.erase(ref);
        open.erase(ref);
        locked.erase(ref);
        blocked.erase(ref);
        moved.erase(ref);
    }
    void clear() {
        disabled.clear();
        open.clear();
        locked.clear();
        blocked.clear();
        moved.clear();
    }

    [[nodiscard]] std::vector<std::uint8_t> save() const {
        save::ByteWriter w;
        w.u32(static_cast<std::uint32_t>(disabled.size()));
        for (const auto& [ref, value] : disabled) {
            w.u32(ref);
            w.u8(value ? 1 : 0);
        }
        w.u32(static_cast<std::uint32_t>(open.size()));
        for (const auto& [ref, value] : open) {
            w.u32(ref);
            w.u64(static_cast<std::uint64_t>(value));
        }
        w.u32(static_cast<std::uint32_t>(locked.size()));
        for (const auto& [ref, value] : locked) {
            w.u32(ref);
            w.u8(value ? 1 : 0);
        }
        w.u32(static_cast<std::uint32_t>(blocked.size()));
        for (const auto ref : blocked) {
            w.u32(ref);
        }
        w.u32(static_cast<std::uint32_t>(moved.size()));
        for (const auto& [ref, to] : moved) {
            w.u32(ref);
            for (const double v : to.position) {
                w.f64(v);
            }
            for (const double v : to.rotation) {
                w.f64(v);
            }
        }
        return w.out;
    }

    /// Decode a section into a fresh object; false if it is cut short or
    /// has bytes left over.
    [[nodiscard]] static bool load(save::ByteReader r, RefStates& out) {
        out = RefStates{};
        std::uint32_t n = 0;
        if (!r.count(n, 5)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t ref = 0;
            std::uint8_t v = 0;
            if (!r.u32(ref) || !r.u8(v)) {
                return false;
            }
            out.disabled[ref] = v != 0;
        }
        if (!r.count(n, 12)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t ref = 0;
            std::uint64_t v = 0;
            if (!r.u32(ref) || !r.u64(v)) {
                return false;
            }
            out.open[ref] = static_cast<std::int64_t>(v);
        }
        if (!r.count(n, 5)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t ref = 0;
            std::uint8_t v = 0;
            if (!r.u32(ref) || !r.u8(v)) {
                return false;
            }
            out.locked[ref] = v != 0;
        }
        if (!r.count(n, 4)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t ref = 0;
            if (!r.u32(ref)) {
                return false;
            }
            out.blocked.insert(ref);
        }
        if (!r.count(n, 4 + 6 * 8)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t ref = 0;
            MovedTo to;
            bool ok = r.u32(ref);
            for (double& v : to.position) {
                ok = ok && r.f64(v);
            }
            for (double& v : to.rotation) {
                ok = ok && r.f64(v);
            }
            if (!ok) {
                return false;
            }
            out.moved[ref] = to;
        }
        return r.at_end();
    }
};

} // namespace skydot::vm
