// The game's own sounds heard from the pilot, as libfpv hears the drone
// (FPV_DRONE_HEARD_FROM_PILOT: the player's "Hear the drone from: The pilot",
// F8 > Camera & sound). San Andreas's audio places every sound relative to
// TheCamera: CAEAudioEnvironment::GetPositionRelativeToCamera (0x4D80B0, all
// 3D sounds, their pan, distance and Doppler) and the vehicle, ped, weapon and
// weather audio entities' own distance checks. While on, each of those reads
// of TheCamera in the audio code (0x4D0000-0x508000, 85 sites) reads a fake
// camera at the pilot's head instead; the renderer and the rest of the game
// keep the real camera.
#pragma once

namespace ear {

// The ear at the pilot: position and the direction it faces (towards the
// drone) and up, host (game) coords. Game thread, every frame it is on.
void set(const float pos[3], const float forward[3], const float up[3]);

// Back to the game's camera. Also before the core unloads.
void clear();

bool on();

}  // namespace ear
