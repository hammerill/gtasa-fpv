# Notes for agents

## Testing in the running game

- The game pauses whenever its window is minimized or loses focus, and it stays
  paused (the world is frozen, and nothing new reaches GTASA.FPV.log) until the
  window is focused again. Before a test, bring the game window to the front:
  `um win drive --proc gta_sa "focus"`. If the pause menu is open, also press Esc
  (`"key 0x1B"`). A test triggered while the game is paused only starts once it
  is focused.
- Build with `cmake --build build`, then always deploy the kamikaze libfpv:
  `LIBFPV_DLL=~/Projects/libfpv/build/win32-kamikaze/libfpv.dll ./deploy.sh`. The
  running game hot-reloads the core DLL. libfpv.dll and the .asi are locked
  while the game runs, so a new libfpv only goes in with the game closed. A hot reload re-runs
  `fpv-data/autotest.txt` if that file exists, so delete it before deploying.
- Test flights: write knobs to `modloader/FPV Drone/fpv-data/autotest.txt` (see
  the header of src/autotest.cpp), then read `modloader/FPV Drone/GTASA.FPV.log`.
- Starting the game from WSL doesn't work (it starts with no window). Ask the
  user to launch it.
