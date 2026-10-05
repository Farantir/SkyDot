// SPDX-License-Identifier: GPL-3.0-or-later
//
// Papyrus values. Arrays are shared by reference, as in Papyrus; everything
// else is copied. An object is a form plus the class it is seen as (the static
// type it was created or cast with) and, if a script of that class is attached
// to the form, that script's instance.
//
// A Value is one of seven kinds and holds only what that kind needs, in 24
// bytes: the numbers, booleans and objects copy as plain bytes, a string is an
// immutable text shared between copies (Papyrus never changes a string in
// place), an array a shared vector. The accessors answer for any kind: a
// number as 0, text as "", a form as 0, a class, instance or array as null,
// when the value is of another kind.
#pragma once

#include <bit>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace skydot::vm {

class ScriptClass;
struct Instance;

enum class Kind : std::uint8_t { none, integer, floating, boolean, string, object, array };

class Value;
using Array = std::vector<Value>;

class Value {
public:
    Value() noexcept : p_() {}
    Value(const Value& other) : kind_(other.kind_), bits_(other.bits_) {
        if (other.shares()) [[unlikely]] {
            copy_shared(other);
        } else {
            p_.object = other.p_.object;
        }
    }
    Value(Value&& other) noexcept : kind_(other.kind_), bits_(other.bits_) {
        if (other.shares()) [[unlikely]] {
            take_shared(other);
        } else {
            p_.object = other.p_.object;
        }
    }
    Value& operator=(const Value& other) {
        if (this != &other) {
            *this = Value(other);
        }
        return *this;
    }
    Value& operator=(Value&& other) noexcept {
        if (this != &other) {
            if (shares()) [[unlikely]] {
                destroy_shared();
            }
            kind_ = other.kind_;
            bits_ = other.bits_;
            if (other.shares()) [[unlikely]] {
                take_shared(other);
            } else {
                p_.object = other.p_.object;
            }
        }
        return *this;
    }
    ~Value() {
        if (shares()) [[unlikely]] {
            destroy_shared();
        }
    }

    static Value integer(std::int32_t v) {
        Value out;
        out.kind_ = Kind::integer;
        out.bits_ = static_cast<std::uint32_t>(v);
        return out;
    }
    static Value floating(float v) {
        Value out;
        out.kind_ = Kind::floating;
        out.bits_ = std::bit_cast<std::uint32_t>(v);
        return out;
    }
    static Value boolean(bool v) {
        Value out;
        out.kind_ = Kind::boolean;
        out.bits_ = v ? 1U : 0U;
        return out;
    }
    static Value string(std::string v) { return text(std::make_shared<const std::string>(std::move(v))); }
    /// A string that shares `v`'s characters.
    static Value text(std::shared_ptr<const std::string> v) {
        Value out;
        if (v == nullptr) {
            return out;
        }
        new (&out.p_.text) std::shared_ptr<const std::string>(std::move(v));
        out.kind_ = Kind::string;
        return out;
    }
    static Value object(std::uint32_t form, const ScriptClass* cls, Instance* instance = nullptr) {
        Value out;
        if (form == 0) {
            return out;
        }
        out.kind_ = Kind::object;
        out.bits_ = form;
        out.p_.object = {cls, instance};
        return out;
    }
    static Value make_array(std::shared_ptr<Array> a) {
        Value out;
        if (a == nullptr) {
            return out;
        }
        new (&out.p_.array) std::shared_ptr<Array>(std::move(a));
        out.kind_ = Kind::array;
        return out;
    }

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_none() const noexcept { return kind_ == Kind::none; }

    [[nodiscard]] std::int32_t i() const noexcept {
        return kind_ == Kind::integer ? static_cast<std::int32_t>(bits_) : 0;
    }
    [[nodiscard]] float f() const noexcept { return kind_ == Kind::floating ? std::bit_cast<float>(bits_) : 0.0F; }
    [[nodiscard]] bool b() const noexcept { return kind_ == Kind::boolean && bits_ != 0; }
    [[nodiscard]] const std::string& s() const noexcept { return kind_ == Kind::string ? *p_.text : k_no_text; }
    [[nodiscard]] std::uint32_t form() const noexcept { return kind_ == Kind::object ? bits_ : 0; }
    [[nodiscard]] const ScriptClass* cls() const noexcept {
        return kind_ == Kind::object ? p_.object.cls : nullptr;
    }
    [[nodiscard]] Instance* instance() const noexcept {
        return kind_ == Kind::object ? p_.object.instance : nullptr;
    }
    [[nodiscard]] const std::shared_ptr<Array>& array() const noexcept {
        return kind_ == Kind::array ? p_.array : k_no_array;
    }

private:
    struct ObjectPtrs {
        const ScriptClass* cls;
        Instance* instance;
    };
    /// Every kind but string and array leaves `object` the active member (zero
    /// unless it is an object), so those copy as bytes.
    union Payload {
        ObjectPtrs object;
        std::shared_ptr<const std::string> text;
        std::shared_ptr<Array> array;
        Payload() noexcept : object{nullptr, nullptr} {}
        ~Payload() {}
    };

    /// Whether the payload is a shared_ptr (a string or an array); every other
    /// kind is plain bytes.
    [[nodiscard]] bool shares() const noexcept { return kind_ == Kind::string || kind_ == Kind::array; }
    void copy_shared(const Value& other) {
        if (kind_ == Kind::string) {
            new (&p_.text) std::shared_ptr<const std::string>(other.p_.text);
        } else {
            new (&p_.array) std::shared_ptr<Array>(other.p_.array);
        }
    }
    /// `other` is left None.
    void take_shared(Value& other) noexcept {
        if (kind_ == Kind::string) {
            new (&p_.text) std::shared_ptr<const std::string>(std::move(other.p_.text));
        } else {
            new (&p_.array) std::shared_ptr<Array>(std::move(other.p_.array));
        }
        other.destroy_shared();
        other.kind_ = Kind::none;
        other.bits_ = 0;
        other.p_.object = {nullptr, nullptr};
    }
    /// Leaves the payload destroyed: the caller overwrites it.
    void destroy_shared() noexcept {
        if (kind_ == Kind::string) {
            p_.text.~shared_ptr();
        } else {
            p_.array.~shared_ptr();
        }
    }

    inline static const std::string k_no_text;
    inline static const std::shared_ptr<Array> k_no_array;

    Kind kind_ = Kind::none;
    /// An integer, a float's bits, a boolean, or an object's form.
    std::uint32_t bits_ = 0;
    Payload p_;
};

static_assert(sizeof(Value) == 24);

} // namespace skydot::vm
