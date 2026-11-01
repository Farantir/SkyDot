// SPDX-License-Identifier: GPL-3.0-or-later
//
// Weapon natives.
#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

namespace skydot::vm {

void bind_weapon_natives(Vm& v, WorldEvents& events) {
    using namespace natives;
    // Fire(akSource, akAmmo): the game layer launches the ammo's projectile
    // from the source. Both may be 0 (None); the game layer decides what that
    // means for the weapon.
    v.bind("Weapon", "Fire", [&events](NativeCall& c) {
        events.fire_weapon(c.self.form(), arg(c, 0).form(), arg(c, 1).form());
        return NativeResult{};
    });
}

} // namespace skydot::vm
