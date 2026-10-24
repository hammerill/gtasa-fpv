#include "render.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "drone.h"
#include "fpvapi.h"
#include "log.h"
#include "paths.h"
#include "sa.h"

namespace render {
namespace {

using fpvapi::fpv;

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct Texture {
    IDirect3DTexture9 *tex = nullptr;
    uint32_t revision = 0;
};
Texture g_textures[3];  // indexed by FPV_TEXTURE_* (1 overlay font, 2 OSD font)
std::vector<Vertex> g_vertices;
LARGE_INTEGER g_last;
std::atomic<float> g_aspect{16.0f / 9.0f};

// Fisheye: a copy of the finished frame, drawn back through a warped grid.
IDirect3DTexture9 *g_scene = nullptr;
UINT g_scene_w = 0, g_scene_h = 0;
D3DFORMAT g_scene_format = D3DFMT_UNKNOWN;
constexpr int kGridX = 64, kGridY = 36;  // cells; the warp is smooth, so this is exact to a pixel
std::vector<Vertex> g_grid;
std::vector<uint16_t> g_grid_indices;

// Signal lost: analog static, regenerated every frame and stretched to the screen.
constexpr UINT kStaticW = 320, kStaticH = 180;
IDirect3DTexture9 *g_static = nullptr;
uint32_t g_noise_seed = 0x2545f491u;

IDirect3DTexture9 *texture(IDirect3DDevice9 *dev, uint64_t id) {
    if (id >= 3) return nullptr;
    fpv_texture t{};
    if (fpv.fpv_texture_get(id, &t) != FPV_OK || !t.rgba) return nullptr;
    Texture &slot = g_textures[id];
    if (slot.tex && slot.revision == t.revision) return slot.tex;
    if (slot.tex) slot.tex->Release();
    slot.tex = nullptr;
    if (FAILED(dev->CreateTexture(t.width, t.height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &slot.tex, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(slot.tex->LockRect(0, &lr, nullptr, 0))) {
        for (uint32_t y = 0; y < t.height; ++y) {
            const uint8_t *src = t.rgba + size_t(y) * t.width * 4;
            uint8_t *dst = static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch;
            for (uint32_t x = 0; x < t.width; ++x, src += 4, dst += 4) {  // RGBA -> BGRA
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
            }
        }
        slot.tex->UnlockRect(0);
    }
    slot.revision = t.revision;
    return slot.tex;
}

// Fixed-function state for alpha-blended screen-space quads, as in Dear
// ImGui's DX9 backend.
void setup_state(IDirect3DDevice9 *dev, float w, float h) {
    D3DVIEWPORT9 vp = {0, 0, DWORD(w), DWORD(h), 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(kFvf);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
}

void draw(IDirect3DDevice9 *dev, const fpv_draw_data &dd) {
    for (uint32_t l = 0; l < dd.list_count; ++l) {
        const fpv_draw_list &list = dd.lists[l];
        g_vertices.resize(list.vertex_count);
        for (uint32_t i = 0; i < list.vertex_count; ++i) {
            const fpv_draw_vertex &s = list.vertices[i];
            uint32_t c = s.color;  // ImGui ABGR -> D3D ARGB
            c = (c & 0xff00ff00u) | ((c & 0xffu) << 16) | ((c >> 16) & 0xffu);
            g_vertices[i] = {s.pos[0] - 0.5f, s.pos[1] - 0.5f, 0.0f, 1.0f, c, s.uv[0], s.uv[1]};
        }
        for (uint32_t c = 0; c < list.command_count; ++c) {
            const fpv_draw_cmd &cmd = list.commands[c];
            if (!cmd.index_count || cmd.vertex_offset >= list.vertex_count) continue;
            RECT clip = {LONG(cmd.clip_rect[0]), LONG(cmd.clip_rect[1]), LONG(cmd.clip_rect[2]), LONG(cmd.clip_rect[3])};
            if (clip.right <= clip.left || clip.bottom <= clip.top) continue;
            dev->SetScissorRect(&clip);
            dev->SetTexture(0, texture(dev, cmd.texture_id));
            dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, list.vertex_count - cmd.vertex_offset,
                                        cmd.index_count / 3, list.indices + cmd.index_offset, D3DFMT_INDEX32,
                                        g_vertices.data() + cmd.vertex_offset, sizeof(Vertex));
        }
    }
}

// libfpv's lens (docs/INTEGRATION.md#lens-distortion) as a mesh instead of a
// pixel shader: each grid vertex at output position uv samples the copied
// scene at c * (1 + k1 r2 + k2 r2^2) / zoom, with |c| = 1 at the corners.
// The game drew the frame with the widened FOV from frame.camera.
bool draw_lens(IDirect3DDevice9 *dev, IDirect3DSurface9 *backbuffer, const D3DSURFACE_DESC &desc,
               const fpv_camera &cam) {
    if (!g_scene || g_scene_w != desc.Width || g_scene_h != desc.Height || g_scene_format != desc.Format) {
        if (g_scene) g_scene->Release();
        g_scene = nullptr;
        if (FAILED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
                                      &g_scene, nullptr)))
            return false;
        g_scene_w = desc.Width, g_scene_h = desc.Height, g_scene_format = desc.Format;
    }
    IDirect3DSurface9 *copy = nullptr;
    if (FAILED(g_scene->GetSurfaceLevel(0, &copy))) return false;
    HRESULT hr = dev->StretchRect(backbuffer, nullptr, copy, nullptr, D3DTEXF_NONE);
    copy->Release();
    if (FAILED(hr)) return false;

    const float w = float(desc.Width), h = float(desc.Height), a = w / h;
    const float sx = a / (0.5f * std::sqrt(a * a + 1)), sy = 1 / (0.5f * std::sqrt(a * a + 1));
    const float k1 = cam.distortion_k1, k2 = cam.distortion_k2, zoom = cam.distortion_zoom > 0 ? cam.distortion_zoom : 1;
    g_grid.resize((kGridX + 1) * (kGridY + 1));
    for (int y = 0; y <= kGridY; ++y)
        for (int x = 0; x <= kGridX; ++x) {
            float u = float(x) / kGridX, v = float(y) / kGridY;
            float cx = (u - 0.5f) * sx, cy = (v - 0.5f) * sy, r2 = cx * cx + cy * cy;
            float m = (1 + k1 * r2 + k2 * r2 * r2) / zoom;
            g_grid[y * (kGridX + 1) + x] = {u * w - 0.5f, v * h - 0.5f, 0.0f, 1.0f, 0xffffffffu,
                                            cx * m / sx + 0.5f, cy * m / sy + 0.5f};
        }
    if (g_grid_indices.empty())
        for (int y = 0; y < kGridY; ++y)
            for (int x = 0; x < kGridX; ++x) {
                uint16_t i = uint16_t(y * (kGridX + 1) + x), j = uint16_t(i + kGridX + 1);
                for (uint16_t k : {i, uint16_t(i + 1), j, uint16_t(i + 1), uint16_t(j + 1), j}) g_grid_indices.push_back(k);
            }
    RECT full = {0, 0, LONG(desc.Width), LONG(desc.Height)};
    dev->SetScissorRect(&full);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetTexture(0, g_scene);
    dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, UINT(g_grid.size()), UINT(g_grid_indices.size() / 3),
                                g_grid_indices.data(), D3DFMT_INDEX16, g_grid.data(), sizeof(Vertex));
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    return true;
}

uint32_t noise() {
    g_noise_seed ^= g_noise_seed << 13, g_noise_seed ^= g_noise_seed >> 17, g_noise_seed ^= g_noise_seed << 5;
    return g_noise_seed;
}

// Grey snow with brighter and darker rolling bands, like an analog receiver
// that lost its transmitter.
bool draw_static(IDirect3DDevice9 *dev, float w, float h) {
    if (!g_static &&
        FAILED(dev->CreateTexture(kStaticW, kStaticH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_static, nullptr)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(g_static->LockRect(0, &lr, nullptr, 0))) return false;
    static float band_phase = 0;
    band_phase += 0.07f + float(noise() & 0xff) / 255.0f * 0.05f;
    for (UINT y = 0; y < kStaticH; ++y) {
        float band = 0.75f + 0.25f * std::sin(float(y) * 0.045f + band_phase);
        if ((noise() & 63) == 0) band *= 1.35f;  // a torn line now and then
        uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch);
        for (UINT x = 0; x < kStaticW; ++x) {
            float v = float(noise() & 0xff) * band;
            uint32_t g = v > 255 ? 255 : uint32_t(v);
            row[x] = 0xff000000u | g << 16 | g << 8 | g;
        }
    }
    g_static->UnlockRect(0);
    Vertex quad[4] = {{-0.5f, -0.5f, 0, 1, 0xffffffffu, 0, 0},
                      {w - 0.5f, -0.5f, 0, 1, 0xffffffffu, 1, 0},
                      {-0.5f, h - 0.5f, 0, 1, 0xffffffffu, 0, 1},
                      {w - 0.5f, h - 0.5f, 0, 1, 0xffffffffu, 1, 1}};
    RECT full = {0, 0, LONG(w), LONG(h)};
    dev->SetScissorRect(&full);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetTexture(0, g_static);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    return true;
}

// Self-test screenshots: creating fpv-data/shot.txt saves the next finished
// frame (with our passes) to fpv-data/shots/<time>.bmp and deletes the file.
// Window capture from outside does not always work (a WSL in session 0).
void save_screenshot(IDirect3DDevice9 *dev, IDirect3DSurface9 *backbuffer, const D3DSURFACE_DESC &desc) {
    static int frames = 0;
    static const std::string data = paths::mod_dir() + "\\fpv-data", trigger = data + "\\shot.txt";
    if (++frames % 10 != 0 || GetFileAttributesA(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(trigger.c_str());
    IDirect3DSurface9 *rt = nullptr, *mem = nullptr;
    bool ok = SUCCEEDED(dev->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                &rt, nullptr)) &&
              SUCCEEDED(dev->StretchRect(backbuffer, nullptr, rt, nullptr, D3DTEXF_NONE)) &&
              SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                        &mem, nullptr)) &&
              SUCCEEDED(dev->GetRenderTargetData(rt, mem));
    D3DLOCKED_RECT lr;
    if (ok && SUCCEEDED(mem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        CreateDirectoryA((data + "\\shots").c_str(), nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\shots\\%02d%02d%02d-%03d.bmp", data.c_str(), t.wHour, t.wMinute,
                 t.wSecond, t.wMilliseconds);
        if (FILE *f = std::fopen(path, "wb")) {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof ih;
            ih.biWidth = LONG(desc.Width);
            ih.biHeight = -LONG(desc.Height);  // top-down
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4d42;
            fh.bfOffBits = sizeof fh + sizeof ih;
            fh.bfSize = fh.bfOffBits + desc.Width * desc.Height * 4;
            std::fwrite(&fh, sizeof fh, 1, f);
            std::fwrite(&ih, sizeof ih, 1, f);
            for (UINT y = 0; y < desc.Height; ++y)
                std::fwrite(static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch, 4, desc.Width, f);
            std::fclose(f);
            fpvlog::write("render: screenshot %s", path);
        }
        mem->UnlockRect();
    } else {
        fpvlog::write("render: screenshot failed (format %d)", int(desc.Format));
    }
    if (mem) mem->Release();
    if (rt) rt->Release();
}

// Mouse for the overlay, in back buffer pixels.
void feed_mouse(IDirect3DDevice9 *dev, float w, float h) {
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
    POINT p;
    RECT rc;
    if (!GetCursorPos(&p) || !ScreenToClient(cp.hFocusWindow, &p) || !GetClientRect(cp.hFocusWindow, &rc)) return;
    float sx = rc.right > 0 ? w / float(rc.right) : 1.0f, sy = rc.bottom > 0 ? h / float(rc.bottom) : 1.0f;
    fpv.fpv_overlay_mouse_move(float(p.x) * sx, float(p.y) * sy);
    static bool was[3];
    const int vk[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int b = 0; b < 3; ++b) {
        bool down = (GetAsyncKeyState(vk[b]) & 0x8000) != 0;
        if (down != was[b]) fpv.fpv_overlay_mouse_button(b, down);
        was[b] = down;
    }
}

// Everything setup_state() and draw() change, so it can be put back.
const D3DRENDERSTATETYPE kRenderStates[] = {
    D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE,
    D3DRS_ALPHABLENDENABLE, D3DRS_BLENDOP, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_RANGEFOGENABLE,
    D3DRS_SPECULARENABLE, D3DRS_STENCILENABLE, D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE,
    D3DRS_COLORWRITEENABLE, D3DRS_BLENDOPALPHA, D3DRS_ZFUNC,
};
const D3DTEXTURESTAGESTATETYPE kStage0States[] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
    D3DTSS_TEXTURETRANSFORMFLAGS,
};
const D3DSAMPLERSTATETYPE kSamplerStates[] = {
    D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
};
constexpr size_t kNumRs = sizeof kRenderStates / sizeof kRenderStates[0];
constexpr size_t kNumTss = sizeof kStage0States / sizeof kStage0States[0];
constexpr size_t kNumSs = sizeof kSamplerStates / sizeof kSamplerStates[0];

struct SavedState {
    DWORD rs[kNumRs], tss0[kNumTss], tss1_color, tss1_alpha, ss[kNumSs];
    D3DVIEWPORT9 viewport;
    RECT scissor;
    IDirect3DBaseTexture9 *texture = nullptr;
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    DWORD fvf;
    IDirect3DVertexBuffer9 *stream = nullptr;
    UINT stream_offset, stream_stride;
    IDirect3DIndexBuffer9 *indices = nullptr;

    void save(IDirect3DDevice9 *dev) {
        for (size_t i = 0; i < kNumRs; ++i) dev->GetRenderState(kRenderStates[i], &rs[i]);
        for (size_t i = 0; i < kNumTss; ++i) dev->GetTextureStageState(0, kStage0States[i], &tss0[i]);
        dev->GetTextureStageState(1, D3DTSS_COLOROP, &tss1_color);
        dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &tss1_alpha);
        for (size_t i = 0; i < kNumSs; ++i) dev->GetSamplerState(0, kSamplerStates[i], &ss[i]);
        dev->GetViewport(&viewport);
        dev->GetScissorRect(&scissor);
        dev->GetTexture(0, &texture);
        dev->GetVertexShader(&vs);
        dev->GetPixelShader(&ps);
        dev->GetVertexDeclaration(&decl);
        dev->GetFVF(&fvf);
        dev->GetStreamSource(0, &stream, &stream_offset, &stream_stride);
        dev->GetIndices(&indices);
    }

    // Releases the references save() took.
    void restore(IDirect3DDevice9 *dev) {
        for (size_t i = 0; i < kNumRs; ++i) dev->SetRenderState(kRenderStates[i], rs[i]);
        for (size_t i = 0; i < kNumTss; ++i) dev->SetTextureStageState(0, kStage0States[i], tss0[i]);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, tss1_color);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, tss1_alpha);
        for (size_t i = 0; i < kNumSs; ++i) dev->SetSamplerState(0, kSamplerStates[i], ss[i]);
        dev->SetViewport(&viewport);
        dev->SetScissorRect(&scissor);
        dev->SetTexture(0, texture);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        if (decl)
            dev->SetVertexDeclaration(decl);
        else
            dev->SetFVF(fvf);
        dev->SetStreamSource(0, stream, stream_offset, stream_stride);
        dev->SetIndices(indices);
        for (IUnknown *u : {static_cast<IUnknown *>(texture), static_cast<IUnknown *>(vs), static_cast<IUnknown *>(ps),
                            static_cast<IUnknown *>(decl), static_cast<IUnknown *>(stream),
                            static_cast<IUnknown *>(indices)})
            if (u) u->Release();
    }
};


// --- Drone model (chase and line-of-sight views) ---

struct ModelVertex {
    float x, y, z;
    DWORD color;
};
constexpr DWORD kModelFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
std::vector<ModelVertex> g_model;

struct Vec3 {
    float x, y, z;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
Vec3 vec(const float v[3]) { return {v[0], v[1], v[2]}; }

// The model is unlit: tint it like the game lights its objects this frame
// (time-of-day ambient plus some of the sun), so it isn't glowing at night.
float g_light[3] = {1, 1, 1};

DWORD shade(DWORD argb, float k) {
    auto ch = [&](int shift, float light) {
        float v = float((argb >> shift) & 0xff) * k * light;
        return DWORD(v > 255 ? 255 : v) << shift;
    };
    return (argb & 0xff000000u) | ch(16, g_light[0]) | ch(8, g_light[1]) | ch(0, g_light[2]);
}

// A box around c with half-extent axes a, b, h (h = its up), lit from above.
void box(Vec3 c, Vec3 a, Vec3 b, Vec3 h, DWORD color) {
    Vec3 corner[8];
    for (int i = 0; i < 8; ++i) corner[i] = c + a * (i & 1 ? 1.f : -1.f) + b * (i & 2 ? 1.f : -1.f) + h * (i & 4 ? 1.f : -1.f);
    static const int faces[6][4] = {{4, 5, 7, 6}, {0, 2, 3, 1}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    static const float light[6] = {1.0f, 0.45f, 0.7f, 0.8f, 0.6f, 0.75f};
    for (int f = 0; f < 6; ++f) {
        DWORD col = shade(color, light[f]);
        const int *q = faces[f];
        for (int k : {q[0], q[1], q[2], q[0], q[2], q[3]})
            g_model.push_back({corner[k].x, corner[k].y, corner[k].z, col});
    }
}

// A flat disc (the props' blur) around c in the plane of a and b.
void disc(Vec3 c, Vec3 a, Vec3 b, float r, DWORD color) {
    constexpr int kSegments = 20;
    for (int i = 0; i < kSegments; ++i) {
        float t0 = 6.2831853f * float(i) / kSegments, t1 = 6.2831853f * float(i + 1) / kSegments;
        Vec3 p0 = c + a * (std::cos(t0) * r) + b * (std::sin(t0) * r);
        Vec3 p1 = c + a * (std::cos(t1) * r) + b * (std::sin(t1) * r);
        g_model.push_back({c.x, c.y, c.z, color});
        g_model.push_back({p0.x, p0.y, p0.z, color});
        g_model.push_back({p1.x, p1.y, p1.z, color});
    }
}

// A 5" freestyle quad: carbon X frame, motors, a red camera at the front,
// spinning props as translucent discs (two blades when they stand still).
void build_model(const drone::ModelPose &p) {
    g_model.clear();
    const float *amb = sa::ambient_light_objects(), *dir = sa::directional_light();
    for (int i = 0; i < 3; ++i) {
        float l = amb[i] + 0.4f * dir[i];
        g_light[i] = l < 0.05f ? 0.05f : l > 1.0f ? 1.0f : l;
    }
    Vec3 o = vec(p.pos), R = vec(p.right), U = vec(p.up), F = vec(p.forward);
    const DWORD carbon = 0xff1c1c1f, motor = 0xff55555c, cam = 0xffa01818, battery = 0xff203868;
    box(o, R * 0.022f, F * 0.07f, U * 0.012f, carbon);                       // body
    box(o + U * 0.032f, R * 0.018f, F * 0.045f, U * 0.016f, battery);        // battery on top
    box(o + F * 0.075f + U * 0.008f, R * 0.012f, F * 0.012f, U * 0.012f, cam);  // camera
    for (int m = 0; m < 4; ++m) {
        Vec3 hub = vec(p.motor[m]);
        if (hub.x == 0 && hub.y == 0) hub = {m & 1 ? 0.08f : -0.08f, m & 2 ? -0.08f : 0.08f, 0};  // no data: plain X
        Vec3 h = o + R * hub.x + F * hub.y + U * hub.z;
        Vec3 mid = o + (R * hub.x + F * hub.y + U * hub.z) * 0.5f;
        float len = std::sqrt(hub.x * hub.x + hub.y * hub.y);
        Vec3 along = (R * hub.x + F * hub.y) * (0.5f / (len > 1e-4f ? len : 1));
        Vec3 across = (R * -hub.y + F * hub.x) * (1.0f / (len > 1e-4f ? len : 1));
        box(mid, along * len, across * 0.009f, U * 0.003f, carbon);  // arm
        box(h + U * 0.012f, R * 0.013f, F * 0.013f, U * 0.01f, motor);
        Vec3 prop = h + U * 0.025f;
        if (p.rpm[m] > 1000.0f) {
            disc(prop, R, F, 0.0635f, shade(0x50808088, 1.0f));
        } else {  // two blades
            Vec3 a = (R * 0.7071f + F * 0.7071f) * (m & 1 ? 1.f : -1.f), b = R * -0.7071f + F * 0.7071f;
            box(prop, a * 0.0635f, b * 0.006f, U * 0.0015f, 0xff404046);
        }
    }
}

}  // namespace

void drone_model(IDirect3DDevice9 *dev) {
    drone::ModelPose pose;
    if (!fpvapi::loaded() || !drone::model_pose(&pose)) return;
    build_model(pose);

    SavedState saved;
    saved.save(dev);
    D3DMATRIX world, view, proj, identity = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    dev->GetTransform(D3DTS_WORLD, &world);
    dev->GetTransform(D3DTS_VIEW, &view);
    dev->GetTransform(D3DTS_PROJECTION, &proj);
    dev->SetTransform(D3DTS_WORLD, &identity);  // vertices are in world space; the game's view and projection stay
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(kModelFvf);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    // RGB only: the game's post effects read the frame's alpha.
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    HRESULT hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(g_model.size() / 3), g_model.data(), sizeof(ModelVertex));
    dev->SetTransform(D3DTS_WORLD, &world);
    saved.restore(dev);
    static int traced = 0;
    if (traced++ < 2)
        fpvlog::write("render: drone model %u tris (hr %08lx), light %.2f %.2f %.2f, view row3 %.1f %.1f %.1f proj %.3f %.3f %.4f %.4f",
                      unsigned(g_model.size() / 3), static_cast<unsigned long>(hr), g_light[0], g_light[1], g_light[2], view._41, view._42, view._43,
                      proj._11, proj._22, proj._33, proj._43);
}

namespace {
}  // namespace

void frame(IDirect3DDevice9 *dev, bool in_game) {
    if (!fpvapi::loaded()) {
        IDirect3DSurface9 *backbuffer = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) return;
        D3DSURFACE_DESC desc;
        backbuffer->GetDesc(&desc);
        g_aspect = float(desc.Width) / float(desc.Height);
        save_screenshot(dev, backbuffer, desc);
        backbuffer->Release();
        return;
    }

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = g_last.QuadPart ? float(now.QuadPart - g_last.QuadPart) / float(freq.QuadPart) : 0.016f;
    g_last = now;

    IDirect3DSurface9 *backbuffer = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) return;
    D3DSURFACE_DESC desc;
    backbuffer->GetDesc(&desc);
    float w = float(desc.Width), h = float(desc.Height);

    g_aspect = w / h;

    fpv.fpv_overlay_set_display(w, h, h > 1080.0f ? h / 1080.0f : 1.0f);
    if (fpv.fpv_overlay_is_open()) feed_mouse(dev, w, h);

    static fpv_frame snapshot;
    const fpv_draw_data *osd = nullptr;
    bool no_signal = false;
    bool fpv_view = in_game && drone::osd_frame(&snapshot, &no_signal);
    bool lens = fpv_view && !no_signal && (snapshot.camera.distortion_k1 != 0 || snapshot.camera.distortion_k2 != 0);
    if (fpv_view && (drone::tuning().osd || no_signal))
        osd = fpv.fpv_osd_draw_data(&snapshot, w, h, FPV_OSD_STYLE_ANALOG);
    const fpv_draw_data *overlay = fpv.fpv_overlay_render(dt);
    if (!lens && !no_signal && (!osd || !osd->list_count) && (!overlay || !overlay->list_count)) {
        save_screenshot(dev, backbuffer, desc);
        backbuffer->Release();
        return;
    }

    // Draw straight onto the bound back buffer and restore exactly the state
    // we touch. A D3DSBT_ALL state block plus a render target / depth swap
    // here made the game's next frame flicker (stale regions, exposure jumps).
    IDirect3DSurface9 *rt = nullptr;
    dev->GetRenderTarget(0, &rt);
    bool on_backbuffer = rt == backbuffer;
    if (rt) rt->Release();
    if (!on_backbuffer) {
        backbuffer->Release();
        return;
    }

    SavedState saved;
    saved.save(dev);
    setup_state(dev, w, h);
    if (lens && !draw_lens(dev, backbuffer, desc, snapshot.camera)) {
        static int reported = 0;
        if (reported++ < 3) fpvlog::write("render: fisheye pass failed (format %d)", int(desc.Format));
    }
    if (no_signal && !draw_static(dev, w, h)) {
        static int reported = 0;
        if (reported++ < 3) fpvlog::write("render: static pass failed");
    }
    if (osd) draw(dev, *osd);
    if (overlay) draw(dev, *overlay);
    saved.restore(dev);
    save_screenshot(dev, backbuffer, desc);
    backbuffer->Release();
}

float aspect() { return g_aspect; }

void release() {
    if (g_scene) g_scene->Release();
    g_scene = nullptr;
    g_scene_w = g_scene_h = 0;
    if (g_static) g_static->Release();
    g_static = nullptr;
    for (Texture &t : g_textures) {
        if (t.tex) t.tex->Release();
        t = Texture{};
    }
}

}  // namespace render
