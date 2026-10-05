// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/image_space.hpp"
#include "render/shader_source.hpp"

#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/uniform_set_cache_rd.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

using godot::Dictionary;
using godot::PackedByteArray;
using godot::Ref;
using godot::RID;
using godot::RenderingDevice;

namespace skydot {

namespace {

// Each thread sums 4x4 pixels, a 16x16 group 64x64.
constexpr int k_reduce_tile = 64;

RenderingDevice* device() {
    auto* rs = godot::RenderingServer::get_singleton();
    return rs != nullptr ? rs->get_rendering_device() : nullptr;
}

/// The text Godot takes of the compute shader file `file`.
godot::String source_of(const char* file) {
    const std::string& code = shader_source::load(file);
    return godot::String::utf8(code.c_str(), static_cast<int>(code.size()));
}

RID compile(RenderingDevice* rd, const char* file, const char* name) {
    Ref<godot::RDShaderSource> source;
    source.instantiate();
    source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
    source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, source_of(file));
    const Ref<godot::RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
    if (spirv.is_null()) {
        return {};
    }
    const godot::String error = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
    if (!error.is_empty()) {
        godot::UtilityFunctions::push_error(godot::String("SkydotImageSpace: ") + name + ": " + error);
        return {};
    }
    return rd->shader_create_from_spirv(spirv, name);
}

Ref<godot::RDUniform> uniform(RenderingDevice::UniformType type, int binding, const RID& rid) {
    Ref<godot::RDUniform> u;
    u.instantiate();
    u->set_uniform_type(type);
    u->set_binding(binding);
    u->add_id(rid);
    return u;
}

RID uniform_set(const RID& shader, const Ref<godot::RDUniform>& a, const Ref<godot::RDUniform>& b) {
    godot::TypedArray<godot::RDUniform> list;
    list.push_back(a);
    list.push_back(b);
    return godot::UniformSetCacheRD::get_cache(shader, 0, list);
}

PackedByteArray bytes(const float* values, std::size_t count) {
    PackedByteArray out;
    out.resize(static_cast<std::int64_t>(count * 4));
    std::memcpy(out.ptrw(), values, count * 4);
    return out;
}

float value(const godot::Variant& list, int index, float fallback) {
    if (list.get_type() != godot::Variant::PACKED_FLOAT32_ARRAY && list.get_type() != godot::Variant::ARRAY) {
        return fallback;
    }
    const godot::Array a = list;
    return index < a.size() ? static_cast<float>(static_cast<double>(a[index])) : fallback;
}

} // namespace

SkydotImageSpace::SkydotImageSpace() {
    set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
}

SkydotImageSpace::~SkydotImageSpace() {
    free_pipelines();
}

Dictionary SkydotImageSpace::shader_codes() {
    Dictionary out;
    out["image_space_reduce"] = source_of("image_space_reduce.comp");
    out["image_space_adapt"] = source_of("image_space_adapt.comp");
    out["image_space_grade"] = source_of("image_space_grade.comp");
    return out;
}

void SkydotImageSpace::set_image_space(const Dictionary& image_space) {
    Params p;
    const godot::Variant hdr = image_space.get("hdr", godot::Variant());
    const godot::Variant cinematic = image_space.get("cinematic", godot::Variant());
    const godot::Variant tint = image_space.get("tint", godot::Variant());
    const float white = value(hdr, 5, 1.0F);
    p.reinhard = white > 0.0F ? 1.0F / (white * white) : 1.0F;
    p.adapt_speed = value(hdr, 0, 0.0F);
    // Capped at 1: with Update.esm's clear day (1.5) three comparison shots
    // came out half again as saturated as the game's, while its brightness
    // and contrast matched them; at 1 the saturation matched too
    // (COMPARISON-SHOTS.md). The game's handling of values over 1 is unknown.
    p.saturation = std::min(value(cinematic, 0, 1.0F), 1.0F);
    p.brightness = value(cinematic, 1, 1.0F);
    p.contrast = value(cinematic, 2, 1.0F);
    p.tint = godot::Color(value(tint, 1, 1.0F), value(tint, 2, 1.0F), value(tint, 3, 1.0F),
                          value(tint, 0, 0.0F));
    const std::scoped_lock lock(mutex_);
    p.reset = params_.reset;
    params_ = p;
}

void SkydotImageSpace::clear() {
    set_image_space(Dictionary());
}

void SkydotImageSpace::reset_adaptation() {
    const std::scoped_lock lock(mutex_);
    params_.reset = true;
}

bool SkydotImageSpace::ensure_pipelines() {
    if (grade_pipeline_.is_valid()) {
        return true;
    }
    if (failed_) {
        return false;
    }
    RenderingDevice* rd = device();
    if (rd == nullptr) {
        return false;
    }
    reduce_shader_ = compile(rd, "image_space_reduce.comp", "skydot_image_space_reduce");
    adapt_shader_ = compile(rd, "image_space_adapt.comp", "skydot_image_space_adapt");
    grade_shader_ = compile(rd, "image_space_grade.comp", "skydot_image_space_grade");
    if (!reduce_shader_.is_valid() || !adapt_shader_.is_valid() || !grade_shader_.is_valid()) {
        failed_ = true;
        free_pipelines();
        return false;
    }
    reduce_pipeline_ = rd->compute_pipeline_create(reduce_shader_);
    adapt_pipeline_ = rd->compute_pipeline_create(adapt_shader_);
    grade_pipeline_ = rd->compute_pipeline_create(grade_shader_);
    const float zero[4] = {0, 0, 0, 0};
    average_ = rd->storage_buffer_create(16, bytes(zero, 4));
    return true;
}

void SkydotImageSpace::free_pipelines() {
    RenderingDevice* rd = device();
    if (rd == nullptr) {
        return;
    }
    // Pipelines go with their shaders.
    for (RID* rid : {&partials_, &average_, &reduce_shader_, &adapt_shader_, &grade_shader_}) {
        if (rid->is_valid()) {
            rd->free_rid(*rid);
            *rid = RID();
        }
    }
    reduce_pipeline_ = adapt_pipeline_ = grade_pipeline_ = RID();
    partial_capacity_ = 0;
}

void SkydotImageSpace::_render_callback(int32_t type, godot::RenderData* data) {
    if (type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT || data == nullptr || !ensure_pipelines()) {
        return;
    }
    RenderingDevice* rd = device();
    Ref<godot::RenderSceneBuffersRD> buffers = data->get_render_scene_buffers();
    if (rd == nullptr || buffers.is_null()) {
        return;
    }
    const godot::Vector2i size = buffers->get_internal_size();
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    const auto groups_x = static_cast<std::uint32_t>((size.x + k_reduce_tile - 1) / k_reduce_tile);
    const auto groups_y = static_cast<std::uint32_t>((size.y + k_reduce_tile - 1) / k_reduce_tile);
    const std::uint32_t partials = groups_x * groups_y;
    if (partials > partial_capacity_) {
        if (partials_.is_valid()) {
            rd->free_rid(partials_);
        }
        partials_ = rd->storage_buffer_create(partials * 4);
        partial_capacity_ = partials;
    }

    Params p;
    {
        const std::scoped_lock lock(mutex_);
        p = params_;
        params_.reset = false;
    }
    const double now = static_cast<double>(godot::Time::get_singleton()->get_ticks_usec()) / 1e6;
    const double dt = last_ticks_ > 0.0 ? std::clamp(now - last_ticks_, 0.0, 0.25) : 0.0;
    last_ticks_ = now;
    // HNAM's eye adapt speed has no documented unit; read as a tenth of
    // the gap closed per second (45, the clear day's, closes it in about
    // a quarter of a second).
    const auto rate = static_cast<float>(std::clamp(static_cast<double>(p.adapt_speed) * 0.1 * dt, 0.0, 1.0));

    for (std::uint32_t view = 0; view < buffers->get_view_count(); ++view) {
        const RID color = buffers->get_color_layer(view);
        const auto image = uniform(RenderingDevice::UNIFORM_TYPE_IMAGE, 0, color);

        const std::int32_t reduce_pc[4] = {size.x, size.y, static_cast<std::int32_t>(groups_x), 0};
        const float adapt_pc[4] = {static_cast<float>(partials),
                                   static_cast<float>(size.x) * static_cast<float>(size.y), rate,
                                   p.reset ? 1.0F : 0.0F};
        const float grade_pc[12] = {p.tint.r, p.tint.g, p.tint.b, p.tint.a,
                                    p.saturation, p.brightness, p.contrast, p.reinhard,
                                    0, 0, 0, 0};
        float grade_bytes[12];
        std::memcpy(grade_bytes, grade_pc, sizeof(grade_pc));
        const std::int32_t grade_size[4] = {size.x, size.y, 0, 0};
        std::memcpy(&grade_bytes[8], grade_size, sizeof(grade_size));
        float reduce_bytes[4];
        std::memcpy(reduce_bytes, reduce_pc, sizeof(reduce_pc));

        const std::int64_t list = rd->compute_list_begin();
        rd->compute_list_bind_compute_pipeline(list, reduce_pipeline_);
        rd->compute_list_bind_uniform_set(
            list, uniform_set(reduce_shader_, image, uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 1, partials_)), 0);
        rd->compute_list_set_push_constant(list, bytes(reduce_bytes, 4), 16);
        rd->compute_list_dispatch(list, groups_x, groups_y, 1);
        rd->compute_list_add_barrier(list);

        rd->compute_list_bind_compute_pipeline(list, adapt_pipeline_);
        rd->compute_list_bind_uniform_set(
            list,
            uniform_set(adapt_shader_, uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 0, partials_),
                        uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 1, average_)),
            0);
        rd->compute_list_set_push_constant(list, bytes(adapt_pc, 4), 16);
        rd->compute_list_dispatch(list, 1, 1, 1);
        rd->compute_list_add_barrier(list);

        rd->compute_list_bind_compute_pipeline(list, grade_pipeline_);
        rd->compute_list_bind_uniform_set(
            list, uniform_set(grade_shader_, image, uniform(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER, 1, average_)), 0);
        rd->compute_list_set_push_constant(list, bytes(grade_bytes, 12), 48);
        rd->compute_list_dispatch(list, static_cast<std::uint32_t>((size.x + 7) / 8),
                                  static_cast<std::uint32_t>((size.y + 7) / 8), 1);
        rd->compute_list_end();
        p.reset = false; // the other views adapt with the first
    }
}

void SkydotImageSpace::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("set_image_space", "image_space"), &SkydotImageSpace::set_image_space);
    godot::ClassDB::bind_method(D_METHOD("clear"), &SkydotImageSpace::clear);
    godot::ClassDB::bind_method(D_METHOD("reset_adaptation"), &SkydotImageSpace::reset_adaptation);
}

} // namespace skydot
