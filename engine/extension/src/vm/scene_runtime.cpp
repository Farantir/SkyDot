// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scene domain of the Papyrus runtime (vm/scene_runtime.hpp): the host the
// scene runner asks about quests, actors, conditions, dialogue and fragments,
// and the Scene natives. Scene fragment scripts (SF_*) attach to the scene's
// form, like TIF__ scripts to an INFO.
#include "vm/scene_runtime.hpp"

#include "data/fb_search.hpp"
#include "vm/activation.hpp"
#include "vm/natives_args.hpp"

namespace skydot::vm {

namespace wfb = bethconv::pack::wfb;

// ---- the domain ------------------------------------------------------------

void SceneRuntime::reset() {
    scenes_.reset();
    scripts_attached_.clear();
}

scenes::Scenes* SceneRuntime::logic() {
    if (scenes_ == nullptr && host_ != nullptr) {
        const auto* root = host_->world_root();
        if (root == nullptr) {
            return nullptr;
        }
        scenes_ = std::make_unique<scenes::Scenes>(*root, *this);
    }
    return scenes_.get();
}

std::vector<scenes::DialogueRequest> SceneRuntime::update(double seconds) {
    auto* runner = logic();
    return runner != nullptr ? runner->update(seconds) : std::vector<scenes::DialogueRequest>{};
}

void SceneRuntime::action_done(std::uint32_t scene, std::uint32_t action) {
    if (auto* runner = logic()) {
        runner->action_finished(scene, action);
    }
}

bool SceneRuntime::start(std::uint32_t scene, bool force) {
    auto* runner = logic();
    return runner != nullptr && (force ? runner->force_start(scene) : runner->start(scene));
}

void SceneRuntime::stop(std::uint32_t scene) {
    if (scenes_ != nullptr) {
        scenes_->stop(scene);
    }
}

bool SceneRuntime::is_playing(std::uint32_t scene) const {
    return scenes_ != nullptr && scenes_->is_playing(scene);
}

bool SceneRuntime::action_complete(std::uint32_t scene, std::uint32_t action) const {
    return scenes_ == nullptr || scenes_->action_complete(scene, action);
}

std::unique_ptr<scenes::Scenes> SceneRuntime::load(save::ByteReader r) {
    if (logic() == nullptr) {
        return nullptr;
    }
    auto loaded = std::make_unique<scenes::Scenes>(*host_->world_root(), *this);
    return scenes::Scenes::load(r, *loaded) ? std::move(loaded) : nullptr;
}

void SceneRuntime::adopt(std::unique_ptr<scenes::Scenes> loaded) {
    auto* runner = logic();
    if (runner == nullptr) {
        return;
    }
    if (loaded != nullptr) {
        runner->adopt(std::move(*loaded));
        // The AI forgot the pushed packages with the rest of its state.
        for (const auto& [actor, package] : runner->pushed_packages()) {
            host_->package_signal(actor, package, true);
        }
    } else {
        runner->clear();
    }
}

// ---- scenes::SceneHost -----------------------------------------------------------

bool SceneRuntime::quest_running(std::uint32_t quest) const {
    const auto* state = host_->quest_host().quests().find(quest);
    return state != nullptr && state->running;
}

std::uint32_t SceneRuntime::alias_ref(std::uint32_t quest, std::int32_t alias) const {
    const auto& quests = host_->quest_host().quests();
    return quests.fill(quests.find_handle(quest, static_cast<std::uint32_t>(alias)));
}

bool SceneRuntime::actor_ready(std::uint32_t actor) const {
    if (!host_->can_talk(actor) || host_->world_access().ref_disabled(actor)) {
        return false;
    }
    return !needs_3d_ || host_->world_access().ref_3d_loaded(actor);
}

bool SceneRuntime::conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list,
                                   std::uint32_t quest, std::uint32_t subject) {
    // The subject of scene and phase conditions is the player (a logged assumption).
    return conditions::conditions_pass(list, host_->condition_world(),
                                       {.subject = subject != 0 ? subject : k_player_ref, .quest = quest});
}

std::vector<dialogue::Line> SceneRuntime::say(std::uint32_t actor, std::uint32_t topic) {
    return host_->say(actor, topic);
}

void SceneRuntime::run_fragment(std::uint32_t scene, const std::string& script, const std::string& function) {
    const auto* root = host_->world_root();
    if (root == nullptr || lookup(root->scenes(), scene) == nullptr) {
        return;
    }
    if (scripts_attached_.insert(scene).second) {
        host_->attach_scene_scripts(scene);
    }
    auto* instance = vm_->instance(scene, script);
    if (instance == nullptr) {
        instance = vm_->attach(scene, script);
    }
    if (instance == nullptr) {
        vm_->error("scene: no fragment script " + script);
        return;
    }
    if (vm_->call_method(instance, function, {}) == 0) {
        vm_->error("scene: no fragment " + function + " in " + script);
    }
}

void SceneRuntime::push_package(std::uint32_t actor, std::uint32_t package) {
    host_->package_signal(actor, package, true);
}

void SceneRuntime::pop_package(std::uint32_t actor, std::uint32_t package) {
    host_->package_signal(actor, package, false);
}

void SceneRuntime::log(const std::string& text) { host_->log(text); }

// ---- natives ---------------------------------------------------------------

void SceneRuntime::bind_natives(Vm& vm, Host& host) {
    using namespace natives;
    vm_ = &vm;
    host_ = &host;
    vm.bind("Scene", "Start", [this](NativeCall& c) {
        start(c.self.form(), false);
        return NativeResult{};
    });
    vm.bind("Scene", "ForceStart", [this](NativeCall& c) {
        start(c.self.form(), true);
        return NativeResult{};
    });
    vm.bind("Scene", "Stop", [this](NativeCall& c) {
        stop(c.self.form());
        return NativeResult{};
    });
    vm.bind("Scene", "IsPlaying", [this](NativeCall& c) {
        return result(Value::boolean(is_playing(c.self.form())));
    });
    vm.bind("Scene", "GetOwningQuest", [this](NativeCall& c) {
        const auto* root = host_->world_root();
        const auto* s = root != nullptr ? lookup(root->scenes(), c.self.form()) : nullptr;
        return NativeResult{.value = s != nullptr ? c.vm.object(s->quest(), "Quest") : Value{}, .wait = -1.0};
    });
    vm.bind("Scene", "IsActionComplete", [this](NativeCall& c) {
        return result(Value::boolean(action_complete(c.self.form(), static_cast<std::uint32_t>(arg(c, 0).i()))));
    });
}

} // namespace skydot::vm
