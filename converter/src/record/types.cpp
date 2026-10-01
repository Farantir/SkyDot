// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/types.hpp"

#include <array>

namespace bethconv::record {
namespace {

std::string hex8(std::uint32_t v) {
    static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string out = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        out.push_back(digits[static_cast<std::size_t>((v >> shift) & 0xF)]);
    }
    return out;
}

} // namespace

std::string FormId::to_string() const { return hex8(value); }

std::string_view to_string(GroupType type) noexcept {
    switch (type) {
    case GroupType::top:                      return "top";
    case GroupType::world_children:           return "world-children";
    case GroupType::interior_cell_block:      return "interior-cell-block";
    case GroupType::interior_cell_sub_block:  return "interior-cell-sub-block";
    case GroupType::exterior_cell_block:      return "exterior-cell-block";
    case GroupType::exterior_cell_sub_block:  return "exterior-cell-sub-block";
    case GroupType::cell_children:            return "cell-children";
    case GroupType::topic_children:           return "topic-children";
    case GroupType::cell_persistent_children: return "cell-persistent-children";
    case GroupType::cell_temporary_children:  return "cell-temporary-children";
    case GroupType::quest_children:           return "quest-children";
    }
    return "unknown";
}

std::string GroupLabel::to_string(GroupType type) const {
    switch (type) {
    case GroupType::top:
        return as_type().to_string();
    case GroupType::exterior_cell_block:
    case GroupType::exterior_cell_sub_block:
        return "(" + std::to_string(grid_x()) + ", " + std::to_string(grid_y()) + ")";
    case GroupType::interior_cell_block:
    case GroupType::interior_cell_sub_block:
        return std::to_string(static_cast<std::int32_t>(raw));
    default:
        return as_form().to_string();
    }
}

} // namespace bethconv::record
