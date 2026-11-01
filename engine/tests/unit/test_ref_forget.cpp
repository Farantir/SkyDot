// SPDX-License-Identifier: GPL-3.0-or-later
//
// The forget registry (vm/ref_forget.hpp) and what the VM keeps per reference. Forgetting an id must leave no
// store holding anything for it: 10,000 references with scripts, timers, threads, inventory and scheduled calls
// come and go, and every store reports what it held before.
#include "support/vm_rig.hpp"
#include "vm/id_ranges.hpp"
#include "vm/ref_forget.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::ForgetRegistry;
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
    for (int i = 1000; i < 11000; ++i) {
        one(i);
    }

    CHECK(sizes_of(vm.forgets()) == baseline);
    CHECK(vm.instance_count() == instances);
    CHECK(vm.thread_count() == 0); // the waiting threads belonged to forgotten instances
    CHECK(vm.scheduled_count() == 0);
    CHECK(vm.animation_registrations() == 0);
}
