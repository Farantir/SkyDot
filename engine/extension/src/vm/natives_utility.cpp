// SPDX-License-Identifier: GPL-3.0-or-later
//
// Utility natives: Wait, real time and random numbers (GetCurrentGameTime reads
// the game clock and is in natives_world.cpp).
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"
#include <algorithm>

namespace skydot {

void SkydotPapyrus::bind_utility_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    [[maybe_unused]] auto& v = *vm_;
    [[maybe_unused]] const auto arg = natives::arg;
    [[maybe_unused]] const auto result = natives::result;
    [[maybe_unused]] const auto number = natives::number;
    v.bind("Utility", "Wait", [arg, number](NativeCall& c) {
        return NativeResult{.value = {}, .wait = std::max(0.0, number(arg(c, 0)))};
    });
    v.bind("Utility", "GetCurrentRealTime", [result](NativeCall& c) {
        return result(Value::floating(static_cast<float>(c.vm.time())));
    });
    // Both bounds inclusive, as documented for RandomInt.
    v.bind("Utility", "RandomInt", [arg, result](NativeCall& c) {
        auto lo = c.args.empty() ? 0 : arg(c, 0).i();
        auto hi = c.args.size() < 2 ? 100 : arg(c, 1).i();
        if (lo > hi) {
            std::swap(lo, hi);
        }
        const auto span = static_cast<std::uint64_t>(static_cast<std::int64_t>(hi) - lo) + 1;
        return result(Value::integer(static_cast<std::int32_t>(lo + static_cast<std::int64_t>(c.vm.random() % span))));
    });
    v.bind("Utility", "RandomFloat", [arg, result, number](NativeCall& c) {
        const double lo = c.args.empty() ? 0.0 : number(arg(c, 0));
        const double hi = c.args.size() < 2 ? 1.0 : number(arg(c, 1));
        const double unit = static_cast<double>(c.vm.random()) / 4294967295.0;
        return result(Value::floating(static_cast<float>(lo + (hi - lo) * unit)));
    });
}

} // namespace skydot
