// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's shaders are text files in the Godot project (game/shaders/, so
// res://shaders/ and, in an exported game, inside the .pck), not string
// literals in C++. The C++ that assembles a shader variant (materials.cpp,
// terrain.cpp, lod.cpp, ...) asks for the file here and adds its defines,
// render modes, fog and ambient exactly as it did to the literal.
//
// The files are read as plain text through FileAccess, so they need no import
// step and work in headless runs. game/shaders/README.md describes them.
#pragma once

#include <string>

namespace skydot::shader_source {

/// The text of res://shaders/`name`, without its header comment: a file may
/// start with a block comment saying which C++ assembles it and how, and that
/// comment and the newline after it are not shader code. Read once and kept;
/// thread safe. A file that is missing or unreadable is reported with
/// push_error, once, and gives an empty string: an empty shader.
const std::string& load(const char* name);

} // namespace skydot::shader_source
