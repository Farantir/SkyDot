// SPDX-License-Identifier: GPL-3.0-or-later
//
// `bethconv form --magic`: which magic effect archetypes a pack's spells,
// potions, scrolls, enchantments and ingredients use, over the whole world and
// over the slice the engine's first milestone covers.
#pragma once

#include "bethconv/pack/world.hpp"

namespace bethconv::cli {

/// Prints the census to stdout.
void print_magic_census(const pack::WorldFile& world);

} // namespace bethconv::cli
