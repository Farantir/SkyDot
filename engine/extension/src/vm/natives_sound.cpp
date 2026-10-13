// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sound natives. The game layer owns the audio players (game/game/sounds.gd);
// these only name the sound, the source and the playback instance.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"

namespace skydot {

void SkydotPapyrus::bind_sound_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    [[maybe_unused]] auto& v = *vm_;
    [[maybe_unused]] const auto arg = natives::arg;
    [[maybe_unused]] const auto number = natives::number;
    // Play(akSource): `play_sound(sound, source, instance)`; the instance id
    // comes back for StopInstance and SetInstanceVolume. The sound is a SNDR
    // or a SOUN; the source may be 0 (None), which plays non-positionally.
    v.bind("Sound", "Play", [this, arg](NativeCall& c) {
        const std::int64_t instance = next_sound_instance_++;
        emit_signal("play_sound", static_cast<std::int64_t>(c.self.form()),
                    static_cast<std::int64_t>(arg(c, 0).form()), instance);
        return NativeResult{.value = Value::integer(static_cast<std::int32_t>(instance)), .wait = -1.0};
    });
    // PlayAndWait waits for `notify_sound_finished(instance)`, which the game
    // layer sends when the clip ends (at once if there is none to play). A
    // loop never ends, so the wait gives up after SOUND_TIMEOUT. The signal
    // goes out after the wait is armed.
    v.bind("Sound", "PlayAndWait", [this, arg](NativeCall& c) {
        const std::int64_t instance = next_sound_instance_++;
        const auto sound = static_cast<std::int64_t>(c.self.form());
        const auto source = static_cast<std::int64_t>(arg(c, 0).form());
        return NativeResult{.value = Value::boolean(true),
                            .wait = SOUND_TIMEOUT,
                            .wait_for = "sound:" + std::to_string(instance),
                            .after = [this, sound, source, instance] {
                                emit_signal("play_sound", sound, source, instance);
                            }};
    });
    v.bind("Sound", "StopInstance", [this, arg](NativeCall& c) {
        emit_signal("stop_sound", static_cast<std::int64_t>(arg(c, 0).i()));
        return NativeResult{};
    });
    v.bind("Sound", "SetInstanceVolume", [this, arg, number](NativeCall& c) {
        emit_signal("sound_volume", static_cast<std::int64_t>(arg(c, 0).i()), number(arg(c, 1)));
        return NativeResult{};
    });
}

} // namespace skydot
