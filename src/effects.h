// What drone impacts do to the peds, vehicles and objects they hit.
#pragma once

#include <libfpv.h>

namespace effects {

// Once per drone tick, with that tick's impacts. Game thread.
void on_frame(const fpv_frame &frame);

}  // namespace effects
