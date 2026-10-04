// SPDX-License-Identifier: GPL-3.0-or-later
//
// The spelling of a virtual path in vpath.idx, and ASCII lowercasing: the
// shared definition (formats/include/skydot_formats/vpath.hpp, which the
// converter writes the index with), under the names the engine uses.
//
// Both are locale-independent on purpose: `std::tolower` and Godot's
// `String::to_lower` change letters the converter leaves alone (Turkish
// dotless i, "Ä"), and then a path no longer finds its asset.
#pragma once

#include "skydot_formats/vpath.hpp"

namespace skydot {

using formats::ascii_lower;
using formats::normalize_vpath;

} // namespace skydot
