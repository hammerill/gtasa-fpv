#include "wind.h"

#include <algorithm>
#include <cmath>

#include "fpvapi.h"
#include "log.h"
#include "sa.h"

using fpvapi::fpv;

namespace wind {
namespace {

// CWeather::Wind is 0..0.25 when sunny, 0.7 cloudy, 1.0 in the rain storms
// and 1.5 in the sandstorm: 1.0 is a storm's 11 m/s at 10 m.
constexpr float kStormMps = 11.0f;
// The extra sunny weathers have Wind 0, dead calm everywhere, while the
// game's wind sound still blows up high (it scales with the camera's
// height). Real air is never that still: a light breeze at least, which
// libfpv makes about 4 m/s on top of Maze Bank.
constexpr float kCalmMps = 2.0f;
// WindDir wobbles over 1-2 s by script, the same everywhere: libfpv makes
// the gusts, so only its slow mean direction is passed on.
constexpr float kDirSeconds = 4.0f;
constexpr float kSourceRange = 150.0f;   // m: helicopters farther away can't reach the drone
constexpr float kBlastRange = 300.0f;    // m
constexpr float kRotorFullSpeed = 0.15f;  // m_fHeliRotorSpeed at full downwash (CHeli::ProcessControl)
constexpr float kAirDensity = 1.2f;

float g_dir[2] = {0.7071f, 0.7071f};  // smoothed WindDir (x, y)
bool g_dir_valid = false;
// explosion slots as last seen: a slot is a new explosion when it turns
// active or moves
bool g_slot_active[sa::kExplosionCount];
sa::CVector g_slot_pos[sa::kExplosionCount];
float g_log_timer = 0;

bool available() {
    return fpv.fpv_wind_defaults && fpv.fpv_wind_set && fpv.fpv_wind_set_sources && fpv.fpv_wind_add_blast;
}

void weather(float dt) {
    const sa::CVector &d = sa::weather_wind_dir();
    if (!g_dir_valid) {
        float l = std::hypot(d.x, d.y);
        if (l > 0.01f) g_dir[0] = d.x / l, g_dir[1] = d.y / l;
        g_dir_valid = true;
    } else {
        float k = 1.0f - std::exp(-dt / kDirSeconds);
        g_dir[0] += (d.x - g_dir[0]) * k;
        g_dir[1] += (d.y - g_dir[1]) * k;
    }
    float l = std::hypot(g_dir[0], g_dir[1]);
    float dx = l > 1e-3f ? g_dir[0] / l : 0.7071f, dy = l > 1e-3f ? g_dir[1] / l : 0.7071f;
    float speed = std::max(kCalmMps, sa::weather_wind() * kStormMps);

    fpv_wind w;
    fpv.fpv_wind_defaults(&w);
    w.velocity[0] = dx * speed;
    w.velocity[1] = dy * speed;
    // Interiors sit high above the map: no weather in there.
    if (sa::current_area() != 0) w.flags |= FPV_WIND_ENCLOSED;
    fpv.fpv_wind_set(&w);
}

// Helicopters with their rotors turning: rotor downwash, from momentum
// theory with the helicopter's weight as the thrust at full rotor speed.
uint32_t helicopters(const float at[3], fpv_wind_source *out, uint32_t max) {
    sa::Pool *pool = sa::vehicle_pool();
    if (!pool || !pool->objects) return 0;
    uint32_t n = 0;
    for (int i = 0; i < pool->size && n < max; ++i) {
        if (pool->flags[i] & 0x80) continue;  // free slot
        auto *v = reinterpret_cast<sa::CVehicle *>(pool->objects + i * sa::kVehicleSize);
        if (sa::vehicle_subtype(v) != sa::kVehicleHeli || !v->matrix) continue;
        float rotor = sa::heli_rotor_speed(v);
        if (!(rotor > 0.01f)) continue;
        const sa::CMatrix &m = *v->matrix;
        float dx = m.pos.x - at[0], dy = m.pos.y - at[1], dz = m.pos.z - at[2];
        if (dx * dx + dy * dy + dz * dz > kSourceRange * kSourceRange) continue;
        const sa::CBoundingBox *box = sa::model_box(v);
        float length = box ? std::max(box->max.y - box->min.y, box->max.x - box->min.x) : 10.0f;
        float top = box ? box->max.z : 2.0f;
        float radius = std::clamp(0.42f * length, 0.3f, 8.0f);  // main rotor about the fuselage's length
        float power = std::min(1.0f, rotor / kRotorFullSpeed);
        float thrust = std::max(0.0f, sa::mass_of(v)) * 9.81f * power * power;
        fpv_wind_source &s = out[n++];
        s.position[0] = m.pos.x + m.up.x * top;
        s.position[1] = m.pos.y + m.up.y * top;
        s.position[2] = m.pos.z + m.up.z * top;
        s.direction[0] = -m.up.x, s.direction[1] = -m.up.y, s.direction[2] = -m.up.z;
        s.radius = radius;
        s.speed = std::sqrt(thrust / (2.0f * kAirDensity * 3.14159265f * radius * radius));
        s.type = FPV_WIND_SOURCE_ROTOR;
    }
    return n;
}

void blasts(const float at[3], bool report) {
    const sa::CExplosionSlot *e = sa::explosions();
    for (int i = 0; i < sa::kExplosionCount; ++i) {
        bool active = e[i].active != 0;
        bool moved = e[i].pos.x != g_slot_pos[i].x || e[i].pos.y != g_slot_pos[i].y || e[i].pos.z != g_slot_pos[i].z;
        bool fresh = active && (!g_slot_active[i] || moved);
        g_slot_active[i] = active;
        g_slot_pos[i] = e[i].pos;
        if (!fresh || !report || e[i].type == sa::kExplosionMolotov) continue;  // a molotov burns, it doesn't blast
        float dx = e[i].pos.x - at[0], dy = e[i].pos.y - at[1], dz = e[i].pos.z - at[2];
        if (dx * dx + dy * dy + dz * dz > kBlastRange * kBlastRange) continue;
        // the game's radius is how far it does damage; the blast's strong core is about half that
        float pos[3] = {e[i].pos.x, e[i].pos.y, e[i].pos.z};
        fpv.fpv_wind_add_blast(pos, std::max(1.0f, 0.5f * e[i].radius));
        fpvlog::write("wind: blast of explosion type %d (radius %.1f) %.0f m from the drone", e[i].type, e[i].radius,
                      std::sqrt(dx * dx + dy * dy + dz * dz));
    }
}

}  // namespace

void reset() {
    g_dir_valid = false;
    float nowhere[3] = {0, 0, 0};
    blasts(nowhere, false);  // the explosions already going on are history
    g_log_timer = 0;
}

void update(float dt, const float drone_pos[3]) {
    if (!available()) return;
    weather(dt);
    fpv_wind_source sources[FPV_MAX_WIND_SOURCES];
    uint32_t n = helicopters(drone_pos, sources, FPV_MAX_WIND_SOURCES);
    fpv.fpv_wind_set_sources(sources, n);
    blasts(drone_pos, true);

    g_log_timer -= dt;
    if (g_log_timer <= 0) {
        g_log_timer = 10.0f;
        fpvlog::write("wind: weather %.2f (%.1f m/s towards %.0f deg from north), area %d, %u rotor source(s)",
                      sa::weather_wind(), std::max(kCalmMps, sa::weather_wind() * kStormMps),
                      std::atan2(g_dir[0], g_dir[1]) * 57.2958f, sa::current_area(), n);
    }
}

}  // namespace wind
