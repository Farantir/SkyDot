// SPDX-License-Identifier: GPL-3.0-or-later
//
// Helpers shared by the natives_<class>.cpp files.
#pragma once

#include "data/godot_values.hpp"
#include "vm/natives_args.hpp"
#include "vm/vm.hpp"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string>

namespace skydot::natives {

inline godot::String to_godot(const std::string& s) {
    return godot::String::utf8(s.c_str(), static_cast<int>(s.size()));
}

inline std::string utf8(const godot::String& s) {
    const auto bytes = s.utf8();
    return {bytes.get_data(), static_cast<std::size_t>(bytes.length())};
}

// Objective state bits.
constexpr std::uint8_t k_displayed = 0x1;
constexpr std::uint8_t k_completed = 0x2;
constexpr std::uint8_t k_failed = 0x4;

// GetOpenState values. Source: the Creation Kit wiki's GetOpenState page.
constexpr std::int64_t k_open = 1;
constexpr std::int64_t k_closed = 3;

} // namespace skydot::natives
