// SPDX-License-Identifier: GPL-3.0-or-later
//
// Script records of world.fb as Godot values: what SkydotWorld describes a
// form's scripts with, and the scripts a reference carries itself.
#pragma once

#include "data/fb_search.hpp"
#include "world_generated.h"

#include <godot_cpp/variant/array.hpp>

#include <cstdint>

namespace skydot {

using ScriptVector = flatbuffers::Vector<flatbuffers::Offset<bethconv::pack::wfb::Script>>;

/// The scripts reference `ref` of `cell` carries itself (VMAD), or null.
inline const bethconv::pack::wfb::RefScripts* ref_scripts(const bethconv::pack::wfb::Cell& cell,
                                                          std::uint32_t ref) {
    return lookup(cell.scripts(), ref);
}

/// Scripts as SkydotWorld::get_ref_info describes them.
godot::Array script_list(const ScriptVector* scripts, bool from_ref);

} // namespace skydot
