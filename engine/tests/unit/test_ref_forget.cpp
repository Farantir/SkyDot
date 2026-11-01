// SPDX-License-Identifier: GPL-3.0-or-later
//
// The forget registry (state/ref_forget.hpp) and what the VM keeps per reference. Forgetting an id must leave no
// store holding anything for it: 10,000 references with scripts, timers, threads, inventory and scheduled calls
// come and go, and every store reports what it held before.
#include "support/vm_rig.hpp"
#include "state/id_ranges.hpp"
#include "state/ref_forget.hpp"

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)
#include <malloc.h>
#endif

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::ForgetRegistry;
using skydot::vm::Vm;
using skydot::vm::Op;

namespace {

std::map<std::string, std::size_t> sizes_of(const ForgetRegistry& registry) {
    std::map<std::string, std::size_t> out;
    for (const auto& [name, size] : registry.sizes()) {
        out[name] = size;
    }
    return out;
}

/// Holder: Run() makes an array, keeps it and `self` in variables and returns. Waiter: the same, then waits.
/// Both register for updates from OnInit-less code (the test registers the timer).
void add_scripts(Rig& rig) {
    const std::vector<VariableSpec> vars{{"list", "Int[]"}, {"me", "Holder"}};
    auto run = method("Run");
    run.add(Op::array_create, {ident("list"), integer(4)})
        .add(Op::array_setelement, {ident("list"), integer(0), integer(10)})
        .add(Op::assign, {ident("me"), ident("self")});
    rig.add(script("Holder", {run, method("OnUpdate")}, vars));

    auto wait = method("Run");
    wait.add(Op::array_create, {ident("list"), integer(4)}).add(Op::assign, {ident("me"), ident("self")});
    call_static(wait, "Util", "Wait", {floating(100000.0F)});
    rig.add(script("Waiter", {wait}, vars));
}

} // namespace

namespace {
// Bytes the allocator hands out, where the C library tells (glibc on Linux); 0 elsewhere, so the growth check
// passes trivially there and the store sizes carry the test.
std::size_t heap_in_use() {
#if defined(__linux__)
    return mallinfo2().uordblks;
#else
    return 0;
#endif
}
} // namespace

TEST_CASE("a registered store is forgotten with the id, and a token unregisters it", "[vm][forget]") {
    ForgetRegistry registry;
    std::set<std::uint32_t> a{1, 2, 3};
    std::set<std::uint32_t> b{2};
    registry.add_owned("a", [&](std::uint32_t id) { a.erase(id); }, [&] { return a.size(); });
    {
        auto token = registry.add("b", [&](std::uint32_t id) { b.erase(id); }, [&] { return b.size(); });
        registry.forget(2);
        CHECK(a == std::set<std::uint32_t>{1, 3});
        CHECK(b.empty());
        CHECK(sizes_of(registry) == std::map<std::string, std::size_t>{{"a", 2}, {"b", 0}});
        auto moved = std::move(token); // the token moves, the registration stays
        CHECK(registry.count() == 2);
    }
    CHECK(registry.count() == 1); // the token's end unregistered "b"
    b = {5};
    registry.forget(5);
    CHECK(b == std::set<std::uint32_t>{5}); // no longer called
}

TEST_CASE("a token that outlives its registry is harmless", "[vm][forget]") {
    ForgetRegistry::Registration token;
    {
        ForgetRegistry registry;
        token = registry.add("gone", [](std::uint32_t) {});
    }
    token.reset();
    SUCCEED();
}

TEST_CASE("10,000 references with scripts, timers, threads and inventory leave every store as it was", "[vm][forget]") {
    Rig rig;
    add_scripts(rig);
    auto& vm = rig.vm;
    skydot::vm::IdAllocator ids{skydot::vm::id_ranges::runtime_refs};
    const auto baseline = sizes_of(vm.forgets());
    REQUIRE(baseline.contains("vm.instances"));
    REQUIRE(baseline.contains("vm.inventory"));
    REQUIRE(baseline.contains("vm.animation_states"));
    REQUIRE(baseline.contains("vm.scheduled"));
    const std::size_t instances = vm.instance_count();

    const auto one = [&](int i) {
        const std::uint32_t ref = ids.allocate();
        REQUIRE(ref != 0);
        const bool waits = i % 100 == 0;
        auto* instance = vm.attach(ref, waits ? "waiter" : "holder");
        REQUIRE(instance != nullptr);
        REQUIRE(vm.call_method(instance, "run", {}) != 0);
        vm.register_update(instance, 5.0, 5.0);
        vm.inventory().add(ref, 0x1000, 3);
        vm.set_animation_state(ref, {1, 2, 3});
        vm.schedule(10.0, "activate", ref);
        vm.update(0.0);
        vm.forgets().forget(ref);
        vm.update(0.01);
    };
    for (int i = 0; i < 1000; ++i) {
        one(i);
    }
    const auto warm = heap_in_use();
    for (int i = 1000; i < 11000; ++i) {
        one(i);
    }

    CHECK(sizes_of(vm.forgets()) == baseline);
    CHECK(vm.instance_count() == instances);
    CHECK(vm.thread_count() == 0); // the waiting threads belonged to forgotten instances
    CHECK(vm.scheduled_count() == 0);
    CHECK(vm.animation_registrations() == 0);
    // The detached instances went too: no thread or variable reaches them.
    CHECK(vm.detached_count() == 0);
    const auto grown = static_cast<long long>(heap_in_use()) - static_cast<long long>(warm);
    CHECK(grown < 1024 * 1024);
}

namespace {

/// Keeper: Take(h) keeps `h` in a variable and in an array; Drop() lets both go. It can keep a Holder (the
/// variables are typed by Link, which Holder is not: the chain test uses Links only).
void add_keeper(Rig& rig, const std::string& type = "Holder", const std::string& name = "Keeper") {
    auto take = method("Take", {{"h", type}});
    take.add(Op::assign, {ident("kept"), ident("h")})
        .add(Op::array_create, {ident("list"), integer(2)})
        .add(Op::array_setelement, {ident("list"), integer(0), ident("h")});
    auto drop = method("Drop");
    drop.add(Op::assign, {ident("kept"), none()}).add(Op::assign, {ident("list"), none()});
    rig.add(script(name, {take, drop}, {{"kept", type}, {"list", type + "[]"}}));
}

} // namespace

TEST_CASE("a detached instance stays while a live script holds it and goes when it lets go", "[vm][forget]") {
    Rig rig;
    add_scripts(rig);
    add_keeper(rig);
    auto& vm = rig.vm;
    auto* keeper = vm.attach(0x2000, "keeper");
    auto* holder = vm.attach(0x3000, "holder");
    REQUIRE(keeper != nullptr);
    REQUIRE(holder != nullptr);
    rig.call(keeper, "take", {skydot::vm::Value::object(0x3000, holder->cls, holder)});

    vm.forgets().forget(0x3000);
    vm.update(0.0);
    CHECK(vm.detached_count() == 1); // the variable and the array point at it
    CHECK(vm.collect_detached() == 0);

    rig.call(keeper, "drop");
    vm.update(0.0); // (an update looks again only now and then; a collection at a quiet moment is at once)
    CHECK(vm.collect_detached() == 1);
    CHECK(vm.detached_count() == 0);
}

TEST_CASE("a detached instance is reached through another detached one that is reached", "[vm][forget]") {
    Rig rig;
    add_keeper(rig, "Link", "Link");
    auto& vm = rig.vm;
    auto* live = vm.attach(0x1000, "link");
    auto* middle = vm.attach(0x2000, "link");
    auto* last = vm.attach(0x3000, "link");
    rig.call(middle, "take", {skydot::vm::Value::object(0x3000, last->cls, last)});
    rig.call(live, "take", {skydot::vm::Value::object(0x2000, middle->cls, middle)});

    vm.forgets().forget(0x2000);
    vm.forgets().forget(0x3000);
    vm.update(0.0);
    CHECK(vm.detached_count() == 2); // live -> middle -> last
    rig.call(live, "drop");
    CHECK(vm.collect_detached() == 2);
    CHECK(vm.detached_count() == 0);
}

TEST_CASE("a thread that has not been reaped keeps its instance, a hook's value too", "[vm][forget]") {
    Rig rig;
    add_scripts(rig);
    auto& vm = rig.vm;
    auto* waiter = vm.attach(0x4000, "waiter");
    REQUIRE(vm.call_method(waiter, "run", {}) != 0);
    vm.update(0.0); // Run waits
    REQUIRE(vm.thread_count() == 1);

    vm.forgets().forget(0x4000);
    // The thread is marked done but its frames are still there until the next update reaps it.
    CHECK(vm.detached_count() == 1);
    CHECK(vm.collect_detached() == 0);
    skydot::vm::Value held = skydot::vm::Value::object(0x4000, waiter->cls, waiter);
    vm.add_root([&](const Vm::RootVisitor& visit) { visit(held); });
    vm.update(0.0); // reaps the thread; the hook still shows the instance
    CHECK(vm.thread_count() == 0);
    CHECK(vm.detached_count() == 1);
    held = skydot::vm::Value{};
    CHECK(vm.collect_detached() == 1);
    CHECK(vm.detached_count() == 0);
}
