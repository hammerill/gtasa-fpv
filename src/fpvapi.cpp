#include "fpvapi.h"

#include <windows.h>

#include <filesystem>
#include <string>

#include "log.h"
#include "paths.h"

namespace fpvapi {

Api fpv;

namespace {

bool g_loaded = false;
bool g_failed = false;

void on_fpv_log(void *, fpv_log_level level, const char *message) {
    static const char *names[] = {"debug", "info", "warn", "error"};
    fpvlog::write("libfpv %s: %s", names[level & 3], message);
}

// The mod's own files in fpv-data, which stay there: test triggers,
// screenshots and the pre-libfpv-settings ini that kamikaze.cpp migrates.
bool mod_own_file(const std::filesystem::path &name) {
    return name == "shots" || name == "autotest.txt" || name == "shot.txt" || name == "gtasa-fpv.ini";
}

// libfpv's shared folder (%APPDATA%\libfpv), so the radio mappings, the tune
// and the settings follow the pilot between libfpv games and survive a game
// reinstall. The first time, it takes over what earlier versions kept in
// fpv-data, unless another game already created it (then the pilot's setup
// there wins). Falls back to fpv-data with an older libfpv.dll or when there
// is no home folder.
std::string pick_data_dir() {
    namespace fs = std::filesystem;
    std::string old_dir = paths::mod_dir() + "\\fpv-data";
    if (!fpv.fpv_shared_data_dir) {
        fpvlog::write("libfpv.dll has no fpv_shared_data_dir (older libfpv), using %s", old_dir.c_str());
        return old_dir;
    }
    char shared[1024];
    fpv_result r = fpv.fpv_shared_data_dir(shared, sizeof shared);
    if (r != FPV_OK) {
        fpvlog::write("fpv_shared_data_dir: %s, using %s", fpv.fpv_result_string(r), old_dir.c_str());
        return old_dir;
    }
    // libfpv's path is UTF-8; the game's narrow strings are the ANSI code page.
    fs::path shared_path = fs::u8path(shared), old_path(old_dir);
    std::error_code ec;
    if (!fs::exists(shared_path, ec) && fs::is_directory(old_path, ec)) {
        fs::create_directories(shared_path, ec);
        for (const auto &entry : fs::directory_iterator(old_path, ec)) {
            if (mod_own_file(entry.path().filename())) continue;
            std::error_code copy_ec;
            fs::copy(entry.path(), shared_path / entry.path().filename(),
                     fs::copy_options::recursive | fs::copy_options::skip_existing, copy_ec);
            if (copy_ec)
                fpvlog::write("cannot copy %s: %s", entry.path().string().c_str(), copy_ec.message().c_str());
        }
        fpvlog::write("moved settings from %s to %s (the old folder is kept)", old_dir.c_str(), shared);
    }
    return shared;
}

}  // namespace

bool loaded() { return g_loaded; }

bool kamikaze() {
    return g_loaded && fpv.fpv_kamikaze_poll_detonation && fpv.fpv_kamikaze_get_state && fpv.fpv_kamikaze_set_installed &&
           fpv.fpv_kamikaze_get_config && fpv.fpv_kamikaze_detonate;
}

bool load() {
    if (g_loaded) return true;
    if (g_failed) return false;
    g_failed = true;  // until proven otherwise; don't retry every key press

    std::string dll = paths::mod_dir() + "\\libfpv.dll";
    HMODULE lib = LoadLibraryA(dll.c_str());
    if (!lib) {
        fpvlog::write("cannot load %s (error %lu)", dll.c_str(), GetLastError());
        return false;
    }
    bool ok = true;
#define FPV_FN(name)                                                                                     \
    fpv.name = reinterpret_cast<decltype(fpv.name)>(reinterpret_cast<void *>(GetProcAddress(lib, #name))); \
    if (!fpv.name) {                                                                                     \
        fpvlog::write("libfpv.dll has no %s", #name);                                                    \
        ok = false;                                                                                      \
    }
    FPV_FUNCTIONS(FPV_FN)
#undef FPV_FN
    if (!ok) return false;
#define FPV_FN(name) \
    fpv.name = reinterpret_cast<decltype(fpv.name)>(reinterpret_cast<void *>(GetProcAddress(lib, #name)));
    FPV_KAMIKAZE_FUNCTIONS(FPV_FN)
    FPV_WIND_FUNCTIONS(FPV_FN)
    FPV_SHARED_FUNCTIONS(FPV_FN)
#undef FPV_FN

    std::string data_dir = pick_data_dir();
    fpv_global_config cfg;
    fpv.fpv_global_config_defaults(&cfg);
    cfg.data_dir = data_dir.c_str();
    // GTA SA: metres, X east, Y north, Z up.
    cfg.coords = {FPV_AXIS_POS_X, FPV_AXIS_POS_Z, FPV_AXIS_POS_Y, 1.0f};
    // Prefer the loader's callback: libfpv keeps it across core reloads.
    if (auto loader_log = reinterpret_cast<fpv_log_fn>(reinterpret_cast<void *>(
            GetProcAddress(GetModuleHandleA("GTASA.FPV.asi"), "gtasa_fpv_libfpv_log"))))
        cfg.log = loader_log;
    else
        cfg.log = &on_fpv_log;
    cfg.log_level = FPV_LOG_INFO;
    fpv_result r = fpv.fpv_global_init(&cfg);
    if (r != FPV_OK && r != FPV_ERROR_ALREADY_INITIALIZED) {
        fpvlog::write("fpv_global_init: %s (%s)", fpv.fpv_result_string(r), fpv.fpv_last_error());
        return false;
    }
    g_failed = false;
    g_loaded = true;
    fpvlog::write("libfpv %s initialised (%s, %s), data in %s", fpv.fpv_version_string(),
                  kamikaze() ? "kamikaze build" : "no warhead", fpv.fpv_wind_set ? "wind" : "no wind (older libfpv)",
                  data_dir.c_str());
    return true;
}

}  // namespace fpvapi
