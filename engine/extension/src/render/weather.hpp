// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotWeather`: an exterior's sky over time. Time of day runs at a time
// scale; the weather comes from the region the camera is in (REGN), else the
// worldspace's climate, and changes by fading from one to the next. It draws
// the sky gradient, sun or moon light, ambient and fog, the weather's cloud
// layers on meshes/sky/clouds.nif, stars, the sun and the moons, rain or snow
// around the camera, and lightning. See docs/weather.md.
#pragma once

#include "world/world.hpp"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/gpu_particles3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace bethconv::pack::wfb {
struct Weather;
struct Climate;
struct World;
} // namespace bethconv::pack::wfb

namespace skydot {

class SkydotWeather : public godot::Node3D {
    GDCLASS(SkydotWeather, godot::Node3D)

public:
    /// Follow `camera` through worldspace `world` of `pack_world`. Picks the
    /// first weather unless one is set before. Fails if the worldspace has
    /// no climate.
    godot::Error setup(const godot::Ref<SkydotWorld>& pack_world, std::int64_t world,
                       godot::Camera3D* camera);

    /// Hours, 0-24.
    void set_hour(double hour);
    double get_hour() const { return hour_; }
    /// Days since the start; the moons' phases follow it.
    void set_day(std::int64_t day) { day_ = day; }
    std::int64_t get_day() const { return day_; }
    /// Game seconds per real second (the game's default is 20; 0 stops time).
    void set_time_scale(double scale) { time_scale_ = scale; }
    double get_time_scale() const { return time_scale_; }

    /// Change to `weather`, fading over `seconds` (real time), or over the
    /// weather's own transition time if negative, or at once if 0.
    void set_weather(std::int64_t weather, double seconds = -1.0);
    std::int64_t get_weather() const { return to_; }
    std::int64_t get_previous_weather() const { return from_; }
    /// Choose weathers from the region or climate as time passes and the
    /// camera moves. On by default; set_weather does not turn it off.
    void set_auto_weather(bool enabled) { auto_weather_ = enabled; }
    bool get_auto_weather() const { return auto_weather_; }
    /// Pick a weather now from those the camera's region or the climate
    /// offer, and fade to it. Returns it, or 0.
    std::int64_t next_weather();

    void set_shadows(bool enabled);
    bool get_shadows() const { return shadows_; }

    /// The image space now ("hdr", "cinematic", "tint" as in
    /// SkydotWorld.get_image_space), blended between the times of day and
    /// weathers; empty if the weathers have none.
    godot::Dictionary get_image_space() const;

    /// weather, previous, transition (0-1), hour, day, daylight, region (the
    /// REGN deciding, or 0), precipitation (0-1, how much is falling),
    /// lightning (seconds since the last flash, or -1), clouds (visible
    /// layers).
    godot::Dictionary get_state() const;

    void _process(double delta) override;

    /// The code of every weather shader, by name.
    static godot::Dictionary shader_codes();

protected:
    static void _bind_methods();

private:
    using Weather = bethconv::pack::wfb::Weather;

    /// One weather's sky at the current time.
    struct Sky {
        godot::Color upper, lower, horizon, ambient, sunlight, sun, stars, fog_near_color,
            fog_far_color;
        float fog_near{}, fog_far{}, fog_power{1}, fog_max{1};
        /// DALC: x+, x-, y+, y-, z+, z- (the game's axes).
        std::array<godot::Color, 6> directional_ambient{};
        bool has_directional_ambient = false;
        /// The image space: HNAM (9), CNAM (3), TNAM (4), in that order.
        std::array<float, 16> image_space{};
        bool has_image_space = false;
    };
    Sky sky_of(const Weather* weather) const;
    const Weather* weather_ptr(std::int64_t id) const;
    /// Weathers offered where the camera is, with their chances, and the
    /// region offering them (0: the climate).
    std::vector<std::pair<std::uint32_t, std::int32_t>> offered(std::uint32_t& region) const;
    std::uint32_t pick(const std::vector<std::pair<std::uint32_t, std::int32_t>>& list);

    void build();
    void build_clouds();
    void build_sky_objects();
    void update_sky(double delta);
    void update_clouds(double delta);
    void update_sky_objects();
    void update_precipitation();
    void configure_precipitation(const Weather* weather);
    /// Fraction of time-of-day key `time` (0 sunrise ... 3 night) now, and
    /// the two keys around it.
    void time_keys(int& from, int& to, float& t) const;
    godot::Ref<godot::Texture2D> texture(const godot::String& vpath) const;
    godot::Vector3 sun_direction(bool& day) const;

    godot::Ref<SkydotWorld> world_;
    /// The world's root; null if there is no world or it is closed.
    const bethconv::pack::wfb::World* world_fb() const;
    std::uint32_t world_id_{};
    const bethconv::pack::wfb::Climate* climate_{};
    godot::ObjectID camera_;
    godot::Camera3D* camera() const;

    double hour_{12.0};
    std::int64_t day_{0};
    double time_scale_{20.0};
    std::uint32_t from_{};
    std::uint32_t to_{};
    double transition_{1.0};      ///< 0 at the start of a fade, 1 when done.
    double transition_speed_{1.0}; ///< Per real second.
    bool auto_weather_{true};
    double hold_hours_{0.0};       ///< Game hours until the next change.
    double region_check_{0.0};
    std::uint32_t region_{};
    bool shadows_{true};
    double lightning_timer_{0.0};
    double since_flash_{-1.0};
    double precipitation_{0.0};
    std::mt19937 random_{0x5D07u};

    godot::Ref<godot::Environment> environment_;
    godot::Ref<godot::ShaderMaterial> sky_material_;
    godot::DirectionalLight3D* light_{};
    godot::Vector3 light_towards_{}; // where the sun light points from, in steps (see update_sky)
    godot::Node3D* dome_{};          ///< Follows the camera.
    struct Layer {
        godot::Ref<godot::ShaderMaterial> material;
        std::array<godot::Vector2, 2> offset; ///< Scroll of the old and new weather.
        std::array<godot::String, 2> texture;
        bool visible{};
    };
    std::vector<Layer> layers_;
    std::vector<godot::Ref<godot::ShaderMaterial>> stars_;
    godot::Ref<godot::ShaderMaterial> sun_material_;
    godot::Ref<godot::ShaderMaterial> glare_material_;
    std::array<godot::Ref<godot::ShaderMaterial>, 2> moon_materials_;
    std::array<godot::MeshInstance3D*, 2> moons_{};
    godot::MeshInstance3D* sun_{};
    godot::MeshInstance3D* glare_{};
    godot::GPUParticles3D* particles_{};
    std::uint32_t particles_for_{}; ///< The precipitation the particles show.
};

} // namespace skydot
