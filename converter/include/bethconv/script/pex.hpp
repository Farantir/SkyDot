// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiled Papyrus (`.pex`). `parse_pex` reads the header and string table
// (enough to identify a file); `read_pex_script` reads everything: debug line
// numbers, user flags, objects with their variables, properties, states and
// functions, and each function's bytecode.
//
// Layout, all big-endian: header, string table (uint16 count, wstrings), debug
// info (a flag byte; if set, a modification time and per function its object,
// state and name indices, kind and one line number per instruction), user
// flags (name index, bit), objects. An object is its name index, a uint32 size,
// parent, docstring, user flags, auto state, then variables, properties and
// states. Functions list parameters, locals and instructions; an instruction
// is an opcode byte and a fixed number of values per opcode, plus for the three
// call opcodes an integer count and that many more. A value is a type byte
// (0 none, 1 identifier, 2 string, 3 int, 4 float, 5 bool) and a string index,
// int32, float or byte.
//
// Skyrim's PEX is big-endian, unlike Fallout 4's; `parse_pex` picks the byte
// order from the magic, so a Fallout 4 script (`DE C0 57 FA`) is reported as
// such instead of "bad magic".
//
// Source: 896 loose `.pex` files and the vanilla script archive, checked
// against <https://en.uesp.net/wiki/Skyrim_Mod:Compiled_Script_File_Format>.
// All 896 are `FA57C0DE 03 02 0001` (major 3, minor 2, game 1); versions are
// recorded, not enforced.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/io/span_reader.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::script {

/// 0xFA57C0DE. Big-endian in Skyrim, so the file starts `FA 57 C0 DE`.
inline constexpr std::uint32_t k_pex_magic = 0xFA57'C0DEu;

/// Compiler, from `gameID`. Fallout 4 is recognized only to report it clearly.
enum class PexGame : std::uint8_t {
    skyrim,     ///< gameID 1. The only convertible one.
    fallout4,   ///< gameID 2.
    unknown,    ///< Anything else; see `game_id`.
};

[[nodiscard]] std::string_view to_string(PexGame game) noexcept;

/// Header facts of a `.pex`. Strings are views into the caller's bytes, which
/// must outlive the result.
struct PexInfo {
    std::uint8_t major_version{};
    std::uint8_t minor_version{};
    std::uint16_t game_id{};
    PexGame game{PexGame::unknown};

    /// Compile time (Unix seconds). Why two compiles of the same `.psc` differ.
    std::uint64_t compilation_time{};

    std::string_view source_file; ///< The `.psc` this was compiled from.
    std::string_view username;    ///< Who compiled it.
    std::string_view machine;     ///< On which machine.

    std::uint16_t string_count{};

    /// False means a Fallout 4 layout, parsed only enough to identify it.
    bool big_endian{true};

    /// End of the string table (where unparsed data begins). At most
    /// `file_bytes`.
    std::size_t parsed_bytes{};
    std::size_t file_bytes{};

    /// Bytes after the string table (debug info, flags, objects, bytecode),
    /// copied unread.
    [[nodiscard]] std::size_t unparsed_bytes() const noexcept {
        return file_bytes - parsed_bytes;
    }

    /// Only Skyrim scripts go into a pack.
    [[nodiscard]] bool convertible() const noexcept { return game == PexGame::skyrim; }
};

/// Read the header and the string table; no bytecode. Fails on bad magic, a
/// truncated header, or string lengths that do not fit the file. Failures are
/// reported and the script is left out of the pack.
[[nodiscard]] io::ParseResult<PexInfo> parse_pex(std::span<const std::byte> bytes,
                                                 std::string_view origin);

// ---- the whole script -----------------------------------------------------

enum class PexValueType : std::uint8_t {
    none = 0,
    identifier = 1, ///< A variable, parameter, local or type name.
    string = 2,
    integer = 3,
    floating = 4,
    boolean = 5,
};

/// An instruction argument or a variable's initial value. `data` is a string
/// index for identifiers and strings, the bits of an int or float, or 0/1.
struct PexValue {
    PexValueType type{};
    std::uint32_t data{};

    [[nodiscard]] std::int32_t as_int() const noexcept { return static_cast<std::int32_t>(data); }
    [[nodiscard]] float as_float() const noexcept { return std::bit_cast<float>(data); }

    friend constexpr bool operator==(const PexValue&, const PexValue&) noexcept = default;
};

/// Opcodes 0x00-0x23. Source: UESP, and every vanilla script decodes with them.
enum class PexOp : std::uint8_t {
    nop, iadd, fadd, isub, fsub, imul, fmul, idiv, fdiv, imod,
    not_, ineg, fneg, assign, cast, cmp_eq, cmp_lt, cmp_le, cmp_gt, cmp_ge,
    jmp, jmpt, jmpf, callmethod, callparent, callstatic, return_, strcat,
    propget, propset, array_create, array_length, array_getelement,
    array_setelement, array_findelement, array_rfindelement,
};
inline constexpr std::size_t k_pex_op_count = 36;

struct PexOpInfo {
    std::string_view name;
    std::uint8_t fixed_args{};
    bool varargs{}; ///< callmethod, callparent, callstatic.
};
[[nodiscard]] const PexOpInfo& op_info(PexOp op) noexcept;

/// Name and type, both string indices.
struct PexNameType {
    std::uint16_t name{};
    std::uint16_t type{};
};

struct PexFunction {
    std::uint16_t name{}; ///< The property's name for getters and setters.
    std::uint16_t return_type{};
    std::uint16_t docstring{};
    std::uint32_t user_flags{};
    std::uint8_t flags{}; ///< Bit 0 global (static), bit 1 native.
    std::vector<PexNameType> params;
    std::vector<PexNameType> locals;
    /// Instruction i is `opcodes[i]` with `args[arg_offsets[i]..arg_offsets[i+1]]`.
    /// A call's variable arguments follow its fixed ones; the count is implied.
    std::vector<PexOp> opcodes;
    std::vector<std::uint32_t> arg_offsets;
    std::vector<PexValue> args;
    /// Source line per instruction, from the debug info; empty without it.
    std::vector<std::uint16_t> lines;

    [[nodiscard]] bool is_global() const noexcept { return (flags & 0x1u) != 0; }
    [[nodiscard]] bool is_native() const noexcept { return (flags & 0x2u) != 0; }
};

struct PexVariable {
    std::uint16_t name{};
    std::uint16_t type{};
    std::uint32_t user_flags{};
    PexValue initial;
};

struct PexProperty {
    std::uint16_t name{};
    std::uint16_t type{};
    std::uint16_t docstring{};
    std::uint32_t user_flags{};
    /// Bit 0 has a getter, bit 1 a setter, bit 2 auto (backed by `auto_var`).
    std::uint8_t flags{};
    std::uint16_t auto_var{};
    std::optional<PexFunction> getter;
    std::optional<PexFunction> setter;
};

struct PexState {
    std::uint16_t name{}; ///< The empty string for the default state.
    std::vector<PexFunction> functions;
};

struct PexObject {
    std::uint16_t name{};
    std::uint16_t parent{}; ///< The empty string for none.
    std::uint16_t docstring{};
    std::uint32_t user_flags{};
    std::uint16_t auto_state{};
    std::vector<PexVariable> variables;
    std::vector<PexProperty> properties;
    std::vector<PexState> states;
};

struct PexUserFlag {
    std::uint16_t name{};
    std::uint8_t bit{};
};

struct PexScript {
    std::uint8_t major_version{};
    std::uint8_t minor_version{};
    std::uint64_t compilation_time{};
    std::string source_file;
    std::vector<std::string> strings;
    bool has_debug_info{};
    std::vector<PexUserFlag> user_flags;
    std::vector<PexObject> objects;

    [[nodiscard]] const std::string& string(std::uint16_t index) const { return strings.at(index); }
};

/// Read a Skyrim script completely. Every string index must be in the table,
/// every opcode known, every jump inside its function and the file consumed
/// exactly; anything else fails. Fallout 4 scripts are `unsupported`.
[[nodiscard]] io::ParseResult<PexScript> read_pex_script(std::span<const std::byte> bytes,
                                                         std::string_view origin);

/// A readable listing: objects, variables, properties, states and each
/// function's instructions with their arguments.
[[nodiscard]] std::string disassemble(const PexScript& script);

} // namespace bethconv::script
