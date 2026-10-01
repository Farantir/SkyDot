// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/vmad.hpp"

#include <optional>
#include <utility>

namespace bethconv::record {

namespace {

/// Reads in sequence and keeps the first failure; every read after it is a
/// no-op returning false.
class Cursor {
public:
    explicit Cursor(io::SpanReader& body) : body_{body} {}

    template <typename T>
    bool read(T& dst) {
        if (failure_) {
            return false;
        }
        auto value = body_.get<T>();
        if (!value) {
            failure_ = std::move(value).error();
            return false;
        }
        dst = *value;
        return true;
    }

    bool read_wstring(std::string& dst) {
        if (failure_) {
            return false;
        }
        auto text = body_.wstring();
        if (!text) {
            failure_ = std::move(text).error();
            return false;
        }
        dst = *text;
        return true;
    }

    bool fail(io::ErrorKind kind, std::string detail) {
        if (!failure_) {
            failure_ = body_.fail(kind, std::move(detail)).error();
        }
        return false;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return body_.remaining(); }
    [[nodiscard]] std::optional<io::ParseError>& failure() noexcept { return failure_; }

private:
    io::SpanReader& body_;
    std::optional<io::ParseError> failure_;
};

bool read_object(Cursor& in, std::int16_t format, ScriptObject& out) {
    std::uint32_t form{};
    std::uint16_t unused{};
    const bool ok = format == 1
                        ? in.read(form) && in.read(out.alias) && in.read(unused)
                        : in.read(unused) && in.read(out.alias) && in.read(form);
    out.form = FormId{form};
    return ok;
}

/// One element of `type`, appended to the matching vector.
bool read_value(Cursor& in, ScriptPropertyType type, std::int16_t format, ScriptProperty& out) {
    switch (type) {
    case ScriptPropertyType::object:
    case ScriptPropertyType::object_array:
        return read_object(in, format, out.objects.emplace_back());
    case ScriptPropertyType::string:
    case ScriptPropertyType::string_array:
        return in.read_wstring(out.strings.emplace_back());
    case ScriptPropertyType::integer:
    case ScriptPropertyType::integer_array:
        return in.read(out.integers.emplace_back());
    case ScriptPropertyType::floating:
    case ScriptPropertyType::floating_array:
        return in.read(out.floats.emplace_back());
    case ScriptPropertyType::boolean:
    case ScriptPropertyType::boolean_array: {
        std::uint8_t value{};
        if (!in.read(value)) {
            return false;
        }
        out.integers.push_back(value != 0 ? 1 : 0);
        return true;
    }
    case ScriptPropertyType::none:
        return true; // No value follows.
    }
    return false;
}

bool known_type(std::uint8_t type) noexcept {
    return type <= 5 || (type >= 11 && type <= 15);
}

bool read_property(Cursor& in, std::int16_t version, std::int16_t format, ScriptProperty& out) {
    std::uint8_t type{};
    if (!in.read_wstring(out.name) || !in.read(type)) {
        return false;
    }
    if (!known_type(type)) {
        return in.fail(io::ErrorKind::bad_value,
                       "VMAD property '" + out.name + "' has type " + std::to_string(type));
    }
    out.type = static_cast<ScriptPropertyType>(type);
    if (version >= 4 && !in.read(out.status)) {
        return false;
    }
    if (!is_array(out.type)) {
        return read_value(in, out.type, format, out);
    }
    std::uint32_t count{};
    if (!in.read(count)) {
        return false;
    }
    // Every element takes at least one byte, so a larger count is corrupt.
    if (count > in.remaining()) {
        return in.fail(io::ErrorKind::truncated,
                       "VMAD array '" + out.name + "' of " + std::to_string(count) +
                           " elements in " + std::to_string(in.remaining()) + " bytes");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!read_value(in, out.type, format, out)) {
            return false;
        }
    }
    return true;
}

bool read_script(Cursor& in, std::int16_t version, std::int16_t format, Script& out) {
    std::uint16_t count{};
    if (!in.read_wstring(out.name) || (version >= 4 && !in.read(out.status)) || !in.read(count)) {
        return false;
    }
    for (std::uint16_t i = 0; i < count; ++i) {
        if (!read_property(in, version, format, out.properties.emplace_back())) {
            return false;
        }
    }
    return true;
}

} // namespace

io::ParseResult<ScriptData> read_script_data(io::SpanReader& body) {
    Cursor in{body};
    ScriptData out;
    std::uint16_t count{};
    if (in.read(out.version) && in.read(out.object_format)) {
        if (out.version < 2 || out.version > 5) {
            in.fail(io::ErrorKind::unsupported, "VMAD version " + std::to_string(out.version));
        } else if (out.object_format != 1 && out.object_format != 2) {
            in.fail(io::ErrorKind::unsupported,
                    "VMAD object format " + std::to_string(out.object_format));
        }
    }
    if (in.read(count)) {
        for (std::uint16_t i = 0; i < count; ++i) {
            if (!read_script(in, out.version, out.object_format, out.scripts.emplace_back())) {
                break;
            }
        }
    }
    if (in.failure()) {
        return std::unexpected(std::move(*in.failure()));
    }
    return out;
}

io::ParseResult<QuestFragments> read_quest_fragments(io::SpanReader& body,
                                                     const ScriptData& scripts) {
    Cursor in{body};
    QuestFragments out;
    std::uint16_t count{};
    if (in.read(out.unknown) && in.read(count) && in.read_wstring(out.script)) {
        for (std::uint16_t i = 0; i < count && !in.failure(); ++i) {
            auto& f = out.fragments.emplace_back();
            in.read(f.stage) && in.read(f.unknown) && in.read(f.log_entry) &&
                in.read(f.unknown2) && in.read_wstring(f.script) && in.read_wstring(f.function);
        }
    }
    std::uint16_t aliases{};
    if (in.read(aliases)) {
        for (std::uint16_t i = 0; i < aliases && !in.failure(); ++i) {
            auto& a = out.aliases.emplace_back();
            std::uint16_t script_count{};
            if (!read_object(in, scripts.object_format, a.alias) || !in.read(a.version) ||
                !in.read(a.object_format) || !in.read(script_count)) {
                break;
            }
            if (a.object_format != 1 && a.object_format != 2) {
                in.fail(io::ErrorKind::unsupported,
                        "VMAD alias object format " + std::to_string(a.object_format));
                break;
            }
            for (std::uint16_t j = 0; j < script_count; ++j) {
                if (!read_script(in, a.version, a.object_format, a.scripts.emplace_back())) {
                    break;
                }
            }
        }
    }
    if (in.failure()) {
        return std::unexpected(std::move(*in.failure()));
    }
    return out;
}

} // namespace bethconv::record
