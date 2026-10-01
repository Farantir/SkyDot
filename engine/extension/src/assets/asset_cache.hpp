// SPDX-License-Identifier: GPL-3.0-or-later
//
// Godot resources built from a pack's asset bytes, cached by virtual path:
// meshes as `SkydotModel` (Godot's runtime glTF loader), textures as
// `ImageTexture` or `Cubemap` (DDS loaded as is, block formats kept). No import
// step and no `.pck` (formats/pack-format.md, "Loading in an engine").
//
// `request` loads on worker threads and reports progress; `get` returns a
// cached resource or loads it on the calling thread, never waiting for a
// worker (one creating GPU resources may be waiting for the main thread).
// A mesh's worker also loads the textures its materials name, so material
// setup on the main thread finds them cached. Misses are cached too.
//
// Shared (std::shared_ptr) by the pack, the world, LOD and materials; owns
// its threads and joins them when the last owner lets go.
#pragma once

#include "assets/pack_store.hpp"

#include "assets/model.hpp"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skydot {

class AssetCache {
public:
    enum class Status { missing, loading, ready };

    /// `threaded` false loads every request on the calling thread.
    AssetCache(std::shared_ptr<const PackStore> store, bool threaded);
    ~AssetCache();
    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    /// Lowercase, forward slashes, no leading slash.
    static std::string normalize(std::string_view vpath);

    /// Queue `vpath` for the workers if it is not cached or queued.
    Status request(const std::string& vpath);

    /// The resource, loading it here if no worker has it. Null if the pack
    /// lacks it or it does not load.
    godot::Ref<godot::Resource> get(const std::string& vpath);
    godot::Ref<SkydotModel> scene(const std::string& vpath);
    godot::Ref<godot::Texture> texture(const std::string& vpath);

    /// Drop cached resources nothing outside the cache holds.
    void trim();

    [[nodiscard]] bool has(const std::string& vpath) const;
    [[nodiscard]] std::size_t cached() const;
    [[nodiscard]] std::size_t pending() const;

    /// Build a texture from DDS bytes: a Cubemap for cube maps, else an
    /// ImageTexture. Null if Godot cannot read them.
    static godot::Ref<godot::Texture> texture_from_dds(const std::vector<std::uint8_t>& dds);

private:
    godot::Ref<godot::Resource> load(const std::string& vpath);
    godot::Ref<godot::Resource> load_scene(const std::string& vpath,
                                           const std::vector<std::uint8_t>& glb);
    void start_workers();
    void work();

    std::shared_ptr<const PackStore> store_;

    mutable std::mutex mutex_;
    std::condition_variable queued_cv_; ///< Work for the workers.
    std::unordered_map<std::string, godot::Ref<godot::Resource>> ready_;
    std::unordered_set<std::string> in_flight_; ///< Queued or loading.
    std::deque<std::string> queue_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
    bool threaded_{true};
};

} // namespace skydot
