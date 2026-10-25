// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors/actor.hpp"

#include "render/animator.hpp"
#include "data/coordinates.hpp"
#include "physics/collision.hpp"

#include <godot_cpp/classes/animation_library.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/navigation_server3d.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using godot::Vector3;
using godot::real_t;

namespace skydot {
namespace {

real_t r(double v) { return static_cast<real_t>(v); }

constexpr double k_tau = 2.0 * std::numbers::pi;
/// Radians per second.
constexpr double k_turn_rate = 5.0;
/// Radians a step turns at least for the actor to count as turning (animate).
constexpr double k_turn_visible = 0.002;
/// A slow turner (the dragon) walks on once its heading is within this cosine (45 degrees) of the corner.
constexpr double k_slow_walk_cos = 0.7;
/// A corner counts as reached this close (metres, horizontally).
constexpr double k_corner_reach = 0.35;
/// Farther than this off the navmesh, a walk does not start.
constexpr double k_off_mesh = 1.5;
/// Less than this progress in `k_stuck_window` seconds gives up a walk.
constexpr double k_stuck_distance = 0.2;
constexpr double k_stuck_window = 1.5;
/// Below this horizontal speed the actor idles.
constexpr double k_idle_speed = 0.15;
/// Seconds between looks for ground while held.
constexpr double k_ground_poll = 0.25;
/// Falling this far below home puts it back there.
constexpr double k_lost_depth = 30.0;
/// A held actor with no ground within 4 m looks this far down (metres) before it waits on.
constexpr double k_drop_search = 40.0;
constexpr double k_blend = 0.25;
/// Doors: how often to look for one ahead (seconds), how far past the body
/// (metres), how long to stand while it swings, and when to close it behind
/// (seconds after opening, metres from its hinge).
constexpr double k_door_probe = 0.2;
constexpr double k_door_reach = 0.8;
constexpr double k_door_wait = 0.8;
constexpr double k_door_close_after = 2.0;
constexpr double k_door_close_distance = 2.0;
/// GetOpenState's values (SkydotPapyrus).
constexpr std::int64_t k_door_open = 1;
constexpr std::int64_t k_door_closed = 3;

/// Level of detail (see actor.hpp). Distances to the camera in metres: a cell
/// is 57.6 m wide, so near is a cell and a bit (what a radius of 1 shows), and
/// the level is left only 10% past the border so that an actor walking along
/// it does not flip back and forth. Seconds between steps of the physics, and
/// between advances of the animation (the clips are sampled at 30 fps).
constexpr double k_lod_near = 80.0;
constexpr double k_lod_far = 160.0;
constexpr double k_lod_hysteresis = 0.1;
constexpr double k_lod_interval = 0.1;
constexpr double k_mid_step = 1.0 / 20.0;
constexpr double k_far_step = 1.0 / 10.0;
/// A standing actor only checks that the ground is still there.
constexpr double k_stand_step = 1.0;
constexpr double k_mid_anim = 1.0 / 15.0;
constexpr double k_far_anim = 1.0 / 7.5;
constexpr double k_mid_ai = 0.2;
constexpr double k_far_ai = 0.5;

/// The placed model a collision object belongs to: its nearest ancestor
/// with a reference.
godot::Node3D* model_of(godot::Object* collider) {
    auto* node = godot::Object::cast_to<godot::Node>(collider);
    for (int depth = 0; node != nullptr && depth < 16; ++depth, node = node->get_parent()) {
        if (node->has_meta("skydot_ref")) {
            return godot::Object::cast_to<godot::Node3D>(node);
        }
    }
    return nullptr;
}

double wrap(double a) {
    a = std::fmod(a + std::numbers::pi, k_tau);
    if (a < 0) {
        a += k_tau;
    }
    return a - std::numbers::pi;
}

} // namespace

void SkydotActor::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::PropertyInfo;
    using godot::Variant;
    ClassDB::bind_method(D_METHOD("walk_to", "target", "run"), &SkydotActor::walk_to);
    ClassDB::bind_method(D_METHOD("stop"), &SkydotActor::stop);
    ClassDB::bind_method(D_METHOD("walk_straight", "target", "run"), &SkydotActor::walk_straight);
    ClassDB::bind_method(D_METHOD("is_walking"), &SkydotActor::is_walking);
    ClassDB::bind_method(D_METHOD("get_path"), &SkydotActor::get_path);
    ClassDB::bind_method(D_METHOD("get_state"), &SkydotActor::get_state);
    ClassDB::bind_method(D_METHOD("set_clip_speeds", "walk", "run"), &SkydotActor::set_clip_speeds);
    ClassDB::bind_method(D_METHOD("set_seed", "seed"), &SkydotActor::set_seed);
    ClassDB::bind_method(D_METHOD("face", "yaw"), &SkydotActor::face);
    ClassDB::bind_method(D_METHOD("set_home", "value"), &SkydotActor::set_home);
    ClassDB::bind_method(D_METHOD("get_home"), &SkydotActor::get_home);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "home"), "set_home", "get_home");
    ClassDB::bind_method(D_METHOD("set_wander", "value"), &SkydotActor::set_wander);
    ClassDB::bind_method(D_METHOD("get_wander"), &SkydotActor::get_wander);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wander"), "set_wander", "get_wander");
    ClassDB::bind_method(D_METHOD("set_wander_radius", "value"), &SkydotActor::set_wander_radius);
    ClassDB::bind_method(D_METHOD("get_wander_radius"), &SkydotActor::get_wander_radius);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wander_radius"), "set_wander_radius", "get_wander_radius");
    ClassDB::bind_method(D_METHOD("set_dead", "dead"), &SkydotActor::set_dead);
    ClassDB::bind_method(D_METHOD("is_dead"), &SkydotActor::is_dead);
    ClassDB::bind_method(D_METHOD("set_frozen", "frozen"), &SkydotActor::set_frozen);
    ClassDB::bind_method(D_METHOD("is_frozen"), &SkydotActor::is_frozen);
    ClassDB::bind_method(D_METHOD("get_lod"), &SkydotActor::get_lod);
    ClassDB::bind_method(D_METHOD("set_stance", "prefix"), &SkydotActor::set_stance);
    ClassDB::bind_method(D_METHOD("get_stance"), &SkydotActor::get_stance);
    ClassDB::bind_method(D_METHOD("play_action", "clip", "speed"), &SkydotActor::play_action);
    ClassDB::bind_method(D_METHOD("is_acting"), &SkydotActor::is_acting);
    ClassDB::bind_method(D_METHOD("set_look_target", "point"), &SkydotActor::set_look_target);
    ClassDB::bind_method(D_METHOD("clear_look_target"), &SkydotActor::clear_look_target);
    ClassDB::bind_method(D_METHOD("has_look_target"), &SkydotActor::has_look_target);
    ClassDB::bind_method(D_METHOD("set_turn_rate", "rate"), &SkydotActor::set_turn_rate);
    ClassDB::bind_method(D_METHOD("get_turn_rate"), &SkydotActor::get_turn_rate);
    ClassDB::bind_method(D_METHOD("get_playing"), &SkydotActor::get_playing);
    ClassDB::bind_method(D_METHOD("fly_command", "kind", "point_skyrim", "radius", "height", "dive"),
                         &SkydotActor::fly_command, DEFVAL(0.0), DEFVAL(0.0), DEFVAL(false));
    ClassDB::bind_method(D_METHOD("is_flying"), &SkydotActor::is_flying);
    ClassDB::bind_method(D_METHOD("can_fly"), &SkydotActor::can_fly);
    ClassDB::bind_method(D_METHOD("get_flying_state"), &SkydotActor::get_flying_state);
    ClassDB::bind_method(D_METHOD("get_flight_mode"), &SkydotActor::get_flight_mode);
    ClassDB::bind_method(D_METHOD("get_flight_bank"), &SkydotActor::get_flight_bank);
    ClassDB::bind_method(D_METHOD("get_flight_speed"), &SkydotActor::get_flight_speed);
    ClassDB::bind_method(D_METHOD("get_flight_pitch"), &SkydotActor::get_flight_pitch);
    ClassDB::bind_method(D_METHOD("set_flight_hurt", "hurt"), &SkydotActor::set_flight_hurt);
    ClassDB::bind_method(D_METHOD("get_flight_hurt"), &SkydotActor::get_flight_hurt);
    ADD_SIGNAL(godot::MethodInfo("flight_took_off"));
    ADD_SIGNAL(godot::MethodInfo("flight_landed"));
    ADD_SIGNAL(godot::MethodInfo("flight_crashed"));
    ADD_SIGNAL(godot::MethodInfo("animation_event", PropertyInfo(Variant::STRING, "event")));
    ADD_SIGNAL(godot::MethodInfo("door_toggled", PropertyInfo(Variant::INT, "ref"),
                                 PropertyInfo(Variant::INT, "open_state")));
}

bool SkydotActor::set_stance(const godot::String& prefix) {
    godot::AnimationPlayer* anim = player();
    stance_idle_ = stance_walk_ = stance_run_ = false;
    stance_walk_speed_ = stance_run_speed_ = 0.0;
    stance_ = "";
    playing_ = "";
    if (prefix.is_empty()) {
        return true;
    }
    if (anim == nullptr || !stance_source_) {
        return false;
    }
    const StanceClips clips = stance_source_(prefix.to_lower().utf8().get_data());
    godot::Ref<godot::AnimationLibrary> library = anim->get_animation_library("");
    if (library.is_null()) {
        return false;
    }
    const auto put = [&](const char* name, const godot::Ref<godot::Animation>& clip) {
        if (clip.is_null()) {
            return false;
        }
        if (library->has_animation(name)) {
            library->remove_animation(name);
        }
        library->add_animation(name, clip);
        return true;
    };
    stance_idle_ = put("stance_idle", clips.idle);
    stance_walk_ = put("stance_walk", clips.walk);
    stance_run_ = put("stance_run", clips.run);
    stance_walk_speed_ = clips.walk_speed;
    stance_run_speed_ = clips.run_speed;
    if (!stance_idle_ && !stance_walk_ && !stance_run_) {
        return false;
    }
    stance_ = prefix.to_lower();
    return true;
}

godot::String SkydotActor::gait_clip(const godot::String& base) const {
    if (base == godot::String("idle") && stance_idle_) {
        return "stance_idle";
    }
    if (base == godot::String("walk") && stance_walk_) {
        return "stance_walk";
    }
    if (base == godot::String("run") && stance_run_) {
        return "stance_run";
    }
    return base;
}

double SkydotActor::play_action(const godot::String& clip, double speed) {
    flight_own_action_ = false;
    return play_clip(clip, speed, false);
}

double SkydotActor::play_clip(const godot::String& clip, double speed, bool loop) {
    godot::AnimationPlayer* anim = player();
    if (anim == nullptr || !clip_source_ || dead_ || clip.is_empty()) {
        return 0.0;
    }
    godot::Ref<godot::AnimationLibrary> library = anim->get_animation_library("");
    if (library.is_null()) {
        return 0.0;
    }
    const std::string key = clip.utf8().get_data();
    auto it = action_clips_.find(key);
    if (it == action_clips_.end()) {
        const godot::Ref<godot::Animation> animation = clip_source_(key);
        if (animation.is_null()) {
            return 0.0;
        }
        const godot::String name = godot::String("action_") + godot::String::num_int64(
            static_cast<std::int64_t>(action_clips_.size()));
        library->add_animation(name, animation);
        it = action_clips_.emplace(key, name).first;
    }
    const double rate = std::max(speed, 0.1);
    const godot::Ref<godot::Animation> animation = library->get_animation(it->second);
    const double length = static_cast<double>(animation->get_length()) / rate;
    animation->set_loop_mode(loop ? godot::Animation::LOOP_LINEAR : godot::Animation::LOOP_NONE);
    anim->set_speed_scale(r(rate));
    anim->play(it->second, k_blend);
    playing_ = it->second;
    action_left_ = loop ? 0.0 : length;
    wake_ = true;
    return length;
}

void SkydotActor::set_look_target(const Vector3& point) {
    look_target_ = point;
    look_active_ = true;
    wake_ = true;
}

void SkydotActor::turn_to_look(double delta) {
    if (!look_active_) {
        return;
    }
    Vector3 to = look_target_ - get_global_position();
    to.y = 0;
    if (static_cast<double>(to.length()) < 0.05) {
        return;
    }
    const double want = std::atan2(-static_cast<double>(to.x), -static_cast<double>(to.z));
    const double step = turn_rate_ * delta;
    face(facing_ + std::clamp(wrap(want - facing_), -step, step));
}

void SkydotActor::set_dead(bool dead) {
    const bool was = dead_;
    dead_ = dead;
    if (!dead) {
        if (was) {
            // Alive again (reanimated): the body blocks, moves and animates as before.
            frozen_ = false;
            action_left_ = 0.0;
            playing_ = "";
            wake_ = true;
            set_collision_layer(body_layer());
            set_collision_mask(body_mask());
            set_process(true);
            set_physics_process(true);
            if (godot::AnimationPlayer* anim = player()) {
                anim->set_active(true);
            }
        }
        return;
    }
    path_.clear();
    corner_ = 0;
    set_input(godot::Vector2(), 0.0, WALK);
    set_velocity(Vector3());
    set_collision_layer(0);
    set_collision_mask(0);
    set_process(false);
    set_physics_process(false);
    if (godot::AnimationPlayer* anim = player()) {
        anim->stop();
        anim->set_active(false);
    }
}

void SkydotActor::set_frozen(bool frozen) {
    if (frozen_ == frozen) {
        return;
    }
    frozen_ = frozen;
    wake_ = true;
    if (frozen) {
        stop();
        set_velocity(Vector3(0, get_velocity().y, 0));
    }
    if (godot::AnimationPlayer* anim = player(); anim != nullptr && !dead_) {
        anim->set_active(!frozen);
    }
}

std::uint32_t SkydotActor::body_layer() const {
    return physics_layer::npc;
}

std::uint32_t SkydotActor::body_mask() const {
    return physics_layer::solid | physics_layer::actor | physics_layer::npc;
}

void SkydotActor::set_seed(std::int64_t seed) {
    if (rng_.is_null()) {
        rng_.instantiate();
    }
    rng_->set_seed(static_cast<std::uint64_t>(seed));
}

void SkydotActor::set_clip_speeds(double walk, double run) {
    clip_walk_ = walk;
    clip_run_ = run;
    if (walk > 0) {
        set_walk_speed(walk);
    }
    // Without a run clip it walks wherever it would run.
    set_run_speed(run > 0 ? run : walk);
}

void SkydotActor::face(double yaw) {
    facing_ = wrap(yaw);
    set_look(facing_, 0.0);
    set_rotation(Vector3(0, r(facing_), 0));
}

void SkydotActor::_ready() {
    if (rng_.is_null()) {
        set_seed(static_cast<std::int64_t>(get_instance_id()));
    }
    // The body takes the node's facing; the cylinder does not care.
    const Vector3 forward = get_global_transform().basis.xform(Vector3(0, 0, -1));
    SkydotPlayer::_ready();
    face(std::atan2(-static_cast<double>(forward.x), -static_cast<double>(forward.z)));
    if (!home_set_) {
        set_home(get_global_position());
    }
    set_hold(true);
    // Spread the first walks out.
    wait_ = static_cast<double>(rng_->randf_range(2.0F, 15.0F));
    // And the work of the levels of detail, so that actors do not all step,
    // animate and look at the camera in the same frame.
    ground_wait_ = static_cast<double>(rng_->randf_range(0.0F, static_cast<float>(k_ground_poll)));
    lod_wait_ = static_cast<double>(rng_->randf_range(0.0F, static_cast<float>(k_lod_interval)));
    step_wait_ = static_cast<double>(rng_->randf_range(0.0F, static_cast<float>(k_far_step)));
    anim_wait_ = static_cast<double>(rng_->randf_range(0.0F, static_cast<float>(k_far_anim)));
    set_process(true);
}

double SkydotActor::ai_interval(std::int32_t lod) {
    return lod <= 0 ? 0.0 : lod == 1 ? k_mid_ai : k_far_ai;
}

godot::AnimationPlayer* SkydotActor::player() const {
    return godot::Object::cast_to<godot::AnimationPlayer>(get_node_or_null("AnimationPlayer"));
}

void SkydotActor::update_lod() {
    const godot::Viewport* viewport = get_viewport();
    const godot::Camera3D* camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
    if (camera == nullptr || flight_active_) {
        lod_ = 0;
        on_screen_ = true;
        return;
    }
    const Vector3 feet = get_global_position();
    const double d = static_cast<double>(feet.distance_to(camera->get_global_position()));
    const double near = lod_ == 0 ? k_lod_near * (1.0 + k_lod_hysteresis) : k_lod_near;
    const double far = lod_ == 2 ? k_lod_far : k_lod_far * (1.0 - k_lod_hysteresis);
    lod_ = d < near ? 0 : d < far ? 1 : 2;
    // On screen: its feet, middle or head are in view (a body's width at the
    // screen's edge is a sliver). Near actors stay as they are: behind the
    // camera they still cast shadows into view.
    const Vector3 up(0, r(get_height()), 0);
    on_screen_ = lod_ == 0 || camera->is_position_in_frustum(feet) ||
                 camera->is_position_in_frustum(feet + up * 0.5F) ||
                 camera->is_position_in_frustum(feet + up);
}

void SkydotActor::animate_lod(double delta) {
    godot::AnimationPlayer* anim = player();
    if (anim == nullptr) {
        return;
    }
    // Near actors' players run themselves; the others are advanced here, at
    // a rate that falls with distance, and not while off screen.
    const bool by_hand = lod_ > 0;
    if (by_hand != manual_) {
        manual_ = by_hand;
        anim->set_callback_mode_process(manual_ ? godot::AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL
                                                : godot::AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_IDLE);
        if (manual_ && !posed_) {
            // Never in view while near: show the clip's first pose at least.
            anim->advance(0.0);
            posed_ = true;
        }
        anim_wait_ = 0.0;
    }
    if (!manual_) {
        posed_ = true;
        return;
    }
    if (!on_screen_) {
        anim_wait_ = 0.0;
        return;
    }
    anim_wait_ += delta;
    if (anim_wait_ >= (lod_ == 1 ? k_mid_anim : k_far_anim)) {
        anim->advance(anim_wait_);
        anim_wait_ = 0.0;
    }
}

void SkydotActor::_process(double delta) {
    if (dead_) {
        return;
    }
    lod_wait_ -= delta;
    if (lod_wait_ <= 0.0) {
        lod_wait_ = k_lod_interval;
        update_lod();
    }
    animate_lod(delta);
    scan_events();
}

void SkydotActor::scan_events() {
    godot::AnimationPlayer* anim = player();
    if (anim == nullptr) {
        return;
    }
    const godot::String name = anim->get_assigned_animation();
    const godot::Ref<godot::Animation> clip = name.is_empty() ? godot::Ref<godot::Animation>()
                                                              : anim->get_animation(name);
    if (clip.is_null()) {
        scan_clip_ = "";
        return;
    }
    // The clip's keys: its markers, "#n" (made unique when the clip was built) cut off.
    EventKeys& cache = event_keys_[clip->get_instance_id()];
    const godot::PackedFloat64Array times = clip->get_meta("skydot_event_times", godot::PackedFloat64Array());
    const godot::PackedStringArray texts = clip->get_meta("skydot_event_texts", godot::PackedStringArray());
    const godot::PackedStringArray names = times.is_empty() ? clip->get_marker_names() : godot::PackedStringArray();
    const auto count = static_cast<std::int32_t>(times.is_empty() ? names.size() : times.size());
    if (cache.markers != count) {
        cache.markers = count;
        cache.keys.clear();
        for (int64_t i = 0; i < times.size() && i < texts.size(); ++i) {
            cache.keys.push_back({times[i], texts[i]});
        }
        for (const godot::String& marker : names) {
            godot::String text = marker;
            const std::int64_t hash = text.rfind("#");
            if (hash > 0 && text.substr(static_cast<std::int32_t>(hash + 1)).is_valid_int()) {
                text = text.substr(0, static_cast<std::int32_t>(hash));
            }
            cache.keys.push_back({static_cast<double>(clip->get_marker_time(marker)), text});
        }
        std::stable_sort(cache.keys.begin(), cache.keys.end(),
                         [](const EventKey& a, const EventKey& b) { return a.time < b.time; });
    }
    const double pos = static_cast<double>(anim->get_current_animation_position());
    const double last = name == scan_clip_ ? scan_pos_ : -1.0;
    scan_clip_ = name;
    scan_pos_ = pos;
    if (cache.keys.empty()) {
        return;
    }
    // Behind where it was: the clip restarted, or looped round. A loop first runs out to the end.
    const bool wrapped = pos < last;
    const bool loops = clip->get_loop_mode() != godot::Animation::LOOP_NONE;
    std::vector<godot::String> raised;
    if (wrapped && loops) {
        for (const EventKey& key : cache.keys) {
            if (key.time > last) {
                raised.push_back(key.text);
            }
        }
    }
    const double from = wrapped ? -1.0 : last;
    for (const EventKey& key : cache.keys) {
        if (key.time > from && key.time <= pos) {
            raised.push_back(key.text);
        }
    }
    for (const godot::String& text : raised) {
        emit_signal("animation_event", text);
        // "Func.DragonBiteEffect" (the dragons' hit keys) is the graph's event "DragonBiteEffect": the
        // behaviour graphs list only the plain names, so scripts register for those.
        if (text.begins_with("Func.") && text.length() > 5) {
            emit_signal("animation_event", text.substr(5));
        }
    }
}

godot::String SkydotActor::get_state() const {
    if (get_hold()) {
        return "held";
    }
    if (path_.is_empty()) {
        return "idle";
    }
    return run_ ? "run" : "walk";
}

bool SkydotActor::walk_to(const Vector3& target, bool run) {
    path_.clear();
    if (!is_inside_tree()) {
        return false;
    }
    auto* nav = godot::NavigationServer3D::get_singleton();
    const godot::RID map = get_world_3d()->get_navigation_map();
    if (nav == nullptr || !map.is_valid() || nav->map_get_iteration_id(map) == 0) {
        return false;
    }
    const Vector3 here = get_global_position();
    const Vector3 from = nav->map_get_closest_point(map, here);
    if (from.distance_to(here) > r(k_off_mesh)) {
        return false;
    }
    const Vector3 to = nav->map_get_closest_point(map, target);
    godot::PackedVector3Array path = nav->map_get_path(map, from, to, true);
    if (path.size() < 2) {
        return false;
    }
    path_ = path;
    wake_ = true;
    corner_ = 1;
    run_ = run;
    stuck_time_ = 0.0;
    stuck_from_ = here;
    return true;
}

bool SkydotActor::walk_straight(const Vector3& target, bool run) {
    if (!is_inside_tree()) {
        return false;
    }
    path_.clear();
    path_.push_back(get_global_position());
    path_.push_back(target);
    wake_ = true;
    corner_ = 1;
    run_ = run;
    stuck_time_ = 0.0;
    stuck_from_ = get_global_position();
    return true;
}

void SkydotActor::stop() {
    wake_ = true;
    path_.clear();
    corner_ = 0;
    set_input(godot::Vector2(), 0.0, WALK);
}

bool SkydotActor::find_ground(double delta) {
    ground_wait_ -= delta;
    if (ground_wait_ > 0.0 || !is_inside_tree()) {
        return false;
    }
    ground_wait_ = k_ground_poll;
    auto* space = get_world_3d()->get_direct_space_state();
    if (space == nullptr) {
        return false;
    }
    const Vector3 feet = get_global_position();
    const auto query = godot::PhysicsRayQueryParameters3D::create(
        feet + Vector3(0, r(get_step_height() + 0.1), 0), feet - Vector3(0, 4, 0),
        physics_layer::world | physics_layer::terrain);
    godot::Dictionary hit = space->intersect_ray(query);
    if (hit.is_empty()) {
        // Placed a little low (a dragon set at the ground height of another spot, the ground
        // at its feet is higher than its step): a ray from half its height up finds it.
        const double up = std::max(get_step_height() + 0.1, get_height() * 0.6);
        const auto raised = godot::PhysicsRayQueryParameters3D::create(
            feet + Vector3(0, r(up), 0), feet - Vector3(0, 4, 0), physics_layer::world | physics_layer::terrain);
        hit = space->intersect_ray(raised);
    }
    if (hit.is_empty()) {
        // Placed high (the Bleak Falls spider hangs from the ceiling): the first floor below.
        const auto far = godot::PhysicsRayQueryParameters3D::create(
            feet + Vector3(0, r(get_step_height() + 0.1), 0), feet - Vector3(0, r(k_drop_search), 0),
            physics_layer::world | physics_layer::terrain);
        hit = space->intersect_ray(far);
    }
    if (hit.is_empty()) {
        return false;
    }
    const Vector3 ground = hit["position"];
    teleport(ground);
    return true;
}

void SkydotActor::think(double delta) {
    if (!wander_ || clip_walk_ <= 0.0 || !path_.is_empty()) {
        return;
    }
    wait_ -= delta;
    if (wait_ > 0.0) {
        return;
    }
    // A point within the radius, uniform over the disc.
    const double angle = static_cast<double>(rng_->randf()) * k_tau;
    const double dist = std::sqrt(static_cast<double>(rng_->randf())) * wander_radius_;
    const Vector3 target = home_ + Vector3(r(std::cos(angle) * dist), 0, r(std::sin(angle) * dist));
    if (walk_to(target, false)) {
        // Not round the houses: a path much longer than the radius is not a stroll.
        double length = 0.0;
        for (std::int64_t i = 1; i < path_.size(); ++i) {
            length += static_cast<double>(path_[i].distance_to(path_[i - 1]));
        }
        if (length > 3.0 * wander_radius_ || length < 1.0) {
            stop();
        }
    }
    wait_ = static_cast<double>(path_.is_empty() ? rng_->randf_range(1.0F, 3.0F) : rng_->randf_range(6.0F, 20.0F));
}

void SkydotActor::steer(double delta) {
    if (action_left_ > 0.0 && turn_rate_ < k_turn_rate) {
        // A slow turner (the dragon) stands and keeps its heading while an action plays: a bite, a tail
        // whip or a breath goes where it was aimed.
        if (!path_.is_empty()) {
            stop();
        }
        set_input(godot::Vector2(), 0.0, WALK);
        return;
    }
    if (path_.is_empty()) {
        set_input(godot::Vector2(), 0.0, WALK);
        turn_to_look(delta);
        return;
    }
    const Vector3 here = get_global_position();
    Vector3 to = path_[corner_] - here;
    to.y = 0;
    // A step of a lower rate may carry it further than the usual reach.
    const Vector3 v = get_velocity();
    const double reach =
        std::max(k_corner_reach, static_cast<double>(Vector3(v.x, 0, v.z).length()) * delta);
    while (static_cast<double>(to.length()) < reach) {
        if (++corner_ >= path_.size()) {
            stop();
            return;
        }
        to = path_[corner_] - here;
        to.y = 0;
    }
    if (door_wait_ > 0.0) {
        door_wait_ -= delta;
        set_input(godot::Vector2(), 0.0, WALK);
        stuck_time_ = 0.0;
        stuck_from_ = here;
        return;
    }
    door_probe_ -= delta;
    if (door_probe_ <= 0.0) {
        door_probe_ = k_door_probe;
        if (open_door_ahead(to.normalized())) {
            door_wait_ = k_door_wait;
            return;
        }
    }
    if (look_active_) {
        // Walking while facing a point: the body turns to it and the corner is reached sideways
        // or backwards (the gait clip still plays forward).
        turn_to_look(delta);
        const Vector3 dir = to.normalized();
        const Vector3 forward(r(-std::sin(facing_)), 0, r(-std::cos(facing_)));
        const Vector3 right(r(std::cos(facing_)), 0, r(-std::sin(facing_)));
        const bool slow = turn_rate_ < k_turn_rate;
        if (slow) {
            // A slow turner walks only where it faces and turns on the spot otherwise.
            const double ahead = static_cast<double>(dir.dot(forward));
            const bool go = ahead > k_slow_walk_cos;
            set_input(godot::Vector2(0, go ? r(ahead) : 0.0F), 0.0, run_ ? RUN : WALK);
            if (!go) {
                stuck_time_ = 0.0;
                stuck_from_ = here;
                return;
            }
        } else {
            set_input(godot::Vector2(dir.dot(right), dir.dot(forward)), 0.0, run_ ? RUN : WALK);
        }
        stuck_time_ += delta;
        if (stuck_time_ >= k_stuck_window) {
            if (static_cast<double>(here.distance_to(stuck_from_)) < k_stuck_distance) {
                stop();
                wait_ = static_cast<double>(rng_->randf_range(3.0F, 8.0F));
            }
            stuck_time_ = 0.0;
            stuck_from_ = here;
        }
        return;
    }
    const double want = std::atan2(-static_cast<double>(to.x), -static_cast<double>(to.z));
    const double diff = wrap(want - facing_);
    const double step = turn_rate_ * delta;
    face(facing_ + std::clamp(diff, -step, step));
    // Turn on the spot when the corner is behind, ease in when it is to the side.
    const bool slow = turn_rate_ < k_turn_rate;
    const double ahead = std::abs(diff) < std::numbers::pi / 2 && (!slow || std::cos(diff) > k_slow_walk_cos)
                             ? std::cos(diff)
                             : 0.0;
    set_input(godot::Vector2(0, r(ahead)), 0.0, run_ ? RUN : WALK);
    if (slow && ahead == 0.0) {
        stuck_time_ = 0.0;
        stuck_from_ = here;
        return;
    }

    stuck_time_ += delta;
    if (stuck_time_ >= k_stuck_window) {
        if (static_cast<double>(here.distance_to(stuck_from_)) < k_stuck_distance) {
            stop();
            wait_ = static_cast<double>(rng_->randf_range(3.0F, 8.0F));
        }
        stuck_time_ = 0.0;
        stuck_from_ = here;
    }
}

bool SkydotActor::open_door_ahead(const Vector3& dir) {
    auto* space = get_world_3d()->get_direct_space_state();
    if (space == nullptr) {
        return false;
    }
    // Waist high: over thresholds and rugs, under lintels.
    const Vector3 from = get_global_position() + Vector3(0, 1, 0);
    const auto query = godot::PhysicsRayQueryParameters3D::create(
        from, from + dir * r(get_radius() + k_door_reach), physics_layer::world);
    const godot::Dictionary hit = space->intersect_ray(query);
    if (hit.is_empty()) {
        return false;
    }
    godot::Node3D* door = model_of(hit["collider"]);
    if (door == nullptr || !door->has_meta("skydot_plain_door") || door->get_meta("skydot_open", false)) {
        return false;
    }
    auto* animator = godot::Object::cast_to<SkydotAnimator>(door->get_node_or_null("SkydotAnimator"));
    if (animator == nullptr || !animator->play("Open")) {
        return false;
    }
    door->set_meta("skydot_open", true);
    door->set_meta("skydot_opened_by", static_cast<std::int64_t>(get_instance_id()));
    opened_.push_back({door->get_instance_id(), 0.0});
    emit_signal("door_toggled", door->get_meta("skydot_ref"), k_door_open);
    return true;
}

void SkydotActor::close_doors(double delta) {
    const Vector3 here = get_global_position();
    std::erase_if(opened_, [&](OpenedDoor& o) {
        o.time += delta;
        auto* door = godot::Object::cast_to<godot::Node3D>(godot::ObjectDB::get_instance(o.model));
        if (door == nullptr || !door->is_inside_tree()) {
            return true;
        }
        // Someone else closed or took it over meanwhile.
        if (!door->get_meta("skydot_open", false) ||
            static_cast<std::int64_t>(door->get_meta("skydot_opened_by", 0)) !=
                static_cast<std::int64_t>(get_instance_id())) {
            return true;
        }
        Vector3 away = door->get_global_position() - here;
        away.y = 0;
        if (o.time < k_door_close_after || static_cast<double>(away.length()) < k_door_close_distance) {
            return false;
        }
        auto* animator = godot::Object::cast_to<SkydotAnimator>(door->get_node_or_null("SkydotAnimator"));
        if (animator != nullptr && animator->play("Close")) {
            door->set_meta("skydot_open", false);
            emit_signal("door_toggled", door->get_meta("skydot_ref"), k_door_closed);
        }
        return true;
    });
}

void SkydotActor::animate() {
    godot::AnimationPlayer* anim = player();
    if (anim == nullptr) {
        return;
    }
    if (action_left_ > 0.0) {
        return; // an action plays over the gait
    }
    const Vector3 v = get_velocity();
    const double speed = get_hold() ? 0.0 : static_cast<double>(Vector3(v.x, 0, v.z).length());
    godot::String want = "idle";
    double clip = 0.0;
    if (speed >= k_idle_speed) {
        if (run_ && clip_run_ > 0.0 && anim->has_animation("run")) {
            want = "run";
            clip = stance_run_ && stance_run_speed_ > 0.0 ? stance_run_speed_ : clip_run_;
        } else if (anim->has_animation("walk")) {
            want = "walk";
            clip = stance_walk_ && stance_walk_speed_ > 0.0 ? stance_walk_speed_ : clip_walk_;
        }
    }
    // Standing and turning: the turn clip of the side it turns to (a dragon's 45 degree turns).
    const double turned = wrap(facing_ - animated_facing_);
    animated_facing_ = facing_;
    if (want == godot::String("idle") && std::abs(turned) > k_turn_visible) {
        const godot::String turn = turned > 0.0 ? "turn_left" : "turn_right";
        if (anim->has_animation(turn)) {
            want = turn;
        }
    }
    want = gait_clip(want);
    if (!anim->has_animation(want)) {
        return;
    }
    // Feet keep pace with the ground: the clip plays as fast as the body moves.
    anim->set_speed_scale(clip > 0.0 ? r(std::clamp(speed / clip, 0.5, 2.0)) : 1.0F);
    if (want != playing_ || !anim->is_playing()) {
        anim->play(want, k_blend);
        playing_ = want;
    }
}

void SkydotActor::_physics_process(double engine_delta) {
    if (dead_) {
        return;
    }
    if (flight_active_) {
        // The mover has the body: no gravity, no slide, no steering, no throttling.
        flight_tick(engine_delta);
        return;
    }
    // Level of detail: a farther actor steps less often, and then for the
    // time since its last step. A standing one only looks for ground.
    double delta = engine_delta;
    if (lod_ > 0) {
        step_wait_ += engine_delta;
        const Vector3 v = get_velocity();
        const bool standing = !get_hold() && path_.is_empty() && is_on_floor() &&
                              static_cast<double>(Vector3(v.x, 0, v.z).length()) < k_idle_speed;
        const double every = standing ? k_stand_step : lod_ == 1 ? k_mid_step : k_far_step;
        if (step_wait_ < every && !wake_) {
            return;
        }
        delta = step_wait_;
    }
    step_wait_ = 0.0;
    wake_ = false;
    if (action_left_ > 0.0) {
        action_left_ -= delta;
        if (action_left_ <= 0.0) {
            action_left_ = 0.0;
            playing_ = ""; // the gait clip starts again
        }
    }
    if (get_hold()) {
        if (find_ground(delta)) {
            set_hold(false);
        }
    } else if (!frozen_) {
        think(delta);
        steer(delta);
    }
    close_doors(delta);
    SkydotPlayer::_physics_process(delta);
    if (!get_hold() && get_global_position().y < home_.y - r(k_lost_depth)) {
        // Fell through something not built yet: back home, wait for ground.
        stop();
        teleport(home_);
        set_hold(true);
    }
    animate();
}

// ---- flight ----------------------------------------------------------------------------

namespace {

constexpr double k_ground_cell = 50.0;     ///< Skyrim units: one cached ray per square this big.
constexpr double k_ground_cache_life = 0.5; ///< Seconds.
constexpr double k_ray_up = 20000.0;       ///< Skyrim units above the probe height the ray starts.
constexpr double k_ray_down = 40000.0;

const char* mode_name(flight::Mode m) {
    switch (m) {
    case flight::Mode::grounded: return "grounded";
    case flight::Mode::takeoff: return "takeoff";
    case flight::Mode::cruise: return "cruise";
    case flight::Mode::orbit: return "orbit";
    case flight::Mode::hover: return "hover";
    case flight::Mode::pass: return "pass";
    case flight::Mode::approach: return "approach";
    case flight::Mode::landing: return "landing";
    case flight::Mode::crash: return "crash";
    case flight::Mode::crashed: return "crashed";
    }
    return "grounded";
}

} // namespace

godot::String SkydotActor::get_flight_mode() const { return mode_name(flight_body_.mode); }

void SkydotActor::set_flight(const flight::Params& params, const std::string& clip_dir, flight::Ground terrain) {
    flight_enabled_ = true;
    flight_params_ = params;
    flight_dir_ = clip_dir;
    flight_terrain_ = std::move(terrain);
}

void SkydotActor::fly_command(const godot::String& kind, const godot::Vector3& point_skyrim, double radius,
                              double height, bool dive) {
    flight::Command c;
    const std::string k = kind.utf8().get_data();
    using flight::CommandKind;
    if (k == "takeoff") c.kind = CommandKind::takeoff;
    else if (k == "fly_to") c.kind = CommandKind::fly_to;
    else if (k == "orbit") c.kind = CommandKind::orbit;
    else if (k == "hover") c.kind = CommandKind::hover;
    else if (k == "pass") c.kind = CommandKind::pass;
    else if (k == "land") c.kind = CommandKind::land;
    else if (k == "crash") c.kind = CommandKind::crash;
    else c.kind = CommandKind::keep;
    c.point = {static_cast<double>(point_skyrim.x), static_cast<double>(point_skyrim.y),
               static_cast<double>(point_skyrim.z)};
    c.radius = radius;
    c.height = height;
    c.dive = dive;
    fly(c);
}

void SkydotActor::fly(const flight::Command& command) {
    if (!flight_enabled_ || dead_ || command.kind == flight::CommandKind::keep) {
        return;
    }
    flight_command_ = command;
    if (flight_active_) {
        return;
    }
    if (flight_body_.mode == flight::Mode::crashed) {
        return;
    }
    // Hand the body to the mover from where it stands.
    const Vector3 at = godot_to_skyrim(get_global_position());
    flight_body_.pos = {static_cast<double>(at.x), static_cast<double>(at.y), static_cast<double>(at.z)};
    flight_body_.yaw = flight::heading_from_facing(facing_);
    flight_body_.last_ground = flight_body_.pos.z;
    flight_body_.yaw_rate = 0;
    flight_body_.vz = 0;
    flight_body_.speed = 0;
    flight_body_.bank = 0;
    flight_body_.pitch = 0;
    const bool takeoff = command.kind == flight::CommandKind::takeoff;
    if (command.kind == flight::CommandKind::land) {
        return; // on the ground already
    }
    if (!takeoff) {
        // Placed in the air: it flies from here.
        flight_body_.mode = flight::Mode::cruise;
        flight_body_.mode_time = 0;
        flight_body_.speed = flight_params_.cruise_speed;
    }
    flight_active_ = true;
    flight_clip_.clear();
    set_hold(false);
    set_velocity(Vector3());
    wake_ = true;
}

double SkydotActor::flight_ground(double x, double y) {
    const auto key = (static_cast<std::int64_t>(std::floor(x / k_ground_cell)) << 32) ^
                     (static_cast<std::int64_t>(std::floor(y / k_ground_cell)) & 0xFFFFFFFF);
    const auto it = ground_cache_.find(key);
    if (it != ground_cache_.end() && it->second.expires > flight_clock_) {
        return it->second.height;
    }
    if (ground_cache_.size() > 8192) {
        std::erase_if(ground_cache_, [&](const auto& e) { return e.second.expires <= flight_clock_; });
    }
    double height = std::nan("");
    auto* world3d = is_inside_tree() ? get_world_3d().ptr() : nullptr;
    auto* space = world3d != nullptr ? world3d->get_direct_space_state() : nullptr;
    if (space != nullptr) {
        const double top = flight_body_.pos.z + k_ray_up;
        const Vector3 from = skyrim_position(Vector3(static_cast<real_t>(x), static_cast<real_t>(y),
                                                     static_cast<real_t>(top)));
        const Vector3 to = skyrim_position(Vector3(static_cast<real_t>(x), static_cast<real_t>(y),
                                                   static_cast<real_t>(top - k_ray_up - k_ray_down)));
        const auto query = godot::PhysicsRayQueryParameters3D::create(
            from, to, physics_layer::world | physics_layer::terrain);
        const godot::Dictionary hit = space->intersect_ray(query);
        if (!hit.is_empty()) {
            const Vector3 p = hit["position"];
            height = static_cast<double>(godot_to_skyrim(p).z);
        }
    }
    if (std::isnan(height) && flight_terrain_) {
        height = flight_terrain_(x, y);
    }
    ground_cache_[key] = {height, flight_clock_ + k_ground_cache_life};
    return height;
}

void SkydotActor::flight_pose() {
    godot::Node3D* units = godot::Object::cast_to<godot::Node3D>(get_node_or_null("bethconv_z_up_to_y_up"));
    if (units == nullptr) {
        return;
    }
    if (!visual_known_) {
        visual_rest_ = units->get_transform();
        visual_known_ = true;
    }
    const bool flying = flight_active_;
    const double pitch = flying ? flight_body_.pitch : 0.0;
    const double roll = flying ? flight_body_.bank * flight_params_.bank_roll_scale : 0.0;
    // Nose up about +X (the body looks down -Z); the left wing (-X) goes down with a positive bank.
    const godot::Basis tilt = godot::Basis(Vector3(0, 0, 1), r(roll)) * godot::Basis(Vector3(1, 0, 0), r(pitch));
    units->set_transform(godot::Transform3D(tilt * visual_rest_.basis, tilt.xform(visual_rest_.origin)));
}

void SkydotActor::animate_flight() {
    using flight::Mode;
    const std::string stem = flight::clip_for(flight_body_, flight_params_, flight_hurt_);
    if (stem.empty() || stem == flight_clip_) {
        return;
    }
    if (action_left_ > 0.0 && !flight_own_action_) {
        return; // an action (a breath) plays over the flight clip
    }
    flight_own_action_ = true;
    const Mode m = flight_body_.mode;
    const bool once = m == Mode::takeoff || m == Mode::landing || m == Mode::crash || m == Mode::crashed;
    if (play_clip(godot::String::utf8((flight_dir_ + stem + ".hkx").c_str()), 1.0, !once) > 0.0) {
        flight_clip_ = stem;
    } else {
        flight_clip_ = stem; // not in the pack: do not ask again every frame
    }
}

void SkydotActor::flight_tick(double delta) {
    flight_clock_ += delta;
    wake_ = false;
    step_wait_ = 0.0;
    if (action_left_ > 0.0) {
        action_left_ -= delta;
        if (action_left_ <= 0.0) {
            action_left_ = 0.0;
            if (!flight_own_action_) {
                flight_clip_.clear(); // the flight clip starts again after a breath
            }
        }
    }
    const flight::Events ev = flight::step(
        flight_body_, flight_command_, flight_params_,
        [this](double x, double y) { return flight_ground(x, y); }, delta);
    const Vector3 at = skyrim_position(Vector3(static_cast<real_t>(flight_body_.pos.x),
                                               static_cast<real_t>(flight_body_.pos.y),
                                               static_cast<real_t>(flight_body_.pos.z)));
    set_global_position(at);
    set_velocity(Vector3());
    face(flight::facing_from_heading(flight_body_.yaw));
    if (ev.took_off) {
        flight_command_ = flight::Command{};
        emit_signal("flight_took_off");
    }
    if (ev.landed || ev.crashed) {
        flight_command_ = flight::Command{};
        flight_active_ = false;
        flight_clip_.clear();
        playing_ = "";
        action_left_ = 0.0;
        flight_pose();
        set_hold(false);
        animated_facing_ = facing_;
        if (godot::AnimationPlayer* anim = player()) {
            anim->set_speed_scale(1.0F);
        }
        emit_signal(ev.landed ? "flight_landed" : "flight_crashed");
        return;
    }
    flight_pose();
    animate_flight();
}

} // namespace skydot
