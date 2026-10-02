// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/animation_asset.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/animation_generated.h"

#include <utility>

namespace bethconv::pack {

namespace {

namespace afb = bethconv::pack::afb;
using animation::Channel;

flatbuffers::Offset<afb::Channel> write_channel(flatbuffers::FlatBufferBuilder& b, const Channel& c) {
    const auto knots = c.knots.empty() ? 0 : b.CreateVector(c.knots).o;
    const auto lo = c.lo.empty() ? 0 : b.CreateVector(c.lo).o;
    const auto hi = c.hi.empty() ? 0 : b.CreateVector(c.hi).o;
    const auto points = c.points.empty() ? 0 : b.CreateVector(c.points).o;
    afb::ChannelBuilder cb(b);
    cb.add_degree(c.degree);
    if (knots != 0) {
        cb.add_knots(knots);
    }
    if (lo != 0) {
        cb.add_lo(lo);
    }
    if (hi != 0) {
        cb.add_hi(hi);
    }
    if (points != 0) {
        cb.add_points(points);
    }
    return cb.Finish();
}

template <typename T>
std::vector<T> copy(const flatbuffers::Vector<T>* v) {
    return v == nullptr ? std::vector<T>{} : std::vector<T>(v->begin(), v->end());
}

std::string text(const flatbuffers::String* s) {
    return s == nullptr ? std::string() : s->str();
}

Channel read_channel(const afb::Channel* c) {
    Channel out;
    if (c == nullptr) {
        return out;
    }
    out.degree = c->degree();
    out.knots = copy(c->knots());
    out.lo = copy(c->lo());
    out.hi = copy(c->hi());
    out.points = copy(c->points());
    return out;
}

} // namespace

std::vector<std::byte> write_animation_asset(const animation::HkxFile& file) {
    flatbuffers::FlatBufferBuilder b(4096);

    std::vector<flatbuffers::Offset<afb::Skeleton>> skeletons;
    for (const auto& s : file.skeletons) {
        std::vector<afb::QsTransform> pose;
        pose.reserve(s.reference_pose.size());
        for (const auto& p : s.reference_pose) {
            pose.emplace_back(p.translation[0], p.translation[1], p.translation[2], p.rotation[0],
                              p.rotation[1], p.rotation[2], p.rotation[3], p.scale[0], p.scale[1],
                              p.scale[2]);
        }
        const auto name = b.CreateString(s.name);
        const auto parents = b.CreateVector(s.parents);
        const auto bones = b.CreateVectorOfStrings(s.bones);
        const auto pose_off = b.CreateVectorOfStructs(pose);
        const auto slots = b.CreateVectorOfStrings(s.float_slots);
        skeletons.push_back(afb::CreateSkeleton(b, name, parents, bones, pose_off, slots));
    }

    std::vector<flatbuffers::Offset<afb::Clip>> clips;
    for (const auto& c : file.clips) {
        std::vector<flatbuffers::Offset<afb::Block>> blocks;
        for (const auto& block : c.blocks) {
            std::vector<flatbuffers::Offset<afb::Track>> tracks;
            tracks.reserve(block.tracks.size());
            for (const auto& t : block.tracks) {
                const auto tr = write_channel(b, t.translation);
                const auto rot = write_channel(b, t.rotation);
                const auto sc = write_channel(b, t.scale);
                tracks.push_back(afb::CreateTrack(b, tr, rot, sc));
            }
            std::vector<flatbuffers::Offset<afb::Channel>> floats;
            for (const auto& f : block.floats) {
                floats.push_back(write_channel(b, f));
            }
            const auto tracks_off = b.CreateVector(tracks);
            const auto floats_off = b.CreateVector(floats);
            blocks.push_back(afb::CreateBlock(b, tracks_off, floats_off));
        }
        std::vector<flatbuffers::Offset<afb::AnnotationTrack>> notes;
        for (const auto& track : c.annotations) {
            std::vector<flatbuffers::Offset<afb::Annotation>> entries;
            for (const auto& a : track.annotations) {
                entries.push_back(afb::CreateAnnotation(b, a.time, b.CreateString(a.text)));
            }
            const auto name = b.CreateString(track.name);
            const auto entries_off = b.CreateVector(entries);
            notes.push_back(afb::CreateAnnotationTrack(b, name, entries_off));
        }
        const auto blocks_off = b.CreateVector(blocks);
        const auto notes_off = b.CreateVector(notes);
        const auto skeleton = b.CreateString(c.skeleton_name);
        const auto t2b = b.CreateVector(c.track_to_bone);
        const auto f2s = b.CreateVector(c.float_to_slot);
        const auto motion = b.CreateString(c.extracted_motion);
        clips.push_back(afb::CreateClip(
            b,
            c.encoding == animation::ClipEncoding::spline ? afb::ClipEncoding::Spline
                                                          : afb::ClipEncoding::Interleaved,
            c.duration, c.frame_count, c.frame_duration, c.frames_per_block, c.transform_tracks,
            c.float_tracks, blocks_off, notes_off, skeleton, t2b, f2s, c.blend_hint, motion));
    }

    const auto skeletons_off = b.CreateVector(skeletons);
    const auto clips_off = b.CreateVector(clips);
    const auto version = b.CreateString(file.version);
    afb::FinishAnimationBuffer(b, afb::CreateAnimation(b, k_animation_format_version, file.pointer_size,
                                                      version, skeletons_off, clips_off));
    const std::span<const std::uint8_t> raw(b.GetBufferPointer(), b.GetSize());
    const auto bytes = std::as_bytes(raw);
    return {bytes.begin(), bytes.end()};
}

io::ParseResult<animation::HkxFile> read_animation_asset(std::span<const std::byte> bytes,
                                                         std::string_view origin) {
    io::SpanReader reader(bytes, origin);
    const auto* raw = static_cast<const std::uint8_t*>(static_cast<const void*>(bytes.data()));
    flatbuffers::Verifier verifier(raw, bytes.size());
    if (bytes.empty() || !afb::VerifyAnimationBuffer(verifier)) {
        return reader.fail(io::ErrorKind::corrupt, "not a valid animation asset");
    }
    const auto* root = afb::GetAnimation(raw);
    if (root->format_version() != k_animation_format_version) {
        return reader.fail(io::ErrorKind::unsupported,
                           "animation asset format " + std::to_string(root->format_version()) +
                               " is not one this build reads (it reads " +
                               std::to_string(k_animation_format_version) + ")");
    }
    animation::HkxFile out;
    out.pointer_size = root->pointer_size();
    out.version = text(root->havok_version());
    if (const auto* skeletons = root->skeletons()) {
        for (const auto* s : *skeletons) {
            animation::Skeleton sk;
            sk.name = text(s->name());
            sk.parents = copy(s->parents());
            if (const auto* bones = s->bones()) {
                for (const auto* name : *bones) {
                    sk.bones.push_back(text(name));
                }
            }
            if (const auto* pose = s->reference_pose()) {
                for (const auto* p : *pose) {
                    sk.reference_pose.push_back(animation::QsTransform{
                        {p->tx(), p->ty(), p->tz()}, {p->rx(), p->ry(), p->rz(), p->rw()}, {p->sx(), p->sy(), p->sz()}});
                }
            }
            if (const auto* slots = s->float_slots()) {
                for (const auto* name : *slots) {
                    sk.float_slots.push_back(text(name));
                }
            }
            out.skeletons.push_back(std::move(sk));
        }
    }
    if (const auto* clips = root->clips()) {
        for (const auto* c : *clips) {
            animation::Clip clip;
            clip.encoding = c->encoding() == afb::ClipEncoding::Spline ? animation::ClipEncoding::spline
                                                                        : animation::ClipEncoding::interleaved;
            clip.duration = c->duration();
            clip.frame_count = c->frame_count();
            clip.frame_duration = c->frame_duration();
            clip.frames_per_block = c->frames_per_block();
            clip.transform_tracks = c->transform_tracks();
            clip.float_tracks = c->float_tracks();
            if (const auto* blocks = c->blocks()) {
                for (const auto* block : *blocks) {
                    animation::Block out_block;
                    if (const auto* tracks = block->tracks()) {
                        for (const auto* t : *tracks) {
                            out_block.tracks.push_back(animation::Track{read_channel(t->translation()),
                                                                        read_channel(t->rotation()),
                                                                        read_channel(t->scale())});
                        }
                    }
                    if (const auto* floats = block->floats()) {
                        for (const auto* f : *floats) {
                            out_block.floats.push_back(read_channel(f));
                        }
                    }
                    clip.blocks.push_back(std::move(out_block));
                }
            }
            if (const auto* notes = c->annotations()) {
                for (const auto* track : *notes) {
                    animation::AnnotationTrack t;
                    t.name = text(track->name());
                    if (const auto* entries = track->annotations()) {
                        for (const auto* a : *entries) {
                            t.annotations.push_back(animation::Annotation{a->time(), text(a->text())});
                        }
                    }
                    clip.annotations.push_back(std::move(t));
                }
            }
            clip.skeleton_name = text(c->skeleton_name());
            clip.track_to_bone = copy(c->track_to_bone());
            clip.float_to_slot = copy(c->float_to_slot());
            clip.blend_hint = c->blend_hint();
            clip.extracted_motion = text(c->extracted_motion());
            out.clips.push_back(std::move(clip));
        }
    }
    return out;
}

} // namespace bethconv::pack
