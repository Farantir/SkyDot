// SPDX-License-Identifier: GPL-3.0-or-later
//
// Papyrus values. Arrays are shared by reference, as in Papyrus; everything
// else is copied. An object is a form plus the class it is seen as (the static
// type it was created or cast with) and, if a script of that class is attached
// to the form, that script's instance.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace skydot::vm {

class ScriptClass;
struct Instance;

enum class Kind : std::uint8_t { none, integer, floating, boolean, string, object, array };

struct Value;
using Array = std::vector<Value>;

struct Value {
    Kind kind = Kind::none;
    std::int32_t i = 0;
    float f = 0.0F;
    bool b = false;
    std::string s;
    std::uint32_t form = 0;
    const ScriptClass* cls = nullptr;
    Instance* instance = nullptr;
    std::shared_ptr<Array> array;

    static Value integer(std::int32_t v) {
        Value out;
        out.kind = Kind::integer;
        out.i = v;
        return out;
    }
    static Value floating(float v) {
        Value out;
        out.kind = Kind::floating;
        out.f = v;
        return out;
    }
    static Value boolean(bool v) {
        Value out;
        out.kind = Kind::boolean;
        out.b = v;
        return out;
    }
    static Value string(std::string v) {
        Value out;
        out.kind = Kind::string;
        out.s = std::move(v);
        return out;
    }
    static Value object(std::uint32_t form, const ScriptClass* cls, Instance* instance = nullptr) {
        Value out;
        if (form == 0) {
            return out;
        }
        out.kind = Kind::object;
        out.form = form;
        out.cls = cls;
        out.instance = instance;
        return out;
    }
    static Value make_array(std::shared_ptr<Array> a) {
        Value out;
        if (a == nullptr) {
            return out;
        }
        out.kind = Kind::array;
        out.array = std::move(a);
        return out;
    }

    [[nodiscard]] bool is_none() const noexcept { return kind == Kind::none; }
};

} // namespace skydot::vm
