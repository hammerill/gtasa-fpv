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

enum class Mode { Off, Body, Queries };  // Body: CJ himself at the drone; Queries: only FindPlayer*

Mode g_mode = Mode::Off;
sa::CVector g_home{};      // CJ's spot
float g_home_heading = 0;  // radians
sa::CVector g_drone{};     // latest drone position
sa::CVector g_placed{};    // where CJ was last re-added
uint32_t g_saved_flags = 0;

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
    if (dist(g_placed, g_drone) > kReplace) {  // new world sector maybe: Remove + Add
        sa::Teleport(cj, g_drone, false);
        g_placed = g_drone;
    } else if (cj->matrix) {
        cj->matrix->pos = g_drone;
    }
}

void begin() {
    sa::CPed *cj = sa::FindPlayerPed();
    if (!cj || !cj->matrix) return;
    g_home = cj->position();
    g_home_heading = std::atan2(-cj->matrix->forward.x, cj->matrix->forward.y);
    if (sa::FindPlayerVehicle()) {
        g_mode = Mode::Queries;
        fpvlog::write("standin: CJ is in a vehicle; streaming and population follow the drone, he stays");
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
}

void end(bool go_home) {
    if (g_mode == Mode::Off) return;
    Mode mode = g_mode;
    g_mode = Mode::Off;
    pilot::away(nullptr);
    sa::CPed *cj = sa::FindPlayerPed();
    if (mode != Mode::Body || !cj) return;
    sa::entity_flags(cj) = (sa::entity_flags(cj) & ~(sa::kEntityUsesCollision | sa::kEntityVisible)) |
                           (g_saved_flags & (sa::kEntityUsesCollision | sa::kEntityVisible));
    if (!go_home) {
        fpvlog::write("standin: over, CJ stays at the drone");
        return;
    }
    sa::LoadScene(g_home);
    sa::Teleport(cj, g_home, false);
    sa::set_ped_heading(cj, g_home_heading);
    fpvlog::write("standin: over, CJ back at %.2f %.2f %.2f", g_home.x, g_home.y, g_home.z);
}

}  // namespace standin
