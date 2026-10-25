#include "sfx.h"

#include <windows.h>

#include "log.h"
#include "world.h"

namespace sfx {
namespace {

constexpr float kImpactSpeed = 3.0f;  // m/s: lighter touches and scrapes stay silent
constexpr DWORD kCooldownMs = 150;    // one sound per hit, not one per contact frame

DWORD g_last_impact = 0;

}  // namespace

void play(sa::FrontendSound sound) { sa::ReportFrontendAudioEvent(sound); }

void on_frame(const fpv_frame &frame) {
    const fpv_impact *hardest = nullptr;
    for (uint32_t i = 0; i < frame.impact_count && i < FPV_MAX_IMPACTS; ++i)
        if (!hardest || frame.impacts[i].speed > hardest->speed) hardest = &frame.impacts[i];
    if (!hardest || hardest->speed < kImpactSpeed || GetTickCount() - g_last_impact < kCooldownMs) return;

    // The game's collision sounds want the entity and its surface: probe the
    // contact again, from just off it, into it.
    const fpv_impact &im = *hardest;
    float origin[3], dir[3];
    for (int i = 0; i < 3; ++i) origin[i] = im.position[i] + im.normal[i] * 0.25f, dir[i] = -im.normal[i];
    fpv_raycast_hit hit{};
    sa::CEntity *entity = nullptr;
    uint8_t surface = 0;
    if (!world::probe(origin, dir, 0.5f, &hit, &entity, &surface) || !entity) return;
    g_last_impact = GetTickCount();
    sa::CVector at{im.position[0], im.position[1], im.position[2]};
    sa::CVector normal{im.normal[0], im.normal[1], im.normal[2]};
    // CPhysical::ApplyCollision reports the speed change per 1/50 s.
    sa::ReportCollision(entity, entity, sa::kSurfacePlastic, surface, at, &normal, im.speed / 50.0f, 1.0f, true);
    static int reported = 0;
    if (reported++ < 20) fpvlog::write("sfx: impact %.1f m/s on surface %u", im.speed, unsigned(surface));
}

}  // namespace sfx
