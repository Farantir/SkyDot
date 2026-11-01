// SPDX-License-Identifier: GPL-3.0-or-later
//
// Argument and result helpers for natives; no Godot, so a unit test can link
// the natives that use them.
#pragma once

#include "vm/vm.hpp"

#include <cstddef>
#include <utility>

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

} // namespace skydot::natives
