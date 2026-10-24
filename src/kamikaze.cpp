#include "kamikaze.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "fpvapi.h"
#include "log.h"
#include "paths.h"
#include "sa.h"

namespace kamikaze {
namespace {

using fpvapi::fpv;

// Up to the 2026-10 libfpv, the mod kept its own "After the blast" choice
// here; libfpv's F8 > Kamikaze tab has it now. A saved "cutscene" carries over once.
std::string old_settings_file() { return paths::mod_dir() + "\\fpv-data\\gtasa-fpv.ini"; }

bool g_ready = false;

void migrate_old_setting() {
    std::string file = old_settings_file();
    FILE *f = std::fopen(file.c_str(), "r");
    if (!f) return;
    char buf[256] = {};
    std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    if (std::strstr(buf, "after_blast=cutscene")) {
        fpv_kamikaze_config kc{};
        kc.struct_size = sizeof kc;
        if (!fpv.fpv_kamikaze_set_config || fpv.fpv_kamikaze_get_config(&kc) != FPV_OK || kc.struct_size < sizeof kc)
            return;  // an older libfpv
        kc.ending_view = FPV_KAMIKAZE_VIEW_CUTSCENE;
        if (fpv.fpv_kamikaze_set_config(&kc) != FPV_OK) return;
        fpvlog::write("kamikaze: old after_blast=cutscene moved to libfpv's ending view");
    }
    std::remove(file.c_str());
}

}  // namespace

EndingChoice ending_choice() {
    EndingChoice e;
    if (!fpvapi::kamikaze()) return e;
    fpv_kamikaze_config kc{};
    kc.struct_size = sizeof kc;
    // An older libfpv fills less (no view/then): those keep the defaults.
    kc.ending_duration = e.seconds;
    kc.ending_view = e.view;
    kc.ending_then = e.then;
    if (fpv.fpv_kamikaze_get_config(&kc) != FPV_OK) return e;
    e.view = kc.ending_view == FPV_KAMIKAZE_VIEW_CUTSCENE ? uint32_t(FPV_KAMIKAZE_VIEW_CUTSCENE) : uint32_t(FPV_KAMIKAZE_VIEW_SIGNAL_LOST);
    e.seconds = kc.ending_duration >= 0 && kc.ending_duration <= 30 ? kc.ending_duration : 3.0f;
    e.then = kc.ending_then <= FPV_KAMIKAZE_THEN_NOTHING ? kc.ending_then : uint32_t(FPV_KAMIKAZE_THEN_RESPAWN);
    return e;
}

void init() {
    if (g_ready || !fpvapi::kamikaze()) return;
    g_ready = true;
    if (fpv.fpv_kamikaze_set_ending_support) {
        const uint32_t views = (1u << FPV_KAMIKAZE_VIEW_SIGNAL_LOST) | (1u << FPV_KAMIKAZE_VIEW_CUTSCENE);
        const uint32_t thens =
            (1u << FPV_KAMIKAZE_THEN_RESPAWN) | (1u << FPV_KAMIKAZE_THEN_RECALL) | (1u << FPV_KAMIKAZE_THEN_NOTHING);
        fpv_result r = fpv.fpv_kamikaze_set_ending_support(views, thens, FPV_KAMIKAZE_VIEW_SIGNAL_LOST,
                                                           FPV_KAMIKAZE_THEN_RESPAWN);
        if (r != FPV_OK) fpvlog::write("fpv_kamikaze_set_ending_support: %s", fpv.fpv_result_string(r));
        migrate_old_setting();
    } else {
        fpvlog::write("kamikaze: libfpv without ending choices (older build): signal lost, then respawn");
    }
    EndingChoice e = ending_choice();
    fpvlog::write("kamikaze: after the blast: %s for %.1f s, then %s",
                  e.view == FPV_KAMIKAZE_VIEW_CUTSCENE ? "cutscene" : "signal lost", e.seconds,
                  e.then == FPV_KAMIKAZE_THEN_RECALL ? "recall" : e.then == FPV_KAMIKAZE_THEN_NOTHING ? "nothing" : "respawn");
}

void explode(const fpv_detonation &d) {
    static const char *causes[] = {"impact", "explode switch", "game"};
    // A 5" strike quad carries a few hundred grams of explosive: grenade-sized.
    // CJ set it off, as far as the world is concerned. Shake -1: the game's
    // own amount for the distance.
    bool added = sa::AddExplosion(nullptr, sa::FindPlayerPed(), sa::kExplosionGrenade,
                                  {d.position[0], d.position[1], d.position[2]}, 0, true, -1.0f, false);
    fpvlog::write("kamikaze: explosion %s", added ? "added" : "refused");
    fpvlog::write("kamikaze: detonation (%s) at %.2f %.2f %.2f speed %.1f object %08x",
                  causes[unsigned(d.cause) < 3 ? d.cause : 2], d.position[0], d.position[1], d.position[2],
                  std::sqrt(d.velocity[0] * d.velocity[0] + d.velocity[1] * d.velocity[1] +
                             d.velocity[2] * d.velocity[2]),
                  d.object_id);
}

}  // namespace kamikaze
