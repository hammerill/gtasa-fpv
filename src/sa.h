// GTA San Andreas 1.0 US addresses and layouts the mod uses. Each one was
// checked against the community gta_sa.pdb and the disassembly; see MODLOG.md.
#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace sa {

struct CVector {
    float x, y, z;
};

// CMatrix: right, forward, up, pos (each padded to 16 bytes), then the
// attached RwMatrix pointer and whether it owns it.
struct CMatrix {
    CVector right;
    uint32_t pad0;
    CVector forward;
    uint32_t pad1;
    CVector up;
    uint32_t pad2;
    CVector pos;
    uint32_t pad3;
    void *attach;
    bool owns_attach;
};

// CPlaceable at the start of every CEntity: vtable, CSimpleTransform
// (pos, heading) used while there is no matrix, then the matrix pointer.
struct CPlaceable {
    void *vtable;
    CVector placement_pos;
    float placement_heading;
    CMatrix *matrix;

    const CVector &position() const { return matrix ? matrix->pos : placement_pos; }
};
using CEntity = CPlaceable;
using CPed = CPlaceable;
using CVehicle = CPlaceable;

// CEntity::m_nType, the low 3 bits of the byte at +0x36.
enum class EntityType : uint8_t { Nothing, Building, Vehicle, Ped, Object, Dummy };
inline EntityType type_of(const CEntity *e) { return EntityType(reinterpret_cast<const uint8_t *>(e)[0x36] & 7); }

// CColPoint, 0x2C bytes.
struct CColPoint {
    CVector point;
    float pad0;
    CVector normal;
    float pad1;
    uint8_t surface_a, piece_a, lighting_a;
    uint8_t surface_b, piece_b, lighting_b;
    uint8_t pad2[2];
    float depth;
};
static_assert(sizeof(CColPoint) == 0x2C, "CColPoint");

// --- Code sites we redirect (call rel32 at `site`, vanilla target `fn`) ---
struct CallSite {
    uintptr_t site, fn;
};
// Idle (0x53E920), the in-game frame.
constexpr CallSite kIdleGameProcess = {0x53E981, 0x53BEE0};   // CGame::Process()
constexpr CallSite kIdleEndUpdate = {0x53EBF5, 0x7EE180};     // RwCameraEndUpdate(Scene.camera)
constexpr CallSite kIdleRenderScene = {0x53EABF, 0x53DF40};   // RenderScene()
constexpr CallSite kIdleHudAfterFade = {0x53EBA2, 0x58D490};  // CHud::DrawAfterFade()
constexpr CallSite kIdleMessages = {0x53EBA9, 0x69EFC0};      // CMessages::Display(false)
// RsMouseSetPos(&centre), every frame outside the pause menu (non-exclusive DirectInput mouse,
// so the OS cursor is what gets pinned).
constexpr CallSite kIdleMouseSetPos = {0x53E9F1, 0x6194A0};
// CGame::Process (0x53BEE0): the pads update, then CMenuManager::Process opens the pause menu on Esc.
constexpr CallSite kGameUpdatePads = {0x53BEE6, 0x541DD0};  // CPad::UpdatePads()
// FrontendIdle (0x53E770), the menus.
constexpr CallSite kFrontendUpdatePads = {0x53E78B, 0x541DD0};  // CPad::UpdatePads()
constexpr CallSite kFrontendEndUpdate = {0x53E87D, 0x7EE180};
// Render2dStuff (0x53E230).
constexpr CallSite kRender2dPickupText = {0x53E298, 0x455000};  // CPickups::RenderPickUpText()
constexpr CallSite kRender2dRadioName = {0x53E4FA, 0x507030};   // AudioEngine.DisplayRadioStationName(), thiscall
constexpr CallSite kRender2dHud = {0x53E4FF, 0x58FAE0};         // CHud::Draw()
constexpr CallSite kRender2dMessages = {0x53E515, 0x69EFC0};    // CMessages::Display(true)
// CCamera::Process (0x52B730): CDraw::SetFOV(fov), right before
// CalculateDerivedValues (frustum) and CopyCameraMatrixToRWCam.
constexpr CallSite kCameraSetFov = {0x52C976, 0x6FF410};
// CCamera::ImproveNearClip: hides the peds close to the game's own camera
// (they come back next frame), a function the crack moved to .HOODLUM.
constexpr CallSite kNearClipHidePeds = {0x517115, 0x4154A0};

// Detour targets (function entry, the whole instructions we move, their bytes).
// FindPlayerCoors(int) returns its CVector through a hidden pointer; the
// FindPlayerCentreOfWorld pair return a const CVector&.
constexpr uintptr_t kFindPlayerCoors = 0x56E010;  // mov eax,[esp+8]; xor edx,edx
constexpr uint8_t kFindPlayerCoorsPrologue[] = {0x8B, 0x44, 0x24, 0x08, 0x33, 0xD2};
constexpr uintptr_t kFindPlayerCentreOfWorld = 0x56E250;  // mov eax,[esp+4]; test eax,eax
constexpr uint8_t kFindPlayerCentreOfWorldPrologue[] = {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0};
constexpr uintptr_t kFindPlayerCentreNoSniper = 0x56E320;  // mov al,[0x9690C1]
constexpr uint8_t kFindPlayerCentreNoSniperPrologue[] = {0xA0, 0xC1, 0x90, 0x96, 0x00};
// CPedDamageResponseCalculator::ComputeDamageResponse(CPed*, CPedDamageResponse&, bool), thiscall, ret 0xC.
constexpr uintptr_t kComputeDamageResponse = 0x4B5AC0;  // mov eax,fs:[0]
constexpr uint8_t kComputeDamageResponsePrologue[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
// CPlayerPed vtable (0x86D168), slot 10: ProcessControl (0x60EA90).
constexpr uintptr_t kPlayerPedProcessControlSlot = 0x86D190;

// CPedDamageResponse, filled by ComputeDamageResponse.
struct CPedDamageResponse {
    float damage_health, damage_armour;
    bool health_zero, force_death, damage_calculated, check_if_affects_ped;
};
// CPedDamageResponseCalculator: damager, damage, body part, weapon type, speak.
struct CPedDamageResponseCalculator {
    CEntity *damager;
    float damage;
    int piece;
    int weapon;
    bool speak;
};

// --- Data ---
constexpr uintptr_t kTheCamera = 0xB6F028;
constexpr uintptr_t kCameraRwCamera = 0x954;  // CCamera::m_pRwCamera
constexpr uintptr_t kCameraMatrix = 0x974;    // CCamera::m_mCameraMatrix
inline CMatrix &camera_matrix() { return *reinterpret_cast<CMatrix *>(kTheCamera + kCameraMatrix); }
inline void *camera_rw() { return *reinterpret_cast<void **>(kTheCamera + kCameraRwCamera); }
inline void *scene_camera() { return *reinterpret_cast<void **>(0xC1703C); }  // Scene.camera

// RwCamera: viewWindow (tan of the half angles) at +0x68, near/far planes at +0x80/+0x84.
inline float rw_view_window_x(void *cam) { return *reinterpret_cast<float *>(static_cast<uint8_t *>(cam) + 0x68); }
inline float rw_view_window_y(void *cam) { return *reinterpret_cast<float *>(static_cast<uint8_t *>(cam) + 0x6C); }
inline float rw_near(void *cam) { return *reinterpret_cast<float *>(static_cast<uint8_t *>(cam) + 0x80); }

inline CPlaceable *&ignore_entity() { return *reinterpret_cast<CPlaceable **>(0xB7CD68); }  // CWorld::pIgnoreEntity

// CPad::DisablePlayerControls, a bit mask; we own one bit of it.
constexpr uintptr_t kPadDisableControls = 0x10E;
constexpr uint16_t kPadDisabledByFpv = 0x4000;

// CPad::NewKeyState.esc (CKeyboardState at 0xB73190, esc at +0x218): the pause menu opens on its
// press, so clearing it after the pads update hides Esc from the game.
inline int16_t &new_key_esc() { return *reinterpret_cast<int16_t *>(0xB733A8); }

// eWeaponType / ePedPieceTypes / eExplosionType values we use.
constexpr int kWeaponRammedByCar = 49;
constexpr int kPedPieceTorso = 3;
constexpr int kExplosionGrenade = 0;

// --- Functions ---
inline CPed *FindPlayerPed(int player = -1) { return reinterpret_cast<CPed *(__cdecl *)(int)>(0x56E210)(player); }
inline CVehicle *FindPlayerVehicle(int player = -1, bool include_remote = false) {
    return reinterpret_cast<CVehicle *(__cdecl *)(int, bool)>(0x56E0D0)(player, include_remote);
}
inline uint8_t *GetPad(int pad = 0) { return reinterpret_cast<uint8_t *(__cdecl *)(int)>(0x53FB70)(pad); }
inline uint16_t &pad_disabled_controls() {
    return *reinterpret_cast<uint16_t *>(GetPad(0) + kPadDisableControls);
}

inline bool ProcessLineOfSight(const CVector &from, const CVector &to, CColPoint &out, CEntity *&entity,
                               bool buildings, bool vehicles, bool peds, bool objects, bool dummies,
                               bool see_through, bool camera_ignore, bool shoot_through) {
    using Fn = bool(__cdecl *)(const CVector &, const CVector &, CColPoint &, CEntity *&, bool, bool, bool, bool, bool,
                               bool, bool, bool);
    return reinterpret_cast<Fn>(0x56BA00)(from, to, out, entity, buildings, vehicles, peds, objects, dummies,
                                          see_through, camera_ignore, shoot_through);
}

// Script handles (as CLEO scripts see them) for pool entities.
inline int GetPedRef(CPed *p) { return reinterpret_cast<int(__cdecl *)(CPed *)>(0x54FF60)(p); }
inline CPed *GetPed(int h) { return reinterpret_cast<CPed *(__cdecl *)(int)>(0x54FF90)(h); }
inline int GetVehicleRef(CVehicle *v) { return reinterpret_cast<int(__cdecl *)(CVehicle *)>(0x54FFC0)(v); }
inline CVehicle *GetVehicle(int h) { return reinterpret_cast<CVehicle *(__cdecl *)(int)>(0x54FFF0)(h); }
inline int GetObjectRef(CEntity *o) { return reinterpret_cast<int(__cdecl *)(CEntity *)>(0x550020)(o); }
inline CEntity *GetObject(int h) { return reinterpret_cast<CEntity *(__cdecl *)(int)>(0x550050)(h); }

// CPools::ms_pPedPool: {objects, flags (bit 7 = free), size}; peds are 0x7C4 bytes.
struct Pool {
    uint8_t *objects;
    uint8_t *flags;
    int size;
};
inline Pool *ped_pool() { return *reinterpret_cast<Pool **>(0xB74490); }
constexpr int kPedSize = 0x7C4;
inline Pool *vehicle_pool() { return *reinterpret_cast<Pool **>(0xB74494); }
constexpr int kVehicleSize = 0xA18;

inline void SetHelpMessage(const char *text) {
    reinterpret_cast<void(__cdecl *)(const char *, bool, bool, bool)>(0x588BE0)(text, false, false, false);
}
// CStreaming::LoadScene loads models and IPLs, not collision: CColStore keeps
// that around FindPlayerCoors each frame (CStreaming::Update), so a spot the
// player left has no ground until the next frames stream it.
inline void LoadScene(const CVector &at) { reinterpret_cast<void(__cdecl *)(const CVector &)>(0x40EB70)(at); }
// CColStore::EnsureCollisionIsInMemory: requests the col files covering `at`
// and, if any was missing, loads them right away (CTimer suspended).
inline void EnsureCollisionIsInMemory(const CVector &at) {
    reinterpret_cast<void(__cdecl *)(const CVector &)>(0x410AD0)(at);
}
// CEntity::Teleport(CVector, bool reset_rotation), virtual slot 14 (CPed's
// is 0x5E4110, CAutomobile's 0x6A9CA0; both pop 0x10 bytes).
inline void Teleport(CEntity *e, CVector at, bool reset_rotation) {
    using Fn = void(__attribute__((thiscall)) *)(CEntity *, CVector, int);
    reinterpret_cast<Fn>(static_cast<void **>(e->vtable)[14])(e, at, reset_rotation);
}
inline void SetHeading(CPlaceable *p, float radians) {
    reinterpret_cast<void(__attribute__((thiscall)) *)(CPlaceable *, float)>(0x43E0C0)(p, radians);
}
// CPhysical::ApplyMoveForce: move speed (metres per 1/50 s) += force / mass.
inline void ApplyMoveForce(CEntity *e, CVector force) {
    reinterpret_cast<void(__attribute__((thiscall)) *)(CEntity *, CVector)>(0x5429F0)(e, force);
}
inline void GenerateDamageEvent(CPed *victim, CEntity *creator, int weapon, int damage, int piece, int direction) {
    reinterpret_cast<void(__cdecl *)(CPed *, CEntity *, int, int, int, int)>(0x73A530)(victim, creator, weapon, damage,
                                                                                       piece, direction);
}
inline void InflictDamage(CVehicle *v, CEntity *damager, int weapon, float damage, CVector at) {
    reinterpret_cast<void(__attribute__((thiscall)) *)(CVehicle *, CEntity *, int, float, CVector)>(0x6D7C90)(
        v, damager, weapon, damage, at);
}
// CExplosion::AddExplosion. The position goes by value (the code reads three
// floats from the stack), although the PDB says CVector const&.
inline bool AddExplosion(CEntity *victim, CEntity *creator, int type, CVector at, uint32_t time, bool sound,
                         float shake, bool invisible) {
    using Fn = bool(__cdecl *)(CEntity *, CEntity *, int, CVector, uint32_t, uint8_t, float, uint8_t);
    return reinterpret_cast<Fn>(0x736A50)(victim, creator, type, at, time, sound, shake, invisible);
}
inline void *RwCameraSetNearClipPlane(void *cam, float near_clip) {
    return reinterpret_cast<void *(__cdecl *)(void *, float)>(0x7EE1D0)(cam, near_clip);
}
inline IDirect3DDevice9 *GetD3DDevice() { return reinterpret_cast<IDirect3DDevice9 *(__cdecl *)()>(0x7F9D50)(); }

// CEntity flags at +0x1C: bit 0 bUsesCollision, bit 7 bIsVisible.
inline uint32_t &entity_flags(CEntity *e) { return *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(e) + 0x1C); }
constexpr uint32_t kEntityUsesCollision = 1u << 0, kEntityVisible = 1u << 7;
// CPhysical::m_vecMoveSpeed / m_vecTurnSpeed.
inline CVector &move_speed(CEntity *e) { return *reinterpret_cast<CVector *>(reinterpret_cast<uint8_t *>(e) + 0x44); }
inline CVector &turn_speed(CEntity *e) { return *reinterpret_cast<CVector *>(reinterpret_cast<uint8_t *>(e) + 0x50); }

// Interiors: CGame::currArea and CEntity::m_nAreaCode (0 = outside).
inline int &current_area() { return *reinterpret_cast<int *>(0xB72914); }
inline uint8_t &area_of(CEntity *e) { return reinterpret_cast<uint8_t *>(e)[0x2F]; }
// CWorld::Remove / CWorld::Add: files an entity out of / into the world sectors.
inline void WorldRemove(CEntity *e) { reinterpret_cast<void(__cdecl *)(CEntity *)>(0x563280)(e); }
inline void WorldAdd(CEntity *e) { reinterpret_cast<void(__cdecl *)(CEntity *)>(0x563220)(e); }
inline void RemoveBuildingsNotInArea(int area) { reinterpret_cast<void(__cdecl *)(int)>(0x4094B0)(area); }

// CPed::m_fCurrentRotation / m_fAimingRotation (radians): a ped's heading
// is these, the matrix follows them.
inline void set_ped_heading(CPed *p, float radians) {
    auto *b = reinterpret_cast<uint8_t *>(p);
    *reinterpret_cast<float *>(b + 0x558) = radians;
    *reinterpret_cast<float *>(b + 0x55C) = radians;
    SetHeading(p, radians);
}

// CPlayerPed::SetWantedLevel(int), thiscall.
inline void SetWantedLevel(CPed *player, int level) {
    reinterpret_cast<void(__attribute__((thiscall)) *)(CPed *, int)>(0x609F10)(player, level);
}

// This frame's light for peds, cars and objects (RwRGBAReal, 0..1): the
// time-of-day ambient and the sun/moon's directional colour.
inline const float *ambient_light_objects() { return reinterpret_cast<const float *>(0xC886C4); }
inline const float *directional_light() { return reinterpret_cast<const float *>(0xC886B4); }

// CTimer::m_snTimeInMilliseconds: game time, stops while the game is paused.
inline uint32_t timer_ms() { return *reinterpret_cast<volatile uint32_t *>(0xB7CB84); }

// CTimer::m_UserPause (also what freeze-the-world mods such as Amazing
// Screenshot's Alt+V set) and CTimer::ms_fTimeScale (the script's set_gamespeed).
inline bool user_paused() { return *reinterpret_cast<volatile uint8_t *>(0xB7CB49) != 0; }
inline float time_scale() { return *reinterpret_cast<volatile float *>(0xB7CB64); }

// CReplay::Mode: 1 while a replay plays back (F1: the last seconds, F3: the
// one saved with F2). CReplay::Update calls CReplay::Init (the buffer and the
// playback are dropped) on every frame with CPad::DisablePlayerControls set.
inline bool replay_playing() { return *reinterpret_cast<volatile uint8_t *>(0xA43088) == 1; }

// CDraw::ms_fFOV, what CDraw::SetFOV was last given.
inline float draw_fov() { return *reinterpret_cast<float *>(0x8D5038); }
inline void SetFOV(float fov) { reinterpret_cast<void(__cdecl *)(float)>(0x6FF410)(fov); }
// What CCamera::Process does right after SetFOV (with false, true and false),
// for frames where it doesn't run (replays: CReplay::ProcessReplayCamera
// instead) or skips SetFOV (Amazing Screenshot jumps over the call).
inline void CalculateDerivedValues(bool a, bool b) {
    reinterpret_cast<void(__attribute__((thiscall)) *)(uintptr_t, bool, bool)>(0x5150E0)(kTheCamera, a, b);
}
inline void CopyCameraMatrixToRWCam(bool a) {
    reinterpret_cast<void(__attribute__((thiscall)) *)(uintptr_t, bool)>(0x50AFA0)(kTheCamera, a);
}

// The game's own sounds. Frontend events as the menus use them (volume in dB, speed 1).
constexpr uintptr_t kAudioEngine = 0xB6BC90;
enum FrontendSound { kSoundSelect = 1, kSoundBack = 2, kSoundHighlight = 3, kSoundError = 4 };
inline void ReportFrontendAudioEvent(int event, float volume_db = 0, float speed = 1) {
    reinterpret_cast<void(__attribute__((thiscall)) *)(uintptr_t, int, float, float)>(0x506EA0)(kAudioEngine, event,
                                                                                               volume_db, speed);
}
// CAudioEngine::ReportCollision, as CPhysical::ApplyCollision calls it: the
// two entities (a never null), their surfaces, the point, the normal (may be
// null), the speed change in metres per 1/50 s, 1, one-shot.
inline void ReportCollision(CEntity *a, CEntity *b, uint8_t surface_a, uint8_t surface_b, CVector &at,
                            CVector *normal, float impact, float volume, bool one_shot) {
    using Fn = void(__attribute__((thiscall)) *)(uintptr_t, CEntity *, CEntity *, int, int, CVector &, CVector *, float,
                                                 float, int, int);
    reinterpret_cast<Fn>(0x506EB0)(kAudioEngine, a, b, surface_a, surface_b, at, normal, impact, volume, one_shot, 0);
}
constexpr uint8_t kSurfacePlastic = 68;  // data/surfinfo.dat

// CClock: the game's time of day.
inline uint8_t &clock_hours() { return *reinterpret_cast<uint8_t *>(0xB70153); }
inline uint8_t &clock_minutes() { return *reinterpret_cast<uint8_t *>(0xB70152); }

// CPed::m_fHealth.
inline float &ped_health(CPed *p) { return *reinterpret_cast<float *>(reinterpret_cast<uint8_t *>(p) + 0x540); }

// CWeather, set in CWeather::Update (0x72C398 on): Wind, the map's one wind by
// weather type (0..1.5), and WindDir, its direction scaled by WindClipped
// (always towards +X +Y, north-east, with a scripted wobble).
inline float weather_wind() { return *reinterpret_cast<volatile float *>(0xC812F0); }
inline const CVector &weather_wind_dir() { return *reinterpret_cast<const CVector *>(0xC813E0); }
// CWeather::ForceWeatherNow (0x72A4F0, through the crack's jump): the forced,
// old and new weather types all become `type` (8 = rainy SF, a storm's wind
// 1.0; 19 = the desert sandstorm, 1.5; -1 = let the weather go on).
inline void ForceWeatherNow(int16_t type) {
    *reinterpret_cast<volatile int16_t *>(0xC81318) = type;
    if (type < 0) return;
    *reinterpret_cast<volatile int16_t *>(0xC81320) = type;
    *reinterpret_cast<volatile int16_t *>(0xC8131C) = type;
}

// CVehicle::m_nCreatedBy (1 random, 2 mission, 3 parked, 4 permanent). CVehicle::CanBeDeleted
// (0x6D1180) lets the population remove a random or parked car even with a ped
// driving it (PED_DRIVING 0x32 at +0x530): fine while CJ is never far from his
// car, not when FindPlayerCentreOfWorld is at the drone.
inline uint8_t &vehicle_created_by(CVehicle *v) { return reinterpret_cast<uint8_t *>(v)[0x4A4]; }
constexpr uint8_t kVehicleRandom = 1, kVehicleMission = 2, kVehicleParked = 3;

// CVehicle::m_pDriver.
inline CPed *vehicle_driver(CVehicle *v) { return *reinterpret_cast<CPed **>(reinterpret_cast<uint8_t *>(v) + 0x460); }

// CPedIntelligence::FlushImmediately(bool set_default_task) on CPed::m_pIntelligence (+0x47C).
// WARP_CHAR_FROM_CAR_TO_COORD (0x482EFC) calls it with true to take a ped out of
// his car on the spot (the car task aborts at once), then teleports him.
inline void FlushImmediately(CPed *p, bool set_default_task) {
    void *intelligence = *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(p) + 0x47C);
    if (intelligence)
        reinterpret_cast<void(__attribute__((thiscall)) *)(void *, bool)>(0x601640)(intelligence, set_default_task);
}

// As WARP_CHAR_INTO_CAR (0x483267): FlushImmediately(false), then a stack
// CTaskSimpleCarSetPedInAsDriver(car, no line-up task) with m_bIsWarping (+0x1C)
// set, ProcessPed(ped), and its (non-deleting) destructor.
inline void WarpIntoCarAsDriver(CPed *p, CVehicle *car) {
    alignas(8) uint8_t task[0x40] = {};
    FlushImmediately(p, false);
    reinterpret_cast<void(__attribute__((thiscall)) *)(void *, CVehicle *, void *)>(0x6470E0)(task, car, nullptr);
    task[0x1C] = 1;
    reinterpret_cast<bool(__attribute__((thiscall)) *)(void *, CPed *)>(0x64B950)(task, p);
    reinterpret_cast<void(__attribute__((thiscall)) *)(void *)>(0x647170)(task);
}

// CTheScripts::IsPlayerOnAMission (its code is behind the crack's stub at
// 0x464D50): OnAMissionFlag (0xA476AC) is an offset into ScriptSpace (0xA49960).
inline bool on_a_mission() {
    int offset = *reinterpret_cast<volatile int *>(0xA476AC);
    return offset > 0 && *reinterpret_cast<volatile int *>(0xA49960 + offset) != 0;
}

// CPed::GetTransformedBonePosition(RwV3d&, bone, update_skin), thiscall: world space.
inline CVector bone_position(CPed *p, int bone) {
    CVector out{};
    reinterpret_cast<void(__attribute__((thiscall)) *)(CPed *, CVector &, int, bool)>(0x5E01C0)(p, out, bone, true);
    return out;
}
constexpr int kBoneHead = 5;  // ePedBones 5: the head (8 is the jaw)
constexpr int kBoneSpine1 = 3;  // what peds aim their guns at (CWeapon::FireInstantHit)
// Where a ped aims at another ped: CPed::GetBonePosition(out, spine1, false),
// thiscall, as CWeapon::FireInstantHit asks it (0x73FE57).
inline CVector aim_position(CPed *p) {
    CVector out{};
    reinterpret_cast<void(__attribute__((thiscall)) *)(CPed *, CVector &, int, bool)>(0x5E4280)(p, out, kBoneSpine1,
                                                                                               false);
    return out;
}

// CVehicle::m_nVehicleSubType (3 = heli, as the CHeli constructor sets it at
// 0x6C41E9), CAutomobile::m_fHeliRotorSpeed (CHeli::ProcessControl's
// downwash is at full power from 0.15), CPhysical::m_fMass (kg).
inline uint32_t vehicle_subtype(const CVehicle *v) {
    return *reinterpret_cast<const uint32_t *>(reinterpret_cast<const uint8_t *>(v) + 0x594);
}
constexpr uint32_t kVehicleHeli = 3;
inline float heli_rotor_speed(const CVehicle *v) {
    return *reinterpret_cast<const float *>(reinterpret_cast<const uint8_t *>(v) + 0x84C);
}
// CAutomobile and the classes built on it (not bikes, boats or trains).
inline bool is_automobile(const CVehicle *v) {
    uint32_t t = vehicle_subtype(v);
    return t <= 4 || t == 11;  // automobile, monster truck, quad, heli, plane, trailer
}
// CAutomobile::PlaceOnRoadProperly (thiscall): its wheels on the ground under them, heading
// kept, as the script's SET_CAR_COORDINATES does. 1.0 US starts it with a jump stub.
inline void PlaceOnRoadProperly(CVehicle *v) { reinterpret_cast<void(__attribute__((thiscall)) *)(CVehicle *)>(0x6AF420)(v); }
inline float mass_of(const CEntity *e) { return *reinterpret_cast<const float *>(reinterpret_cast<const uint8_t *>(e) + 0x8C); }

// An entity's collision box (CEntity::GetColModel 0x535300 for non-vehicles:
// CModelInfo::ms_modelInfoPtrs[m_nModelIndex]->m_pColModel; its CBoundingBox
// min and max come first). Model space, null without a model.
struct CBoundingBox {
    CVector min, max;
};
inline const CBoundingBox *model_box(const CEntity *e) {
    int16_t model = *reinterpret_cast<const int16_t *>(reinterpret_cast<const uint8_t *>(e) + 0x22);
    if (model < 0) return nullptr;
    auto *mi = reinterpret_cast<uint8_t *const *>(0xA9B0C8)[model];
    return mi ? *reinterpret_cast<const CBoundingBox *const *>(mi + 0x14) : nullptr;
}

// CExplosion::aExplosions, 16 of 0x7C bytes (CExplosion::AddExplosion
// 0x736A50 walks them): type, position, radius (9 or 6 m by type), and the
// active counter at +0x28 (0 = free).
struct CExplosionSlot {
    int type;
    CVector pos;
    float radius;
    uint8_t pad[0x28 - 0x14];
    uint8_t active;
    uint8_t pad2[0x7C - 0x29];
};
static_assert(sizeof(CExplosionSlot) == 0x7C, "CExplosion");
inline const CExplosionSlot *explosions() { return reinterpret_cast<const CExplosionSlot *>(0xC88950); }
constexpr int kExplosionCount = 16;
constexpr int kExplosionMolotov = 1;

// CFireManager gFireManager (0xB71F80, 53 references in the exe): 60 CFire of
// 0x28 bytes first. CFire: flags (bit 0 active), script index, position, the
// entity it burns (its position follows it), the creator, burn time, strength.
struct CFire {
    uint8_t flags;
    uint8_t pad0;
    int16_t script_index;
    CVector pos;
    CEntity *target;
    CEntity *creator;
    uint32_t burn_until;
    float strength;
    uint8_t pad1[0x28 - 0x20];
};
static_assert(sizeof(CFire) == 0x28, "CFire");
inline const CFire *fires() { return reinterpret_cast<const CFire *>(0xB71F80); }
constexpr int kFireCount = 60;
constexpr uint8_t kFireActive = 0x1;

// CWaterLevel::GetWaterLevel(x, y, z, &level, touching_water, normals): false where there is no water.
inline bool GetWaterLevel(float x, float y, float z, float *level, bool touching, CVector *normals) {
    return reinterpret_cast<bool(__cdecl *)(float, float, float, float *, bool, CVector *)>(0x6EB690)(
        x, y, z, level, touching, normals);
}

// CWeapon::DoBulletImpact(owner, victim, start, end, colpoint, int), thiscall, ret 0x14:
// push -1; push 0x848E50 (its SEH frame), absolute, so they move to a trampoline as they are.
constexpr uintptr_t kDoBulletImpact = 0x73B550;
constexpr uint8_t kDoBulletImpactPrologue[] = {0x6A, 0xFF, 0x68, 0x50, 0x8E, 0x84, 0x00};

}  // namespace sa
