// CJ at his piloting spot: while the drone flies he stands still in a
// pilot's pose (otb.ifp's betting-slip anim: head down, both hands in front)
// holding a radio transmitter, a small procedural model drawn in his hands.
// While he stands in for the drone (standin.cpp) his real position is at the
// drone, so for the render passes of each frame he is put back at his spot
// and shown, then returned: the world keeps seeing "CJ" at the drone, and
// the pilot still shows from the drone looking back home (like sm64ex-fpv's
// Mario, who stays where he is for his own update, camera and rendering).
#pragma once

#include "sa.h"

namespace pilot {

// Render-pass hooks (Idle: ConstructRenderList, RenderEffects). Once, game thread.
void install_hooks();

// Every drone step while CJ flies the drone on foot: the pose (and radio).
void hold(sa::CPed *cj);

// Body stand-in on (home: CJ's spot, his real position is at the drone) or off (null).
void away(const sa::CVector *home);

}  // namespace pilot
