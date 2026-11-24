// SPDX-License-Identifier: GPL-3.0-or-later
//
// Argument and result helpers for natives; no Godot, so a unit test can link
// the natives that use them.
#pragma once

#include "vm/vm.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace skydot::natives {

/// Argument `i` of the call; None if the script passed fewer.
inline const vm::Value& arg(vm::NativeCall& c, std::size_t i) {
    static const vm::Value none;
    return i < c.args.size() ? c.args[i] : none;
}

/// Whether argument `i` is an object (None and missing are not).
inline bool is_object(vm::NativeCall& c, std::size_t i) { return arg(c, i).kind() == vm::Kind::object; }

/// Argument `i`'s form if it is an object, else 0.
inline std::uint32_t object_form(vm::NativeCall& c, std::size_t i) { return arg(c, i).form(); }

/// Argument `i` as an int, or `fallback` if the script passed fewer.
inline std::int32_t int_or(vm::NativeCall& c, std::size_t i, std::int32_t fallback) {
    return i < c.args.size() ? c.args[i].i() : fallback;
}

/// Argument `i` as a bool (see `Vm::truthy`), or `fallback` if the script passed fewer.
inline bool bool_or(vm::NativeCall& c, std::size_t i, bool fallback) {
    return i < c.args.size() ? vm::Vm::truthy(c.args[i]) : fallback;
}

/// The result of a native that does not wait.
inline vm::NativeResult result(vm::Value value) { return vm::NativeResult{.value = std::move(value)}; }

/// A bool result.
inline vm::NativeResult flag(bool x) { return result(vm::Value::boolean(x)); }

/// An int or float value as a double; 0 for anything else.
inline double number(const vm::Value& x) {
    return x.kind() == vm::Kind::floating ? static_cast<double>(x.f())
                                          : (x.kind() == vm::Kind::integer ? static_cast<double>(x.i()) : 0.0);
}

/// Argument `i` as a number (see `number`), or `fallback` if the script passed fewer.
inline double float_or(vm::NativeCall& c, std::size_t i, double fallback) {
    return i < c.args.size() ? number(c.args[i]) : fallback;
}

// Objective state bits.
constexpr std::uint8_t k_displayed = 0x1;
constexpr std::uint8_t k_completed = 0x2;
constexpr std::uint8_t k_failed = 0x4;

// GetOpenState values. Source: the Creation Kit wiki's GetOpenState page.
constexpr std::int64_t k_open = 1;
constexpr std::int64_t k_closed = 3;

} // namespace skydot::natives
