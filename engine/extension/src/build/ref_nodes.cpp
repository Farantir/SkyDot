// SPDX-License-Identifier: GPL-3.0-or-later
#include "build/ref_nodes.hpp"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <mutex>
#include <unordered_map>

namespace skydot::ref_nodes {
namespace {

std::mutex& mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::uint32_t, godot::ObjectID>& nodes() {
    static std::unordered_map<std::uint32_t, godot::ObjectID> map;
    return map;
}

/// The node leaves the tree: forget it, unless a newer node took its place.
void on_exiting(std::uint32_t ref, std::int64_t id) {
    const std::lock_guard lock(mutex());
    if (const auto it = nodes().find(ref); it != nodes().end() && static_cast<std::int64_t>(it->second) == id) {
        nodes().erase(it);
    }
}

} // namespace

void add(std::uint32_t ref, godot::Node* node) {
    if (node == nullptr) {
        return;
    }
    const auto id = node->get_instance_id();
    {
        const std::lock_guard lock(mutex());
        nodes()[ref] = godot::ObjectID(id);
    }
    const auto exiting = callable_mp_static(&on_exiting).bind(ref, static_cast<std::int64_t>(id));
    if (!node->is_connected("tree_exiting", exiting)) {
        node->connect("tree_exiting", exiting);
    }
}

godot::Node* find(std::uint32_t ref) {
    godot::ObjectID id;
    {
        const std::lock_guard lock(mutex());
        const auto it = nodes().find(ref);
        if (it == nodes().end()) {
            return nullptr;
        }
        id = it->second;
    }
    auto* node = godot::Object::cast_to<godot::Node>(godot::ObjectDB::get_instance(id));
    // A node freed while outside the tree never signalled; one queued for
    // deletion is as good as gone.
    return node != nullptr && !node->is_queued_for_deletion() ? node : nullptr;
}

void clear() {
    const std::lock_guard lock(mutex());
    nodes().clear();
}

} // namespace skydot::ref_nodes
