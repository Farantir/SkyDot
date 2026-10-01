// SPDX-License-Identifier: GPL-3.0-or-later
#include "nif_controllers.hpp"

#include <Animation.hpp>
#include <ExtraData.hpp>
#include <Nodes.hpp>
#include <Particles.hpp>
#include <Shaders.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_set>

namespace bethconv::mesh::detail {
namespace {

/// Controller chains and key counts come from the file; bound both.
constexpr std::size_t k_max_chain = 64;
constexpr std::size_t k_max_keys = 1u << 16;

// NiTimeController flags: bits 1-2 cycle type, bit 3 active.
constexpr std::uint16_t k_flag_active = 1u << 3;

CycleMode cycle_of(std::int64_t value) {
    switch (value) {
    case 1: return CycleMode::reverse;
    case 2: return CycleMode::clamp;
    default: return CycleMode::loop;
    }
}

/// BSEffectShaderPropertyFloatController variables (nif.xml
/// EffectShaderControlledVariable).
std::string_view effect_float_name(std::uint32_t v) {
    switch (v) {
    case 0: return "emissive_multiple";
    case 1: return "falloff_start_angle";
    case 2: return "falloff_stop_angle";
    case 3: return "falloff_start_opacity";
    case 4: return "falloff_stop_opacity";
    case 5: return "alpha";
    case 6: return "u_offset";
    case 7: return "u_scale";
    case 8: return "v_offset";
    case 9: return "v_scale";
    default: return {};
    }
}

/// BSLightingShaderPropertyFloatController variables (nif.xml
/// LightingShaderControlledVariable).
std::string_view lighting_float_name(std::uint32_t v) {
    switch (v) {
    case 0: return "refraction_strength";
    case 8: return "environment_map_scale";
    case 9: return "glossiness";
    case 10: return "specular_strength";
    case 11: return "emissive_multiple";
    case 12: return "alpha";
    case 20: return "u_offset";
    case 21: return "u_scale";
    case 22: return "v_offset";
    case 23: return "v_scale";
    default: return {};
    }
}

/// A float that NIFs use to mean "no value" (FLT_MAX or -FLT_MAX).
bool is_unset(float v) { return !std::isfinite(v) || std::fabs(v) >= 3.0e38f; }

template <typename T>
void push_components(std::vector<float>& out, const T& v);

template <>
void push_components(std::vector<float>& out, const float& v) { out.push_back(v); }
template <>
void push_components(std::vector<float>& out, const std::uint8_t& v) {
    out.push_back(v != 0 ? 1.0f : 0.0f);
}
template <>
void push_components(std::vector<float>& out, const nifly::Vector3& v) {
    out.insert(out.end(), {v.x, v.y, v.z});
}

/// Kochanek-Bartels tangents, in the per-segment units Gamebryo's Hermite
/// keys use. `tbc` holds tension, bias, continuity per key.
void tbc_to_tangents(AnimationChannel& ch, const std::vector<nifly::TBC>& tbc) {
    const std::size_t n = ch.times.size();
    const std::size_t c = ch.components;
    ch.in_tangents.assign(n * c, 0.0f);
    ch.out_tangents.assign(n * c, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const float t = tbc[i].tension;
        const float b = tbc[i].bias;
        const float k = tbc[i].continuity;
        for (std::size_t j = 0; j < c; ++j) {
            const float p1 = ch.values[i * c + j];
            const float p0 = i > 0 ? ch.values[(i - 1) * c + j] : p1;
            const float p2 = i + 1 < n ? ch.values[(i + 1) * c + j] : p1;
            const float before = p1 - p0;
            const float after = p2 - p1;
            ch.out_tangents[i * c + j] = 0.5f * (1 - t) * ((1 + b) * (1 + k) * before +
                                                           (1 - b) * (1 - k) * after);
            ch.in_tangents[i * c + j] = 0.5f * (1 - t) * ((1 + b) * (1 - k) * before +
                                                          (1 - b) * (1 + k) * after);
        }
    }
}

/// Keys of one NiAnimationKeyGroup into `ch`. Returns false if there are none.
template <typename T>
bool read_keys(const nifly::NiAnimationKeyGroup<T>& group, std::uint8_t components,
               AnimationChannel& ch) {
    const std::uint32_t count = std::min<std::uint32_t>(group.GetNumKeys(), k_max_keys);
    if (count == 0) {
        return false;
    }
    ch.components = components;
    const nifly::NiKeyType type = group.GetInterpolationType();
    ch.interp = type == nifly::LINEAR_KEY                                 ? KeyInterp::linear
                : type == nifly::QUADRATIC_KEY || type == nifly::TBC_KEY ? KeyInterp::cubic
                                                                          : KeyInterp::step;
    std::vector<nifly::TBC> tbc;
    for (std::uint32_t i = 0; i < count; ++i) {
        const nifly::NiAnimationKey<T> key = group.GetKey(static_cast<int>(i));
        ch.times.push_back(key.time);
        push_components(ch.values, key.value);
        if (type == nifly::QUADRATIC_KEY) {
            push_components(ch.in_tangents, key.backward);
            push_components(ch.out_tangents, key.forward);
        }
        tbc.push_back(key.tbc);
    }
    if (type == nifly::TBC_KEY) {
        tbc_to_tangents(ch, tbc);
    }
    return true;
}

AnimationChannel constant(std::size_t node, std::string property, std::vector<float> value) {
    AnimationChannel ch;
    ch.node = node;
    ch.property = std::move(property);
    ch.interp = KeyInterp::step;
    ch.components = static_cast<std::uint8_t>(value.size());
    ch.times.push_back(0.0f);
    ch.values = std::move(value);
    return ch;
}

bool finite_vec(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 vec3(const nifly::Vector3& v) { return Vec3{v.x, v.y, v.z}; }
Vec4 vec4(const nifly::Color4& c) { return Vec4{c.r, c.g, c.b, c.a}; }

} // namespace

void ControllerReader::attach(nifly::NiObjectNET* owner, std::size_t node) {
    if (owner == nullptr) {
        return;
    }
    owners_.push_back(Owner{owner, node});
    node_by_owner_.try_emplace(nif_.GetBlockID(owner), node);
}

void ControllerReader::add_particles(nifly::NiParticleSystem* system, std::size_t node,
                                     std::size_t material) {
    pending_particles_.push_back(PendingParticles{system, node, material});
}

std::optional<std::size_t> ControllerReader::node_of(std::uint32_t block) const {
    const auto it = node_by_block_.find(block);
    if (it == node_by_block_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void ControllerReader::note_unsupported(std::string_view what) {
    ++unsupported_[std::string(what)];
}

void ControllerReader::mark(std::size_t node) {
    if (node < model_.nodes.size()) {
        model_.nodes[node].referenced = true;
    }
}

void ControllerReader::finish() {
    for (const PendingParticles& pending : pending_particles_) {
        read_particles(pending);
    }

    // Standalone controllers, grouped by clock into unnamed clips.
    using Clock = std::tuple<int, float, float, float, float, bool>;
    std::map<Clock, std::size_t> clip_by_clock;
    std::vector<nifly::NiControllerManager*> managers;
    std::unordered_set<std::uint32_t> seen;

    for (const Owner& owner : owners_) {
        auto* controller = nif_.GetHeader().GetBlock<nifly::NiTimeController>(
            owner.object->controllerRef);
        for (std::size_t depth = 0; controller != nullptr && depth < k_max_chain; ++depth) {
            if (!seen.insert(nif_.GetBlockID(controller)).second) {
                break; // Chains can loop back on themselves.
            }
            if (auto* manager = dynamic_cast<nifly::NiControllerManager*>(controller)) {
                managers.push_back(manager);
            } else if (dynamic_cast<nifly::NiMultiTargetTransformController*>(controller) !=
                       nullptr) {
                // Only ever driven through its manager's sequences.
            } else if (auto* interp = dynamic_cast<nifly::NiInterpController*>(controller);
                       interp == nullptr || !interp->managerControlled) {
                std::vector<AnimationChannel> channels;
                auto* single = dynamic_cast<nifly::NiSingleInterpController*>(controller);
                nifly::NiInterpolator* interpolator =
                    single != nullptr ? nif_.GetHeader().GetBlock<nifly::NiInterpolator>(
                                            single->interpolatorRef.index)
                                      : nullptr;
                // A blend interpolator means sequences drive this controller,
                // whatever its own flag says.
                if (dynamic_cast<nifly::NiBlendInterpolator*>(interpolator) == nullptr) {
                    channels_for(controller, {}, interpolator, owner.node, channels);
                }
                if (!channels.empty()) {
                    const Clock clock{static_cast<int>((controller->flags >> 1) & 3u),
                                      controller->frequency, controller->phase,
                                      controller->startTime, controller->stopTime,
                                      (controller->flags & k_flag_active) != 0};
                    auto [it, added] = clip_by_clock.try_emplace(clock, model_.animations.size());
                    if (added) {
                        AnimationClip clip;
                        clip.cycle = cycle_of(std::get<0>(clock));
                        clip.frequency = controller->frequency;
                        clip.phase = controller->phase;
                        clip.start = is_unset(controller->startTime) ? 0.0f : controller->startTime;
                        clip.stop = is_unset(controller->stopTime) ? 0.0f : controller->stopTime;
                        clip.autoplay = std::get<5>(clock);
                        model_.animations.push_back(std::move(clip));
                    }
                    auto& target = model_.animations[it->second].channels;
                    std::ranges::move(channels, std::back_inserter(target));
                }
            }
            controller = nif_.GetHeader().GetBlock<nifly::NiTimeController>(
                controller->nextControllerRef);
        }
    }

    for (nifly::NiControllerManager* manager : managers) {
        for (auto& ref : manager->controllerSequenceRefs) {
            if (auto* sequence = nif_.GetHeader().GetBlock<nifly::NiControllerSequence>(ref)) {
                read_sequence(sequence);
            }
        }
    }

    for (const AnimationClip& clip : model_.animations) {
        for (const AnimationChannel& ch : clip.channels) {
            mark(ch.node);
        }
    }
    resolve_birth_rates();
    for (const auto& [what, count] : unsupported_) {
        model_.warnings.push_back(std::format("{} x{}: not converted", what, count));
    }
}

void ControllerReader::resolve_birth_rates() {
    // The rate a system starts with: its standalone controller's first key,
    // else the highest rate any sequence gives it (effects switched on by
    // `particlesOn` or `mIdle`).
    for (ParticleSystem& ps : model_.particles) {
        float rate = -1.0f;
        for (const AnimationClip& clip : model_.animations) {
            for (const AnimationChannel& ch : clip.channels) {
                if (ch.node != ps.node || ch.property != "particles.birth_rate" ||
                    ch.values.empty()) {
                    continue;
                }
                const float peak = clip.name.empty()
                                       ? ch.values.front()
                                       : *std::ranges::max_element(ch.values);
                if (clip.name.empty()) {
                    rate = peak;
                    break;
                }
                rate = std::max(rate, peak);
            }
            if (clip.name.empty() && rate >= 0.0f) {
                break;
            }
        }
        for (ParticleEmitter& em : ps.emitters) {
            em.birth_rate = std::max(rate, 0.0f);
        }
    }
}

void ControllerReader::read_sequence(nifly::NiControllerSequence* sequence) {
    AnimationClip clip;
    clip.name = sequence->name.get();
    clip.cycle = cycle_of(sequence->cycleType);
    clip.frequency = sequence->frequency;
    clip.phase = sequence->phase;
    clip.start = sequence->startTime;
    clip.stop = sequence->stopTime;

    if (auto* keys = nif_.GetHeader().GetBlock<nifly::NiTextKeyExtraData>(sequence->textKeyRef)) {
        for (auto& key : keys->textKeys) {
            clip.text_keys.emplace_back(key.time, key.value.get());
        }
    }

    for (auto& link : sequence->controlledBlocks) {
        auto* interpolator = nif_.GetHeader().GetBlock<nifly::NiInterpolator>(link.interpolatorRef);
        auto* controller = nif_.GetHeader().GetBlock<nifly::NiTimeController>(link.controllerRef);
        if (interpolator == nullptr) {
            continue;
        }

        // Shader and alpha controllers target their property; transform
        // controllers are multi-target and are found by node name.
        std::optional<std::size_t> node;
        if (controller != nullptr &&
            dynamic_cast<nifly::NiMultiTargetTransformController*>(controller) == nullptr) {
            if (const auto it = node_by_owner_.find(controller->targetRef.index);
                it != node_by_owner_.end()) {
                node = it->second;
            }
        }
        if (!node.has_value()) {
            if (const auto it = node_by_name_.find(link.nodeName.get()); it != node_by_name_.end()) {
                node = it->second;
            }
        }
        if (!node.has_value()) {
            continue;
        }

        if (controller == nullptr || dynamic_cast<nifly::NiMultiTargetTransformController*>(
                                         controller) != nullptr) {
            // Only transforms are driven without a controller of their own.
            if (controller != nullptr || link.ctrlType.get() == "NiTransformController") {
                transform_channels(interpolator, *node, clip.channels);
            }
            continue;
        }
        channels_for(controller, link.interpID.get(), interpolator, *node, clip.channels);
    }
    model_.animations.push_back(std::move(clip));
}

void ControllerReader::channels_for(nifly::NiTimeController* controller,
                                    std::string_view interp_id,
                                    nifly::NiInterpolator* interpolator, std::size_t node,
                                    std::vector<AnimationChannel>& out) {
    auto& hdr = nif_.GetHeader();

    // Float and colour interpolators feed shader and particle variables.
    auto float_channel = [&](std::string property) {
        AnimationChannel ch;
        ch.node = node;
        ch.property = std::move(property);
        if (auto* f = dynamic_cast<nifly::NiFloatInterpolator*>(interpolator)) {
            auto* data = hdr.GetBlock<nifly::NiFloatData>(f->dataRef);
            if (data != nullptr && read_keys(data->data, 1, ch)) {
                out.push_back(std::move(ch));
            } else if (!is_unset(f->floatValue)) {
                out.push_back(constant(node, ch.property, {f->floatValue}));
            }
        } else if (interpolator != nullptr) {
            note_unsupported(std::format("interpolator {}", interpolator->GetBlockName()));
        }
    };
    auto color_channel = [&](std::string property) {
        AnimationChannel ch;
        ch.node = node;
        ch.property = std::move(property);
        if (auto* p = dynamic_cast<nifly::NiPoint3Interpolator*>(interpolator)) {
            auto* data = hdr.GetBlock<nifly::NiPosData>(p->dataRef);
            if (data != nullptr && read_keys(data->data, 3, ch)) {
                out.push_back(std::move(ch));
            } else if (!is_unset(p->point3Value.x)) {
                out.push_back(constant(node, ch.property,
                                       {p->point3Value.x, p->point3Value.y, p->point3Value.z}));
            }
        } else if (interpolator != nullptr) {
            note_unsupported(std::format("interpolator {}", interpolator->GetBlockName()));
        }
    };
    auto bool_channel = [&](std::string property) {
        AnimationChannel ch;
        ch.node = node;
        ch.property = std::move(property);
        if (auto* b = dynamic_cast<nifly::NiBoolInterpolator*>(interpolator)) {
            auto* data = hdr.GetBlock<nifly::NiBoolData>(b->dataRef);
            if (data != nullptr && read_keys(data->data, 1, ch)) {
                ch.interp = KeyInterp::step;
                ch.in_tangents.clear();
                ch.out_tangents.clear();
                out.push_back(std::move(ch));
            } else if (b->boolValue <= 1) {
                out.push_back(constant(node, ch.property, {b->boolValue != 0 ? 1.0f : 0.0f}));
            }
        } else if (interpolator != nullptr) {
            note_unsupported(std::format("interpolator {}", interpolator->GetBlockName()));
        }
    };

    if (dynamic_cast<nifly::NiKeyframeController*>(controller) != nullptr) {
        transform_channels(interpolator, node, out);
        return;
    }

    if (dynamic_cast<nifly::NiVisController*>(controller) != nullptr) {
        bool_channel("visible");
    } else if (auto* ef = dynamic_cast<nifly::BSEffectShaderPropertyFloatController*>(controller)) {
        const std::string_view name = effect_float_name(ef->typeOfControlledVariable);
        if (name.empty()) {
            note_unsupported(std::format("effect shader variable {}", ef->typeOfControlledVariable));
        } else {
            float_channel(std::format("effect.{}", name));
        }
    } else if (dynamic_cast<nifly::BSEffectShaderPropertyColorController*>(controller) != nullptr) {
        color_channel("effect.emissive_color");
    } else if (auto* lf = dynamic_cast<nifly::BSLightingShaderPropertyFloatController*>(controller)) {
        const std::string_view name = lighting_float_name(lf->typeOfControlledVariable);
        if (name.empty()) {
            note_unsupported(
                std::format("lighting shader variable {}", lf->typeOfControlledVariable));
        } else {
            float_channel(std::format("lighting.{}", name));
        }
    } else if (auto* lc = dynamic_cast<nifly::BSLightingShaderPropertyColorController*>(controller)) {
        color_channel(lc->typeOfControlledColor == 0 ? "lighting.specular_color"
                                                     : "lighting.emissive_color");
    } else if (dynamic_cast<nifly::BSNiAlphaPropertyTestRefController*>(controller) != nullptr) {
        float_channel("alpha_test_ref");
    } else if (auto* emitter = dynamic_cast<nifly::NiPSysEmitterCtlr*>(controller)) {
        // Standalone: the interpolator is the birth rate and a second one
        // switches the emitter. In a sequence each has its own link.
        if (interp_id == "EmitterActive") {
            bool_channel("particles.active");
        } else {
            float_channel("particles.birth_rate");
            if (interp_id.empty()) {
                interpolator = hdr.GetBlock<nifly::NiInterpolator>(emitter->visInterpolatorRef.index);
                if (interpolator != nullptr) {
                    bool_channel("particles.active");
                }
            }
        }
    } else if (dynamic_cast<nifly::NiPSysGravityStrengthCtlr*>(controller) != nullptr) {
        float_channel("particles.gravity_strength");
    } else if (dynamic_cast<nifly::NiPSysEmitterSpeedCtlr*>(controller) != nullptr) {
        float_channel("particles.speed");
    } else if (dynamic_cast<nifly::NiPSysEmitterInitialRadiusCtlr*>(controller) != nullptr) {
        float_channel("particles.radius");
    } else if (dynamic_cast<nifly::NiPSysEmitterLifeSpanCtlr*>(controller) != nullptr) {
        float_channel("particles.life_span");
    } else if (dynamic_cast<nifly::NiPSysUpdateCtlr*>(controller) != nullptr ||
               dynamic_cast<nifly::NiPSysModifierActiveCtlr*>(controller) != nullptr) {
        // Particles update themselves; modifiers stay on.
    } else if (controller != nullptr) {
        note_unsupported(std::format("controller {}", controller->GetBlockName()));
    }
}

void ControllerReader::transform_channels(nifly::NiInterpolator* interpolator, std::size_t node,
                                          std::vector<AnimationChannel>& out) {
    auto& hdr = nif_.GetHeader();
    auto* t = dynamic_cast<nifly::NiTransformInterpolator*>(interpolator);
    if (t == nullptr) {
        if (interpolator != nullptr) {
            note_unsupported(std::format("interpolator {}", interpolator->GetBlockName()));
        }
        return;
    }
    auto* data = hdr.GetBlock<nifly::NiTransformData>(t->dataRef);
    bool has_translation = false;
    bool has_rotation = false;
    bool has_scale = false;
    if (data != nullptr) {
        AnimationChannel translation;
        translation.node = node;
        translation.property = "translation";
        if (read_keys(data->translations, 3, translation)) {
            out.push_back(std::move(translation));
            has_translation = true;
        }
        if (data->rotationType == nifly::XYZ_ROTATION_KEY) {
            const nifly::NiAnimationKeyGroup<float>* axes[3] = {
                &data->xRotations, &data->yRotations, &data->zRotations};
            const char* names[3] = {"rotation_x", "rotation_y", "rotation_z"};
            for (int axis = 0; axis < 3; ++axis) {
                AnimationChannel ch;
                ch.node = node;
                ch.property = names[axis];
                if (read_keys(*axes[axis], 1, ch)) {
                    out.push_back(std::move(ch));
                    has_rotation = true;
                }
            }
        } else if (!data->quaternionKeys.empty()) {
            AnimationChannel ch;
            ch.node = node;
            ch.property = "rotation";
            ch.components = 4;
            // Quaternion keys are slerped whatever their key type says.
            ch.interp = data->rotationType == nifly::NO_INTERP ||
                                data->rotationType == nifly::CONST_KEY
                            ? KeyInterp::step
                            : KeyInterp::linear;
            const std::size_t count = std::min(data->quaternionKeys.size(), k_max_keys);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& key = data->quaternionKeys[i];
                ch.times.push_back(key.time);
                ch.values.insert(ch.values.end(),
                                 {key.value.x, key.value.y, key.value.z, key.value.w});
            }
            out.push_back(std::move(ch));
            has_rotation = true;
        }
        AnimationChannel scale;
        scale.node = node;
        scale.property = "scale";
        if (read_keys(data->scales, 1, scale)) {
            out.push_back(std::move(scale));
            has_scale = true;
        }
    }
    // Without keys the interpolator's own pose applies (a sequence
    // holding a part still).
    if (!has_translation && !is_unset(t->translation.x)) {
        out.push_back(constant(node, "translation",
                               {t->translation.x, t->translation.y, t->translation.z}));
    }
    if (!has_rotation && !is_unset(t->rotation.x) && !is_unset(t->rotation.w)) {
        out.push_back(constant(node, "rotation",
                               {t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w}));
    }
    if (!has_scale && !is_unset(t->scale)) {
        out.push_back(constant(node, "scale", {t->scale}));
    }
}

void ControllerReader::read_particles(const PendingParticles& pending) {
    auto& hdr = nif_.GetHeader();
    nifly::NiParticleSystem* system = pending.system;

    ParticleSystem ps;
    ps.node = pending.node;
    ps.material = pending.material;
    ps.world_space = system->isWorldSpace;
    ps.strip = dynamic_cast<nifly::BSStripParticleSystem*>(system) != nullptr;
    mark(ps.node);

    if (auto* data = hdr.GetBlock<nifly::NiParticlesData>(system->dataRef)) {
        ps.max_particles = data->GetNumVertices();
        for (auto& rect : data->subtexOffsets) {
            ps.subtex_offsets.push_back(Vec4{rect.x, rect.y, rect.z, rect.w});
        }
    }

    auto node_ref = [&](std::uint32_t block) -> std::optional<std::size_t> {
        auto node = node_of(block);
        if (node.has_value()) {
            mark(*node);
        }
        return node;
    };

    for (auto& ref : system->modifierRefs) {
        auto* modifier = hdr.GetBlock<nifly::NiPSysModifier>(ref);
        if (modifier == nullptr) {
            continue;
        }
        if (auto* e = dynamic_cast<nifly::NiPSysEmitter*>(modifier)) {
            ParticleEmitter em;
            em.speed = e->speed;
            em.speed_variation = e->speedVariation;
            em.declination = e->declination;
            em.declination_variation = e->declinationVariation;
            em.planar_angle = e->planarAngle;
            em.planar_angle_variation = e->planarAngleVariation;
            em.color = vec4(e->color);
            em.radius = e->radius;
            em.radius_variation = e->radiusVariation;
            em.life_span = e->lifeSpan;
            em.life_span_variation = e->lifeSpanVariation;
            if (auto* v = dynamic_cast<nifly::NiPSysVolumeEmitter*>(e)) {
                em.node = node_ref(v->emitterNodeRef.index);
            }
            if (auto* box = dynamic_cast<nifly::NiPSysBoxEmitter*>(e)) {
                em.kind = EmitterKind::box;
                em.size = Vec3{box->width, box->height, box->depth};
            } else if (auto* sphere = dynamic_cast<nifly::NiPSysSphereEmitter*>(e)) {
                em.kind = EmitterKind::sphere;
                em.size = Vec3{sphere->radius, 0, 0};
            } else if (auto* cyl = dynamic_cast<nifly::NiPSysCylinderEmitter*>(e)) {
                em.kind = EmitterKind::cylinder;
                em.size = Vec3{cyl->radius, cyl->height, 0};
            } else if (auto* mesh = dynamic_cast<nifly::NiPSysMeshEmitter*>(e)) {
                em.kind = EmitterKind::mesh;
                for (auto& m : mesh->meshRefs) {
                    if (auto node = node_ref(m.index)) {
                        em.meshes.push_back(*node);
                    }
                }
                em.mesh_velocity = mesh->velocityType;
                em.mesh_axis = vec3(mesh->emissionAxis);
            } else {
                note_unsupported(std::format("emitter {}", modifier->GetBlockName()));
                continue;
            }
            ps.emitters.push_back(std::move(em));
        } else if (auto* g = dynamic_cast<nifly::NiPSysGravityModifier*>(modifier)) {
            ParticleGravity gravity;
            gravity.node = node_ref(g->gravityObjRef.index);
            gravity.axis = vec3(g->gravityAxis);
            gravity.strength = g->strength;
            gravity.decay = g->decay;
            gravity.spherical = g->forceType == nifly::FORCE_SPHERICAL;
            gravity.turbulence = g->turbulence;
            gravity.turbulence_scale = g->turbulenceScale;
            gravity.world_aligned = g->worldAligned;
            if (!finite_vec(gravity.axis)) {
                gravity.axis = Vec3{0, 0, 1};
            }
            ps.gravity.push_back(gravity);
        } else if (auto* r = dynamic_cast<nifly::NiPSysRotationModifier*>(modifier)) {
            ps.has_rotation = true;
            ps.rotation_speed = r->initialSpeed;
            ps.rotation_speed_variation = r->initialSpeedVariation;
            ps.rotation_angle = r->initialAngle;
            ps.rotation_angle_variation = r->initialAngleVariation;
            ps.rotation_random_sign = r->randomSpeedSign;
        } else if (auto* s = dynamic_cast<nifly::BSPSysScaleModifier*>(modifier)) {
            ps.scales.assign(s->floats.begin(), s->floats.end());
        } else if (auto* gf = dynamic_cast<nifly::NiPSysGrowFadeModifier*>(modifier)) {
            ps.grow_time = gf->growTime;
            ps.fade_time = gf->fadeTime;
            ps.base_scale = gf->baseScale > 0.0f ? gf->baseScale : 1.0f;
        } else if (auto* c = dynamic_cast<nifly::BSPSysSimpleColorModifier*>(modifier)) {
            ps.has_simple_color = true;
            ps.fade_in = c->fadeInPercent;
            ps.fade_out = c->fadeOutPercent;
            ps.color1_end = c->color1EndPercent;
            ps.color2_start = c->color2StartPercent;
            ps.color2_end = c->color2EndPercent;
            ps.color3_start = c->color3StartPercent;
            ps.colors = {vec4(c->color1), vec4(c->color2), vec4(c->color3)};
        } else if (auto* cm = dynamic_cast<nifly::NiPSysColorModifier*>(modifier)) {
            if (auto* data = hdr.GetBlock<nifly::NiColorData>(cm->dataRef)) {
                const std::uint32_t count =
                    std::min<std::uint32_t>(data->data.GetNumKeys(), k_max_keys);
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto key = data->data.GetKey(static_cast<int>(i));
                    ps.color_keys.emplace_back(key.time, vec4(key.value));
                }
            }
        } else if (auto* d = dynamic_cast<nifly::NiPSysDragModifier*>(modifier)) {
            ps.drag = d->percentage;
            ps.drag_range = d->range;
            ParticleDrag drag;
            drag.node = node_ref(d->parentRef.index);
            drag.axis = finite_vec(vec3(d->dragAxis)) ? vec3(d->dragAxis) : Vec3{};
            drag.percentage = d->percentage;
            drag.range = d->range;
            drag.range_falloff = d->rangeFalloff;
            ps.drags.push_back(drag);
        } else if (auto* st = dynamic_cast<nifly::BSPSysSubTexModifier*>(modifier)) {
            ps.has_subtex = true;
            ps.subtex_start = st->startFrame;
            ps.subtex_start_variation = st->startFrameVariation;
            ps.subtex_end = st->endFrame;
            ps.subtex_loop_start = st->loopStartFrame;
            ps.subtex_loop_start_variation = st->loopStartFrameVariation;
            ps.subtex_frame_count = st->frameCount;
            ps.subtex_frame_count_variation = st->frameCountVariation;
        } else if (dynamic_cast<nifly::NiPSysPositionModifier*>(modifier) != nullptr ||
                   dynamic_cast<nifly::NiPSysBoundUpdateModifier*>(modifier) != nullptr ||
                   dynamic_cast<nifly::NiPSysAgeDeathModifier*>(modifier) != nullptr ||
                   dynamic_cast<nifly::NiPSysSpawnModifier*>(modifier) != nullptr ||
                   dynamic_cast<nifly::BSPSysLODModifier*>(modifier) != nullptr ||
                   dynamic_cast<nifly::BSPSysStripUpdateModifier*>(modifier) != nullptr) {
            // Implied by any particle simulation, only an optimization, or
            // spawning on death, which vanilla uses for nothing visible.
        } else {
            const std::string name = modifier->GetBlockName();
            if (std::ranges::find(ps.unsupported, name) == ps.unsupported.end()) {
                ps.unsupported.push_back(name);
            }
            note_unsupported(std::format("particle modifier {}", name));
        }
    }
    model_.particles.push_back(std::move(ps));
}

} // namespace bethconv::mesh::detail
