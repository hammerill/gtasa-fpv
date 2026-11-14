// Draws libfpv's fisheye lens, OSD and F8 overlay over the game (D3D9). SA renders on
// the game thread, so everything here runs on it.
#pragma once

#include <d3d9.h>

namespace render {

// After the game finished its frame, before RwCameraEndUpdate. in_game:
// Idle's frame (false: the menus, FrontendIdle).
void frame(IDirect3DDevice9 *dev, bool in_game);

// The drone itself for the chase and line-of-sight views, depth-tested
// against the scene. Right after RenderScene, while the scene's camera and
// depth buffer are bound.
void drone_model(IDirect3DDevice9 *dev);

// The kept game UI layer (docs/INTEGRATION.md in libfpv, "No game UI in FPV"):
// between these two calls the game draws into a transparent screen-sized
// texture instead of the back buffer. frame() mixes it into the camera's
// picture after the lens and under the OSD, so the video noise covers it but
// the fisheye doesn't bend it. False: it can't, and the game must not draw.
bool begin_kept_ui(IDirect3DDevice9 *dev);
void end_kept_ui(IDirect3DDevice9 *dev);

// Width / height of the game's back buffer (16:9 until the first frame). Any thread.
float aspect();

// Releases textures (device lost, core unload).
void release();

}  // namespace render
