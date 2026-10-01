// SPDX-License-Identifier: GPL-3.0-or-later
//
// One Papyrus script, loaded from a script asset (formats/pack-format.md,
// "Script assets"; schema formats/schema/script.fbs). Loading
// checks every index the interpreter will follow, so running a class never
// reads out of bounds, and resolves each identifier argument once: to a
// register (parameter or local), a script variable, `self` or `::State`.
//
// Names are case-insensitive in Papyrus; lookups take lowercase keys.
#pragma once

#include "script_generated.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::vm {

namespace sfb = bethconv::pack::sfb;

enum class Op : std::uint8_t {
    nop, iadd, fadd, isub, fsub, imul, fmul, idiv, fdiv, imod,
    not_, ineg, fneg, assign, cast, cmp_eq, cmp_lt, cmp_le, cmp_gt, cmp_ge,
    jmp, jmpt, jmpf, callmethod, callparent, callstatic, return_, strcat,
    propget, propset, array_create, array_length, array_getelement,
    array_setelement, array_findelement, array_rfindelement,
};
inline constexpr std::size_t k_op_count = 36;

/// Where an argument's value lives.
struct Slot {
    enum class Type : std::uint8_t { literal, reg, var, self, state, name };
    Type type = Type::literal;
    std::uint32_t index = 0; ///< Register or variable index, or string index for `name`.
};

class ScriptClass;

struct Function {
    const ScriptClass* owner = nullptr;
    std::string name;        ///< Lowercase.
    std::string state;       ///< Lowercase; the state it is defined in.
    /// 0 a function in `state`, 1 the getter and 2 the setter of property `name`.
    std::uint8_t handler = 0;
    std::string return_type; ///< Lowercase.
    bool native = false;
    bool global = false;
    std::uint32_t param_count = 0;
    std::vector<std::string> reg_names; ///< Parameters, then locals; lowercase.
    std::vector<std::string> reg_types; ///< Lowercase.
    std::vector<Op> ops;
    std::vector<std::uint32_t> offsets; ///< ops.size() + 1 entries into `args`.
    std::vector<sfb::Value> args;
    std::vector<Slot> slots; ///< One per argument.
    std::vector<std::uint16_t> lines;
};

struct Variable {
    std::string name; ///< Lowercase.
    std::string type; ///< Lowercase.
    sfb::Value initial;
};

struct Property {
    std::string name; ///< Lowercase.
    std::string type; ///< Lowercase.
    std::optional<std::uint32_t> auto_var;
    const Function* getter = nullptr;
    const Function* setter = nullptr;
};

class ScriptClass {
public:
    /// Verify and index a script asset. Returns null and sets `error` if the
    /// bytes are not one or break a rule the interpreter relies on.
    static std::unique_ptr<ScriptClass> load(std::vector<std::uint8_t> bytes, std::string& error);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& key() const noexcept { return key_; }
    [[nodiscard]] const std::string& parent_key() const noexcept { return parent_key_; }
    [[nodiscard]] const ScriptClass* parent() const noexcept { return parent_; }
    void set_parent(const ScriptClass* parent) noexcept { parent_ = parent; }
    [[nodiscard]] const std::string& auto_state() const noexcept { return auto_state_; }

    /// String table entry `index` (checked at load), as stored.
    [[nodiscard]] const std::string& string(std::uint32_t index) const { return strings_[index]; }
    [[nodiscard]] const std::string& lower(std::uint32_t index) const { return lower_[index]; }

    /// This class's function `name` in `state` ("" for the default state).
    [[nodiscard]] const Function* find(std::string_view state, std::string_view name) const;
    [[nodiscard]] const std::vector<Variable>& variables() const noexcept { return variables_; }
    [[nodiscard]] const Property* property(std::string_view name) const;
    /// Whether the class declares native functions, i.e. is a type the
    /// engine provides (Form, ObjectReference, Debug...), not a script that
    /// must be attached.
    [[nodiscard]] bool engine_type() const noexcept { return engine_type_; }
    [[nodiscard]] bool derives_from(const ScriptClass* other) const noexcept;

private:
    bool read(std::string& error);
    bool read_function(const sfb::Function& f, std::string_view name, Function& out,
                       std::string& error) const;

    std::vector<std::uint8_t> bytes_;
    const sfb::Script* root_ = nullptr;
    std::vector<std::string> strings_;
    std::vector<std::string> lower_;
    std::string name_;
    std::string key_;
    std::string parent_key_;
    std::string auto_state_;
    const ScriptClass* parent_ = nullptr;
    bool engine_type_ = false;
    std::vector<Variable> variables_;
    std::map<std::string, Property, std::less<>> properties_;
    /// "state\x1Ffunction" -> function. Functions are heap-allocated so the
    /// pointers properties and frames hold stay valid.
    std::map<std::string, std::unique_ptr<Function>, std::less<>> functions_;
    std::vector<std::unique_ptr<Function>> handlers_;
};

[[nodiscard]] std::string to_lower(std::string_view text);

} // namespace skydot::vm
