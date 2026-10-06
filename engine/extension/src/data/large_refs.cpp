// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/large_refs.hpp"

#include "world_generated.h"

#include <bit>

namespace skydot::large_refs {

std::vector<std::uint32_t> select(const WorldData& data, std::uint32_t world, std::int32_t cx,
                                  std::int32_t cy, std::int32_t radius, const CellBuilt& built) {
    std::vector<std::uint32_t> out;
    const std::uint32_t count = data.large_ref_count(world);
    if (radius < 0 || count == 0) {
        return out;
    }
    // A bit per reference marks the union (lists overlap), and reading the
    // bits back gives the indices ascending, without sorting thousands of
    // entries again for every step of the camera.
    std::vector<std::uint64_t> bits((static_cast<std::size_t>(count) + 63) / 64, 0);
    std::vector<std::uint32_t> list;
    for (std::int32_t y = cy - radius; y <= cy + radius; ++y) {
        for (std::int32_t x = cx - radius; x <= cx + radius; ++x) {
            list.clear();
            data.large_cell_refs(world, x, y, list);
            for (const std::uint32_t index : list) {
                if (index < count) {
                    bits[index / 64] |= std::uint64_t{1} << (index % 64);
                }
            }
        }
    }
    for (std::size_t word = 0; word < bits.size(); ++word) {
        for (std::uint64_t rest = bits[word]; rest != 0; rest &= rest - 1) {
            const auto index = static_cast<std::uint32_t>(word * 64 + static_cast<std::size_t>(std::countr_zero(rest)));
            if (built) {
                const auto* ref = data.large_ref(world, index);
                if (ref == nullptr || built(ref->cell_x(), ref->cell_y())) {
                    continue;
                }
            }
            out.push_back(index);
        }
    }
    return out;
}

} // namespace skydot::large_refs
