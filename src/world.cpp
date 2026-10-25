#include "world.h"

#include "log.h"

namespace world {
namespace {

sa::CEntity *g_ignore = nullptr;

// Kind and script handle of an entity, packed as an fpv object_id.
uint32_t classify(sa::CEntity *e) {
    if (!e) return 0;
    switch (sa::type_of(e)) {
        case sa::EntityType::Ped: return uint32_t(Kind::Ped) << 28 | (uint32_t(sa::GetPedRef(e)) & 0x0fffffffu);
        case sa::EntityType::Vehicle:
            return uint32_t(Kind::Vehicle) << 28 | (uint32_t(sa::GetVehicleRef(e)) & 0x0fffffffu);
        case sa::EntityType::Object:
            return uint32_t(Kind::Object) << 28 | (uint32_t(sa::GetObjectRef(e)) & 0x0fffffffu);
        default: return uint32_t(Kind::Other) << 28;
    }
}

}  // namespace

void ignore(sa::CEntity *entity) { g_ignore = entity; }

bool probe(const float origin[3], const float dir[3], float max_distance, fpv_raycast_hit *hit,
           sa::CEntity **entity_out, uint8_t *surface_out) {
    sa::CVector from{origin[0], origin[1], origin[2]};
    sa::CVector to{origin[0] + dir[0] * max_distance, origin[1] + dir[1] * max_distance,
                   origin[2] + dir[2] * max_distance};
    sa::CColPoint cp{};
    sa::CEntity *entity = nullptr;
    // Everything solid: buildings, vehicles, peds, objects and dummies (objects
    // not created yet), glass and fences too (no see-through skipping).
    sa::CEntity *saved = sa::ignore_entity();
    sa::ignore_entity() = g_ignore;
    bool ok = sa::ProcessLineOfSight(from, to, cp, entity, true, true, true, true, true, false, false, false);
    sa::ignore_entity() = saved;
    if (!ok) return false;

    float dx = cp.point.x - origin[0], dy = cp.point.y - origin[1], dz = cp.point.z - origin[2];
    float distance = dx * dir[0] + dy * dir[1] + dz * dir[2];
    if (distance < 0 || distance > max_distance) return false;

    const sa::CVector &n = cp.normal;
    float len2 = n.x * n.x + n.y * n.y + n.z * n.z;
    if (len2 > 0.81f && len2 < 1.21f && n.x * dir[0] + n.y * dir[1] + n.z * dir[2] < 0) {
        hit->normal[0] = n.x;
        hit->normal[1] = n.y;
        hit->normal[2] = n.z;
    } else {  // no trustworthy normal: treat it as facing straight back at the ray
        hit->normal[0] = -dir[0];
        hit->normal[1] = -dir[1];
        hit->normal[2] = -dir[2];
    }
    hit->distance = distance;
    hit->material = 0;
    hit->object_id = classify(entity);
    if (entity_out) *entity_out = entity;
    if (surface_out) *surface_out = cp.surface_b;

    static sa::CEntity *last_entity = nullptr;
    static int traced = 0;
    if (traced < 10 || (entity != last_entity && traced < 60)) {
        ++traced;
        last_entity = entity;
        fpvlog::write("world: hit d %.2f entity %p type %d id %08x surface %u pos %.2f %.2f %.2f n %.2f %.2f %.2f",
                      distance, static_cast<void *>(entity), entity ? int(sa::type_of(entity)) : -1, hit->object_id,
                      cp.surface_b, cp.point.x, cp.point.y, cp.point.z, hit->normal[0], hit->normal[1],
                      hit->normal[2]);
    }
    return true;
}

int raycast(void *, const float origin[3], const float dir[3], float max_distance, fpv_raycast_hit *hit) {
    return probe(origin, dir, max_distance, hit, nullptr) ? 1 : 0;
}

}  // namespace world
