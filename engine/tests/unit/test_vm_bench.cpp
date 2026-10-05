// SPDX-License-Identifier: GPL-3.0-or-later
//
// Timings of the Papyrus VM's interpreter, for judging a change to it. Hidden
// from the normal run; build the release preset and run
//
//   skydot_core_tests "[bench]" --benchmark-samples 30
//
// Each benchmark runs a fixed amount of script work (the loop count is in its
// name) and says what Catch2 measured for the lot.
#include "support/vm_rig.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::Op;
using skydot::vm::Value;

namespace {

constexpr std::int32_t k_loops = 10000;

/// `while (i < n) { body; i += 1 }`, then return `result`.
FunctionSpec counted_loop(const std::string& name, std::vector<Param> locals, const std::string& result,
                          const std::string& return_type, const std::function<void(FunctionSpec&)>& prologue,
                          const std::function<void(FunctionSpec&)>& body) {
    locals.push_back({"i", "Int"});
    locals.push_back({"c", "Bool"});
    auto f = global_function(name, {{"n", "Int"}}, std::move(locals), return_type);
    prologue(f);
    f.add(Op::assign, {ident("i"), integer(0)})
        .label("top")
        .add(Op::cmp_lt, {ident("c"), ident("i"), ident("n")})
        .jmpf(ident("c"), "end");
    body(f);
    f.add(Op::iadd, {ident("i"), ident("i"), integer(1)}).jmp("top").label("end");
    f.add(Op::return_, {result.empty() ? none() : ident(result)});
    return f;
}

/// The same as a method, to run on an instance.
FunctionSpec counted_method(const std::string& name, std::vector<Param> locals, const std::string& result,
                            const std::string& return_type, const std::function<void(FunctionSpec&)>& body) {
    auto f = counted_loop(name, std::move(locals), result, return_type, [](FunctionSpec&) {}, body);
    f.global = false;
    return f;
}

ScriptSpec bench_script() {
    std::vector<FunctionSpec> functions;
    // int_loop(n): acc = acc * 3 + i, a float and a comparison on the way
    functions.push_back(counted_loop(
        "int_loop", {{"acc", "Int"}, {"t", "Int"}, {"x", "Float"}, {"b", "Bool"}}, "acc", "Int",
        [](FunctionSpec& f) { f.add(Op::assign, {ident("acc"), integer(1)}); },
        [](FunctionSpec& f) {
            f.add(Op::imul, {ident("t"), ident("acc"), integer(3)})
                .add(Op::iadd, {ident("acc"), ident("t"), ident("i")})
                .add(Op::cast, {ident("x"), ident("acc")})
                .add(Op::fmul, {ident("x"), ident("x"), floating(0.5F)})
                .add(Op::cmp_gt, {ident("b"), ident("x"), floating(100.0F)});
        }));
    // string_loop(n): concatenations, copies and comparisons of short strings
    functions.push_back(counted_loop(
        "string_loop", {{"s", "String"}, {"t", "String"}, {"u", "String"}, {"b", "Bool"}}, "u", "String",
        [](FunctionSpec& f) { f.add(Op::assign, {ident("s"), str("Skyrim")}); },
        [](FunctionSpec& f) {
            f.add(Op::strcat, {ident("t"), ident("s"), str(" / Whiterun")})
                .add(Op::assign, {ident("u"), ident("t")})
                .add(Op::cmp_eq, {ident("b"), ident("u"), str("skyrim / whiterun")})
                .add(Op::strcat, {ident("t"), str("quest "), ident("i")});
        }));
    // array_loop(n): a 16-element array written, read and measured
    functions.push_back(counted_loop(
        "array_loop", {{"a", "Int[]"}, {"j", "Int"}, {"v", "Int"}, {"len", "Int"}}, "v", "Int",
        [](FunctionSpec& f) { f.add(Op::array_create, {ident("a"), integer(16)}); },
        [](FunctionSpec& f) {
            f.add(Op::imod, {ident("j"), ident("i"), integer(16)})
                .add(Op::array_setelement, {ident("a"), ident("j"), ident("i")})
                .add(Op::array_getelement, {ident("v"), ident("a"), ident("j")})
                .add(Op::array_length, {ident("len"), ident("a")});
        }));
    // native_loop(n): Util.Triple(i)
    functions.push_back(counted_loop("native_loop", {{"t", "Int"}}, "t", "Int", [](FunctionSpec&) {},
                                     [](FunctionSpec& f) {
                                         f.add(Op::callstatic, {ident("Util"), ident("Triple"), ident("t"), ident("i")});
                                     }));
    return script("Bench", functions);
}

/// A script with state, a variable, an auto property and methods, as quest
/// scripts are.
ScriptSpec actor_script() {
    auto bump = method("Bump", {{"by", "Int"}}, {{"t", "Int"}}, "Int");
    bump.add(Op::iadd, {ident("t"), ident("count"), ident("by")})
        .add(Op::assign, {ident("count"), ident("t")})
        .add(Op::return_, {ident("t")});
    // method_loop(n): self.Bump(i), the object seen as its own class
    auto method_loop = counted_method("method_loop", {{"t", "Int"}}, "t", "Int", [](FunctionSpec& f) {
        f.add(Op::callmethod, {ident("Bump"), ident("self"), ident("t"), ident("i")});
    });
    // property_loop(n): Level += i, through the auto property
    auto property_loop = counted_method("property_loop", {{"t", "Int"}}, "t", "Int", [](FunctionSpec& f) {
        f.add(Op::propget, {ident("Level"), ident("self"), ident("t")})
            .add(Op::iadd, {ident("t"), ident("t"), ident("i")})
            .add(Op::propset, {ident("Level"), ident("self"), ident("t")});
    });
    // state_loop(n): GetState() and a method that exists only in the busy
    // state, as scripts that switch states do
    auto get_state = method("GetState", {}, {}, "String");
    get_state.add(Op::return_, {ident("::State")});
    auto busy_only = method("BusyOnly", {}, {}, "Int", "busy");
    busy_only.add(Op::return_, {integer(1)});
    auto state_loop = counted_method("state_loop", {{"t", "Int"}, {"s", "String"}}, "t", "Int", [](FunctionSpec& f) {
        f.add(Op::callmethod, {ident("BusyOnly"), ident("self"), ident("t")})
            .add(Op::callmethod, {ident("GetState"), ident("self"), ident("s")});
    });
    // OnPing(x): the kind of handler an event runs
    auto on_ping = method("OnPing", {{"x", "Int"}}, {{"t", "Int"}}, "None", "busy");
    on_ping.add(Op::iadd, {ident("t"), ident("count"), ident("x")}).add(Op::assign, {ident("count"), ident("t")});
    ScriptSpec spec = script("Actor", {bump, method_loop, property_loop, get_state, busy_only, state_loop, on_ping},
                             {{"count", "Int"}, {"::Level_var", "Int"}});
    spec.properties = {{.name = "Level", .type = "Int", .auto_var = "::Level_var"}};
    spec.auto_state = "busy";
    return spec;
}

} // namespace

TEST_CASE("the interpreter", "[.bench][vm]") {
    Rig rig;
    rig.add(bench_script());
    rig.add(actor_script());
    rig.vm.set_budget(100'000'000);
    auto* instance = rig.vm.attach(0x40, "actor");
    REQUIRE(instance != nullptr);
    REQUIRE(rig.vm.attach(0x41, "actor") != nullptr);

    const auto before = rig.vm.instructions();
    CHECK(rig.call("bench", "int_loop", {Int(k_loops)}).kind() == skydot::vm::Kind::integer);
    CHECK(rig.call("bench", "string_loop", {Int(k_loops)}).s() == "Skyrim / Whiterun");
    CHECK(rig.call("bench", "array_loop", {Int(k_loops)}).i() == k_loops - 1);
    CHECK(rig.call("bench", "native_loop", {Int(k_loops)}).i() == 3 * (k_loops - 1));
    CHECK(rig.call(instance, "method_loop", {Int(k_loops)}).kind() == skydot::vm::Kind::integer);
    CHECK(rig.call(instance, "property_loop", {Int(k_loops)}).kind() == skydot::vm::Kind::integer);
    CHECK(rig.call(instance, "state_loop", {Int(k_loops)}).kind() == skydot::vm::Kind::integer);
    CHECK(rig.vm.errors() == 0);
    WARN("instructions for one pass of everything: " << rig.vm.instructions() - before);

    BENCHMARK("int loop, 10000 iterations") { return rig.call("bench", "int_loop", {Int(k_loops)}); };
    BENCHMARK("string loop, 10000 iterations") { return rig.call("bench", "string_loop", {Int(k_loops)}); };
    BENCHMARK("array loop, 10000 iterations") { return rig.call("bench", "array_loop", {Int(k_loops)}); };
    BENCHMARK("native calls, 10000") { return rig.call("bench", "native_loop", {Int(k_loops)}); };
    BENCHMARK("method calls, 10000") { return rig.call(instance, "method_loop", {Int(k_loops)}); };
    BENCHMARK("property get+set, 10000") { return rig.call(instance, "property_loop", {Int(k_loops)}); };
    BENCHMARK("state lookups, 10000 pairs") { return rig.call(instance, "state_loop", {Int(k_loops)}); };
    BENCHMARK("events, 1000 sent to 2 scripts") {
        for (int n = 0; n < 1000; ++n) {
            rig.vm.send_event(0x40, "OnPing", {Int(n)});
            rig.vm.send_event(0x41, "OnPing", {Int(n)});
        }
        rig.vm.update(0.0);
        return rig.vm.thread_count();
    };
}
