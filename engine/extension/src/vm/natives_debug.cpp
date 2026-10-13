// SPDX-License-Identifier: GPL-3.0-or-later
//
// Debug natives: Trace, Notification, MessageBox; and Message.Show.
#include "vm/natives_common.hpp"
#include "vm/natives_world.hpp"
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
    // The message's text with its arguments filled in goes out as `message`
    // (a box or a notification, as the record says). Nobody picks a button
    // yet: the first one is chosen. A message without text shows its title.
    const auto show = [this](NativeCall& c, const std::vector<double>& args, bool help) {
        vm::MessageText m;
        if (access_ == nullptr || !access_->message(c.self.form(), m)) {
            c.vm.error("Message.Show: form is not a message");
            return natives::result(Value::integer(0));
        }
        const String text = to_godot(vm::format_message(m.text.empty() ? m.title : m.text, args));
        const bool box = m.box && !help;
        godot::UtilityFunctions::print(String(box ? "[Message] " : help ? "[Help] " : "[Notification] ") + text);
        emit_signal("message", text, box);
        return natives::result(Value::integer(0));
    };
    v.bind("Message", "Show", [show, arg, number](NativeCall& c) {
        std::vector<double> args;
        for (std::size_t i = 0; i < 9; ++i) {
            args.push_back(number(arg(c, i)));
        }
        return show(c, args, false);
    });
    v.bind("Message", "ShowAsHelpMessage", [show](NativeCall& c) { return show(c, {}, true); });
}

} // namespace skydot
