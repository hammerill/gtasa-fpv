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

- 2026-10-31 (afternoon): wind **verified in game** (Big Smoke save, CJ walked out of the house first: the autotest
  teleport from an interior leaves the black-and-white interior sky, so it doesn't count as outside).
  - User report: sometimes no wind at all, even on top of Maze Bank. The log showed CWeather::Wind easing from 0.20
    to 0.00 and back (an extra sunny weather type, 0 in the game's table) while the game's wind sound still blew up
    high (it scales with the camera's height). Fix: the weather wind is at least 2 m/s (kCalmMps).
  - Calm (weather=0, game Wind 0.00), Maze Bank roof: about 0.7 m/s on the roof's surface, about 3.8 m/s
    hovering 1.5 m above it, drifting north-east 25 m in 10 s.
  - Storm (weather=8), Grove St at 13 m: mean about 3 m/s, gusts to 5.7 m/s with short reversals between the
    houses, 17 m of drift in 10 s.
  - Storm, Maze Bank roof: 7-9 m/s on the roof, 21-23 m/s once the drone drifted off its edge (the log profile at
    330 m), carried about 150 m in 10 s. Real, and hard to fly in: the F8 slider scales it.
  - autotest: weather=-1 gives the weather back to the game (ForcedWeatherType -1). Seen: a core hot-reload re-runs
    the last autotest.txt (the trigger time is not kept across reloads); delete the file after a test.

- 2026-10-31 (evening): stand-in leaves the pilot's spot without ground (user report: from a car, flying far makes
  the car sink into the ground and get deleted; on foot CJ drops through and is put back on the ground from below).
  - Cause: CStreaming::LoadScene (0x40EB70) loads models and IPLs but never collision; CColStore streams that around
    FindPlayerCoors each frame, which the stand-in moves to the drone, so the spot's col files get unloaded. The
    held car then falls through; and CVehicle::CanBeDeleted (0x6D1180) lets the population remove a random (1) or
    parked (3) car even with CJ driving it (ped state 0x32), once it's far from FindPlayerCentreOfWorld.
  - Fix (not yet verified in game): Queries mode holds the car (matrix kept, speeds zeroed every step) only while the
    drone is over 100 m from it, so near events (a truck pushing it) still move it; and sets m_nCreatedBy (+0x4A4) to
    mission (2) until the stand-in ends. Every return home (car or CJ) and "Move CJ here"
    calls CColStore::EnsureCollisionIsInMemory (0x410AD0) after LoadScene: it requests the col files covering the
    point and loads them right away.

- 2026-10-31 (night): **stand-in from a vehicle works like on foot** (user: peds should look at the drone as if it
  were CJ, and pickups should work, also when he flies from a car). Built and deployed, **not yet verified in game**
  (the user was at the keyboard).
  - Why it didn't: Queries mode (CJ stays in his car) moves only FindPlayer*. CPickup::Update (0x457410) tests a
    player in a vehicle with CVehicle::IsSphereTouchingVehicle on his car, on foot with his ped position (|dz| < 2,
    xy distance); peds' looks (IKChainManager_c, g_ikChainMan 0xC15448) go for his ped's head bone.
  - Now, driving (not a passenger, not on a mission: OnAMissionFlag 0xA476AC into ScriptSpace 0xA49960, since
    IsPlayerOnAMission 0x464D50 is behind a crack stub), CJ gets out on the spot as WARP_CHAR_FROM_CAR_TO_COORD
    does (0x482EFC: CPedIntelligence::FlushImmediately(true) 0x601640, intelligence at +0x47C) and stands in in Body
    mode. Back behind the wheel at the end as WARP_CHAR_INTO_CAR does (0x483267: FlushImmediately(false), stack
    CTaskSimpleCarSetPedInAsDriver(car, null) 0x6470E0 with +0x1C warp set, ProcessPed 0x64B950, dtor 0x647170),
    also before Move CJ here (so the car still goes along). The car keeps the earlier hold/mission-car logic.
    Still Queries mode when on a mission, as a passenger, or if he didn't get out.
  - pilot.cpp draws him in his seat: his matrix in the car's frame is kept from the moment he got out, and the anim
    he sat with (the strongest non-partial association: hierarchy +0x14, blend +0x18, flags +0x2E, ctor 0x4CEFC0;
    a ref on its block, CAnimBlendHierarchy +0xC) is blended every step instead of betslp_loop. No radio there.
  - Placement: his head bone (5, CPed::GetTransformedBonePosition 0x5E01C0) is put on the drone, not his root, so
    the look-at IK aims right at the quad; FindPlayerCoors is still the drone.
  - autotest: "incar" (nearest empty car beside CJ, warped in before the launch), pickup=<m> (pistol pickup on the
    path at cruise height; its type byte +0x1C and CJ's pistol ammo logged), and every 2 s the count of peds within
    60 m whose look-at entity is CJ. To verify: `at=2490,-1668,13.4,0 incar pickup=30 secs=20 shots=12` (Grove St).
- 2026-11-01: the car radio keeps playing while CJ is out of his car standing in. CAEVehicleAudioEntity::Service
  (0x502280) calls TurnOffRadioForVehicle (0x4F5B60, site 0x5023A3) the frame the player stops driving and
  TurnOnRadioForVehicle (0x4F5B20, site 0x50237C; it sets the radio globals 0xB6B98C driver / 0xB6B99C settings
  and calls CAudioEngine::StartRadio) when he drives again. Both redirected in standin.cpp: "off" is skipped for
  CJ's car in a seated stand-in (logged "radio kept on"), the next "on" for that car is skipped (still playing;
  logged), and if the warp back in fails the "off" runs then. The audio entity is CVehicle +0x138 (Initialise at
  0x6B12E4), its vehicle at +0x4. Built and deployed with the kamikaze libfpv.
  - Launching from WSL: `cmd /c start` and `explorer.exe gta_sa.exe` both left a GTA_SA.exe with no window
    (about 11 MB, stuck; the first had loaded the ASIs). The gtasa-fpv-test scheduled task no longer exists, and
    re-creating it was blocked by the session's permission rules.
- 2026-11-01: stand-in from a vehicle **verified in game** (kamikaze libfpv, LSIA runway at 1700,-2493 heading 270,
  autotest `incar`: no empty car near Grove St, so it creates a Greenwood (CCarCtrl::CreateCarForScript 0x431F80,
  set to a random car) or takes a traffic car after its driver gets out):
  - At 20 m: "CJ out of his vehicle (seat anim found)", "radio kept on"; his head bone within 0.1 m of the drone
    for the whole stand-in. A pistol pickup dropped on the drone's path (autotest pickup=<s>: at that time, 1 s
    ahead) was collected: pickup gone, CJ's pistol ammo 0 -> 34.
  - Looking back from 13 m: CJ drawn seated behind the wheel of the parked car.
  - End of flight: "CJ back in his vehicle", "radio still on" (the next TurnOnRadio skipped).
  - Not shown: peds' look-at counts. Grove St (north: houses) and Pershing Square (a wall at x 1453) stopped the
    scripted flight inside 20 m, and the runway has no peds. The placement puts his head on the drone; the
    look-at itself is up to the user's own flight.
- 2026-11-01: the weather wind floor (kCalmMps) is 0.5 m/s, a quarter of the 2 m/s before (user: too windy when
  calm). Deployed; not flown yet with weather 0.
- 2026-11-01: user: peds react fine to the stand-in. New bug: fly 100-200 m from the car, then F7, and the car
  is on its back. Traced the car's pose (temporary `DEBUG car` log lines in standin.cpp, plus
  standin::debug_tick from core.cpp for 4 s after the stand-in ends):
  - LSIA runway: nothing. It was held level and stayed level after CJ got back in.
  - The user's rooftop (2044.6,-1401,68.5, autotest `at=2044.6,-1401.0,68.5,175 incar secs=28 height=5
    pitch=0.8`): held level too, but on release (CJ warped back in) its first physics step shoved it up 0.4-0.55 m
    in one frame (a push-out, not speed: move speed only 0.06-0.09) and it rolled 17 deg in 7 ms; in the user's
    flight it rolled to 60 deg and back, the direction differing from run to run.
  - What we saw: CAutomobile's wheel compression (+0x7D4, 1 = off the ground) goes to 1.00 on all wheels the
    frame CJ gets out, the car free and the ground loaded: an empty car isn't simulated, it hangs in its pose.
    A hover test (never 100 m away, so never held) came back cleanly. After a hold it didn't, even with the
    roof's collision loaded again (line probes hit it at 67.3 under every corner).
  - Tried, didn't help: CAutomobile::Teleport (vtable 14, 0x6A9CA0) to the held pose; restoring a snapshot of
    the wheel contact state (m_wheelColPoint[4] 0x724, compression 0x7D4 and the next array: it read 1.00
    already); CAutomobile::PlaceOnRoadProperly (0x6AF420, a jump stub in this exe) alone: level at 68.10, but
    still thrown 1 m up on the first step.
  - Fix, **verified** (3 rooftop runs in a row, the car still to the cm, wheels 0.85-0.87 after): a held car, on
    release or when the drone comes back near, is put down with PlaceOnRoadProperly, then pinned in that pose
    (speeds zeroed) for 500 ms of game time by standin::tick (every frame, from core.cpp) while its wheels find
    the ground. Logged as "standin: CJ's car set down at ...". The debug tracing is gone.
- 2026-11-03: adapted to libfpv 52be06c (the pilot API, views, damage). Not flown yet; the user tests.
  - F in flight: the pilot view. CJ is the player's again (the game's camera, HUD and controls; the stand-in
    ends, CJ home), the drone stays in the world and keeps ticking, drawn from the game's camera. libfpv hears
    it from CJ: `fpv_set_view(FPV_VIEW_PILOT)`, the game camera as `fpv_audio_set_listener` every frame,
    `fpv_drone_set_pilot_position` follows CJ. The keyboard's flight keys (W/A/S/D, arrows, Enter, T, B, K, X)
    go to CJ, not libfpv, while the overlay is closed. F7 goes back to the drone (a kamikaze blast's view still
    on shows as lost signal); C does nothing there (crouch). Refused while watching a replay or a frozen world.
  - The pilot position is CJ's feet now (his position minus 1 m), so the LOS camera and the ear are 1.6 m above
    the ground instead of 2.6 m.
  - `fpv_pilot_set_support(STAND_IN | POSE, 20, 10)` after init; standin.cpp reads the distances and the switch
    from `fpv_pilot_get_config`. The pose plays while `FPV_DRONE_RADIO_ACTIVE` and CJ stands still on foot
    (always with an older libfpv), and fades back to his idle otherwise.
  - damage.cpp, `fpv_drone_damage_props` per prop: under water (CWaterLevel::GetWaterLevel 0x6EB690) and in
    fire (gFireManager 0xB71F80, 60 CFire of 0x28; 53 references to it in the exe) 0.5/s; a fresh explosion
    within its radius 1 on all four; a turning heli rotor disc 1 on all four; a bullet passing within 7.4 cm of
    a prop 0.75 (detour of CWeapon::DoBulletImpact 0x73B550, prologue `6A FF 68 50 8E 84 00` read from the exe).
    Bullets that hit nothing behind the drone don't reach DoBulletImpact and do nothing; melee is not covered.
- 2026-11-03 (later): the user's test of the above, and fixes. Built and deployed; not flown yet (the user tests).
  - **Pose broken by CJ's idle moves** in the F view: CTaskSimplePlayerOnFoot's PlayIdleAnimations (0x6872C0,
    thiscall(task, CPlayerPed*), `ret 4`; its only call is 0x6887E2) plays the PLAYIDLES block (anims
    0x105-0x108: shoulder roll, stretches, watch) after 10 s without pad input, blending the pose out. The call
    is redirected and skipped while CJ is posed on foot.
  - **Respawn at the pilot**: R and the kamikaze ending's "respawn" now put the drone in front of CJ where he is
    (after the stand-in brings him home), as at a launch (place_spawn, shared with start). A watching flight
    still respawns at its launch point.
  - **The game's sounds heard from the pilot** (libfpv `FPV_DRONE_HEARD_FROM_PILOT`, i.e. "Hear the drone
    from: The pilot", or Auto in LOS): ear.cpp. SA positions every sound relative to TheCamera:
    CAEAudioEnvironment::GetPositionRelativeToCamera (0x4D80B0, cdecl(out, pos); the CPlaceable overload at
    0x4D8340 jumps into it) builds a CMatrix from m_mCameraMatrix (0xB6F99C) and TheCamera.m_matrix's position
    (0xB6F03C, fallback 0xB6F02C), and the vehicle/ped/weapon/weather entities check distances to the camera
    themselves. Vehicle audio is serviced from each vehicle's ProcessControl (CAEVehicleAudioEntity::Service
    0x502280, called from 0x6B18F1, 0x6B9298, 0x6F1793, 0x6F86B6), not from CAudioEngine::Service (Idle
    0x53E98B, after CGame::Process), so swapping the camera around the audio service would not do. Instead,
    while on, the 85 operands in 0x4D0000-0x508000 that are TheCamera, its matrix pointer, placement position or
    camera matrix (found with objdump; each checked before writing, all or nothing) point at a fake camera at
    `frame.los_camera` (the pilot's head facing the drone). Left alone: 0x4D7E44 (Doppler reads a flag). Off
    in the pilot view (the camera is on CJ) and on unload.
  - **Stand-in vs. this** (asked to research): not a technical conflict. The relocation works with the stand-in
    on (CJ at the drone only moves GetPositionRelativeToCamera's ear up to 0.5 m towards him). What changes is
    what there is to hear: with the stand-in, population and traffic follow FindPlayer* (the drone), so once
    the drone is far the pilot's surroundings empty out and go quiet. Left to the player (turn the stand-in
    off for a living world around the pilot), noted in libfpv's INTEGRATION.md.
  - **Wobbly drone sound** (LOS, 20-50 m): libfpv's synth followed the travel time's per-frame staircase within
    each audio block, a pitch swing of 15 % at 25 fps (measured by libfpv's new fpv_synth_tests). Fixed in
    libfpv: three 40 ms one-pole stages per sample (under 0.3 %), and the ground reflection low-passed at
    1.5 kHz (it flanged the whole sound).
- 2026-11-04: user report: after a kamikaze blast the respawn put the drone back near the blast, at "CJ" who
  stands in there. Log: CJ stood in from his car; standin::end warped him back behind the wheel, but place_spawn
  read cj->position(), still at the drone (a ped in a car only follows it on the next frame). In a vehicle the
  spawn is now measured from the car's matrix. And the respawn in front of CJ only happens once the pilot view
  (F) was used this flight; otherwise R and the ending's respawn go back to the launch point, as before.
- 2026-11-04: user report: F into the pilot view also made CJ get into a car nearby. to_pilot released CJ's
  controls in the same frame, so the game saw F (its enter-vehicle key) go down. CJ now stays held until F is
  released; by then the game's old and new pad state agree, so there is no press for it to act on.
- 2026-11-04: the pilot pose is gone from the F view (user's call). Posing CJ again whenever he stood still
  there fought the game (its idle moves broke the pose, which needed PlayIdleAnimations skipped). Now CJ is the
  player's in the pilot view, with the game's own anims, and the radio still flies the drone. Removed: the
  PlayIdleAnimations redirect (0x6887E2), the stand-still check, and release's walk-off case. The pose plays in
  the drone's views only (radio in use, F8 pose on); libfpv's INTEGRATION.md, header and overlay hint say so.
