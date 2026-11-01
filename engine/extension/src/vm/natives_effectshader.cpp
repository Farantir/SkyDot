// SPDX-License-Identifier: GPL-3.0-or-later
//
// EffectShader natives.
#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

namespace skydot::vm {

void bind_effectshader_natives(Vm& v, WorldEvents& events) {
    using namespace natives;
    v.bind("EffectShader", "Play", [&events](NativeCall& c) {
        events.effect_shader(c.self.form(), arg(c, 0).form(), true);
        return NativeResult{};
    });
    v.bind("EffectShader", "Stop", [&events](NativeCall& c) {
        events.effect_shader(c.self.form(), arg(c, 0).form(), false);
        return NativeResult{};
    });
}

} // namespace skydot::vm
