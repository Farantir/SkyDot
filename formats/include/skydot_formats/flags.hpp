// SPDX-License-Identifier: GPL-3.0-or-later
//
// Testing a flag word of the pack schemas. The `bit_flags` enums flatc
// generates come with `|` and `&` but no way to ask whether a bit is set.
#pragma once

#include <type_traits>

namespace skydot::formats {

/// Whether any bit of `bits` is set in `flags`: for one bit, that bit. `E` is
/// a generated `bit_flags` enum, whose operators are found through its
/// namespace.
template <class E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr bool has_flag(E flags, E bits) noexcept {
    return (flags & bits) != E{};
}

} // namespace skydot::formats
