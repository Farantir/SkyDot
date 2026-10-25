// SPDX-License-Identifier: GPL-3.0-or-later
//
// Form natives: identity and update timers.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"
#include <algorithm>

namespace skydot {

void SkydotPapyrus::bind_form_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    [[maybe_unused]] auto& v = *vm_;
    [[maybe_unused]] const auto arg = natives::arg;
    [[maybe_unused]] const auto result = natives::result;
    [[maybe_unused]] const auto number = natives::number;
    v.bind("Form", "GetFormID", [result](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(c.self.form())));
    });
    const auto updates = [arg, number](NativeCall& c, bool repeat) {
        if (c.self.instance() == nullptr) {
            c.vm.error("RegisterForUpdate needs a script to send OnUpdate to");
            return NativeResult{};
        }
        const double interval = std::max(0.0, number(arg(c, 0)));
        c.vm.register_update(c.self.instance(), interval, repeat ? std::max(interval, 0.001) : 0.0);
        return NativeResult{};
    };
    v.bind("Form", "RegisterForSingleUpdate", [updates](NativeCall& c) { return updates(c, false); });
    v.bind("Form", "RegisterForUpdate", [updates](NativeCall& c) { return updates(c, true); });
    v.bind("Form", "UnregisterForUpdate", [](NativeCall& c) {
        if (c.self.instance() != nullptr) {
            c.vm.unregister_update(c.self.instance());
        }
        return NativeResult{};
    });

    // Animation events: a form, an alias or a running magic effect asks for OnAnimationEvent when a
    // reference's animation raises a named event (see notify_animation_event). Skyrim refuses a sender
    // without 3D; the registration is one per (script, sender, event).
    for (const char* cls : {"Form", "Alias", "ActiveMagicEffect"}) {
        v.bind(cls, "RegisterForAnimationEvent", [this, arg, result](NativeCall& c) {
            const auto sender = arg(c, 0).form();
            if (c.self.instance() == nullptr || sender == 0 || !is_3d_loaded(sender)) {
                return result(Value::boolean(false));
            }
            c.vm.register_animation_event(c.self.instance(), sender, arg(c, 1).s());
            emit_signal("animation_events_wanted", static_cast<std::int64_t>(sender));
            return result(Value::boolean(true));
        });
        v.bind(cls, "UnregisterForAnimationEvent", [arg](NativeCall& c) {
            if (c.self.instance() != nullptr) {
                c.vm.unregister_animation_event(c.self.instance(), arg(c, 0).form(), arg(c, 1).s());
            }
            return NativeResult{};
        });
    }
}

} // namespace skydot
