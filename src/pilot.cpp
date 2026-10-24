#include "pilot.h"

#include <windows.h>

#include <d3d9.h>

#include <cmath>
#include <vector>

#include "drone.h"
#include "log.h"
#include "patch.h"

namespace pilot {
namespace {

// Idle (0x53E920): the render list is built, then PreRender, shadows, RenderScene, RenderEffects.
constexpr sa::CallSite kIdleConstructRenderList = {0x53E9F9, 0x5556E0};  // CRenderer::ConstructRenderList()
constexpr sa::CallSite kIdleRenderEffects = {0x53EAD3, 0x53E170};        // RenderEffects()

// The pose: otb.ifp (anim.img), a punter at the off-track betting shop
// reading his slip: standing, head down, both hands in front at the waist,
// which is how a pilot holds a radio.
constexpr const char *kAnimBlock = "otb";
constexpr const char *kAnimName = "betslp_loop";
constexpr int kIfpStreamingBase = 25575;  // streaming ids of anim.img's IFPs
constexpr int kStreamKeepInMemory = 0x8, kStreamPriority = 0x10;
constexpr int kAnimStarted = 0x1, kAnimLooped = 0x2;
constexpr int kAnimIdle = 3;  // ANIM_ID_IDLE in a ped's motion group

// CAnimManager::ms_aAnimBlocks entries: name[16], loaded at +0x10.
inline bool block_loaded(int index) { return reinterpret_cast<const uint8_t *>(0xB5D4A0 + index * 0x20)[0x10] != 0; }
inline const void *block(int index) { return reinterpret_cast<const void *>(0xB5D4A0 + index * 0x20); }

inline int GetAnimationBlockIndex(const char *name) { return reinterpret_cast<int(__cdecl *)(const char *)>(0x4D3990)(name); }
inline void RequestModel(int id, int flags) { reinterpret_cast<void(__cdecl *)(int, int)>(0x4087E0)(id, flags); }
inline void *GetAnimation(const char *name, const void *anim_block) {
    return reinterpret_cast<void *(__cdecl *)(const char *, const void *)>(0x4D42F0)(name, anim_block);
}
// CAnimManager::BlendAnimation: blends this one in, every other full-body anim out (and deletes them faded).
inline void *BlendAnimation(void *clump, void *hierarchy, int flags, float delta) {
    return reinterpret_cast<void *(__cdecl *)(void *, void *, int, float)>(0x4D4410)(clump, hierarchy, flags, delta);
}
inline void *BlendAnimation(void *clump, int group, int anim, float delta) {
    return reinterpret_cast<void *(__cdecl *)(void *, int, int, float)>(0x4D4610)(clump, group, anim, delta);
}
// CEntity::UpdateRwMatrix (matrix -> RwFrame), UpdateRwFrame (dirty the frame's objects),
// UpdateRpHAnim (bone matrices from the frame and the anims, in world space).
inline void UpdateRW(sa::CEntity *e) { reinterpret_cast<void(__attribute__((thiscall)) *)(sa::CEntity *)>(0x446F90)(e); }
inline void UpdateRwFrame(sa::CEntity *e) { reinterpret_cast<void(__attribute__((thiscall)) *)(sa::CEntity *)>(0x532B00)(e); }
inline void UpdateRpHAnim(sa::CEntity *e) { reinterpret_cast<void(__attribute__((thiscall)) *)(sa::CEntity *)>(0x532B20)(e); }
inline void *GetAnimHierarchyFromSkinClump(void *clump) { return reinterpret_cast<void *(__cdecl *)(void *)>(0x734A40)(clump); }
inline int RpHAnimIDGetIndex(void *hierarchy, int bone) {
    return reinterpret_cast<int(__cdecl *)(void *, int)>(0x7C51A0)(hierarchy, bone);
}
inline uint8_t *RpHAnimHierarchyGetMatrixArray(void *hierarchy) {
    return reinterpret_cast<uint8_t *(__cdecl *)(void *)>(0x7C5120)(hierarchy);
}

inline void *&rw_object(sa::CEntity *e) { return *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(e) + 0x18); }
inline int motion_group(sa::CPed *p) { return *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(p) + 0x4D4); }

// ePedBones.
constexpr int kBoneNeck = 5, kBoneHead = 8, kBoneRightHand = 24, kBoneRightFinger = 25, kBoneLeftHand = 34,
              kBoneLeftFinger = 35;

struct Original {
    uintptr_t construct_render_list, render_effects;
};
Original g;

int g_block = -1;
void *g_anim = nullptr;      // CAnimBlendHierarchy*
sa::CPed *g_posed = nullptr;  // CJ, in the pose

bool g_away = false;  // CJ's real position is at the drone
sa::CVector g_home{};
bool g_swapped = false;  // CJ is at home for this frame's render passes
sa::CVector g_saved_pos{};

bool load_anim() {
    if (g_anim) return true;
    if (g_block < 0) {
        g_block = GetAnimationBlockIndex(kAnimBlock);
        if (g_block < 0) {
            static bool reported = false;
            if (!reported) fpvlog::write("pilot: no anim block '%s'", kAnimBlock);
            reported = true;
            return false;
        }
        RequestModel(g_block + kIfpStreamingBase, kStreamKeepInMemory | kStreamPriority);
        fpvlog::write("pilot: requested anim block %s (%d)", kAnimBlock, g_block);
    }
    if (!block_loaded(g_block)) return false;  // the streaming loads it within a few frames
    g_anim = GetAnimation(kAnimName, block(g_block));
    fpvlog::write("pilot: anim %s %s", kAnimName, g_anim ? "loaded" : "missing");
    return g_anim != nullptr;
}

void release() {
    sa::CPed *cj = g_posed;
    g_posed = nullptr;
    if (!cj || cj != sa::FindPlayerPed() || !rw_object(cj)) return;
    BlendAnimation(rw_object(cj), motion_group(cj), kAnimIdle, 4.0f);  // the pose fades out and is deleted
    fpvlog::write("pilot: pose released");
}

void swap_in() {
    if (!g_away || g_swapped) return;
    sa::CPed *cj = sa::FindPlayerPed();
    if (!cj || !cj->matrix || sa::FindPlayerVehicle()) return;
    g_saved_pos = cj->matrix->pos;
    cj->matrix->pos = g_home;
    sa::entity_flags(cj) |= sa::kEntityVisible;
    UpdateRW(cj);
    UpdateRwFrame(cj);
    g_swapped = true;
}

void swap_out() {
    if (!g_swapped) return;
    g_swapped = false;
    sa::CPed *cj = sa::FindPlayerPed();
    if (!cj || !cj->matrix) return;
    cj->matrix->pos = g_saved_pos;
    sa::entity_flags(cj) &= ~sa::kEntityVisible;
    UpdateRW(cj);
    UpdateRwFrame(cj);
}

// --- The radio: a small procedural transmitter between CJ's hands ---

struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a) {
    float l = length(a);
    return l > 1e-5f ? a * (1.0f / l) : V3{0, 0, 1};
}

struct Vertex {
    float x, y, z;
    DWORD color;
};
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
std::vector<Vertex> g_mesh;
float g_light[3] = {1, 1, 1};

DWORD shade(DWORD argb, float k) {
    auto ch = [&](int shift, float light) {
        float v = float((argb >> shift) & 0xff) * k * light;
        return DWORD(v > 255 ? 255 : v) << shift;
    };
    return 0xff000000u | ch(16, g_light[0]) | ch(8, g_light[1]) | ch(0, g_light[2]);
}

// A box around c with half-extent axes a, b, h; faces shaded by how much they face up.
void box(V3 c, V3 a, V3 b, V3 h, DWORD color) {
    V3 corner[8];
    for (int i = 0; i < 8; ++i)
        corner[i] = c + a * (i & 1 ? 1.f : -1.f) + b * (i & 2 ? 1.f : -1.f) + h * (i & 4 ? 1.f : -1.f);
    static const int faces[6][4] = {{4, 5, 7, 6}, {0, 2, 3, 1}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    V3 normals[6] = {normalize(h), normalize(h) * -1, normalize(b) * -1, normalize(b), normalize(a) * -1, normalize(a)};
    for (int f = 0; f < 6; ++f) {
        DWORD col = shade(color, 0.6f + 0.4f * normals[f].z);
        const int *q = faces[f];
        for (int k : {q[0], q[1], q[2], q[0], q[2], q[3]}) g_mesh.push_back({corner[k].x, corner[k].y, corner[k].z, col});
    }
}

V3 bone(uint8_t *matrices, void *hierarchy, int id) {
    int i = RpHAnimIDGetIndex(hierarchy, id);
    const float *pos = reinterpret_cast<const float *>(matrices + i * 64 + 48);  // RwMatrix pos
    return {pos[0], pos[1], pos[2]};
}

// A gamepad-style FPV radio (two gimbals, switches, antenna), about 17 x 11 x 5 cm.
bool build_radio(sa::CPed *cj) {
    g_mesh.clear();
    void *clump = rw_object(cj);
    if (!clump) return false;
    UpdateRpHAnim(cj);  // CJ's bones where he is drawn this frame
    void *hierarchy = GetAnimHierarchyFromSkinClump(clump);
    if (!hierarchy) return false;
    uint8_t *m = RpHAnimHierarchyGetMatrixArray(hierarchy);
    if (!m) return false;
    V3 rh = (bone(m, hierarchy, kBoneRightHand) + bone(m, hierarchy, kBoneRightFinger)) * 0.5f;
    V3 lh = (bone(m, hierarchy, kBoneLeftHand) + bone(m, hierarchy, kBoneLeftFinger)) * 0.5f;
    V3 head = (bone(m, hierarchy, kBoneNeck) + bone(m, hierarchy, kBoneHead)) * 0.5f;
    V3 centre = (rh + lh) * 0.5f;
    if (length(centre - head) > 1.5f) return false;  // bones not in place yet

    const float *amb = sa::ambient_light_objects(), *dir = sa::directional_light();
    for (int i = 0; i < 3; ++i) {
        float l = amb[i] + 0.4f * dir[i];
        g_light[i] = l < 0.05f ? 0.05f : l > 1.0f ? 1.0f : l;
    }
    V3 X = normalize(rh - lh);                                   // CJ's right
    V3 N = head - centre;                                        // the face, towards his eyes
    N = normalize(N - X * dot(N, X));
    V3 Z = cross(N, X);                                          // up the radio, the antenna's way
    float w = length(rh - lh) + 0.02f;
    w = w < 0.15f ? 0.15f : w > 0.21f ? 0.21f : w;
    V3 c = centre - N * 0.01f;
    const DWORD body = 0xff2b2b30, grip = 0xff141416, metal = 0xffa0a0a8, cap = 0xff1e1e20, ring = 0xff3c3c44,
                accent = 0xffd06818, screen = 0xff1a3050;
    box(c, X * (w * 0.5f), N * 0.022f, Z * 0.05f, body);
    for (float s : {-1.0f, 1.0f}) {
        box(c + X * (s * (w * 0.5f - 0.022f)) - Z * 0.06f, X * 0.022f, N * 0.024f, Z * 0.03f, grip);  // grips
        box(c + X * (s * (w * 0.5f - 0.004f)), X * 0.004f, N * 0.023f, Z * 0.035f, accent);          // side trim
        V3 g = c + X * (s * w * 0.26f);
        box(g + N * 0.0235f, X * 0.021f, N * 0.0015f, Z * 0.021f, ring);    // gimbal
        box(g + N * 0.034f, X * 0.0035f, N * 0.011f, Z * 0.0035f, metal);  // stick
        box(g + N * 0.047f, X * 0.0065f, N * 0.003f, Z * 0.0065f, cap);    // stick end
        for (float k : {0.36f, 0.44f})                                       // switches on the top edge
            box(c + X * (s * w * k) + Z * 0.058f, X * 0.003f, N * 0.003f, Z * 0.009f, metal);
    }
    box(c + N * 0.023f - Z * 0.028f, X * 0.018f, N * 0.0012f, Z * 0.012f, screen);
    V3 a0 = c + X * (w * 0.18f) + Z * 0.05f;
    V3 along = normalize(Z * 0.9f - N * 0.35f);
    box(a0 + along * 0.06f, X * 0.005f, normalize(cross(along, X)) * 0.005f, along * 0.06f, grip);  // antenna
    return true;
}

void draw_radio(sa::CPed *cj) {
    IDirect3DDevice9 *dev = sa::GetD3DDevice();
    if (!dev || !build_radio(cj)) return;
    static const D3DRENDERSTATETYPE kStates[] = {
        D3DRS_ZENABLE,    D3DRS_ZWRITEENABLE, D3DRS_ZFUNC,           D3DRS_CULLMODE,         D3DRS_LIGHTING,
        D3DRS_FOGENABLE,  D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_COLORWRITEENABLE, D3DRS_STENCILENABLE,
        D3DRS_SCISSORTESTENABLE,
    };
    static const D3DTEXTURESTAGESTATETYPE kStage0[] = {D3DTSS_COLOROP, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG2};
    DWORD rs[sizeof kStates / sizeof kStates[0]], tss[4], tss1_color, tss1_alpha, fvf;
    for (size_t i = 0; i < sizeof kStates / sizeof kStates[0]; ++i) dev->GetRenderState(kStates[i], &rs[i]);
    for (int i = 0; i < 4; ++i) dev->GetTextureStageState(0, kStage0[i], &tss[i]);
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &tss1_color);
    dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &tss1_alpha);
    IDirect3DBaseTexture9 *texture = nullptr;
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    dev->GetTexture(0, &texture);
    dev->GetVertexShader(&vs);
    dev->GetPixelShader(&ps);
    dev->GetVertexDeclaration(&decl);
    dev->GetFVF(&fvf);
    D3DMATRIX world, identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    dev->GetTransform(D3DTS_WORLD, &world);

    dev->SetTransform(D3DTS_WORLD, &identity);  // world-space vertices; the scene's view and projection stay
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(kFvf);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    // RGB only: the game's post effects read the frame's alpha.
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(g_mesh.size() / 3), g_mesh.data(), sizeof(Vertex));

    dev->SetTransform(D3DTS_WORLD, &world);
    for (size_t i = 0; i < sizeof kStates / sizeof kStates[0]; ++i) dev->SetRenderState(kStates[i], rs[i]);
    for (int i = 0; i < 4; ++i) dev->SetTextureStageState(0, kStage0[i], tss[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, tss1_color);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, tss1_alpha);
    dev->SetTexture(0, texture);
    dev->SetVertexShader(vs);
    dev->SetPixelShader(ps);
    if (decl)
        dev->SetVertexDeclaration(decl);
    else
        dev->SetFVF(fvf);
    for (IUnknown *u : {static_cast<IUnknown *>(texture), static_cast<IUnknown *>(vs), static_cast<IUnknown *>(ps),
                        static_cast<IUnknown *>(decl)})
        if (u) u->Release();
}

// Idle -> CRenderer::ConstructRenderList(): CJ goes home for the render passes.
void __cdecl construct_render_list_hook() {
    swap_in();
    reinterpret_cast<void(__cdecl *)()>(g.construct_render_list)();
}

// Idle -> RenderEffects(): the world is drawn; the radio goes in, CJ back to the drone.
void __cdecl render_effects_hook() {
    reinterpret_cast<void(__cdecl *)()>(g.render_effects)();
    if (g_posed && !drone::active()) release();
    sa::CPed *cj = sa::FindPlayerPed();
    if (g_posed && cj == g_posed && cj->matrix && !sa::FindPlayerVehicle() && (!g_away || g_swapped)) draw_radio(cj);
    swap_out();
}

}  // namespace

void install_hooks() {
    g.construct_render_list = patch::redirect_call(kIdleConstructRenderList.site, kIdleConstructRenderList.fn,
                                                   reinterpret_cast<void *>(&construct_render_list_hook));
    g.render_effects = patch::redirect_call(kIdleRenderEffects.site, kIdleRenderEffects.fn,
                                            reinterpret_cast<void *>(&render_effects_hook));
    fpvlog::write("pilot: hooks render list %d, effects %d", g.construct_render_list != 0, g.render_effects != 0);
}

void hold(sa::CPed *cj) {
    swap_out();  // a frame that never reached RenderEffects
    if (!cj || !rw_object(cj) || !load_anim()) return;
    BlendAnimation(rw_object(cj), g_anim, kAnimStarted | kAnimLooped, 4.0f);
    if (g_posed != cj) fpvlog::write("pilot: CJ in the pilot's pose");
    g_posed = cj;
}

void away(const sa::CVector *home) {
    swap_out();
    g_away = home != nullptr;
    if (home) g_home = *home;
}

}  // namespace pilot
