// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotSwitch`: NiSwitchNode support. The converter marks a switch node with
// `extras.bethconv.switch`, the index of the child the model starts with; the
// other children are hidden. Flora keep the whole plant as child 0 and the
// harvested one as child 1, which the game shows once the reference is
// harvested.
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

#include <cstdint>

namespace skydot {

class SkydotSwitch : public godot::RefCounted {
    GDCLASS(SkydotSwitch, godot::RefCounted)

public:
    /// Show only the marked index in every switch node under `root`
    /// (inclusive). Returns how many switch nodes there are.
    static std::int64_t apply_defaults(godot::Node* root);

    /// Show only child `index` in every switch node under `root` (inclusive);
    /// an index past the last child shows the last. Returns the switch count.
    static std::int64_t set_index(godot::Node* root, std::int64_t index);

    /// The index the first switch node under `root` shows, or -1 without one.
    static std::int64_t get_index(godot::Node* root);

protected:
    static void _bind_methods();
};

} // namespace skydot
