// Kamikaze builds of libfpv (warhead, bomb arm and explode switches): the
// game's side of a detonation. Everything here is a no-op with a libfpv.dll
// built without LIBFPV_KAMIKAZE.
#pragma once

#include <libfpv.h>

namespace kamikaze {

// What the pilot sees after the blast, before control goes back to CJ.
enum class Ending { SignalLost, Cutscene };

// The player's choice (F8 > Fly button), saved in fpv-data/gtasa-fpv.ini.
Ending ending();

// How long the ending lasts (F8 > Fly, libfpv's ending_duration), seconds.
float ending_seconds();

// Once libfpv is loaded: reads the setting and adds the F8 button. Game thread.
void init();

// Handles a click on an overlay button; false when it is not ours. Game thread.
bool on_action(uint32_t id);

// The game's explosion at the detonation point. Game thread.
void explode(const fpv_detonation &d);

}  // namespace kamikaze
