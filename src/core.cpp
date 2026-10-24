// GTA San Andreas FPV drone mode, powered by libfpv. This is
// GTASA.FPV.core.dll; the GTASA.FPV.asi loader calls it once per frame, right
// after CGame::Process, and hot-reloads it. The core installs its own hooks
// on the first tick and removes them before it is unloaded.
#include <windows.h>

#include <d3d9.h>

#include "autotest.h"
#include "drone.h"
#include "fpvapi.h"
#include "log.h"
#include "patch.h"
#include "render.h"
#include "sa.h"
#include "standin.h"

namespace {

// What the redirected calls went to before (the game, or another mod).
struct Original {
    uintptr_t set_fov, render_scene, idle_end_update, frontend_end_update, hud, radio_name, pickup_text, messages_2d,
        hud_after_fade, messages_after_fade, mouse_set_pos, update_pads;
    void *release_video_memory;
};
Original g;

using VoidFn = void(__cdecl *)();

// CCamera::Process -> CDraw::SetFOV(fov): our camera goes in here.
void __cdecl set_fov_hook(float fov) { reinterpret_cast<void(__cdecl *)(float)>(g.set_fov)(drone::camera(fov)); }

// Idle -> RenderScene(): the world is drawn, the drone model goes in with it.
void __cdecl render_scene_hook() {
    reinterpret_cast<VoidFn>(g.render_scene)();
    if (IDirect3DDevice9 *dev = sa::GetD3DDevice()) render::drone_model(dev);
}

void *end_update(uintptr_t original, void *camera, bool in_game) {
    if (IDirect3DDevice9 *dev = sa::GetD3DDevice()) {
        if (in_game)
            if (void *rw = sa::scene_camera()) drone::observe_view_window(sa::rw_view_window_y(rw));
        render::frame(dev, in_game);
    }
    return reinterpret_cast<void *(__cdecl *)(void *)>(original)(camera);
}
void *__cdecl idle_end_update_hook(void *camera) { return end_update(g.idle_end_update, camera, true); }
void *__cdecl frontend_end_update_hook(void *camera) { return end_update(g.frontend_end_update, camera, false); }

// The HUD, radar and radio station name would cover the FPV feed; with the
// pilot's OSD disable switch on, help boxes, subtitles and other text go too.
void __cdecl hud_hook() {
    if (!drone::hide_hud()) reinterpret_cast<VoidFn>(g.hud)();
}
void __attribute__((thiscall)) radio_name_hook(void *audio_engine) {
    if (!drone::hide_hud()) reinterpret_cast<void(__attribute__((thiscall)) *)(void *)>(g.radio_name)(audio_engine);
}
void __cdecl pickup_text_hook() {
    if (!drone::hide_hud()) reinterpret_cast<VoidFn>(g.pickup_text)();
}
void __cdecl messages_2d_hook(bool flag) {
    if (!drone::hide_text()) reinterpret_cast<void(__cdecl *)(bool)>(g.messages_2d)(flag);
}
void __cdecl hud_after_fade_hook() {
    if (!drone::hide_text()) reinterpret_cast<VoidFn>(g.hud_after_fade)();
}
void __cdecl messages_after_fade_hook(bool flag) {
    if (!drone::hide_text()) reinterpret_cast<void(__cdecl *)(bool)>(g.messages_after_fade)(flag);
}

// Idle -> RsMouseSetPos(&centre): the game pins the cursor to the middle of
// the screen each frame, which leaves the overlay's mouse nowhere to go.
void __cdecl mouse_set_pos_hook(void *pos) {
    if (fpvapi::loaded() && fpvapi::fpv.fpv_overlay_is_open()) return;
    reinterpret_cast<void(__cdecl *)(void *)>(g.mouse_set_pos)(pos);
}

// CGame::Process -> CPad::UpdatePads(): Esc closes libfpv's overlay (or its
// popup), so the game must not see it and open the pause menu. It stays hidden until
// released, or the press that closed the overlay pauses the game a frame later.
void __cdecl update_pads_hook() {
    static bool swallow_esc = false;
    reinterpret_cast<VoidFn>(g.update_pads)();
    int16_t &esc = sa::new_key_esc();
    if (!esc) swallow_esc = false;
    else if (fpvapi::loaded() && fpvapi::fpv.fpv_overlay_is_open()) swallow_esc = true;
    if (swallow_esc) esc = 0;
}

// D3D9DeviceReleaseVideoMemory: RenderWare is about to Reset the device (alt-tab,
// resolution change). Our D3DPOOL_DEFAULT texture must go first or Reset fails.
int __cdecl release_video_memory_hook() {
    render::release();
    return reinterpret_cast<int(__cdecl *)()>(g.release_video_memory)();
}
const uint8_t kReleaseVideoMemoryPrologue[] = {0x53, 0x83, 0xC9, 0xFF, 0x33, 0xDB};  // push ebx; or ecx,-1; xor ebx,ebx

uintptr_t hook_call(const sa::CallSite &at, void *hook) { return patch::redirect_call(at.site, at.fn, hook); }

void install_hooks() {
    g.set_fov = hook_call(sa::kCameraSetFov, reinterpret_cast<void *>(&set_fov_hook));
    g.render_scene = hook_call(sa::kIdleRenderScene, reinterpret_cast<void *>(&render_scene_hook));
    g.idle_end_update = hook_call(sa::kIdleEndUpdate, reinterpret_cast<void *>(&idle_end_update_hook));
    g.frontend_end_update = hook_call(sa::kFrontendEndUpdate, reinterpret_cast<void *>(&frontend_end_update_hook));
    g.hud = hook_call(sa::kRender2dHud, reinterpret_cast<void *>(&hud_hook));
    g.radio_name = hook_call(sa::kRender2dRadioName, reinterpret_cast<void *>(&radio_name_hook));
    g.pickup_text = hook_call(sa::kRender2dPickupText, reinterpret_cast<void *>(&pickup_text_hook));
    g.messages_2d = hook_call(sa::kRender2dMessages, reinterpret_cast<void *>(&messages_2d_hook));
    g.hud_after_fade = hook_call(sa::kIdleHudAfterFade, reinterpret_cast<void *>(&hud_after_fade_hook));
    g.messages_after_fade = hook_call(sa::kIdleMessages, reinterpret_cast<void *>(&messages_after_fade_hook));
    g.mouse_set_pos = hook_call(sa::kIdleMouseSetPos, reinterpret_cast<void *>(&mouse_set_pos_hook));
    g.update_pads = hook_call(sa::kGameUpdatePads, reinterpret_cast<void *>(&update_pads_hook));
    g.release_video_memory = patch::detour(0x7F7F70, kReleaseVideoMemoryPrologue, sizeof kReleaseVideoMemoryPrologue,
                                           reinterpret_cast<void *>(&release_video_memory_hook));
    standin::install_hooks();
    fpvlog::write("hooks: camera %d, scene %d, frame %d/%d, hud %d %d %d, text %d %d %d, mouse %d, pads %d, device reset %d", g.set_fov != 0, g.render_scene != 0,
                  g.idle_end_update != 0, g.frontend_end_update != 0, g.hud != 0, g.radio_name != 0,
                  g.pickup_text != 0, g.messages_2d != 0, g.hud_after_fade != 0, g.messages_after_fade != 0,
                  g.mouse_set_pos != 0, g.update_pads != 0, g.release_video_memory != nullptr);
}

// The first reading of each key is only a baseline: a key held while the
// core (re)loads must not count as a press.
bool key_pressed(int vk) {
    static bool was_down[256], seen[256];
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = seen[vk] && down && !was_down[vk];
    was_down[vk] = down;
    seen[vk] = true;
    return pressed;
}

void ensure_hooks() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    install_hooks();
}

void tick() {
    static bool first = true;
    if (first) {
        first = false;
        fpvlog::write("first game tick");
    }
    ensure_hooks();
    if (key_pressed(VK_F7)) drone::toggle();
    autotest::tick();
    drone::tick();
}

}  // namespace

extern "C" __declspec(dllexport) void gtasa_fpv_core_tick() { tick(); }

// The menus' frame: only the hooks (screenshots and the overlay work there too).
extern "C" __declspec(dllexport) void gtasa_fpv_core_menu_tick() { ensure_hooks(); }

// Called before the loader unloads this core: leave nothing pointing into it.
extern "C" __declspec(dllexport) void gtasa_fpv_core_unload() {
    drone::shutdown();
    autotest::abort();
    render::release();
    patch::restore_all();
    fpvlog::write("core unloaded");
}
