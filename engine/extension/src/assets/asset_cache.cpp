// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets/asset_cache.hpp"

#include "assets/vpath.hpp"
#include "actors/actor_animation.hpp"
#include "physics/collision.hpp"

#include <godot_cpp/classes/cubemap.hpp>
#include <godot_cpp/classes/gltf_document.hpp>
#include <godot_cpp/classes/gltf_state.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <cstring>

namespace skydot {

namespace {

using godot::Ref;

constexpr std::string_view k_clip_prefix = "clip:";

godot::PackedByteArray to_godot(const std::vector<std::uint8_t>& bytes) {
    godot::PackedByteArray out;
    out.resize(static_cast<std::int64_t>(bytes.size()));
    if (!bytes.empty()) {
        std::memcpy(out.ptrw(), bytes.data(), bytes.size());
    }
    return out;
}

std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

void put_u32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

// DDS layout (Microsoft, "DDS_HEADER"): "DDS " then a 124-byte header; caps2
// at file offset 112, the pixel format's fourCC at 84. A "DX10" fourCC adds a
// 20-byte header whose miscFlag (offset 136) marks cube maps.
constexpr std::size_t k_dds_header = 128;
constexpr std::size_t k_dds_dx10_header = 148;
constexpr std::uint32_t k_caps2_cubemap = 0x200;
constexpr std::uint32_t k_caps2_all_faces = 0xFE00;
constexpr std::uint32_t k_dx10_misc_cube = 0x4;

godot::Ref<godot::Image> image_from_dds(const std::vector<std::uint8_t>& dds) {
    Ref<godot::Image> image;
    image.instantiate();
    if (image->load_dds_from_buffer(to_godot(dds)) != godot::Error::OK) {
        return {};
    }
    return image;
}

/// The material extras' texture paths: `extras.bethconv.texture_slots.*.path`.
std::vector<std::string> slot_paths(const Ref<godot::Material>& material) {
    std::vector<std::string> out;
    if (material.is_null() || !material->has_meta("extras")) {
        return out;
    }
    const godot::Variant extras = material->get_meta("extras");
    if (extras.get_type() != godot::Variant::DICTIONARY) {
        return out;
    }
    const godot::Variant block = godot::Dictionary(extras).get("bethconv", godot::Variant());
    if (block.get_type() != godot::Variant::DICTIONARY) {
        return out;
    }
    const godot::Variant slots = godot::Dictionary(block).get("texture_slots", godot::Variant());
    if (slots.get_type() != godot::Variant::DICTIONARY) {
        return out;
    }
    const godot::Array values = godot::Dictionary(slots).values();
    for (int64_t i = 0; i < values.size(); ++i) {
        if (values[i].get_type() != godot::Variant::DICTIONARY) {
            continue;
        }
        const godot::String path = godot::Dictionary(values[i]).get("path", godot::String());
        if (!path.is_empty()) {
            out.emplace_back(path.utf8().get_data());
        }
    }
    return out;
}

} // namespace

AssetCache::AssetCache(std::shared_ptr<const PackStore> store, bool threaded)
    : store_(std::move(store)), threaded_(threaded) {}

AssetCache::~AssetCache() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    queued_cv_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

bool AssetCache::has(const std::string& vpath) const {
    return store_->find(normalize_vpath(vpath)) != nullptr;
}

godot::PackedByteArray AssetCache::bytes(const std::string& vpath) const {
    godot::PackedByteArray out;
    if (const auto data = store_->read(normalize_vpath(vpath))) {
        out.resize(static_cast<std::int64_t>(data->size()));
        std::copy(data->begin(), data->end(), out.ptrw());
    }
    return out;
}

void AssetCache::trim() {
    std::lock_guard lock(mutex_);
    std::erase_if(ready_, [](const auto& entry) {
        return entry.second.is_valid() && entry.second->get_reference_count() <= 1;
    });
}

std::size_t AssetCache::cached() const {
    std::lock_guard lock(mutex_);
    return ready_.size();
}

std::size_t AssetCache::pending() const {
    std::lock_guard lock(mutex_);
    return in_flight_.size();
}

void AssetCache::start_workers() {
    // Called with mutex_ held.
    if (!workers_.empty()) {
        return;
    }
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned count = std::clamp(cores / 2, 1u, 4u);
    for (unsigned i = 0; i < count; ++i) {
        workers_.emplace_back([this] { work(); });
    }
}

AssetCache::Status AssetCache::request(const std::string& vpath) {
    const std::string key = normalize_vpath(vpath);
    if (!threaded_) {
        return get(key).is_valid() ? Status::ready : Status::missing;
    }
    std::lock_guard lock(mutex_);
    if (const auto it = ready_.find(key); it != ready_.end()) {
        return it->second.is_valid() ? Status::ready : Status::missing;
    }
    if (in_flight_.contains(key)) {
        return Status::loading;
    }
    if (!loadable(key)) {
        ready_.emplace(key, Ref<godot::Resource>());
        return Status::missing;
    }
    in_flight_.insert(key);
    queue_.push_back(key);
    start_workers();
    queued_cv_.notify_one();
    return Status::loading;
}

Ref<godot::Resource> AssetCache::get(const std::string& vpath) {
    const std::string key = normalize_vpath(vpath);
    // Never wait for a worker: a worker creating GPU resources can itself wait
    // for the main thread, so the caller loads the asset too and the first
    // result is kept.
    bool mine = false;
    {
        std::lock_guard lock(mutex_);
        if (const auto it = ready_.find(key); it != ready_.end()) {
            return it->second;
        }
        if (!in_flight_.contains(key)) {
            in_flight_.insert(key);
            mine = true;
        } else if (const auto queued = std::find(queue_.begin(), queue_.end(), key);
                   queued != queue_.end()) {
            queue_.erase(queued); // not started: take it over
            mine = true;
        }
    }
    Ref<godot::Resource> loaded = load(key);
    std::lock_guard lock(mutex_);
    const auto [it, inserted] = ready_.emplace(key, loaded);
    if (mine) {
        in_flight_.erase(key);
    }
    return it->second;
}

Ref<godot::Resource> AssetCache::cached(const std::string& vpath) const {
    std::lock_guard lock(mutex_);
    const auto it = ready_.find(normalize_vpath(vpath));
    return it != ready_.end() ? it->second : Ref<godot::Resource>();
}

Ref<SkydotModel> AssetCache::scene(const std::string& vpath) {
    return get(vpath);
}

Ref<godot::Texture> AssetCache::texture(const std::string& vpath) {
    if (vpath.empty()) {
        return {};
    }
    return get(vpath);
}

void AssetCache::work() {
    while (true) {
        std::string key;
        {
            std::unique_lock lock(mutex_);
            queued_cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            key = std::move(queue_.front());
            queue_.pop_front();
        }
        Ref<godot::Resource> loaded = load(key);
        std::lock_guard lock(mutex_);
        ready_.emplace(key, loaded); // a caller of `get` may have been first
        in_flight_.erase(key);
    }
}

std::string AssetCache::clip_key(std::string_view clip, std::string_view skeleton) {
    return std::string(k_clip_prefix) + normalize_vpath(clip) + "|" + normalize_vpath(skeleton);
}

Ref<godot::Animation> AssetCache::clip(const std::string& clip, const std::string& skeleton) {
    return get(clip_key(clip, skeleton));
}

bool AssetCache::loadable(const std::string& key) const {
    if (key.starts_with(k_clip_prefix)) {
        const auto bar = key.find('|');
        return bar != std::string::npos &&
               store_->find(key.substr(k_clip_prefix.size(), bar - k_clip_prefix.size())) != nullptr &&
               store_->find(key.substr(bar + 1)) != nullptr;
    }
    return store_->find(key) != nullptr;
}

Ref<godot::Resource> AssetCache::load_clip(const std::string& key) const {
    const auto bar = key.find('|');
    if (bar == std::string::npos) {
        return {};
    }
    Ref<godot::Animation> clip = SkydotAnimation::build_clip_for(
        bytes(key.substr(k_clip_prefix.size(), bar - k_clip_prefix.size())), bytes(key.substr(bar + 1)),
        "bethconv_z_up_to_y_up/Skeleton");
    if (clip.is_valid()) {
        clip->set_loop_mode(godot::Animation::LOOP_LINEAR);
    }
    return clip;
}

Ref<godot::Resource> AssetCache::load(const std::string& key) {
    if (key.starts_with(k_clip_prefix)) {
        return load_clip(key);
    }
    const PackStore::Entry* entry = store_->find(key);
    if (entry == nullptr) {
        return {};
    }
    const auto bytes = store_->read(key);
    if (!bytes) {
        return {};
    }
    if (entry->kind == "mesh") {
        return load_scene(key, *bytes);
    }
    if (entry->kind == "texture") {
        return texture_from_dds(*bytes);
    }
    return {};
}

Ref<godot::Resource> AssetCache::load_scene(const std::string& vpath,
                                            const std::vector<std::uint8_t>& glb) {
    Ref<godot::GLTFDocument> document;
    document.instantiate();
    Ref<godot::GLTFState> state;
    state.instantiate();
    // Pack meshes carry no glTF images; the base path is never used.
    if (document->append_from_buffer(to_godot(glb), "", state) != godot::Error::OK) {
        return {};
    }
    // The textures the materials will ask for, loaded here rather than on the
    // main thread.
    const godot::TypedArray<Ref<godot::Material>> materials = state->get_materials();
    std::vector<Ref<godot::Texture>> textures;
    for (int64_t i = 0; i < materials.size(); ++i) {
        for (const std::string& path : slot_paths(materials[i])) {
            if (auto t = texture(path); t.is_valid()) {
                textures.push_back(std::move(t));
            }
        }
    }
    // Importer defaults: 30 fps, no trimming, immutable tracks removed.
    godot::Node* root = document->generate_scene(state, 30.0F, false, true);
    if (root == nullptr) {
        return {};
    }
    Ref<SkydotModel> model;
    model.instantiate();
    model->set_collision(ModelCollision::take_from(root));
    model->set_template(root, godot::String::utf8(vpath.c_str()));
    model->set_textures(std::move(textures));
    return model;
}

Ref<godot::Texture> AssetCache::texture_from_dds(const std::vector<std::uint8_t>& dds) {
    if (dds.size() < k_dds_header || dds[0] != 'D' || dds[1] != 'D' || dds[2] != 'S' ||
        dds[3] != ' ') {
        return {};
    }
    const bool dx10 = u32(dds, 84) == 0x30315844; // "DX10"
    const std::size_t data_start = dx10 ? k_dds_dx10_header : k_dds_header;
    const bool cube = (u32(dds, 112) & k_caps2_cubemap) != 0 ||
                      (dx10 && dds.size() >= data_start && (u32(dds, 136) & k_dx10_misc_cube) != 0);
    if (!cube) {
        const Ref<godot::Image> image = image_from_dds(dds);
        return image.is_valid() ? Ref<godot::Texture>(godot::ImageTexture::create_from_image(image))
                                : Ref<godot::Texture>();
    }

    // A cube map is six faces, each a full mip chain, in +X -X +Y -Y +Z -Z
    // order (Godot's Cubemap order too). Load each as a 2D DDS.
    if (dds.size() <= data_start || (dds.size() - data_start) % 6 != 0) {
        return {};
    }
    const std::size_t face_bytes = (dds.size() - data_start) / 6;
    godot::TypedArray<Ref<godot::Image>> faces;
    for (std::size_t face = 0; face < 6; ++face) {
        std::vector<std::uint8_t> one(dds.begin(), dds.begin() + static_cast<std::ptrdiff_t>(data_start));
        put_u32(one, 112, u32(one, 112) & ~(k_caps2_cubemap | k_caps2_all_faces));
        if (dx10) {
            put_u32(one, 136, u32(one, 136) & ~k_dx10_misc_cube);
            put_u32(one, 140, 1); // arraySize
        }
        const auto begin = dds.begin() + static_cast<std::ptrdiff_t>(data_start + face * face_bytes);
        one.insert(one.end(), begin, begin + static_cast<std::ptrdiff_t>(face_bytes));
        const Ref<godot::Image> image = image_from_dds(one);
        if (image.is_null()) {
            return {};
        }
        faces.push_back(image);
    }
    Ref<godot::Cubemap> cubemap;
    cubemap.instantiate();
    if (cubemap->create_from_images(faces) != godot::Error::OK) {
        return {};
    }
    return cubemap;
}

} // namespace skydot
