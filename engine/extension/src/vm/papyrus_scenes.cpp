// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's scene surface: thin forwarders to the scene module
// (vm/scene_runtime.hpp) that convert between its types and Godot's. The rules
// are scenes/scenes.hpp, the host and the Scene natives are SceneRuntime.
#include "vm/papyrus.hpp"

#include "vm/papyrus_common.hpp"

using godot::Array;
using godot::Dictionary;

namespace skydot {

Array SkydotPapyrus::scenes_update(double seconds) {
    Array out;
    for (auto& request : scene_.update(seconds)) {
        Dictionary d;
        d["scene"] = static_cast<std::int64_t>(request.scene);
        d["action"] = static_cast<std::int64_t>(request.action);
        d["speaker"] = static_cast<std::int64_t>(request.speaker);
        d["lines"] = lines_array(request.lines);
        out.push_back(d);
    }
    return out;
}

void SkydotPapyrus::scene_action_done(std::int64_t scene, std::int64_t action) {
    scene_.action_done(static_cast<std::uint32_t>(scene), static_cast<std::uint32_t>(action));
}

bool SkydotPapyrus::scene_start(std::int64_t scene, bool force) {
    return scene_.start(static_cast<std::uint32_t>(scene), force);
}

void SkydotPapyrus::scene_stop(std::int64_t scene) { scene_.stop(static_cast<std::uint32_t>(scene)); }

bool SkydotPapyrus::scene_is_playing(std::int64_t scene) const {
    return scene_.is_playing(static_cast<std::uint32_t>(scene));
}

} // namespace skydot
