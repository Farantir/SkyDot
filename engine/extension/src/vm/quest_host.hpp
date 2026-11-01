// SPDX-License-Identifier: GPL-3.0-or-later
//
// `QuestHost`: what the quest-related natives change or ask of the quest
// state beyond records. The natives that start, stop and stage quests extend
// it (vm/natives_quest.cpp).
#pragma once

#include <cstdint>

namespace skydot::vm {

class QuestHost {
public:
    virtual ~QuestHost() = default;

    /// Attach the scripts of quest `quest` if it is one and has none yet;
    /// `on_init` also sends their OnInit. Anything else is left alone.
    virtual void attach_quest_scripts(std::uint32_t quest, bool on_init) = 0;
};

} // namespace skydot::vm
