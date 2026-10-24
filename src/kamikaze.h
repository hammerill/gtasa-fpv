// Kamikaze builds of libfpv (warhead, bomb arm and explode switches): the
// game's side of a detonation. Everything here is a no-op with a libfpv.dll
// built without LIBFPV_KAMIKAZE.
#pragma once

#include <libfpv.h>

namespace kamikaze {

// What happens after the blast, as the player set it in F8 > Kamikaze.
struct EndingChoice {
    uint32_t view = FPV_KAMIKAZE_VIEW_SIGNAL_LOST;  // fpv_kamikaze_ending_view
    float seconds = 3.0f;                           // 0: no view, the "then" at once
    uint32_t then = FPV_KAMIKAZE_THEN_RESPAWN;      // fpv_kamikaze_ending_then
};

// Read at the detonation (libfpv's settings.json). Game thread.
EndingChoice ending_choice();

// Once libfpv is loaded: tells libfpv which endings the game offers. Game thread.
void init();

// The game's explosion at the detonation point. Game thread.
void explode(const fpv_detonation &d);

}  // namespace kamikaze
