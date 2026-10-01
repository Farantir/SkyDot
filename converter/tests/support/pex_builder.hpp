// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic compiled-Papyrus writer for the script tests (string tables larger
// than the file, overlong lengths, Fallout 4 scripts).
//
// Big-endian is written by hand, not with the reader's helper, so fixture and
// code under test cannot share a bug.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace bethconv::testing {

struct PexSpec {
    std::uint32_t magic = 0xFA57'C0DEu;
    std::uint8_t major = 3;
    std::uint8_t minor = 2;
    std::uint16_t game_id = 1; ///< 1 Skyrim, 2 Fallout 4.
    std::uint64_t compiled_at = 0x5CAA'A31Fu;

    std::string source_file{"Fixture.psc"};
    std::string username{"tester"};
    std::string machine{"BUILDBOX"};

    std::vector<std::string> strings{"Fixture", "", "OnInit"};

    /// Written to the string count; 0 means "match `strings`". For tables that
    /// lie about their size.
    std::uint16_t declared_string_count = 0;

    /// Bytes after the string table, standing in for the unparsed rest.
    std::size_t trailing_bytes = 64;

    /// Little-endian throughout (Fallout 4).
    bool little_endian = false;
};

/// Emit a `.pex` per `spec`.
[[nodiscard]] inline std::vector<std::byte> build_pex(const PexSpec& spec) {
    std::vector<std::byte> out;

    const auto push = [&out](std::uint8_t byte) { out.push_back(static_cast<std::byte>(byte)); };
    const auto put16 = [&](std::uint16_t value) {
        if (spec.little_endian) {
            push(static_cast<std::uint8_t>(value & 0xFFu));
            push(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
        } else {
            push(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
            push(static_cast<std::uint8_t>(value & 0xFFu));
        }
    };
    const auto put32 = [&](std::uint32_t value) {
        if (spec.little_endian) {
            for (int shift = 0; shift < 32; shift += 8) {
                push(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
            }
        } else {
            for (int shift = 24; shift >= 0; shift -= 8) {
                push(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
            }
        }
    };
    const auto put64 = [&](std::uint64_t value) {
        if (spec.little_endian) {
            for (int shift = 0; shift < 64; shift += 8) {
                push(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
            }
        } else {
            for (int shift = 56; shift >= 0; shift -= 8) {
                push(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
            }
        }
    };
    const auto put_string = [&](std::string_view text) {
        put16(static_cast<std::uint16_t>(text.size()));
        for (const char c : text) {
            push(static_cast<std::uint8_t>(c));
        }
    };

    put32(spec.magic);
    push(spec.major);
    push(spec.minor);
    put16(spec.game_id);
    put64(spec.compiled_at);
    put_string(spec.source_file);
    put_string(spec.username);
    put_string(spec.machine);

    put16(spec.declared_string_count != 0
              ? spec.declared_string_count
              : static_cast<std::uint16_t>(spec.strings.size()));
    for (const auto& entry : spec.strings) {
        put_string(entry);
    }

    for (std::size_t i = 0; i < spec.trailing_bytes; ++i) {
        push(static_cast<std::uint8_t>(0xAB));
    }
    return out;
}

/// The first `keep` bytes, for truncation tests.
[[nodiscard]] inline std::vector<std::byte> truncate_pex(const std::vector<std::byte>& file,
                                                         std::size_t keep) {
    return std::vector<std::byte>(file.begin(),
                                  file.begin() + static_cast<std::ptrdiff_t>(
                                                     keep < file.size() ? keep : file.size()));
}

// ---- whole scripts ----------------------------------------------------------
// A small assembler: strings are collected into the table as they are used, and
// each object's size is computed from what is written after it.

/// A value: type byte and its payload (see script/pex.hpp).
struct PexArg {
    std::uint8_t type = 0;
    std::string text;
    std::uint32_t bits = 0;
};
[[nodiscard]] inline PexArg none() { return {}; }
[[nodiscard]] inline PexArg ident(std::string name) {
    return {.type = 1, .text = std::move(name), .bits = 0};
}
[[nodiscard]] inline PexArg str(std::string text) {
    return {.type = 2, .text = std::move(text), .bits = 0};
}
[[nodiscard]] inline PexArg integer(std::int32_t v) {
    return {.type = 3, .text = {}, .bits = static_cast<std::uint32_t>(v)};
}
[[nodiscard]] inline PexArg floating(float v) {
    std::uint32_t bits = 0;
    static_assert(sizeof bits == sizeof v);
    std::memcpy(&bits, &v, sizeof bits);
    return {.type = 4, .text = {}, .bits = bits};
}
[[nodiscard]] inline PexArg boolean(bool v) {
    return {.type = 5, .text = {}, .bits = v ? 1U : 0U};
}

struct PexInstructionSpec {
    std::uint8_t op = 0;
    std::vector<PexArg> args;
    /// For callmethod (0x17), callparent (0x18) and callstatic (0x19): written
    /// as an integer count and the values.
    std::vector<PexArg> varargs;
};

struct PexFunctionSpec {
    std::string name;
    std::string return_type{"None"};
    std::uint8_t flags = 0; ///< Bit 0 global, bit 1 native.
    std::vector<std::pair<std::string, std::string>> params; ///< name, type
    std::vector<std::pair<std::string, std::string>> locals;
    std::vector<PexInstructionSpec> code;
    std::vector<std::uint16_t> lines; ///< Debug line per instruction, or empty.
};

struct PexVariableSpec {
    std::string name;
    std::string type;
    PexArg initial;
};

struct PexPropertySpec {
    std::string name;
    std::string type;
    std::string auto_var; ///< Auto property backed by this variable.
};

struct PexStateSpec {
    std::string name; ///< Empty for the default state.
    std::vector<PexFunctionSpec> functions;
};

struct PexObjectSpec {
    std::string name;
    std::string parent;
    std::string auto_state;
    std::vector<PexVariableSpec> variables;
    std::vector<PexPropertySpec> properties;
    std::vector<PexStateSpec> states;
};

struct PexScriptSpec {
    std::string source_file{"Fixture.psc"};
    std::vector<PexObjectSpec> objects;
    bool debug_info = true;
    /// Declared object size minus the real one, for corrupt-size tests.
    std::int32_t size_error = 0;
};

/// A script with one empty object.
[[nodiscard]] inline PexScriptSpec empty_script(std::string name, std::string parent = "") {
    PexScriptSpec spec;
    PexObjectSpec object;
    object.name = std::move(name);
    object.parent = std::move(parent);
    spec.objects.push_back(std::move(object));
    return spec;
}

/// Emit a complete Skyrim `.pex` (big-endian, game 1).
[[nodiscard]] inline std::vector<std::byte> build_script(const PexScriptSpec& spec) {
    std::vector<std::string> table;
    const auto index = [&](const std::string& text) -> std::uint16_t {
        for (std::size_t i = 0; i < table.size(); ++i) {
            if (table[i] == text) {
                return static_cast<std::uint16_t>(i);
            }
        }
        table.push_back(text);
        return static_cast<std::uint16_t>(table.size() - 1);
    };

    using Bytes = std::vector<std::byte>;
    const auto u8 = [](Bytes& b, std::uint8_t v) { b.push_back(static_cast<std::byte>(v)); };
    const auto u16 = [&](Bytes& b, std::uint16_t v) {
        u8(b, static_cast<std::uint8_t>(v >> 8));
        u8(b, static_cast<std::uint8_t>(v & 0xFFu));
    };
    const auto u32 = [&](Bytes& b, std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            u8(b, static_cast<std::uint8_t>((v >> shift) & 0xFFu));
        }
    };
    const auto u64 = [&](Bytes& b, std::uint64_t v) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            u8(b, static_cast<std::uint8_t>((v >> shift) & 0xFFu));
        }
    };
    const auto text = [&](Bytes& b, std::string_view t) {
        u16(b, static_cast<std::uint16_t>(t.size()));
        for (const char c : t) {
            u8(b, static_cast<std::uint8_t>(c));
        }
    };
    const auto value = [&](Bytes& b, const PexArg& v) {
        u8(b, v.type);
        if (v.type == 1 || v.type == 2) {
            u16(b, index(v.text));
        } else if (v.type == 3 || v.type == 4) {
            u32(b, v.bits);
        } else if (v.type == 5) {
            u8(b, static_cast<std::uint8_t>(v.bits));
        }
    };
    const auto function = [&](Bytes& b, const PexFunctionSpec& f) {
        u16(b, index(f.return_type));
        u16(b, index(""));
        u32(b, 0);
        u8(b, f.flags);
        u16(b, static_cast<std::uint16_t>(f.params.size()));
        for (const auto& [name, type] : f.params) {
            u16(b, index(name));
            u16(b, index(type));
        }
        u16(b, static_cast<std::uint16_t>(f.locals.size()));
        for (const auto& [name, type] : f.locals) {
            u16(b, index(name));
            u16(b, index(type));
        }
        u16(b, static_cast<std::uint16_t>(f.code.size()));
        for (const auto& ins : f.code) {
            u8(b, ins.op);
            for (const auto& a : ins.args) {
                value(b, a);
            }
            if (ins.op >= 0x17 && ins.op <= 0x19) {
                value(b, integer(static_cast<std::int32_t>(ins.varargs.size())));
                for (const auto& a : ins.varargs) {
                    value(b, a);
                }
            }
        }
    };

    // Objects first, so every string they use is in the table.
    Bytes objects;
    Bytes debug;
    std::uint16_t debug_count = 0;
    u16(objects, static_cast<std::uint16_t>(spec.objects.size()));
    for (const auto& o : spec.objects) {
        Bytes body;
        u16(body, index(o.parent));
        u16(body, index(""));
        u32(body, 0);
        u16(body, index(o.auto_state));
        u16(body, static_cast<std::uint16_t>(o.variables.size()));
        for (const auto& v : o.variables) {
            u16(body, index(v.name));
            u16(body, index(v.type));
            u32(body, 0);
            value(body, v.initial);
        }
        u16(body, static_cast<std::uint16_t>(o.properties.size()));
        for (const auto& p : o.properties) {
            u16(body, index(p.name));
            u16(body, index(p.type));
            u16(body, index(""));
            u32(body, 0);
            u8(body, 0x4); // auto
            u16(body, index(p.auto_var));
        }
        u16(body, static_cast<std::uint16_t>(o.states.size()));
        for (const auto& state : o.states) {
            u16(body, index(state.name));
            u16(body, static_cast<std::uint16_t>(state.functions.size()));
            for (const auto& f : state.functions) {
                u16(body, index(f.name));
                function(body, f);
                if (!f.lines.empty()) {
                    u16(debug, index(o.name));
                    u16(debug, index(state.name));
                    u16(debug, index(f.name));
                    u8(debug, 0);
                    u16(debug, static_cast<std::uint16_t>(f.lines.size()));
                    for (const auto line : f.lines) {
                        u16(debug, line);
                    }
                    ++debug_count;
                }
            }
        }
        u16(objects, index(o.name));
        u32(objects, static_cast<std::uint32_t>(static_cast<std::int64_t>(body.size()) + 4 +
                                                spec.size_error));
        objects.insert(objects.end(), body.begin(), body.end());
    }

    Bytes out;
    u32(out, 0xFA57'C0DEu);
    u8(out, 3);
    u8(out, 2);
    u16(out, 1);
    u64(out, 0);
    text(out, spec.source_file);
    text(out, "tester");
    text(out, "BUILDBOX");
    index("");
    u16(out, static_cast<std::uint16_t>(table.size()));
    for (const auto& entry : table) {
        text(out, entry);
    }
    u8(out, spec.debug_info ? 1 : 0);
    if (spec.debug_info) {
        u64(out, 0);
        u16(out, debug_count);
        out.insert(out.end(), debug.begin(), debug.end());
    }
    u16(out, 0); // user flags
    out.insert(out.end(), objects.begin(), objects.end());
    return out;
}

} // namespace bethconv::testing
