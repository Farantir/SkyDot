// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where the voice file of a dialogue response lives (ADR 0004, pack-format
// "Sound assets"): `sound/voice/<plugin>/<voice type>/<stem>.fuz`, all
// lowercase, the stem `<quest>_<topic>_<info id>_<response number>`. The
// quest and topic EDIDs are cut only when together they exceed 25 characters
// (an empty topic EDID counts as 0): then the quest is cut to 10 and the topic
// to what is left of 25. Read off the archive (tools/voice_census.gd).
// The plugin is the one that defines the INFO (not the last to override it);
// the id is the INFO's local FormID, 8 hex digits. Nothing here touches Godot.
#pragma once

#include "world_generated.h"

#include <cstdint>
#include <string>

namespace skydot::dialogue {

/// Lowercase file name stem of a response.
std::string voice_stem(const std::string& quest_edid, const std::string& topic_edid, std::uint32_t local_id,
                       unsigned number);

/// The plugin that defines `form` (its FormID prefix names it) and the form's
/// id inside it (24 bits, 12 for a light plugin). False if no plugin matches.
bool plugin_of_form(const bethconv::pack::wfb::World& world, std::uint32_t form, std::string& plugin_file,
                    std::uint32_t& local_id);

/// `sound/voice/<plugin>/<voice type>/<stem>.fuz`.
std::string voice_path(const std::string& plugin_file, const std::string& voice_type_edid, const std::string& stem);

} // namespace skydot::dialogue
