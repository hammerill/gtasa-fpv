#include "damage.h"

#include <algorithm>
#include <cmath>

#include "fpvapi.h"
#include "log.h"
#include "patch.h"
#include "sa.h"

namespace damage {
namespace {

// The amounts INTEGRATION.md suggests: 1 breaks a prop.
constexpr float kSoakPerSecond = 0.5f;  // in water or fire, on each prop in it
constexpr float kBullet = 0.75f;        // a bullet through a prop
constexpr float kBlast = 1.0f;          // an explosion within its radius, all four
constexpr float kRotor = 1.0f;          // into a helicopter's main rotor, all four
constexpr float kPropRadius = 0.0635f;  // 5" props
constexpr float kBulletReach = kPropRadius + 0.01f;
// Fires: a ground fire's flames reach about this far around and up from its
// point; a burning car's (the fire follows the car) a little wider.
constexpr float kFireRadius = 1.2f, kEntityFireRadius = 2.0f, kFireHeight = 2.5f;
constexpr float kRotorThickness = 0.4f;   // m above or below the rotor disc
constexpr float kRotorFullSpeed = 0.15f;  // m_fHeliRotorSpeed at full speed (wind.cpp)
constexpr uint32_t kRotorCooldownMs = 250;

fpv_drone *g_drone = nullptr;
float g_props[4][3];  // world positions of the props, latest step
bool g_have_props = false;
bool g_slot_active[sa::kExplosionCount];
sa::CVector g_slot_pos[sa::kExplosionCount];
uint32_t g_rotor_ms = 0;  // game time of the last rotor hit
int g_reported = 0;

void report(const char *what, int motor, float amount) {
    if (g_reported++ < 40) fpvlog::write("damage: %s, prop %d +%.2f", what, motor, amount);
}

// v rotated by quaternion q {x, y, z, w}.
void rotate(const float q[4], const float v[3], float out[3]) {
    float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
          tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

void update_props(const fpv_frame &f) {
    for (int m = 0; m < 4; ++m) {
        // hubs in drone-local host coords (right, forward, up); the prop sits a little above
        float local[3] = {f.motors[m].position[0], f.motors[m].position[1], f.motors[m].position[2] + 0.025f};
        if (local[0] == 0 && local[1] == 0) local[0] = m & 1 ? 0.08f : -0.08f, local[1] = m & 2 ? -0.08f : 0.08f;
        float w[3];
        rotate(f.rotation, local, w);
        for (int i = 0; i < 3; ++i) g_props[m][i] = f.position[i] + w[i];
    }
    g_have_props = true;
}

// The sea, rivers, lakes and pools: each prop under the surface.
void water(float dt) {
    for (int m = 0; m < 4; ++m) {
        float level = 0;
        if (sa::GetWaterLevel(g_props[m][0], g_props[m][1], g_props[m][2], &level, false, nullptr) &&
            g_props[m][2] < level) {
            fpvapi::damage_props(g_drone, m, kSoakPerSecond * dt);
            report("in water", m, kSoakPerSecond * dt);
        }
    }
}

// CFireManager's fires: on the ground (molotovs, petrol, the flamethrower)
// or on an entity (a burning car or ped), whose position the fire follows.
void fires(float dt) {
    const sa::CFire *fire = sa::fires();
    for (int i = 0; i < sa::kFireCount; ++i) {
        if (!(fire[i].flags & sa::kFireActive)) continue;
        float r = fire[i].target ? kEntityFireRadius : kFireRadius;
        for (int m = 0; m < 4; ++m) {
            float dx = g_props[m][0] - fire[i].pos.x, dy = g_props[m][1] - fire[i].pos.y,
                  dz = g_props[m][2] - fire[i].pos.z;
            if (dx * dx + dy * dy > r * r || dz < -1.0f || dz > kFireHeight) continue;
            fpvapi::damage_props(g_drone, m, kSoakPerSecond * dt);
            report("in fire", m, kSoakPerSecond * dt);
        }
    }
}

// A slot is a new explosion when it turns active or moves (as in wind.cpp).
void explosions(const fpv_frame &f, bool apply) {
    const sa::CExplosionSlot *e = sa::explosions();
    for (int i = 0; i < sa::kExplosionCount; ++i) {
        bool active = e[i].active != 0;
        bool moved = e[i].pos.x != g_slot_pos[i].x || e[i].pos.y != g_slot_pos[i].y || e[i].pos.z != g_slot_pos[i].z;
        bool fresh = active && (!g_slot_active[i] || moved);
        g_slot_active[i] = active;
        g_slot_pos[i] = e[i].pos;
        if (!fresh || !apply) continue;
        float dx = f.position[0] - e[i].pos.x, dy = f.position[1] - e[i].pos.y, dz = f.position[2] - e[i].pos.z;
        float r = std::max(1.0f, e[i].radius);
        if (dx * dx + dy * dy + dz * dz > r * r) continue;
        fpvapi::damage_props(g_drone, -1, kBlast);
        fpvlog::write("damage: explosion type %d %.1f m from the drone (radius %.1f), all props +%.2f", e[i].type,
                      std::sqrt(dx * dx + dy * dy + dz * dz), e[i].radius, kBlast);
    }
}

// Into a turning main rotor: the disc above the fuselage, as wind.cpp models its downwash.
void rotors(const fpv_frame &f) {
    if (sa::timer_ms() - g_rotor_ms < kRotorCooldownMs) return;
    sa::Pool *pool = sa::vehicle_pool();
    if (!pool || !pool->objects) return;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->flags[i] & 0x80) continue;  // free slot
        auto *v = reinterpret_cast<sa::CVehicle *>(pool->objects + i * sa::kVehicleSize);
        if (sa::vehicle_subtype(v) != sa::kVehicleHeli || !v->matrix) continue;
        float rotor = sa::heli_rotor_speed(v);
        if (!(rotor > 0.1f * kRotorFullSpeed)) continue;
        const sa::CMatrix &m = *v->matrix;
        const sa::CBoundingBox *box = sa::model_box(v);
        float length = box ? std::max(box->max.y - box->min.y, box->max.x - box->min.x) : 10.0f;
        float top = box ? box->max.z : 2.0f;
        float radius = std::clamp(0.42f * length, 0.3f, 8.0f);
        float d[3] = {f.position[0] - (m.pos.x + m.up.x * top), f.position[1] - (m.pos.y + m.up.y * top),
                      f.position[2] - (m.pos.z + m.up.z * top)};
        float off_plane = d[0] * m.up.x + d[1] * m.up.y + d[2] * m.up.z;
        float r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - off_plane * off_plane;
        if (std::fabs(off_plane) > kRotorThickness || r2 > (radius + kPropRadius) * (radius + kPropRadius)) continue;
        fpvapi::damage_props(g_drone, -1, kRotor);
        fpvlog::write("damage: helicopter rotor (%.1f m disc, %.1f m from its hub), all props +%.2f", radius,
                      std::sqrt(r2), kRotor);
        g_rotor_ms = sa::timer_ms();
        return;
    }
}

// Distance from p to the segment a..b.
float segment_distance(const float p[3], const sa::CVector &a, const sa::CVector &b) {
    float ab[3] = {b.x - a.x, b.y - a.y, b.z - a.z}, ap[3] = {p[0] - a.x, p[1] - a.y, p[2] - a.z};
    float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = len2 > 1e-6f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0;
    t = std::clamp(t, 0.0f, 1.0f);
    float c[3] = {ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t};
    return std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
}

// CWeapon::DoBulletImpact(owner, victim, start, end, colpoint, int), thiscall:
// every instant-hit bullet (and shotgun pellet) that hit something, from the
// muzzle to where it hit. The drone isn't in the game's collision, so a
// bullet through it goes on to whatever is behind: the props it passed close
// enough to are hit.
uintptr_t g_bullet_impact = 0;
void __attribute__((thiscall)) bullet_impact_hook(void *weapon, sa::CEntity *owner, sa::CEntity *victim,
                                                  sa::CVector *start, sa::CVector *end, sa::CColPoint *cp, int arg) {
    if (g_drone && g_have_props && start && end)
        for (int m = 0; m < 4; ++m)
            if (segment_distance(g_props[m], *start, *end) < kBulletReach) {
                fpvapi::damage_props(g_drone, m, kBullet);
                fpvlog::write("damage: bullet through prop %d (owner %p), +%.2f", m, static_cast<void *>(owner), kBullet);
            }
    reinterpret_cast<void(__attribute__((thiscall)) *)(void *, sa::CEntity *, sa::CEntity *, sa::CVector *,
                                                       sa::CVector *, sa::CColPoint *, int)>(g_bullet_impact)(
        weapon, owner, victim, start, end, cp, arg);
}

}  // namespace

void install_hooks() {
    g_bullet_impact = reinterpret_cast<uintptr_t>(patch::detour(sa::kDoBulletImpact, sa::kDoBulletImpactPrologue,
                                                                sizeof sa::kDoBulletImpactPrologue,
                                                                reinterpret_cast<void *>(&bullet_impact_hook)));
    fpvlog::write("damage: bullet hook %d", g_bullet_impact != 0);
}

void update(fpv_drone *drone, const fpv_frame &frame, float dt) {
    bool fresh = g_drone != drone;
    g_drone = drone;
    if (!fpvapi::fpv.fpv_drone_damage_props) return;  // older libfpv
    update_props(frame);
    explosions(frame, !fresh);  // the explosions going on at the launch are history
    water(dt);
    fires(dt);
    rotors(frame);
}

void reset() {
    g_drone = nullptr;
    g_have_props = false;
    g_reported = 0;
}

}  // namespace damage
