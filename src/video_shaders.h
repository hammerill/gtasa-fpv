// HLSL (ps_3_0) port of libfpv's docs/renderers/fpv_video_gl3.h: the video
// feed's signal pass and the goggles' present pass. The GLSL is the
// reference; this keeps its conventions (picture coordinates 0..1 with y
// up) and flips y only where it reads a texture (S()).
#pragma once

namespace video {

// The signal: one pixel of the feed's raster per fragment. Compiled once per
// mode (MODE 0 lens only, 1 analog, 2 digital), so each fits ps_3_0's slots.
constexpr const char *kSignalPs = R"hlsl(
sampler2D scene : register(s0);
sampler2D osd : register(s1);
sampler2D lines : register(s2);
sampler2D history : register(s3);
float2 raster : register(c0);       // pixels of this target
float noise : register(c1);
float res : register(c2);
float osdIn : register(c3);
float picAspect : register(c4);
float sceneAspect : register(c5);
float2 k : register(c6);
float zoom : register(c7);
float lumaCycles : register(c8);
float chromaCycles : register(c9);
float subcarrier : register(c10);
float sharpen : register(c11);
float parity : register(c12);
float roll : register(c13);
float seed : register(c14);
float activeLines : register(c15);
float totalLines : register(c16);
float4 interf : register(c17);
float2 digital : register(c18);
float blocks : register(c19);
float softness : register(c20);
float lineRows : register(c21);     // rows of the lines texture

static const float PI = 3.14159265;
// Dave Hoskins' hash without sine: the same on every GPU
float h13(float3 p) { p = frac(p * 0.1031); p += dot(p, p.zyx + 31.32); return frac((p.x + p.y) * p.z); }
float gn(float3 p) { return (h13(p) + h13(p + float3(17.1, 3.7, 9.3)) - 1.0) * 2.45; }
float glmod(float x, float y) { return x - y * floor(x / y); }
// a texture at picture coordinates (y up), as GL's texture()
float4 S(sampler2D s, float2 p) { return tex2Dlod(s, float4(p.x, 1.0 - p.y, 0.0, 0.0)); }
// picture (0..1, y up) to the scene texture through the lens
float2 sceneUv(float2 p) {
  float2 scale = float2(picAspect, 1.0) / (0.5 * sqrt(picAspect * picAspect + 1.0));
  float2 c = (p - 0.5) * scale; float r2 = dot(c, c);
  float2 d = c * (1.0 + k.x * r2 + k.y * r2 * r2) / zoom / scale;
  float s = picAspect / sceneAspect;
  if (s <= 1.0) d.x *= s; else d.y /= s;
  return d + 0.5;
}
float3 tap(float2 p) {
  float2 s = sceneUv(p);
  float3 c = S(scene, s).rgb;
  return (s.x < 0.0 || s.x > 1.0 || s.y < 0.0 || s.y > 1.0) ? float3(0.0, 0.0, 0.0) : c;
}
// the OSD chip's 4:3 grid; over a picture of another aspect (resolution off) it stays 4:3
float4 osdTap(float2 p) {
  if (osdIn < 0.5) return float4(0.0, 0.0, 0.0, 0.0);
  if (res < 0.5) p.x = (p.x - 0.5) * picAspect / (4.0 / 3.0) + 0.5;
  float4 o = S(osd, p);
  return (p.x < 0.0 || p.x > 1.0) ? float4(0.0, 0.0, 0.0, 0.0) : o;
}
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float3 toYiq(float3 c) {
  return float3(luma(c), dot(c, float3(0.596, -0.274, -0.322)), dot(c, float3(0.211, -0.523, 0.312)));
}
float3 toRgb(float3 y) {
  return float3(dot(y, float3(1.0, 0.956, 0.621)), dot(y, float3(1.0, -0.272, -0.647)), dot(y, float3(1.0, -1.106, 1.703)));
}

#if MODE == 1
float3 analog(float2 uv) {
  float2 px = floor(uv * raster);
  // the field line this pixel shows (from the top) and, when the picture
  // rolls, the transmitted line it carries
  float lineF = res > 0.5 ? raster.y - 1.0 - px.y : (1.0 - uv.y) * activeLines;
  float rolled = glmod(lineF + roll * totalLines, totalLines);
  float ln = floor(rolled);
  float4 t0 = float4(0.0, 0.0, 1.0, 1.0), t1 = float4(0.5, 0.5, 0.5, 0.5);
  if (noise > 0.5) {
    float v = (ln + 0.5) / lineRows;
    t0 = tex2Dlod(lines, float4(0.25, v, 0.0, 0.0));
    t1 = tex2Dlod(lines, float4(0.75, v, 0.0, 0.0));
  }
  float shift = noise > 0.5 ? (floor(t1.r * 255.0 + 0.5) * 256.0 + floor(t1.g * 255.0 + 0.5)) / 65535.0 * 0.5 - 0.25 : 0.0;
  bool blank = rolled >= activeLines;  // the vertical blanking: a black bar
  // where the line sits in the camera's picture: interlaced, a field carries every other line
  float yTop = res > 0.5 ? (2.0 * ln + parity + 0.5) / (2.0 * activeLines) : rolled / activeLines;
  float x = (res > 0.5 ? (px.x + 0.5) / raster.x : uv.x) + shift;
  float2 p = float2(x, 1.0 - yTop);
  float3 yiq;
  [branch] if (res > 0.5) {
    // the camera and the link: luma limited to lumaCycles, colour to chromaCycles,
    // colour a little late; a quarter line up and down against vertical aliasing
    float sl = 0.35 / lumaCycles, sc = 0.35 / chromaCycles, dv = 0.25 / (2.0 * activeLines);
    float Y = 0.0, Yring = 0.0, wsum = 0.0; float3 wide = float3(0.0, 0.0, 0.0); float wcs = 0.0;
    [loop] for (int i = -3; i <= 3; i++) {
      float fi = float(i); float w = exp(-0.5 * (fi * 0.75) * (fi * 0.75));
      float2 q = p + float2(fi * 0.75 * sl, frac(abs(fi) * 0.5) > 0.25 ? dv : -dv);
      float3 c = tap(q); float4 od = osdTap(q); c = lerp(c, od.rgb, od.a);
      float y = luma(c);
      Y += w * y; wsum += w; if (abs(fi) >= 2.0) Yring += y * 0.25;
      float2 qw = p + float2(fi * 0.75 * sc + 0.3 * sc, 0.0);
      float3 cw = tap(qw); float4 ow = osdTap(qw);
      wide += w * lerp(cw, ow.rgb, ow.a); wcs += w;
    }
    Y /= wsum; wide /= wcs;
    Y += sharpen * 1.6 * (Y - Yring);  // the camera's edge enhancement: the halos
    yiq = float3(Y, toYiq(wide).yz);
    // composite: fine detail near the subcarrier decodes as colour (rainbows)
    float hp = 0.5 / subcarrier;
    float yc = luma(tap(p));
    float yhf = yc - 0.5 * (luma(tap(p + float2(hp, 0.0))) + luma(tap(p - float2(hp, 0.0))));
    float ph = 2.0 * PI * (x * subcarrier + 0.5 * ln + 0.5 * parity);
    yiq.yz += 0.45 * yhf * float2(cos(ph), sin(ph));
  } else {
    float3 c = tap(p); float4 od = osdTap(p); yiq = toYiq(lerp(c, od.rgb, od.a));
  }
  // the black porch where a torn line starts, the blanking bar of a rolling picture:
  // black while there is a signal, the receiver's snow when there is none
  if (x < 0.0 || x > 1.0) yiq = float3(0.0, 0.0, 0.0);
  [branch] if (noise > 0.5) {
    float grain = t0.r * 0.5, density = t0.g * t0.g * t0.g, chroma = t0.b, pic = t0.a;
    float hue = (t1.a - 0.5) * 2.0 * PI;
    float3 sd = float3(0.0, ln, seed);
    // grain: the link's bandwidth stretches it along the line
    float gc = x * lumaCycles * 2.0; float gi = floor(gc), gf = gc - gi;
    float g = lerp(gn(sd + float3(gi, 0.0, 0.0)), gn(sd + float3(gi + 1.0, 0.0, 0.0)), gf);
    float cc = x * 90.0; float ci = floor(cc), cf = cc - ci;
    float2 gcol = lerp(float2(gn(sd + float3(ci, 51.0, 0.0)), gn(sd + float3(ci, 77.0, 0.0))),
                       float2(gn(sd + float3(ci + 1.0, 51.0, 0.0)), gn(sd + float3(ci + 1.0, 77.0, 0.0))), cf);
    // no picture: the receiver shows its own noise, coarse grey streaks
    float sc2 = x * 260.0; float si = floor(sc2), sf = sc2 - si;
    float snow = 0.42 + 0.30 * lerp(gn(sd + float3(si, 9.0, 1.0)), gn(sd + float3(si + 1.0, 9.0, 1.0)), sf) + 0.12 * g;
    if (blank) { yiq = float3(0.01, 0.0, 0.0); chroma = 0.0; }
    yiq.x = lerp(snow, yiq.x, pic);
    yiq.yz *= pic;
    // colour: the burst's phase (hue) and the colour killer
    float cs = cos(hue), sn = sin(hue);
    yiq.yz = float2(cs * yiq.y - sn * yiq.z, sn * yiq.y + cs * yiq.z) * chroma;
    yiq.x += grain * g;
    // FM noise is strongest high up the spectrum, where the colour lives: colour
    // speckle while the burst holds; the colour killer leaves grey snow
    yiq.yz += grain * 1.6 * gcol * chroma;
    // FM clicks: short comets, white on dark, black on bright, with a colour fringe
    [branch] if (density > 0.0) {
      float kc = x * 360.0; float k0 = floor(kc);
      [loop] for (int j = 0; j < 6; j++) {
        float cell = k0 - float(j);
        float3 q = sd + float3(cell, 0.0, 33.0);
        [branch] if (h13(q) < density) {
          float dist = kc - cell - h13(q + 1.3) * 0.8;
          float len = 0.6 + 3.5 * h13(q + 2.9);
          float v = dist < 0.0 ? 0.0 : exp(-dist / len) * (1.0 - exp(-dist * 6.0));
          float pol = h13(q + 4.7) < 0.8 ? (yiq.x < 0.45 ? 1.0 : -1.0) : (h13(q + 6.1) < 0.5 ? 1.0 : -1.0);
          yiq.x += pol * v * 1.4;
          yiq.yz += (float2(h13(q + 7.7), h13(q + 8.3)) - 0.5) * v * 0.9 * chroma;
        }
      }
    }
    // motor and ESC noise on the power wiring, and the line's own offset
    yiq.x += interf.x * sin(2.0 * PI * (x * interf.y + ln * interf.z + interf.w));
    yiq.x += t1.b - 0.5;
  }
  return saturate(toRgb(yiq));
}
#endif

#if MODE == 2
float3 boxTap(float2 p, float2 size) {
  float2 o = 0.25 * size;
  return 0.25 * (tap(p + float2(-o.x, -o.y)) + tap(p + float2(o.x, -o.y)) + tap(p + float2(-o.x, o.y)) + tap(p + float2(o.x, o.y)));
}
float3 digitalPix(float2 uv) {
  float2 p = uv;
  float3 c = res > 0.5 ? boxTap(p, 1.0 / raster) : tap(p);
  float2 bsize = 16.0 / digital;
  float2 b = floor(p / bsize);
  [branch] if (softness > 0.0) {  // the bitrate drops: detail goes first, in blocks
    float2 bc = (b + 0.5) * bsize;
    float3 dc = boxTap(bc, bsize);
    float3 mid = boxTap(p, bsize * 0.5);
    c = lerp(c, lerp(mid, dc, 0.35 * softness), softness);
  }
  [branch] if (noise > 0.5 && blocks > 0.0) {
    float3 q = float3(b, seed);
    // a lost packet takes a run of blocks along the row, then the decoder resyncs
    float runStart = floor(b.x / 6.0);
    float run = h13(float3(runStart, b.y, seed + 3.1));
    float runLen = 1.0 + floor(h13(float3(runStart, b.y, seed + 9.7)) * 6.0);
    bool inRun = run < blocks * 0.7 && b.x - runStart * 6.0 < runLen;
    [branch] if (inRun || h13(q) < blocks * 0.3) {
      float kind = h13(q + 5.5);
      [branch] if (kind < 0.6) {  // stale: last frame's block, moved by a broken motion vector
        float2 mv = (float2(h13(q + 1.1), h13(q + 2.2)) - 0.5) * bsize * 3.0;
        c = S(history, p + mv).rgb;
      } else if (kind < 0.9) {  // only the DC: a flat block, colour off
        float3 dc = boxTap((b + 0.5) * bsize, bsize);
        float3 y = toYiq(dc);
        y.yz += (float2(h13(float3(runStart, b.y, seed + 3.3)), h13(float3(runStart, b.y, seed + 4.4))) - 0.5) * 0.18;
        float2 f = frac(p / bsize) * 2.0 - 1.0;
        y.x += 0.08 * (h13(q + 6.6) - 0.5) * (f.x + f.y);
        c = toRgb(y);
      } else {  // garbage coefficients: stripes
        float2 f = frac(p / bsize);
        float s = cos(PI * floor(h13(q + 7.7) * 8.0) * f.x) * cos(PI * floor(h13(q + 8.8) * 8.0) * f.y);
        c = saturate(boxTap((b + 0.5) * bsize, bsize) + 0.25 * s * float3(0.8, 1.0, 0.7));
      }
    }
  }
  return saturate(c);
}
#endif

float4 main(float2 vpos : VPOS) : COLOR {
  // GL's uv: pixel centres, y up
  float2 uv = float2((vpos.x + 0.5) / raster.x, 1.0 - (vpos.y + 0.5) / raster.y);
#if MODE == 1
  float3 c = analog(uv);
#elif MODE == 2
  float3 c = digitalPix(uv);
#else
  float3 c = tap(uv);
  if (osdIn > 0.5) { float4 od = osdTap(uv); c = lerp(c, od.rgb, od.a); }
#endif
  return float4(c, 1.0);
}
)hlsl";

// The goggles: the signal scaled into the picture, black around it.
constexpr const char *kPresentPs = R"hlsl(
sampler2D signal : register(s0);
float4 rect : register(c0);   // the picture: x, y (from the bottom), width, height
float4 rows : register(c1);   // field lines, parity (0 lines = progressive), framebuffer height
float4 main(float2 vpos : VPOS) : COLOR {
  float2 fc = float2(vpos.x + 0.5, rows.z - (vpos.y + 0.5));  // gl_FragCoord
  float2 p = (fc - rect.xy) / rect.zw;
  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return float4(0.0, 0.0, 0.0, 1.0);
  if (rows.x > 0.0) {
    // bob: a field's lines sit half a line lower when it carries the odd lines
    float fromTop = (1.0 - p.y) * rows.x - 0.5 * rows.y;
    p.y = 1.0 - fromTop / rows.x;
  }
  return float4(tex2Dlod(signal, float4(p.x, 1.0 - p.y, 0.0, 0.0)).rgb, 1.0);
}
)hlsl";

// A full-screen quad given in clip space: ps_3_0 needs a vs_3_0.
constexpr const char *kQuadVs = R"hlsl(
float4 main(float4 p : POSITION) : POSITION { return p; }
)hlsl";

}  // namespace video
