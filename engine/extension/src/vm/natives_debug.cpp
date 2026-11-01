// SPDX-License-Identifier: GPL-3.0-or-later
//
// Debug natives: Trace, Notification, MessageBox; and Message.Show.
#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"
#include "vm/natives_world.hpp"

namespace skydot::vm {

void bind_debug_natives(Vm& v, const WorldAccess& world, WorldEvents& events) {
    using namespace natives;
    v.bind("Debug", "Trace", [&events](NativeCall& c) {
        events.trace(Vm::to_string(arg(c, 0)));
        return NativeResult{};
    });
    for (const bool box : {false, true}) {
        v.bind("Debug", box ? "MessageBox" : "Notification", [&events, box](NativeCall& c) {
            events.message(Vm::to_string(arg(c, 0)), box, false);
            return NativeResult{};
        });
    }
    // The message's text with its arguments filled in goes out as `message`
    // (a box or a notification, as the record says). Nobody picks a button
    // yet: the first one is chosen. A message without text shows its title.
    const auto show = [&world, &events](NativeCall& c, const std::vector<double>& args, bool help) {
        MessageText m;
        if (!world.message(c.self.form(), m)) {
            c.vm.error("Message.Show: form is not a message");
            return result(Value::integer(0));
        }
        events.message(format_message(m.text.empty() ? m.title : m.text, args), m.box && !help, help);
        return result(Value::integer(0));
    };
    v.bind("Message", "Show", [show](NativeCall& c) {
        std::vector<double> args;
        for (std::size_t i = 0; i < 9; ++i) {
            args.push_back(number(arg(c, i)));
        }
        return show(c, args, false);
    });
    v.bind("Message", "ShowAsHelpMessage", [show](NativeCall& c) { return show(c, {}, true); });
}

} // namespace skydot::vm
