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
#include "vm/name.hpp"
#include "vm/value.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
    /// Index into `Function::literals`, a register or a variable, or the string
    /// index of a `name`.
    std::uint32_t index = 0;
};

class ScriptClass;

/// What a declared type name (lowercase) stands for.
enum class TypeKind : std::uint8_t { none, integer, floating, boolean, string, array, object };
[[nodiscard]] TypeKind type_kind(std::string_view type);
/// A variable's or register's value before it is set: 0, 0.0, False or "" for
/// the scalar types, else None.
[[nodiscard]] Value default_of(TypeKind kind);

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
    std::vector<TypeKind> reg_kinds;    ///< Of `reg_types`.
    std::vector<Value> reg_defaults;    ///< A register's value before it is set: its type's default.
    std::vector<Op> ops;
    std::vector<std::uint32_t> offsets; ///< ops.size() + 1 entries into `args`.
    std::vector<sfb::Value> args;
    std::vector<Slot> slots; ///< One per argument.
    std::vector<Value> literals; ///< The arguments that are literals, as values.
    std::vector<std::uint16_t> lines;
    /// The VM's cache of what each callstatic instruction resolved to. A class
    /// never unloads, so an entry never goes stale.
    mutable std::vector<const Function*> static_targets;
};

struct Variable {
    std::string name; ///< Lowercase.
    std::string type; ///< Lowercase.
    sfb::Value initial;
    TypeKind kind = TypeKind::none; ///< Of `type`.
};

struct Property {
    std::string name; ///< Lowercase.
    std::string type; ///< Lowercase.
    std::optional<std::uint32_t> auto_var;
    const Function* getter = nullptr;
    const Function* setter = nullptr;
    TypeKind kind = TypeKind::none; ///< Of `type`.
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
    /// String table entry `index` in lowercase, with its hash.
    [[nodiscard]] Name lower_name(std::uint32_t index) const {
        return {lower_[index], lower_hash_[index]};
    }
    /// A literal argument or initial value as a Value (None for no value).
    [[nodiscard]] Value literal(const sfb::Value& v) const;

    /// This class's function `name` in `state` ("" for the default state).
    [[nodiscard]] const Function* find(const Name& state, const Name& name) const;
    [[nodiscard]] const std::vector<Variable>& variables() const noexcept { return variables_; }
    [[nodiscard]] const Property* property(const Name& name) const;
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
    std::vector<std::size_t> lower_hash_; ///< Of each entry of `lower_`.
    std::string name_;
    std::string key_;
    std::string parent_key_;
    std::string auto_state_;
    const ScriptClass* parent_ = nullptr;
    bool engine_type_ = false;
    std::vector<Variable> variables_;
    std::map<std::string, Property, std::less<>> properties_;
    HashTable<Property> property_table_;
    /// The functions, owned here so the pointers properties and frames hold
    /// stay valid, and found by state and name.
    std::vector<std::unique_ptr<Function>> functions_;
    HashTable<Function> function_table_;
    std::vector<std::unique_ptr<Function>> handlers_;
};

[[nodiscard]] std::string to_lower(std::string_view text);

} // namespace skydot::vm
