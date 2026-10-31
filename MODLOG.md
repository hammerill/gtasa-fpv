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

- 2026-10-24 (later): repo skeleton + vertical slice built and **verified in game** (self-test, mod screenshots):
  - Loader GTASA.FPV.asi in `modloader/FPV Drone/` (Modloader loads it; paths from the module, cwd is the game root).
    Hooks `Idle`'s call to CGame::Process (0x53E981: **CLEO already redirects it**, so redirect_call chains to whatever
    the call targets) and FrontendIdle's CPad::UpdatePads (0x53E78B) for the menus. Hot-reloads GTASA.FPV.core.dll.
  - Other mods already on our call sites (chained fine): WSFix on CCamera::Process->SetFOV (0x52C976), Open Limit
    Adjuster on Render2dStuff->CHud::Draw (0x53E4FF), CLEO+ on ->RenderPickUpText (0x53E298).
  - **Camera**: drone::camera() runs inside CCamera::Process at its SetFOV call (0x52C976): steps libfpv there, writes
    TheCamera.m_mCameraMatrix (0xB6F028+0x974: right, fwd, up, pos), RwCameraSetNearClipPlane(m_pRwCamera +0x954),
    passes our FOV on. The game then derives the frustum and copies it to RW itself. Full roll works.
    **Convention (measured): the matrix's right = up x forward = screen LEFT**; passing screen-right flipped the image.
  - FOV: RwCamera viewWindow.y (+0x6C) = 0.75 * tan(fov/2) with this WSFix setup (learned per frame anyway).
  - HUD/radar/radio name/pickup text skipped in flight; CMessages/DrawAfterFade skipped with the OSD disable switch.
  - Collision via CWorld::ProcessLineOfSight(all flags, CWorld::pIgnoreEntity = CJ) works in one call (ground, fences,
    objects); fence impact logged as object handle. Screenshots: fpv-data/shot.txt -> fpv-data/shots/*.bmp.
  - Self-test: launch via `schtasks /Run /TN gtasa-fpv-test` (task runs `cmd /c cd /d <game> && start gta_sa.exe`; it
    needs the game dir as cwd). Space skips intros; Enter, Down+Enter, Enter loads save 1. **Losing focus pauses SA**
    (main loop Sleep(100) in WinMain, then the pause menu): refocus with `um win drive --proc GTA_SA focus`, Enter = Resume.
    The first launch after load lost focus once (libfpv SDL init? unconfirmed).
  - autotest.txt knobs: at=x,y,z,heading (teleports CJ outside, sets area 0), hover, still, height=, roll, secs=,
    shots=t1,t2, cj, fov=, near=, osd=, osdoff, boom, boomsw, signal/cutscene. Grove St test spot: at=2490,-1668,13.4,0.
  - Open: interior FPV shot had an odd lower band (lens or interior rendering?); first-launch focus loss; stand-in.

- 2026-10-24 (evening): stand-in, model, kamikaze, effects **verified in game**:
  - **Stand-in** (src/standin.cpp): physical anchor like gtaiv-fpv with sm64ex thresholds (on > 20 m from CJ's spot,
    off < 10 m). CJ is Teleport()ed to the drone (re-added to world sectors every 2 m, matrix pos in between), his
    CPlayerPed::ProcessControl (vtable 0x86D168 slot 10 = 0x86D190) is skipped, collision + visible bits (+0x1C bits
    0 and 7) cleared **every frame** (the game re-shows the player by itself), damage to him ignored through a detour
    of CPedDamageResponseCalculator::ComputeDamageResponse (0x4B5AC0). FindPlayerCoors / FindPlayerCentreOfWorld /
    _NoSniperShift are detoured to return the drone (covers CJ in a vehicle: then only these follow the drone).
    Seen: with 3 stars, police shot at the drone (tracers; no hits, CJ is collision-less); a Grove homie walked after
    the drone. Move CJ here from a stand-in: CJ appears under the drone, HUD back.
  - **Drone model** for chase/LOS: D3D fixed-function triangles after Idle->RenderScene (0x53EABF), using the game's
    own VIEW/PROJECTION transforms (GetTransform works), RGB writes only (alpha writes made it glow through the post
    effects), tinted by AmbientLightColourForFrame_PedsCarsAndObjects (0xC886C4) + DirectionalLightColourForFrame.
    Still a bit bright at night.
  - **Kamikaze**: current libfpv needs `fpv_kamikaze_set_installed(1)` (player: F8 > Fly > Bomb installed); gtaiv-fpv
    predates that. Ending length from fpv_kamikaze_get_config().ending_duration. **CExplosion::AddExplosion takes the
    CVector by value** (PDB says const&; with a pointer the blast went nowhere). Signal lost static + cutscene work.
  - **Effects**: ped hit at 4.1 m/s: CWeapon::GenerateDamageEvent(victim, CJ, WEAPON_RAMMEDBYCAR, ...) 100 -> 92
    health. Line of sight against peds only hits torso/head (a drone at 0.45 m flew between a ped's legs).
  - Kept the installed bomb across flights = heavier drone (autotest now sets installed per test).
  - First-launch focus loss not reproduced after a restart with the kamikaze DLL.

  - Car impact: drone into a parked LSPD car at 6.2 m/s -> InflictDamage + ApplyMoveForce, no crash. Autotest
    targets: path-following traffic re-places itself, so the target car's status is set to ABANDONED (byte 0x36
    bits 3-7 = 4) and bikes are skipped (CAutomobile vtable 0x871120 only).
  - Long range: 420 m west of Grove St at 36 m, city fully streamed (stand-in carries streaming).
  - Docs: README.md, docs/INSTALL.md (release text), .github/workflows/release.yml (ZIP = "FPV Drone/" folder for
    modloader), LICENSE (GPL-3.0-or-later). Nothing committed yet (user decides).

## Next — start here
1. README/INSTALL, release workflow, field note.
2. Open: interior lower band in FPV; gunfire at the stand-in doesn't reach the drone (CJ has no collision);
   model brightness at night; stand-in with CJ in a vehicle (Queries mode) is untested; first-launch focus loss
   seen once.

- 2026-10-24 (night): user report: the overlay's mouse was unusable, the cursor snapped back to the centre every frame.
  Cause: Idle (0x53E9F1) calls RsMouseSetPos(screen centre) each frame unless the pause menu is up or the screen is
  faded out. SA's DirectInput mouse is non-exclusive (diMouseInit 0x7469A0: DISCL_NONEXCLUSIVE|FOREGROUND in game),
  so the OS cursor moves normally otherwise. Fix: that call is redirected and skipped while the overlay is open (ImGui
  draws its own cursor). Built and deployed; not yet verified in game.
- Esc closes the F8 overlay: libfpv's overlay now closes on Esc itself (after a popup or text field takes it). The
  game opened its pause menu on the same press: CGame::Process -> CPad::UpdatePads (0x53BEE6) is redirected and
  clears CPad::NewKeyState.esc (0xB733A8; CMenuManager::CheckForMenuClosing 0x576B70 checks New && !Old) while the
  overlay is open, and until Esc is released.
- 2026-10-24 (late night): **the pilot stays visible** (user: CJ vanished from his spot while he stood in at the
  drone). src/pilot.cpp, like sm64ex-fpv's Mario who stays home for his own update/camera/rendering: for the frame's
  render passes CJ is moved home and shown, then put back at the drone. Idle -> CRenderer::ConstructRenderList
  (0x53E9F9) swaps him in (matrix pos + bIsVisible, CEntity::UpdateRwMatrix 0x446F90 + UpdateRwFrame 0x532B00); he
  is still listed in the drone's sector, which is always scanned, so he gets PreRender/shadow/RenderScene at home;
  Idle -> RenderEffects (0x53EAD3) draws the radio and swaps him back. Bone matrices are **world space** (CPed::
  GetTransformedBonePosition uses them raw); PreRenderAfterTest sets ped flag 0x474 bit 0x400 + UpdateRpHAnim,
  CPedIntelligence::ProcessAfterPreRender clears it, so AI bone queries after the swap recompute at the drone.
  - Pose: no RC-controller model or anim in vanilla ("Zero RC" is Zero's shop; Zero holds a detonator; only
    CJ_REMOTE, a TV remote). Tried crib.ifp CRIB_Console_Loop (sits), food SHP_Tray_Pose (arms down), cop_ambient
    Copbrowse_loop; **otb.ifp betslp_loop** (reading a betting slip: head down, both hands at the waist) looks like
    a pilot. Streamed with CStreaming::RequestModel(25575 + block, keep|priority), CAnimManager::GetAnimation
    (0x4D42F0), re-blended every drone step with BlendAnimation(clump, hier, started|looped, 4) (0x4D4410: blends
    other full-body anims out with flag 0x4 = delete when faded). Released when drone::active() goes false:
    BlendAnimation(clump, ped+0x4D4 motion group, ANIM_ID_IDLE 3, 4) (0x4D4610).
  - Radio: a procedural low-poly transmitter (body, grips, orange trim, gimbals, sticks, switches, screen,
    antenna), D3D fixed-function like the drone model, placed between bones 24/25 and 34/35, face toward the neck.
    No asset files. autotest: `yawdur=<s>` (length of the yaw turn; 1.1 s is about a half turn).
  - Verified in game (kamikaze libfpv): close-up pose with the radio; from 28 m with the stand-in on, CJ in the pose
    at his spot (Grove St, day); about 64 m on the LSIA runway a CJ-sized figure at the home spot; wanted=3 with
    the stand-in on: cop car, cops aiming and firing at the drone, police helicopter's searchlight on it; CJ back in
    his idle stance after the flight. One hang (main thread spinning in the message loop) after another app took
    focus mid-test; a restart ran the same test fine.
- 2026-10-24 (late night): kamikaze ending moved to libfpv's generalized system (F8 > Kamikaze: view SIGNAL_LOST /
  CUTSCENE, duration 0..30 s, then RESPAWN / RECALL / NOTHING). The mod's own "After the blast" F8 button (action 2)
  and fpv-data/gtasa-fpv.ini are gone (a saved after_blast=cutscene is moved into libfpv once, then the file is
  deleted). kamikaze::init calls fpv_kamikaze_set_ending_support (all supported, defaults SIGNAL_LOST / RESPAWN; the
  function is optional in src/fpvapi.h, older kamikaze DLLs fall back to signal lost 3 s + respawn). Choice read at
  the detonation; timed with CTimer::m_snTimeInMilliseconds (0xB7CB84, stops in the pause menu). Duration 0 skips
  the view for RESPAWN/RECALL; NOTHING holds the view whatever the duration. During the view R / F7 / Y (and the
  F8 Move CJ here button) act at once. Autotest knobs: dur=, then=respawn|recall|nothing, endact=r|y|f7 +
  endwait=<s>, boomsw=<s>. Verified in game: signal 3 s -> respawn (3.007 s), cutscene dur=0 + nothing held until R
  at 4 s, signal 2 s -> recall, Y 1.5 s into a 10 s view (CJ on the roof at the blast), F7 2 s into a cutscene,
  Esc pause mid-view (12.9 s real for a 4 s view), libfpv's values without overrides (cutscene 3 s -> respawn).
  Seen: a blast at the launch spot (2 m) can kill CJ; the cutscene camera can end up against a wall (1 m clear);
  the pause menu is hidden under the signal-lost static.

- 2026-10-25: watching flights, frozen world and replays; sound reactions (built, **not yet tested in game**).
  - Replay: CReplay::Update 0x460500 reads rsF1/F2/F3 (1001-1003) via GetIsKeyboardKeyJustDown: F1 plays the
    buffer (TriggerPlayback), F2 saves it, F3 plays it from disk (PlayReplayFromHD); CReplay::Mode 0xA43088 (1 =
    playback). It calls CReplay::Init on any frame with CPad::DisablePlayerControls set, so our pad bit is never set
    during playback (it would drop the replay), and no replay can start during a normal flight. During playback
    CGame::Process calls CCamera::ProcessFade instead of CCamera::Process (ShouldStandardCameraBeProcessed 0x45C440),
    so the SetFOV hook doesn't run: drone::tick applies the camera itself after CGame::Process (SetFOV, then
    CalculateDerivedValues(false, true) 0x5150E0 and CopyCameraMatrixToRWCam(false) 0x50AFA0, as Process does).
  - Amazing Screenshot 1.2 (cleo/Amazingscreen.cs, Alt+V): CTimer::StartUserPause (m_UserPause 0xB7CB49) with the
    user-pause check in CGame::Process nopped (0x53BF8A/0x53BF96), so the world block runs with no time step, and
    `jmp` over CCamera::Process's SetFOV call (0x52C971): same fallback camera. Frozen is detected from the game
    (m_UserPause or ms_fTimeScale 0xB7CB64 < 0.01), not from the mod.
  - Sounds: CAudioEngine (AudioEngine 0xB6BC90) ReportFrontendAudioEvent 0x506EA0 (event, dB, speed); events seen in
    CMenuManager: 1 select, 2 back, 3 highlight, 4 error. ReportCollision 0x506EB0 called as
    CPhysical::ApplyCollision 0x5435C0 does (a, b, surfaces, point, normal, impulse/mass, 1, one-shot); a must be
    non-null (types are read). Drone = surface 68 PLASTIC.

- 2026-10-31: **wind** (libfpv `fpv_wind_*`, DECISIONS D30; built, **not yet tested in game**: the session was locked,
  so no input reached the game, and SA pauses without focus).
  - Weather: CWeather::Wind 0xC812F0 (0..0.25 sunny, 0.7 cloudy, 1.0 rainy SF/countryside, 1.5 sandstorm; table
    0x8D5E50) and WindDir 0xC813E0 (= WindClipped·0.7 per axis plus table wobble: always towards +X +Y, i.e.
    north-east; scripted 1 s / 2 s / 256 ms modulation). Checked the stores in CWeather::Update at 0x72C3C2/0x72C40C.
    src/wind.cpp low-passes WindDir over 4 s for the direction and passes Wind × 11 m/s (1.0 = a storm) to
    fpv_wind_set; interiors (CGame::currArea != 0) get FPV_WIND_ENCLOSED. libfpv does height, shelter and gusts.
  - The game's own wind: CVehicle::FlyingControl 0x6D85F0 works on the airspeed m_vecMoveSpeed − WindDir·fWindMult
    (planes, helis, RC, autogyro; mission vehicles exempt). Trees: CEntity::ModifyMatrixForTreeInWind 0x534E90 (gusty
    table from Wind 0.5). The rooftop wind sound is CAEWeatherAudioEntity scaling with the camera's z / 500.
  - CWindModifiers (Number 0xC81450, Array 0xC81458): the only RegisterOne (0x72C8B0) call is at 0x6C51E4 in
    CHeli::ProcessControl: power min(1, m_fHeliRotorSpeed·6.667) (0x866FBC), from rotor speed 0.0075. Read by
    ModifyMatrixForTreeInWind (0x53501D) and 0x5E6980. Not used: the mod scans the vehicle pool itself.
  - Helicopter downwash: m_nVehicleSubType +0x594 == 3 (CHeli ctor stores it at 0x6C41E9), m_fHeliRotorSpeed +0x84C
    (CHeli::ProcessControl reads it), CPhysical::m_fMass +0x8C (ApplyMoveForce 0x542A07). Rotor radius 0.42 × the
    col box's length (CEntity::GetColModel 0x535300: ms_modelInfoPtrs 0xA9B0C8 [+0x22 model] -> +0x14 col model),
    hub at the box's top. Thrust = weight × (rotor / 0.15)², induced velocity by momentum theory.
  - Explosions: CExplosion::aExplosions 0xC88950, 16 × 0x7C (AddExplosion 0x736A50 walks them; active counter
    +0x28, radius +0x10 set to 9 or 6). A slot that turns active (or moves) is a blast, radius × 0.5; molotovs
    (type 1) don't blast.
  - autotest: weather=<type> forces the weather at launch (ForcedWeatherType 0xC81318, Old 0xC81320, New 0xC8131C,
    as ForceWeatherNow does; its entry at 0x72A4F0 jumps through the crack's stub).
  - The drone trace line logs frame.wind; src/wind.cpp logs the weather wind and rotor sources every 10 s and each
    blast. The wind functions are optional in src/fpvapi.h: an older libfpv.dll flies in still air.
  - To verify in game: `at=2490,-1668,13.4,0 hover weather=8 secs=20` (Grove St, storm): the trace's wind should be
    a few m/s near the ground, the drone drifting north-east; then the same from a rooftop and beside a police
    helicopter (wanted=3).


- 2026-10-31: libfpv's shared data folder. fpvapi::load passes fpv_shared_data_dir() (%APPDATA%\libfpv, or
  LIBFPV_DATA_DIR) as cfg.data_dir, so radio mappings, EEPROMs and settings.json follow the pilot between libfpv
  games. First run: if the shared folder doesn't exist, fpv-data is copied into it (minus the mod's own files:
  shots/, autotest.txt, shot.txt, gtasa-fpv.ini; the old folder is kept); if it exists, it wins. The function is
  optional in src/fpvapi.h: an older libfpv.dll, or a failed call, keeps fpv-data. Mod-only files (autotest and
  shot triggers, screenshots, the old ini) stay in fpv-data. The UTF-8 path goes through fs::u8path. Built; **not
  yet verified in game** (check the log's "data in" line and the "moved settings" line).
