#pragma once

#include <string>

namespace paths {

// The folder this module (the .asi or the core) was loaded from, e.g.
// "...\modloader\FPV Drone". The game's working directory is the game root.
std::string mod_dir();

}  // namespace paths
