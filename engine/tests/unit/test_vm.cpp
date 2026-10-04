// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Papyrus VM's interpreter (vm/vm.cpp) as docs/papyrus.md describes it:
// opcodes, calls, runtime errors, arrays and conversions. Scripts are
// assembled in memory (support/script_builder.hpp) and run on a VM set up as
// SkydotPapyrus sets one up (support/vm_rig.hpp), so this is the interpreter
// without a pack or an editor. Objects, natives and classes are in
// test_vm_objects.cpp, threads and time in test_vm_threads.cpp, saves in
// test_vm_save.cpp.
#include "support/vm_rig.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::Kind;
using skydot::vm::Op;
using skydot::vm::Value;
using Catch::Matchers::ContainsSubstring;

namespace {

/// `name(a: type_a, b: type_b) -> result_type`: one instruction on its
/// parameters, returned.
FunctionSpec binary(const std::string& name, Op op, const std::string& type_a, const std::string& type_b,
                    const std::string& result_type) {
    FunctionSpec f{.name = name,
                   .return_type = result_type,
                   .global = true,
                   .params = {{"a", type_a}, {"b", type_b}},
                   .locals = {{"r", result_type}}};
    f.add(op, {ident("r"), ident("a"), ident("b")}).add(Op::return_, {ident("r")});
    return f;
}

/// `name(a: type) -> result_type`.
FunctionSpec unary(const std::string& name, Op op, const std::string& type, const std::string& result_type) {
    FunctionSpec f{.name = name,
                   .return_type = result_type,
                   .global = true,
                   .params = {{"a", type}},
                   .locals = {{"r", result_type}}};
    f.add(op, {ident("r"), ident("a")}).add(Op::return_, {ident("r")});
    return f;
}

} // namespace

// ---- opcodes ----------------------------------------------------------------

TEST_CASE("integer arithmetic", "[vm][opcodes]") {
    Rig rig;
    rig.add(script("Ints", {binary("add", Op::iadd, "Int", "Int", "Int"),
                            binary("sub", Op::isub, "Int", "Int", "Int"),
                            binary("mul", Op::imul, "Int", "Int", "Int"),
                            binary("div", Op::idiv, "Int", "Int", "Int"),
                            binary("mod", Op::imod, "Int", "Int", "Int"),
                            unary("neg", Op::ineg, "Int", "Int")}));

    CHECK(rig.call("ints", "add", {Int(7), Int(5)}).i == 12);
    CHECK(rig.call("ints", "sub", {Int(7), Int(10)}).i == -3);
    CHECK(rig.call("ints", "mul", {Int(6), Int(7)}).i == 42);
    CHECK(rig.call("ints", "div", {Int(17), Int(5)}).i == 3);
    CHECK(rig.call("ints", "mod", {Int(17), Int(5)}).i == 2);
    CHECK(rig.call("ints", "neg", {Int(9)}).i == -9);
    CHECK(rig.vm.errors() == 0);

    // The result is an Int whatever the operands were: a Float argument is
    // converted to the parameter's type first.
    const auto sum = rig.call("ints", "add", {Float(2.9F), Int(1)});
    CHECK(sum.kind == Kind::integer);
    CHECK(sum.i == 3);
}

TEST_CASE("float arithmetic", "[vm][opcodes]") {
    Rig rig;
    rig.add(script("Floats", {binary("add", Op::fadd, "Float", "Float", "Float"),
                              binary("sub", Op::fsub, "Float", "Float", "Float"),
                              binary("mul", Op::fmul, "Float", "Float", "Float"),
                              binary("div", Op::fdiv, "Float", "Float", "Float"),
                              unary("neg", Op::fneg, "Float", "Float")}));

    CHECK(rig.call("floats", "add", {Float(1.5F), Float(2.25F)}).f == 3.75F);
    CHECK(rig.call("floats", "sub", {Float(1.5F), Float(2.0F)}).f == -0.5F);
    CHECK(rig.call("floats", "mul", {Float(1.5F), Float(4.0F)}).f == 6.0F);
    CHECK(rig.call("floats", "div", {Float(1.0F), Float(4.0F)}).f == 0.25F);
    CHECK(rig.call("floats", "neg", {Float(2.5F)}).f == -2.5F);

    // An Int argument is a Float inside.
    const auto sum = rig.call("floats", "add", {Int(2), Int(3)});
    CHECK(sum.kind == Kind::floating);
    CHECK(sum.f == 5.0F);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("division by zero is an error and gives 0; the script goes on", "[vm][opcodes][errors]") {
    Rig rig;
    rig.add(script("Zero", {binary("idiv", Op::idiv, "Int", "Int", "Int"),
                            binary("imod", Op::imod, "Int", "Int", "Int"),
                            binary("fdiv", Op::fdiv, "Float", "Float", "Float")}));

    CHECK(rig.call("zero", "idiv", {Int(5), Int(0)}).i == 0);
    CHECK(rig.vm.errors() == 1);
    CHECK(rig.call("zero", "imod", {Int(5), Int(0)}).i == 0);
    CHECK(rig.vm.errors() == 2);
    CHECK(rig.call("zero", "fdiv", {Float(5.0F), Float(0.0F)}).f == 0.0F);
    CHECK(rig.vm.errors() == 3);
    REQUIRE(rig.log.size() == 3);
    CHECK_THAT(rig.log[0], ContainsSubstring("division by zero"));
}

TEST_CASE("comparisons: numbers, strings without case, none and objects", "[vm][opcodes]") {
    Rig rig;
    std::vector<FunctionSpec> functions;
    for (const auto& [suffix, type_a, type_b] : {std::tuple{"ii", "Int", "Int"}, {"ff", "Float", "Float"},
                                                  {"if", "Int", "Float"}, {"ss", "String", "String"},
                                                  {"oo", "ObjectReference", "ObjectReference"}}) {
        for (const auto& [name, op] : {std::pair{"eq", Op::cmp_eq}, {"lt", Op::cmp_lt}, {"le", Op::cmp_le},
                                       {"gt", Op::cmp_gt}, {"ge", Op::cmp_ge}}) {
            functions.push_back(binary(std::string(name) + "_" + suffix, op, type_a, type_b, "Bool"));
        }
    }
    rig.add(script("Compare", functions));
    const auto test = [&](const char* fn, Value a, Value b) {
        return rig.call("compare", fn, {std::move(a), std::move(b)}).b;
    };

    CHECK(test("eq_ii", Int(3), Int(3)));
    CHECK_FALSE(test("eq_ii", Int(3), Int(4)));
    CHECK(test("lt_ii", Int(3), Int(4)));
    CHECK_FALSE(test("lt_ii", Int(4), Int(4)));
    CHECK(test("le_ii", Int(4), Int(4)));
    CHECK(test("gt_ii", Int(5), Int(4)));
    CHECK(test("ge_ii", Int(4), Int(4)));
    CHECK_FALSE(test("ge_ii", Int(3), Int(4)));

    CHECK(test("eq_ff", Float(0.5F), Float(0.5F)));
    CHECK(test("lt_ff", Float(0.5F), Float(0.75F)));
    CHECK(test("gt_ff", Float(0.75F), Float(0.5F)));

    // An Int and a Float compare as numbers.
    CHECK(test("eq_if", Int(2), Float(2.0F)));
    CHECK(test("lt_if", Int(1), Float(1.5F)));
    CHECK(test("gt_if", Int(2), Float(1.5F)));

    // Strings ignore case, as Papyrus's do.
    CHECK(test("eq_ss", String("Hello"), String("hELLO")));
    CHECK_FALSE(test("eq_ss", String("Hello"), String("Help")));
    CHECK(test("lt_ss", String("apple"), String("Banana")));
    CHECK(test("gt_ss", String("Cherry"), String("banana")));
    CHECK(test("lt_ss", String("app"), String("apple")));

    // Objects are equal when they are the same form; none equals only none.
    const auto object = [&](std::uint32_t form) { return rig.vm.object(form, "ObjectReference"); };
    CHECK(test("eq_oo", object(0x10), object(0x10)));
    CHECK_FALSE(test("eq_oo", object(0x10), object(0x11)));
    CHECK(test("eq_oo", Value{}, Value{}));
    CHECK_FALSE(test("eq_oo", Value{}, object(0x10)));
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("not, strcat and cast convert as Papyrus does", "[vm][opcodes]") {
    Rig rig;
    rig.add(script("Convert", {unary("not_int", Op::not_, "Int", "Bool"),
                               unary("not_str", Op::not_, "String", "Bool"),
                               binary("cat_if", Op::strcat, "Int", "Float", "String"),
                               binary("cat_bs", Op::strcat, "Bool", "String", "String"),
                               unary("to_float", Op::cast, "Int", "Float"),
                               unary("to_int", Op::cast, "Float", "Int"),
                               unary("to_int_str", Op::cast, "String", "Int"),
                               unary("to_str", Op::cast, "Float", "String"),
                               unary("to_bool", Op::cast, "Int", "Bool")}));

    CHECK(rig.call("convert", "not_int", {Int(0)}).b);
    CHECK_FALSE(rig.call("convert", "not_int", {Int(7)}).b);
    CHECK(rig.call("convert", "not_str", {String("")}).b);
    CHECK_FALSE(rig.call("convert", "not_str", {String("x")}).b);

    // Values are written as Papyrus prints them: six decimals, True/False.
    CHECK(rig.call("convert", "cat_if", {Int(5), Float(1.5F)}).s == "51.500000");
    CHECK(rig.call("convert", "cat_bs", {Bool(true), String("!")}).s == "True!");
    CHECK(rig.call("convert", "cat_bs", {Bool(false), String("!")}).s == "False!");

    CHECK(rig.call("convert", "to_float", {Int(3)}).f == 3.0F);
    CHECK(rig.call("convert", "to_int", {Float(3.9F)}).i == 3);
    CHECK(rig.call("convert", "to_int", {Float(-3.9F)}).i == -3);
    CHECK(rig.call("convert", "to_int_str", {String("42")}).i == 42);
    CHECK(rig.call("convert", "to_str", {Float(0.5F)}).s == "0.500000");
    CHECK(rig.call("convert", "to_bool", {Int(2)}).b);
    CHECK_FALSE(rig.call("convert", "to_bool", {Int(0)}).b);
    CHECK(rig.vm.errors() == 0);
}

// ---- jumps and calls --------------------------------------------------------

TEST_CASE("jumps make loops and branches", "[vm][opcodes][jumps]") {
    // Function Sum(int n) global: 1 + 2 + ... + n
    auto sum = global_function("sum", {{"n", "Int"}}, {{"r", "Int"}, {"i", "Int"}, {"t", "Bool"}}, "Int");
    sum.add(Op::assign, {ident("r"), integer(0)})
        .add(Op::assign, {ident("i"), integer(1)})
        .label("loop")
        .add(Op::cmp_gt, {ident("t"), ident("i"), ident("n")})
        .jmpt(ident("t"), "done")
        .add(Op::iadd, {ident("r"), ident("r"), ident("i")})
        .add(Op::iadd, {ident("i"), ident("i"), integer(1)})
        .jmp("loop")
        .label("done")
        .add(Op::return_, {ident("r")});

    // Function Max(int a, int b) global: the greater, by a forward jump.
    auto max = global_function("max", {{"a", "Int"}, {"b", "Int"}}, {{"t", "Bool"}}, "Int");
    max.add(Op::cmp_gt, {ident("t"), ident("a"), ident("b")})
        .jmpf(ident("t"), "second")
        .add(Op::return_, {ident("a")})
        .label("second")
        .add(Op::return_, {ident("b")});

    Rig rig;
    rig.add(script("Loops", {sum, max}));
    CHECK(rig.call("loops", "sum", {Int(10)}).i == 55);
    CHECK(rig.call("loops", "sum", {Int(100)}).i == 5050);
    // The loop's first test already jumps out.
    CHECK(rig.call("loops", "sum", {Int(0)}).i == 0);
    CHECK(rig.call("loops", "max", {Int(3), Int(9)}).i == 9);
    CHECK(rig.call("loops", "max", {Int(9), Int(3)}).i == 9);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("a function calls a function of its script: arguments in, result out", "[vm][calls]") {
    // Function Add(int a, int b) global
    auto add = global_function("add", {{"a", "Int"}, {"b", "Int"}}, {{"r", "Int"}}, "Int");
    add.add(Op::iadd, {ident("r"), ident("a"), ident("b")}).add(Op::return_, {ident("r")});
    // Function AddThree(int a) global: Add(a, 3), a static call
    auto add_three = global_function("addthree", {{"a", "Int"}}, {{"r", "Int"}}, "Int");
    add_three.add(Op::callstatic, {ident("Calls"), ident("Add"), ident("r"), ident("a"), integer(3)})
        .add(Op::return_, {ident("r")});
    // Function Double(int a): a method
    auto twice = method("twice", {{"a", "Int"}}, {{"r", "Int"}}, "Int");
    twice.add(Op::imul, {ident("r"), ident("a"), integer(2)}).add(Op::return_, {ident("r")});
    // Function Quad(int a): Double(Double(a)), through self
    auto quad = method("quad", {{"a", "Int"}}, {{"t", "Int"}, {"r", "Int"}}, "Int");
    quad.add(Op::callmethod, {ident("Twice"), ident("self"), ident("t"), ident("a")})
        .add(Op::callmethod, {ident("Twice"), ident("self"), ident("r"), ident("t")})
        .add(Op::return_, {ident("r")});

    Rig rig;
    rig.add(script("Calls", {add, add_three, twice, quad}));
    CHECK(rig.call("calls", "addthree", {Int(4)}).i == 7);

    auto* instance = rig.vm.attach(0x20, "calls");
    REQUIRE(instance != nullptr);
    CHECK(rig.call(instance, "quad", {Int(5)}).i == 20);
    // Names are case-insensitive.
    CHECK(rig.call(instance, "QUAD", {Int(1)}).i == 4);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("calls nest on a stack of their own, which is bounded", "[vm][calls]") {
    // Function SumTo(int n) global: n + SumTo(n - 1)
    auto sum_to = global_function("sumto", {{"n", "Int"}}, {{"t", "Bool"}, {"m", "Int"}, {"r", "Int"}}, "Int");
    sum_to.add(Op::cmp_le, {ident("t"), ident("n"), integer(0)})
        .jmpf(ident("t"), "more")
        .add(Op::return_, {integer(0)})
        .label("more")
        .add(Op::isub, {ident("m"), ident("n"), integer(1)})
        .add(Op::callstatic, {ident("Deep"), ident("SumTo"), ident("r"), ident("m")})
        .add(Op::iadd, {ident("r"), ident("r"), ident("n")})
        .add(Op::return_, {ident("r")});
    // Function Forever(int n) global: Forever(n + 1)
    auto forever = global_function("forever", {{"n", "Int"}}, {{"m", "Int"}, {"r", "Int"}}, "Int");
    forever.add(Op::iadd, {ident("m"), ident("n"), integer(1)})
        .add(Op::callstatic, {ident("Deep"), ident("Forever"), ident("r"), ident("m")})
        .add(Op::return_, {ident("r")});

    Rig rig;
    rig.add(script("Deep", {sum_to, forever}));
    CHECK(rig.call("deep", "sumto", {Int(300)}).i == 45150);
    CHECK(rig.vm.errors() == 0);

    // Past the frame limit the call fails with an error and the thread winds
    // down: no native recursion to overflow.
    const auto result = rig.call("deep", "forever", {Int(0)});
    CHECK(result.is_none());
    CHECK(rig.vm.errors() == 1);
    REQUIRE(rig.log.size() == 1);
    CHECK_THAT(rig.log[0], ContainsSubstring("stack overflow"));
}

// ---- runtime errors ---------------------------------------------------------

TEST_CASE("a runtime error is counted and the script goes on", "[vm][errors]") {
    // Each function: note "before", one bad thing, note "after".
    const auto faulty = [](const std::string& name, const std::vector<Instruction>& bad) {
        auto f = method(name, {}, {{"nothing", "ObjectReference"}, {"n", "Int"}, {"list", "Int[]"}});
        f.add(Op::array_create, {ident("list"), integer(2)});
        note(f, "before");
        for (const auto& ins : bad) {
            f.add(ins.op, ins.args);
        }
        note(f, "after");
        return f;
    };
    Rig rig;
    rig.add(script(
        "Faulty",
        {faulty("on_none", {{Op::callmethod, {ident("GetFormID"), ident("nothing"), ident("n")}, {}}}),
         faulty("no_function", {{Op::callmethod, {ident("NoSuchFunction"), ident("self"), ident("n")}, {}}}),
         faulty("no_such_class", {{Op::callstatic, {ident("NoSuchClass"), ident("Run"), ident("n")}, {}}}),
         faulty("property_of_none", {{Op::propget, {ident("Anything"), ident("nothing"), ident("n")}, {}}}),
         faulty("no_property", {{Op::propget, {ident("NoSuchProperty"), ident("self"), ident("n")}, {}}}),
         faulty("index_past_the_end", {{Op::array_getelement, {ident("n"), ident("list"), integer(2)}, {}}}),
         faulty("negative_index", {{Op::array_setelement, {ident("list"), integer(-1), integer(1)}, {}}}),
         faulty("array_too_big", {{Op::array_create, {ident("list"), integer(129)}, {}}}),
         // Missing events, OnBeginState and the like, are not errors.
         faulty("missing_event", {{Op::callmethod, {ident("OnNothing"), ident("self"), ident("n")}, {}}})}));
    auto* instance = rig.vm.attach(0x90, "faulty");
    REQUIRE(instance != nullptr);

    const auto [name, errors] = GENERATE(table<const char*, std::uint64_t>(
        {{"on_none", 1}, {"no_function", 1}, {"no_such_class", 2}, {"property_of_none", 1}, {"no_property", 1},
         {"index_past_the_end", 1}, {"negative_index", 1}, {"array_too_big", 1}, {"missing_event", 0}}));
    INFO(name);
    rig.call(instance, name);
    CHECK(rig.vm.errors() == errors);
    CHECK(rig.log.size() == errors);
    // Whatever went wrong, the function ran to its end.
    CHECK(rig.notes == std::vector<std::string>{"before", "after"});
}

TEST_CASE("an error in one thread does not stop the others", "[vm][errors][threads]") {
    auto faulty = global_function("faulty", {}, {{"nothing", "ObjectReference"}, {"n", "Int"}});
    note(faulty, "f before");
    faulty.add(Op::callmethod, {ident("GetFormID"), ident("nothing"), ident("n")});
    note(faulty, "f after");
    auto healthy = global_function("healthy");
    note(healthy, "h");
    Rig rig;
    rig.add(script("Threads", {faulty, healthy}));

    rig.vm.call_global("threads", "faulty", {});
    rig.vm.call_global("threads", "healthy", {});
    rig.vm.call_global("threads", "faulty", {});
    rig.vm.update(0.0);
    CHECK(rig.vm.errors() == 2);
    CHECK(rig.vm.thread_count() == 0);
    CHECK(rig.notes == std::vector<std::string>{"f before", "f after", "h", "f before", "f after"});
}

TEST_CASE("an error names the script and the line", "[vm][errors]") {
    auto run = global_function("run", {}, {{"nothing", "ObjectReference"}});
    run.add(Op::nop).add(Op::callmethod, {ident("GetFormID"), ident("nothing"), ident("::NoneVar")});
    run.lines = {10, 11};
    Rig rig;
    rig.add(script("Located", {run}));
    rig.call("located", "run");
    REQUIRE(rig.log.size() == 1);
    CHECK_THAT(rig.log[0], ContainsSubstring("Located"));
    CHECK_THAT(rig.log[0], ContainsSubstring("line 11"));
    CHECK_THAT(rig.log[0], ContainsSubstring("None"));
}

// ---- arrays -----------------------------------------------------------------

namespace {

/// Lists: functions around arrays of Int.
ScriptSpec lists() {
    // Function Fill(int[] a) global: a[0] = 42
    auto fill = global_function("fill", {{"a", "Int[]"}});
    fill.add(Op::array_setelement, {ident("a"), integer(0), integer(42)});

    // Function RoundTrip() global: a[0] set by Fill, a[1] set through an alias
    auto round_trip =
        global_function("roundtrip", {}, {{"a", "Int[]"}, {"b", "Int[]"}, {"x", "Int"}, {"y", "Int"}}, "Int");
    round_trip.add(Op::array_create, {ident("a"), integer(3)});
    call_static(round_trip, "Lists", "Fill", {ident("a")});
    round_trip.add(Op::assign, {ident("b"), ident("a")})
        .add(Op::array_setelement, {ident("b"), integer(1), integer(7)})
        .add(Op::array_getelement, {ident("x"), ident("a"), integer(0)})
        .add(Op::array_getelement, {ident("y"), ident("a"), integer(1)})
        .add(Op::imul, {ident("x"), ident("x"), integer(100)})
        .add(Op::iadd, {ident("x"), ident("x"), ident("y")})
        .add(Op::return_, {ident("x")});

    // Function Make() global: [5, 7, 5, 9]
    auto make = global_function("make", {}, {{"a", "Int[]"}}, "Int[]");
    make.add(Op::array_create, {ident("a"), integer(4)});
    int index = 0;
    for (const int value : {5, 7, 5, 9}) {
        make.add(Op::array_setelement, {ident("a"), integer(index++), integer(value)});
    }
    make.add(Op::return_, {ident("a")});

    // Function Find(int[] a, int x, int from) global, RFind likewise, Length
    auto find = global_function("find", {{"a", "Int[]"}, {"x", "Int"}, {"from", "Int"}}, {{"r", "Int"}}, "Int");
    find.add(Op::array_findelement, {ident("a"), ident("r"), ident("x"), ident("from")})
        .add(Op::return_, {ident("r")});
    auto rfind = global_function("rfind", {{"a", "Int[]"}, {"x", "Int"}, {"from", "Int"}}, {{"r", "Int"}}, "Int");
    rfind.add(Op::array_rfindelement, {ident("a"), ident("r"), ident("x"), ident("from")})
        .add(Op::return_, {ident("r")});
    auto length = global_function("length", {{"a", "Int[]"}}, {{"r", "Int"}}, "Int");
    length.add(Op::array_length, {ident("r"), ident("a")}).add(Op::return_, {ident("r")});

    // Function Create(int n) global, for Int and String elements
    auto create = global_function("create", {{"n", "Int"}}, {{"a", "Int[]"}}, "Int[]");
    create.add(Op::array_create, {ident("a"), ident("n")}).add(Op::return_, {ident("a")});
    auto create_text = global_function("createtext", {{"n", "Int"}}, {{"a", "String[]"}}, "String[]");
    create_text.add(Op::array_create, {ident("a"), ident("n")}).add(Op::return_, {ident("a")});

    // Init() makes the variable's array, Poke() writes into it.
    auto init = method("Init");
    init.add(Op::array_create, {ident("list"), integer(2)});
    auto poke = method("Poke");
    poke.add(Op::array_setelement, {ident("list"), integer(0), integer(9)});

    return script("Lists", {fill, round_trip, make, find, rfind, length, create, create_text, init, poke},
                  {{"list", "Int[]"}});
}

} // namespace

TEST_CASE("arrays are shared by reference", "[vm][arrays]") {
    Rig rig;
    rig.add(lists());

    // Fill writes the caller's array; an alias writes the same one.
    CHECK(rig.call("lists", "roundtrip").i == 4207);

    // A native or the host passing an array in gets it changed.
    auto array = std::make_shared<skydot::vm::Array>(3, Int(0));
    rig.call("lists", "fill", {Value::make_array(array)});
    CHECK(array->at(0).i == 42);
    CHECK(array->at(1).i == 0);

    // A script variable's array is the one the script goes on writing to.
    auto* instance = rig.vm.attach(0xA0, "lists");
    REQUIRE(instance != nullptr);
    CHECK(rig.vm.get_variable(instance, "list").is_none());
    rig.call(instance, "init");
    const auto before = rig.vm.get_variable(instance, "list");
    REQUIRE(before.kind == Kind::array);
    REQUIRE(before.array->size() == 2);
    CHECK(before.array->at(0).i == 0);
    rig.call(instance, "poke");
    CHECK(before.array->at(0).i == 9);
    CHECK(rig.vm.get_variable(instance, "list").array == before.array);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("array opcodes: create, length, find from either end", "[vm][arrays]") {
    Rig rig;
    rig.add(lists());

    const auto made = rig.call("lists", "make");
    REQUIRE(made.kind == Kind::array);
    REQUIRE(made.array->size() == 4);
    CHECK(made.array->at(3).i == 9);

    const auto search = [&](const char* fn, int x, int from) {
        return rig.call("lists", fn, {Value::make_array(made.array), Int(x), Int(from)}).i;
    };
    const auto find = [&](int x, int from) { return search("find", x, from); };
    const auto rfind = [&](int x, int from) { return search("rfind", x, from); };
    CHECK(find(5, 0) == 0);
    CHECK(find(5, 1) == 2);
    CHECK(find(9, 0) == 3);
    CHECK(find(8, 0) == -1);
    CHECK(find(5, 3) == -1);
    // A negative start of the reverse search is the last element.
    CHECK(rfind(5, -1) == 2);
    CHECK(rfind(5, 1) == 0);
    CHECK(rfind(7, -1) == 1);
    CHECK(rfind(8, -1) == -1);

    CHECK(rig.call("lists", "length", {made}).i == 4);
    // A None array has no elements.
    CHECK(rig.call("lists", "length", {Value{}}).i == 0);

    // New elements are the type's default.
    const auto ints = rig.call("lists", "create", {Int(3)});
    REQUIRE(ints.kind == Kind::array);
    REQUIRE(ints.array->size() == 3);
    CHECK(ints.array->at(2).kind == Kind::integer);
    CHECK(ints.array->at(2).i == 0);
    const auto texts = rig.call("lists", "createtext", {Int(2)});
    REQUIRE(texts.array->size() == 2);
    CHECK(texts.array->at(1).kind == Kind::string);
    CHECK(texts.array->at(1).s.empty());

    // Papyrus arrays hold up to 128 elements.
    CHECK(rig.call("lists", "create", {Int(128)}).array->size() == 128);
    CHECK(rig.vm.errors() == 0);
    CHECK(rig.call("lists", "create", {Int(129)}).is_none());
    CHECK(rig.call("lists", "create", {Int(-1)}).is_none());
    CHECK(rig.vm.errors() == 2);
}

// ---- values -----------------------------------------------------------------

TEST_CASE("values: defaults, truth, text and conversions", "[vm][values]") {
    Rig rig;
    using skydot::vm::Vm;

    CHECK(Vm::default_for("int").kind == Kind::integer);
    CHECK(Vm::default_for("float").kind == Kind::floating);
    CHECK(Vm::default_for("bool").kind == Kind::boolean);
    CHECK_FALSE(Vm::default_for("bool").b);
    CHECK(Vm::default_for("string").kind == Kind::string);
    CHECK(Vm::default_for("objectreference").is_none());
    CHECK(Vm::default_for("int[]").is_none());

    CHECK_FALSE(Vm::truthy(Value{}));
    CHECK_FALSE(Vm::truthy(Int(0)));
    CHECK(Vm::truthy(Int(-1)));
    CHECK_FALSE(Vm::truthy(Float(0.0F)));
    CHECK(Vm::truthy(Float(0.5F)));
    CHECK_FALSE(Vm::truthy(Bool(false)));
    CHECK_FALSE(Vm::truthy(String("")));
    CHECK(Vm::truthy(String("0")));
    CHECK(Vm::truthy(rig.vm.object(0x10, "ObjectReference")));
    // An array is true when it has elements (docs/papyrus.md).
    CHECK_FALSE(Vm::truthy(Value::make_array(std::make_shared<skydot::vm::Array>())));
    CHECK(Vm::truthy(Value::make_array(std::make_shared<skydot::vm::Array>(1))));

    // Text as Papyrus prints it: six decimals, True and False, [Class <FormID>]
    // (docs/papyrus.md, "Assumptions not checked against the game").
    CHECK(Vm::to_string(Value{}) == "None");
    CHECK(Vm::to_string(Int(-12)) == "-12");
    CHECK(Vm::to_string(Float(1.5F)) == "1.500000");
    CHECK(Vm::to_string(Float(-0.25F)) == "-0.250000");
    CHECK(Vm::to_string(Bool(true)) == "True");
    CHECK(Vm::to_string(Bool(false)) == "False");
    CHECK(Vm::to_string(String("text")) == "text");
    CHECK(Vm::to_string(rig.vm.object(0x1A, "ObjectReference")) == "[ObjectReference <0000001A>]");
    CHECK(Vm::to_string(Value::object(0xABCDEF01, nullptr)) == "[Form <ABCDEF01>]");

    CHECK(rig.vm.cast(Float(3.9F), "int").i == 3);
    CHECK(rig.vm.cast(Bool(true), "int").i == 1);
    CHECK(rig.vm.cast(String("42"), "int").i == 42);
    CHECK(rig.vm.cast(String("nonsense"), "int").i == 0);
    CHECK(rig.vm.cast(Int(2), "float").f == 2.0F);
    CHECK(rig.vm.cast(String("2.5"), "float").f == 2.5F);
    CHECK(rig.vm.cast(String("x"), "bool").b);
    CHECK_FALSE(rig.vm.cast(Int(0), "bool").b);
    CHECK(rig.vm.cast(Int(7), "string").s == "7");
    // Something that is not an array does not become one.
    CHECK(rig.vm.cast(Int(1), "int[]").is_none());
    CHECK(rig.vm.cast(Value{}, "objectreference").is_none());
    CHECK(rig.vm.errors() == 0);
}
