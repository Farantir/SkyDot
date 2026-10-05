// SPDX-License-Identifier: GPL-3.0-or-later
//
// What is built: SkydotWorld's settings, read when a cell is built, so a
// change applies from the next build.
#pragma once

namespace skydot {

struct BuildOptions {
    /// Replace imported materials with Skyrim-style shader materials (see
    /// materials.hpp).
    bool skyrim_materials{true};
    /// Play models' controllers and particle systems and flicker lights.
    bool effects{true};
    /// Grass on exterior terrain (GRAS).
    bool grass{true};
    /// Shadows on every light, not only those whose record asks for them.
    bool all_light_shadows{false};
    /// Placed actors (ACHR).
    bool actors{true};
    /// Actors walk around their place while no AI packages are read.
    bool actor_wander{true};
    /// Physics bodies for models and terrain.
    bool collision{true};
    /// Navmeshes as navigation regions.
    bool navigation{true};
    /// Land texture repeats per cell side.
    double terrain_tiling{8.0};
};

} // namespace skydot
