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
constexpr float kMaxHeadOffset = 1.5f;  // beyond this his bones aren't in place yet

Mode g_mode = Mode::Off;
sa::CVector g_home{};      // CJ's spot
float g_home_heading = 0;  // radians
sa::CVector g_drone{};     // latest drone position
sa::CVector g_placed{};    // where CJ was last re-added
uint32_t g_saved_flags = 0;
bool g_seated = false;  // Body mode from a vehicle: he goes back in it afterwards

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

// CAEVehicleAudioEntity::Service (0x502280) turns the radio off the frame the
// player stops driving and on again when he drives: kept on while he is out
// of his car standing in (it isn't positional, so it sounds as before).
constexpr sa::CallSite kRadioOn = {0x50237C, 0x4F5B20};   // TurnOnRadioForVehicle(), thiscall
constexpr sa::CallSite kRadioOff = {0x5023A3, 0x4F5B60};  // TurnOffRadioForVehicle(), thiscall
constexpr uintptr_t kVehicleAudio = 0x138;               // CVehicle::m_vehicleAudio (+0x4: its vehicle)
bool g_radio_kept = false;  // we skipped the "off" for CJ's car

struct Original {
    uintptr_t find_coors, find_centre, find_centre_no_sniper, damage, process_control, radio_on, radio_off;
};
Original g;

void *pilot_clump(sa::CEntity *e) { return *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(e) + 0x18); }

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

sa::CVehicle *audio_vehicle(void *audio) { return *reinterpret_cast<sa::CVehicle **>(static_cast<uint8_t *>(audio) + 4); }

void __attribute__((thiscall)) radio_off_hook(void *audio) {
    if (g_mode == Mode::Body && g_seated && g_car >= 0 && audio_vehicle(audio) == sa::GetVehicle(g_car)) {
        if (!g_radio_kept) fpvlog::write("standin: CJ's car radio kept on");
        g_radio_kept = true;
        return;
    }
    if (g_car < 0 || audio_vehicle(audio) == sa::GetVehicle(g_car)) g_radio_kept = false;
    reinterpret_cast<void(__attribute__((thiscall)) *)(void *)>(g.radio_off)(audio);
}

void __attribute__((thiscall)) radio_on_hook(void *audio) {
    if (g_radio_kept && audio_vehicle(audio) == sa::FindPlayerVehicle()) {  // back in it: still playing
        g_radio_kept = false;
        fpvlog::write("standin: CJ back in his car, radio still on");
        return;
    }
    g_radio_kept = false;
    reinterpret_cast<void(__attribute__((thiscall)) *)(void *)>(g.radio_on)(audio);
}

void place(sa::CPed *cj) {
    // Every frame: the game makes the player visible again on its own (camera code).
    sa::entity_flags(cj) &= ~(sa::kEntityUsesCollision | sa::kEntityVisible);
    if (!cj->matrix) return;
    // His head right at the drone: what peds look at (their look-at IK aims at
    // the head bone) and close to what they aim at. FindPlayerCoors is the
    // drone itself. The bones are where he was last placed, so head - pos is
    // the pose's own offset.
    sa::CVector at = g_drone;
    if (pilot_clump(cj)) {
        sa::CVector head = sa::bone_position(cj, sa::kBoneHead), &pos = cj->matrix->pos;
        sa::CVector off = {head.x - pos.x, head.y - pos.y, head.z - pos.z};
        if (dist(head, pos) < kMaxHeadOffset) at = {g_drone.x - off.x, g_drone.y - off.y, g_drone.z - off.z};
    }
    // New world sector maybe: Remove + Add. Not CPed::Teleport, which first
    // flushes his tasks: that knocked him out of the pilot's pose every 2 m.
    bool refile = dist(g_placed, at) > kReplace;
    if (refile) sa::WorldRemove(cj), g_placed = at;
    cj->matrix->pos = at;
    if (refile) sa::WorldAdd(cj);
}

// A held car out of the hold (CJ back in it, or the drone back near). While CJ is out of it,
// it isn't simulated: it hangs in its pose with its wheels marked off the ground, and after a
// hold its first steps threw it up off a roof, onto its side or its back. So the game puts it
// down on its wheels, then it's pinned there a moment while its wheels find the ground again.
constexpr uint32_t kSettleMs = 500;
int g_settle_car = -1;  // script handle
uint32_t g_settle_until = 0;
sa::CVector g_settle_axes[3]{}, g_settle_pos{};

void set_down(sa::CVehicle *car) {
    if (sa::is_automobile(car)) sa::PlaceOnRoadProperly(car);
    sa::move_speed(car) = sa::turn_speed(car) = {0, 0, 0};
    const sa::CMatrix &m = *car->matrix;
    g_settle_axes[0] = m.right, g_settle_axes[1] = m.forward, g_settle_axes[2] = m.up;
    g_settle_pos = m.pos;
    g_settle_car = sa::GetVehicleRef(car);
    g_settle_until = sa::timer_ms() + kSettleMs;
}

void settle() {
    if (g_settle_car < 0) return;
    sa::CVehicle *car = sa::GetVehicle(g_settle_car);
    if (!car || !car->matrix || sa::timer_ms() >= g_settle_until) {
        if (car && car->matrix)
            fpvlog::write("standin: CJ's car set down at %.2f %.2f %.2f, up %.2f %.2f %.2f", car->matrix->pos.x,
                          car->matrix->pos.y, car->matrix->pos.z, car->matrix->up.x, car->matrix->up.y,
                          car->matrix->up.z);
        g_settle_car = -1;
        return;
    }
    sa::CMatrix &m = *car->matrix;
    m.right = g_settle_axes[0], m.forward = g_settle_axes[1], m.up = g_settle_axes[2];
    m.pos = g_settle_pos;
    sa::move_speed(car) = sa::turn_speed(car) = {0, 0, 0};
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
        set_down(car);
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
            if (g_car_held) set_down(car);
        }
        sa::vehicle_created_by(car) = g_car_created_by;
        fpvlog::write("standin: CJ's car released%s", g_car_held ? " (was held)" : "");
    } else {
        fpvlog::write("standin: CJ's car is gone");
    }
    g_car = -1;
    g_car_held = false;
}

float dot(const sa::CVector &a, const sa::CVector &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// v in the frame of m's axes.
sa::CVector to_local(const sa::CMatrix &m, const sa::CVector &v) { return {dot(v, m.right), dot(v, m.forward), dot(v, m.up)}; }

// From the driver's seat: CJ gets out on the spot (as WARP_CHAR_FROM_CAR_TO_COORD
// does), so he stands in on foot like anywhere else (peds look at him, pickups
// see him), and pilot.cpp draws him in his seat. False: he is still in it.
bool leave_car(sa::CPed *cj, sa::CVehicle *car, pilot::Seat &seat) {
    const sa::CMatrix &c = *car->matrix, &m = *cj->matrix;
    seat.car = g_car;
    seat.axes[0] = to_local(c, m.right), seat.axes[1] = to_local(c, m.forward), seat.axes[2] = to_local(c, m.up);
    seat.offset = to_local(c, {m.pos.x - c.pos.x, m.pos.y - c.pos.y, m.pos.z - c.pos.z});
    seat.anim = pilot::seat_anim(cj);
    sa::FlushImmediately(cj, true);
    return !sa::FindPlayerVehicle();
}

void begin() {
    sa::CPed *cj = sa::FindPlayerPed();
    if (!cj || !cj->matrix) return;
    g_home = cj->position();
    g_home_heading = std::atan2(-cj->matrix->forward.x, cj->matrix->forward.y);
    g_seated = false;
    pilot::Seat seat{};
    if (sa::CVehicle *car = sa::FindPlayerVehicle()) {
        keep_car(car);
        // Missions check that he stays in it; as a passenger there's no seat to warp back to.
        bool can_leave = car->matrix && sa::vehicle_driver(car) == cj && !sa::on_a_mission();
        if (!can_leave || !leave_car(cj, car, seat)) {
            g_mode = Mode::Queries;
            fpvlog::write("standin: CJ is in a vehicle (%s); streaming and population follow the drone, he and it "
                          "stay (created by %d)", !can_leave ? sa::on_a_mission() ? "on a mission" : "not driving"
                                                             : "didn't get out", g_car_created_by);
            return;
        }
        g_seated = true;
        fpvlog::write("standin: CJ out of his vehicle (created by %d, seat anim %s)", g_car_created_by,
                      seat.anim ? "found" : "missing");
    }
    g_saved_flags = sa::entity_flags(cj);
    g_mode = Mode::Body;
    g_placed = {1e9f, 0, 0};
    place(cj);
    pilot::away(&g_home, g_seated ? &seat : nullptr);  // still drawn at his spot (or seat)
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
    g.radio_on = patch::redirect_call(kRadioOn.site, kRadioOn.fn, reinterpret_cast<void *>(&radio_on_hook));
    g.radio_off = patch::redirect_call(kRadioOff.site, kRadioOff.fn, reinterpret_cast<void *>(&radio_off_hook));
    pilot::install_hooks();
    fpvlog::write("standin: hooks coors %d centre %d/%d damage %d control %08X radio %d/%d", g.find_coors != 0,
                  g.find_centre != 0, g.find_centre_no_sniper != 0, g.damage != 0, unsigned(g.process_control),
                  g.radio_on != 0, g.radio_off != 0);
}

bool on() { return g_mode != Mode::Off; }

void tick() { settle(); }

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
    if (g_car >= 0) hold_car();
}

void end(bool go_home) {
    if (g_mode == Mode::Off) return;
    Mode mode = g_mode;
    g_mode = Mode::Off;
    pilot::away(nullptr);
    sa::CVehicle *car = g_car >= 0 ? sa::GetVehicle(g_car) : nullptr;
    if (car && car->matrix) g_home = car->matrix->pos;  // it can have been pushed
    release_car(go_home);  // its ground first
    sa::CPed *cj = sa::FindPlayerPed();
    if (mode != Mode::Body || !cj) return;
    sa::entity_flags(cj) = (sa::entity_flags(cj) & ~(sa::kEntityUsesCollision | sa::kEntityVisible)) |
                           (g_saved_flags & (sa::kEntityUsesCollision | sa::kEntityVisible));
    if (g_seated && car) {  // back behind the wheel, home or not ("Move CJ here" takes the car along)
        sa::WarpIntoCarAsDriver(cj, car);
        bool in = sa::FindPlayerVehicle() == car;
        fpvlog::write("standin: over, CJ back in his vehicle%s", in ? "" : " (FAILED)");
        void *audio = reinterpret_cast<uint8_t *>(car) + kVehicleAudio;
        if (!in && g_radio_kept && audio_vehicle(audio) == car) {  // on foot after all: the radio goes off
            g_radio_kept = false;
            reinterpret_cast<void(__attribute__((thiscall)) *)(void *)>(g.radio_off)(audio);
        }
        return;
    }
    if (g_seated) fpvlog::write("standin: CJ's vehicle is gone; he stays on foot");
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
