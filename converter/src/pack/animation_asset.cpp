// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/animation_asset.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/animation_generated.h"

#include <algorithm>
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

std::vector<std::byte> finish(const flatbuffers::FlatBufferBuilder& b) {
    const std::span<const std::uint8_t> raw(b.GetBufferPointer(), b.GetSize());
    const auto bytes = std::as_bytes(raw);
    return {bytes.begin(), bytes.end()};
}

/// The verified root, or why not.
io::ParseResult<const afb::Animation*> verified(std::span<const std::byte> bytes, std::string_view origin) {
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
    return root;
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

    std::vector<flatbuffers::Offset<afb::Character>> characters;
    for (const auto& c : file.characters) {
        const auto name = b.CreateString(c.name);
        const auto rig = b.CreateString(c.rig);
        const auto ragdoll = b.CreateString(c.ragdoll);
        const auto behavior = b.CreateString(c.behavior);
        const auto animations = b.CreateVectorOfStrings(c.animations);
        characters.push_back(afb::CreateCharacter(b, name, rig, ragdoll, behavior, animations));
    }

    std::vector<flatbuffers::Offset<afb::ClipGenerator>> generators;
    for (const auto& g : file.clip_generators) {
        const auto name = b.CreateString(g.name);
        const auto animation = b.CreateString(g.animation);
        generators.push_back(afb::CreateClipGenerator(b, name, animation));
    }

    const auto skeletons_off = b.CreateVector(skeletons);
    const auto clips_off = b.CreateVector(clips);
    const auto characters_off = characters.empty() ? 0 : b.CreateVector(characters).o;
    const auto generators_off = generators.empty() ? 0 : b.CreateVector(generators).o;
    const auto version = b.CreateString(file.version);
    afb::AnimationBuilder ab(b);
    ab.add_format_version(k_animation_format_version);
    ab.add_pointer_size(file.pointer_size);
    ab.add_havok_version(version);
    ab.add_skeletons(skeletons_off);
    ab.add_clips(clips_off);
    if (characters_off != 0) {
        ab.add_characters(characters_off);
    }
    if (generators_off != 0) {
        ab.add_clip_generators(generators_off);
    }
    afb::FinishAnimationBuffer(b, ab.Finish());
    return finish(b);
}

namespace {

struct ProjectOffsets {
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> files;
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<afb::ProjectClip>>> clips;
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<afb::Motion>>> motions;
};

ProjectOffsets write_project(flatbuffers::FlatBufferBuilder& b, const animation::ProjectData& data) {
    std::vector<flatbuffers::Offset<afb::ProjectClip>> clips;
    for (const auto& c : data.clips) {
        std::vector<flatbuffers::Offset<afb::Annotation>> notes;
        for (const auto& [time, text] : c.annotations) {
            notes.push_back(afb::CreateAnnotation(b, time, b.CreateString(text)));
        }
        const auto name = b.CreateString(c.name);
        const auto notes_off = b.CreateVector(notes);
        clips.push_back(afb::CreateProjectClip(b, name, c.animation, c.speed, c.crop_start, c.crop_end, notes_off));
    }
    std::vector<flatbuffers::Offset<afb::Motion>> motions;
    for (const auto& m : data.motions) {
        std::vector<afb::MotionKey> moves;
        for (const auto& k : m.translations) {
            moves.emplace_back(k.time, k.translation[0], k.translation[1], k.translation[2]);
        }
        std::vector<afb::RotationKey> turns;
        for (const auto& k : m.rotations) {
            turns.emplace_back(k.time, k.rotation[0], k.rotation[1], k.rotation[2], k.rotation[3]);
        }
        const auto moves_off = b.CreateVectorOfStructs(moves);
        const auto turns_off = b.CreateVectorOfStructs(turns);
        motions.push_back(afb::CreateMotion(b, m.animation, m.duration, moves_off, turns_off));
    }
    ProjectOffsets out;
    if (!data.files.empty()) {
        out.files = b.CreateVectorOfStrings(data.files);
    }
    if (!clips.empty()) {
        out.clips = b.CreateVector(clips);
    }
    if (!motions.empty()) {
        out.motions = b.CreateVector(motions);
    }
    return out;
}

std::string lowercase(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

animation::ProjectData read_project(const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* files,
                                    const flatbuffers::Vector<flatbuffers::Offset<afb::ProjectClip>>* clips,
                                    const flatbuffers::Vector<flatbuffers::Offset<afb::Motion>>* motions) {
    animation::ProjectData out;
    if (files != nullptr) {
        for (const auto* f : *files) {
            out.files.push_back(text(f));
        }
    }
    if (clips != nullptr) {
        for (const auto* c : *clips) {
            animation::ProjectClip clip;
            clip.name = text(c->name());
            clip.animation = c->animation();
            clip.speed = c->speed();
            clip.crop_start = c->crop_start();
            clip.crop_end = c->crop_end();
            if (const auto* notes = c->annotations()) {
                for (const auto* a : *notes) {
                    clip.annotations.emplace_back(a->time(), text(a->text()));
                }
            }
            out.clips.push_back(std::move(clip));
        }
    }
    if (motions != nullptr) {
        for (const auto* m : *motions) {
            animation::Motion motion;
            motion.animation = m->animation();
            motion.duration = m->duration();
            if (const auto* keys = m->translations()) {
                for (const auto* k : *keys) {
                    motion.translations.push_back(animation::MotionKey{k->time(), {k->x(), k->y(), k->z()}});
                }
            }
            if (const auto* keys = m->rotations()) {
                for (const auto* k : *keys) {
                    motion.rotations.push_back(
                        animation::RotationKey{k->time(), {k->x(), k->y(), k->z(), k->w()}});
                }
            }
            out.motions.push_back(std::move(motion));
        }
    }
    return out;
}

} // namespace

std::vector<std::byte> write_animation_asset(const animation::ProjectData& data) {
    flatbuffers::FlatBufferBuilder b(4096);
    const auto top = write_project(b, data);
    // Sorted by lowercase name, so a reader can search.
    std::vector<const animation::ProjectData*> sorted;
    for (const auto& p : data.projects) {
        sorted.push_back(&p);
    }
    std::ranges::stable_sort(sorted, [](const auto* x, const auto* y) { return lowercase(x->name) < lowercase(y->name); });
    std::vector<flatbuffers::Offset<afb::Project>> projects;
    for (const auto* p : sorted) {
        const auto offsets = write_project(b, *p);
        const auto name = b.CreateString(p->name);
        projects.push_back(afb::CreateProject(b, name, offsets.files, offsets.clips, offsets.motions));
    }
    const auto projects_off = projects.empty() ? 0 : b.CreateVector(projects).o;
    afb::AnimationBuilder ab(b);
    ab.add_format_version(k_animation_format_version);
    if (!top.files.IsNull()) {
        ab.add_project_files(top.files);
    }
    if (!top.clips.IsNull()) {
        ab.add_project_clips(top.clips);
    }
    if (!top.motions.IsNull()) {
        ab.add_motions(top.motions);
    }
    if (projects_off != 0) {
        ab.add_projects(projects_off);
    }
    afb::FinishAnimationBuffer(b, ab.Finish());
    return finish(b);
}

io::ParseResult<animation::HkxFile> read_animation_asset(std::span<const std::byte> bytes,
                                                         std::string_view origin) {
    auto checked = verified(bytes, origin);
    if (!checked) {
        return std::unexpected(std::move(checked).error());
    }
    const auto* root = *checked;
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
    if (const auto* characters = root->characters()) {
        for (const auto* c : *characters) {
            animation::Character ch;
            ch.name = text(c->name());
            ch.rig = text(c->rig());
            ch.ragdoll = text(c->ragdoll());
            ch.behavior = text(c->behavior());
            if (const auto* names = c->animations()) {
                for (const auto* n : *names) {
                    ch.animations.push_back(text(n));
                }
            }
            out.characters.push_back(std::move(ch));
        }
    }
    if (const auto* generators = root->clip_generators()) {
        for (const auto* g : *generators) {
            out.clip_generators.push_back(animation::ClipGenerator{text(g->name()), text(g->animation())});
        }
    }
    return out;
}

io::ParseResult<animation::ProjectData> read_project_asset(std::span<const std::byte> bytes,
                                                           std::string_view origin) {
    auto checked = verified(bytes, origin);
    if (!checked) {
        return std::unexpected(std::move(checked).error());
    }
    const auto* root = *checked;
    animation::ProjectData out = read_project(root->project_files(), root->project_clips(), root->motions());
    if (const auto* projects = root->projects()) {
        for (const auto* p : *projects) {
            auto project = read_project(p->files(), p->clips(), p->motions());
            project.name = text(p->name());
            out.projects.push_back(std::move(project));
        }
    }
    return out;
}

} // namespace bethconv::pack
