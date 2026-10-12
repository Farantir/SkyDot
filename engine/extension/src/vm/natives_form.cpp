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

}

} // namespace skydot
