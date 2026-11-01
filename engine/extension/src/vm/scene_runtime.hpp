// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scene domain of the Papyrus runtime: owns the scene runner
// (scenes/scenes.hpp) and the fragment bookkeeping, hosts it (scenes::SceneHost)
// and binds the Scene natives. No Godot: it reaches the engine through the
// narrow SceneRuntime::Host, which SkydotPapyrus::Access implements.
#pragma once

#include "conditions/conditions.hpp"
#include "dialogue/dialogue.hpp"
#include "save/bytes.hpp"
#include "scenes/scenes.hpp"
#include "vm/quest_host.hpp"
#include "vm/vm.hpp"
#include "vm/world_access.hpp"
#include "world_generated.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace skydot::vm {

class SceneRuntime final : public scenes::SceneHost {
public:
    /// What the scene domain needs of the rest of the engine, beyond the shared interfaces.
    class Host {
    public:
        virtual ~Host() = default;
        /// The pack's world records; null without a world.
        [[nodiscard]] virtual const bethconv::pack::wfb::World* world_root() const = 0;
        [[nodiscard]] virtual WorldAccess& world_access() = 0;
        [[nodiscard]] virtual QuestHost& quest_host() = 0;
        [[nodiscard]] virtual conditions::ConditionWorld& condition_world() = 0;
        /// Whether `ref` is a living actor that dialogue can address.
        [[nodiscard]] virtual bool can_talk(std::uint32_t ref) const = 0;
        /// The lines `actor` says for `topic` (dialogue's selection); empty without dialogue.
        [[nodiscard]] virtual std::vector<dialogue::Line> say(std::uint32_t actor, std::uint32_t topic) = 0;
        /// Attach the SF_ scripts of `scene` to its form.
        virtual void attach_scene_scripts(std::uint32_t scene) = 0;
        /// An AI package was pushed (`push`) on or popped from an actor by a scene.
        virtual void package_signal(std::uint32_t actor, std::uint32_t package, bool push) = 0;
        virtual void log(const std::string& text) = 0;
    };

    /// Bind the Scene natives to `vm` and remember the host (SkydotPapyrus::bind_natives).
    void bind_natives(Vm& vm, Host& host);
    /// Forget the scenes and fragment bookkeeping (a new world).
    void reset();

    /// Whether a scene's actors must have their 3D loaded (default), else a loaded, living actor will do.
    void set_actors_need_3d(bool need) { needs_3d_ = need; }

    /// The scene runner, made on first use; null without a world or before the natives are bound.
    scenes::Scenes* logic();
    [[nodiscard]] bool started() const { return scenes_ != nullptr; }

    std::vector<scenes::DialogueRequest> update(double seconds);
    void action_done(std::uint32_t scene, std::uint32_t action);
    bool start(std::uint32_t scene, bool force);
    void stop(std::uint32_t scene);
    /// A quest started (scenes that begin with it wait to start) or stopped (those that end with it stop).
    void quest_started(std::uint32_t quest) {
        if (auto* runner = logic()) {
            runner->quest_started(quest);
        }
    }
    void quest_stopped(std::uint32_t quest) {
        if (scenes_ != nullptr) {
            scenes_->quest_stopped(quest);
        }
    }
    [[nodiscard]] bool is_playing(std::uint32_t scene) const;
    /// Also true when no scene ever ran (conditions ask this).
    [[nodiscard]] bool action_complete(std::uint32_t scene, std::uint32_t action) const;

    /// The SCEN section's bytes, empty if no scene runner exists.
    [[nodiscard]] std::vector<std::uint8_t> save() const { return scenes_ != nullptr ? scenes_->save() : std::vector<std::uint8_t>{}; }
    /// Read a SCEN section into a runner not yet adopted; null if it is damaged or there is no world.
    [[nodiscard]] std::unique_ptr<scenes::Scenes> load(save::ByteReader r);
    /// Adopt a loaded section (null: no scene was running) and re-push its packages through the host.
    void adopt(std::unique_ptr<scenes::Scenes> loaded);

    /// `ref` is gone: drop the packages scenes pushed on it.
    void forget(std::uint32_t ref) {
        if (scenes_ != nullptr) {
            scenes_->forget(ref);
        }
    }
    [[nodiscard]] std::size_t pushed_count() const { return scenes_ != nullptr ? scenes_->pushed_count() : 0; }

    // scenes::SceneHost
    bool quest_running(std::uint32_t quest) const override;
    std::uint32_t alias_ref(std::uint32_t quest, std::int32_t alias) const override;
    bool actor_ready(std::uint32_t actor) const override;
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<bethconv::pack::wfb::Condition>>* list,
                         std::uint32_t quest, std::uint32_t subject) override;
    std::vector<dialogue::Line> say(std::uint32_t actor, std::uint32_t topic) override;
    void run_fragment(std::uint32_t scene, const std::string& script, const std::string& function) override;
    void push_package(std::uint32_t actor, std::uint32_t package) override;
    void pop_package(std::uint32_t actor, std::uint32_t package) override;
    void log(const std::string& text) override;

private:
    Vm* vm_ = nullptr;
    Host* host_ = nullptr;
    std::unique_ptr<scenes::Scenes> scenes_;
    std::set<std::uint32_t> scripts_attached_;
    bool needs_3d_ = true;
};

} // namespace skydot::vm
