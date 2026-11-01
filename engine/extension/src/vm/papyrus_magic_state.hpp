// SPDX-License-Identifier: GPL-3.0-or-later
//
// Private to the papyrus_*.cpp files of the magic glue (papyrus_magic.cpp, papyrus_casting.cpp,
// papyrus_shouts.cpp, papyrus_enchantment_effects.cpp): the Godot helpers they share. The magic
// itself is vm/magic_runtime.hpp. Do not include it elsewhere.
#pragma once

#include "vm/papyrus.hpp"
#include "vm/magic_runtime.hpp"

#include "vm/condition_world.hpp"

#include "data/scripts.hpp"
#include "conditions/functions.hpp"
#include "data/fb_search.hpp"
#include "data/actors.hpp"
#include "data/leveled.hpp"
#include "data/world_data.hpp"
#include "magic/active_effects.hpp"
#include "magic/casting.hpp"
#include "magic/enchanted.hpp"
#include "magic/finish_grace.hpp"
#include "magic/magic_save.hpp"
#include "magic/shouts.hpp"
#include "magic/spell_lists.hpp"
#include "vm/natives_magic.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace skydot {

namespace magic_glue {

namespace wfb = bethconv::pack::wfb;

inline String to_godot(const std::string& s) { return String::utf8(s.c_str(), static_cast<int>(s.size())); }

inline std::string hex(std::uint32_t v) { return vm::magic_hex(v); }

inline std::string number(double v) { return vm::magic_number(v); }

inline constexpr int kind_value(magic::SourceKind k) { return static_cast<int>(k); }


/// The common start of the API's casts: ok false with the reason when `caster` cannot act.
inline bool caster_can_act(const vm::ActorStates& actors, std::uint32_t caster, Dictionary& out) {
    if (!actors.is_actor(caster)) {
        out["reason"] = "no caster";
        return false;
    }
    if (actors.is_dead(caster)) {
        out["reason"] = "dead";
        return false;
    }
    return true;
}


} // namespace magic_glue

using namespace magic_glue;

} // namespace skydot
