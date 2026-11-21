// Unattended test flight, started by creating (or touching) fpv-data/autotest.txt
// in the mod's folder. Launches the drone in front of CJ, flies a fixed stick
// sequence through injected channels, takes screenshots and logs everything.
//
// autotest.txt knobs: secs=<flight seconds> (20), shots=<t1,t2,...> (seconds
// to save a screenshot at), "hover" (2.5 m up, no cruise), "still" (stays on
// the ground), height=<m> (cruise height, 0.7), "roll" (a slow roll to the
// right from 6 s, for the camera roll check), "cj" (end with Move CJ here),
// fov=<deg> near=<m> osd=<0|1>, "osdoff" (OSD disable switch on), view=chase|los,
// at=<x>,<y>,<z>,<heading deg> (CJ goes there, outside, 3 s before the launch),
// wanted=<stars> (CJ's wanted level at the launch), ped=<m> (the nearest ped
// is put that far ahead of the drone, a target), car=<m> (the same with the
// nearest car, parked across the path), time=<hour> (time of day), weather=<type> (forced
// from the launch: 8 rainy SF, a storm's wind; 19 sandstorm; -1 gives it back to the game), cruise=<s> (stop
// cruising and hover from then on), pitch=<0..1> (cruise stick, 0.5), yaw=<s> (turn around at that time) for yawdur=<s> (1.5), kamikaze
// builds: "boom" (bomb arm switch on, then cruise into whatever is ahead),
// "boomsw" (explode switch at 9 s; boomsw=<s> at that time), "signal" / "cutscene" (ending view), dur=<s> (its
// duration), then=respawn|recall|nothing, endact=r|y|f7 with endwait=<s> (the player's
// respawn / Move CJ here / exit that long after the blast, game time), "incar" (CJ is
// warped into the nearest car, put beside him, before the launch: the stand-in from a vehicle),
// pickup=<s> (at that flight time, a pistol pickup 1 s ahead on the drone's path; its state and
// CJ's pistol ammo are logged). While flying, the peds looking at CJ (IK look-at) are logged every 2 s.
// "perch": the nearest car (or a new one of model=<id>) is parked 5 m ahead of CJ and at perchat=<s>
// (2.5) the disarmed drone is dropped on its roof from perchh=<m> (0.12) over it (perchx=<m>,
// perchy=<m> off the roof's centre to the right / forward), then
// from driveat=<s> (5) the car is pushed along at drive=<km/h> (0: stays parked), turning at
// turn=<deg/s> ("rayplain": the raycast without the car's pose, as before libfpv 1.0.9). The drone's place in the car's frame is logged every frame, and a summary at the end.
#include "autotest.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "drone.h"
#include "fpvapi.h"
#include "log.h"
#include "paths.h"
#include "sa.h"
#include "world.h"

namespace autotest {
namespace {

struct Config {
    float secs = 20, explode_at = 9, height = 0.7f, cruise_until = 1e9f, yaw_at = 1e9f, pitch = 0.5f, yaw_secs = 1.5f;
    bool hover = false, still = false, roll = false, move_cj = false, osd_off = false, bomb = false,
         explode_switch = false;
    std::vector<float> shots;
    bool teleport = false;
    int wanted = -1, hour = -1, weather = -2;  // weather -2: untouched
    float ped_at = 0, car_at = 0, pickup_at = 0;
    bool in_car = false;
    bool perch = false;
    float perch_at = 2.5f, drive_at = 5, drive_kmh = 0, turn_dps = 0, perch_x = 0, perch_y = 0, perch_h = 0.12f;
    int model = 0;  // a new car of this model for the perch (0: the nearest, or a Greenwood)
    float at[4] = {};
    char end_action = 0;  // 'r', 'y' or '7' during a blast's ending
    float end_action_at = 1;
};
Config g_cfg;
bool g_running = false;
uint32_t g_blast_ms = 0;  // sa::timer_ms() when the ending began, 0 before
bool g_waiting = false;  // CJ was moved; the launch follows once the world streamed in
LARGE_INTEGER g_t0;
FILETIME g_trigger_time{};
float g_hold_z = 0;
size_t g_next_shot = 0;

std::string data_dir() { return paths::mod_dir() + "\\fpv-data"; }

// True when autotest.txt appeared or was touched since the last run.
bool triggered() {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA((data_dir() + "\\autotest.txt").c_str(), GetFileExInfoStandard, &data)) return false;
    if (CompareFileTime(&data.ftLastWriteTime, &g_trigger_time) == 0) return false;
    g_trigger_time = data.ftLastWriteTime;
    return true;
}

void read_config() {
    char buf[512] = {};
    if (FILE *f = std::fopen((data_dir() + "\\autotest.txt").c_str(), "r")) {
        std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
    }
    g_cfg = Config{};
    g_cfg.hover = std::strstr(buf, "hover") != nullptr;
    g_cfg.still = std::strstr(buf, "still") != nullptr;
    g_cfg.roll = std::strstr(buf, "roll") != nullptr;
    g_cfg.move_cj = std::strstr(buf, "cj") != nullptr;
    g_cfg.osd_off = std::strstr(buf, "osdoff") != nullptr;
    g_cfg.bomb = std::strstr(buf, "boom") != nullptr;
    g_cfg.explode_switch = std::strstr(buf, "boomsw") != nullptr;
    if (const char *p = std::strstr(buf, "boomsw=")) g_cfg.explode_at = float(std::atof(p + 7));
    if (const char *p = std::strstr(buf, "secs=")) g_cfg.secs = float(std::atof(p + 5));
    if (const char *p = std::strstr(buf, "height=")) g_cfg.height = float(std::atof(p + 7));
    if (const char *p = std::strstr(buf, "cruise=")) g_cfg.cruise_until = float(std::atof(p + 7));
    if (const char *p = std::strstr(buf, "pitch=")) g_cfg.pitch = float(std::atof(p + 6));
    if (const char *p = std::strstr(buf, "yaw=")) g_cfg.yaw_at = float(std::atof(p + 4));
    if (const char *p = std::strstr(buf, "yawdur=")) g_cfg.yaw_secs = float(std::atof(p + 7));
    if (const char *p = std::strstr(buf, "ped=")) g_cfg.ped_at = float(std::atof(p + 4));
    if (const char *p = std::strstr(buf, "car=")) g_cfg.car_at = float(std::atof(p + 4));
    if (const char *p = std::strstr(buf, "pickup=")) g_cfg.pickup_at = float(std::atof(p + 7));
    g_cfg.in_car = std::strstr(buf, "incar") != nullptr;
    g_cfg.perch = std::strstr(buf, "perch") != nullptr;
    if (const char *p = std::strstr(buf, "perchat=")) g_cfg.perch_at = float(std::atof(p + 8));
    if (const char *p = std::strstr(buf, "perchx=")) g_cfg.perch_x = float(std::atof(p + 7));
    if (const char *p = std::strstr(buf, "perchy=")) g_cfg.perch_y = float(std::atof(p + 7));
    if (const char *p = std::strstr(buf, "perchh=")) g_cfg.perch_h = float(std::atof(p + 7));
    if (const char *p = std::strstr(buf, "model=")) g_cfg.model = std::atoi(p + 6);
    if (const char *p = std::strstr(buf, "driveat=")) g_cfg.drive_at = float(std::atof(p + 8));
    if (const char *p = std::strstr(buf, "drive=")) g_cfg.drive_kmh = float(std::atof(p + 6));
    if (const char *p = std::strstr(buf, "turn=")) g_cfg.turn_dps = float(std::atof(p + 5));
    if (g_cfg.perch) g_cfg.still = true;
    fpvapi::g_plain_raycast = std::strstr(buf, "rayplain") != nullptr;
    if (const char *p = std::strstr(buf, "time=")) g_cfg.hour = std::atoi(p + 5);
    if (const char *p = std::strstr(buf, "weather=")) g_cfg.weather = std::atoi(p + 8);
    if (const char *p = std::strstr(buf, "wanted=")) g_cfg.wanted = std::atoi(p + 7);
    if (const char *p = std::strstr(buf, "endact=")) g_cfg.end_action = p[7] == 'f' ? '7' : p[7];
    if (const char *p = std::strstr(buf, "endwait=")) g_cfg.end_action_at = float(std::atof(p + 8));
    if (const char *p = std::strstr(buf, "at="))
        g_cfg.teleport = std::sscanf(p + 3, "%f,%f,%f,%f", &g_cfg.at[0], &g_cfg.at[1], &g_cfg.at[2], &g_cfg.at[3]) == 4;
    if (const char *p = std::strstr(buf, "shots=")) {
        for (p += 6;;) {
            char *end;
            float t = std::strtof(p, &end);
            if (end == p) break;
            g_cfg.shots.push_back(t);
            p = end;
            if (*p != ',') break;
            ++p;
        }
    }
    drone::Tuning &t = drone::tuning();
    t = drone::Tuning{};
    t.after_blast = std::strstr(buf, "cutscene") ? 1 : std::strstr(buf, "signal") ? 0 : -1;
    if (const char *p = std::strstr(buf, "fov=")) t.fov = float(std::atof(p + 4));
    if (const char *p = std::strstr(buf, "near=")) t.near_clip = float(std::atof(p + 5));
    if (const char *p = std::strstr(buf, "osd=")) t.osd = std::atoi(p + 4) != 0;
    if (const char *p = std::strstr(buf, "dur=")) t.ending_secs = float(std::atof(p + 4));
    if (const char *p = std::strstr(buf, "then=")) {
        p += 5;
        t.ending_then = !std::strncmp(p, "recall", 6)    ? FPV_KAMIKAZE_THEN_RECALL
                        : !std::strncmp(p, "nothing", 7) ? FPV_KAMIKAZE_THEN_NOTHING
                                                         : FPV_KAMIKAZE_THEN_RESPAWN;
    }
    t.view = std::strstr(buf, "view=chase") ? 1 : std::strstr(buf, "view=los") ? 2 : 0;
    fpvlog::write("autotest: config '%s' -> %.0f s, %u shots", buf, g_cfg.secs, unsigned(g_cfg.shots.size()));
}

float seconds() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return float(now.QuadPart - g_t0.QuadPart) / float(freq.QuadPart);
}

void request_shot() {
    if (FILE *f = std::fopen((data_dir() + "\\shot.txt").c_str(), "w")) std::fclose(f);
}

// AETR + AUX1 arm, AUX2 angle mode, AUX3 beeper, AUX4 turtle, AUX5 air mode,
// AUX6 OSD disable, AUX7 bomb arm, AUX8 explode. A crude altitude hold keeps
// it g_cfg.height up while it cruises forward.
void sticks(float t) {
    const fpv_frame &f = drone::last_frame();
    if (g_hold_z == 0 && t > 0.5f && f.struct_size)  // after the first fresh frame
        g_hold_z = f.position[2] + (g_cfg.hover ? 2.5f : g_cfg.height);
    const float cruise_at = g_cfg.bomb ? 7.0f : 5.0f;  // "boom" waits for the warhead to go live
    float ch[FPV_CHANNEL_COUNT] = {0, 0, -1, 0, -1, 1, -1, -1, -1, g_cfg.osd_off ? 1.0f : -1.0f};
    if (FPV_CHANNEL_COUNT > 10) ch[10] = ch[11] = -1;
    if (!g_cfg.still) {
        if (t > 2.0f) ch[4] = 1;  // arm
        if (t > 3.0f && g_hold_z != 0) {
            float err = g_hold_z - f.position[2];
            float tilt_comp = t > cruise_at && !g_cfg.hover && t < g_cfg.cruise_until ? 0.12f * g_cfg.pitch : 0.0f;  // more thrust while pitched forward
            float thr = -0.71f + tilt_comp + 0.15f * err - 0.08f * f.velocity[2];  // -0.71 = hover
            ch[2] = thr < -0.9f ? -0.9f : thr > 0.3f ? 0.3f : thr;
        }
        if (t > cruise_at && !g_cfg.hover && t < g_cfg.cruise_until) ch[1] = g_cfg.pitch;  // nose down: cruise forward
        if (t > g_cfg.yaw_at && t < g_cfg.yaw_at + g_cfg.yaw_secs) ch[3] = 0.6f;               // yaw right, about a half turn
        if (g_cfg.roll && t > 6.0f) ch[0] = 0.4f;         // bank right (angle mode holds the tilt)
#if FPV_CHANNEL_COUNT > 10
        if (g_cfg.bomb && t > 3.5f) ch[10] = 1;            // warhead live 3 s later
        if (g_cfg.explode_switch && t > g_cfg.explode_at) ch[11] = 1;  // detonate in the air
#endif
    }
    drone::inject_channels(ch);
}

// CJ to the test spot, outside (interiors are separate areas in the sky).
void move_cj() {
    sa::CPed *cj = sa::FindPlayerPed();
    if (!cj) return;
    sa::current_area() = 0;
    sa::RemoveBuildingsNotInArea(0);
    sa::area_of(cj) = 0;
    sa::CVector at{g_cfg.at[0], g_cfg.at[1], g_cfg.at[2]};
    sa::LoadScene(at);
    sa::Teleport(cj, at, false);
    sa::set_ped_heading(cj, g_cfg.at[3] / 57.2957795f);
    fpvlog::write("autotest: CJ moved to %.2f %.2f %.2f heading %.0f", at.x, at.y, at.z, g_cfg.at[3]);
}

// The target ped and its spot: it is held there until the drone is close.
int g_target = 0;  // a ped handle, or a vehicle handle when g_target_car
bool g_target_car = false;
sa::CVector g_target_at{};

// The nearest ped to CJ stands `ahead` metres in front of him, facing him.
void place_target_ped(float ahead) {
    sa::CPed *cj = sa::FindPlayerPed();
    sa::Pool *pool = sa::ped_pool();
    if (!cj || !cj->matrix || !pool) return;
    sa::CVector c = cj->position(), f = cj->matrix->forward;
    sa::CPed *best = nullptr;
    float best_d = 1e9f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->flags[i] & 0x80) continue;
        auto *ped = reinterpret_cast<sa::CPed *>(pool->objects + i * sa::kPedSize);
        if (ped == cj || sa::ped_health(ped) <= 0) continue;
        sa::CVector p = ped->position();
        float d = (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y);
        if (d < best_d) best_d = d, best = ped;
    }
    if (!best) {
        fpvlog::write("autotest: no ped to use as a target");
        return;
    }
    sa::CVector at{c.x + f.x * ahead, c.y + f.y * ahead, c.z};
    sa::Teleport(best, at, false);
    g_target = sa::GetPedRef(best);
    g_target_car = false;
    g_target_at = at;
    sa::set_ped_heading(best, std::atan2(f.x, -f.y));
    fpvlog::write("autotest: target ped %d (health %.0f) at %.2f %.2f %.2f", sa::GetPedRef(best), sa::ped_health(best),
                  at.x, at.y, at.z);
}

// A Greenwood (492), a random car as far as the population knows, at `at`.
sa::CVehicle *create_car(sa::CVector at, int model = 492) {
    reinterpret_cast<void(__cdecl *)(int, int)>(0x4087E0)(model, 0x10);         // CStreaming::RequestModel
    reinterpret_cast<void(__cdecl *)(bool)>(0x40EA10)(false);                 // LoadAllRequestedModels
    using Create = sa::CVehicle *(__cdecl *)(int, sa::CVector, uint8_t);      // CCarCtrl::CreateCarForScript
    sa::CVehicle *car = reinterpret_cast<Create>(0x431F80)(model, at, 0);
    if (car) sa::vehicle_created_by(car) = sa::kVehicleRandom;
    return car;
}

// The nearest vehicle without CJ in it, `ahead` metres in front of CJ, across his path
// (with `create`, a new car when there is none within 50 m).
void place_target_car(float ahead, bool create = false) {
    sa::CPed *cj = sa::FindPlayerPed();
    sa::Pool *pool = sa::vehicle_pool();
    if (!cj || !cj->matrix || !pool) return;
    sa::CVector c = cj->position(), f = cj->matrix->forward;
    sa::CVehicle *best = nullptr;
    float best_d = 1e9f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->flags[i] & 0x80) continue;
        auto *v = reinterpret_cast<sa::CVehicle *>(pool->objects + i * sa::kVehicleSize);
        if (v == sa::FindPlayerVehicle() || v->vtable != reinterpret_cast<void *>(0x871120)) continue;  // cars only
        sa::CVector p = v->position();
        float d = (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y);
        if (d < best_d) best_d = d, best = v;
    }
    sa::CVector at{c.x + f.x * ahead, c.y + f.y * ahead, c.z + 0.5f};
    if (((!best || best_d > 50 * 50) && create) || (create && g_cfg.model > 0)) {
        best = create_car(at, g_cfg.model > 0 ? g_cfg.model : 492);
        if (best) fpvlog::write("autotest: created a car as the target");
    }
    if (!best) {
        fpvlog::write("autotest: no vehicle to use as a target");
        return;
    }
    // A parked physics car (STATUS_ABANDONED in the bits above the entity type):
    // path-following traffic places itself and would drive off the spot.
    uint8_t &status = reinterpret_cast<uint8_t *>(best)[0x36];
    status = uint8_t((status & 7) | (4 << 3));
    sa::Teleport(best, at, true);
    sa::SetHeading(best, std::atan2(-f.x, f.y) + 1.5708f);
    sa::move_speed(best) = {0, 0, 0};
    g_target = sa::GetVehicleRef(best);
    g_target_car = true;
    g_target_at = at;
    fpvlog::write("autotest: target vehicle %d at %.2f %.2f %.2f", sa::GetVehicleRef(best), at.x, at.y, at.z);
}

void hold_target() {
    sa::CEntity *ped = !g_target ? nullptr : g_target_car ? sa::GetVehicle(g_target) : sa::GetPed(g_target);
    if (!ped) return;
    const fpv_frame &f = drone::last_frame();
    float dx = f.position[0] - g_target_at.x, dy = f.position[1] - g_target_at.y;
    if (dx * dx + dy * dy < 4.0f * 4.0f) {  // the drone is about to arrive: let go
        g_target = 0;
        return;
    }
    sa::CVector p = ped->position();
    if ((p.x - g_target_at.x) * (p.x - g_target_at.x) + (p.y - g_target_at.y) * (p.y - g_target_at.y) > 0.25f)
        sa::Teleport(ped, g_target_at, false);
    if (g_target_car) sa::move_speed(ped) = {0, 0, 0};
}

// The nearest car, beside CJ, and CJ in it behind the wheel.
void put_cj_in_car() {
    sa::CPed *cj = sa::FindPlayerPed();
    sa::Pool *pool = sa::vehicle_pool();
    if (!cj || !cj->matrix || !pool || sa::FindPlayerVehicle()) return;
    sa::CVector c = cj->position(), r = cj->matrix->right;
    sa::CVehicle *best = nullptr;
    float best_d = 1e9f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->flags[i] & 0x80) continue;
        auto *v = reinterpret_cast<sa::CVehicle *>(pool->objects + i * sa::kVehicleSize);
        if (v->vtable != reinterpret_cast<void *>(0x871120)) continue;  // cars only
        sa::CVector p = v->position();
        float d = (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y);
        if (d < best_d) best_d = d, best = v;
    }
    if (!best || best_d > 30 * 30) {  // none around
        best = create_car({c.x + r.x * 3, c.y + r.y * 3, c.z + 0.5f});
        if (!best) {
            fpvlog::write("autotest: no car for CJ");
            return;
        }
        fpvlog::write("autotest: created a car for CJ");
    }
    if (sa::CPed *driver = sa::vehicle_driver(best)) {  // traffic: its driver gets out and walks
        sa::FlushImmediately(driver, true);
        fpvlog::write("autotest: the car's driver %s", sa::vehicle_driver(best) ? "stayed in (FAILED)" : "got out");
        if (sa::vehicle_driver(best)) return;
    }
    uint8_t &status = reinterpret_cast<uint8_t *>(best)[0x36];  // parked (abandoned), not traffic
    status = uint8_t((status & 7) | (4 << 3));
    sa::Teleport(best, {c.x + r.x * 3, c.y + r.y * 3, c.z + 0.5f}, true);
    sa::SetHeading(best, std::atan2(-cj->matrix->forward.x, cj->matrix->forward.y));
    sa::move_speed(best) = {0, 0, 0};
    sa::WarpIntoCarAsDriver(cj, best);
    fpvlog::write("autotest: CJ in car %d (%s)", sa::GetVehicleRef(best),
                  sa::FindPlayerVehicle() == best ? "driving" : "FAILED");
}

int g_pickup = -1;
inline int pistol_ammo(sa::CPed *cj) { return *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(cj) + 0x5A0 + 2 * 0x1C + 0xC); }
inline uint8_t pickup_type(int handle) { return reinterpret_cast<uint8_t *>(0x9788C0 + (handle & 0xFFFF) * 0x20)[0x1C]; }

void place_pickup() {
    sa::CPed *cj = sa::FindPlayerPed();
    if (!cj) return;
    const fpv_frame &f = drone::last_frame();
    // CPickups::GenerateNewOne_WeaponType(pos, WEAPON_PISTOL, PICKUP_ONCE, ammo, empty, message)
    using Fn = int(__cdecl *)(sa::CVector, int, uint8_t, unsigned, bool, char *);
    sa::CVector at{f.position[0] + f.velocity[0], f.position[1] + f.velocity[1], f.position[2] + f.velocity[2]};
    g_pickup = reinterpret_cast<Fn>(0x457380)(at, 22, 3, 34, false, nullptr);
    fpvlog::write("autotest: pickup %08X (type %d) at %.2f %.2f %.2f, CJ's pistol ammo %d", unsigned(g_pickup),
                  g_pickup >= 0 ? pickup_type(g_pickup) : -1, at.x, at.y, at.z, pistol_ammo(cj));
}

// Which peds within 60 m look at CJ (IKChainManager_c::GetLookAtEntity on g_ikChainMan).
void log_lookers(float t) {
    sa::CPed *cj = sa::FindPlayerPed();
    sa::Pool *pool = sa::ped_pool();
    if (!cj || !pool) return;
    using Fn = sa::CEntity *(__attribute__((thiscall)) *)(void *, sa::CPed *);
    const fpv_frame &fr = drone::last_frame();
    sa::CVector d{fr.position[0], fr.position[1], fr.position[2]};
    int nearby = 0, looking = 0;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->flags[i] & 0x80) continue;
        auto *ped = reinterpret_cast<sa::CPed *>(pool->objects + i * sa::kPedSize);
        if (ped == cj) continue;
        sa::CVector p = ped->position();
        if ((p.x - d.x) * (p.x - d.x) + (p.y - d.y) * (p.y - d.y) > 60 * 60) continue;
        ++nearby;
        if (reinterpret_cast<Fn>(0x6181D0)(reinterpret_cast<void *>(0xC15448), ped) == cj) ++looking;
    }
    sa::CVector head = cj->matrix ? sa::bone_position(cj, sa::kBoneHead) : sa::CVector{};
    fpvlog::write("autotest: %.0f s: %d of %d peds near the drone look at CJ; his head %.2f %.2f %.2f, drone %.2f %.2f "
                  "%.2f%s", t, looking, nearby, head.x, head.y, head.z, d.x, d.y, d.z,
                  sa::FindPlayerVehicle() ? ", CJ in a vehicle" : "");
    if (g_pickup >= 0)
        fpvlog::write("autotest: pickup type %d, CJ's pistol ammo %d", pickup_type(g_pickup), pistol_ammo(cj));
}

// --- perch: the drone on a car's roof ---
int g_perch_car = 0;   // vehicle handle
bool g_perched = false;
float g_perch_l0[3];   // where it came to rest, in the car's frame
struct PerchStats {
    float slip = 0, lift = 0, low = 0;
    int frames = 0, off = 0;
    bool fell = false;
    float fell_at = -1;
} g_ps;

// p (world) in the car's frame: right, forward, up from its matrix position.
void car_local(const sa::CMatrix &m, const float p[3], float out[3]) {
    float d[3] = {p[0] - m.pos.x, p[1] - m.pos.y, p[2] - m.pos.z};
    out[0] = d[0] * m.right.x + d[1] * m.right.y + d[2] * m.right.z;
    out[1] = d[0] * m.forward.x + d[1] * m.forward.y + d[2] * m.forward.z;
    out[2] = d[0] * m.up.x + d[1] * m.up.y + d[2] * m.up.z;
}

void perch_tick(float t) {
    sa::CVehicle *car = g_perch_car ? sa::GetVehicle(g_perch_car) : nullptr;
    if (!car || !car->matrix) return;
    const sa::CMatrix &m = *car->matrix;
    if (!g_perched && t >= g_cfg.perch_at) {
        // the roof under the centre (perchx/perchy off it): a ray down from above the car
        float o[3] = {m.pos.x + m.right.x * g_cfg.perch_x + m.forward.x * g_cfg.perch_y,
                      m.pos.y + m.right.y * g_cfg.perch_x + m.forward.y * g_cfg.perch_y, m.pos.z + 4};
        float down[3] = {0, 0, -1};
        fpv_raycast_hit hit{};
        sa::CEntity *e = nullptr;
        if (!world::probe(o, down, 8, &hit, &e) || e != car) {
            fpvlog::write("autotest: perch: no roof under the drone (hit %p, car %p)", static_cast<void *>(e),
                          static_cast<void *>(car));
            g_perch_car = 0;
            return;
        }
        float at[3] = {o[0], o[1], o[2] - hit.distance + g_cfg.perch_h};
        drone::place(at);
        g_perched = true;
        g_ps = PerchStats{};
        g_perch_l0[0] = -1e9f;
        fpvlog::write("autotest: perch: drone put on car %d's roof at %.2f %.2f %.2f (roof %.2f, n %.2f %.2f %.2f)",
                      g_perch_car, at[0], at[1], at[2], o[2] - hit.distance, hit.normal[0], hit.normal[1],
                      hit.normal[2]);
        return;
    }
    if (!g_perched) return;
    if (g_cfg.drive_kmh > 0 && t >= g_cfg.drive_at) {
        // pushed along its nose (SA speeds are metres per 1/50 s), its own physics for the rest
        reinterpret_cast<uint8_t *>(car)[0x1C] &= ~uint8_t(1u << 2);  // bIsStatic: awake
        float v = g_cfg.drive_kmh / 3.6f / 50.0f;
        float ramp = std::min(1.0f, (t - g_cfg.drive_at) / 3.0f);  // 3 s up to speed
        sa::CVector &ms = sa::move_speed(car);
        float fx = m.forward.x, fy = m.forward.y, fl = std::sqrt(fx * fx + fy * fy);
        if (fl > 1e-3f) ms.x = fx / fl * v * ramp, ms.y = fy / fl * v * ramp;
        sa::turn_speed(car).z = g_cfg.turn_dps / 57.2957795f / 50.0f * ramp;
    }
    const fpv_frame &f = drone::last_frame();
    float l[3];
    car_local(m, f.position, l);
    bool ground = (f.flags & FPV_DRONE_ON_GROUND) != 0;
    if (t < g_cfg.perch_at + 1.0f) {  // settling: measured from where it came to rest
        std::memcpy(g_perch_l0, l, sizeof l);
        return;
    }
    float dx = l[0] - g_perch_l0[0], dy = l[1] - g_perch_l0[1], lift = l[2] - g_perch_l0[2];
    float slip = std::sqrt(dx * dx + dy * dy);
    g_ps.frames++;
    if (!ground) g_ps.off++;
    if (!g_ps.fell) {
        g_ps.slip = std::max(g_ps.slip, slip);
        g_ps.lift = std::max(g_ps.lift, lift);
        g_ps.low = std::min(g_ps.low, lift);
        if (lift < -0.3f || slip > 1.5f) {
            g_ps.fell = true;
            g_ps.fell_at = t;
            fpvlog::write("autotest: perch: FELL off at %.2f s (slip %.2f, lift %.2f)", t, slip, lift);
        }
    }
    // every 15th frame, and every frame off the roof or lifted: where it is on
    // the car, the car's speed, and the drone's tilt (its up axis) on it
    static int n = 0;
    if (!ground || std::fabs(lift) > 0.008f || ++n % 15 == 0) {
        const sa::CVector &ms = sa::move_speed(car);
        const float *q = f.rotation;
        float up[3] = {2 * (q[0] * q[2] + q[3] * q[1]), 2 * (q[1] * q[2] - q[3] * q[0]),
                       1 - 2 * (q[0] * q[0] + q[1] * q[1])};
        float tilt_r = up[0] * m.right.x + up[1] * m.right.y + up[2] * m.right.z;
        float tilt_f = up[0] * m.forward.x + up[1] * m.forward.y + up[2] * m.forward.z;
        fpvlog::write("autotest: perch %.3f on the car %+.3f %+.3f %+.3f ground %d, car %.1f km/h, tilt %+.3f %+.3f",
                      t, l[0], l[1], l[2], ground ? 1 : 0,
                      std::sqrt(ms.x * ms.x + ms.y * ms.y + ms.z * ms.z) * 50 * 3.6f, tilt_r, tilt_f);
    }
}

void perch_summary() {
    if (!g_perched) return;
    fpvlog::write("autotest: perch summary: drive %.0f km/h turn %.0f deg/s: slip %.3f m, lift %.3f m (low %.3f), "
                  "off the roof %.0f %% of %d frames, %s",
                  g_cfg.drive_kmh, g_cfg.turn_dps, g_ps.slip, g_ps.lift, g_ps.low,
                  g_ps.frames ? 100.0f * g_ps.off / g_ps.frames : 0.0f, g_ps.frames, g_ps.fell ? "FELL" : "stayed");
    g_perched = false;
    g_perch_car = 0;
}

void launch() {
    g_target = 0;
    g_perch_car = 0;
    g_perched = false;
    g_pickup = -1;
    if (g_cfg.in_car) put_cj_in_car();

    if (g_cfg.ped_at > 0) place_target_ped(g_cfg.ped_at);
    if (g_cfg.car_at > 0 || g_cfg.perch) place_target_car(g_cfg.perch ? 5.0f : g_cfg.car_at, g_cfg.perch);
    if (g_cfg.perch && g_target_car) {
        g_perch_car = g_target;
        g_target = 0;  // not held in place: the drone sits on it
        // on its wheels on the road and awake, settled before the drone goes on
        if (sa::CVehicle *car = sa::GetVehicle(g_perch_car)) {
            sa::PlaceOnRoadProperly(car);
            reinterpret_cast<uint8_t *>(car)[0x1C] &= ~uint8_t(1u << 2);  // bIsStatic
        }
    }
    if (fpvapi::load() && fpvapi::kamikaze())  // the player ticks it in F8 > Fly
        fpvapi::fpv.fpv_kamikaze_set_installed(g_cfg.bomb ? 1 : 0);
    if (g_cfg.hour >= 0) {
        sa::clock_hours() = uint8_t(g_cfg.hour % 24);
        sa::clock_minutes() = 0;
    }
    if (g_cfg.weather != -2) sa::ForceWeatherNow(int16_t(g_cfg.weather));
    if (g_cfg.wanted >= 0)
        if (sa::CPed *cj = sa::FindPlayerPed()) sa::SetWantedLevel(cj, g_cfg.wanted);
    drone::toggle();
    if (!drone::active()) {
        fpvlog::write("autotest: drone did not launch");
        return;
    }
    g_running = true;
    g_blast_ms = 0;
    g_hold_z = 0;
    g_next_shot = 0;
    QueryPerformanceCounter(&g_t0);
    fpvlog::write("autotest: flying");
}

void finish(const char *why) {
    perch_summary();
    g_running = false;
    fpvlog::write("autotest: %s", why);
    if (!drone::active()) return;
    if (g_cfg.move_cj)
        drone::move_cj_here();
    else
        drone::toggle();
}

}  // namespace

void tick() {
    if (triggered()) {
        read_config();
        g_running = false;
        if (drone::active()) drone::toggle();
        if (g_cfg.teleport) {
            move_cj();
            g_waiting = true;
            QueryPerformanceCounter(&g_t0);
        } else {
            launch();
        }
    }
    if (g_waiting && seconds() > 3.0f) {
        g_waiting = false;
        launch();
    }
    if (!g_running) return;
    if (!drone::active()) {  // stopped by a blast ending, CJ's death or a key
        g_running = false;
        fpvlog::write("autotest: drone gone at %.1f s", seconds());
        return;
    }
    float t = seconds();
    if (drone::ending() && !g_blast_ms) {
        g_blast_ms = sa::timer_ms() ? sa::timer_ms() : 1;
        fpvlog::write("autotest: blast ending at %.1f s", t);
    }
    float since_blast = g_blast_ms ? float(int32_t(sa::timer_ms() - g_blast_ms)) / 1000.0f : 0;
    if (g_blast_ms && g_cfg.end_action && since_blast >= g_cfg.end_action_at) {
        char a = g_cfg.end_action;
        g_cfg.end_action = 0;
        fpvlog::write("autotest: player action '%c' %.1f s after the blast (ending %s)", a, since_blast,
                      drone::ending() ? "still on" : "over");
        if (a == 'r') drone::respawn();
        else if (a == 'y') drone::move_cj_here();
        else drone::toggle();
        if (!drone::active()) {
            g_running = false;
            fpvlog::write("autotest: drone gone at %.1f s", t);
            return;
        }
    }
    if (!drone::ending()) g_blast_ms = 0;  // a new blast may follow a respawn
    sticks(t);
    hold_target();
    perch_tick(t);
    static float next_log = 0;
    if (t < 1) next_log = 2;
    if (g_cfg.pickup_at > 0 && t >= g_cfg.pickup_at) g_cfg.pickup_at = 0, place_pickup();
    if (t >= next_log) next_log += 2, log_lookers(t);
    if (g_next_shot < g_cfg.shots.size() && t >= g_cfg.shots[g_next_shot]) {
        ++g_next_shot;
        request_shot();
        const fpv_frame &f = drone::last_frame();
        fpvlog::write("autotest: shot at %.1f s, drone %.2f %.2f %.2f fwd %.2f %.2f %.2f up %.2f %.2f %.2f", t,
                      f.position[0], f.position[1], f.position[2], f.camera.forward[0], f.camera.forward[1],
                      f.camera.forward[2], f.camera.up[0], f.camera.up[1], f.camera.up[2]);
    }
    if (t > g_cfg.secs) finish("done");
}

void abort() {
    if (g_running) finish("aborted");
}

}  // namespace autotest
