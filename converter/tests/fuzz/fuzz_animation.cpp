// SPDX-License-Identifier: GPL-3.0-or-later
//
// Havok packfiles (`.hkx`). Whatever decodes is sampled at every track's
// first, middle and last frame, written as an animation asset and must read
// back the same. The same bytes also go through the three animationdata text
// readers, whose assets must read back too.
#include "bethconv/animation/animation_data.hpp"
#include "bethconv/animation/hkx.hpp"
#include "bethconv/pack/animation_asset.hpp"

#include "fuzz_support.hpp"

#include <cmath>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > bethconv::fuzz::k_max_input) {
        return 0;
    }
    const auto bytes = bethconv::fuzz::as_bytes(data, size);
    for (const char* vpath : {"meshes/animationdata/fuzz.txt", "meshes/animationdata/boundanims/anims_fuzz.txt",
                              "meshes/animationdatasinglefile.txt"}) {
        if (auto text = bethconv::animation::read_animation_data(bytes, vpath)) {
            const auto back =
                bethconv::pack::read_project_asset(bethconv::pack::write_animation_asset(*text), "fuzz.animfb");
            BETHCONV_FUZZ_CHECK(back.has_value());
            BETHCONV_FUZZ_CHECK(back->clips.size() == text->clips.size());
            BETHCONV_FUZZ_CHECK(back->motions.size() == text->motions.size());
            BETHCONV_FUZZ_CHECK(back->projects.size() == text->projects.size());
        }
    }
    auto file = bethconv::animation::read_hkx(bytes, "fuzz.hkx");
    if (!file) {
        return 0;
    }
    for (const auto& skeleton : file->skeletons) {
        BETHCONV_FUZZ_CHECK(skeleton.parents.size() == skeleton.bones.size());
        for (std::size_t i = 0; i < skeleton.parents.size(); ++i) {
            BETHCONV_FUZZ_CHECK(skeleton.parents[i] >= -1 && skeleton.parents[i] < static_cast<std::int16_t>(i));
        }
    }
    for (const auto& clip : file->clips) {
        BETHCONV_FUZZ_CHECK(clip.frame_count <= bethconv::animation::k_max_frames);
        BETHCONV_FUZZ_CHECK(!clip.blocks.empty());
        for (const auto& block : clip.blocks) {
            BETHCONV_FUZZ_CHECK(block.tracks.size() == clip.transform_tracks);
        }
        for (std::uint32_t t = 0; t < clip.transform_tracks; ++t) {
            for (const std::uint32_t f : {0u, clip.frame_count / 2, clip.frame_count - 1}) {
                const auto pose = bethconv::animation::sample(clip, t, f);
                // A finite rotation is unit length; NaN inputs may stay NaN.
                float len = 0.0f;
                for (const float v : pose.rotation) {
                    len += v * v;
                }
                BETHCONV_FUZZ_CHECK(!std::isfinite(len) || std::fabs(len - 1.0f) < 1e-3f);
            }
        }
    }
    const auto asset = bethconv::pack::write_animation_asset(*file);
    const auto back = bethconv::pack::read_animation_asset(asset, "fuzz.animfb");
    BETHCONV_FUZZ_CHECK(back.has_value());
    BETHCONV_FUZZ_CHECK(back->clips.size() == file->clips.size());
    BETHCONV_FUZZ_CHECK(back->skeletons.size() == file->skeletons.size());
    BETHCONV_FUZZ_CHECK(back->clip_generators.size() == file->clip_generators.size());
    return 0;
}
