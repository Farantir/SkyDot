// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/scripts.hpp"

#include "data/text.hpp"
#include "skydot_formats/flags.hpp"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>

using godot::Array;
using godot::Dictionary;
using godot::Variant;

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

Variant object_value(const wfb::ScriptObject& object) {
    if (object.alias() < 0) {
        return static_cast<std::int64_t>(object.form());
    }
    Dictionary alias;
    alias["form"] = static_cast<std::int64_t>(object.form());
    alias["alias"] = static_cast<std::int64_t>(object.alias());
    return alias;
}

/// A property's value; see get_ref_info.
Variant property_value(const wfb::ScriptProperty& p) {
    Array values;
    const std::uint8_t type = p.type();
    const std::uint8_t scalar = type >= 11 ? static_cast<std::uint8_t>(type - 10) : type;
    switch (scalar) {
    case 1:
        if (p.objects() != nullptr) {
            for (const auto* o : *p.objects()) {
                values.push_back(object_value(*o));
            }
        }
        break;
    case 2:
        if (p.strings() != nullptr) {
            for (const auto* s : *p.strings()) {
                values.push_back(to_godot(s));
            }
        }
        break;
    case 3:
    case 5:
        if (p.ints() != nullptr) {
            for (const auto i : *p.ints()) {
                values.push_back(scalar == 5 ? Variant(i != 0) : Variant(static_cast<std::int64_t>(i)));
            }
        }
        break;
    case 4:
        if (p.floats() != nullptr) {
            for (const auto f : *p.floats()) {
                values.push_back(static_cast<double>(f));
            }
        }
        break;
    default:
        return Variant();
    }
    if (type >= 11) {
        return values;
    }
    return values.is_empty() ? Variant() : values[0];
}

} // namespace

Array script_list(const ScriptVector* scripts, bool from_ref) {
    Array out;
    if (scripts == nullptr) {
        return out;
    }
    for (const auto* s : *scripts) {
        Dictionary script;
        script["name"] = to_godot(s->name());
        script["status"] = static_cast<std::int64_t>(s->status());
        script["removed"] = formats::has_flag(s->status(), wfb::ScriptStatus::removed);
        script["from_ref"] = from_ref;
        Dictionary properties;
        if (s->properties() != nullptr) {
            for (const auto* p : *s->properties()) {
                properties[to_godot(p->name())] = property_value(*p);
            }
        }
        script["properties"] = properties;
        out.push_back(script);
    }
    return out;
}

} // namespace skydot
