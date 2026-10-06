// SPDX-License-Identifier: GPL-3.0-or-later
//
// What activating a reference involves: doors, scripts, links.
#include "world/queries.hpp"
#include "data/coordinates.hpp"
#include "data/fb_search.hpp"
#include "data/large_refs.hpp"
#include "build/refs.hpp"
#include "data/text.hpp"

#include "skydot_formats/flags.hpp"
#include "world_generated.h"

#include <algorithm>
#include <vector>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot::queries {

namespace {

String type_name(std::uint32_t type) {
    const char chars[5] = {static_cast<char>(type & 0xFF), static_cast<char>((type >> 8) & 0xFF),
                           static_cast<char>((type >> 16) & 0xFF),
                           static_cast<char>((type >> 24) & 0xFF), 0};
    return String(chars);
}

/// Elements of a struct vector sorted by `ref()` that belong to `ref`: for the
/// vectors a reference can have several entries in.
template <typename T>
std::vector<const T*> by_ref(const flatbuffers::Vector<const T*>* list, std::uint32_t ref) {
    std::vector<const T*> out;
    if (list == nullptr) {
        return out;
    }
    for (auto i = first_at_least(list, ref, [](const T* e) { return e->ref(); });
         i < list->size() && list->Get(i)->ref() == ref; ++i) {
        out.push_back(list->Get(i));
    }
    return out;
}

const wfb::Ref* find_ref(const wfb::Cell& cell, std::uint32_t id) {
    return lookup(cell.refs(), id);
}

} // namespace

Dictionary get_door(const WorldData& data, std::int64_t ref) {
    Dictionary out;
    const auto& doors = data.doors();
    const auto it = doors.find(static_cast<std::uint32_t>(ref));
    if (it == doors.end()) {
        return out;
    }
    const auto& [cell, link] = it->second;
    out["ref"] = static_cast<std::int64_t>(link->ref());
    out["cell"] = static_cast<std::int64_t>(cell->id());
    out["destination"] = static_cast<std::int64_t>(link->destination());
    const auto dest = doors.find(link->destination());
    const wfb::Cell* dest_cell = dest != doors.end() ? dest->second.first : nullptr;
    out["destination_cell"] = static_cast<std::int64_t>(dest_cell != nullptr ? dest_cell->id() : 0);
    out["destination_world"] =
        static_cast<std::int64_t>(dest_cell != nullptr ? dest_cell->world() : 0);
    out["destination_interior"] =
        dest_cell != nullptr && formats::has_flag(dest_cell->flags(), wfb::CellFlags::interior);
    const auto& p = link->position();
    const auto& r = link->rotation();
    out["arrival"] = skyrim_transform(Vector3(p.x(), p.y(), p.z()), Vector3(r.x(), r.y(), r.z()), 1.0);
    return out;
}

Dictionary get_ref_info(const WorldData& data, std::int64_t cell_id, std::int64_t ref_id) {
    Dictionary out;
    const auto* cell = data.cell_ptr(cell_id);
    const auto id = static_cast<std::uint32_t>(ref_id);
    const auto* ref = cell != nullptr ? find_ref(*cell, id) : nullptr;
    if (ref == nullptr) {
        return out;
    }
    const auto* base = data.base_ptr(ref->base());
    out["id"] = static_cast<std::int64_t>(id);
    out["cell"] = static_cast<std::int64_t>(cell->id());
    out["base"] = static_cast<std::int64_t>(ref->base());
    out["type"] = base != nullptr ? type_name(base->type()) : String();
    out["editor_id"] = base != nullptr ? to_godot(base->editor_id()) : String();
    out["activatable"] = activatable(data, base, cell->id(), id);
    out["parent_activate_only"] =
        formats::has_flag(ref->flags(), wfb::RefFlags::parent_activate_only);
    out["disabled"] = data.initially_disabled(*ref);
    out["enable_parent"] = static_cast<std::int64_t>(ref->enable_parent());
    out["enable_opposite"] = formats::has_flag(ref->flags(), wfb::RefFlags::enable_opposite);
    out["position"] = Vector3(ref->position().x(), ref->position().y(), ref->position().z());
    out["rotation"] = Vector3(ref->rotation().x(), ref->rotation().y(), ref->rotation().z());
    out["scale"] = ref->scale();
    const auto primitives = by_ref(cell->primitives(), id);
    if (primitives.empty()) {
        out["primitive"] = Variant();
    } else {
        Dictionary primitive;
        const auto& b = primitives.front()->bounds();
        primitive["bounds"] = Vector3(b.x(), b.y(), b.z());
        primitive["type"] = static_cast<std::int64_t>(primitives.front()->type());
        out["primitive"] = primitive;
    }

    if (const auto* locked = lookup(cell->locks(), id)) {
        Dictionary lock;
        lock["level"] = static_cast<std::int64_t>(locked->level());
        lock["key"] = static_cast<std::int64_t>(locked->key());
        out["lock"] = lock;
    } else {
        out["lock"] = Variant();
    }
    const Dictionary door = get_door(data, id);
    out["door"] = door.is_empty() ? Variant() : Variant(door);

    Array links;
    for (const auto* link : by_ref(cell->links(), id)) {
        Dictionary entry;
        entry["keyword"] = static_cast<std::int64_t>(link->keyword());
        entry["target"] = static_cast<std::int64_t>(link->target());
        links.push_back(entry);
    }
    out["links"] = links;
    Array parents;
    for (const auto* parent : by_ref(cell->activate_parents(), id)) {
        Dictionary entry;
        entry["parent"] = static_cast<std::int64_t>(parent->parent());
        entry["delay"] = static_cast<double>(parent->delay());
        parents.push_back(entry);
    }
    out["activate_parents"] = parents;

    Array scripts = script_list(base != nullptr ? base->scripts() : nullptr, false);
    if (const auto* own = ref_scripts(*cell, id)) {
        scripts.append_array(script_list(own->scripts(), true));
    }
    out["scripts"] = scripts;
    return out;
}

godot::PackedInt64Array get_enable_children(const WorldData& data, std::int64_t ref) {
    godot::PackedInt64Array out;
    for (const auto child : data.enable_children(static_cast<std::uint32_t>(ref))) {
        out.push_back(child);
    }
    return out;
}

Array get_activate_children(const WorldData& data, std::int64_t ref) {
    Array out;
    for (const auto& [child, cell, delay] : data.activate_children(static_cast<std::uint32_t>(ref))) {
        Dictionary entry;
        entry["ref"] = static_cast<std::int64_t>(child);
        entry["cell"] = static_cast<std::int64_t>(cell);
        entry["delay"] = static_cast<double>(delay);
        out.push_back(entry);
    }
    return out;
}

godot::PackedInt64Array get_scripted_refs(const WorldData& data, std::int64_t cell_id) {
    godot::PackedInt64Array out;
    const auto* cell = data.cell_ptr(cell_id);
    if (cell == nullptr || cell->refs() == nullptr) {
        return out;
    }
    for (const auto* ref : *cell->refs()) {
        const auto* base = data.base_ptr(ref->base());
        const bool base_scripted = base != nullptr && base->scripts() != nullptr && base->scripts()->size() != 0;
        if (base_scripted || ref_scripts(*cell, ref->id()) != nullptr) {
            out.push_back(ref->id());
        }
    }
    return out;
}

std::int64_t get_large_ref_count(const WorldData& data, std::int64_t world) {
    return data.large_ref_count(static_cast<std::uint32_t>(world));
}

Array get_large_refs(const WorldData& data, std::int64_t world, std::int64_t x, std::int64_t y,
                     std::int64_t radius) {
    Array out;
    const auto w = static_cast<std::uint32_t>(world);
    if (radius < 0 || radius > 64) {
        return out;
    }
    // The union of the lists around (x, y), each reference once (as the engine draws them).
    const auto indices = large_refs::select(data, w, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                            static_cast<std::int32_t>(radius), {});
    for (const std::uint32_t index : indices) {
        const auto* ref = data.large_ref(w, index);
        Dictionary entry;
        entry["ref"] = static_cast<std::int64_t>(ref->id());
        entry["base"] = static_cast<std::int64_t>(ref->base());
        entry["position"] = Vector3(ref->position().x(), ref->position().y(), ref->position().z());
        entry["rotation"] = Vector3(ref->rotation().x(), ref->rotation().y(), ref->rotation().z());
        entry["scale"] = static_cast<double>(ref->scale());
        entry["cell"] = godot::Vector2i(ref->cell_x(), ref->cell_y());
        entry["enable_parent"] = static_cast<std::int64_t>(ref->enable_parent());
        entry["disabled"] = data.initially_disabled(*ref);
        // The cell holding the reference as a normal reference, 0 if none.
        entry["in_cell"] = data.cell_of_ref(ref->id());
        out.push_back(entry);
    }
    return out;
}

} // namespace skydot::queries
