// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's scene surface: the host the scene runner (scenes/scenes.hpp)
// asks about quests, actors, conditions, dialogue and fragments, the update
// that hands the game layer its dialogue actions, and the Scene natives.
// Scene fragment scripts (SF_*) attach to the scene's form, like TIF__ scripts
// to an INFO.
#include "vm/papyrus.hpp"

#include "build/refs.hpp"
#include "data/fb_search.hpp"
#include "vm/natives_common.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace skydot {

namespace wfb = bethconv::pack::wfb;

class SceneHostImpl final : public scenes::SceneHost {
public:
    explicit SceneHostImpl(SkydotPapyrus& owner) : owner_(owner) {}

    bool quest_running(std::uint32_t quest) const override {
        const auto* state = owner_.quests_.find(quest);
        return state != nullptr && state->running;
    }
    std::uint32_t alias_ref(std::uint32_t quest, std::int32_t alias) const override {
        return static_cast<std::uint32_t>(owner_.get_alias_ref(quest, alias));
    }
    bool actor_ready(std::uint32_t actor) const override {
        if (!owner_.dialogue_can_talk(actor) || owner_.disabled(actor)) {
            return false;
        }
        return !owner_.scene_needs_3d_ || owner_.is_3d_loaded(actor);
    }
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, std::uint32_t quest,
                         std::uint32_t subject) override {
        // The subject of scene and phase conditions is the player (a logged assumption).
        return conditions::conditions_pass(list, owner_.condition_world(),
                                           {.subject = subject != 0 ? subject : k_player_ref, .quest = quest});
    }
    std::vector<dialogue::Line> say(std::uint32_t actor, std::uint32_t topic) override {
        auto* logic = owner_.dialogue_logic();
        return logic != nullptr ? logic->say(actor, topic).lines : std::vector<dialogue::Line>{};
    }
    void run_fragment(std::uint32_t scene, const std::string& script, const std::string& function) override {
        auto& vm = *owner_.vm_;
        const auto* root = owner_.world_->data().root();
        const auto* s = lookup(root->scenes(), scene);
        if (s == nullptr) {
            return;
        }
        if (owner_.scene_scripts_.insert(scene).second) {
            owner_.attach_scripts(scene, script_list(s->scripts(), false), s->quest());
        }
        auto* instance = vm.instance(scene, script);
        if (instance == nullptr) {
            instance = vm.attach(scene, script);
        }
        if (instance == nullptr) {
            vm.error("scene: no fragment script " + script);
            return;
        }
        if (vm.call_method(instance, function, {}) == 0) {
            vm.error("scene: no fragment " + function + " in " + script);
        }
    }
    void push_package(std::uint32_t actor, std::uint32_t package) override {
        owner_.emit_signal("scene_package", static_cast<std::int64_t>(actor), static_cast<std::int64_t>(package), true);
    }
    void pop_package(std::uint32_t actor, std::uint32_t package) override {
        owner_.emit_signal("scene_package", static_cast<std::int64_t>(actor), static_cast<std::int64_t>(package), false);
    }
    void log(const std::string& text) override { godot::UtilityFunctions::print(natives::to_godot(text)); }

private:
    SkydotPapyrus& owner_;
};

scenes::Scenes* SkydotPapyrus::scene_logic() {
    if (scenes_ == nullptr) {
        const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
        if (root == nullptr) {
            return nullptr;
        }
        if (scene_host_ == nullptr) {
            scene_host_ = std::make_unique<SceneHostImpl>(*this);
        }
        scenes_ = std::make_unique<scenes::Scenes>(*root, *scene_host_);
    }
    return scenes_.get();
}

Array SkydotPapyrus::scenes_update(double seconds) {
    Array out;
    auto* logic = scene_logic();
    if (logic == nullptr) {
        return out;
    }
    for (auto& request : logic->update(seconds)) {
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
    if (auto* logic = scene_logic()) {
        logic->action_finished(static_cast<std::uint32_t>(scene), static_cast<std::uint32_t>(action));
    }
}

bool SkydotPapyrus::scene_start(std::int64_t scene, bool force) {
    auto* logic = scene_logic();
    return logic != nullptr && (force ? logic->force_start(static_cast<std::uint32_t>(scene))
                                      : logic->start(static_cast<std::uint32_t>(scene)));
}

void SkydotPapyrus::scene_stop(std::int64_t scene) {
    if (scenes_ != nullptr) {
        scenes_->stop(static_cast<std::uint32_t>(scene));
    }
}

bool SkydotPapyrus::scene_is_playing(std::int64_t scene) const {
    return scenes_ != nullptr && scenes_->is_playing(static_cast<std::uint32_t>(scene));
}

void SkydotPapyrus::bind_scene_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    auto& v = *vm_;
    v.bind("Scene", "Start", [this](NativeCall& c) {
        scene_start(c.self.form(), false);
        return NativeResult{};
    });
    v.bind("Scene", "ForceStart", [this](NativeCall& c) {
        scene_start(c.self.form(), true);
        return NativeResult{};
    });
    v.bind("Scene", "Stop", [this](NativeCall& c) {
        scene_stop(c.self.form());
        return NativeResult{};
    });
    v.bind("Scene", "IsPlaying", [this](NativeCall& c) {
        return result(Value::boolean(scene_is_playing(c.self.form())));
    });
    v.bind("Scene", "GetOwningQuest", [this](NativeCall& c) {
        const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
        const auto* s = root != nullptr ? lookup(root->scenes(), c.self.form()) : nullptr;
        return NativeResult{.value = s != nullptr ? c.vm.object(s->quest(), "Quest") : Value{}, .wait = -1.0};
    });
    v.bind("Scene", "IsActionComplete", [this](NativeCall& c) {
        return result(Value::boolean(scenes_ == nullptr ||
                                     scenes_->action_complete(c.self.form(), static_cast<std::uint32_t>(arg(c, 0).i()))));
    });
}

} // namespace skydot
