// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/idle_choice.hpp"

#include "assets/vpath.hpp"

#include <algorithm>

namespace skydot::ai {
namespace {

namespace wfb = bethconv::pack::wfb;

constexpr int k_max_depth = 32; ///< Only against a loop in a broken tree.

std::string spelled(std::string_view path) {
    std::string out = ascii_lower(std::string(path));
    std::ranges::replace(out, '\\', '/');
    return out;
}

std::optional<IdleChoice> search(const WorldData& data, const wfb::Idle& node, conditions::ConditionWorld& world,
                                 const conditions::ConditionContext& context, int depth) {
    if (depth > k_max_depth || !conditions::conditions_pass(node.conditions(), world, context)) {
        return std::nullopt;
    }
    for (const wfb::Idle* child : data.idle_children(node.id())) {
        if (auto found = search(data, *child, world, context, depth + 1)) {
            return found;
        }
    }
    if (node.event() != nullptr && node.event()->size() > 0) {
        return IdleChoice{node.id(), node.event()->str()};
    }
    return std::nullopt;
}

} // namespace

std::optional<IdleChoice> choose_idle(const WorldData& data, std::uint32_t action, const std::string& root_behaviour,
                                      conditions::ConditionWorld& world, const conditions::ConditionContext& context) {
    const std::string wanted = spelled(root_behaviour);
    for (const wfb::Idle* root : data.idle_children(action)) {
        if (root->behaviour() == nullptr || spelled(root->behaviour()->string_view()) != wanted) {
            continue;
        }
        if (auto found = search(data, *root, world, context, 0)) {
            return found;
        }
    }
    return std::nullopt;
}

} // namespace skydot::ai
