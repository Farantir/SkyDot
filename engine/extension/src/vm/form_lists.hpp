// SPDX-License-Identifier: GPL-3.0-or-later
//
// What scripts added to form lists (FormList.AddForm): per list, the forms in
// the order added. The list's own forms come from the world; this is only the
// difference, and part of the VM save. Nothing here touches Godot.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace skydot::vm {

class FormListAdditions {
public:
    using Forms = std::vector<std::uint32_t>;

    /// Add `form` to `list` unless it was added already.
    void add(std::uint32_t list, std::uint32_t form) {
        auto& forms = added_[list];
        if (std::ranges::find(forms, form) == forms.end()) {
            forms.push_back(form);
        }
    }
    /// Take back a form that was added; false if it was not.
    bool remove(std::uint32_t list, std::uint32_t form) {
        const auto it = added_.find(list);
        if (it == added_.end()) {
            return false;
        }
        const auto at = std::ranges::find(it->second, form);
        if (at == it->second.end()) {
            return false;
        }
        it->second.erase(at);
        if (it->second.empty()) {
            added_.erase(it);
        }
        return true;
    }
    /// Forget everything added to `list`.
    void revert(std::uint32_t list) { added_.erase(list); }
    [[nodiscard]] const Forms& added(std::uint32_t list) const {
        static const Forms none;
        const auto it = added_.find(list);
        return it != added_.end() ? it->second : none;
    }
    void clear() { added_.clear(); }
    [[nodiscard]] const std::map<std::uint32_t, Forms>& lists() const { return added_; }

private:
    std::map<std::uint32_t, Forms> added_;
};

} // namespace skydot::vm
