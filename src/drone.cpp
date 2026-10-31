#include "drone.h"

#include <windows.h>

#include <cmath>

#include "audio.h"
#include "effects.h"
#include "fpvapi.h"
#include "kamikaze.h"
#include "log.h"
#include "render.h"
#include "sa.h"
#include "sfx.h"
#include "standin.h"
#include "wind.h"
#include "world.h"

namespace drone {
namespace {

constexpr float kRadToDeg = 57.2957795f;
constexpr float kNearClip = 0.2f;      // with clear space in front of the lens
constexpr float kMinNearClip = 0.02f;  // when the lens almost touches something
constexpr float kWidestAspect = 2.4f;  // frustum probes cover up to 21:9
constexpr float kCutsceneDistance = 14.0f;  // camera back from the blast, horizontally
constexpr float kCutsceneHeight = 5.0f;     // and up

using fpvapi::fpv;

enum class View { Fpv, Chase, Los, Count };
constexpr uint32_t kMoveCjAction = 1;  // F8 > Fly button id

// Where a flight happens. Live: the game as usual. Frozen: the world is on
// hold (a freeze mod such as Amazing Screenshot's Alt+V, or a script's
// set_gamespeed 0). Replay: a replay plays back (F1, F3). Launched in the last
// two, the drone only watches: it starts under the game's camera, CJ is left
// alone, and the flight ends when that scene does.
enum class Scene { Live, Frozen, Replay };
Scene scene_now() {
    if (sa::replay_playing()) return Scene::Replay;
    if (sa::user_paused() || sa::time_scale() < 0.01f) return Scene::Frozen;
    return Scene::Live;
}
const char *scene_name(Scene s) { return s == Scene::Replay ? "replay" : s == Scene::Frozen ? "frozen world" : "live"; }

fpv_drone *g_drone = nullptr;
Scene g_launch_scene = Scene::Live;
float g_pilot_pos[3];       // a watching flight's pilot and home: the camera at launch
float g_fov_before = 70.0f;  // CDraw's FOV at launch, given back after a watching flight
View g_view = View::Fpv;
bool g_stepped = false;  // camera() ran this frame (inside CCamera::Process)
float g_spawn_pos[3];
float g_spawn_rot[4];
LARGE_INTEGER g_last_tick;
double g_first_sim_time = -1;  // sim clock at the first frame of this flight
fpv_frame g_frame{};
bool g_have_frame = false;  // g_frame is from this flight
bool g_osd_visible = false;
bool g_osd_no_signal = false;

// The camera the game shows this frame, applied inside CCamera::Process.
struct Shot {
    bool on = false;
    float pos[3], forward[3], up[3], right[3];
    float fov;  // vertical degrees
    float near_clip;
};
Shot g_shot;

// The game's FOV is not vertical: CDraw::SetFOV feeds a view window whose
// vertical half-tangent is fov_scale * tan(fov / 2), where the scale depends
// on the aspect ratio and on widescreen fixes. Learned from the RwCamera
// after each frame (observe_view_window), so the vertical FOV comes out right.
float g_fov_scale = 0.75f;   // 4:3 horizontal FOV, the vanilla game
float g_fov_passed = 0;      // what we gave SetFOV this frame, degrees

// Kamikaze builds: after the warhead went off, static in the goggles or an
// outside shot of the blast for a while (F8 > Kamikaze), then a respawn, back
// to CJ, or nothing (the view stays until the player acts). Timed with the
// game's clock, so a paused game pauses it too; while the world is on hold
// (frozen, replay) that clock stops or jumps, so real time counts there.
struct Ending {
    bool on = false;
    bool shown = false;     // the view is up (false: duration 0, straight to "then")
    bool cutscene = false;  // the view is the outside shot (else signal lost)
    bool timed = false;     // false: held until the player respawns, recalls or moves CJ
    uint32_t then = FPV_KAMIKAZE_THEN_RESPAWN;
    uint32_t end_ms = 0;  // g_clock_ms when "then" happens
    View view_before = View::Fpv;  // the camera to go back to after a respawn
};
Ending g_ending;

uint32_t g_clock_ms = 0;  // the ending's clock
uint32_t g_clock_game = 0;
DWORD g_clock_real = 0;

bool watching() { return g_launch_scene != Scene::Live; }
bool world_on_hold() { return scene_now() != Scene::Live; }

void advance_clock() {
    uint32_t game = sa::timer_ms();
    DWORD real = GetTickCount(), real_step = real - g_clock_real;
    g_clock_ms += world_on_hold() ? (real_step > 100 ? 100 : real_step) : game - g_clock_game;
    g_clock_game = game;
    g_clock_real = real;
}

void print(const char *text) { sa::SetHelpMessage(text); }

bool game_has_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Keyboard fallback for players without a radio (libfpv: W/S throttle, A/D yaw,
// arrows pitch/roll, Enter arms, T turtle, B beeper; kamikaze builds: K bomb
// arm, X explode) and the F8 overlay's keys.
void forward_keys() {
    // letters by scan code (physical position, as libfpv wants): on AZERTY the
    // throttle/yaw keys are labelled Z/Q/S/D, and their VKs are 'Z'/'Q'/'S'/'D'
    struct Key { int vk; int fpv; UINT sc = 0; };
    static const Key keys[] = {
        {0, 'W', 0x11}, {0, 'A', 0x1E}, {0, 'S', 0x1F}, {0, 'D', 0x20},
        {0, 'T', 0x14}, {0, 'B', 0x30}, {0, 'K', 0x25}, {0, 'X', 0x2D},
        {VK_RETURN, FPV_KEY_ENTER}, {VK_LEFT, FPV_KEY_LEFT}, {VK_RIGHT, FPV_KEY_RIGHT},
        {VK_UP, FPV_KEY_UP}, {VK_DOWN, FPV_KEY_DOWN}, {VK_F8, FPV_KEY_F8}, {VK_ESCAPE, FPV_KEY_ESCAPE},
        {VK_TAB, FPV_KEY_TAB}, {VK_BACK, FPV_KEY_BACKSPACE}, {VK_SPACE, FPV_KEY_SPACE},
    };
    static bool was_down[sizeof(keys) / sizeof(keys[0])];
    static bool primed = false;  // a key already held when we start is not a press
    bool focus = game_has_focus();
    HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        int vk = keys[i].sc ? (int)MapVirtualKeyExW(keys[i].sc, MAPVK_VSC_TO_VK, layout) : keys[i].vk;
        bool down = focus && vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (primed && down != was_down[i]) fpv.fpv_input_key(keys[i].fpv, down);
        was_down[i] = down;
    }
    primed = true;
}

// Edge-detected key press. The first reading of each key is only a baseline,
// so a key that is already held (or stuck) never counts as a press.
bool key_pressed(int vk) {
    static bool was_down[256], seen[256];
    bool down = game_has_focus() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = seen[vk] && down && !was_down[vk];
    was_down[vk] = down;
    seen[vk] = true;
    return pressed;
}

// CJ can't move while we fly, or while the overlay takes the keyboard. We own
// one bit of CPad::DisablePlayerControls, so scripts' own bits are untouched.
void hold_cj(bool hold) {
    if (sa::replay_playing()) hold = false;  // the bit would end the replay (CReplay::Init)
    uint16_t &bits = sa::pad_disabled_controls();
    bits = hold ? uint16_t(bits | sa::kPadDisabledByFpv) : uint16_t(bits & ~sa::kPadDisabledByFpv);
}

// Heading in radians, 0 = north, counter-clockwise positive (the game's).
float heading_of(const sa::CVector &forward) { return std::atan2(-forward.x, forward.y); }

// The FPV camera rides a few cm above the road: a fixed near plane cuts the
// ground away under the lens and the game shows whatever was left in that
// part of the frame. Probe the view frustum (centre, edge midpoints and
// corners) and pull the near plane in front of the closest geometry.
float near_clip_for(const fpv_camera &c, float fov_y) {
    const float ty = std::tan(fov_y * 0.5f / kRadToDeg), tx = ty * kWidestAspect;
    float nearest = kNearClip / 0.8f;  // depth along the view axis
    for (int sy = -1; sy <= 1; ++sy)
        for (int sx = -1; sx <= 1; ++sx) {
            float d[3], len = 0;
            for (int i = 0; i < 3; ++i) {
                d[i] = c.forward[i] + c.right[i] * tx * float(sx) + c.up[i] * ty * float(sy);
                len += d[i] * d[i];
            }
            len = std::sqrt(len);
            for (float &v : d) v /= len;
            fpv_raycast_hit hit{};
            // depth = distance * cos(angle to the axis) = distance / len
            if (world::raycast(nullptr, c.position, d, nearest * len, &hit) && hit.distance / len < nearest)
                nearest = hit.distance / len;
        }
    float n = nearest * 0.8f;
    return n < kMinNearClip ? kMinNearClip : n > kNearClip ? kNearClip : n;
}

// libfpv widens the FOV for its fisheye so the top and bottom edges of the
// distorted image show the FOV the player chose, but for the camera's
// aspect_hint (4:3 analog). Redo it for the game's real aspect.
float lens_edge(const fpv_camera &c, float aspect) {
    float r2 = 1 / (1 + aspect * aspect);  // squared radius of the top edge centre
    float zoom = c.distortion_zoom > 0 ? c.distortion_zoom : 1;
    return (1 + c.distortion_k1 * r2 + c.distortion_k2 * r2 * r2) / zoom;
}
float lens_fov(const fpv_camera &c) {
    if ((c.distortion_k1 == 0 && c.distortion_k2 == 0) || c.aspect_hint <= 0) return c.fov_y_degrees;
    float chosen = std::tan(c.fov_y_degrees * 0.5f / kRadToDeg) * lens_edge(c, c.aspect_hint);
    float fov = 2 * std::atan(chosen / lens_edge(c, render::aspect())) * kRadToDeg;
    return fov > 170 ? 170 : fov;
}

void set_shot(const float pos[3], const float forward[3], const float up[3], const float right[3], float fov,
              float near_clip) {
    for (int i = 0; i < 3; ++i) {
        g_shot.pos[i] = pos[i];
        g_shot.forward[i] = forward[i];
        g_shot.up[i] = up[i];
        g_shot.right[i] = right[i];
    }
    g_shot.fov = fov;
    g_shot.near_clip = near_clip;
    g_shot.on = true;
}

void apply_camera(const fpv_camera &c, bool fpv_view) {
    float fov = fpv_view && tuning().fov > 0 ? tuning().fov : fpv_view ? lens_fov(c) : c.fov_y_degrees;
    float near_clip = tuning().near_clip > 0 ? tuning().near_clip : fpv_view ? near_clip_for(c, fov) : kNearClip;
    set_shot(c.position, c.forward, c.up, c.right, fov, near_clip);
}

// Points the camera from `from` at `at`, level (no roll).
void aim_camera(const float from[3], const float at[3], float fov) {
    float f[3] = {at[0] - from[0], at[1] - from[1], at[2] - from[2]};
    float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (len < 1e-3f) return;
    for (float &v : f) v /= len;
    float r[3] = {f[1], -f[0], 0};  // forward x world up
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    if (rl < 1e-3f) r[0] = 1, r[1] = 0, rl = 1;
    r[0] /= rl, r[1] /= rl;
    float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    set_shot(from, f, u, r, fov, kNearClip);
}

// Betaflight's home arrow, home distance and GPS Rescue lead back to CJ
// (his own spot while he stands in for the drone).
void update_home() {
    float home[3];
    if (watching())
        for (int i = 0; i < 3; ++i) home[i] = g_pilot_pos[i];
    else
        standin::home(home);
    fpv.fpv_gps_set_home(home);
}

void start() {
    if (!fpvapi::load()) {
        print("FPV: libfpv failed to load, see GTASA.FPV.log");
        return;
    }
    Scene scene = scene_now();
    sa::CPed *cj = sa::FindPlayerPed();
    if (scene == Scene::Live && (!cj || !cj->matrix)) {
        sfx::play(sa::kSoundError);
        return;
    }
    sa::CVehicle *car = scene == Scene::Live ? sa::FindPlayerVehicle() : nullptr;
    // Live: in front of CJ (or his vehicle), resting on the ground. Watching:
    // on the ground a little ahead of the game's camera, wherever it is (in
    // the air where there is none), with the camera as the pilot.
    const sa::CMatrix &m = scene != Scene::Live ? sa::camera_matrix() : car && car->matrix ? *car->matrix : *cj->matrix;
    sa::CVector p = scene != Scene::Live ? m.pos : cj->position();
    float h = heading_of(m.forward);
    float ahead = car ? 4.0f : 2.0f;
    g_spawn_pos[0] = p.x - std::sin(h) * ahead;
    g_spawn_pos[1] = p.y + std::cos(h) * ahead;
    float origin[3] = {g_spawn_pos[0], g_spawn_pos[1], scene != Scene::Live ? p.z : p.z + 1.0f}, down[3] = {0, 0, -1};
    fpv_raycast_hit hit{};
    world::ignore(scene == Scene::Live ? cj : nullptr);
    bool ground = world::raycast(nullptr, origin, down, scene != Scene::Live ? 300.0f : 5.0f, &hit) != 0;
    g_spawn_pos[2] = ground ? origin[2] - hit.distance : scene != Scene::Live ? p.z : p.z - 1.0f;
    g_spawn_rot[0] = 0;
    g_spawn_rot[1] = 0;
    g_spawn_rot[2] = std::sin(h / 2);
    g_spawn_rot[3] = std::cos(h / 2);

    fpv_drone_desc desc;
    fpv.fpv_drone_desc_defaults(&desc);
    for (int i = 0; i < 3; ++i) desc.position[i] = g_spawn_pos[i];
    for (int i = 0; i < 4; ++i) desc.rotation[i] = g_spawn_rot[i];
    desc.pilot_position[0] = p.x;
    desc.pilot_position[1] = p.y;
    desc.pilot_position[2] = p.z;
    fpv_result r = fpv.fpv_drone_create(&desc, &g_drone);
    if (r != FPV_OK) {
        fpvlog::write("fpv_drone_create: %s (%s)", fpv.fpv_result_string(r), fpv.fpv_last_error());
        print("FPV: drone creation failed, see GTASA.FPV.log");
        sfx::play(sa::kSoundError);
        g_drone = nullptr;
        return;
    }
    g_launch_scene = scene;
    g_pilot_pos[0] = p.x, g_pilot_pos[1] = p.y, g_pilot_pos[2] = p.z;
    g_fov_before = sa::draw_fov();
    g_clock_game = sa::timer_ms();
    g_clock_real = GetTickCount();
    fpv.fpv_world_set_raycast_callback(&world::raycast, nullptr);
    if (fpvapi::kamikaze()) {  // detonations of earlier drones are history
        fpv_detonation d;
        d.struct_size = sizeof d;
        while (fpv.fpv_kamikaze_poll_detonation(&d)) {}
    }
    g_ending = Ending{};
    update_home();
    wind::reset();

    hold_cj(true);
    QueryPerformanceCounter(&g_last_tick);
    g_first_sim_time = -1;
    g_have_frame = false;
    g_shot.on = false;
    g_view = View(tuning().view % int(View::Count));
    audio::start();
    if (!watching()) fpv.fpv_overlay_set_action(kMoveCjAction, "Move CJ here (Y)");
    sfx::play(sa::kSoundSelect);

    fpv_input_status st{};
    st.struct_size = sizeof st;
    fpv.fpv_input_get_status(&st);
    fpvlog::write("drone launched at %.2f %.2f %.2f heading %.1f (ground %s, %s, %s), input %d '%s'%s", g_spawn_pos[0],
                  g_spawn_pos[1], g_spawn_pos[2], h * kRadToDeg, ground ? "hit" : "missed", scene_name(scene),
                  watching() ? "under the camera" : car ? "in a vehicle" : "on foot", int(st.source), st.device_name,
                  st.needs_setup ? " (needs setup)" : "");
}

void stop(bool go_home = true) {
    fpv.fpv_overlay_set_action(kMoveCjAction, nullptr);
    audio::stop();  // before the drone: the audio thread reads libfpv's synth
    g_shot.on = false;
    standin::end(go_home);
    hold_cj(false);
    if (g_drone) {
        fpv.fpv_world_set_raycast_callback(nullptr, nullptr);
        world::ignore(nullptr);
        fpv.fpv_drone_delete(g_drone);
        g_drone = nullptr;
    }
    g_ending = Ending{};
    g_osd_visible = false;
    g_osd_no_signal = false;
    // Nothing else sets the FOV again while the world is on hold.
    if (scene_now() != Scene::Live) sa::SetFOV(g_fov_before);
    g_launch_scene = Scene::Live;
    fpvlog::write("drone stopped");
}

// Cutscene: a camera behind and above where the drone came from, pulled in
// front of any wall in the way, watching the blast.
void place_blast_camera(const fpv_detonation &d) {
    float back[2] = {-d.velocity[0], -d.velocity[1]};
    float bl = std::sqrt(back[0] * back[0] + back[1] * back[1]);
    if (bl < 1.0f) {  // dropped straight down or hovered: behind the lens instead
        back[0] = -g_frame.camera.forward[0], back[1] = -g_frame.camera.forward[1];
        bl = std::sqrt(back[0] * back[0] + back[1] * back[1]);
    }
    if (bl < 1e-3f) back[0] = 0, back[1] = -1, bl = 1;
    back[0] /= bl, back[1] /= bl;
    const float *n = d.normal;
    bool has_normal = n[0] * n[0] + n[1] * n[1] + n[2] * n[2] > 0.5f;
    float origin[3];  // off the surface, so the probes start in open air
    for (int i = 0; i < 3; ++i) origin[i] = d.position[i] + (has_normal ? n[i] : (i == 2 ? 1.0f : 0.0f)) * 0.5f;
    const float len = std::sqrt(kCutsceneDistance * kCutsceneDistance + kCutsceneHeight * kCutsceneHeight);
    static const float kTurns[] = {0, 45, -45, 90, -90, 135, -135, 180};
    float best[3] = {0, 0, 0}, best_clear = -1;
    for (float turn : kTurns) {
        float a = turn / kRadToDeg, c = std::cos(a), sn = std::sin(a);
        float dir[3] = {(back[0] * c - back[1] * sn) * kCutsceneDistance / len,
                        (back[0] * sn + back[1] * c) * kCutsceneDistance / len, kCutsceneHeight / len};
        fpv_raycast_hit hit{};
        float clear = world::raycast(nullptr, origin, dir, len, &hit) ? hit.distance : len;
        if (clear > best_clear) {
            best_clear = clear;
            for (int i = 0; i < 3; ++i) best[i] = dir[i];
        }
        if (clear >= len * 0.9f) break;
    }
    float dist_out = best_clear * 0.85f < 2.5f ? 2.5f : best_clear * 0.85f;
    float from[3], at[3] = {d.position[0], d.position[1], d.position[2] + 1.0f};  // the fireball rises
    for (int i = 0; i < 3; ++i) from[i] = origin[i] + best[i] * dist_out;
    aim_camera(from, at, 50.0f);
    fpvlog::write("kamikaze: cutscene camera at %.2f %.2f %.2f (%.1f m clear)", from[0], from[1], from[2], best_clear);
}

const char *then_name(uint32_t then) {
    return then == FPV_KAMIKAZE_THEN_RECALL ? "recall" : then == FPV_KAMIKAZE_THEN_NOTHING ? "nothing" : "respawn";
}

// The player's F8 > Kamikaze choice (or the autotest's), read at the blast.
// The "then" itself runs from tick(), outside the camera hook.
void begin_ending(const fpv_detonation &d) {
    kamikaze::EndingChoice e = kamikaze::ending_choice();
    if (tuning().after_blast >= 0)
        e.view = tuning().after_blast == 1 ? FPV_KAMIKAZE_VIEW_CUTSCENE : FPV_KAMIKAZE_VIEW_SIGNAL_LOST;
    if (tuning().ending_secs >= 0) e.seconds = tuning().ending_secs;
    if (tuning().ending_then >= 0) e.then = uint32_t(tuning().ending_then);
    g_ending = Ending{};
    g_ending.on = true;
    g_ending.then = e.then;
    g_ending.view_before = g_view;
    // "Nothing" keeps the view up whatever the duration: there is nothing else to show.
    g_ending.shown = e.seconds > 0 || e.then == FPV_KAMIKAZE_THEN_NOTHING;
    g_ending.timed = e.then != FPV_KAMIKAZE_THEN_NOTHING;
    g_ending.end_ms = g_clock_ms + (e.seconds > 0 ? uint32_t(e.seconds * 1000.0f) : 0);
    g_ending.cutscene = g_ending.shown && e.view == FPV_KAMIKAZE_VIEW_CUTSCENE;
    fpvlog::write("kamikaze: ending: %s%s, then %s", !g_ending.shown ? "no view" : g_ending.cutscene ? "cutscene" : "signal lost",
                  g_ending.timed && g_ending.shown ? "" : g_ending.shown ? " until the player acts" : "", then_name(e.then));
    if (g_ending.timed && g_ending.shown) fpvlog::write("kamikaze: view for %.1f s (game time)", e.seconds);
    if (!g_ending.shown) return;
    if (g_ending.cutscene) {
        place_blast_camera(d);
    } else {
        g_view = View::Fpv;  // the goggles
        audio::set_static(true);
    }
    g_osd_visible = !g_ending.cutscene;
    g_osd_no_signal = g_osd_visible;
}

// The blast's view is over: back to the live feed (the drone was just reset).
void clear_ending() {
    if (!g_ending.on) return;
    if (g_ending.shown && !g_ending.cutscene) audio::set_static(false);
    g_view = g_ending.view_before;
    g_ending = Ending{};
    g_osd_no_signal = false;
}

// R, or the ending's "respawn": repaired, at the launch point.
void respawn_drone() {
    standin::end(true);  // CJ home first: the spawn's collision must be streamed in
    fpv.fpv_drone_reset(g_drone, g_spawn_pos, g_spawn_rot);
    fpv.fpv_drone_repair(g_drone);
    clear_ending();
}

// One libfpv step; called from inside CCamera::Process so the camera it
// produces is the one this frame renders.
void step() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = float(now.QuadPart - g_last_tick.QuadPart) / float(freq.QuadPart);
    g_last_tick = now;
    if (dt > 0.1f) dt = 0.1f;

    wind::update(dt, g_have_frame ? g_frame.position : g_spawn_pos);
    fpv_frame frame{};
    frame.struct_size = sizeof frame;
    fpv_result r = fpv.fpv_drone_tick(g_drone, dt, &frame);
    if (r != FPV_OK) {
        static int reported = 0;
        if (reported++ < 5) fpvlog::write("fpv_drone_tick: %s", fpv.fpv_result_string(r));
        return;
    }
    // Until libfpv's sim thread has stepped the new drone, the frame shows a
    // stale snapshot (the world origin, or the last drone) and interpolates
    // from it: the camera would jump there. libfpv's clock is not reset per
    // drone, so wait until it moved 50 ms past its first reading.
    if (g_first_sim_time < 0) g_first_sim_time = frame.sim_time;
    if (frame.sim_time < g_first_sim_time + 0.05) return;
    g_frame = frame;
    g_have_frame = true;
    if (!watching()) standin::update(frame.position);
    update_home();
    if (fpvapi::kamikaze()) {
        fpv_detonation d;
        d.struct_size = sizeof d;
        while (fpv.fpv_kamikaze_poll_detonation(&d)) {
            if (world_on_hold())  // nothing would move it: no fireball, no damage when the world goes on
                fpvlog::write("kamikaze: detonation with the world on hold (%s), no explosion", scene_name(scene_now()));
            else
                kamikaze::explode(d);
            if (!g_ending.on) begin_ending(d);
        }
    }
    if (!g_ending.on) g_osd_visible = g_view == View::Fpv;
    if (!g_ending.cutscene)
        apply_camera(g_view == View::Fpv ? frame.camera : g_view == View::Chase ? frame.chase_camera : frame.los_camera,
                     g_view == View::Fpv);

    for (uint32_t i = 0; i < frame.impact_count && i < FPV_MAX_IMPACTS; ++i) {
        const fpv_impact &im = frame.impacts[i];
        if (im.speed < 1.0f) continue;  // resting contact
        fpvlog::write("impact at %.2f %.2f %.2f n %.2f %.2f %.2f speed %.1f impulse %.2f motor %d object %08x",
                      im.position[0], im.position[1], im.position[2], im.normal[0], im.normal[1], im.normal[2], im.speed,
                      im.impulse, int(im.motor_index), im.object_id);
    }
    if (!world_on_hold()) effects::on_frame(frame);  // frozen or replayed peds and cars can't react
    sfx::on_frame(frame);

    // Sparse trace for debugging the axes and the camera mapping.
    static float log_timer = 0;
    log_timer += dt;
    if (log_timer > 1.0f) {
        log_timer = 0;
        if (fpvapi::kamikaze()) {
            fpv_kamikaze_state ks{};
            ks.struct_size = sizeof ks;
            if (fpv.fpv_kamikaze_get_state(g_drone, &ks) == FPV_OK)
                fpvlog::write("kamikaze: warhead %d (%.1f s to live), aux7 %.2f aux8 %.2f", int(ks.warhead),
                              ks.arming_remaining, frame.channels[10], frame.channels[11]);
        }
        fpvlog::write("drone pos %.2f %.2f %.2f flags 0x%x alt %.2f thr %.2f fov %.1f (game %.1f, scale %.3f) near %.3f "
                      "wind %.1f %.1f %.1f arm '%s'",
                      frame.position[0], frame.position[1], frame.position[2], frame.flags, frame.altitude,
                      frame.channels[2], g_shot.fov, g_fov_passed, g_fov_scale, g_shot.near_clip, frame.wind[0],
                      frame.wind[1], frame.wind[2], frame.arming_disable_text);
    }
}

}  // namespace

Tuning &tuning() {
    static Tuning t;
    return t;
}

void inject_channels(const float channels[FPV_CHANNEL_COUNT]) {
    if (fpvapi::loaded()) fpv.fpv_input_set_channels(channels, FPV_CHANNEL_COUNT);
}

bool active() { return g_drone != nullptr; }

bool hide_hud() { return g_drone != nullptr; }

bool hide_text() { return g_drone && g_have_frame && (g_frame.flags & FPV_DRONE_OSD_DISABLED); }

// CJ faces the drone's heading. In a vehicle, the vehicle goes with him.
void move_cj_here() {
    if (watching()) {  // CJ is part of the frozen or replayed scene
        sfx::play(sa::kSoundError);
        return;
    }
    float pos[3] = {g_frame.position[0], g_frame.position[1], g_frame.position[2]};
    const float *f = g_frame.camera.forward;
    float heading = (f[0] * f[0] + f[1] * f[1] > 1e-6f) ? std::atan2(-f[0], f[1]) : 0;
    // Stand on whatever is under the drone (roof, road, floor); the probe
    // ignores CJ. Fall back to the drone's own height.
    float origin[3] = {pos[0], pos[1], pos[2] + 0.5f}, down[3] = {0, 0, -1};
    fpv_raycast_hit hit{};
    bool ground = world::raycast(nullptr, origin, down, 300.0f, &hit) != 0;
    float z = ground ? origin[2] - hit.distance : pos[2];
    stop(false);
    sa::CPed *cj = sa::FindPlayerPed();
    if (!cj) return;
    sa::LoadScene({pos[0], pos[1], z});
    sa::CVehicle *car = sa::FindPlayerVehicle();
    sa::CEntity *mover = car ? car : cj;
    sa::Teleport(mover, {pos[0], pos[1], z + (car ? 1.5f : 1.0f)}, false);
    if (car)
        sa::SetHeading(car, heading);
    else
        sa::set_ped_heading(cj, heading);
    sfx::play(sa::kSoundSelect);
    fpvlog::write("moved CJ%s to the drone: %.2f %.2f %.2f (ground %s) heading %.1f", car ? " and his vehicle" : "",
                  pos[0], pos[1], z, ground ? "hit" : "missed", heading * kRadToDeg);
}

void respawn() {
    if (!g_drone) return;
    if (g_ending.on) fpvlog::write("kamikaze: ending cut short: respawn");
    respawn_drone();
}

bool ending() { return g_ending.on; }

const fpv_frame &last_frame() { return g_frame; }

namespace {
// v rotated by quaternion q {x, y, z, w}.
void rotate(const float q[4], const float v[3], float out[3]) {
    float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
          tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}
}  // namespace

bool model_pose(ModelPose *out) {
    if (!g_drone || !g_have_frame || !g_shot.on || (g_view == View::Fpv && !g_ending.cutscene)) return false;
    const fpv_frame &f = g_frame;
    // libfpv rotations map the host's right/up/forward (east, up, north) to world axes.
    static const float kRight[3] = {1, 0, 0}, kUp[3] = {0, 0, 1}, kForward[3] = {0, 1, 0};
    rotate(f.rotation, kRight, out->right);
    rotate(f.rotation, kUp, out->up);
    rotate(f.rotation, kForward, out->forward);
    for (int i = 0; i < 3; ++i) {
        out->pos[i] = f.position[i];
        out->eye[i] = g_shot.pos[i];
        out->eye_right[i] = g_shot.right[i];
        out->eye_up[i] = g_shot.up[i];
        out->eye_forward[i] = g_shot.forward[i];
    }
    for (int m = 0; m < 4; ++m) {
        // motor hubs in drone-local host coords: x right, y forward, z up
        for (int i = 0; i < 3; ++i) out->motor[m][i] = f.motors[m].position[i];
        out->rpm[m] = f.motors[m].rpm;
    }
    return true;
}

bool osd_frame(fpv_frame *out, bool *no_signal) {
    if (!g_osd_visible || !g_have_frame) return false;
    *out = g_frame;
    *no_signal = g_osd_no_signal;
    return true;
}

void toggle() {
    if (active()) {
        stop();
        sfx::play(sa::kSoundBack);
    } else {
        start();
    }
}

void shutdown() {
    if (active()) stop();
    if (fpvapi::loaded()) hold_cj(false);
}

float camera(float game_fov) {
    if (g_drone) step(), g_stepped = true;
    if (!g_drone || !g_shot.on) return game_fov;
    sa::CMatrix &m = sa::camera_matrix();
    // The game's camera "right" is up x forward, i.e. screen left (measured).
    m.right = {-g_shot.right[0], -g_shot.right[1], -g_shot.right[2]};
    m.forward = {g_shot.forward[0], g_shot.forward[1], g_shot.forward[2]};
    m.up = {g_shot.up[0], g_shot.up[1], g_shot.up[2]};
    m.pos = {g_shot.pos[0], g_shot.pos[1], g_shot.pos[2]};
    if (void *rw = sa::camera_rw()) sa::RwCameraSetNearClipPlane(rw, g_shot.near_clip);
    float t = std::tan(g_shot.fov * 0.5f / kRadToDeg) / g_fov_scale;
    g_fov_passed = 2 * std::atan(t) * kRadToDeg;
    return g_fov_passed;
}

// Frames where CCamera::Process didn't give us the camera: a replay plays
// (CReplay::ProcessReplayCamera instead) or a freeze mod skips its SetFOV.
// Called after CGame::Process, before the frame is drawn: the same steps.
void camera_outside_process() {
    float fov = camera(sa::draw_fov());
    if (!g_shot.on) return;
    sa::SetFOV(fov);
    sa::CalculateDerivedValues(false, true);
    sa::CopyCameraMatrixToRWCam(false);
    static Scene reported = Scene::Live;
    if (scene_now() != reported) {
        reported = scene_now();
        fpvlog::write("camera: applied outside CCamera::Process (%s)", scene_name(reported));
    }
}

void observe_view_window(float tan_half_y) {
    if (!g_drone || !g_shot.on || g_fov_passed <= 0 || tan_half_y <= 0) return;
    float scale = tan_half_y / std::tan(g_fov_passed * 0.5f / kRadToDeg);
    if (std::fabs(scale - g_fov_scale) > 0.002f) {
        static int reported = 0;
        if (reported++ < 10)
            fpvlog::write("camera: view window %.4f for SetFOV %.2f: scale %.4f (was %.4f)", tan_half_y, g_fov_passed,
                          scale, g_fov_scale);
        g_fov_scale = scale;
    }
}

// While the overlay is open on foot, CJ must not walk around as you type.
void hold_cj_for_overlay() {
    static bool held = false;
    bool open = fpv.fpv_overlay_wants_input() != 0;
    if (g_drone || open == held) return;
    hold_cj(open);
    held = open;
}

void tick() {
    if (!fpvapi::loaded()) {
        if (key_pressed(VK_F8)) {  // F8 opens the overlay before the first flight too
            fpvlog::write("F8 pressed, loading libfpv");
            fpvapi::load();
        }
        return;
    }
    kamikaze::init();
    forward_keys();
    hold_cj_for_overlay();
    bool move_cj = false;  // F8 buttons work on foot too, but Move CJ here only in flight
    for (uint32_t id; (id = fpv.fpv_overlay_poll_action()) != 0;) move_cj |= id == kMoveCjAction;
    if (!g_drone) return;
    advance_clock();

    Scene scene = scene_now();
    if (watching() && scene != g_launch_scene) {  // the replay ended, the world goes on (or froze in a replay)
        fpvlog::write("%s over, drone stopped", scene_name(g_launch_scene));
        stop(false);
        sfx::play(sa::kSoundBack);
        return;
    }
    sa::CPed *cj = sa::FindPlayerPed();
    if (!watching() && (!cj || sa::ped_health(cj) <= 0)) {  // flew the warhead into himself: the game takes over
        fpvlog::write("CJ died, drone stopped");
        stop();
        return;
    }
    // During the blast's view, R / F7 / Y (and the F8 button) act at once and
    // replace the ending's own "then".
    bool camera_key = key_pressed('C'), reset_key = key_pressed('R'), cj_key = key_pressed('Y');
    if (!fpv.fpv_overlay_wants_input()) {  // otherwise the keys belong to the overlay
        move_cj |= cj_key;
        if (camera_key && !g_ending.on) {
            g_view = View((int(g_view) + 1) % int(View::Count));
            sfx::play(sa::kSoundHighlight);
        }
        if (reset_key) respawn();
    }
    if (g_ending.on && !move_cj && g_ending.timed && int32_t(g_clock_ms - g_ending.end_ms) >= 0) {
        fpvlog::write("kamikaze: ending over, %s", then_name(g_ending.then));
        if (g_ending.then == FPV_KAMIKAZE_THEN_RECALL) {
            stop();  // as F7: CJ back in control where he stood
            return;
        }
        respawn_drone();
    }
    if (g_ending.on && move_cj && !watching()) fpvlog::write("kamikaze: ending cut short: Move CJ here");
    hold_cj(true);  // scripts may have cleared the pad's state (cutscenes, missions)
    if (move_cj && g_have_frame) move_cj_here();
    if (g_drone && !g_stepped) camera_outside_process();
    g_stepped = false;
}

}  // namespace drone
