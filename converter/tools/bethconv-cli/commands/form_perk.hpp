// SPDX-License-Identifier: GPL-3.0-or-later
//
// `bethconv form --perk`, `--avif`, `--class` and `--perk-census`: perks with their effects,
// skills with their use and improve values and perk tree, classes, and a count of the perk
// effects by kind and entry point.
#pragma once

#include "bethconv/pack/world.hpp"

#include <cstdint>
#include <string>

namespace bethconv::cli {

void print_perk(const pack::WorldFile& world, std::uint32_t id);
/// `what` is a FormID (0x...) or a skill name ("OneHanded", "AVOneHanded", "Smithing").
void print_avif(const pack::WorldFile& world, const std::string& what);
void print_class(const pack::WorldFile& world, std::uint32_t id);
void print_perk_census(const pack::WorldFile& world);

} // namespace bethconv::cli
