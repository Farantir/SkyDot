// SPDX-License-Identifier: GPL-3.0-or-later
//
// Conversions between world.fb's strings and ids and Godot's types, which the
// readers of world.fb all need.
#pragma once

#include <flatbuffers/flatbuffers.h>

#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace skydot {

/// A FlatBuffers string (UTF-8) as a Godot string; empty for null.
inline godot::String to_godot(const flatbuffers::String* s) {
    return s == nullptr ? godot::String() : godot::String::utf8(s->c_str(), static_cast<int>(s->size()));
}

/// A Godot string as UTF-8 bytes.
inline std::string utf8(const godot::String& s) {
    const auto bytes = s.utf8();
    return {bytes.get_data(), static_cast<std::size_t>(bytes.length())};
}

/// A model's virtual path as the asset cache keys it.
inline godot::String model_path(std::string_view model) {
    return godot::String::utf8(model.data(), static_cast<int>(model.size()));
}

/// A form id as "0x0001A2B3".
inline godot::String hex_id(std::uint32_t id) {
    return godot::String("0x") + godot::String::num_uint64(id, 16, true).lpad(8, "0");
}

/// RGBA bytes packed little-endian (red in the low byte); alpha is unused.
inline godot::Color unpack_color(std::uint32_t rgba) {
    return godot::Color(static_cast<float>(rgba & 0xFFu) / 255.0F,
                        static_cast<float>((rgba >> 8) & 0xFFu) / 255.0F,
                        static_cast<float>((rgba >> 16) & 0xFFu) / 255.0F);
}

} // namespace skydot
