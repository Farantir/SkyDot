// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game natives: GetPlayer, GetForm.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"

namespace skydot {

void SkydotPapyrus::bind_game_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    [[maybe_unused]] auto& v = *vm_;
    [[maybe_unused]] const auto arg = natives::arg;
    [[maybe_unused]] const auto result = natives::result;
    [[maybe_unused]] const auto number = natives::number;
    v.bind("Game", "GetPlayer", [result](NativeCall& c) {
        return result(c.vm.object(static_cast<std::uint32_t>(PLAYER_REF), "Actor"));
    });
    // ---- Game.GetForm: any form by id, seen as Form.
    v.bind("Game", "GetForm", [this, arg, result](NativeCall& c) {
        const auto form = static_cast<std::uint32_t>(arg(c, 0).i());
        if (form == 0) {
            return NativeResult{};
        }
        attach_quest_scripts(form, true);
        return result(c.vm.object(form, "Form"));
    });
}

} // namespace skydot
