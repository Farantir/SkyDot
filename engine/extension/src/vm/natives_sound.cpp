// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sound natives. The game layer owns the audio players (game/game/sounds.gd);
// these only name the sound, the source and the playback instance.
#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"
#include "vm/sound_host.hpp"

#include <string>

namespace skydot::vm {

void bind_sound_natives(Vm& v, SoundHost& sounds, WorldEvents& events) {
    using namespace natives;
    // Play(akSource): `play_sound(sound, source, instance)`; the instance id
    // comes back for StopInstance and SetInstanceVolume. The sound is a SNDR
    // or a SOUN; the source may be 0 (None), which plays non-positionally.
    v.bind("Sound", "Play", [&sounds, &events](NativeCall& c) {
        const std::int64_t instance = sounds.new_sound_instance();
        if (instance == 0) {
            return NativeResult{.value = Value::integer(0), .wait = -1.0};
        }
        events.play_sound(c.self.form(), arg(c, 0).form(), instance);
        return NativeResult{.value = Value::integer(static_cast<std::int32_t>(instance)), .wait = -1.0};
    });
    // PlayAndWait waits for `notify_sound_finished(instance)`, which the game
    // layer sends when the clip ends (at once if there is none to play). A
    // loop never ends, so the wait gives up after k_sound_timeout. The signal
    // goes out after the wait is armed.
    v.bind("Sound", "PlayAndWait", [&sounds, &events](NativeCall& c) {
        const std::int64_t instance = sounds.new_sound_instance();
        if (instance == 0) {
            return NativeResult{.value = Value::boolean(false), .wait = -1.0};
        }
        const auto sound = c.self.form();
        const auto source = arg(c, 0).form();
        return NativeResult{.value = Value::boolean(true),
                            .wait = k_sound_timeout,
                            .wait_for = "sound:" + std::to_string(instance),
                            .after = [&events, sound, source, instance] { events.play_sound(sound, source, instance); }};
    });
    v.bind("Sound", "StopInstance", [&events](NativeCall& c) {
        events.stop_sound(static_cast<std::int64_t>(arg(c, 0).i()));
        return NativeResult{};
    });
    v.bind("Sound", "SetInstanceVolume", [&events](NativeCall& c) {
        events.sound_volume(static_cast<std::int64_t>(arg(c, 0).i()), number(arg(c, 1)));
        return NativeResult{};
    });
}

} // namespace skydot::vm
