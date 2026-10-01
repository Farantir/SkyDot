// SPDX-License-Identifier: GPL-3.0-or-later
//
// The full PEX reader and the script asset. Real data is covered by `bethconv
// script` (every vanilla script in three installs and the FUS list decodes);
// these cover every value type, call arguments, jumps, line numbers and the
// ways a file can be wrong.
#include "bethconv/pack/script_asset.hpp"
#include "bethconv/script/pex.hpp"

#include "../support/pex_builder.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace bethconv;
using namespace bethconv::testing;
using script::PexOp;
using script::PexValueType;

namespace {

/// `Scriptname Lever extends ObjectReference` with a variable, an auto
/// property and OnActivate:
///
///     if Target
///         if Target.IsDisabled()
///             Target.Enable(false)
///         else
///             Target.Disable(false)
///         endif
///     endif
PexScriptSpec lever() {
    PexScriptSpec spec = empty_script("Lever", "ObjectReference");
    auto& o = spec.objects[0];
    o.variables.push_back({.name = "::Target_var", .type = "ObjectReference", .initial = none()});
    o.variables.push_back({.name = "Count", .type = "Int", .initial = integer(-3)});
    o.variables.push_back({.name = "Speed", .type = "Float", .initial = floating(1.5F)});
    o.variables.push_back({.name = "Title", .type = "String", .initial = str("pull")});
    o.variables.push_back({.name = "Armed", .type = "Bool", .initial = boolean(true)});
    o.properties.push_back({.name = "Target", .type = "ObjectReference", .auto_var = "::Target_var"});
    PexFunctionSpec f;
    f.name = "OnActivate";
    f.params = {{"akActionRef", "ObjectReference"}};
    f.locals = {{"::temp0", "Bool"}, {"::temp1", "Bool"}, {"::NoneVar", "None"}};
    f.code = {
        {.op = 0x0E, .args = {ident("::temp0"), ident("::Target_var")}, .varargs = {}},
        {.op = 0x16, .args = {ident("::temp0"), integer(6)}, .varargs = {}},
        {.op = 0x17, .args = {ident("IsDisabled"), ident("::Target_var"), ident("::temp1")},
         .varargs = {}},
        {.op = 0x16, .args = {ident("::temp1"), integer(3)}, .varargs = {}},
        {.op = 0x17, .args = {ident("Enable"), ident("::Target_var"), ident("::NoneVar")},
         .varargs = {boolean(false)}},
        {.op = 0x14, .args = {integer(2)}, .varargs = {}},
        {.op = 0x17, .args = {ident("Disable"), ident("::Target_var"), ident("::NoneVar")},
         .varargs = {boolean(false)}},
    };
    f.lines = {3, 3, 4, 4, 5, 5, 7};
    o.states.push_back({.name = "", .functions = {f}});
    return spec;
}

const script::PexFunction& on_activate(const script::PexScript& s) {
    return s.objects.at(0).states.at(0).functions.at(0);
}

} // namespace

TEST_CASE("a whole script decodes: variables, properties, code and lines", "[pex]") {
    const auto bytes = build_script(lever());
    const auto s = script::read_pex_script(bytes, "lever.pex");
    REQUIRE(s.has_value());
    REQUIRE(s->objects.size() == 1);
    const auto& o = s->objects[0];
    CHECK(s->string(o.name) == "Lever");
    CHECK(s->string(o.parent) == "ObjectReference");
    REQUIRE(o.variables.size() == 5);
    CHECK(o.variables[0].initial.type == PexValueType::none);
    CHECK(o.variables[1].initial.as_int() == -3);
    CHECK(o.variables[2].initial.as_float() == 1.5F);
    CHECK(s->string(static_cast<std::uint16_t>(o.variables[3].initial.data)) == "pull");
    CHECK(o.variables[4].initial == script::PexValue{PexValueType::boolean, 1});
    REQUIRE(o.properties.size() == 1);
    CHECK(o.properties[0].flags == 0x4);
    CHECK(s->string(o.properties[0].auto_var) == "::Target_var");

    const auto& f = on_activate(*s);
    CHECK(s->string(f.name) == "OnActivate");
    REQUIRE(f.opcodes.size() == 7);
    CHECK(f.opcodes[4] == PexOp::callmethod);
    // Fixed arguments, then the variable ones without their count.
    CHECK(f.arg_offsets[5] - f.arg_offsets[4] == 4);
    CHECK(f.args[f.arg_offsets[4] + 3] == script::PexValue{PexValueType::boolean, 0});
    CHECK(f.lines == std::vector<std::uint16_t>{3, 3, 4, 4, 5, 5, 7});

    const auto text = script::disassemble(*s);
    CHECK(text.find("callmethod Enable, ::Target_var, ::NoneVar, false  ; line 5") !=
          std::string::npos);
    CHECK(text.find("jmpf ::temp0, 6") != std::string::npos);
}

TEST_CASE("a script asset reads back what was decoded", "[pex][pack]") {
    const auto s = script::read_pex_script(build_script(lever()), "lever.pex");
    REQUIRE(s.has_value());
    const auto asset = pack::write_script_asset(*s);
    const auto back = pack::read_script_asset(asset, "lever.pexfb");
    REQUIRE(back.has_value());
    CHECK(back->strings == s->strings);
    CHECK(script::disassemble(*back) == script::disassemble(*s));

    auto broken = asset;
    broken.resize(broken.size() / 2);
    CHECK_FALSE(pack::read_script_asset(broken, "half.pexfb").has_value());
}

TEST_CASE("a script that is wrong anywhere is refused", "[pex]") {
    const auto refuse = [](const PexScriptSpec& spec) {
        return script::read_pex_script(build_script(spec), "bad.pex");
    };
    SECTION("an unknown opcode") {
        auto spec = lever();
        spec.objects[0].states[0].functions[0].code[0].op = 0x24;
        CHECK(refuse(spec).error().kind == io::ErrorKind::bad_value);
    }
    SECTION("a jump out of its function") {
        auto spec = lever();
        spec.objects[0].states[0].functions[0].code[5].args[0] = integer(3);
        CHECK(refuse(spec).error().kind == io::ErrorKind::corrupt);
        spec.objects[0].states[0].functions[0].code[5].args[0] = integer(-6);
        CHECK(refuse(spec).error().kind == io::ErrorKind::corrupt);
    }
    SECTION("a jump to the end is fine") {
        auto spec = lever();
        spec.objects[0].states[0].functions[0].code[5].args[0] = integer(2);
        CHECK(refuse(spec).has_value());
    }
    SECTION("an unknown value type") {
        auto spec = lever();
        spec.objects[0].variables[1].initial.type = 9;
        CHECK(refuse(spec).error().kind == io::ErrorKind::bad_value);
    }
    SECTION("an object size that disagrees with its content") {
        auto spec = lever();
        spec.size_error = 2;
        CHECK(refuse(spec).error().kind == io::ErrorKind::corrupt);
    }
    SECTION("a Fallout 4 script") {
        const auto fo4 = build_pex(PexSpec{.game_id = 2, .little_endian = true});
        CHECK(script::read_pex_script(fo4, "fo4.pex").error().kind == io::ErrorKind::unsupported);
    }
    SECTION("bytes after the objects") {
        auto bytes = build_script(lever());
        bytes.push_back(std::byte{0});
        CHECK(script::read_pex_script(bytes, "long.pex").error().kind == io::ErrorKind::corrupt);
    }
}

TEST_CASE("a script cut short anywhere fails without reading past the end", "[pex]") {
    const auto bytes = build_script(lever());
    for (std::size_t keep = 0; keep < bytes.size(); ++keep) {
        const std::vector<std::byte> part(bytes.begin(),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(keep));
        INFO("kept " << keep << " of " << bytes.size());
        CHECK_FALSE(script::read_pex_script(part, "cut.pex").has_value());
    }
}

TEST_CASE("a string index past the table is refused", "[pex]") {
    auto bytes = build_script(empty_script("Fixture"));
    // The object's name index is the last-but-18 bytes' first word: find it by
    // rewriting the only object's name index to 0xFFFF.
    const auto s = script::read_pex_script(bytes, "fixture.pex");
    REQUIRE(s.has_value());
    // Objects are last: count (2), name (2), size (4), parent, docstring (2
    // each), user flags (4), auto state (2), three empty counts (6).
    const std::size_t name_at = bytes.size() - 22;
    bytes[name_at] = std::byte{0xFF};
    bytes[name_at + 1] = std::byte{0xFF};
    CHECK(script::read_pex_script(bytes, "fixture.pex").error().kind ==
          io::ErrorKind::out_of_range);
}
