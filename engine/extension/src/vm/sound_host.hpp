// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SoundHost`: the playback ids the sound natives hand out. The game layer
// owns the audio players; the ids tie a Play to its StopInstance, volume and
// the end notice (`notify_sound_finished`).
#pragma once

#include <cstdint>

namespace skydot::vm {

/// How long PlayAndWait waits for the clip to end, in seconds; a loop never does.
inline constexpr double k_sound_timeout = 30.0;

class SoundHost {
public:
    virtual ~SoundHost() = default;

    /// A new playback instance id; 0 if the ids are used up (the host
    /// reports that itself).
    [[nodiscard]] virtual std::int64_t new_sound_instance() = 0;

    /// MusicType.Add and Remove: the music stack, in Add order (a type added
    /// again moves to the top). The game layer's music player reads it.
    virtual void add_music(std::uint32_t music) = 0;
    virtual void remove_music(std::uint32_t music) = 0;
};

} // namespace skydot::vm
