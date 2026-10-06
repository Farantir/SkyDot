// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/large_refs.hpp"

#include "world_generated.h"

#include <algorithm>

namespace skydot::large_refs {

std::vector<std::uint32_t> select(const WorldData& data, std::uint32_t world, std::int32_t cx,
                                  std::int32_t cy, std::int32_t radius, const CellBuilt& built) {
    std::vector<std::uint32_t> out;
    if (radius < 0 || data.large_ref_count(world) == 0) {
        return out;
    }
    for (std::int32_t y = cy - radius; y <= cy + radius; ++y) {
        for (std::int32_t x = cx - radius; x <= cx + radius; ++x) {
            data.large_cell_refs(world, x, y, out);
        }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
    if (built) {
        std::erase_if(out, [&](std::uint32_t index) {
            const auto* ref = data.large_ref(world, index);
            return ref == nullptr || built(ref->cell_x(), ref->cell_y());
        });
    }
    return out;
}

} // namespace skydot::large_refs
