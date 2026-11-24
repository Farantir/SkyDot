// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hosts the natives families ask the world and the game layer through.
#include "vm/papyrus.hpp"

namespace skydot {

struct SkydotPapyrus::SoundHost final : vm::SoundHost {
    explicit SoundHost(SkydotPapyrus& owner) : owner_(owner) {}
    std::int64_t new_sound_instance() override { return owner_.new_sound_instance(); }
    SkydotPapyrus& owner_;
};

std::unique_ptr<vm::SoundHost> SkydotPapyrus::new_sound_host() { return std::make_unique<SoundHost>(*this); }

} // namespace skydot
