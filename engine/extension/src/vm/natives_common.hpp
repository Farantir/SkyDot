// SPDX-License-Identifier: GPL-3.0-or-later
//
// Helpers shared by the natives_<class>.cpp files.
#pragma once

#include "data/godot_values.hpp"
#include "vm/vm.hpp"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string>

namespace skydot::natives {

/// Argument `i` of the call; None if the script passed fewer.
inline const vm::Value& arg(vm::NativeCall& c, std::size_t i) {
    static const vm::Value none;
    return i < c.args.size() ? c.args[i] : none;
}

inline vm::NativeResult result(vm::Value value) {
    return vm::NativeResult{.value = std::move(value), .wait = -1.0};
}

/// An int or float value as a double; 0 for anything else.
inline double number(const vm::Value& x) {
    return x.kind() == vm::Kind::floating ? static_cast<double>(x.f())
                                          : (x.kind() == vm::Kind::integer ? static_cast<double>(x.i()) : 0.0);
}

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
