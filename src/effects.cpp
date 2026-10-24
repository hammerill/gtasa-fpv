#include "effects.h"

#include <windows.h>

#include "log.h"
#include "sa.h"
#include "world.h"

namespace effects {
namespace {

// A 5" quad is ~0.65 kg; at 30 m/s that is a real hit for a pedestrian and a
// dent for a car. Speeds are the relative normal speed libfpv reports (m/s).
constexpr float kPedKnockSpeed = 4.0f;
constexpr float kPedDamagePerMs = 2.0f;  // health points per m/s (peds have ~100)
constexpr float kCarDamageSpeed = 5.0f;
constexpr float kCarDamagePerMs = 3.0f;  // body health points per m/s (cars have 1000)
constexpr float kDroneMass = 0.65f;
constexpr DWORD kCooldownMs = 500;  // one effect per entity per half second

struct Recent {
    uint32_t object_id;
    DWORD time;
};
Recent g_recent[16];

bool cooling_down(uint32_t object_id) {
    DWORD now = GetTickCount();
    for (const Recent &r : g_recent)
        if (r.object_id == object_id && now - r.time < kCooldownMs) return true;
    return false;
}

void start_cooldown(uint32_t object_id) {
    Recent *oldest = &g_recent[0];
    for (Recent &r : g_recent)
        if (r.time < oldest->time) oldest = &r;
    *oldest = {object_id, GetTickCount()};
}

// The drone's momentum as a CPhysical move force: the game keeps speeds in
// metres per 1/50 s, so an impulse in kg m/s is divided by 50.
sa::CVector push_of(const fpv_frame &frame) {
    float k = kDroneMass / 50.0f;
    return {frame.velocity[0] * k, frame.velocity[1] * k, frame.velocity[2] * k};
}

// The world treats the drone as CJ's (see stand-in in drone.cpp): he is the
// one who hit them.
bool hit_ped(int handle, float speed, sa::CVector push) {
    sa::CPed *ped = sa::GetPed(handle);
    sa::CPed *cj = sa::FindPlayerPed();
    if (!ped || ped == cj || speed < kPedKnockSpeed) return false;
    float before = sa::ped_health(ped);
    sa::GenerateDamageEvent(ped, cj, sa::kWeaponRammedByCar, int(speed * kPedDamagePerMs), sa::kPedPieceTorso, 0);
    sa::ApplyMoveForce(ped, push);
    fpvlog::write("effects: ped %d hit at %.1f m/s, health %.0f -> %.0f", handle, speed, before, sa::ped_health(ped));
    return true;
}

bool hit_vehicle(int handle, float speed, sa::CVector push, const float at[3]) {
    sa::CVehicle *car = sa::GetVehicle(handle);
    if (!car || speed < kCarDamageSpeed) return false;
    sa::ApplyMoveForce(car, push);
    sa::InflictDamage(car, sa::FindPlayerPed(), sa::kWeaponRammedByCar, speed * kCarDamagePerMs, {at[0], at[1], at[2]});
    fpvlog::write("effects: vehicle %d hit at %.1f m/s", handle, speed);
    return true;
}

bool hit_object(int handle, float speed, sa::CVector push) {
    sa::CEntity *object = sa::GetObject(handle);
    if (!object) return false;
    sa::ApplyMoveForce(object, push);
    fpvlog::write("effects: object %d hit at %.1f m/s", handle, speed);
    return true;
}

}  // namespace

void on_frame(const fpv_frame &frame) {
    for (uint32_t i = 0; i < frame.impact_count && i < FPV_MAX_IMPACTS; ++i) {
        const fpv_impact &im = frame.impacts[i];
        int handle = int(world::handle_of(im.object_id));
        if (!handle || im.speed < 1.0f || cooling_down(im.object_id)) continue;
        bool applied = false;
        switch (world::kind_of(im.object_id)) {
            case world::Kind::Ped: applied = hit_ped(handle, im.speed, push_of(frame)); break;
            case world::Kind::Vehicle: applied = hit_vehicle(handle, im.speed, push_of(frame), im.position); break;
            case world::Kind::Object: applied = hit_object(handle, im.speed, push_of(frame)); break;
            default: break;
        }
        if (applied) start_cooldown(im.object_id);
    }
}

}  // namespace effects
