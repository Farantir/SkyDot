// SPDX-License-Identifier: GPL-3.0-or-later
//
// Weapon natives.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"

namespace skydot {

void SkydotPapyrus::bind_weapon_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using namespace natives;
    [[maybe_unused]] auto& v = *vm_;
    [[maybe_unused]] const auto arg = natives::arg;
    // Fire(akSource, akAmmo): the game layer launches the ammo's projectile
    // from the source. Both may be 0 (None); the game layer decides what that
    // means for the weapon.
    v.bind("Weapon", "Fire", [this, arg](NativeCall& c) {
        emit_signal("fire_weapon", static_cast<std::int64_t>(c.self.form()),
                    static_cast<std::int64_t>(arg(c, 0).form()),
                    static_cast<std::int64_t>(arg(c, 1).form()));
        return NativeResult{};
    });
}

} // namespace skydot
