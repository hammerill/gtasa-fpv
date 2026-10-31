#include "standin.h"

#include <cmath>

#include "log.h"
#include "patch.h"
#include "pilot.h"
#include "sa.h"

namespace standin {
namespace {

constexpr float kStart = 20.0f;  // drone this far from CJ's spot: he stands in for it
constexpr float kStop = 10.0f;   // back this close: he goes home
constexpr float kReplace = 2.0f;  // re-add him to the world sectors after this much travel
constexpr float kHoldCar = 100.0f;  // drone this far from CJ's car: it's held where it is

enum class Mode { Off, Body, Queries };  // Body: CJ himself at the drone; Queries: only FindPlayer*

Mode g_mode = Mode::Off;
sa::CVector g_home{};      // CJ's spot
float g_home_heading = 0;  // radians
sa::CVector g_drone{};     // latest drone position
sa::CVector g_placed{};    // where CJ was last re-added
uint32_t g_saved_flags = 0;

// Queries mode: CJ's car. Streaming follows the drone, so with the drone far
// the ground's collision under it goes away (it would sink through the map):
// past kHoldCar it's held where it is. Closer, it's free (a truck can push it).
// And the population sees it far from "the player" (it would be deleted):
// it's a mission car until the stand-in ends.
int g_car = -1;  // script handle: GetVehicle gives null once it is gone
bool g_car_held = false;
sa::CVector g_car_axes[3]{};  // right, forward, up
sa::CVector g_car_pos{};
uint8_t g_car_created_by = 0;

struct Original {
    uintptr_t find_coors, find_centre, find_centre_no_sniper, damage, process_control;
};
Original g;

float dist(const sa::CVector &a, const sa::CVector &b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool local(int player) { return player <= 0; }

sa::CVector *__cdecl find_coors_hook(sa::CVector *out, int player) {
    if (g_mode != Mode::Off && local(player)) {
        *out = g_drone;
        return out;
    }
    return reinterpret_cast<sa::CVector *(__cdecl *)(sa::CVector *, int)>(g.find_coors)(out, player);
}
const sa::CVector *__cdecl find_centre_hook(int player) {
    if (g_mode != Mode::Off && local(player)) return &g_drone;
    return reinterpret_cast<const sa::CVector *(__cdecl *)(int)>(g.find_centre)(player);
}
const sa::CVector *__cdecl find_centre_no_sniper_hook(int player) {
    if (g_mode != Mode::Off && local(player)) return &g_drone;
    return reinterpret_cast<const sa::CVector *(__cdecl *)(int)>(g.find_centre_no_sniper)(player);
}

// CJ can't be hurt while he stands in: whatever goes for him at the drone
// (fists, bullets, cars, blasts) is ignored.
void __attribute__((thiscall)) damage_hook(sa::CPedDamageResponseCalculator *calc, sa::CPed *ped,
                                          sa::CPedDamageResponse *resp, bool flag) {
    if (g_mode == Mode::Body && ped == sa::FindPlayerPed()) {
        resp->damage_health = resp->damage_armour = 0;
        resp->health_zero = resp->force_death = false;
        resp->damage_calculated = true;
        static int reported = 0;
        if (reported++ < 20)
            fpvlog::write("standin: CJ hit at the drone (weapon %d, %.1f damage), ignored", calc->weapon, calc->damage);
        return;
    }
    reinterpret_cast<void(__attribute__((thiscall)) *)(sa::CPedDamageResponseCalculator *, sa::CPed *,
                                                       sa::CPedDamageResponse *, bool)>(g.damage)(calc, ped, resp, flag);
}

// CPlayerPed::ProcessControl: frozen at the drone (no gravity, no tasks).
void __attribute__((thiscall)) process_control_hook(sa::CPed *ped) {
    if (g_mode == Mode::Body && ped == sa::FindPlayerPed()) {
        sa::move_speed(ped) = {0, 0, 0};
        sa::turn_speed(ped) = {0, 0, 0};
        return;
    }
    reinterpret_cast<void(__attribute__((thiscall)) *)(sa::CPed *)>(g.process_control)(ped);
}

void place(sa::CPed *cj) {
    // Every frame: the game makes the player visible again on its own (camera code).
    sa::entity_flags(cj) &= ~(sa::kEntityUsesCollision | sa::kEntityVisible);
    if (!cj->matrix) return;
    // New world sector maybe: Remove + Add. Not CPed::Teleport, which first
    // flushes his tasks: that knocked him out of the pilot's pose every 2 m.
    bool refile = dist(g_placed, g_drone) > kReplace;
    if (refile) sa::WorldRemove(cj), g_placed = g_drone;
    cj->matrix->pos = g_drone;
    if (refile) sa::WorldAdd(cj);
}

void hold_car() {
    sa::CVehicle *car = g_car >= 0 ? sa::GetVehicle(g_car) : nullptr;
    if (!car || !car->matrix) return;
    bool far = dist(car->matrix->pos, g_drone) > kHoldCar;
    if (far && !g_car_held) {  // from where it is now
        g_car_axes[0] = car->matrix->right;
        g_car_axes[1] = car->matrix->forward;
        g_car_axes[2] = car->matrix->up;
        g_car_pos = car->matrix->pos;
        fpvlog::write("standin: drone %.0f m from CJ's car, held at %.2f %.2f %.2f", dist(g_car_pos, g_drone),
                      g_car_pos.x, g_car_pos.y, g_car_pos.z);
    } else if (!far && g_car_held) {
        sa::EnsureCollisionIsInMemory(g_car_pos);  // usually streamed by now; if not, before it can fall
        fpvlog::write("standin: drone back near CJ's car, free");
    }
    g_car_held = far;
    if (!far) return;
    car->matrix->right = g_car_axes[0];
    car->matrix->forward = g_car_axes[1];
    car->matrix->up = g_car_axes[2];
    car->matrix->pos = g_car_pos;
    sa::move_speed(car) = sa::turn_speed(car) = {0, 0, 0};
}

void keep_car(sa::CVehicle *car) {
    g_car = sa::GetVehicleRef(car);
    g_car_held = false;
    g_car_created_by = sa::vehicle_created_by(car);
    if (g_car_created_by == sa::kVehicleRandom || g_car_created_by == sa::kVehicleParked)
        sa::vehicle_created_by(car) = sa::kVehicleMission;
}

// go_home false: it is about to be moved to the drone, where the ground is streamed.
void release_car(bool go_home) {
    if (g_car < 0) return;
    if (sa::CVehicle *car = sa::GetVehicle(g_car)) {
        if (go_home && car->matrix) {
            sa::LoadScene(car->matrix->pos);
            sa::EnsureCollisionIsInMemory(car->matrix->pos);  // the ground back under it before it moves again
        }
        sa::vehicle_created_by(car) = g_car_created_by;
        fpvlog::write("standin: CJ's car released%s", g_car_held ? " (was held)" : "");
    } else {
        fpvlog::write("standin: CJ's car is gone");
    }
    g_car = -1;
    g_car_held = false;
}

void begin() {
    sa::CPed *cj = sa::FindPlayerPed();
    if (!cj || !cj->matrix) return;
    g_home = cj->position();
    g_home_heading = std::atan2(-cj->matrix->forward.x, cj->matrix->forward.y);
    if (sa::CVehicle *car = sa::FindPlayerVehicle()) {
        g_mode = Mode::Queries;
        keep_car(car);
        fpvlog::write("standin: CJ is in a vehicle; streaming and population follow the drone, he and it stay "
                      "(created by %d)", g_car_created_by);
        return;
    }
    g_saved_flags = sa::entity_flags(cj);
    g_mode = Mode::Body;
    g_placed = {1e9f, 0, 0};
    place(cj);
    pilot::away(&g_home);  // still drawn at his spot
    fpvlog::write("standin: CJ stands in for the drone, home %.2f %.2f %.2f", g_home.x, g_home.y, g_home.z);
}

}  // namespace

void install_hooks() {
    g.find_coors = reinterpret_cast<uintptr_t>(patch::detour(sa::kFindPlayerCoors, sa::kFindPlayerCoorsPrologue,
                                                             sizeof sa::kFindPlayerCoorsPrologue,
                                                             reinterpret_cast<void *>(&find_coors_hook)));
    g.find_centre = reinterpret_cast<uintptr_t>(
        patch::detour(sa::kFindPlayerCentreOfWorld, sa::kFindPlayerCentreOfWorldPrologue,
                      sizeof sa::kFindPlayerCentreOfWorldPrologue, reinterpret_cast<void *>(&find_centre_hook)));
    g.find_centre_no_sniper = reinterpret_cast<uintptr_t>(patch::detour(
        sa::kFindPlayerCentreNoSniper, sa::kFindPlayerCentreNoSniperPrologue,
        sizeof sa::kFindPlayerCentreNoSniperPrologue, reinterpret_cast<void *>(&find_centre_no_sniper_hook)));
    g.damage = reinterpret_cast<uintptr_t>(patch::detour(sa::kComputeDamageResponse, sa::kComputeDamageResponsePrologue,
                                                         sizeof sa::kComputeDamageResponsePrologue,
                                                         reinterpret_cast<void *>(&damage_hook)));
    g.process_control =
        patch::replace_pointer(sa::kPlayerPedProcessControlSlot, reinterpret_cast<void *>(&process_control_hook));
    pilot::install_hooks();
    fpvlog::write("standin: hooks coors %d centre %d/%d damage %d control %08X", g.find_coors != 0,
                  g.find_centre != 0, g.find_centre_no_sniper != 0, g.damage != 0, unsigned(g.process_control));
}

bool on() { return g_mode != Mode::Off; }

void home(float out[3]) {
    sa::CVector p = g_mode != Mode::Off ? g_home : [] {
        sa::CPed *cj = sa::FindPlayerPed();
        return cj ? cj->position() : sa::CVector{0, 0, 0};
    }();
    out[0] = p.x, out[1] = p.y, out[2] = p.z;
}

void update(const float drone_pos[3]) {
    g_drone = {drone_pos[0], drone_pos[1], drone_pos[2]};
    if (sa::CPed *cj = sa::FindPlayerPed())
        if (!sa::FindPlayerVehicle()) pilot::hold(cj);
    if (g_mode == Mode::Off) {
        sa::CPed *cj = sa::FindPlayerPed();
        if (cj && dist(cj->position(), g_drone) > kStart) begin();
        return;
    }
    if (dist(g_home, g_drone) < kStop) {
        end(true);
        return;
    }
    if (g_mode == Mode::Body)
        if (sa::CPed *cj = sa::FindPlayerPed()) place(cj);
    if (g_mode == Mode::Queries) hold_car();
}

void end(bool go_home) {
    if (g_mode == Mode::Off) return;
    Mode mode = g_mode;
    g_mode = Mode::Off;
    pilot::away(nullptr);
    if (mode == Mode::Queries) release_car(go_home);
    sa::CPed *cj = sa::FindPlayerPed();
    if (mode != Mode::Body || !cj) return;
    sa::entity_flags(cj) = (sa::entity_flags(cj) & ~(sa::kEntityUsesCollision | sa::kEntityVisible)) |
                           (g_saved_flags & (sa::kEntityUsesCollision | sa::kEntityVisible));
    if (!go_home) {
        fpvlog::write("standin: over, CJ stays at the drone");
        return;
    }
    sa::LoadScene(g_home);
    sa::EnsureCollisionIsInMemory(g_home);  // else he drops through his spot until it streams back
    if (cj->matrix) {  // like place(): CPed::Teleport would restart the pose
        sa::WorldRemove(cj);
        cj->matrix->pos = g_home;
        sa::WorldAdd(cj);
        sa::move_speed(cj) = sa::turn_speed(cj) = {0, 0, 0};
    } else {
        sa::Teleport(cj, g_home, false);
    }
    sa::set_ped_heading(cj, g_home_heading);
    fpvlog::write("standin: over, CJ back at %.2f %.2f %.2f", g_home.x, g_home.y, g_home.z);
}

}  // namespace standin
