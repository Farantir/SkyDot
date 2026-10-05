// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's shaders are text files in the Godot project (game/shaders/, so
// res://shaders/ and, in an exported game, inside the .pck), not string
// literals in C++. Godot's own shader preprocessor assembles them: a variant
// is a stub, `shader_type spatial;`, a few `#define`s and an `#include` of the
// file whose `#ifdef`s and `#if`s pick the render modes and code (variant()),
// and a shader that has no variants is a .gdshader the project loads as it is
// (shader()). The C++ only chooses the variant. The compute shaders of the
// image space are GLSL for the RenderingDevice, which has no such
// preprocessor; load() reads their text.
//
// game/shaders/README.md describes the files.
#pragma once

#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <vector>

namespace skydot::shader_source {

/// The code of one variant of the fragment res://shaders/`file` (a
/// .gdshaderinc): `shader_type spatial;`, one `#define` for each of `defines`
/// ("NAME", or "NAME value") and `#include "res://shaders/<file>"`. Godot's
/// preprocessor does the rest when the code is given to a Shader. Makes sure
/// the game's fog globals exist, which every variant may compile against.
std::string variant(const char* file, const std::vector<std::string>& defines = {});

/// The Shader of the whole shader file res://shaders/`name` (.gdshader), as
/// Godot loads it: once per path, with its `#include`s resolved by the
/// preprocessor. Makes sure the game's fog globals exist. A missing file is
/// reported by Godot, and gives a null Ref.
godot::Ref<godot::Shader> shader(const char* name);

/// The code of the shader file `name` as written, `#include`s not expanded, or
/// an empty string if it is missing.
godot::String code(const char* name);

/// The text of res://shaders/`name` (a .comp file), without its header
/// comment: a file may start with a block comment saying which C++ uses it,
/// and that comment and the newline after it are not shader code. Read once
/// and kept; thread safe. A file that is missing or unreadable is reported
/// with push_error, once, and gives an empty string: an empty shader.
const std::string& load(const char* name);

} // namespace skydot::shader_source
