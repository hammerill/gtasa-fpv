#include "damage.h"

#include <algorithm>
#include <cmath>

#include "fpvapi.h"
#include "log.h"
#include "patch.h"
#include "sa.h"
#include "standin.h"

namespace damage {
namespace {

// The amounts INTEGRATION.md suggests: 1 breaks a prop.
constexpr float kSoakPerSecond = 0.5f;  // in water or fire, on each prop in it
constexpr float kBullet = 1.0f;         // a bullet through a prop breaks it
constexpr float kBlast = 1.0f;          // an explosion within its radius, all four
constexpr float kRotor = 1.0f;          // into a helicopter's main rotor, all four
constexpr float kPropRadius = 0.0635f;  // 5" props
// A bullet through the body (frame, battery, flight controller, camera; the
// warhead if there is one), a ball about the centre: the drone goes down.
constexpr float kBodyRadius = 0.06f;
constexpr float kPropThickness = 0.012f;  // m either side of a prop's disc
constexpr float kShotLogRange = 2.0f;     // shots passing this close are logged
// A ped's shot that passed this close to CJ's spine was aimed at him.
constexpr float kAimedAtCj = 1.5f;
// Fires: a ground fire's flames reach about this far around and up from its
// point; a burning car's (the fire follows the car) a little wider.
constexpr float kFireRadius = 1.2f, kEntityFireRadius = 2.0f, kFireHeight = 2.5f;
constexpr float kRotorThickness = 0.4f;   // m above or below the rotor disc
constexpr float kRotorFullSpeed = 0.15f;  // m_fHeliRotorSpeed at full speed (wind.cpp)
constexpr uint32_t kRotorCooldownMs = 250;

fpv_drone *g_drone = nullptr;
float g_props[4][3];  // world positions of the props, latest step
float g_centre[3], g_up[3];
bool g_have_props = false;
bool g_slot_active[sa::kExplosionCount];
sa::CVector g_slot_pos[sa::kExplosionCount];
uint32_t g_rotor_ms = 0;  // game time of the last rotor hit
// Fire and water soak the props a little every frame: one line each time it stops.
struct Soak {
    const char *what;
    float seconds = 0, amount[4] = {};
    bool now = false;
};
Soak g_fire{"in fire"}, g_water{"in water"};
float g_logged_damage[4];  // the props' damage at the last log line

void soak(Soak &s, int motor, float amount) {
    s.amount[motor] += amount;
    s.now = true;
}

void end_frame(Soak &s, float dt) {
    if (s.now) s.seconds += dt;
    else if (s.seconds > 0) {
        fpvlog::write("damage: %s for %.1f s, props +%.2f +%.2f +%.2f +%.2f", s.what, s.seconds, s.amount[0],
                      s.amount[1], s.amount[2], s.amount[3]);
        s = Soak{s.what};
    }
    s.now = false;
}

// libfpv's own hits too: a line whenever a prop is 0.05 worse than last said.
void log_prop_damage(const fpv_frame &f) {
    bool worse = false;
    for (int m = 0; m < 4; ++m) worse |= f.motors[m].prop_damage > g_logged_damage[m] + 0.05f;
    if (!worse) {
        for (int m = 0; m < 4; ++m) g_logged_damage[m] = std::min(g_logged_damage[m], f.motors[m].prop_damage);
        return;  // repaired or respawned: from there
    }
    for (int m = 0; m < 4; ++m) g_logged_damage[m] = f.motors[m].prop_damage;
    fpvlog::write("damage: props now %.2f %.2f %.2f %.2f", g_logged_damage[0], g_logged_damage[1],
                  g_logged_damage[2], g_logged_damage[3]);
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
    const float up[3] = {0, 0, 1};
    rotate(f.rotation, up, g_up);
    for (int i = 0; i < 3; ++i) g_centre[i] = f.position[i];
    g_have_props = true;
}

// The sea, rivers, lakes and pools: each prop under the surface.
void water(float dt) {
    for (int m = 0; m < 4; ++m) {
        float level = 0;
        if (sa::GetWaterLevel(g_props[m][0], g_props[m][1], g_props[m][2], &level, false, nullptr) &&
            g_props[m][2] < level) {
            fpvapi::damage_props(g_drone, m, kSoakPerSecond * dt);
            soak(g_water, m, kSoakPerSecond * dt);
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
            soak(g_fire, m, kSoakPerSecond * dt);
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

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The point of the segment a..b nearest to p.
void nearest_on_segment(const float p[3], const float a[3], const float b[3], float out[3]) {
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    float len2 = dot3(ab, ab);
    float t = len2 > 1e-6f ? std::clamp(dot3(ap, ab) / len2, 0.0f, 1.0f) : 0;
    for (int i = 0; i < 3; ++i) out[i] = a[i] + ab[i] * t;
}

float distance(const float a[3], const float b[3]) {
    float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return std::sqrt(dot3(d, d));
}

// The segment a..b through a prop's disc (centre c, the drone's up n): across
// its plane within the radius, or nearly edge-on through its thickness.
bool through_prop(const float c[3], const float n[3], const float a[3], const float b[3]) {
    float ca[3] = {a[0] - c[0], a[1] - c[1], a[2] - c[2]}, cb[3] = {b[0] - c[0], b[1] - c[1], b[2] - c[2]};
    float da = dot3(ca, n), db = dot3(cb, n);
    if (da * db <= 0 && da != db) {
        float t = da / (da - db), p[3];
        for (int i = 0; i < 3; ++i) p[i] = a[i] + (b[i] - a[i]) * t;
        if (distance(p, c) < kPropRadius) return true;
    }
    float q[3];
    nearest_on_segment(c, a, b, q);
    float cq[3] = {q[0] - c[0], q[1] - c[1], q[2] - c[2]};
    return std::fabs(dot3(cq, n)) < kPropThickness && distance(q, c) < kPropRadius;
}

int g_shots_logged = 0;

// One instant-hit bullet from a to b (where it hit, or as far as it goes).
void shot(sa::CEntity *owner, const sa::CVector &from, const sa::CVector &to) {
    float a[3] = {from.x, from.y, from.z}, b[3] = {to.x, to.y, to.z};
    sa::CPed *cj = sa::FindPlayerPed();
    bool by_cj = owner && owner == cj;
    // CJ standing in at the drone: peds and helicopters aim at his spine, half
    // a metre under the drone. A shot aimed at him is aimed at the drone: the
    // same line, turned about the muzzle to miss the drone by as much as it
    // missed him.
    bool reaimed = false;
    float spine[3];
    if (!by_cj && standin::aim_point(spine)) {
        float q[3];
        nearest_on_segment(spine, a, b, q);
        float to_spine = distance(a, spine), length = distance(a, b);
        if (distance(q, spine) < kAimedAtCj && to_spine > 0.5f) {
            float k = length / to_spine;
            for (int i = 0; i < 3; ++i) b[i] += (g_centre[i] - spine[i]) * k;
            reaimed = true;
        }
    }
    float q[3];
    nearest_on_segment(g_centre, a, b, q);
    float miss = distance(q, g_centre);
    if (miss > kShotLogRange) return;
    sa::EntityType type = owner ? sa::type_of(owner) : sa::EntityType::Nothing;
    const char *who = by_cj                              ? "CJ"
                      : type == sa::EntityType::Ped     ? (reaimed ? "a ped, aimed at CJ there" : "a ped")
                      : type == sa::EntityType::Vehicle ? (reaimed ? "a vehicle, aimed at CJ there" : "a vehicle")
                                                        : "someone";
    if (miss < kBodyRadius) {
        fpvlog::write("damage: bullet from %s (%.0f m) through the body, %.2f m from the centre", who,
                      distance(a, g_centre), miss);
        // The warhead is on the body: it goes off, armed or not. Without one, the drone falls.
        if (!(fpvapi::kamikaze() && fpvapi::fpv.fpv_kamikaze_detonate(g_drone, 1) == FPV_OK))
            fpvapi::damage_props(g_drone, -1, kBlast);
        return;
    }
    bool hit = false;
    for (int m = 0; m < 4; ++m)
        if (through_prop(g_props[m], g_up, a, b)) {
            fpvapi::damage_props(g_drone, m, kBullet);
            fpvlog::write("damage: bullet from %s (%.0f m) through prop %d, broken", who, distance(a, g_centre), m);
            hit = true;
        }
    if (!hit && g_shots_logged++ < 200) {
        // Where it passed, from the drone: a bias (all under it, say) shows a wrong aim point.
        float spine_gap = reaimed ? distance(spine, g_centre) : -1;
        fpvlog::write("damage: bullet from %s (%.0f m) missed the drone by %.2f m (%+.2f %+.2f %+.2f), spine %.2f m off",
                      who, distance(a, g_centre), miss, q[0] - g_centre[0], q[1] - g_centre[1], q[2] - g_centre[2],
                      spine_gap);
    }
}

// CWeapon::DoBulletImpact(owner, victim, start, end, colpoint, int), thiscall:
// every instant-hit bullet (and shotgun pellet), from the muzzle to where it
// hit or as far as it goes (FireInstantHit calls it with no victim too). The
// drone isn't in the game's collision, so a bullet through it goes on to
// whatever is behind.
uintptr_t g_bullet_impact = 0;
void __attribute__((thiscall)) bullet_impact_hook(void *weapon, sa::CEntity *owner, sa::CEntity *victim,
                                                  sa::CVector *start, sa::CVector *end, sa::CColPoint *cp, int arg) {
    if (g_drone && g_have_props && start && end) {
        // It stops at what it hit: nothing behind a wall is hit.
        sa::CVector to = *end;
        auto d2 = [&](const sa::CVector &p) {
            float x = p.x - start->x, y = p.y - start->y, z = p.z - start->z;
            return x * x + y * y + z * z;
        };
        if (victim && cp && d2(cp->point) < d2(to)) to = cp->point;
        shot(owner, *start, to);
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
    end_frame(g_water, dt);
    end_frame(g_fire, dt);
    rotors(frame);
    log_prop_damage(frame);
}

void reset() {
    g_drone = nullptr;
    g_have_props = false;
    g_fire = Soak{"in fire"}, g_water = Soak{"in water"};
    for (float &d : g_logged_damage) d = 0;
    g_shots_logged = 0;
}

}  // namespace damage
