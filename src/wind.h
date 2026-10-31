// The game's air for libfpv: the weather's wind, helicopter downwash and
// explosion blasts. libfpv makes it local (height, shelter, gusts).
#pragma once

namespace wind {

// Forgets the last flight's wind direction and explosions. At launch.
void reset();

// Once per drone step, before fpv_drone_tick, with the drone's position
// (game coords). Game thread only.
void update(float dt, const float drone_pos[3]);

}  // namespace wind
