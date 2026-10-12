// SPDX-License-Identifier: GPL-3.0-or-later
//
// EffectShader natives.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"

namespace skydot {

void SkydotPapyrus::bind_effectshader_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    [[maybe_unused]] auto& v = *vm_;
    [[maybe_unused]] const auto arg = natives::arg;
    [[maybe_unused]] const auto result = natives::result;
    [[maybe_unused]] const auto number = natives::number;
    v.bind("EffectShader", "Play", [this, arg](NativeCall& c) {
        emit_signal("effect_shader", static_cast<std::int64_t>(c.self.form()),
                    static_cast<std::int64_t>(arg(c, 0).form()), true);
        return NativeResult{};
    });
    v.bind("EffectShader", "Stop", [this, arg](NativeCall& c) {
        emit_signal("effect_shader", static_cast<std::int64_t>(c.self.form()),
                    static_cast<std::int64_t>(arg(c, 0).form()), false);
        return NativeResult{};
    });
}

} // namespace skydot
