// SPDX-License-Identifier: GPL-3.0-or-later
//
// Godot helpers shared by the papyrus*.cpp files (the natives_* files must stay Godot-free).
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

} // namespace skydot::natives
