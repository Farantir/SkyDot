// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's scene surface: thin forwarders to the scene module
// (vm/scene_runtime.hpp) that convert between its types and Godot's. The rules
// are scenes/scenes.hpp, the host and the Scene natives are SceneRuntime.
#include "vm/papyrus.hpp"

#include "data/fb_search.hpp"
#include "data/scripts.hpp"
#include "vm/papyrus_common.hpp"
#include "world_generated.h"

using godot::Array;
using godot::Dictionary;

namespace skydot {

struct SkydotPapyrus::SceneHost final : vm::SceneRuntime::Host {
    explicit SceneHost(SkydotPapyrus& owner) : owner_(owner) {}
    const bethconv::pack::wfb::World* world_root() const override { return owner_.world_root(); }
    vm::WorldAccess& world_access() override { return *owner_.world_host_; }
    vm::QuestHost& quest_host() override { return *owner_.quest_host_; }
    conditions::ConditionWorld& condition_world() override { return owner_.condition_world(); }
    void attach_scene_scripts(std::uint32_t scene) override {
        const auto* root = owner_.world_root();
        if (const auto* s = root != nullptr ? lookup(root->scenes(), scene) : nullptr) {
            owner_.attach_scripts(scene, script_list(s->scripts(), false), s->quest());
        }
    }
    void package_signal(std::uint32_t actor, std::uint32_t package, bool push) override {
        owner_.emit_signal("scene_package", static_cast<std::int64_t>(actor), static_cast<std::int64_t>(package), push);
    }
    void log(const std::string& text) override { owner_.print_line(text); }
    SkydotPapyrus& owner_;
};

std::unique_ptr<vm::SceneRuntime::Host> SkydotPapyrus::new_scene_host() { return std::make_unique<SceneHost>(*this); }

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
