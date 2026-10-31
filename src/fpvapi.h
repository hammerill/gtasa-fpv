// libfpv.dll, loaded at runtime on the first launch so a missing or broken DLL
// never stops the game (or the rest of the mod) from starting.
#pragma once

#include <libfpv.h>

namespace fpvapi {

#define FPV_FUNCTIONS(X)                                                                         \
    X(fpv_global_config_defaults) X(fpv_global_init) X(fpv_last_error) X(fpv_result_string)      \
    X(fpv_version_string) X(fpv_world_set_raycast_callback) X(fpv_drone_desc_defaults)           \
    X(fpv_drone_create) X(fpv_drone_delete) X(fpv_drone_reset) X(fpv_drone_repair)               \
    X(fpv_drone_tick) X(fpv_input_key) X(fpv_input_get_status) X(fpv_input_set_channels)         \
    X(fpv_texture_get) X(fpv_overlay_is_open) X(fpv_overlay_wants_input) X(fpv_overlay_set_display) \
    X(fpv_overlay_mouse_move) X(fpv_overlay_mouse_button) X(fpv_overlay_mouse_wheel)             \
    X(fpv_overlay_render) X(fpv_osd_draw_data) X(fpv_audio_configure) X(fpv_audio_read)          \
    X(fpv_overlay_set_action) X(fpv_overlay_poll_action) X(fpv_gps_set_home)

// Only in libfpv builds with -DLIBFPV_KAMIKAZE=ON; null otherwise. kamikaze()
// needs poll_detonation..detonate; fpv_kamikaze_set_ending_support (2026-10)
// is missing from older kamikaze builds and stays null there.
#define FPV_KAMIKAZE_FUNCTIONS(X)                                                                \
    X(fpv_kamikaze_poll_detonation) X(fpv_kamikaze_get_state) X(fpv_kamikaze_set_installed)    \
    X(fpv_kamikaze_get_config) X(fpv_kamikaze_detonate) X(fpv_kamikaze_set_config)             \
    X(fpv_kamikaze_set_ending_support)

// Wind (2026-10); null with an older libfpv.dll, which then flies in still air.
#define FPV_WIND_FUNCTIONS(X) X(fpv_wind_defaults) X(fpv_wind_set) X(fpv_wind_set_sources) X(fpv_wind_add_blast)

// The shared per-user data folder (2026-10); null with an older libfpv.dll,
// which then keeps its data in the mod's fpv-data folder.
#define FPV_SHARED_FUNCTIONS(X) X(fpv_shared_data_dir)

struct Api {
#define FPV_FN(name) decltype(&::name) name = nullptr;
    FPV_FUNCTIONS(FPV_FN)
    FPV_KAMIKAZE_FUNCTIONS(FPV_FN)
    FPV_WIND_FUNCTIONS(FPV_FN)
    FPV_SHARED_FUNCTIONS(FPV_FN)
#undef FPV_FN
};

extern Api fpv;

// Loads and initialises libfpv once; later calls return the first result.
bool load();
bool loaded();

// The loaded libfpv.dll carries a warhead (a kamikaze build).
bool kamikaze();

}  // namespace fpvapi
