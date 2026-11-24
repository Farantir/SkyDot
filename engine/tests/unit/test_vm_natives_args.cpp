// SPDX-License-Identifier: GPL-3.0-or-later
//
// The argument and result helpers of the natives (vm/natives_args.hpp): a
// missing argument gives the default, a passed one its value even if None.
#include "vm/natives_args.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::natives;
using skydot::vm::NativeCall;
using skydot::vm::Thread;
using skydot::vm::Value;
using skydot::vm::Vm;

TEST_CASE("object arguments give their form, anything else 0", "[vm][natives]") {
    Vm vm;
    Thread thread;
    std::vector<Value> args{Value::object(0x1234, nullptr), Value::integer(7), Value{}};
    NativeCall c{vm, thread, Value{}, args};
    CHECK(is_object(c, 0));
    CHECK(object_form(c, 0) == 0x1234);
    CHECK_FALSE(is_object(c, 1));
    CHECK(object_form(c, 1) == 0);
    CHECK(object_form(c, 2) == 0);
    CHECK_FALSE(is_object(c, 5));
    CHECK(object_form(c, 5) == 0);
}

TEST_CASE("int_or, float_or and bool_or take the default only for a missing argument", "[vm][natives]") {
    Vm vm;
    Thread thread;
    std::vector<Value> args{Value::integer(3), Value::floating(2.5F), Value::integer(4), Value{}};
    NativeCall c{vm, thread, Value{}, args};
    CHECK(int_or(c, 0, 1) == 3);
    CHECK(int_or(c, 3, 1) == 0); // None passed
    CHECK(int_or(c, 4, 1) == 1);
    CHECK(float_or(c, 1, 0.0) == 2.5);
    CHECK(float_or(c, 2, 0.0) == 4.0); // an int counts as its number
    CHECK(float_or(c, 4, 9.0) == 9.0);
    CHECK(bool_or(c, 0, false));
    CHECK_FALSE(bool_or(c, 3, true)); // None is false
    CHECK(bool_or(c, 4, true));
}

TEST_CASE("result and flag do not wait", "[vm][natives]") {
    const auto r = result(Value::integer(5));
    CHECK(r.value.i() == 5);
    CHECK(r.wait < 0.0);
    CHECK(r.wait_for.empty());
    const auto f = flag(true);
    CHECK(f.value.b());
    CHECK(f.wait < 0.0);
}
