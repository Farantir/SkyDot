// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialogue/dialogue_states.hpp"

namespace skydot::dialogue {
namespace {

void write_set(save::ByteWriter& w, const std::set<std::uint32_t>& values) {
    w.u32(static_cast<std::uint32_t>(values.size()));
    for (const auto v : values) {
        w.u32(v);
    }
}

bool read_set(save::ByteReader& r, std::set<std::uint32_t>& out) {
    std::uint32_t n = 0;
    if (!r.count(n, 4)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t v = 0;
        if (!r.u32(v)) {
            return false;
        }
        out.insert(v);
    }
    return true;
}

} // namespace

std::vector<std::uint8_t> DialogueStates::save() const {
    save::ByteWriter w;
    write_set(w, infos_);
    write_set(w, topics_);
    write_set(w, once_);
    write_set(w, talked_);
    return w.out;
}

bool DialogueStates::load(save::ByteReader r, DialogueStates& out) {
    out = DialogueStates{};
    return read_set(r, out.infos_) && read_set(r, out.topics_) && read_set(r, out.once_) &&
           read_set(r, out.talked_) && r.at_end();
}

} // namespace skydot::dialogue
