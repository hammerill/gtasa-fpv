// Motor sound: plays libfpv's synthesized audio through waveOut while the
// drone is up.
#pragma once

namespace audio {

// Both are idempotent. stop() joins the audio thread, so call it before the
// core is unloaded.
void start();
void stop();

// Analog receiver hiss on top of the drone sound (signal lost). Any thread.
void set_static(bool on);

}  // namespace audio
