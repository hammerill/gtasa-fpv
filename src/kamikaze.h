// Kamikaze builds of libfpv (warhead, bomb arm and explode switches): the
// game's side of a detonation. Only in mod builds with GTASA_FPV_KAMIKAZE
// (LIBFPV_KAMIKAZE); otherwise everything here is a no-op.
#pragma once

#include <cstdint>

#include <libfpv.h>

#if !defined(LIBFPV_KAMIKAZE)
// Plain builds: just enough of libfpv's kamikaze types for the shared code to
// compile. Nothing asks libfpv for them, and no detonation ever comes.
enum { FPV_KAMIKAZE_VIEW_SIGNAL_LOST = 0, FPV_KAMIKAZE_VIEW_CUTSCENE = 1 };
enum { FPV_KAMIKAZE_THEN_RESPAWN = 0, FPV_KAMIKAZE_THEN_RECALL = 1, FPV_KAMIKAZE_THEN_NOTHING = 2 };
struct fpv_detonation {
    uint32_t struct_size;
    double sim_time;
    uint32_t cause;
    float position[3], velocity[3], normal[3];
    uint8_t material;
    uint32_t object_id;
    int32_t motor_index;
};
#endif

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
