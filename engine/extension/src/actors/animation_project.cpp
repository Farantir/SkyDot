// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors/animation_project.hpp"

#include "assets/vpath.hpp"

#include <algorithm>

namespace skydot {
namespace {

namespace afb = bethconv::pack::afb;

constexpr std::uint32_t k_animation_format = 5;

std::string text(const flatbuffers::String* s) { return s != nullptr ? s->str() : std::string(); }

} // namespace

std::string normalize_path(const std::string& path) {
    std::string s = ascii_lower(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto end = s.find('/', start);
        if (end == std::string::npos) {
            end = s.size();
        }
        const std::string part = s.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = end + 1;
    }
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) {
            out += '/';
        }
        out += p;
    }
    return out;
}

const afb::Animation* animation_asset(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return nullptr;
    }
    flatbuffers::Verifier verifier(bytes.data(), bytes.size());
    if (!afb::VerifyAnimationBuffer(verifier)) {
        return nullptr;
    }
    const auto* root = afb::GetAnimation(bytes.data());
    return root->format_version() == k_animation_format ? root : nullptr;
}

const ProjectClip* AnimationProject::clip(const std::string& wanted) const {
    const std::string key = ascii_lower(wanted);
    for (const ProjectClip& c : clips) {
        if (ascii_lower(c.name) == key) {
            return &c;
        }
    }
    return nullptr;
}

AnimationProject read_animation_project(const std::string& behaviour, const AssetReader& read) {
    AnimationProject out;
    std::string path = normalize_path(behaviour);
    if (!path.starts_with("meshes/")) {
        path = "meshes/" + path;
    }
    const auto slash = path.rfind('/');
    const auto dot = path.rfind('.');
    if (slash == std::string::npos || dot == std::string::npos || dot < slash) {
        out.missing = "no behaviour project";
        return out;
    }
    out.folder = path.substr(0, slash + 1);
    out.name = path.substr(slash + 1, dot - slash - 1);

    using Strings = flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>;
    using Clips = flatbuffers::Vector<flatbuffers::Offset<afb::ProjectClip>>;
    using Motions = flatbuffers::Vector<flatbuffers::Offset<afb::Motion>>;
    const Strings* files = nullptr;
    const Clips* clips = nullptr;
    const Motions* motions = nullptr;
    // The project's own files, else its entry in the single file.
    const std::vector<std::uint8_t> project_bytes = read("meshes/animationdata/" + out.name + ".txt");
    const std::vector<std::uint8_t> bound_bytes = read("meshes/animationdata/boundanims/anims_" + out.name + ".txt");
    std::vector<std::uint8_t> single_bytes;
    if (const auto* proj = animation_asset(project_bytes); proj != nullptr && proj->project_clips() != nullptr) {
        files = proj->project_files();
        clips = proj->project_clips();
        if (const auto* bound = animation_asset(bound_bytes)) {
            motions = bound->motions();
        }
    } else {
        single_bytes = read("meshes/animationdatasinglefile.txt");
        if (const auto* single = animation_asset(single_bytes); single != nullptr && single->projects() != nullptr) {
            for (const auto* p : *single->projects()) {
                if (p->name() != nullptr && ascii_lower(p->name()->str()) == out.name) {
                    files = p->files();
                    clips = p->clips();
                    motions = p->motions();
                    break;
                }
            }
        }
    }
    if (clips == nullptr) {
        out.missing = "no animationdata for " + out.name;
        return out;
    }
    if (files != nullptr) {
        for (const auto* f : *files) {
            out.files.push_back(normalize_path(f->str()));
        }
    }
    for (const auto* c : *clips) {
        ProjectClip clip;
        clip.name = text(c->name());
        clip.animation = c->animation();
        clip.speed = c->speed() > 0.0F ? c->speed() : 1.0F;
        if (const auto* entries = c->annotations()) {
            for (const auto* a : *entries) {
                clip.notes.emplace_back(a->time(), text(a->text()));
            }
        }
        out.clips.push_back(std::move(clip));
    }
    if (motions != nullptr) {
        for (const auto* m : *motions) {
            ProjectMotion motion;
            motion.duration = m->duration();
            if (const auto* keys = m->translations()) {
                for (const auto* k : *keys) {
                    motion.translations.push_back({k->time(), k->x(), k->y(), k->z()});
                }
            }
            if (const auto* keys = m->rotations()) {
                for (const auto* k : *keys) {
                    motion.rotations.push_back({k->time(), k->x(), k->y(), k->z(), k->w()});
                }
            }
            out.motions[m->animation()] = std::move(motion);
        }
    }
    return out;
}

} // namespace skydot
