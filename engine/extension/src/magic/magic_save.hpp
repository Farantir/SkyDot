// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MAGC section of a save (docs/adr/0002, 0008): the running effects (abilities not among them),
// the spells added and removed per actor, the equipped spells, the power cooldowns and the known
// enchantments. Optional on load: a save without it has no effect running and no spell changed.
// Layout: u32 version, then the ActiveEffects bytes and the SpellBook bytes and (version 2) the
// ShoutBook bytes, each as a text (u32 length + bytes). The temporary modifiers the effects hold are not in ACTV; `ActiveEffects::load`
// holds them again. Nothing here touches Godot.
#pragma once

#include "magic/active_effects.hpp"
#include "magic/shouts.hpp"
#include "magic/spell_lists.hpp"

#include <cstdint>
#include <vector>

namespace skydot::magic {

inline constexpr std::uint32_t k_magc_version = 2;
/// The oldest version `read_magc` still reads (1: no ShoutBook part, so no shouts).
inline constexpr std::uint32_t k_magc_min_version = 1;

[[nodiscard]] std::vector<std::uint8_t> write_magc(const ActiveEffects& effects, const SpellBook& book, const ShoutBook& shouts);

struct MagcParts {
    std::vector<std::uint8_t> effects;
    std::vector<std::uint8_t> book;
    /// Empty for a version 1 section (decodes as an empty ShoutBook).
    std::vector<std::uint8_t> shouts;
};

/// Split a section (version 1 or 2); false if it is cut short or has bytes left over.
[[nodiscard]] bool read_magc(save::ByteReader r, MagcParts& out);
/// Whether all parts decode against `world`, changing nothing live.
[[nodiscard]] bool check_magc(const wfb::World& world, const MagcParts& parts);

} // namespace skydot::magic
