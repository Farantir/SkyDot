// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/natives_world.hpp"

namespace skydot::vm {

std::uint32_t nth_linked_ref(const WorldAccess& world, std::uint32_t ref, std::int32_t n) {
    if (n < 0) {
        return 0;
    }
    // A chain that loops back never ends; the steps are bounded by `n`.
    for (std::int32_t i = 0; i < n && ref != 0; ++i) {
        ref = world.linked_ref(ref, 0);
    }
    return ref;
}

void bind_world_natives(Vm& vm, const WorldAccess& world) {
    // ObjectReference.psc: the nth linked ref in the chain of this reference's
    // links without a keyword (DartTrap picks a port with it). 0 is not
    // documented; it gives the reference itself (assumed, not checked).
    vm.bind("ObjectReference", "GetNthLinkedRef", [&world](NativeCall& c) {
        const std::int32_t n = c.args.empty() ? 0 : c.args[0].i();
        const std::uint32_t target = nth_linked_ref(world, c.self.form(), n);
        return NativeResult{.value = target != 0 ? c.vm.object(target, "ObjectReference") : Value{}, .wait = -1.0};
    });
    // The cell is a form of its own (class Cell, the cell record's id).
    vm.bind("ObjectReference", "GetParentCell", [&world](NativeCall& c) {
        const std::uint32_t cell = world.parent_cell(c.self.form());
        return NativeResult{.value = cell != 0 ? c.vm.object(cell, "Cell") : Value{}, .wait = -1.0};
    });
    vm.bind("Cell", "IsInterior", [&world](NativeCall& c) {
        return NativeResult{.value = Value::boolean(world.cell_interior(c.self.form())), .wait = -1.0};
    });
    vm.bind("Cell", "IsAttached", [&world](NativeCall& c) {
        return NativeResult{.value = Value::boolean(world.cell_attached(c.self.form())), .wait = -1.0};
    });
    // Levelled lists are resolved against the player's level for now.
    vm.bind("ObjectReference", "CalculateEncounterLevel", [&world](NativeCall&) {
        return NativeResult{.value = Value::integer(world.player_level()), .wait = -1.0};
    });
}

} // namespace skydot::vm
