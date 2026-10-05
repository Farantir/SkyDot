// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Papyrus VM's Value (vm/value.hpp): what each kind holds and answers for,
// how copies and moves share, and the name tables lookups use (vm/name.hpp).
#include "support/vm_rig.hpp"

#include "vm/name.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace skydot::testing;
using skydot::vm::Kind;
using skydot::vm::Value;

TEST_CASE("a value holds what its kind needs and answers 0, empty or null for the rest", "[vm][value]") {
    static_assert(sizeof(Value) == 24);

    const Value none;
    CHECK(none.kind() == Kind::none);
    CHECK(none.is_none());

    const Value text = String("Whiterun");
    CHECK(text.kind() == Kind::string);
    CHECK(text.s() == "Whiterun");
    CHECK(text.i() == 0);
    CHECK(text.form() == 0);
    CHECK(text.array() == nullptr);

    const Value number = Int(-7);
    CHECK(number.i() == -7);
    CHECK(number.f() == 0.0F); // the integer's bits are not a float
    CHECK_FALSE(number.b());
    CHECK(number.s().empty());
    CHECK(number.cls() == nullptr);
    CHECK(number.instance() == nullptr);

    const Value real = Float(2.5F);
    CHECK(real.f() == 2.5F);
    CHECK(real.i() == 0);

    const Value yes = Bool(true);
    CHECK(yes.b());
    CHECK(yes.i() == 0);

    // A form that is 0 is no object.
    CHECK(Value::object(0, nullptr).is_none());
    const Value object = Value::object(0x14, nullptr);
    CHECK(object.kind() == Kind::object);
    CHECK(object.form() == 0x14);
    CHECK(object.i() == 0);
    CHECK(Value::make_array(nullptr).is_none());
    CHECK(Value::text(nullptr).is_none());
}

TEST_CASE("copies share strings and arrays, and a moved-from value is None", "[vm][value]") {
    Value a = String("a string that is longer than any small-string buffer holds");
    Value b = a;
    CHECK(&a.s() == &b.s()); // one immutable text
    CHECK(b.s() == a.s());

    Value c = std::move(a);
    CHECK(a.is_none()); // NOLINT(bugprone-use-after-move)
    CHECK(c.s() == b.s());
    CHECK(&c.s() == &b.s());

    auto elements = std::make_shared<skydot::vm::Array>(2);
    Value array = Value::make_array(elements);
    Value same = array;
    (*same.array())[1] = Int(5);
    CHECK((*array.array())[1].i() == 5); // arrays are shared by reference
    CHECK(array.array() == same.array());
    CHECK(elements.use_count() == 3);

    // Assigning over a value of another kind releases what it held.
    same = Int(1);
    CHECK(elements.use_count() == 2);
    array = String("x");
    CHECK(elements.use_count() == 1);
    CHECK(array.s() == "x");
    const Value& itself = array;
    array = itself;
    CHECK(array.s() == "x");
    Value other = Value::object(0x20, nullptr);
    other = std::move(array);
    CHECK(other.s() == "x");
    CHECK(array.is_none()); // NOLINT(bugprone-use-after-move)
}

TEST_CASE("a name table finds, replaces and grows through collisions", "[vm][names]") {
    using skydot::vm::HashTable;
    struct Entry {
        std::string key;
    };
    std::vector<std::unique_ptr<Entry>> entries;
    HashTable<Entry> table;
    const auto same = [](const std::string& key) { return [&key](const Entry& e) { return e.key == key; }; };

    CHECK(table.find(1, same("none")) == nullptr);
    // Four distinct hashes for 500 keys: long probe runs, and a few growths.
    for (int i = 0; i < 500; ++i) {
        entries.push_back(std::make_unique<Entry>(Entry{"key" + std::to_string(i)}));
        table.put(static_cast<std::size_t>(i % 4), entries.back().get(), same(entries.back()->key));
    }
    for (int i = 0; i < 500; ++i) {
        const auto key = "key" + std::to_string(i);
        CHECK(table.find(static_cast<std::size_t>(i % 4), same(key)) == entries[static_cast<std::size_t>(i)].get());
    }
    CHECK(table.find(0, same("key1")) == nullptr); // right key, wrong hash

    // Putting a key again replaces its entry.
    entries.push_back(std::make_unique<Entry>(Entry{"key3"}));
    table.put(3, entries.back().get(), same("key3"));
    CHECK(table.find(3, same("key3")) == entries.back().get());
    CHECK(table.find(3, same("key7")) == entries[7].get());

    CHECK(skydot::vm::Name("onactivate").hash == skydot::vm::name_hash("onactivate"));
    CHECK(skydot::vm::name_hash("a") != skydot::vm::name_hash("b"));
}
