# GTA San Andreas FPV

Fly an FPV drone through San Andreas with your own radio. Press F7 and CJ
launches a quad from the ground in front of him. It is simulated by
[libfpv](https://github.com/hammerill/libfpv), which runs real Betaflight
firmware, so rates, angle mode, air mode, arming and turtle mode behave as
they do on a real quad.

## Features

- **Real Betaflight** in the loop, with your rates, modes and tuning. Set up
  your radio once in the F8 overlay's wizard. Betaflight Configurator can
  connect too.
- **Three cameras**: FPV (the goggles view, with a fisheye lens and full
  roll), chase and line of sight from where CJ stands. The chase and line of
  sight views show a small 5" quad.
- **Betaflight OSD** in the FPV view: battery, cell voltage, current, flight
  timer and mode. The GPS elements work too; home is CJ.
- **Collides with the world**: buildings, the ground, fences, cars, peds and
  props, all through the game's own line-of-sight test.
- **Crash effects**: a fast hit hurts a ped (the game's own damage event, as
  if CJ rammed them), dents a car or shoves a prop.
- **The world treats the drone as CJ.** Once the drone is 20 m from CJ, he
  stands in for it: hidden, frozen and untouchable, right where the quad is.
  Peds, gangs and the police react to the drone as they would to CJ (with a
  wanted level, the cops come for the drone and shoot at it), and the city
  streams in, with traffic and pedestrians, wherever you fly. Back within
  10 m of his spot, he is himself again. In a vehicle he stays put; only
  streaming and traffic follow the drone.
- **Motor sound** from libfpv's synthesizer.
- **Clean view**: the HUD, radar and radio station name are hidden while you
  fly. Put Betaflight's OSD disable on a switch to hide the OSD, help boxes
  and subtitles too, for recording.
- **Move CJ here**: ends the flight with CJ standing under the drone (and his
  vehicle with him), facing the way it faced.
- **Kamikaze** (only with a kamikaze build of libfpv): the drone can carry a
  warhead (F8 > Fly > Bomb installed). A detonation sets off a grenade-sized
  explosion. Then the goggles show static with "SIGNAL LOST", or an outside
  camera shows the blast ("Cutscene", chosen in F8 > Fly), and you control
  CJ again.

## Controls

| Key | Action |
|---|---|
| F7 | Launch the drone / return to CJ |
| F8 | Settings overlay (radio setup, rates, OSD, camera, fisheye strength) |
| C | Cycle cameras: FPV, chase, line of sight |
| R | Respawn the drone at its launch point, repaired |
| Y | Move CJ here (also a button in F8 > Fly) |

Without a radio, the keyboard works as a fallback. Enter arms, W/S is
throttle, A/D is yaw, and the arrow keys are pitch and roll. A gamepad works
too. Kamikaze builds: K toggles the bomb arm switch, X held explodes.

## Requirements

- GTA San Andreas **1.0 US** (`gta_sa.exe`, 14,383,616 bytes), the version
  nearly every PC mod needs. Steam and Rockstar Launcher copies have to be
  downgraded first. Other versions are detected and the mod stays off.
- An ASI loader (Ultimate ASI Loader, or the one in CLEO/Modloader setups).
- [Modloader](https://github.com/thelink2012/modloader) is recommended:
  then everything lives in one folder, `modloader/FPV Drone/`.
- Windows 10 or 11. The game is 32-bit, so libfpv is an i686 build.

Tested with Alexander PolyAK's classic repack (30.12.2025: CLEO 4.4.4,
Modloader 0.3.7, SilentPatch, SkyGfx, ThirteenAG's Widescreen Fix, Open Limit
Adjuster). The mod chains onto the hooks those mods already placed.

## Installing

Players: see [docs/INSTALL.md](docs/INSTALL.md), which is also the text of
every release. Every push to `main` builds a release
(`.github/workflows/release.yml`) with the latest libfpv.

| File | What it is |
|---|---|
| `GTASA.FPV.asi` | Loader that the ASI loader (or Modloader) picks up |
| `GTASA.FPV.core.dll` | The mod itself (hot-reloaded by the loader) |
| `libfpv.dll` | libfpv, i686 build |
| `libfpv.LICENSE.txt` | libfpv's license (GPL-3.0-or-later) |

Everything is found next to the `.asi`: settings, radio mappings and
Betaflight EEPROMs go in `fpv-data/` there, and the log is `GTASA.FPV.log`.

## Building

CMake 3.24 or newer and the i686 MinGW-w64 toolchain
(`i686-w64-mingw32-g++-posix`); it cross-compiles from Linux or WSL.

```sh
# libfpv, 32-bit (from the libfpv checkout); add -DLIBFPV_KAMIKAZE=ON for the warhead
cmake -S . -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-i686.cmake
cmake --build build/win32 -j

# this mod
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake
cmake --build build
./deploy.sh   # into "<game>/modloader/FPV Drone/"; GAME=... to point elsewhere
LIBFPV_DLL=~/Projects/libfpv/build/win32-kamikaze/libfpv.dll ./deploy.sh   # kamikaze
```

While the game runs, a new core reloads within a second of `./deploy.sh`.
The `.asi` and `libfpv.dll` stay locked until the game exits.

## How it works

Every address is in `src/sa.h`, checked against the disassembly and the
community `gta_sa.pdb`. Hooks redirect `call` instructions (chaining onto
whatever another mod put there) or detour a few function entries; every
patch checks the bytes it replaces and logs instead of crashing on a
mismatch. The core removes its hooks before it is unloaded.

- **Loop.** The loader hooks `Idle`'s call to `CGame::Process` and the menu
  loop; the core does keys and the session there.
- **Camera.** Inside `CCamera::Process`, at its call to `CDraw::SetFOV`, the
  core steps libfpv and writes `TheCamera`'s matrix (its "right" axis points
  to screen left) and near plane, and passes the FOV on. The game then
  builds the frustum and the RenderWare camera itself, so the whole frame,
  culling included, uses the drone's camera with full roll. The FOV is
  calibrated from the RenderWare view window, so widescreen fixes don't skew
  it, and widened for the fisheye to the real aspect.
- **World collision.** `CWorld::ProcessLineOfSight` with everything solid
  and CJ ignored; hits map to script handles through the pools.
- **Stand-in.** CJ is teleported along with the drone, his
  `CPlayerPed::ProcessControl` is skipped, and his visible and collision
  flags are cleared every frame. Damage to him is dropped in
  `CPedDamageResponseCalculator::ComputeDamageResponse`. The FindPlayer*
  position queries return the drone.
- **Drawing.** The fisheye (a 64x36 warped grid over a copy of the frame),
  the OSD and the overlay are D3D9 fixed-function passes before
  `RwCameraEndUpdate`. The drone model is drawn right after `RenderScene`,
  depth-tested against the scene and tinted with the time of day's light.
- **Audio.** A waveOut thread plays `fpv_audio_read`.

`MODLOG.md` is the development journal.

## Testing without hands

`fpv-data/autotest.txt` (next to the `.asi`) starts a scripted flight
whenever it is created or touched; see the top of `src/autotest.cpp` for the
knobs (`at=x,y,z,heading`, `hover`, `height=`, `secs=`, `shots=`, `ped=`,
`wanted=`, `time=`, `view=chase`, `boom`, `cutscene`...). **Delete it
afterwards.** `fpv-data/shot.txt` saves the next frame to
`fpv-data/shots/<time>.bmp`, which works when window capture can't see a
fullscreen game.

## Credits

- [libfpv](https://github.com/hammerill/libfpv): the drone, Betaflight
  (through SimITL), OSD, overlay and sound.
- The community `gta_sa.pdb` symbols, and
  [plugin-sdk](https://github.com/DK22Pac/plugin-sdk) as a reference for
  layouts.
- [invilso/gta-fpv-drone](https://github.com/invilso/gta-fpv-drone), an
  earlier SA drone, for its lessons.
- Written with Claude (Anthropic).

## License

GPL-3.0-or-later, the same as libfpv (which embeds Betaflight). See
[LICENSE](LICENSE).
