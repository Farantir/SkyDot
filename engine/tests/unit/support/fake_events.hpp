// SPDX-License-Identifier: GPL-3.0-or-later
//
// A WorldEvents that records what natives sent, for natives tests.
#pragma once

#include "vm/world_events.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace skydot::testing {

struct FakeEvents final : skydot::vm::WorldEvents {
    struct Message {
        std::string text;
        bool box = false;
        bool help = false;
    };
    struct Shader {
        std::uint32_t shader = 0;
        std::uint32_t target = 0;
        bool play = false;
    };
    struct Shot {
        std::uint32_t weapon = 0;
        std::uint32_t source = 0;
        std::uint32_t ammo = 0;
    };
    std::vector<Shot> shots;
    struct Played {
        std::uint32_t sound = 0;
        std::uint32_t source = 0;
        std::int64_t instance = 0;
    };
    std::vector<Played> played;
    std::vector<std::int64_t> stopped;
    std::vector<std::pair<std::int64_t, double>> volumes;
    std::vector<std::uint32_t> animation_senders;
    struct Moved {
        std::uint32_t ref = 0;
        vm::Xyz position{};
        vm::Xyz rotation{};
    };
    std::vector<Moved> moved;
    std::vector<std::pair<std::uint32_t, std::string>> animations;
    struct Impulse {
        std::uint32_t ref = 0;
        vm::Xyz direction{};
        double magnitude = 0.0;
    };
    std::vector<Impulse> impulses;
    struct Push {
        std::uint32_t source = 0;
        std::uint32_t target = 0;
        double force = 0.0;
    };
    std::vector<Push> pushes;
    struct Knock {
        std::uint32_t source = 0;
        double magnitude = 0.0;
        double radius = 0.0;
    };
    std::vector<Knock> knocks;
    std::vector<std::pair<std::uint32_t, std::int32_t>> motion_types;
    std::vector<std::pair<std::uint32_t, bool>> opened;
    struct Effect {
        std::uint32_t ref = 0;
        std::uint32_t art = 0;
        std::uint32_t target = 0;
        double duration = 0.0;
        bool play = false;
    };
    std::vector<Effect> effects;
    std::vector<std::pair<std::uint32_t, std::string>> sub_graphs;
    std::vector<std::string> traces;
    std::vector<Shader> shaders;
    std::vector<Message> messages;

    void trace(const std::string& text) override { traces.push_back(text); }
    void message(const std::string& text, bool box, bool help) override { messages.push_back({text, box, help}); }
    void effect_shader(std::uint32_t shader, std::uint32_t target, bool play) override {
        shaders.push_back({shader, target, play});
    }
    void fire_weapon(std::uint32_t weapon, std::uint32_t source, std::uint32_t ammo) override {
        shots.push_back({weapon, source, ammo});
    }
    void play_sound(std::uint32_t sound, std::uint32_t source, std::int64_t instance) override {
        played.push_back({sound, source, instance});
    }
    void stop_sound(std::int64_t instance) override { stopped.push_back(instance); }
    void sound_volume(std::int64_t instance, double volume) override { volumes.emplace_back(instance, volume); }
    void animation_events_wanted(std::uint32_t sender) override { animation_senders.push_back(sender); }
    void ref_moved(std::uint32_t ref, const vm::Xyz& position, const vm::Xyz& rotation) override {
        moved.push_back({ref, position, rotation});
    }
    void play_animation(std::uint32_t ref, const std::string& animation) override {
        animations.emplace_back(ref, animation);
    }
    void havok_impulse(std::uint32_t ref, const vm::Xyz& direction, double magnitude) override {
        impulses.push_back({ref, direction, magnitude});
    }
    void push_actor(std::uint32_t source, std::uint32_t target, double force) override {
        pushes.push_back({source, target, force});
    }
    void knock_area(std::uint32_t source, double magnitude, double radius) override {
        knocks.push_back({source, magnitude, radius});
    }
    void motion_type_changed(std::uint32_t ref, std::int32_t motion_type) override {
        motion_types.emplace_back(ref, motion_type);
    }
    void open_changed(std::uint32_t ref, bool open) override { opened.emplace_back(ref, open); }
    void visual_effect(std::uint32_t ref, std::uint32_t art, std::uint32_t target, double duration,
                       bool play) override {
        effects.push_back({ref, art, target, duration, play});
    }
    void sub_graph_animation(std::uint32_t ref, const std::string& animation) override {
        sub_graphs.emplace_back(ref, animation);
    }
};

} // namespace skydot::testing
