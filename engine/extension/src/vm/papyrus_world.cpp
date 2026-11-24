// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hosts the natives families ask the world and the game layer through.
#include "vm/papyrus.hpp"

namespace skydot {

struct SkydotPapyrus::SoundHost final : vm::SoundHost {
    explicit SoundHost(SkydotPapyrus& owner) : owner_(owner) {}
    std::int64_t new_sound_instance() override { return owner_.new_sound_instance(); }
    SkydotPapyrus& owner_;
};

std::unique_ptr<vm::SoundHost> SkydotPapyrus::new_sound_host() { return std::make_unique<SoundHost>(*this); }

struct SkydotPapyrus::ActorHost final : vm::ActorHost {
    explicit ActorHost(SkydotPapyrus& owner) : owner_(owner) {}
    vm::ActorStates& actor_states() override { return owner_.actors_; }
    conditions::ConditionWorld& conditions() override { return owner_.condition_world(); }
    bool location_loaded(std::uint32_t location) override { return owner_.location_loaded(location); }
    void evaluate_package(std::uint32_t ref) override { owner_.evaluate_package(ref); }
    void set_last_damager(std::uint32_t killer) override { owner_.combat_.set_last_damager(killer); }
    SkydotPapyrus& owner_;
};

std::unique_ptr<vm::ActorHost> SkydotPapyrus::new_actor_host() { return std::make_unique<ActorHost>(*this); }

} // namespace skydot
