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

constexpr uint32_t kEndingAction = 2;  // F8 > Fly button id (1 is Move CJ here)

std::string settings_file() { return paths::mod_dir() + "\\fpv-data\\gtasa-fpv.ini"; }

Ending g_ending = Ending::SignalLost;
bool g_ready = false;

void load_setting() {
    char buf[256] = {};
    if (FILE *f = std::fopen(settings_file().c_str(), "r")) {
        std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
    }
    g_ending = std::strstr(buf, "after_blast=cutscene") ? Ending::Cutscene : Ending::SignalLost;
}

void save_setting() {
    if (FILE *f = std::fopen(settings_file().c_str(), "w")) {
        std::fprintf(f, "after_blast=%s\n", g_ending == Ending::Cutscene ? "cutscene" : "signal_lost");
        std::fclose(f);
    }
}

void update_button() {
    fpv.fpv_overlay_set_action(kEndingAction, g_ending == Ending::Cutscene ? "After the blast: Cutscene (click to change)"
                                                                           : "After the blast: Signal lost (click to change)");
}

}  // namespace

Ending ending() { return g_ending; }

float ending_seconds() {
    fpv_kamikaze_config kc{};
    kc.struct_size = sizeof kc;
    kc.ending_duration = 3.0f;
    if (fpvapi::kamikaze()) fpv.fpv_kamikaze_get_config(&kc);
    return kc.ending_duration > 0 ? kc.ending_duration : 3.0f;
}

void init() {
    if (g_ready || !fpvapi::kamikaze()) return;
    g_ready = true;
    load_setting();
    update_button();
    fpvlog::write("kamikaze: after the blast: %s", g_ending == Ending::Cutscene ? "cutscene" : "signal lost");
}

bool on_action(uint32_t id) {
    if (id != kEndingAction || !g_ready) return false;
    g_ending = g_ending == Ending::Cutscene ? Ending::SignalLost : Ending::Cutscene;
    save_setting();
    update_button();
    fpvlog::write("kamikaze: after the blast now: %s", g_ending == Ending::Cutscene ? "cutscene" : "signal lost");
    return true;
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
