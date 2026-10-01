// SPDX-License-Identifier: GPL-3.0-or-later
//
// Extension entry point and class registration. `skydot_library_init` (named
// in game/skydot.gdextension) is the only exported symbol; classes are
// registered with ClassDB at SCENE level, since no server needs them.
#include "register_types.hpp"

#include "assets/model.hpp"
#include "assets/pack.hpp"
#include "vm/papyrus.hpp"
#include "world/animator.hpp"
#include "world/billboard.hpp"
#include "world/flicker.hpp"
#include "world/lod.hpp"
#include "world/particles.hpp"
#include "world/world.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

void skydot_initialize(godot::ModuleInitializationLevel level) {
    if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    GDREGISTER_CLASS(skydot::SkydotBillboard);
    GDREGISTER_CLASS(skydot::SkydotFlicker);
    GDREGISTER_CLASS(skydot::SkydotParticles);
    GDREGISTER_CLASS(skydot::SkydotAnimator);
    GDREGISTER_CLASS(skydot::SkydotMaterials);
    GDREGISTER_CLASS(skydot::SkydotWorld);
    GDREGISTER_CLASS(skydot::SkydotModel);
    GDREGISTER_CLASS(skydot::SkydotPack);
    GDREGISTER_CLASS(skydot::SkydotPapyrus);
    GDREGISTER_CLASS(skydot::SkydotLod);
}

void skydot_uninitialize(godot::ModuleInitializationLevel level) {
    if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

extern "C" {
GDExtensionBool GDE_EXPORT skydot_library_init(GDExtensionInterfaceGetProcAddress get_proc_address,
                                               GDExtensionClassLibraryPtr library,
                                               GDExtensionInitialization* initialization) {
    godot::GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(skydot_initialize);
    init.register_terminator(skydot_uninitialize);
    init.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
}
