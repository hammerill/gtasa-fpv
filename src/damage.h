// What breaks a prop in San Andreas that libfpv can't see (INTEGRATION.md
// "Damage"): water, fire (burning cars and molotov flames too), explosions,
// helicopter rotors and bullets through the props, as fpv_drone_damage_props.
// libfpv ignores all of it while the player has damage off, so this never
// checks the setting.
#pragma once

#include <libfpv.h>

namespace damage {

// The bullet hook (CWeapon::DoBulletImpact). Once, game thread.
void install_hooks();

// Every drone step with the world live (not frozen, not a replay). Game thread.
void update(fpv_drone *drone, const fpv_frame &frame, float dt);

// The drone is gone or was reset: forget it, and the explosions going on now.
void reset();

}  // namespace damage
