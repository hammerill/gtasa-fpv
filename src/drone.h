// The FPV drone session: libfpv drone + the game camera.
#pragma once

#include <libfpv.h>

namespace drone {

// Launches the drone from CJ, or returns to CJ. Game thread only.
void toggle();

// Once per frame after CGame::Process: keys, overlay, session state.
void tick();

// From CCamera::Process, just before CDraw::SetFOV(game_fov): steps the
// drone and, in flight, writes the camera matrix and near plane. Returns the
// FOV to pass on.
float camera(float game_fov);

// After the frame: the RwCamera view window the game built from our FOV.
void observe_view_window(float tan_half_y);

bool active();

// The world must show nothing of the game's HUD (in flight), or no game text
// at all (the pilot's OSD disable switch is on).
bool hide_hud();
bool hide_text();

// Ends the flight with CJ standing under the drone (F8 > Fly button, Y key). Game thread only.
void move_cj_here();

// The frame from the latest tick (struct_size 0 before the first one).
const fpv_frame &last_frame();

// Copies the latest frame for the lens pass and the OSD; false when no drone
// is flying or a third-person camera is on. no_signal: the warhead went off
// and the goggles show static.
bool osd_frame(fpv_frame *out, bool *no_signal);

// What the drone model pass needs: the drone's pose and motors, and the
// camera this frame renders from. False in the FPV view (the lens is inside
// the frame) and when no drone is up.
struct ModelPose {
    float pos[3], right[3], up[3], forward[3];  // body axes, world
    float motor[4][3];                           // hubs, body-local (right, forward, up)
    float rpm[4];
    float eye[3], eye_right[3], eye_up[3], eye_forward[3];
};
bool model_pose(ModelPose *out);

// Host-injected radio channels (AETR, AUX1..8, -1..1), overriding devices for 0.5 s.
void inject_channels(const float channels[FPV_CHANNEL_COUNT]);

// Test knobs for the autotest (0 / negative = normal behaviour).
struct Tuning {
    float fov = 0;        // vertical degrees, overrides libfpv's FPV FOV
    float near_clip = 0;  // overrides kNearClip
    bool osd = true;
    int after_blast = -1;  // 0 signal lost, 1 cutscene; -1 = the player's F8 choice
    int view = 0;          // camera at launch: 0 FPV, 1 chase, 2 line of sight
};
Tuning &tuning();

// Called before the core is unloaded.
void shutdown();

}  // namespace drone
