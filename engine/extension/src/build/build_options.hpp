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
    /// Land texture repeats per cell side. 24 as in the game: Lighting.hlsl
    /// maps the land UV onto a 4-cell LOD texture with a factor of 1/96, and
    /// top-down game shots of Riverwood match 24 (flower spots per area and
    /// their size; 8 drew each texture three times too large).
    double terrain_tiling{24.0};
};

} // namespace skydot
