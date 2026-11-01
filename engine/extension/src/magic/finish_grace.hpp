// SPDX-License-Identifier: GPL-3.0-or-later
//
// The script objects of a finished effect stay a while: OnEffectFinish was sent and its handler needs the VM's
// updates before the objects go. The wait is VM seconds (frames differ in length), not a count of updates.
// Nothing here touches Godot.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace skydot::magic {

class FinishGrace {
public:
    /// VM seconds the script objects of a finished effect stay after OnEffectFinish was sent.
    static constexpr double k_effect_finish_grace = 0.1;

    /// `id` (an effect instance) is over: it is due `k_effect_finish_grace` seconds from now.
    void add(std::uint32_t id) { waiting_.emplace_back(id, k_effect_finish_grace); }
    /// `seconds` of VM time passed: the ids whose wait is over (and no longer waiting).
    [[nodiscard]] std::vector<std::uint32_t> tick(double seconds) {
        std::vector<std::uint32_t> due;
        for (auto it = waiting_.begin(); it != waiting_.end();) {
            it->second -= seconds;
            if (it->second <= 0.0) {
                due.push_back(it->first);
                it = waiting_.erase(it);
            } else {
                ++it;
            }
        }
        return due;
    }
    void forget(std::uint32_t id) {
        std::erase_if(waiting_, [id](const auto& w) { return w.first == id; });
    }
    [[nodiscard]] std::size_t size() const noexcept { return waiting_.size(); }
    void clear() { waiting_.clear(); }

private:
    std::vector<std::pair<std::uint32_t, double>> waiting_;
};

} // namespace skydot::magic
