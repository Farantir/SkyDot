// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotImageSpace`: the game's image space pass (IMGS) as a compositor
// effect. The scene is drawn with the game's gamma-space numbers (see
// materials.hpp); this pass does what the game's ISHDR shader does with them
// (as reverse-engineered by Community Shaders, ISHDR.hlsl) and hands Godot
// linear colour, so its Linear tonemapper's sRGB encoding shows the game's
// result:
//
//   L      = luminance; avg = the eye's adapted mean luminance of the frame
//   c      = c * reinhard(L) / L        reinhard(L) = L (L p + 1) / (L + 1),
//                                       p = 1 / white^2 (identity at 1)
//   tinted = brightness * mix(mix(L, c, saturation), L * tint, tint_amount)
//   out    = mix(avg, tinted, contrast)
//
// The game also scales by an eye adaptation exposure (avg.y / avg.x, whose
// y is not documented) and adds bloom; neither is done here. The adapted
// mean follows the frame's at the image space's eye adapt speed.
//
// The pass must run on every frame of a SkyDot scene, also with a neutral
// image space: without it the gamma-space numbers would reach the screen as
// if they were linear, too bright.
#pragma once

#include <godot_cpp/classes/compositor_effect.hpp>
#include <godot_cpp/classes/render_data.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <mutex>

namespace skydot {

class SkydotImageSpace : public godot::CompositorEffect {
    GDCLASS(SkydotImageSpace, godot::CompositorEffect)

public:
    SkydotImageSpace();
    ~SkydotImageSpace() override;

    /// Take an image space as SkydotWorld.get_image_space or
    /// SkydotWeather.get_image_space give it: "hdr" (nine floats),
    /// "cinematic" (three), "tint" (four). Missing parts are neutral.
    void set_image_space(const godot::Dictionary& image_space);
    /// Neutral: the scene's numbers go to the screen unchanged.
    void clear();
    /// Restart eye adaptation at the next frame's mean (after a load door).
    void reset_adaptation();

    void _render_callback(int32_t type, godot::RenderData* data) override;

    /// The source of every compute shader, by name.
    static godot::Dictionary shader_codes();

protected:
    static void _bind_methods();

private:
    struct Params {
        float saturation = 1.0F;
        float brightness = 1.0F;
        float contrast = 1.0F;
        float reinhard = 1.0F;
        godot::Color tint{1, 1, 1, 0}; ///< rgb and amount in alpha
        float adapt_speed = 0.0F;      ///< HNAM eye adapt speed
        bool reset = true;
    };

    bool ensure_pipelines();
    void free_pipelines();

    mutable std::mutex mutex_;
    Params params_;
    double last_ticks_ = 0.0;

    godot::RID reduce_shader_, reduce_pipeline_;
    godot::RID adapt_shader_, adapt_pipeline_;
    godot::RID grade_shader_, grade_pipeline_;
    godot::RID partials_; ///< per-workgroup sums
    godot::RID average_;  ///< adapted mean luminance, and the frame's
    std::uint32_t partial_capacity_ = 0;
    bool failed_ = false;
};

} // namespace skydot
