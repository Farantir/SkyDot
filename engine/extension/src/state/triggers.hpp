// SPDX-License-Identifier: GPL-3.0-or-later
//
// Trigger volumes and the actors that move through them, kept by the VM side
// of the game: a reference with an XPRM box or sphere, or a model with
// phantoms, is a trigger; `move_actor` reports the triggers an actor enters
// and leaves. Saved in the PAPY section. Nothing here touches Godot: the
// binding (SkydotPapyrus) converts the world's types to these and sends the
// events.
#pragma once

#include "save/bytes.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace skydot::vm {

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

/// A point to local coordinates: a 3x3 matrix (rows) and a translation.
struct Affine {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3 origin;

    [[nodiscard]] Vec3 xform(const Vec3& v) const;
};

/// A volume taken from a model's phantom (a pressure plate's). `to_local`
/// maps a Skyrim-space position to the volume's frame, game units.
struct Volume {
    Affine to_local;
    Vec3 half; ///< Half extents; x is the radius of a sphere.
    bool sphere = false;
};

struct Trigger {
    static constexpr std::uint32_t k_box = 1;
    static constexpr std::uint32_t k_sphere = 2;

    bool primitive = true; ///< XPRM box or sphere; false if only `volumes` apply.
    std::vector<Volume> volumes;
    Vec3 origin;
    Affine axes; ///< Skyrim space to the volume's axes, scale removed (no translation).
    Vec3 half;   ///< Half extents; x is the radius of a sphere.
    std::uint32_t type = 0;
    std::set<std::uint32_t> inside;

    [[nodiscard]] bool contains(const Vec3& position) const;
};

class Triggers {
public:
    /// A trigger from an XPRM primitive; the volumes it already has are kept.
    void set_primitive(std::uint32_t ref, Trigger trigger);
    /// The volumes of a model's phantoms; makes a volumes-only trigger if
    /// `ref` is none yet. Empty `volumes`: nothing happens.
    void set_volumes(std::uint32_t ref, std::vector<Volume> volumes);
    /// Forget trigger `ref` and, if it is one, the actor `ref`.
    void forget(std::uint32_t ref) {
        triggers_.erase(ref);
        actors_.erase(ref);
    }
    /// Forget where `actor` was (it left the screen); the triggers it is inside keep it.
    void forget_position(std::uint32_t actor) { actors_.erase(actor); }
    void clear() {
        triggers_.clear();
        actors_.clear();
    }

    /// Move `actor` to `position` (Skyrim space). For each trigger it crosses
    /// (in reference order) `notices(trigger)` is asked on entering, and a
    /// trigger that does not notice is skipped; then `crossed(trigger,
    /// entered)` runs. The inside set is updated before `crossed`.
    void move_actor(std::uint32_t actor, const Vec3& position,
                    const std::function<bool(std::uint32_t)>& notices,
                    const std::function<void(std::uint32_t, bool)>& crossed);

    [[nodiscard]] std::size_t inside_count(std::uint32_t ref) const {
        const auto it = triggers_.find(ref);
        return it != triggers_.end() ? it->second.inside.size() : 0;
    }
    [[nodiscard]] bool has(std::uint32_t ref) const { return triggers_.contains(ref); }
    [[nodiscard]] const std::map<std::uint32_t, Trigger>& triggers() const { return triggers_; }
    /// Where `actor` was last put by move_actor; null if never.
    [[nodiscard]] const Vec3* actor_position(std::uint32_t actor) const {
        const auto it = actors_.find(actor);
        return it != actors_.end() ? &it->second : nullptr;
    }
    [[nodiscard]] const std::map<std::uint32_t, Vec3>& actors() const { return actors_; }

    /// What a save holds of this: who is inside which trigger, and where the
    /// actors are. The triggers themselves come back from the references.
    struct Saved {
        std::map<std::uint32_t, std::set<std::uint32_t>> inside;
        std::map<std::uint32_t, Vec3> actors;
    };
    void write(save::ByteWriter& w) const;
    [[nodiscard]] static bool read(save::ByteReader& r, Saved& out);
    /// Put a decoded save back: the inside sets of triggers that exist, all
    /// the actors.
    void restore(Saved&& saved);

private:
    std::map<std::uint32_t, Trigger> triggers_;
    std::map<std::uint32_t, Vec3> actors_;
};

} // namespace skydot::vm
