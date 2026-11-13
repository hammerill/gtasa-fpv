#include "video.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

#include "fpvapi.h"
#include "log.h"
#include "video_shaders.h"

namespace video {
namespace {

using fpvapi::fpv;

// --- shaders, compiled on a thread (about 0.7 s) ---

using CompileFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                    LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
HMODULE g_compiler = nullptr;
CompileFn g_compile = nullptr;
HANDLE g_thread = nullptr;
enum { kIdle, kCompiling, kReady, kFailed };
std::atomic<int> g_state{kIdle};

// signal for MODE 0, 1, 2; present; the quad's vertex shader
enum { kSignal0, kSignal1, kSignal2, kPresent, kVs, kShaders };
std::vector<uint8_t> g_code[kShaders];

bool compile(const char *src, const char *mode, const char *target, std::vector<uint8_t> &out) {
    D3D_SHADER_MACRO macros[] = {{"MODE", mode}, {nullptr, nullptr}};
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = g_compile(src, std::strlen(src), "fpv_video", macros, nullptr, "main", target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) {
        fpvlog::write("video: shader %s MODE=%s: %s", target, mode, static_cast<const char *>(errors->GetBufferPointer()));
        errors->Release();
    }
    if (FAILED(hr) || !code) return false;
    const uint8_t *p = static_cast<const uint8_t *>(code->GetBufferPointer());
    out.assign(p, p + code->GetBufferSize());
    code->Release();
    return true;
}

DWORD WINAPI compile_thread(void *) {
    DWORD start = GetTickCount();
    bool ok = compile(kSignalPs, "0", "ps_3_0", g_code[kSignal0]) && compile(kSignalPs, "1", "ps_3_0", g_code[kSignal1]) &&
              compile(kSignalPs, "2", "ps_3_0", g_code[kSignal2]) && compile(kPresentPs, "0", "ps_3_0", g_code[kPresent]) &&
              compile(kQuadVs, "0", "vs_3_0", g_code[kVs]);
    fpvlog::write("video: shaders %s in %lu ms", ok ? "compiled" : "failed", GetTickCount() - start);
    g_state = ok ? kReady : kFailed;
    return 0;
}

// --- device resources ---

constexpr UINT kLineRows = 512;  // >= FPV_VIDEO_MAX_LINES

struct Device {
    bool tried = false, ok = false;
    IDirect3DPixelShader9 *ps[4] = {};  // signal 0..2, present
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    IDirect3DTexture9 *tex[2] = {};  // the signal, ping-pong: [cur] is the latest
    int cur = 0, raster_w = 0, raster_h = 0;
    IDirect3DTexture9 *osd = nullptr;
    int osd_w = 0, osd_h = 0;
    IDirect3DTexture9 *lines = nullptr;
    uint32_t field = 0, flags = 0;
    bool have = false;  // a signal was drawn
} g;

template <class T> void drop(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

bool create(IDirect3DDevice9 *dev) {
    D3DCAPS9 caps{};
    dev->GetDeviceCaps(&caps);
    if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.MaxPixelShader30InstructionSlots < 1024) {
        fpvlog::write("video: no ps_3_0 with 1024 slots (version %08lx, %lu slots): no video feed",
                      static_cast<unsigned long>(caps.PixelShaderVersion),
                      static_cast<unsigned long>(caps.MaxPixelShader30InstructionSlots));
        return false;
    }
    for (int i = 0; i < 4; ++i)
        if (FAILED(dev->CreatePixelShader(reinterpret_cast<const DWORD *>(g_code[i].data()), &g.ps[i]))) {
            fpvlog::write("video: pixel shader %d failed", i);
            return false;
        }
    if (FAILED(dev->CreateVertexShader(reinterpret_cast<const DWORD *>(g_code[kVs].data()), &g.vs))) {
        fpvlog::write("video: vertex shader failed");
        return false;
    }
    const D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    if (FAILED(dev->CreateVertexDeclaration(elements, &g.decl))) return false;
    if (FAILED(dev->CreateTexture(2, kLineRows, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g.lines, nullptr))) return false;
    fpvlog::write("video: ready (%lu ps_3_0 slots)", static_cast<unsigned long>(caps.MaxPixelShader30InstructionSlots));
    return true;
}

bool ensure(IDirect3DDevice9 *dev) {
    if (g_state != kReady) return false;
    if (!g.tried) {
        g.tried = true;
        g.ok = create(dev);
    }
    return g.ok;
}

bool target(IDirect3DDevice9 *dev, IDirect3DTexture9 *&tex, int w, int h) {
    drop(tex);
    return SUCCEEDED(dev->CreateTexture(UINT(w), UINT(h), 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                        &tex, nullptr));
}

bool set_target(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex) {
    IDirect3DSurface9 *s = nullptr;
    if (FAILED(tex->GetSurfaceLevel(0, &s))) return false;
    HRESULT hr = dev->SetRenderTarget(0, s);  // the viewport becomes all of it
    s->Release();
    return SUCCEEDED(hr);
}

void sampler(IDirect3DDevice9 *dev, DWORD stage, IDirect3DBaseTexture9 *tex, bool linear) {
    dev->SetTexture(stage, tex);
    DWORD f = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, f);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, f);
    dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

void quad(IDirect3DDevice9 *dev, IDirect3DPixelShader9 *ps) {
    static const float v[4][4] = {{-1, 1, 0, 1}, {1, 1, 0, 1}, {-1, -1, 0, 1}, {1, -1, 0, 1}};
    dev->SetVertexDeclaration(g.decl);
    dev->SetVertexShader(g.vs);
    dev->SetPixelShader(ps);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
}

void upload_lines(const fpv_video_feed &vf) {
    D3DLOCKED_RECT lr;
    if (FAILED(g.lines->LockRect(0, &lr, nullptr, 0))) return;
    UINT rows = vf.line_count < FPV_VIDEO_MAX_LINES ? vf.line_count : FPV_VIDEO_MAX_LINES;
    for (UINT y = 0; y < rows; ++y) {
        const uint8_t *src = vf.lines + y * 8;
        uint8_t *dst = static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch;
        for (int t = 0; t < 2; ++t, src += 4, dst += 4) {  // RGBA -> BGRA
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    }
    g.lines->UnlockRect(0);
}

// What draw() changes beyond render.cpp's SavedState: the render target,
// depth, stages 1-3 and the pixel shader constants.
constexpr DWORD kStages = 4;
constexpr UINT kConstants = 22;
const D3DSAMPLERSTATETYPE kSamplerStates[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
                                              D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_SRGBTEXTURE};
constexpr size_t kNumSs = sizeof kSamplerStates / sizeof kSamplerStates[0];

struct Saved {
    IDirect3DSurface9 *rt = nullptr, *depth = nullptr;
    IDirect3DBaseTexture9 *tex[kStages] = {};
    DWORD ss[kStages][kNumSs];
    float constants[kConstants][4];

    void save(IDirect3DDevice9 *dev) {
        dev->GetRenderTarget(0, &rt);
        dev->GetDepthStencilSurface(&depth);
        for (DWORD s = 0; s < kStages; ++s) {
            dev->GetTexture(s, &tex[s]);
            for (size_t i = 0; i < kNumSs; ++i) dev->GetSamplerState(s, kSamplerStates[i], &ss[s][i]);
        }
        dev->GetPixelShaderConstantF(0, &constants[0][0], kConstants);
    }
    void restore(IDirect3DDevice9 *dev) {
        dev->SetRenderTarget(0, rt);
        dev->SetDepthStencilSurface(depth);
        for (DWORD s = 0; s < kStages; ++s) {
            dev->SetTexture(s, tex[s]);
            for (size_t i = 0; i < kNumSs; ++i) dev->SetSamplerState(s, kSamplerStates[i], ss[s][i]);
            if (tex[s]) tex[s]->Release();
        }
        dev->SetPixelShaderConstantF(0, &constants[0][0], kConstants);
        if (rt) rt->Release();
        if (depth) depth->Release();
    }
};

}  // namespace

bool start() {
    if (g_state != kIdle) return g_state != kFailed;
    g_compiler = LoadLibraryA("d3dcompiler_47.dll");
    g_compile = g_compiler ? reinterpret_cast<CompileFn>(reinterpret_cast<void *>(GetProcAddress(g_compiler, "D3DCompile")))
                           : nullptr;
    if (!g_compile) {
        fpvlog::write("video: no d3dcompiler_47.dll: no video feed");
        g_state = kFailed;
        return false;
    }
    g_state = kCompiling;
    g_thread = CreateThread(nullptr, 0, &compile_thread, nullptr, 0, nullptr);
    if (!g_thread) {
        g_state = kFailed;
        return false;
    }
    return true;
}

uint32_t flags(const fpv_frame &f) {
    // an older libfpv fills no video_feed: struct_size tells
    if (f.struct_size < offsetof(fpv_frame, video_feed) + sizeof(fpv_video_feed)) return 0;
    return f.video_feed.flags;
}

bool usable() { return g_state == kReady && (!g.tried || g.ok); }

bool draw(IDirect3DDevice9 *dev, IDirect3DTexture9 *scene, UINT scene_w, UINT scene_h, UINT fb_w, UINT fb_h,
          const fpv_frame &f, bool draw_osd, DrawUi draw_ui) {
    if (!ensure(dev) || !scene || !fb_w || !fb_h || !scene_w || !scene_h) return false;
    const fpv_video_feed &vf = f.video_feed;
    uint32_t fl = flags(f);
    bool noise = fl & FPV_VIDEO_FEED_NOISE, res = fl & FPV_VIDEO_FEED_RESOLUTION;
    int mode = !fl ? 0 : vf.system != FPV_VIDEO_DIGITAL ? 1 : 2;
    bool osd_in = draw_osd && mode == 1 && (fl & FPV_VIDEO_FEED_OSD_IN_SIGNAL);
    float active = vf.system == FPV_VIDEO_PAL ? 288.0f : 240.0f;
    float total = vf.system == FPV_VIDEO_PAL ? 313.0f : 263.0f;

    // the picture on the screen: the camera's aspect with black bars, or all of it
    float rx = 0, ry = 0, rw = float(fb_w), rh = float(fb_h);
    float pic_aspect = rw / rh;
    if (res && vf.picture_aspect > 0.1f) {
        pic_aspect = vf.picture_aspect;
        if (rw / rh > pic_aspect)
            rw = rh * pic_aspect, rx = (float(fb_w) - rw) * 0.5f;
        else
            rh = rw / pic_aspect, ry = (float(fb_h) - rh) * 0.5f;
    }
    // the signal's raster: an analog field, a digital frame, or the screen's pixels
    int w = int(rw), h = int(rh);
    if (res && mode == 1) w = int(vf.raster_width), h = int(active);
    else if (res && mode == 2) w = int(vf.raster_width), h = int(vf.raster_height);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;

    if (w != g.raster_w || h != g.raster_h || !g.tex[0] || !g.tex[1]) {
        if (!target(dev, g.tex[0], w, h) || !target(dev, g.tex[1], w, h)) {
            drop(g.tex[0]), drop(g.tex[1]);
            g.raster_w = g.raster_h = 0;
            return false;
        }
        g.raster_w = w, g.raster_h = h;
        g.have = false;
    }

    Saved saved;
    saved.save(dev);
    dev->SetDepthStencilSurface(nullptr);  // the targets may be larger than the game's depth
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);

    // a new field (frame), a change of settings, or no effects at all: draw the signal
    bool fresh = !g.have || !fl || vf.field != g.field || fl != g.flags;
    bool hold = mode == 2 && noise && vf.frozen && g.have;  // the goggles hold the last frame
    bool ok = true;
    if (fresh && !hold) {
        if (osd_in) {
            // the OSD chip's picture: the analog 4:3 OSD stretched over the raster
            int ow = res ? w : int(vf.raster_width), oh = int(active * 2);
            if (ow < 1) ow = 720;
            if ((ow != g.osd_w || oh != g.osd_h || !g.osd) && !target(dev, g.osd, ow, oh)) {
                g.osd_w = g.osd_h = 0;
                osd_in = false;
            } else {
                g.osd_w = ow, g.osd_h = oh;
            }
            if (osd_in && set_target(dev, g.osd)) {
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
                if (const fpv_draw_data *dd = fpv.fpv_osd_draw_data(&f, float(oh) * 4.0f / 3.0f, float(oh), FPV_OSD_STYLE_ANALOG))
                    draw_ui(dev, *dd, float(ow), float(oh));
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            }
        }
        if (noise && mode == 1) upload_lines(vf);
        int next = g.have ? 1 - g.cur : g.cur;
        ok = set_target(dev, g.tex[next]);
        if (ok) {
            sampler(dev, 0, scene, true);
            sampler(dev, 1, osd_in ? g.osd : nullptr, true);
            sampler(dev, 2, g.lines, false);
            sampler(dev, 3, g.tex[1 - next], true);  // the last frame (never the target itself)
            float c[kConstants][4] = {};
            c[0][0] = float(w), c[0][1] = float(h);
            c[1][0] = noise;
            c[2][0] = res;
            c[3][0] = osd_in;
            c[4][0] = pic_aspect;
            c[5][0] = float(scene_w) / float(scene_h);
            c[6][0] = f.camera.distortion_k1, c[6][1] = f.camera.distortion_k2;
            c[7][0] = f.camera.distortion_zoom > 0 ? f.camera.distortion_zoom : 1.0f;
            c[8][0] = vf.luma_cycles > 1 ? vf.luma_cycles : 250.0f;
            c[9][0] = vf.chroma_cycles > 1 ? vf.chroma_cycles : 53.0f;
            c[10][0] = vf.subcarrier_cycles > 1 ? vf.subcarrier_cycles : 188.5f;
            c[11][0] = vf.sharpening;
            c[12][0] = float(vf.field_parity & 1);
            c[13][0] = vf.roll;
            c[14][0] = vf.seed;
            c[15][0] = active;
            c[16][0] = total;
            for (int i = 0; i < 4; ++i) c[17][i] = vf.interference[i];
            c[18][0] = vf.raster_width ? float(vf.raster_width) : float(w);
            c[18][1] = vf.raster_height ? float(vf.raster_height) : float(h);
            c[19][0] = vf.block_errors;
            c[20][0] = noise ? vf.softness : 0.0f;
            c[21][0] = float(kLineRows);
            dev->SetPixelShaderConstantF(0, &c[0][0], kConstants);
            quad(dev, g.ps[mode]);
            g.cur = next;
            g.have = true;
            g.field = vf.field;
            g.flags = fl;
        }
    }

    // the goggles
    if (ok) {
        dev->SetRenderTarget(0, saved.rt);
        sampler(dev, 0, g.tex[g.cur], true);
        for (DWORD s = 1; s < kStages; ++s) dev->SetTexture(s, nullptr);
        float c[2][4] = {{rx, ry, rw, rh}, {res && mode == 1 ? active : 0.0f, float(vf.field_parity & 1), float(fb_h), 0}};
        dev->SetPixelShaderConstantF(0, &c[0][0], 2);
        quad(dev, g.ps[3]);
    }
    saved.restore(dev);
    if (!ok) return false;

    // the OSD over the picture (digital: the goggles draw it; or effects off)
    if (draw_osd && !osd_in) {
        fpv_osd_style style = fl ? fpv_osd_style(vf.osd_style) : FPV_OSD_STYLE_ANALOG;
        if (const fpv_draw_data *dd = fpv.fpv_osd_draw_data(&f, float(fb_w), float(fb_h), style))
            draw_ui(dev, *dd, float(fb_w), float(fb_h));
    }
    return true;
}

void release() {
    for (IDirect3DPixelShader9 *&p : g.ps) drop(p);
    drop(g.vs);
    drop(g.decl);
    drop(g.tex[0]);
    drop(g.tex[1]);
    drop(g.osd);
    drop(g.lines);
    g = Device{};
}

void shutdown() {
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    release();
    if (g_compiler) FreeLibrary(g_compiler);
    g_compiler = nullptr;
    g_compile = nullptr;
    g_state = kIdle;
}

}  // namespace video
