// GTASA.FPV.asi: a thin loader around GTASA.FPV.core.dll. It owns the log and
// the game-loop hook, and reloads the core when a new build is deployed (the
// file's timestamp changes), so the game needn't restart. The core is loaded
// from a copy so the build can overwrite the original. SA runs its game logic
// and rendering on one thread, so the core is only ever entered from here
// and from the hooks it installs itself (it removes them before unloading).
#include <windows.h>

#include <string>

#include "log.h"
#include "patch.h"
#include "paths.h"
#include "sa.h"

namespace {

using CoreFn = void (*)();

HMODULE g_core = nullptr;
CoreFn g_core_tick = nullptr;
CoreFn g_core_unload = nullptr;
CoreFn g_core_menu_tick = nullptr;
FILETIME g_core_time{};
uintptr_t g_game_process = 0;  // CGame::Process, or another mod's hook of it
uintptr_t g_update_pads = 0;   // CPad::UpdatePads in the menus

std::string core_path() { return paths::mod_dir() + "\\GTASA.FPV.core.dll"; }
std::string live_copy_path() { return paths::mod_dir() + "\\GTASA.FPV.core.live.dll"; }

bool core_changed() {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(core_path().c_str(), GetFileExInfoStandard, &data)) return false;
    if (CompareFileTime(&data.ftLastWriteTime, &g_core_time) == 0) return false;
    g_core_time = data.ftLastWriteTime;
    return true;
}

void unload_core() {
    if (!g_core) return;
    if (g_core_unload) g_core_unload();
    FreeLibrary(g_core);
    g_core = nullptr;
    g_core_tick = g_core_unload = g_core_menu_tick = nullptr;
}

void load_core() {
    unload_core();
    if (!CopyFileA(core_path().c_str(), live_copy_path().c_str(), FALSE)) {
        fpvlog::write("loader: cannot copy the core (error %lu)", GetLastError());
        return;
    }
    g_core = LoadLibraryA(live_copy_path().c_str());
    if (!g_core) {
        fpvlog::write("loader: cannot load %s (error %lu)", live_copy_path().c_str(), GetLastError());
        return;
    }
    auto get = [](const char *name) { return reinterpret_cast<void *>(GetProcAddress(g_core, name)); };
    g_core_tick = reinterpret_cast<CoreFn>(get("gtasa_fpv_core_tick"));
    g_core_unload = reinterpret_cast<CoreFn>(get("gtasa_fpv_core_unload"));
    g_core_menu_tick = reinterpret_cast<CoreFn>(get("gtasa_fpv_core_menu_tick"));
    fpvlog::write("loader: core loaded (tick %p)", reinterpret_cast<void *>(g_core_tick));
}

void poll_core() {
    static int poll = 0;
    if (poll++ % 30 == 0 && core_changed()) load_core();
}

// Replaces Idle's call to CGame::Process: the world has just been updated
// (including TheCamera), nothing of this frame is rendered yet.
void __cdecl game_process_hook() {
    reinterpret_cast<void(__cdecl *)()>(g_game_process)();
    poll_core();
    if (g_core_tick) g_core_tick();
}

// FrontendIdle's CPad::UpdatePads: the menus' frame (before a save is loaded,
// and in the pause menu).
void __cdecl update_pads_hook() {
    reinterpret_cast<void(__cdecl *)()>(g_update_pads)();
    poll_core();
    if (g_core_menu_tick) g_core_menu_tick();
}

}  // namespace

// libfpv's log callback lives here, not in the core, because libfpv keeps
// calling it from its own threads across core reloads.
extern "C" __declspec(dllexport) void gtasa_fpv_libfpv_log(void *, int level, const char *message) {
    static const char *names[] = {"debug", "info", "warn", "error"};
    fpvlog::write("libfpv %s: %s", names[level & 3], message);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        fpvlog::write("GTASA.FPV attached from %s", paths::mod_dir().c_str());
        if (!patch::is_sa_10_us()) {
            fpvlog::write("loader: this gta_sa.exe is not 1.0 US; the mod stays off");
            return TRUE;
        }
        g_game_process = patch::redirect_call(sa::kIdleGameProcess.site, sa::kIdleGameProcess.fn,
                                              reinterpret_cast<void *>(&game_process_hook));
        g_update_pads = patch::redirect_call(sa::kFrontendUpdatePads.site, sa::kFrontendUpdatePads.fn,
                                             reinterpret_cast<void *>(&update_pads_hook));
        if (g_game_process && g_update_pads) fpvlog::write("loader: game loop and menu loop hooked");
    }
    return TRUE;
}
