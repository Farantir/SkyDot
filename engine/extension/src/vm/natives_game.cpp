// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game natives: GetPlayer, GetForm.
#include "vm/activation.hpp"
#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

namespace skydot::vm {

void bind_game_natives(Vm& v, QuestHost& quests) {
    using namespace natives;
    v.bind("Game", "GetPlayer", [](NativeCall& c) { return result(c.vm.object(k_player_ref, "Actor")); });
    // ---- Game.GetForm: any form by id, seen as Form.
    v.bind("Game", "GetForm", [&quests](NativeCall& c) {
        const auto form = static_cast<std::uint32_t>(arg(c, 0).i());
        if (form == 0) {
            return NativeResult{};
        }
        quests.attach_quest_scripts(form, true);
        return result(c.vm.object(form, "Form"));
    });
}

} // namespace skydot::vm
