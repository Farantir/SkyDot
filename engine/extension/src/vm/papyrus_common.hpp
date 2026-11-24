// SPDX-License-Identifier: GPL-3.0-or-later
//
// Godot helpers shared by the papyrus*.cpp files (the natives_* files must stay Godot-free).
#pragma once

#include "data/godot_values.hpp"
#include "data/text.hpp"
#include "vm/natives_args.hpp"
#include "vm/vm.hpp"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string>

namespace skydot::natives {

using skydot::to_godot;
using skydot::utf8;

} // namespace skydot::natives
