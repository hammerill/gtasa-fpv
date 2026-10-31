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
// respawn / Move CJ here / exit that long after the blast, game time).
#include "autotest.h"

#include <windows.h>

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

namespace autotest {
namespace {

struct Config {
    float secs = 20, explode_at = 9, height = 0.7f, cruise_until = 1e9f, yaw_at = 1e9f, pitch = 0.5f, yaw_secs = 1.5f;
    bool hover = false, still = false, roll = false, move_cj = false, osd_off = false, bomb = false,
         explode_switch = false;
    std::vector<float> shots;
    bool teleport = false;
    int wanted = -1, hour = -1, weather = -2;  // weather -2: untouched
    float ped_at = 0, car_at = 0;
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

// The nearest vehicle without CJ in it, `ahead` metres in front of CJ, across his path.
void place_target_car(float ahead) {
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
    if (!best) {
        fpvlog::write("autotest: no vehicle to use as a target");
        return;
    }
    sa::CVector at{c.x + f.x * ahead, c.y + f.y * ahead, c.z + 0.5f};
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

void launch() {
    g_target = 0;
    if (g_cfg.ped_at > 0) place_target_ped(g_cfg.ped_at);
    if (g_cfg.car_at > 0) place_target_car(g_cfg.car_at);
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
