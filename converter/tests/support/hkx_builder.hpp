// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic Havok packfiles (hk_2010.2.0-r1) for the animation tests, the
// fuzz seeds and the test pack. Writes the subset bethconv reads: skeletons,
// spline-compressed animations and their bindings, with LE (4-byte) or SE
// (8-byte) pointers. Layout: converter/docs/spikes/hkx.md.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bethconv::test {

struct HkxBone {
    std::string name;
    std::int16_t parent{-1};
    std::array<float, 3> translation{0, 0, 0};
    std::array<float, 4> rotation{0, 0, 0, 1};
};

/// One property of a track in a block, as the builder encodes it.
struct HkxChannel {
    enum class Kind : std::uint8_t { identity, constant, spline };
    Kind kind = Kind::identity;
    std::uint8_t degree = 1;
    std::vector<std::uint8_t> knots;
    /// constant: the value. spline: control points, width floats each.
    std::vector<std::array<float, 4>> points;
    bool eight_bit = false; ///< Positions only: 8-bit control points.

    static HkxChannel constant(std::array<float, 4> v) {
        HkxChannel c;
        c.kind = Kind::constant;
        c.points = {v};
        return c;
    }
    /// A degree-1 spline through `points`, one per frame from 0.
    static HkxChannel linear(std::vector<std::array<float, 4>> pts) {
        HkxChannel c;
        c.kind = Kind::spline;
        c.degree = 1;
        c.knots.push_back(0);
        for (std::size_t i = 0; i < pts.size(); ++i) {
            c.knots.push_back(static_cast<std::uint8_t>(i));
        }
        c.knots.push_back(static_cast<std::uint8_t>(pts.size() - 1));
        c.points = std::move(pts);
        return c;
    }
};

struct HkxTrack {
    HkxChannel translation;
    HkxChannel rotation;
};

struct HkxBlock {
    std::vector<HkxTrack> tracks;
    std::vector<HkxChannel> floats; ///< constant or spline, width 1.
};

struct HkxClip {
    std::uint32_t frames{};
    std::uint32_t frames_per_block{256};
    float frame_duration = 1.0f / 30.0f;
    std::uint32_t tracks{};
    std::uint32_t float_tracks{};
    std::vector<HkxBlock> blocks;
    std::vector<std::pair<float, std::string>> annotations;
    std::string skeleton_name;
    std::vector<std::int16_t> track_to_bone;
    /// Write transformOffsets (Havok does for multi-block clips).
    bool track_offsets = false;
    /// Corrupt the second track's offset, for the consistency check.
    bool wrong_track_offset = false;
    /// Rotation quantization written in the masks (1 = THREECOMP40).
    std::uint8_t rotation_quantization = 1;
};

/// THREECOMP40: three 12-bit components, the dropped largest one's index and
/// sign. The inverse of the reader's decode.
inline std::array<std::uint8_t, 5> encode_quat40(std::array<float, 4> q) {
    float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& v : q) {
        v /= len;
    }
    std::size_t largest = 0;
    for (std::size_t i = 1; i < 4; ++i) {
        if (std::fabs(q[i]) > std::fabs(q[largest])) {
            largest = i;
        }
    }
    const std::uint64_t mask = (1u << 12) - 1;
    const float half = static_cast<float>(mask >> 1);
    const float fraction = 1.0f / (half * std::sqrt(2.0f));
    std::uint64_t c = 0;
    std::size_t slot = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        if (i == largest) {
            continue;
        }
        const auto v = static_cast<std::int64_t>(std::lround(q[i] / fraction + half));
        c |= static_cast<std::uint64_t>(std::clamp<std::int64_t>(v, 0, static_cast<std::int64_t>(mask))) << (12 * slot);
        ++slot;
    }
    c |= static_cast<std::uint64_t>(largest) << 36;
    c |= static_cast<std::uint64_t>(q[largest] < 0 ? 1 : 0) << 38;
    std::array<std::uint8_t, 5> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        out[i] = static_cast<std::uint8_t>((c >> (8 * i)) & 0xFF);
    }
    return out;
}

class HkxBuilder {
public:
    explicit HkxBuilder(std::uint8_t pointer_size) : P_(pointer_size) {}

    void add_skeleton(const std::string& name, const std::vector<HkxBone>& bones,
                      const std::vector<std::string>& float_slots = {}) {
        const std::size_t A = P_ + 8;
        const std::size_t o = object("hkaSkeleton", ref() + P_ + 7 * A);
        string_ptr(o + ref(), name);
        const std::size_t arrays = o + ref() + P_;
        // parents
        const std::size_t parents = blob(bones.size() * 2, 16);
        for (std::size_t i = 0; i < bones.size(); ++i) {
            put<std::int16_t>(parents + 2 * i, bones[i].parent);
        }
        array(arrays, parents, bones.size());
        // bones: hkStringPtr + hkBool, padded to 2P
        const std::size_t bone_data = blob(bones.size() * 2 * P_, 16);
        for (std::size_t i = 0; i < bones.size(); ++i) {
            string_ptr(bone_data + i * 2 * P_, bones[i].name);
        }
        array(arrays + A, bone_data, bones.size());
        // reference pose
        const std::size_t pose = blob(bones.size() * 48, 16);
        for (std::size_t i = 0; i < bones.size(); ++i) {
            for (std::size_t k = 0; k < 3; ++k) {
                put<float>(pose + i * 48 + 4 * k, bones[i].translation[k]);
                put<float>(pose + i * 48 + 32 + 4 * k, 1.0f);
            }
            for (std::size_t k = 0; k < 4; ++k) {
                put<float>(pose + i * 48 + 16 + 4 * k, bones[i].rotation[k]);
            }
        }
        array(arrays + 2 * A, pose, bones.size());
        if (!float_slots.empty()) {
            const std::size_t slots = blob(float_slots.size() * P_, 16);
            for (std::size_t i = 0; i < float_slots.size(); ++i) {
                string_ptr(slots + i * P_, float_slots[i]);
            }
            array(arrays + 4 * A, slots, float_slots.size());
        }
    }

    void add_clip(const HkxClip& clip) {
        const std::size_t A = P_ + 8;
        const std::size_t base_off = ref() + 16 + P_ + A;
        const std::size_t arrays_rel = align(base_off + 28, P_);
        const std::size_t size = arrays_rel + 5 * A + 4;
        const std::size_t o = object("hkaSplineCompressedAnimation", size);
        put<std::int32_t>(o + ref(), 5); // spline
        put<float>(o + ref() + 4, clip.frame_duration * static_cast<float>(clip.frames - 1));
        put<std::int32_t>(o + ref() + 8, static_cast<std::int32_t>(clip.tracks));
        put<std::int32_t>(o + ref() + 12, static_cast<std::int32_t>(clip.float_tracks));
        if (!clip.annotations.empty()) {
            const std::size_t track = blob(P_ + A, 16);
            const std::size_t notes = blob(clip.annotations.size() * 2 * P_, 16);
            for (std::size_t i = 0; i < clip.annotations.size(); ++i) {
                put<float>(notes + i * 2 * P_, clip.annotations[i].first);
                string_ptr(notes + i * 2 * P_ + P_, clip.annotations[i].second);
            }
            array(track + P_, notes, clip.annotations.size());
            array(o + ref() + 16 + P_, track, 1);
        }
        const std::size_t base = o + base_off;
        put<std::int32_t>(base, static_cast<std::int32_t>(clip.frames));
        put<std::int32_t>(base + 4, static_cast<std::int32_t>(clip.blocks.size()));
        put<std::int32_t>(base + 8, static_cast<std::int32_t>(clip.frames_per_block));
        const std::size_t mask_size = align(4 * clip.tracks + clip.float_tracks, 4);
        put<std::int32_t>(base + 12, static_cast<std::int32_t>(mask_size));
        const float block_duration = clip.frame_duration * static_cast<float>(clip.frames_per_block - 1);
        put<float>(base + 16, block_duration);
        put<float>(base + 20, 1.0f / block_duration);
        put<float>(base + 24, clip.frame_duration);

        // Encode every block into one byte array.
        std::vector<std::uint8_t> data;
        std::vector<std::int32_t> block_offsets, float_offsets, track_offsets;
        for (const HkxBlock& block : clip.blocks) {
            const std::size_t start = data.size();
            block_offsets.push_back(static_cast<std::int32_t>(start));
            for (const HkxTrack& t : block.tracks) {
                data.push_back(static_cast<std::uint8_t>((t.translation.eight_bit ? 0 : 1) |
                                                         (clip.rotation_quantization << 2)));
                data.push_back(vector_flags(t.translation));
                data.push_back(rotation_flags(t.rotation));
                data.push_back(0); // scale: identity
            }
            for (const HkxChannel& f : block.floats) {
                data.push_back(f.kind == HkxChannel::Kind::spline ? 0x12 : 0x03);
            }
            data.resize(start + mask_size, 0);
            for (std::size_t i = 0; i < block.tracks.size(); ++i) {
                const std::int32_t at = static_cast<std::int32_t>(data.size() - start);
                track_offsets.push_back(clip.wrong_track_offset && i == 1 ? at + 4 : at);
                encode_vector(data, block.tracks[i].translation);
                encode_rotation(data, block.tracks[i].rotation);
            }
            pad(data, 16);
            float_offsets.push_back(static_cast<std::int32_t>(data.size() - start));
            for (const HkxChannel& f : block.floats) {
                encode_float(data, f);
            }
            pad(data, 16);
        }
        const std::size_t arrays = o + arrays_rel;
        int_array(arrays, block_offsets);
        int_array(arrays + A, clip.float_tracks != 0 ? float_offsets : std::vector<std::int32_t>{});
        int_array(arrays + 2 * A, clip.track_offsets ? track_offsets : std::vector<std::int32_t>{});
        const std::size_t bytes = blob(data.size(), 16);
        std::memcpy(data_.data() + bytes, data.data(), data.size());
        array(arrays + 4 * A, bytes, data.size());

        // The binding.
        const std::size_t b = object("hkaAnimationBinding", ref() + 2 * P_ + 2 * A + 4);
        string_ptr(b + ref(), clip.skeleton_name);
        pointer(b + ref() + P_, o);
        if (!clip.track_to_bone.empty()) {
            const std::size_t t2b = blob(clip.track_to_bone.size() * 2, 16);
            for (std::size_t i = 0; i < clip.track_to_bone.size(); ++i) {
                put<std::int16_t>(t2b + 2 * i, clip.track_to_bone[i]);
            }
            array(b + ref() + 2 * P_, t2b, clip.track_to_bone.size());
        }
    }

    /// An hkaInterleavedUncompressedAnimation: `poses[frame][track]` as
    /// (translation, rotation), unit scale.
    void add_interleaved(const std::vector<std::vector<std::pair<std::array<float, 3>, std::array<float, 4>>>>& poses,
                         float duration) {
        const std::size_t A = P_ + 8;
        const std::size_t base_off = ref() + 16 + P_ + A;
        const std::size_t o = object("hkaInterleavedUncompressedAnimation", base_off + 2 * A);
        const std::size_t tracks = poses.empty() ? 0 : poses[0].size();
        put<std::int32_t>(o + ref(), 1); // interleaved
        put<float>(o + ref() + 4, duration);
        put<std::int32_t>(o + ref() + 8, static_cast<std::int32_t>(tracks));
        const std::size_t count = poses.size() * tracks;
        const std::size_t d = blob(count * 48, 16);
        for (std::size_t f = 0; f < poses.size(); ++f) {
            for (std::size_t t = 0; t < tracks; ++t) {
                const std::size_t at = d + (f * tracks + t) * 48;
                for (std::size_t k = 0; k < 3; ++k) {
                    put<float>(at + 4 * k, poses[f][t].first[k]);
                    put<float>(at + 32 + 4 * k, 1.0f);
                }
                for (std::size_t k = 0; k < 4; ++k) {
                    put<float>(at + 16 + 4 * k, poses[f][t].second[k]);
                }
            }
        }
        array(o + base_off, d, count);
    }

    /// Serialize: header, __classnames__, __types__ (empty), __data__.
    [[nodiscard]] std::vector<std::byte> bytes() const {
        std::vector<std::uint8_t> names;
        std::vector<std::pair<std::size_t, std::size_t>> virtuals; // object, name offset
        for (const auto& [object, cls] : objects_) {
            virtuals.emplace_back(object, class_name(names, cls));
        }
        std::vector<std::uint8_t> out(0x40 + 3 * 48, 0);
        put_le<std::uint32_t>(out, 0, 0x57E0E057);
        put_le<std::uint32_t>(out, 4, 0x10C0C010);
        put_le<std::int32_t>(out, 12, 8);
        out[16] = P_;
        out[17] = 1;
        out[18] = 0;
        out[19] = 1;
        put_le<std::int32_t>(out, 20, 3);
        put_le<std::int32_t>(out, 24, 2); // contents section
        const char* version = "hk_2010.2.0-r1";
        std::memcpy(out.data() + 40, version, std::strlen(version));
        out[55] = 0xFF;
        // Sections.
        const auto section = [&](std::size_t index, const char* name, std::size_t start,
                                 std::array<std::size_t, 6> rel) {
            const std::size_t h = 0x40 + index * 48;
            std::memcpy(out.data() + h, name, std::strlen(name));
            out[h + 19] = 0xFF;
            put_le<std::int32_t>(out, h + 20, static_cast<std::int32_t>(start));
            for (std::size_t i = 0; i < 6; ++i) {
                put_le<std::int32_t>(out, h + 24 + 4 * i, static_cast<std::int32_t>(rel[i]));
            }
        };
        // __classnames__
        const std::size_t names_start = out.size();
        out.insert(out.end(), names.begin(), names.end());
        pad(out, 16);
        const std::size_t names_len = out.size() - names_start;
        section(0, "__classnames__", names_start, {names_len, names_len, names_len, names_len, names_len, names_len});
        const std::size_t types_start = out.size();
        section(1, "__types__", types_start, {0, 0, 0, 0, 0, 0});
        // __data__ with its fixup tables.
        const std::size_t data_start = out.size();
        out.insert(out.end(), data_.begin(), data_.end());
        pad(out, 16);
        const std::size_t local = out.size() - data_start;
        for (const auto& [src, dst] : locals_) {
            append_le<std::int32_t>(out, static_cast<std::int32_t>(src));
            append_le<std::int32_t>(out, static_cast<std::int32_t>(dst));
        }
        pad_ff(out, 16);
        const std::size_t global = out.size() - data_start;
        const std::size_t virt = global;
        for (const auto& [object, name] : virtuals) {
            append_le<std::int32_t>(out, static_cast<std::int32_t>(object));
            append_le<std::int32_t>(out, 0);
            append_le<std::int32_t>(out, static_cast<std::int32_t>(name));
        }
        pad_ff(out, 16);
        const std::size_t end = out.size() - data_start;
        section(2, "__data__", data_start, {local, global, virt, end, end, end});
        std::vector<std::byte> result(out.size());
        std::memcpy(result.data(), out.data(), out.size());
        return result;
    }

private:
    [[nodiscard]] std::size_t ref() const { return P_ == 8 ? 16 : 8; }
    static std::size_t align(std::size_t v, std::size_t to) { return (v + to - 1) / to * to; }

    template <typename T>
    void put(std::size_t at, T v) {
        std::memcpy(data_.data() + at, &v, sizeof(T));
    }
    template <typename T>
    static void put_le(std::vector<std::uint8_t>& out, std::size_t at, T v) {
        std::memcpy(out.data() + at, &v, sizeof(T));
    }
    template <typename T>
    static void append_le(std::vector<std::uint8_t>& out, T v) {
        const std::size_t at = out.size();
        out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &v, sizeof(T));
    }
    static void pad(std::vector<std::uint8_t>& out, std::size_t to) { out.resize(align(out.size(), to), 0); }
    static void pad_ff(std::vector<std::uint8_t>& out, std::size_t to) { out.resize(align(out.size(), to), 0xFF); }

    std::size_t blob(std::size_t size, std::size_t alignment) {
        const std::size_t at = align(data_.size(), alignment);
        data_.resize(at + std::max<std::size_t>(size, 1), 0);
        return at;
    }
    std::size_t object(const std::string& cls, std::size_t size) {
        const std::size_t at = blob(size, 16);
        objects_.emplace_back(at, cls);
        return at;
    }
    void pointer(std::size_t at, std::size_t target) { locals_.emplace_back(at, target); }
    void string_ptr(std::size_t at, const std::string& text) {
        const std::size_t s = blob(text.size() + 1, 2);
        std::memcpy(data_.data() + s, text.data(), text.size());
        pointer(at, s);
    }
    void array(std::size_t at, std::size_t target, std::size_t count) {
        pointer(at, target);
        put<std::int32_t>(at + P_, static_cast<std::int32_t>(count));
        put<std::uint32_t>(at + P_ + 4, static_cast<std::uint32_t>(count) | 0x80000000u);
    }
    void int_array(std::size_t at, const std::vector<std::int32_t>& values) {
        if (values.empty()) {
            return;
        }
        const std::size_t d = blob(values.size() * 4, 16);
        for (std::size_t i = 0; i < values.size(); ++i) {
            put<std::int32_t>(d + 4 * i, values[i]);
        }
        array(at, d, values.size());
    }

    /// Offset of `cls` in the class name table, appending it if new. Each
    /// entry is a 4-byte signature (not checked by the reader), 0x09, the
    /// name and a NUL.
    static std::size_t class_name(std::vector<std::uint8_t>& names, const std::string& cls) {
        std::size_t p = 0;
        while (p + 5 <= names.size()) {
            const std::size_t start = p + 5;
            std::size_t e = start;
            while (e < names.size() && names[e] != 0) {
                ++e;
            }
            if (std::string(names.begin() + static_cast<std::ptrdiff_t>(start),
                            names.begin() + static_cast<std::ptrdiff_t>(e)) == cls) {
                return start;
            }
            p = e + 1;
        }
        names.insert(names.end(), {0, 0, 0, 0, 0x09});
        const std::size_t at = names.size();
        names.insert(names.end(), cls.begin(), cls.end());
        names.push_back(0);
        return at;
    }

    static std::uint8_t vector_flags(const HkxChannel& c) {
        switch (c.kind) {
        case HkxChannel::Kind::identity: return 0;
        case HkxChannel::Kind::constant: return 0x07;
        case HkxChannel::Kind::spline: return 0x70;
        }
        return 0;
    }
    static std::uint8_t rotation_flags(const HkxChannel& c) {
        switch (c.kind) {
        case HkxChannel::Kind::identity: return 0;
        case HkxChannel::Kind::constant: return 0x0F;
        case HkxChannel::Kind::spline: return 0xF0;
        }
        return 0;
    }

    static void knots(std::vector<std::uint8_t>& d, const HkxChannel& c) {
        const auto n = static_cast<std::uint16_t>(c.points.size() - 1);
        d.push_back(static_cast<std::uint8_t>(n & 0xFF));
        d.push_back(static_cast<std::uint8_t>(n >> 8));
        d.push_back(c.degree);
        d.insert(d.end(), c.knots.begin(), c.knots.end());
    }
    template <typename T>
    static void raw(std::vector<std::uint8_t>& d, T v) {
        append_le<T>(d, v);
    }

    static void encode_vector(std::vector<std::uint8_t>& d, const HkxChannel& c) {
        if (c.kind == HkxChannel::Kind::constant) {
            for (std::size_t a = 0; a < 3; ++a) {
                raw<float>(d, c.points[0][a]);
            }
            return;
        }
        if (c.kind != HkxChannel::Kind::spline) {
            return;
        }
        knots(d, c);
        pad(d, 4);
        std::array<float, 3> lo{}, hi{};
        for (std::size_t a = 0; a < 3; ++a) {
            lo[a] = hi[a] = c.points[0][a];
            for (const auto& p : c.points) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
            raw<float>(d, lo[a]);
            raw<float>(d, hi[a]);
        }
        for (const auto& p : c.points) {
            for (std::size_t a = 0; a < 3; ++a) {
                const float range = hi[a] - lo[a];
                const float q = range > 0 ? (p[a] - lo[a]) / range : 0.0f;
                if (c.eight_bit) {
                    d.push_back(static_cast<std::uint8_t>(std::lround(q * 255.0f)));
                } else {
                    raw<std::uint16_t>(d, static_cast<std::uint16_t>(std::lround(q * 65535.0f)));
                }
            }
        }
        pad(d, 4);
    }

    static void encode_rotation(std::vector<std::uint8_t>& d, const HkxChannel& c) {
        if (c.kind == HkxChannel::Kind::identity) {
            return;
        }
        if (c.kind == HkxChannel::Kind::spline) {
            knots(d, c);
        }
        for (const auto& p : c.points) {
            const auto q = encode_quat40(p);
            d.insert(d.end(), q.begin(), q.end());
        }
        pad(d, 4);
    }

    static void encode_float(std::vector<std::uint8_t>& d, const HkxChannel& c) {
        if (c.kind == HkxChannel::Kind::constant) {
            raw<float>(d, c.points[0][0]);
            return;
        }
        knots(d, c);
        pad(d, 4);
        float lo = c.points[0][0];
        float hi = lo;
        for (const auto& p : c.points) {
            lo = std::min(lo, p[0]);
            hi = std::max(hi, p[0]);
        }
        raw<float>(d, lo);
        raw<float>(d, hi);
        for (const auto& p : c.points) {
            const float q = hi > lo ? (p[0] - lo) / (hi - lo) : 0.0f;
            raw<std::uint16_t>(d, static_cast<std::uint16_t>(std::lround(q * 65535.0f)));
        }
        pad(d, 4);
    }

    std::uint8_t P_;
    std::vector<std::uint8_t> data_;
    std::vector<std::pair<std::size_t, std::string>> objects_;
    std::vector<std::pair<std::size_t, std::size_t>> locals_;
};

} // namespace bethconv::test
