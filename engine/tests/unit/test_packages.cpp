// SPDX-License-Identifier: GPL-3.0-or-later
//
// AI packages (world/packages.cpp) as docs/ai.md describes them: which package
// an actor runs at a time of day ("Which package runs") and what a package
// does ("What a package does", the procedure tree flattened into steps). The
// world.fb is built in memory (support/world_builder.hpp); conditions are
// answered by a host the test fills in. Everything goes through the public
// API of world/packages.hpp.
#include "support/world_builder.hpp"

#include "world/packages.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace skydot::testing;
namespace ai = skydot::ai;

namespace {

/// Hands conditions the values a test sets. A function it was not given is 0,
/// as for the engine's host.
class FakeHost final : public ai::ConditionHost {
public:
    std::map<std::uint16_t, double> values;
    std::map<std::uint32_t, double> globals;
    /// An actor's linked reference.
    std::map<std::uint32_t, std::uint32_t> linked;
    /// The subject each function_value call was for, in order.
    std::vector<std::uint32_t> subjects;

    double function_value(const wfb::Condition& c, std::uint32_t subject) override {
        subjects.push_back(subject);
        const auto it = values.find(c.function());
        return it != values.end() ? it->second : 0.0;
    }
    double global_value(std::uint32_t global) override {
        const auto it = globals.find(global);
        return it != globals.end() ? it->second : 0.0;
    }
    std::uint32_t linked_ref(std::uint32_t actor, std::uint32_t) override {
        const auto it = linked.find(actor);
        return it != linked.end() ? it->second : 0;
    }
};

/// Condition functions a test host answers: 1 is 1 (true when compared with
/// 1), 2 is 0 (false).
constexpr std::uint16_t k_true = 1;
constexpr std::uint16_t k_false = 2;

FakeHost host_with_truths() {
    FakeHost host;
    host.values[k_true] = 1.0;
    return host;
}

/// A condition that holds or does not, ORed with the next if `or_next`.
ConditionSpec truth(bool holds, bool or_next = false) {
    return condition(holds ? k_true : k_false, eq, 1.0F, or_next ? k_or : std::uint8_t{0});
}

/// The middle of a minute on a day, as a clock. A clock exactly on a minute
/// can land either side of it once the day's fraction is a double.
ai::Clock at(int day, int hour, int minute) {
    return {.days = day + (hour * 60 + minute + 0.5) / 1440.0};
}

/// The package `spec`, whose id is 1, out of a world of its own.
struct Alone {
    explicit Alone(PackageSpec spec) : world(WorldSpec{.packages = {std::move(spec)}}) {}
    BuiltWorld world;
    [[nodiscard]] const wfb::Package& package() const { return *ai::find_package(*world, 1); }
};

/// Whether a package with this schedule is on at `clock`.
bool active(PackageSpec spec, const ai::Clock& clock) {
    spec.id = 1;
    const Alone alone(std::move(spec));
    return ai::schedule_active(alone.package(), clock);
}

} // namespace

// ---- the game clock ---------------------------------------------------------

TEST_CASE("the clock: time of day, day of the week and the date", "[ai][clock]") {
    // Days passed since the game started, the time of day as the fraction.
    CHECK(ai::Clock{.days = 0.0}.hour() == 0.0);
    CHECK(ai::Clock{.days = 0.25}.hour() == 6.0);
    CHECK(ai::Clock{.days = 3.75}.hour() == 18.0);

    // A new game starts on the 17th of Last Seed, a Morndas (docs/ai.md lists
    // the weekday among its guesses); weeks have seven days and Sundas is 0.
    const ai::Clock start{.days = 0.5};
    CHECK(start.day_of_week() == 1);
    CHECK(start.month() == 7);
    CHECK(start.date() == 17);
    CHECK(ai::Clock{.days = 5.5}.day_of_week() == 6); // Loredas
    CHECK(ai::Clock{.days = 6.5}.day_of_week() == 0); // Sundas
    CHECK(ai::Clock{.days = 7.5}.day_of_week() == 1);
    CHECK(ai::Clock{.days = 700.5}.day_of_week() == 1);

    // Months have their own lengths; Last Seed has 31 days, and the year 365.
    CHECK(ai::Clock{.days = 14.5}.month() == 7);
    CHECK(ai::Clock{.days = 14.5}.date() == 31);
    CHECK(ai::Clock{.days = 15.5}.month() == 8);
    CHECK(ai::Clock{.days = 15.5}.date() == 1);
    CHECK(ai::Clock{.days = 136.5}.month() == 11);
    CHECK(ai::Clock{.days = 136.5}.date() == 31);
    CHECK(ai::Clock{.days = 137.5}.month() == 0);
    CHECK(ai::Clock{.days = 137.5}.date() == 1);
    CHECK(ai::Clock{.days = 365.5}.month() == 7);
    CHECK(ai::Clock{.days = 365.5}.date() == 17);
}

// ---- schedules --------------------------------------------------------------

TEST_CASE("a package without a start hour runs at any time", "[ai][schedule]") {
    for (const int hour : {0, 5, 12, 23}) {
        CHECK(active({.id = 1}, at(0, hour, 30)));
        // The duration means nothing without a start.
        CHECK(active({.id = 1, .duration = 30}, at(0, hour, 30)));
    }
}

TEST_CASE("the day of the week: one, weekdays, weekends, and the two alternating sets", "[ai][schedule]") {
    // 0 Sundas ... 6 Loredas, 7 weekdays, 8 weekends, 9 Morndas/Middas/Fredas,
    // 10 Tirdas/Turdas; -1 any (docs/ai.md).
    const auto days_of = [](int day_of_week) -> std::vector<int> {
        switch (day_of_week) {
        case -1: return {0, 1, 2, 3, 4, 5, 6};
        case 7: return {1, 2, 3, 4, 5};
        case 8: return {0, 6};
        case 9: return {1, 3, 5};
        case 10: return {2, 4};
        default: return {day_of_week};
        }
    };
    for (const int setting : {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
        const auto wanted = days_of(setting);
        // Two weeks, so it is the day of the week that decides, not the day.
        for (int day = 0; day < 14; ++day) {
            const auto clock = at(day, 12, 0);
            const bool expected = std::ranges::count(wanted, clock.day_of_week()) > 0;
            INFO("day_of_week " << setting << " on day " << day << " (Clock says " << clock.day_of_week() << ")");
            CHECK(active({.id = 1, .day_of_week = static_cast<std::int8_t>(setting)}, clock) == expected);
        }
    }
}

TEST_CASE("a start hour opens a window of its duration, an hour when that is 0", "[ai][schedule]") {
    const PackageSpec two_hours{.id = 1, .hour = 8, .duration = 120};
    CHECK_FALSE(active(two_hours, at(0, 7, 59)));
    CHECK(active(two_hours, at(0, 8, 0)));
    CHECK(active(two_hours, at(0, 9, 59)));
    CHECK_FALSE(active(two_hours, at(0, 10, 0)));
    CHECK_FALSE(active(two_hours, at(0, 20, 0)));

    // The start minute counts, and the window is its duration from there.
    const PackageSpec half_past{.id = 1, .hour = 8, .minute = 30, .duration = 90};
    CHECK_FALSE(active(half_past, at(0, 8, 29)));
    CHECK(active(half_past, at(0, 8, 30)));
    CHECK(active(half_past, at(0, 9, 59)));
    CHECK_FALSE(active(half_past, at(0, 10, 0)));

    // Duration 0: an hour.
    const PackageSpec an_hour{.id = 1, .hour = 14};
    CHECK_FALSE(active(an_hour, at(0, 13, 59)));
    CHECK(active(an_hour, at(0, 14, 0)));
    CHECK(active(an_hour, at(0, 14, 59)));
    CHECK_FALSE(active(an_hour, at(0, 15, 0)));

    // The same on every day.
    CHECK(active(two_hours, at(3, 9, 0)));
    CHECK_FALSE(active(two_hours, at(3, 11, 0)));
}

TEST_CASE("a window may run past midnight", "[ai][schedule]") {
    const PackageSpec night{.id = 1, .hour = 22, .duration = 240}; // 22:00 to 02:00
    CHECK_FALSE(active(night, at(0, 21, 59)));
    CHECK(active(night, at(0, 22, 0)));
    CHECK(active(night, at(0, 23, 59)));
    CHECK(active(night, at(1, 0, 0)));
    CHECK(active(night, at(1, 1, 59)));
    CHECK_FALSE(active(night, at(1, 2, 0)));
    CHECK_FALSE(active(night, at(1, 12, 0)));

    // Starting at midnight needs no wrapping.
    const PackageSpec after_midnight{.id = 1, .hour = 0, .duration = 90};
    CHECK(active(after_midnight, at(2, 0, 0)));
    CHECK(active(after_midnight, at(2, 1, 29)));
    CHECK_FALSE(active(after_midnight, at(2, 1, 30)));
    CHECK_FALSE(active(after_midnight, at(2, 23, 59)));
}

TEST_CASE("a window of a day or more covers the whole day", "[ai][schedule]") {
    for (const std::uint32_t minutes : {1440U, 3000U}) {
        for (const int hour : {0, 6, 12, 23}) {
            CHECK(active({.id = 1, .hour = 6, .duration = minutes}, at(0, hour, 0)));
        }
    }
}

TEST_CASE("a day of the week and a window must both hold", "[ai][schedule]") {
    const PackageSpec weekday_morning{.id = 1, .day_of_week = 7, .hour = 8, .duration = 120};
    int morndas = 0;
    while (at(morndas, 0, 0).day_of_week() != 1) {
        ++morndas;
    }
    CHECK(active(weekday_morning, at(morndas, 8, 30)));
    CHECK_FALSE(active(weekday_morning, at(morndas, 10, 30)));
    // Sundas, six days later.
    CHECK(at(morndas + 6, 0, 0).day_of_week() == 0);
    CHECK_FALSE(active(weekday_morning, at(morndas + 6, 8, 30)));
}

TEST_CASE("a month and a date narrow a schedule to days of the year", "[ai][schedule]") {
    // Day 0 is the 17th of Last Seed (month 7); day 15 is Hearthfire (8) 1st.
    CHECK(active({.id = 1, .month = 7, .date = 17}, at(0, 12, 0)));
    CHECK_FALSE(active({.id = 1, .month = 7, .date = 17}, at(1, 12, 0)));
    CHECK_FALSE(active({.id = 1, .month = 7, .date = 17}, at(365 + 1, 12, 0)));
    CHECK(active({.id = 1, .month = 7, .date = 17}, at(365, 12, 0)));

    // A month alone is every day of it; a date alone is that day of any month.
    CHECK_FALSE(active({.id = 1, .month = 8}, at(14, 12, 0)));
    CHECK(active({.id = 1, .month = 8}, at(15, 12, 0)));
    CHECK(active({.id = 1, .month = 8}, at(44, 12, 0)));
    CHECK_FALSE(active({.id = 1, .month = 8}, at(45, 12, 0)));
    CHECK(active({.id = 1, .date = 1}, at(15, 12, 0)));
    CHECK(active({.id = 1, .date = 1}, at(45, 12, 0)));
    CHECK_FALSE(active({.id = 1, .date = 1}, at(16, 12, 0)));
}

// ---- conditions -------------------------------------------------------------

namespace {

/// Whether a package whose conditions are `list` passes them.
bool passes(std::vector<ConditionSpec> list, FakeHost& host, std::uint32_t actor = 0x14) {
    const Alone alone({.id = 1, .conditions = std::move(list)});
    return ai::conditions_pass(alone.package().conditions(), host, actor);
}

} // namespace

TEST_CASE("no conditions pass", "[ai][conditions]") {
    FakeHost host;
    CHECK(ai::conditions_pass(nullptr, host, 0x14));
    CHECK(passes({}, host));
}

TEST_CASE("a condition compares its function with a value", "[ai][conditions]") {
    FakeHost host;
    host.values[7] = 5.0;
    const auto test = [&](Compare compare, float with) { return passes({condition(7, compare, with)}, host); };

    CHECK(test(eq, 5.0F));
    CHECK_FALSE(test(eq, 6.0F));
    CHECK_FALSE(test(ne, 5.0F));
    CHECK(test(ne, 6.0F));
    CHECK(test(gt, 4.0F));
    CHECK_FALSE(test(gt, 5.0F));
    CHECK(test(ge, 5.0F));
    CHECK_FALSE(test(ge, 6.0F));
    CHECK(test(lt, 6.0F));
    CHECK_FALSE(test(lt, 5.0F));
    CHECK(test(le, 5.0F));
    CHECK_FALSE(test(le, 4.0F));
}

TEST_CASE("a condition with the global flag compares with a global's value", "[ai][conditions]") {
    FakeHost host;
    host.values[7] = 3.0;
    host.globals[0x500] = 3.0;
    // The value field is ignored; the global is the comparand.
    ConditionSpec c = condition(7, eq, 99.0F, k_use_global);
    c.value_global = 0x500;
    CHECK(passes({c}, host));
    host.globals[0x500] = 4.0;
    CHECK_FALSE(passes({c}, host));
    c.type = static_cast<std::uint8_t>(lt | k_use_global);
    CHECK(passes({c}, host));
}

TEST_CASE("conditions are ANDed, and a run joined by the OR flag needs one of its members", "[ai][conditions]") {
    auto host = host_with_truths();
    // Docs: "a condition with the OR flag (0x01) joined with the next; the
    // runs of ORs are ANDed."
    CHECK(passes({truth(true)}, host));
    CHECK_FALSE(passes({truth(false)}, host));
    CHECK(passes({truth(true), truth(true)}, host));
    CHECK_FALSE(passes({truth(true), truth(false)}, host));
    CHECK_FALSE(passes({truth(false), truth(true)}, host));
    CHECK(passes({truth(false, true), truth(true)}, host));
    CHECK(passes({truth(true, true), truth(false)}, host));
    CHECK_FALSE(passes({truth(false, true), truth(false)}, host));
    // (false OR true) AND true, and (false OR true) AND false
    CHECK(passes({truth(false, true), truth(true), truth(true)}, host));
    CHECK_FALSE(passes({truth(false, true), truth(true), truth(false)}, host));
    // true AND (false OR true)
    CHECK(passes({truth(true), truth(false, true), truth(true)}, host));
    CHECK_FALSE(passes({truth(true), truth(false, true), truth(false)}, host));
}

TEST_CASE("every grouping of up to four conditions is ANDed runs of ORs", "[ai][conditions]") {
    auto host = host_with_truths();
    // The runs, written out the long way: a run ends at a condition without
    // the OR flag (or at the end), and every run needs one condition that holds.
    const auto expected = [](const std::vector<std::pair<bool, bool>>& list) {
        std::vector<std::vector<bool>> runs;
        std::vector<bool> run;
        for (const auto& [holds, or_next] : list) {
            run.push_back(holds);
            if (!or_next) {
                runs.push_back(run);
                run.clear();
            }
        }
        if (!run.empty()) {
            runs.push_back(run); // a trailing OR closes its run at the end
        }
        return std::ranges::all_of(runs, [](const std::vector<bool>& r) {
            return std::ranges::find(r, true) != r.end();
        });
    };
    for (unsigned count = 1; count <= 4; ++count) {
        for (unsigned holds = 0; holds < (1U << count); ++holds) {
            for (unsigned ors = 0; ors < (1U << count); ++ors) {
                std::vector<std::pair<bool, bool>> list;
                std::vector<ConditionSpec> specs;
                for (unsigned i = 0; i < count; ++i) {
                    list.emplace_back((holds >> i & 1U) != 0, (ors >> i & 1U) != 0);
                    specs.push_back(truth(list.back().first, list.back().second));
                }
                INFO(count << " conditions, truths " << holds << ", ORs " << ors);
                CHECK(passes(specs, host) == expected(list));
            }
        }
    }
}

TEST_CASE("a condition asks about the actor, a reference, or the actor's linked reference", "[ai][conditions]") {
    FakeHost host;
    host.linked[0x14] = 0x333;
    auto on = [](std::uint32_t run_on, std::uint32_t reference = 0) {
        ConditionSpec c = condition(7, ge, 0.0F);
        c.run_on = run_on;
        c.reference = reference;
        return c;
    };
    // Subject, target (a package has none: the actor), a reference, the linked
    // reference, and combat target (not known: no subject).
    CHECK(passes({on(k_on_subject), on(k_on_target), on(k_on_reference, 0x77), on(k_on_linked_ref),
                  on(k_on_combat_target)},
                 host, 0x14));
    CHECK(host.subjects == std::vector<std::uint32_t>{0x14, 0x14, 0x77, 0x333, 0});
}

// ---- packages and templates -------------------------------------------------

TEST_CASE("packages are found by id", "[ai][packages]") {
    const BuiltWorld world(
        WorldSpec{.packages = {{.id = 30, .editor_id = "Thirty"}, {.id = 5}, {.id = 20}, {.id = 10}}});
    for (const std::uint32_t id : {5U, 10U, 20U, 30U}) {
        const auto* p = ai::find_package(*world, id);
        REQUIRE(p != nullptr);
        CHECK(p->id() == id);
    }
    CHECK(ai::find_package(*world, 1) == nullptr);
    CHECK(ai::find_package(*world, 25) == nullptr);
    CHECK(ai::find_package(*world, 31) == nullptr);

    CHECK(ai::resolve_package(*world, 30).editor_id() == "Thirty");
    CHECK(ai::resolve_package(*world, 5).editor_id().empty());
    const auto none = ai::resolve_package(*world, 99);
    CHECK_FALSE(none);
    CHECK(none.chain.empty());
}

TEST_CASE("a package runs its template's tree with its own inputs over the template's", "[ai][packages]") {
    const BuiltWorld world(WorldSpec{
        .packages = {
            {.id = 20,
             .type = 19,
             .inputs = {input(1, "Float", 30.0F), input(2, "Bool", 1.0F)},
             .tree = sequence({procedure("Sandbox"), procedure("Wait")})},
            {.id = 10, .template_id = 20, .inputs = {input(1, "Float", 5.0F)}},
            {.id = 11, .tree = procedure("Travel")},
        }});

    const auto pkg = ai::resolve_package(*world, 10);
    REQUIRE(pkg);
    CHECK(pkg.own->id() == 10);
    // The tree is the template's; inputs are looked up in the package, then the template.
    REQUIRE(pkg.tree != nullptr);
    CHECK(pkg.tree->id() == 20);
    REQUIRE(pkg.chain.size() == 2);
    CHECK(pkg.chain[0]->id() == 10);
    CHECK(pkg.chain[1]->id() == 20);
    CHECK(pkg.number(1) == 5.0);   // its own wins
    CHECK(pkg.number(2) == 1.0);   // the template's
    CHECK(pkg.number(3) == 0.0);   // neither: the fallback
    CHECK(pkg.number(3, 9.5) == 9.5);
    CHECK(pkg.input(3) == nullptr);
    REQUIRE(pkg.input(1) != nullptr);
    CHECK(pkg.input(1)->number() == 5.0F);

    // A package with a tree of its own is its own tree.
    const auto own = ai::resolve_package(*world, 11);
    CHECK(own.tree == own.own);
    CHECK(own.chain.size() == 1);
}

TEST_CASE("a template chain that loops ends", "[ai][packages]") {
    const BuiltWorld world(WorldSpec{.packages = {{.id = 10, .template_id = 11}, {.id = 11, .template_id = 10}}});
    const auto pkg = ai::resolve_package(*world, 10);
    REQUIRE(pkg);
    CHECK(pkg.tree == nullptr);
    CHECK(pkg.chain.size() <= 16);
    FakeHost host;
    CHECK(ai::plan_steps(pkg, host, 0x14, [] { return 0.0; }).empty());
}

TEST_CASE("a tree repeats when its root says so", "[ai][packages]") {
    BranchSpec repeating = sequence({procedure("Travel")});
    repeating.flags = wfb::BranchFlags::repeat_when_complete;
    const BuiltWorld world(WorldSpec{.packages = {{.id = 1, .tree = repeating},
                                                  {.id = 2, .tree = sequence({procedure("Travel")})},
                                                  {.id = 3}}});
    CHECK(ai::repeats(ai::resolve_package(*world, 1)));
    CHECK_FALSE(ai::repeats(ai::resolve_package(*world, 2)));
    CHECK_FALSE(ai::repeats(ai::resolve_package(*world, 3)));
}

TEST_CASE("a completed package stays done for its window, an hour without one", "[ai][packages]") {
    const auto done = [](PackageSpec spec) {
        spec.id = 1;
        const Alone alone(std::move(spec));
        return ai::done_minutes(alone.package());
    };
    CHECK(done({.id = 1, .hour = 8, .duration = 90}) == 90.0);
    CHECK(done({.id = 1, .hour = 8}) == 60.0);
    CHECK(done({.id = 1, .duration = 90}) == 60.0);
    CHECK(done({.id = 1}) == 60.0);
}

TEST_CASE("procedures are known by their record names, the rest do nothing", "[ai][packages]") {
    using ai::Procedure;
    const std::vector<std::pair<const char*, Procedure>> known{
        {"Travel", Procedure::travel},
        {"Sandbox", Procedure::sandbox},
        {"Wander", Procedure::wander},
        {"Sleep", Procedure::sleep},
        {"Sit", Procedure::sit},
        {"Eat", Procedure::eat},
        {"UseIdleMarker", Procedure::use_idle_marker},
        {"Activate", Procedure::activate},
        {"HoldPosition", Procedure::hold_position},
        {"Guard", Procedure::guard},
        {"Patrol", Procedure::patrol},
        {"Find", Procedure::find},
        {"Wait", Procedure::wait},
        {"UnlockDoors", Procedure::unlock_doors},
        {"LockDoors", Procedure::lock_doors},
    };
    std::vector<std::string> names{ai::procedure_name(Procedure::other)};
    for (const auto& [name, procedure] : known) {
        INFO(name);
        CHECK(ai::procedure_of(name) == procedure);
        names.emplace_back(ai::procedure_name(procedure));
    }
    // Follow, Escort, UseWeapon and the like have no handling: the actor stays put.
    for (const char* name : {"Follow", "Escort", "UseWeapon", "", "travel"}) {
        CHECK(ai::procedure_of(name) == Procedure::other);
    }
    // Each has a name of its own for logs.
    std::ranges::sort(names);
    CHECK(std::ranges::adjacent_find(names) == names.end());
    CHECK(names.front() != "");
}

// ---- which package runs -----------------------------------------------------

namespace {

constexpr std::uint32_t k_actor = 0x14;

/// An NPC with `packages` and `default_packages`, in a world with `others`.
WorldSpec world_of(std::vector<PackageSpec> packages, std::vector<std::uint32_t> list = {},
                   std::vector<std::uint32_t> defaults = {}) {
    return {.npcs = {{.id = 100, .packages = std::move(list), .default_packages = std::move(defaults)}},
            .packages = std::move(packages)};
}

} // namespace

TEST_CASE("the first package whose schedule covers the clock runs: the NPC's list, then its default list",
          "[ai][choose]") {
    FakeHost host;
    const BuiltWorld world(world_of({{.id = 10, .hour = 8, .duration = 240}, // 08:00 to 12:00
                                     {.id = 11, .hour = 12, .duration = 360}, // 12:00 to 18:00
                                     {.id = 12}},                            // always
                                    {10, 11}, {12}));
    const auto choose = [&](const ai::Clock& clock) { return ai::choose_package(*world, 100, k_actor, clock, host); };

    CHECK(choose(at(0, 9, 0)) == 10);
    CHECK(choose(at(0, 13, 0)) == 11);
    // Neither of the NPC's own covers the night: the default list does.
    CHECK(choose(at(0, 20, 0)) == 12);
    CHECK(choose(at(0, 7, 59)) == 12);
}

TEST_CASE("a package whose conditions fail is passed over", "[ai][choose]") {
    auto host = host_with_truths();
    const BuiltWorld world(world_of({{.id = 10, .conditions = {truth(false)}},
                                     {.id = 11, .conditions = {truth(false, true), truth(true)}},
                                     {.id = 12, .conditions = {truth(true)}}},
                                    {10, 11}, {12}));
    // 10 fails; 11 passes (false OR true); so 11 is first.
    CHECK(ai::choose_package(*world, 100, k_actor, at(0, 9, 0), host) == 11);

    const BuiltWorld only_defaults(world_of({{.id = 10, .conditions = {truth(false)}}, {.id = 12}}, {10}, {12}));
    CHECK(ai::choose_package(*only_defaults, 100, k_actor, at(0, 9, 0), host) == 12);
}

TEST_CASE("the actor is the subject of a package's conditions", "[ai][choose]") {
    FakeHost host;
    const BuiltWorld world(world_of({{.id = 10, .conditions = {condition(7, ge, 0.0F)}}}, {10}));
    CHECK(ai::choose_package(*world, 100, 0xAB, at(0, 9, 0), host) == 10);
    CHECK(host.subjects == std::vector<std::uint32_t>{0xAB});
}

TEST_CASE("a package's own conditions can read its data inputs (GetNumericPackageData)", "[ai][choose]") {
    FakeHost host;
    // Package 10 runs template 20, whose input 4 is 0; 11 sets it to 1. Both
    // are gated on input 4 being 1.
    const auto gated = condition(ai::k_get_numeric_package_data, eq, 1.0F, 0, 4);
    const BuiltWorld world(world_of(
        {{.id = 20, .type = 19, .inputs = {input(4, "Bool", 0.0F)}},
         {.id = 10, .conditions = {gated}, .template_id = 20},
         {.id = 11, .conditions = {gated}, .template_id = 20, .inputs = {input(4, "Bool", 1.0F)}},
         {.id = 12}},
        {10, 11, 12}));
    CHECK(ai::choose_package(*world, 100, k_actor, at(0, 9, 0), host) == 11);
    // The package answered itself; the host was not asked.
    CHECK(host.subjects.empty());
}

TEST_CASE("packages an actor has completed are skipped, and unknown ones", "[ai][choose]") {
    FakeHost host;
    const BuiltWorld world(world_of({{.id = 10}, {.id = 11}, {.id = 12}}, {99, 10, 11}, {12}));
    const auto choose = [&](const std::function<bool(std::uint32_t)>& skip) {
        return ai::choose_package(*world, 100, k_actor, at(0, 9, 0), host, skip);
    };
    CHECK(choose({}) == 10);
    CHECK(choose([](std::uint32_t id) { return id == 10; }) == 11);
    CHECK(choose([](std::uint32_t id) { return id != 12; }) == 12);
    CHECK(choose([](std::uint32_t) { return true; }) == 0);

    // The same over a list that is already made.
    const std::vector<std::uint32_t> list{12, 11};
    CHECK(ai::choose_package(*world, list, k_actor, at(0, 9, 0), host) == 12);
    CHECK(ai::choose_package(*world, std::vector<std::uint32_t>{}, k_actor, at(0, 9, 0), host) == 0);
}

TEST_CASE("no package that applies gives 0", "[ai][choose]") {
    FakeHost host;
    const BuiltWorld world(world_of({{.id = 10, .hour = 8, .duration = 60}}, {10}));
    CHECK(ai::choose_package(*world, 100, k_actor, at(0, 12, 0), host) == 0);
    // An NPC that is none.
    CHECK(ai::choose_package(*world, 555, k_actor, at(0, 8, 30), host) == 0);
}

TEST_CASE("the template flags lead to the NPC whose packages count", "[ai][choose]") {
    // A's template is B. The "AI packages" flag takes the PKID list from B;
    // the "default package list" flag takes DPLT from B too.
    constexpr auto k_packages = wfb::NpcTemplateFlags::use_ai_packages;
    constexpr auto k_defaults = wfb::NpcTemplateFlags::use_package_list;
    const auto list_for = [&](wfb::NpcTemplateFlags flags) {
        const BuiltWorld world(WorldSpec{
            .npcs = {{.id = 1, .template_id = 2, .template_flags = flags, .packages = {10}, .default_packages = {20}},
                     {.id = 2, .packages = {11}, .default_packages = {21}}},
            .packages = {{.id = 10}, {.id = 11}, {.id = 20}, {.id = 21}}});
        return ai::package_list(*world, 1, k_actor);
    };
    using List = std::vector<std::uint32_t>;
    CHECK(list_for(wfb::NpcTemplateFlags::NONE) == List{10, 20});
    CHECK(list_for(k_packages) == List{11, 20});
    CHECK(list_for(k_defaults) == List{10, 21});
    CHECK(list_for(k_packages | k_defaults) == List{11, 21});
    // Traits and the like do not move packages.
    CHECK(list_for(wfb::NpcTemplateFlags::use_traits | wfb::NpcTemplateFlags::use_inventory) == List{10, 20});
}

TEST_CASE("a list names each package once, in order", "[ai][choose]") {
    const BuiltWorld world(WorldSpec{.npcs = {{.id = 1, .packages = {10, 11, 10}, .default_packages = {11, 12, 10}}},
                                     .packages = {{.id = 10}, {.id = 11}, {.id = 12}}});
    CHECK(ai::package_list(*world, 1, k_actor) == std::vector<std::uint32_t>{10, 11, 12});
    CHECK(ai::package_list(*world, 2, k_actor).empty());
}

TEST_CASE("an NPC from a leveled list of NPCs has the packages of the one picked for its actor", "[ai][choose]") {
    const BuiltWorld world(WorldSpec{
        .npcs = {{.id = 1, .template_id = 50, .template_flags = wfb::NpcTemplateFlags::use_ai_packages, .packages = {10}},
                 {.id = 2, .packages = {11}},
                 {.id = 3, .packages = {12}}},
        .leveled_lists = {{.id = 50, .type = k_lvln, .entries = {{1, 2}, {1, 3}}}},
        .packages = {{.id = 10}, {.id = 11}, {.id = 12}}});
    std::vector<std::uint32_t> seen;
    for (std::uint32_t actor = 0x1000; actor < 0x1040; ++actor) {
        const auto list = ai::package_list(*world, 1, actor);
        REQUIRE(list.size() == 1);
        CHECK((list[0] == 11 || list[0] == 12));
        // The same actor always gets the same one.
        CHECK(ai::package_list(*world, 1, actor) == list);
        seen.push_back(list[0]);
    }
    CHECK(std::ranges::count(seen, 11U) > 0);
    CHECK(std::ranges::count(seen, 12U) > 0);
}

// ---- what a package does ----------------------------------------------------

namespace {

/// A package (id 1 unless told) out of `packages`, ready to be planned.
struct Planner {
    explicit Planner(std::vector<PackageSpec> packages, std::uint32_t id = 1)
        : world(WorldSpec{.packages = std::move(packages)}), package(ai::resolve_package(*world, id)) {}

    BuiltWorld world;
    ai::Package package;
    FakeHost host = host_with_truths();

    [[nodiscard]] std::vector<ai::Step> steps(const ai::Dice& dice = [] { return 0.0; }) {
        return ai::plan_steps(package, host, k_actor, dice);
    }
};

/// The procedures of `steps` as the record names them.
std::vector<std::string> names(const std::vector<ai::Step>& steps) {
    std::vector<std::string> out;
    for (const auto& s : steps) {
        out.push_back(s.name);
    }
    return out;
}

using Names = std::vector<std::string>;

} // namespace

TEST_CASE("a sequence runs its children in order", "[ai][plan]") {
    Planner p({{.id = 1, .tree = sequence({procedure("Travel"), procedure("Sandbox"), procedure("Sleep")})}});
    const auto steps = p.steps();
    REQUIRE(steps.size() == 3);
    CHECK(names(steps) == Names{"Travel", "Sandbox", "Sleep"});
    CHECK(steps[0].procedure == ai::Procedure::travel);
    CHECK(steps[1].procedure == ai::Procedure::sandbox);
    CHECK(steps[2].procedure == ai::Procedure::sleep);
}

TEST_CASE("a tree of one procedure is one step; no tree is none", "[ai][plan]") {
    CHECK(names(Planner({{.id = 1, .tree = procedure("Sleep")}}).steps()) == Names{"Sleep"});
    CHECK(Planner({{.id = 1}}).steps().empty());
    CHECK(Planner({{.id = 1, .tree = sequence({})}}).steps().empty());
}

TEST_CASE("a branch whose conditions fail is left out, with what is under it", "[ai][plan]") {
    // Only the failing child goes; its siblings stay.
    Planner some({{.id = 1,
                   .tree = sequence({procedure("Travel", {}, {truth(false)}), procedure("Sandbox"),
                                     sequence({procedure("Sit"), procedure("Eat")}, {truth(false)}),
                                     procedure("Sleep", {}, {truth(true)})})}});
    CHECK(names(some.steps()) == Names{"Sandbox", "Sleep"});

    // A failing root leaves nothing.
    Planner none({{.id = 1, .tree = sequence({procedure("Travel")}, {truth(false)})}});
    CHECK(none.steps().empty());
    Planner leaf({{.id = 1, .tree = procedure("Travel", {}, {truth(false)})}});
    CHECK(leaf.steps().empty());

    // The same rules for the grouping of conditions as for a package's.
    Planner or_run({{.id = 1, .tree = procedure("Travel", {}, {truth(false, true), truth(true)})}});
    CHECK(names(or_run.steps()) == Names{"Travel"});
}

TEST_CASE("a stacked node runs the first child whose conditions pass", "[ai][plan]") {
    const auto pick = [](bool a, bool b, bool c) {
        Planner p({{.id = 1,
                    .tree = stacked({procedure("Travel", {}, {truth(a)}), procedure("Sandbox", {}, {truth(b)}),
                                     procedure("Sleep", {}, {truth(c)})})}});
        return names(p.steps());
    };
    CHECK(pick(true, true, true) == Names{"Travel"});
    CHECK(pick(false, true, true) == Names{"Sandbox"});
    CHECK(pick(false, false, true) == Names{"Sleep"});
    CHECK(pick(false, false, false).empty());

    // A child that is a sequence brings all its steps.
    Planner nested({{.id = 1,
                     .tree = stacked({sequence({procedure("Travel"), procedure("Wait")}, {truth(false)}),
                                      sequence({procedure("Sit"), procedure("Eat")})})}});
    CHECK(names(nested.steps()) == Names{"Sit", "Eat"});
}

TEST_CASE("a random node runs one of the children whose conditions pass, by the dice", "[ai][plan]") {
    Planner p({{.id = 1,
                .tree = random_of({procedure("Travel"), procedure("Sandbox", {}, {truth(false)}),
                                   procedure("Sleep"), procedure("Eat")})}});
    // Three are open (the second fails); the dice, uniform in [0, 1), picks among them.
    const auto with = [&](double roll) { return names(p.steps([roll] { return roll; })); };
    CHECK(with(0.0) == Names{"Travel"});
    CHECK(with(0.32) == Names{"Travel"});
    CHECK(with(0.34) == Names{"Sleep"});
    CHECK(with(0.66) == Names{"Sleep"});
    CHECK(with(0.67) == Names{"Eat"});
    CHECK(with(0.999) == Names{"Eat"});

    Planner none({{.id = 1, .tree = random_of({procedure("Travel", {}, {truth(false)})})}});
    CHECK(none.steps().empty());
}

TEST_CASE("a simultaneous node: finds first, then the first procedure that moves, for as long as the waits say",
          "[ai][plan]") {
    const std::vector<InputSpec> inputs{input(2, "Float", 30.0F), input(3, "ObjectList")};
    const auto plan = [&](std::vector<BranchSpec> children) {
        Planner p({{.id = 1, .inputs = inputs, .tree = simultaneous(std::move(children))}});
        return p.steps();
    };

    const auto steps = plan({procedure("Wait", {2}), procedure("Travel"), procedure("Find", {3})});
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].procedure == ai::Procedure::find);
    CHECK(steps[0].list_key == 3);
    CHECK(steps[1].procedure == ai::Procedure::travel);
    CHECK(steps[1].seconds == 30.0);

    // Only the first of the moving ones runs.
    CHECK(names(plan({procedure("Travel"), procedure("Sandbox")})) == Names{"Travel"});
    CHECK(plan({procedure("Travel"), procedure("Sandbox")})[0].seconds == 0.0);
    // A wait alone is a wait.
    const auto wait = plan({procedure("Wait", {2})});
    REQUIRE(wait.size() == 1);
    CHECK(wait[0].procedure == ai::Procedure::wait);
    CHECK(wait[0].seconds == 30.0);
}

TEST_CASE("branch conditions are tested when the plan is made", "[ai][plan]") {
    Planner p({{.id = 1, .tree = sequence({procedure("Travel"), procedure("Sandbox", {}, {truth(true)})})}});
    CHECK(names(p.steps()) == Names{"Travel", "Sandbox"});
    // The host's answer decides each time a plan is made...
    p.host.values[k_true] = 0.0;
    CHECK(names(p.steps()) == Names{"Travel"});
    // ...and the actor is who they are asked about.
    Planner who({{.id = 1, .tree = procedure("Travel", {}, {condition(7, ge, 0.0F)})}});
    CHECK_FALSE(who.steps().empty());
    CHECK(who.host.subjects == std::vector<std::uint32_t>{k_actor});
}

TEST_CASE("GetNumericPackageData in a tree reads the package's own inputs", "[ai][plan]") {
    // Sandbox's template: Travel, then UnlockDoors only if "Unlock On
    // Arrival?" (input 4, a Bool) is set. The template's is 0.
    const auto unlock_if_set = condition(ai::k_get_numeric_package_data, eq, 1.0F, 0, 4);
    const PackageSpec sandbox{.id = 20,
                              .type = 19,
                              .inputs = {input(1, "Location"), input(4, "Bool", 0.0F)},
                              .tree = sequence({procedure("Travel", {1}),
                                                procedure("UnlockDoors", {}, {unlock_if_set})})};
    Planner unset({sandbox, {.id = 10, .template_id = 20}}, 10);
    CHECK(names(unset.steps()) == Names{"Travel"});
    Planner set({sandbox, {.id = 10, .template_id = 20, .inputs = {input(4, "Bool", 1.0F)}}}, 10);
    CHECK(names(set.steps()) == Names{"Travel", "UnlockDoors"});
    // The package answers; the host is not asked.
    CHECK(set.host.subjects.empty());
    CHECK(unset.host.subjects.empty());
}

TEST_CASE("a step takes its place, target, object list and seconds from the inputs its procedure names",
          "[ai][plan]") {
    const std::vector<InputSpec> inputs{input(1, "Location"),   input(2, "SingleRef"), input(3, "ObjectList"),
                                        input(5, "Float", 30.0F), input(6, "Float", 7.0F), input(7, "TargetSelector")};
    Planner p({{.id = 1,
                .inputs = inputs,
                .tree = sequence({procedure("Travel", {1, 2}),         // a place and a target
                                  procedure("Sit", {3, 7, 2}),         // a list; the first target of two
                                  procedure("Wait", {5, 6}),           // its seconds
                                  procedure("Sandbox", {255, 9, 6})})}}); // no key, no such input, a Float
    const auto steps = p.steps();
    REQUIRE(steps.size() == 4);

    CHECK(steps[0].location == p.package.input(1));
    CHECK(steps[0].target == p.package.input(2));
    CHECK(steps[0].list_key == -1);
    CHECK(steps[0].seconds == 0.0);

    CHECK(steps[1].location == nullptr);
    CHECK(steps[1].list_key == 3);
    CHECK(steps[1].target == p.package.input(7));

    // Only a Wait takes its seconds from a Float, the first one.
    CHECK(steps[2].procedure == ai::Procedure::wait);
    CHECK(steps[2].seconds == 30.0);

    CHECK(steps[3].location == nullptr);
    CHECK(steps[3].target == nullptr);
    CHECK(steps[3].list_key == -1);
    CHECK(steps[3].seconds == 0.0);
}

TEST_CASE("a step reads the package's inputs before its template's", "[ai][plan]") {
    Planner p({{.id = 20, .type = 19, .inputs = {input(5, "Float", 30.0F)}, .tree = procedure("Wait", {5})},
               {.id = 1, .template_id = 20, .inputs = {input(5, "Float", 8.0F)}}});
    const auto steps = p.steps();
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].seconds == 8.0);
}

namespace {

/// Answers a function 1 the first time it is asked and 0 every time after,
/// as GetRandomPercent's rolls come out differently each time.
class FlakyHost final : public ai::ConditionHost {
public:
    int asked = 0;
    double function_value(const wfb::Condition&, std::uint32_t) override { return asked++ == 0 ? 1.0 : 0.0; }
    double global_value(std::uint32_t) override { return 0.0; }
    std::uint32_t linked_ref(std::uint32_t, std::uint32_t) override { return 0; }
};

std::vector<ai::Step> plan_with(FlakyHost& host, BranchSpec tree) {
    const BuiltWorld world(WorldSpec{.packages = {{.id = 1, .tree = std::move(tree)}}});
    return ai::plan_steps(ai::resolve_package(*world, 1), host, k_actor, [] { return 0.0; });
}

} // namespace

TEST_CASE("a sequence and a stacked node ask each branch's conditions once", "[ai][plan]") {
    const auto flaky = condition(k_true, eq, 1.0F);
    FlakyHost in_sequence;
    CHECK(names(plan_with(in_sequence, sequence({procedure("Travel", {}, {flaky})}))) == Names{"Travel"});
    CHECK(in_sequence.asked == 1);
    FlakyHost in_stack;
    CHECK(names(plan_with(in_stack, stacked({procedure("Travel", {}, {flaky})}))) == Names{"Travel"});
    CHECK(in_stack.asked == 1);
}

// A Random node asks each child's conditions to find the open ones; the picked
// child is then expanded without asking again, as docs/ai.md says, since a
// condition like GetRandomPercent can answer differently the second time.
TEST_CASE("a random node asks each child's conditions once", "[ai][plan]") {
    FlakyHost host;
    const auto steps = plan_with(host, random_of({procedure("Travel", {}, {condition(k_true, eq, 1.0F)})}));
    CHECK(names(steps) == Names{"Travel"});
    CHECK(host.asked == 1);
}
