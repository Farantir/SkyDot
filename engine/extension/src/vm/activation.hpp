// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who may activate what: the rules `SkydotPapyrus::activate_ref` applies
// before anything runs. Nothing here touches Godot.
//
//   - A reference that answers only to its activate parents refuses the
//     player; a script's Activate and an activate parent reach it.
//   - A locked reference refuses the player unless it picks the lock (the
//     force flag); scripts and parents are not refused, and none of them
//     unlocks it. Only the player's pick does.
//   - The player's use of furniture with a behaviour graph that raises
//     ObjectActivated takes effect when the graph says so: the scripts and
//     activate children wait (PendingActivations). A second use meanwhile is
//     ignored.
//   - A FLOR or TREE with a harvest gives its ingredient once (vm/harvest.hpp)
//     and then answers "already harvested" until it respawns. A FURN with a
//     bench type (workbenches) is crafting, never a lever.
//   - An actor that activates a loose item (an ALCH, AMMO, ARMO, BOOK, INGR,
//     KEYM, MISC, SLGM, SCRL or WEAP reference) picks it up, unless scripts
//     blocked the activation.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydot {

/// The player's reference, which activations come from.
inline constexpr std::uint32_t k_player_ref = 0x14;

enum class ActivationSource { player = 0, script = 1, parent = 2 };

enum class ActivationGate { pass, parent_only, locked };

struct ActivationRequest {
    ActivationSource source{ActivationSource::player};
    bool parent_activate_only{};
    /// The lock level if locked, else -1.
    int lock_level{-1};
    /// The player picks the lock (Shift+F, or the viewer's pick-locks).
    bool force{};
};

[[nodiscard]] ActivationGate activation_gate(const ActivationRequest& request);
/// Whether the activation picks the lock open as it goes.
[[nodiscard]] bool activation_unlocks(const ActivationRequest& request);
/// Whether an activation of a reference of record `type` waits for the
/// graph's ObjectActivated: the player's use of furniture whose graph raises it.
/// A bench (`bench_type` != 0) is never a lever.
[[nodiscard]] bool activation_waits_for_graph(std::string_view type, ActivationSource source, bool graph_raises,
                                              int bench_type = 0);

/// Whether a reference of record `type` is a loose item an activator picks up.
[[nodiscard]] bool activation_picks_up(std::string_view type);

/// Activations waiting for their ObjectActivated, one per reference.
class PendingActivations {
public:
    struct Entry {
        std::int64_t activator{};
        bool default_only{};
    };
    /// False if `ref` is already pending (the use is ignored).
    bool begin(std::uint32_t ref, Entry entry);
    /// Take the entry of `ref`, if it waits.
    std::optional<Entry> take(std::uint32_t ref);
    [[nodiscard]] bool pending(std::uint32_t ref) const { return entries_.count(ref) != 0; }
    void clear() { entries_.clear(); }

private:
    std::map<std::uint32_t, Entry> entries_;
};

/// XLOC's level as the game names it.
[[nodiscard]] std::string lock_level_name(int level);

/// What the rules need to know about the reference being activated.
struct ActivationTarget {
    std::uint32_t base{};
    std::string type;      ///< Record type of the base: "DOOR", "FURN", "MISC", ...
    std::string editor_id; ///< The base's.
    bool parent_activate_only{};
    bool load_door{};
    std::int64_t count{1}; ///< How many a loose item stands for.
    /// A FLOR or TREE that gives an ingredient (a harvest in its base).
    bool harvestable{};
    /// FURN WBDT: the bench type, 0 if the furniture is no bench.
    int bench_type{};
    /// A dead actor: activating it opens its inventory (like a container).
    bool dead{};
};

/// How an activation ended, for the host to act on. `kind` is one of missing,
/// parent_only, locked, blocked, busy (the host shows `note`), door,
/// furniture (with `event`; `deferred` if the graph's ObjectActivated is
/// awaited), toggle_open, container (a CONT: the host opens its menu), harvest
/// (done; `harvest_*` say what was given, nothing when the chance failed),
/// crafting (a bench: the host opens its menu for `bench_type`) or none
/// (the host applies it; `note` is for when it cannot; a harvested plant
/// says "already harvested"), pickup (done).
struct ActivationOutcome {
    std::uint32_t ref{};
    std::string kind{"missing"};
    std::string note;
    /// "0x%08X editor_id (type)"; unset for a reference nobody knows.
    std::optional<std::string> label;
    /// How many OnActivate handlers ran.
    std::int64_t scripts{};
    /// The lock level, for kind locked.
    std::optional<int> lock_level;
    std::string event;
    bool deferred{};
    /// The reference is a load door (the host adds the door's data).
    bool door{};
    /// kind harvest: the ingredient form given (0 if none), how many, its
    /// name, and the sound descriptor to play (0 if none).
    std::uint32_t harvest_form{};
    std::int64_t harvest_count{};
    std::string harvest_name;
    std::uint32_t harvest_sound{};
    /// kind crafting: the bench type (FURN WBDT).
    int bench_type{};
};

/// What a harvest gave.
struct HarvestResult {
    std::uint32_t form{};
    std::int64_t count{};
    std::string name;
    std::uint32_t sound{};
};

/// What an activation asks of the world and the VM. `SkydotPapyrus` answers it
/// from the pack and its scripts; a unit test from a few fields.
class ActivationHost {
public:
    virtual ~ActivationHost() = default;
    /// False if `ref` is not a reference.
    [[nodiscard]] virtual bool target(std::uint32_t ref, ActivationTarget& out) const = 0;
    /// The lock level if locked, else -1 (a load door answers for its partner).
    [[nodiscard]] virtual int lock_level(std::uint32_t ref) const = 0;
    virtual void unlock(std::uint32_t ref) = 0;
    /// Whether the reference's behaviour graph raises ObjectActivated.
    [[nodiscard]] virtual bool graph_raises_activation(std::uint32_t ref) const = 0;
    /// Send OnActivate(activator) to `ref`'s scripts; how many handlers ran.
    virtual std::int64_t send_activate(std::uint32_t ref, std::int64_t activator) = 0;
    [[nodiscard]] virtual bool activation_blocked(std::uint32_t ref) const = 0;
    struct Child {
        std::uint32_t ref{};
        double delay{};
    };
    [[nodiscard]] virtual std::vector<Child> activate_children(std::uint32_t ref) const = 0;
    /// Run, after `delay` VM seconds, an activation of `child` by `parent`
    /// (ActivationSource::parent).
    virtual void schedule_child(double delay, std::uint32_t child, std::uint32_t parent) = 0;
    /// `activator` takes the loose item `ref`: it goes into the inventory and
    /// the reference leaves the world.
    virtual void pick_up(std::uint32_t ref, std::int64_t activator, const ActivationTarget& item) = 0;
    /// Whether a harvestable `ref` is harvested now.
    [[nodiscard]] virtual bool harvested(std::uint32_t) const { return false; }
    /// Harvest `ref` for `activator`: roll the season's chance, add the
    /// ingredient to its inventory, mark the reference harvested.
    virtual HarvestResult harvest(std::uint32_t, std::int64_t) { return {}; }
};

/// The whole of an activation, minus what only the host can do: the rules
/// above, then OnActivate with `activator` as akActionRef (not with
/// `default_only`), then the activate children (not if scripts block the
/// activation), then the pickup of a loose item.
class Activations {
public:
    explicit Activations(ActivationHost& host) : host_(host) {}

    /// `activator` 0 is the player.
    ActivationOutcome activate(std::uint32_t ref, std::int64_t activator, bool default_only,
                               ActivationSource source, bool force);
    /// Finish an activation that `activate` left pending (`deferred`): the
    /// graph raised ObjectActivated. Nothing pending: nullopt.
    std::optional<ActivationOutcome> complete(std::uint32_t ref);

    [[nodiscard]] bool pending(std::uint32_t ref) const { return pending_.pending(ref); }
    void clear() { pending_.clear(); }

private:
    /// OnActivate, the blocked check and the activate children: what an
    /// activation does once it is going ahead. False if scripts blocked it.
    bool run(std::uint32_t ref, std::int64_t activator, bool default_only, ActivationOutcome& out);

    ActivationHost& host_;
    PendingActivations pending_;
};

} // namespace skydot
