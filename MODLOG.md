# GTA San Andreas FPV drone — MODLOG

## Goal
FPV drone mode in GTA San Andreas powered by libfpv (~/Projects/libfpv), at parity with gtaiv-fpv
(/mnt/d/SteamLibrary/steamapps/common/Grand Theft Auto IV/gtaiv-fpv): fly with a radio, FPV/chase/LOS
cams, Betaflight OSD, F8 overlay, fisheye, motor sound, world collision, impacts on peds/cars,
streaming around the drone, Move CJ here (Y), kamikaze when libfpv is a kamikaze build.

## Environment (recon 2026-10-24)
- Game: /mnt/d/Games/GTA San Andreas/GTA_SA.exe — **SA 1.0 US** (CLEO and Modloader both report it),
  PE32 i386, 14,383,616 bytes, md5 2b5066bd4097ac2944ce6a9cf8fe5677. D3D9 via RenderWare 3.6. No anti-cheat.
  Single player only (no SA-MP installed).
- Repack: Alexander PolyAK's "classic" build from 30.12.2025 (89 mods, Russian text). Already loaded:
  - Ultimate ASI Loader as vorbisFile.dll (original = vorbisHooked.dll); loads root *.asi and scripts\.
  - CLEO 4.4.4 (+ CLEO+, FileSystemOperations, IniFiles, IntOperations), Modloader 0.3.7 (modloader/).
  - Relevant to us: SilentPatch, GInput (XInput pads -> CPad), SkyGfx (render/postfx hooks),
    ThirteenAG Widescreen Fix (FOV/aspect hooks; `FOVControl` float at 0x6FF41B), Framerate Vigilante
    (>30 fps fixes), Improved Streaming, Open Limit Adjuster, CrashInfo.SA.asi, _noDEP.asi.
  - **gta_sa.pdb in the game folder** (MSF 7.00, ~37k strings, mangled names such as
    `_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntitybbbbbbbb`,
    `_Z23FindPlayerCentreOfWorldj`, `_ZN7CCamera11SetRwCameraEP8RwCamera`, `_RwD3D9GetCurrentD3DDevice`).
    Community symbol file for 1.0 US (CrashInfo uses it). Load into Ghidra for RE.
- User files: C:/Users/Hammerill/Documents/GTA San Andreas User Files (GTASAsf1.b, gta_sa.set,
  RepairGTASaveExtender1.dat).
- No native windowed mode (WSFix `AllowAltTabbingWithoutPausing` applies only if windowed).
- Toolchains: i686-w64-mingw32-g++-posix 13, cmake. No MSVC, no wine, no ninja.
- libfpv: main @ 9133485. build/win32/libfpv.dll and build/win32-kamikaze/libfpv.dll date from
  2026-10-15 and need a rebuild (newer commits: scancode keys, Y move-here, hover throttle...).
- RE tools: ~/Software/ghidra_12.1.4_PUBLIC, jdk-25, ~/pyghidra_mcp_projects (no MCP server attached
  to this session; headless Ghidra works).
- KB (`um kb search`): nothing for GTA SA.

## Prior art
- invilso/gta-fpv-drone (MIT, MoonLoader + SA-MP client + Python SDL bridge, own flight model, no
  Betaflight). Clone for reading only: scratchpad prior-art/. Their lessons:
  - Camera roll could not be set from MoonLoader: their hooks ran after the 3D scene (onD3DPresent),
    and CCam fields (fTilt, fRoll) are recomputed each frame. They used the in-vehicle camera by
    warping CJ into the drone vehicle. **A native hook before RenderScene does not have that limit**
    (write the camera matrix after CCamera::Process, before the RW camera is updated).
  - Drone model: RCRAIDER (vehicle 465) looks like a quad. Its local axes: right = -drone.right,
    up = drone.fwd, at = -drone.up. Model id 594 "RCCAM" is a plant pot.
  - `setCarCoordinates` snaps to nearby ground; write the entity matrix directly.
  - processLineOfSight with solid+car flags in one call missed ground for them; cast separately if seen.
  - Filter the drone's own entity out of rays. Particle names: `explosion_large`, not `explosion_huge`.
- gtaiv-fpv: the template. ASI loader + hot-reloadable core, libfpv via LoadLibrary + X-macro table,
  D3D9 fixed-function OSD/overlay on the bound back buffer (save/restore only touched states),
  fisheye via StretchRect + 64x36 grid, waveOut audio, frustum-probe near plane, player as streaming
  anchor, autotest.txt-driven self tests, schtasks to launch in the user's session from WSL.

## Decisions (agreed with user 2026-10-24)
1. Build: i686 MinGW (posix), native .asi + hot-reloadable core DLL, own 1.0 US address table checked
   in Ghidra against gta_sa.pdb. plugin-sdk / gta-reversed as reference only (never vendored).
2. Install: everything in `modloader/FPV Drone/` (asi, core dll, libfpv.dll, fpv-data). Game root untouched.
3. Self-testing allowed (check user idle first, close the game afterwards, delete autotest triggers).

4. **The world reacts to the drone as if it were CJ** (user, 2026-10-24; like sm64ex-fpv's stand-in,
   ~/Projects/sm64ex/src/pc/fpv/fpv_sm64.c update_anchor / fpv_sm64_anchor_begin/end, FPV_DRONE.md).
   sm64ex: once the drone is 20 m from Mario (back under 10 m: off), Mario's object is moved to the
   drone around every *other* object's update and restored for his own update, camera and video link.
   So enemies spawn, wake, aim and go for the drone, but can't hurt Mario, and nobody talks to the stand-in.
   SA design (to verify): while the stand-in is on, put CJ's position at the drone for the world's
   view of him (peds' reactions, gang/police AI, population spawning, wanted level, pickups?,
   streaming, FindPlayerCoors / FindPlayerCentreOfWorld*), and restore it around CJ's own
   ProcessControl/physics/rendering. Candidates: swap CJ's matrix pos around CWorld::Process minus his
   own entry, plus hooks on FindPlayer* (0x56E010, 0x56E250...). CJ must not take damage at the drone.
   Gunfire aimed at the stand-in should reach the drone (kamikaze: fpv_kamikaze_detonate(even_if_safe)).
   Simpler fallback if the swap is fragile: gtaiv-fpv's invisible, invincible, collision-less CJ moved
   to the drone (but then peds do see and shoot "him" there, and he must be put back afterwards).

## SA engine facts to use (1.0 US, verify each against the PDB/Ghidra before relying on it)
- World: X east, Y north, Z up, metres -> coords {POS_X, POS_Z, POS_Y, 1.0} (same as IV).
- Collision: CWorld::ProcessLineOfSight(from, to, CColPoint&, CEntity*&, buildings, vehicles, peds,
  objects, dummies, seeThrough, camera, shootThrough). CColPoint has normal and surface type.
  CWorld::pIgnoreEntity excludes one entity.
- Streaming: FindPlayerCentreOfWorld / _NoSniperShift are real functions in SA (not inlined like IV),
  so they can be hooked to return the drone position instead of moving CJ as an anchor. Check callers.
- Camera: CCamera (TheCamera) -> RwCamera via CCamera::CopyCameraMatrixToRWCam / SetRwCamera; FOV
  through CDraw::SetFOV (WSFix hooks it: check interaction). Near clip via RwCameraSetNearClipPlane.
- D3D9 device: RwD3D9GetCurrentD3DDevice. Draw point: after the 2D HUD, before present (hook
  RsCameraShowRaster / Render2dStuff end). Device lost/reset: RW handles reset; hook RW's callback.
- Player control: CPad::DisablePlayerControls / CPlayerInfo, HUD off via CHud/TheCamera flags.
  GInput feeds pads into CPad, so disabling player control also stops the gamepad moving CJ.
- Pause: FrontEndMenuManager.m_bMenuActive; tick from the game's process loop so libfpv auto-pauses.

## Plan
1. Backups: saves + gta_sa.set (`um backup`), hash the game root files (restore list).
2. Rebuild libfpv win32 (+ win32-kamikaze) from main; run tests on Windows via WSL interop.
3. Ghidra project on GTA_SA.exe with gta_sa.pdb; confirm the addresses above.
4. Hello .asi (log, F7 logs CJ position) through the existing ASI loader.
5. Vertical slice: F7 spawns the drone in front of CJ, camera follows frame.camera with full roll,
   CJ frozen, keyboard fallback flies it.
6. Raycast callback via CWorld::ProcessLineOfSight; impacts -> ped/vehicle effects.
7. D3D9 OSD + overlay + fisheye, audio, HUD hiding, OSD-disable rules.
8. Streaming around the drone, Move CJ here (Y), kamikaze, drone model (RC Raider) for chase/LOS.
9. README/INSTALL, release workflow, field note.

## Log
- 2026-10-24: recon done (above). Nothing in the game folder changed yet.
- Backups: ~/.universal-modder/backups/gtasa-userfiles/20261024-181607.zip (User Files). Game root hashed
  in vanilla-root-files.md5, modloader dirs listed in vanilla-modloader-dirs.txt (restore = delete
  `modloader/FPV Drone/`; root should match the md5 list).
- **Symbol table**: ~/gtasa-ref/symbols.txt (VA name, 38k entries; reference only, not in the repo), made by
  `llvm-pdbutil-18 dump --publics gta_sa.pdb` + section VAs (sec 1 = 0x401000; the PDB is IDA-exported,
  `sub_852180` -> 0x852180 checks out). Pipe through c++filt. Confirmed: CCamera::CopyCameraMatrixToRWCam
  0x50AFA0, CCamera::SetRwCamera 0x50C100, CCamera::Process 0x52B730, CGame::Process 0x53BEE0,
  RenderScene 0x53DF40, Render2dStuff 0x53E230, CPad::GetPad 0x53FB70, CWorld::ProcessLineOfSight 0x56BA00,
  FindPlayerCoors 0x56E010, FindPlayerCentreOfWorld 0x56E250 (_NoSniperShift 0x56E320, _NoInteriorShift
  0x56E400), CHud::Draw 0x58FAE0, CDraw::SetFOV 0x6FF410, RwCameraSetNearClipPlane 0x7EE1D0,
  RwD3D9GetCurrentD3DDevice 0x7F9D50, CDraw::ms_fFOV 0x8D5038, ms_fNearClipZ 0xC3EFA0, ms_fAspectRatio 0xC3EFA4.
- libfpv win32 + win32-kamikaze rebuilt from main 9133485: Windows tests pass (28 tests / 562 checks,
  kamikaze 34 / 669) via WSL interop (copy libfpv.dll next to tests/fpv_tests.exe first).

## Next — start here
1. Repo skeleton: CMakeLists (i686, LIBFPV_STATIC + LIBFPV_KAMIKAZE decls), cmake toolchain, deploy.sh
   -> `modloader/FPV Drone/`, loader .asi + core dll (hot reload), log, fpvapi X-macro table (port from gtaiv-fpv).
2. Hello .asi: confirm Modloader loads an .asi from its folder and LoadLibrary of libfpv.dll from there.
   Find the game-thread tick (hook a CALL in CGame::Process 0x53BEE0 or the idle loop) and the draw point.
3. Vertical slice (plan step 5).
