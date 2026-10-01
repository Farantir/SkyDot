// SPDX-License-Identifier: GPL-3.0-or-later
//
// The full PEX reader. Layout per UESP's compiled script file format page,
// checked by decoding all 14,302 vanilla SE scripts with no bytes left over.
#include "bethconv/script/pex.hpp"

#include <cstdio>
#include <map>
#include <tuple>
#include <utility>

namespace bethconv::script {

namespace {

constexpr PexOpInfo k_ops[k_pex_op_count] = {
    {"nop", 0, false},
    {"iadd", 3, false},
    {"fadd", 3, false},
    {"isub", 3, false},
    {"fsub", 3, false},
    {"imul", 3, false},
    {"fmul", 3, false},
    {"idiv", 3, false},
    {"fdiv", 3, false},
    {"imod", 3, false},
    {"not", 2, false},
    {"ineg", 2, false},
    {"fneg", 2, false},
    {"assign", 2, false},
    {"cast", 2, false},
    {"cmp_eq", 3, false},
    {"cmp_lt", 3, false},
    {"cmp_le", 3, false},
    {"cmp_gt", 3, false},
    {"cmp_ge", 3, false},
    {"jmp", 1, false},
    {"jmpt", 2, false},
    {"jmpf", 2, false},
    {"callmethod", 3, true},  // name, self, result
    {"callparent", 2, true},  // name, result
    {"callstatic", 3, true},  // type, name, result
    {"return", 1, false},
    {"strcat", 3, false},
    {"propget", 3, false},    // name, object, result
    {"propset", 3, false},    // name, object, value
    {"array_create", 2, false},
    {"array_length", 2, false},
    {"array_getelement", 3, false},
    {"array_setelement", 3, false},
    {"array_findelement", 4, false},
    {"array_rfindelement", 4, false},
};

/// Big-endian reads that keep the first failure; every read after it is a
/// no-op returning false.
class Reader {
public:
    explicit Reader(io::SpanReader& in) : in_{in} {}

    template <typename T>
    bool read(T& dst) {
        if (failure_) {
            return false;
        }
        auto value = in_.get<T>();
        if (!value) {
            failure_ = std::move(value).error();
            return false;
        }
        if constexpr (sizeof(T) > 1) {
            dst = static_cast<T>(std::byteswap(static_cast<std::make_unsigned_t<T>>(*value)));
        } else {
            dst = *value;
        }
        return true;
    }

    bool text(std::string& dst) {
        std::uint16_t length{};
        if (!read(length)) {
            return false;
        }
        auto chars = in_.chars(length);
        if (!chars) {
            failure_ = std::move(chars).error();
            return false;
        }
        dst = *chars;
        return true;
    }

    bool skip(std::size_t n) {
        if (failure_) {
            return false;
        }
        if (auto skipped = in_.skip(n); !skipped) {
            failure_ = std::move(skipped).error();
            return false;
        }
        return true;
    }

    bool fail(io::ErrorKind kind, std::string detail) {
        if (!failure_) {
            failure_ = in_.fail(kind, std::move(detail)).error();
        }
        return false;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return in_.remaining(); }
    [[nodiscard]] std::size_t position() const noexcept { return in_.position(); }
    [[nodiscard]] std::optional<io::ParseError>& failure() noexcept { return failure_; }

private:
    io::SpanReader& in_;
    std::optional<io::ParseError> failure_;
};

/// Debug line numbers, keyed by object, state and function name indices and
/// the function kind (0 regular, 1 getter, 2 setter).
using LineKey = std::tuple<std::uint16_t, std::uint16_t, std::uint16_t, std::uint8_t>;

class ScriptReader {
public:
    ScriptReader(Reader& in, PexScript& out) : in_{in}, out_{out} {}

    bool index(std::uint16_t& dst) {
        if (!in_.read(dst)) {
            return false;
        }
        if (dst >= out_.strings.size()) {
            return in_.fail(io::ErrorKind::out_of_range,
                            "string index " + std::to_string(dst) + " of " +
                                std::to_string(out_.strings.size()));
        }
        return true;
    }

    bool value(PexValue& dst) {
        std::uint8_t type{};
        if (!in_.read(type)) {
            return false;
        }
        dst.type = static_cast<PexValueType>(type);
        switch (dst.type) {
        case PexValueType::none:
            dst.data = 0;
            return true;
        case PexValueType::identifier:
        case PexValueType::string: {
            std::uint16_t i{};
            if (!index(i)) {
                return false;
            }
            dst.data = i;
            return true;
        }
        case PexValueType::integer:
        case PexValueType::floating:
            return in_.read(dst.data);
        case PexValueType::boolean: {
            std::uint8_t b{};
            if (!in_.read(b)) {
                return false;
            }
            dst.data = b != 0 ? 1 : 0;
            return true;
        }
        }
        return in_.fail(io::ErrorKind::bad_value, "value type " + std::to_string(type));
    }

    bool name_types(std::vector<PexNameType>& dst) {
        std::uint16_t count{};
        if (!in_.read(count)) {
            return false;
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            auto& entry = dst.emplace_back();
            if (!index(entry.name) || !index(entry.type)) {
                return false;
            }
        }
        return true;
    }

    bool function(PexFunction& f) {
        if (!index(f.return_type) || !index(f.docstring) || !in_.read(f.user_flags) ||
            !in_.read(f.flags) || !name_types(f.params) || !name_types(f.locals)) {
            return false;
        }
        std::uint16_t count{};
        if (!in_.read(count)) {
            return false;
        }
        f.opcodes.reserve(count);
        f.arg_offsets.reserve(static_cast<std::size_t>(count) + 1);
        for (std::uint16_t i = 0; i < count; ++i) {
            std::uint8_t op{};
            if (!in_.read(op)) {
                return false;
            }
            if (op >= k_pex_op_count) {
                return in_.fail(io::ErrorKind::bad_value, "opcode " + std::to_string(op));
            }
            f.opcodes.push_back(static_cast<PexOp>(op));
            f.arg_offsets.push_back(static_cast<std::uint32_t>(f.args.size()));
            const auto& info = k_ops[op];
            for (std::uint8_t a = 0; a < info.fixed_args; ++a) {
                if (!value(f.args.emplace_back())) {
                    return false;
                }
            }
            if (info.varargs) {
                PexValue n;
                if (!value(n)) {
                    return false;
                }
                // Each value takes at least a byte, so a larger count is corrupt.
                if (n.type != PexValueType::integer || n.as_int() < 0 ||
                    static_cast<std::size_t>(n.as_int()) > in_.remaining()) {
                    return in_.fail(io::ErrorKind::bad_value,
                                    std::string(info.name) + " with a bad argument count");
                }
                for (std::int32_t a = 0; a < n.as_int(); ++a) {
                    if (!value(f.args.emplace_back())) {
                        return false;
                    }
                }
            }
        }
        f.arg_offsets.push_back(static_cast<std::uint32_t>(f.args.size()));
        return check_jumps(f);
    }

    /// jmp, jmpt and jmpf take a relative offset (their last argument); the
    /// target may be one past the last instruction.
    bool check_jumps(const PexFunction& f) {
        const auto n = static_cast<std::int64_t>(f.opcodes.size());
        for (std::int64_t i = 0; i < n; ++i) {
            const auto op = f.opcodes[static_cast<std::size_t>(i)];
            if (op != PexOp::jmp && op != PexOp::jmpt && op != PexOp::jmpf) {
                continue;
            }
            const auto& offset = f.args[f.arg_offsets[static_cast<std::size_t>(i) + 1] - 1];
            const std::int64_t target = i + offset.as_int();
            if (offset.type != PexValueType::integer || target < 0 || target > n) {
                return in_.fail(io::ErrorKind::corrupt,
                                "jump at " + std::to_string(i) + " leaves its function");
            }
        }
        return true;
    }

    bool object(PexObject& o) {
        std::uint32_t size{};
        if (!index(o.name) || !in_.read(size)) {
            return false;
        }
        // `size` counts itself and the object's data; checked, not trusted.
        const std::size_t start = in_.position();
        if (!index(o.parent) || !index(o.docstring) || !in_.read(o.user_flags) ||
            !index(o.auto_state)) {
            return false;
        }
        std::uint16_t count{};
        if (!in_.read(count)) {
            return false;
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            auto& v = o.variables.emplace_back();
            if (!index(v.name) || !index(v.type) || !in_.read(v.user_flags) || !value(v.initial)) {
                return false;
            }
        }
        if (!in_.read(count)) {
            return false;
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            auto& p = o.properties.emplace_back();
            if (!index(p.name) || !index(p.type) || !index(p.docstring) ||
                !in_.read(p.user_flags) || !in_.read(p.flags)) {
                return false;
            }
            if ((p.flags & 0x4u) != 0) {
                if (!index(p.auto_var)) {
                    return false;
                }
                continue;
            }
            if ((p.flags & 0x1u) != 0) {
                p.getter.emplace().name = p.name;
                if (!function(*p.getter)) {
                    return false;
                }
                attach_lines(o.name, 0, p.name, 1, *p.getter);
            }
            if ((p.flags & 0x2u) != 0) {
                p.setter.emplace().name = p.name;
                if (!function(*p.setter)) {
                    return false;
                }
                attach_lines(o.name, 0, p.name, 2, *p.setter);
            }
        }
        if (!in_.read(count)) {
            return false;
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            auto& state = o.states.emplace_back();
            std::uint16_t functions{};
            if (!index(state.name) || !in_.read(functions)) {
                return false;
            }
            for (std::uint16_t j = 0; j < functions; ++j) {
                auto& f = state.functions.emplace_back();
                if (!index(f.name) || !function(f)) {
                    return false;
                }
                attach_lines(o.name, state.name, f.name, 0, f);
            }
        }
        const std::size_t consumed = in_.position() - start + sizeof(std::uint32_t);
        if (consumed != size) {
            return in_.fail(io::ErrorKind::corrupt,
                            "object declares " + std::to_string(size) + " bytes, has " +
                                std::to_string(consumed));
        }
        return true;
    }

    std::map<LineKey, std::vector<std::uint16_t>> lines;

private:
    void attach_lines(std::uint16_t object, std::uint16_t state, std::uint16_t name,
                      std::uint8_t kind, PexFunction& f) {
        // Property handlers are listed under the empty state.
        const auto it = lines.find({object, kind == 0 ? state : empty_string(), name, kind});
        if (it != lines.end() && it->second.size() == f.opcodes.size()) {
            f.lines = std::move(it->second);
        }
    }

    std::uint16_t empty_string() const {
        for (std::size_t i = 0; i < out_.strings.size(); ++i) {
            if (out_.strings[i].empty()) {
                return static_cast<std::uint16_t>(i);
            }
        }
        return 0xFFFF;
    }

    Reader& in_;
    PexScript& out_;
};

} // namespace

const PexOpInfo& op_info(PexOp op) noexcept {
    return k_ops[static_cast<std::size_t>(op) < k_pex_op_count ? static_cast<std::size_t>(op) : 0];
}

namespace {

std::string value_text(const PexScript& script, const PexValue& v) {
    switch (v.type) {
    case PexValueType::none:
        return "none";
    case PexValueType::identifier:
        return script.strings[v.data];
    case PexValueType::string:
        return "\"" + script.strings[v.data] + "\"";
    case PexValueType::integer:
        return std::to_string(v.as_int());
    case PexValueType::floating: {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%gf", static_cast<double>(v.as_float()));
        return buffer;
    }
    case PexValueType::boolean:
        return v.data != 0 ? "true" : "false";
    }
    return "?";
}

void disassemble_function(const PexScript& script, const PexFunction& f, std::string_view label,
                          std::string& out) {
    const auto& str = [&](std::uint16_t i) -> const std::string& { return script.strings[i]; };
    out += "    ";
    out += label;
    out += " " + str(f.return_type) + " " + str(f.name) + "(";
    for (std::size_t i = 0; i < f.params.size(); ++i) {
        out += (i != 0 ? ", " : "") + str(f.params[i].type) + " " + str(f.params[i].name);
    }
    out += ")";
    out += f.is_global() ? " global" : "";
    out += f.is_native() ? " native" : "";
    out += "\n";
    for (const auto& local : f.locals) {
        out += "      local " + str(local.type) + " " + str(local.name) + "\n";
    }
    for (std::size_t i = 0; i < f.opcodes.size(); ++i) {
        char head[32];
        std::snprintf(head, sizeof(head), "      %4zu  ", i);
        out += head;
        out += op_info(f.opcodes[i]).name;
        for (auto a = f.arg_offsets[i]; a < f.arg_offsets[i + 1]; ++a) {
            out += (a == f.arg_offsets[i] ? " " : ", ") + value_text(script, f.args[a]);
        }
        if (i < f.lines.size()) {
            out += "  ; line " + std::to_string(f.lines[i]);
        }
        out += "\n";
    }
}

} // namespace

std::string disassemble(const PexScript& script) {
    std::string out;
    const auto& str = [&](std::uint16_t i) -> const std::string& { return script.strings[i]; };
    for (const auto& o : script.objects) {
        out += "object " + str(o.name);
        if (!str(o.parent).empty()) {
            out += " extends " + str(o.parent);
        }
        out += "\n";
        if (!str(o.auto_state).empty()) {
            out += "  auto state " + str(o.auto_state) + "\n";
        }
        for (const auto& v : o.variables) {
            out += "  variable " + str(v.type) + " " + str(v.name) + " = " +
                   value_text(script, v.initial) + "\n";
        }
        for (const auto& p : o.properties) {
            out += "  property " + str(p.type) + " " + str(p.name);
            if ((p.flags & 0x4u) != 0) {
                out += " auto -> " + str(p.auto_var);
            }
            out += "\n";
            if (p.getter) {
                disassemble_function(script, *p.getter, "get", out);
            }
            if (p.setter) {
                disassemble_function(script, *p.setter, "set", out);
            }
        }
        for (const auto& state : o.states) {
            out += "  state " + (str(state.name).empty() ? std::string("(default)") : str(state.name)) +
                   "\n";
            for (const auto& f : state.functions) {
                disassemble_function(script, f, "function", out);
            }
        }
    }
    return out;
}

io::ParseResult<PexScript> read_pex_script(std::span<const std::byte> bytes,
                                           std::string_view origin) {
    auto info = parse_pex(bytes, origin);
    if (!info) {
        return std::unexpected(std::move(info).error());
    }
    io::SpanReader span(bytes, origin);
    if (!info->convertible() || !info->big_endian) {
        return span.fail(io::ErrorKind::unsupported,
                         "compiled for " + std::string(to_string(info->game)) + ", not skyrim");
    }
    PexScript out;
    out.major_version = info->major_version;
    out.minor_version = info->minor_version;
    out.compilation_time = info->compilation_time;
    out.source_file = info->source_file;

    Reader in(span);
    ScriptReader script(in, out);
    // The header again, to reach the string table.
    std::uint32_t magic{};
    std::uint16_t game{};
    std::uint8_t version{};
    std::string ignored;
    (void)(in.read(magic) && in.read(version) && in.read(version) && in.read(game) &&
           in.read(out.compilation_time) && in.text(ignored) && in.text(ignored) &&
           in.text(ignored));

    std::uint16_t count{};
    if (in.read(count)) {
        out.strings.reserve(count);
        for (std::uint16_t i = 0; i < count && in.text(out.strings.emplace_back()); ++i) {
        }
    }

    std::uint8_t has_debug{};
    if (in.read(has_debug) && has_debug != 0) {
        out.has_debug_info = true;
        std::uint64_t modified{};
        std::uint16_t functions{};
        if (in.read(modified) && in.read(functions)) {
            for (std::uint16_t i = 0; i < functions; ++i) {
                std::uint16_t object{};
                std::uint16_t state{};
                std::uint16_t name{};
                std::uint8_t kind{};
                std::uint16_t instructions{};
                if (!script.index(object) || !script.index(state) || !script.index(name) ||
                    !in.read(kind) || !in.read(instructions)) {
                    break;
                }
                std::vector<std::uint16_t> lines(instructions);
                for (auto& line : lines) {
                    in.read(line);
                }
                script.lines[{object, state, name, kind}] = std::move(lines);
            }
        }
    }

    if (in.read(count)) {
        for (std::uint16_t i = 0; i < count; ++i) {
            auto& flag = out.user_flags.emplace_back();
            if (!script.index(flag.name) || !in.read(flag.bit)) {
                break;
            }
        }
    }

    if (in.read(count)) {
        for (std::uint16_t i = 0; i < count; ++i) {
            if (!script.object(out.objects.emplace_back())) {
                break;
            }
        }
    }

    if (!in.failure() && in.remaining() != 0) {
        in.fail(io::ErrorKind::corrupt, std::to_string(in.remaining()) + " bytes after the objects");
    }
    if (in.failure()) {
        return std::unexpected(std::move(*in.failure()));
    }
    return out;
}

} // namespace bethconv::script
