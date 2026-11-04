// CJ at his piloting spot: while the drone flies from its views (the radio in
// use, and the player's F8 > Fly > Pilot pose on; not in the pilot view) he stands in a
// pilot's pose (otb.ifp's betting-slip anim: both hands in front; his head and face kept from the idle)
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

// Every drone step while CJ flies the drone on foot. pose: the pilot's pose
// (and radio) now, else the game's own anims (released from the pose). His
// seat while he stands in from a vehicle is drawn either way.
void hold(sa::CPed *cj, bool pose);

// CJ's seat while he stands in from a vehicle: he is out of it (at the
// drone), and drawn sitting there in the anim he had, instead of at a spot.
struct Seat {
    int car;               // script handle
    sa::CVector axes[3];   // his right, forward, up in the car's frame
    sa::CVector offset;    // his position in the car's frame
    void *anim;            // CAnimBlendHierarchy* he sat with (null: not drawn)
};

// The full-body anim CJ plays in his vehicle now (the strongest one), or null.
void *seat_anim(sa::CPed *cj);

// Body stand-in on (home: CJ's spot, his real position is at the drone; seat:
// from a vehicle) or off (null).
void away(const sa::CVector *home, const Seat *seat = nullptr);

}  // namespace pilot
