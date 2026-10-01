// SPDX-License-Identifier: GPL-3.0-or-later
//
// Script assets: a decoded `.pex` as a FlatBuffer (schema formats/schema/script.fbs),
// so the engine runs scripts without parsing Bethesda's format.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/script/pex.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bethconv::pack {

/// Bumped whenever the meaning of anything in script.fbs changes.
inline constexpr std::uint32_t k_script_format_version = 1;

[[nodiscard]] std::vector<std::byte> write_script_asset(const script::PexScript& script);

/// Verify a script asset and read it back into the PEX model (without
/// docstrings, which are not stored).
[[nodiscard]] io::ParseResult<script::PexScript> read_script_asset(std::span<const std::byte> bytes,
                                                                   std::string_view origin);

} // namespace bethconv::pack
