// Little reactions in the game's own sounds (as sm64ex-fpv does with its
// own): menu clicks for the drone's actions, a collision sound at impacts.
#pragma once

#include <libfpv.h>

#include "sa.h"

namespace sfx {

// A frontend sound (launch, recall, camera switch, refused action). Game thread only.
void play(sa::FrontendSound sound);

// Once per drone step: the hardest impact of the frame, if it was a real hit,
// sounds like the drone's plastic against what it hit.
void on_frame(const fpv_frame &frame);

}  // namespace sfx
