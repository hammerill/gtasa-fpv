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

// Width / height of the game's back buffer (16:9 until the first frame). Any thread.
float aspect();

// Releases textures (device lost, core unload).
void release();

}  // namespace render
