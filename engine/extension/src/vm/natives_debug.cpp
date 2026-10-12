// SPDX-License-Identifier: GPL-3.0-or-later
//
// Debug natives: Trace, Notification, MessageBox.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"
#include <godot_cpp/variant/utility_functions.hpp>

using godot::String;

namespace skydot {

void SkydotPapyrus::bind_debug_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    [[maybe_unused]] auto& v = *vm_;
    [[maybe_unused]] const auto arg = natives::arg;
    [[maybe_unused]] const auto result = natives::result;
    [[maybe_unused]] const auto number = natives::number;
    v.bind("Debug", "Trace", [this, arg](NativeCall& c) {
        const String text = to_godot(vm::Vm::to_string(arg(c, 0)));
        godot::UtilityFunctions::print(String("[Papyrus] ") + text);
        emit_signal("trace", text);
        return NativeResult{};
    });
    for (const bool box : {false, true}) {
        v.bind("Debug", box ? "MessageBox" : "Notification", [this, arg, box](NativeCall& c) {
            const String text = to_godot(vm::Vm::to_string(arg(c, 0)));
            godot::UtilityFunctions::print(String(box ? "[Message] " : "[Notification] ") + text);
            emit_signal("message", text, box);
            return NativeResult{};
        });
    }
}

} // namespace skydot
