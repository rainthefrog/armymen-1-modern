/* ============================================================================================
 * gpu.c - scaling the game picture to the window on the graphics card (Direct3D 11 shaders)
 *
 * The CPU only converts the game-size picture (e.g. 1024x540) from palette indices to colours;
 * the GPU scales it to the window, so a 4K fullscreen costs no more CPU than a small window.
 * The shaders are compiled at start-up with Windows' d3dcompiler_47.dll (also in Wine); if that
 * fails, present.c scales on the CPU as before.
 *
 * Per frame:
 *   1. Ambient fill: where the picture does not fill the window (4:3 menu screens, videos), the
 *      space around it gets a dimmed, blurred, zoomed copy of the picture instead of black.
 *   2. The picture in its place (view.c layout): the game's original square pixels (point
 *      sampling - the user found any anti-aliasing worse than the original look); videos smooth.
 * ============================================================================================ */
#define COBJMACROS
#include "common.h"
#include <d3d11.h>
#include <d3dcommon.h>
#include <stdlib.h>
#include <string.h>

static const char SHADERS[] =
"cbuffer P : register(b0) {\n"
"  float4 mode;     /* x: 1 = smooth (videos); y: 1 = CRT filter; z, w: picture size */\n"
"  float4 amb;      /* ambient fill: uv scale (x, y), dim, blur radius in uv */\n"
"  float4 hd;       /* x: share of the HD picture left and right of the game picture; y: 1 = even-edge menu scaling (sharp_uv), z, w: its scale */\n"
"  float4 op;       /* upscaled picture: x, y, w, h in view pixels */\n"
"  float4 uvr;      /* its part of the HD texture: u0, v0, u1, v1 */\n"
"  float4 opid;     /* x: id in the id map; y, z: view size */\n"
"  float4 sub;      /* x, y: sub-pixel scroll of the battlefield in game pixels; z: 1 = on */\n"
"  float4 zm;       /* battlefield zoom (zoom.c): x, y = centre in uv, z = factor (1 = off) */\n"
"  float4 ramp[64]; /* army colour ramps: green, tan, blue, grey x 16 */\n"
"  float4 lk[4];    /* battlefield links (hdworld.c): screen rect l t r b */\n"
"  float4 lo[4];    /* their offset into the map (x, y); lo[0].z = number of links */\n"
"  float4 pm;       /* palette pass: x = 1 interface mask on, y = number of rects (mr) */\n"
"  float4 mr[3];    /* palette pass: interface rects l t r b (our boxes) */\n"
"  float4 tz[32];   /* objectives_zones: target sectors, target spots, boxes of objects on a sector border; l t r b, game px */\n"
"  float4 tc;       /* tz: [0, x) sectors, [x, y) spots, [y, z) object boxes */\n"
"  float4 tk[32];   /* per tz entry: a border object's pixel key - (atlas xy - box corner) mod 2048, slice, w 1 = known */\n"
"  float4 ta;       /* target paint: x = sector line strength (fade in / out), y = glow (0..1); z = 1 menu over the field; w = panels in tp */\n"
"  float4 td;       /* pinned objective box l t r b (game px): the battlefield under it is darkened; z <= x = none */\n"
"  float4 tp[4];    /* open pop-ups / menu panels drawn onto the screen (layout_open_panels), ta.w of them: no battlefield art */\n"
"  float4 ti;       /* building interior (roofs.c): xy = picture px of floor tile (0,0), z = strength, w = number of keys */\n"
"  float4 tik[8];   /* keys of the building's images (as tk) */\n"
"  float4 tsg;      /* the cut-away circle: xy = Sarge (picture px), z = radius (0 = no circle) */\n"
"};\n"
"Texture2D tex : register(t0);\n"
"SamplerState pt : register(s0);\n"
"SamplerState ln : register(s1);\n"
"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) {\n"
"  V o; o.uv = float2((id << 1) & 2, id & 2);\n"
"  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;\n"
"}\n"
"/* battlefield pixels (alpha 1) are read zoomed around zm.xy (zoom.c) and shifted by the sub-pixel\n"
"   scroll sub.xy (game pixels); the interface (alpha 0) stays where it is. A zoomed read that lands\n"
"   on the interface (beside the HUD panel's top, small zooms) takes the battlefield just above it */\n"
"float2 scroll_uv(float2 uv) {\n"
"  if (sub.z < 0.5) return uv;\n"
"  int2 ip = int2(floor(uv * mode.zw));\n"
"  if (tex.Load(int3(ip, 0)).a < 0.5) return uv;\n"
"  float2 u = zm.xy + (uv - zm.xy) / max(zm.z, 1) + sub.xy / mode.zw;\n"
"  if (zm.z > 1.001) {\n"
"    for (int k = 0; k < 128; k++) {\n"
"      int2 q = int2(floor(u * mode.zw));\n"
"      if (q.y <= 0 || tex.Load(int3(q, 0)).a > 0.5) break;\n"
"      u.y -= 1 / mode.w;\n"
"    }\n"
"  }\n"
"  return u;\n"
"}\n"
"/* hd.y = 1: a 640x480 menu screen shown at a size that is not a whole multiple (2026-11-18, user: \"the menu\n"
"   text needs to be cleaned up for all sizes\"). Nearest sampling made text strokes 1 or 2 screen pixels wide at\n"
"   random (ragged letters). Here every game pixel keeps its flat inside and only its EDGE is blended over one\n"
"   screen pixel, so all strokes get the same width. Whole multiples and the game itself stay nearest. */\n"
"float2 sharp_uv(float2 uv) {\n"
"  float2 t = uv * mode.zw;\n"
"  float2 sc = max(hd.zw, 1);          /* screen pixels per game pixel */\n"
"  float2 rr = 0.5 - 0.5 / sc;\n"
"  float2 cd = frac(t) - 0.5;\n"
"  return (floor(t) + (cd - clamp(cd, -rr, rr)) * sc + 0.5) / mode.zw;\n"
"}\n"
"float4 ps_scale(V i) : SV_Target {\n"
"  float2 su = sharp_uv(i.uv);\n"
"  if (mode.x > 0.5) return float4(tex.SampleLevel(ln, i.uv, 0).rgb, 1);\n"
"  if (hd.y > 0.5 && mode.y < 0.5) return float4(tex.SampleLevel(ln, su, 0).rgb, 1);\n"
"  i.uv = scroll_uv(i.uv);\n"
"  if (mode.y < 0.5) return float4(tex.SampleLevel(pt, i.uv, 0).rgb, 1);\n"
"  /* CRT: each picture row is a scanline (bright in the middle, dark at its edges), neighbouring\n"
"     pixels in the row blend, and a faint RGB mask runs across the screen */\n"
"  float2 t = i.uv * mode.zw;\n"
"  float2 uv = float2(i.uv.x, (floor(t.y) + 0.5) / mode.w);\n"
"  float3 c = tex.SampleLevel(ln, uv, 0).rgb;\n"
"  float d = frac(t.y) - 0.5;\n"
"  c *= (0.55 + 0.45 * exp(-d * d * 14)) * 1.3;\n"
"  uint m = (uint)i.pos.x % 3;\n"
"  c *= m == 0 ? float3(1, 0.86, 0.86) : m == 1 ? float3(0.86, 1, 0.86) : float3(0.86, 0.86, 1);\n"
"  return float4(saturate(c), 1);\n"
"}\n"
"/* HD menu background (hdart.c): the wide HD picture fills the quad; the game picture is its\n"
"   centre, drawn on top where its alpha says the game drew something (panel, text, cursor) */\n"
"Texture2D hires : register(t1);\n"
"float4 ps_hd(V i) : SV_Target {\n"
"  float gx = (i.uv.x - hd.x) / (1 - 2 * hd.x);\n"
"  float2 su = sharp_uv(float2(gx, i.uv.y));\n"
"  float4 g = hd.y > 0.5 ? tex.SampleLevel(ln, su, 0) : tex.SampleLevel(pt, float2(gx, i.uv.y), 0);\n"
"  if (gx < 0 || gx > 1) g = float4(0, 0, 0, 0);\n"
"  float3 h = hires.SampleLevel(ln, i.uv, 0).rgb;\n"
"  return float4(lerp(h, g.rgb, g.a), 1);\n"
"}\n"
"/* upscaled picture (hdsprites.c): its HD pixels exactly where the screen still shows it */\n"
"Texture2D<uint> idmap : register(t2);\n"
"Texture2D sprite : register(t3);\n"
"float4 ps_sprite(V i) : SV_Target {\n"
"  float2 gp = op.xy + i.uv * op.zw;\n"
"  int2 ip = int2(floor(gp));\n"
"  if (ip.x < 0 || ip.y < 0 || ip.x >= (int)opid.y || ip.y >= (int)opid.z) discard;\n"
"  uint me = idmap.Load(int3(ip, 0));\n"
"  if ((me & 127) != (uint)opid.x) discard;\n"
"  float4 s4 = sprite.SampleLevel(ln, lerp(uvr.xy, uvr.zw, i.uv), 0);\n"
"  float3 under = tex.Load(int3(ip, 0)).rgb;\n"
"  /* bit 7: this game pixel shows the ORIGINAL picture's own ink. Where the HD art is see-through there (its\n"
"     shapes differ a little from the old pixels) the old ink must not show through - old + new letters gave a\n"
"     rough mix (2026-11-18). Under it is the background: the nearest pixels that are not this picture's ink. */\n"
"  if ((me & 128) && s4.a < 0.98) {\n"
"    float3 acc = float3(0, 0, 0); float cnt = 0;\n"
"    for (int r = 1; r <= 3; r++) {\n"
"      if (cnt > 0) break;\n"
"      for (int dy = -r; dy <= r; dy++)\n"
"        for (int dx = -r; dx <= r; dx++) {\n"
"          if (max(abs(dx), abs(dy)) != r) continue;\n"
"          int2 q = ip + int2(dx, dy);\n"
"          if (q.x < 0 || q.y < 0 || q.x >= (int)opid.y || q.y >= (int)opid.z) continue;\n"
"          uint v = idmap.Load(int3(q, 0));\n"
"          if ((v & 127) == (uint)opid.x && (v & 128)) continue;\n"
"          acc += tex.Load(int3(q, 0)).rgb; cnt += 1;\n"
"        }\n"
"    }\n"
"    if (cnt > 0) under = acc / cnt;\n"
"  }\n"
"  float3 c = under * (1 - s4.a) + s4.rgb;   /* HD pictures are premultiplied (hdart_load_image_alpha): see-through parts show what is under */\n"
"  if (s4.a > 0.99 && min(c.g, c.b) - c.r > 0.25 && c.g > 0.35 && c.b > 0.35) c = under;   /* opaque art's cyan background and its fringe */\n"
"  return float4(c, 1);\n"
"}\n"
"/* upscaled battlefield (hdworld.c): per game pixel a reference into the 2x atlas */\n"
"Texture2DArray atlas : register(t4);\n"
"Texture2D<uint> wref : register(t5);\n"
"Texture2D<uint> wdepth : register(t16);   /* ps_resolve's second target: painter depth, bit 15 under-overlay */\n"
"Texture2D<uint> wpick : register(t17);    /* ps_pick: the front / back neighbour per game pixel */\n"
"/* q (seen dx, dy away from a pixel whose reference is r) belongs to ANOTHER sprite (not r's own image\n"
"   continuing): atlas texel positions differ from the pixel offset, or r is ground */\n"
"bool other_sprite(uint r, uint q, int dx, int dy) {\n"
"  if (!(r & (1u << 29))) return true;\n"
"  if (((r >> 22) & 3) != ((q >> 22) & 3)) return true;\n"
"  int mx = (r & (1u << 28)) ? -dx : dx;\n"
"  return (int)(q & 0x7ff) - (int)(r & 0x7ff) != mx || (int)((q >> 11) & 0x7ff) - (int)((r >> 11) & 0x7ff) != dy;\n"
"}\n"
"float lum(float3 c) { return dot(c, float3(0.3, 0.59, 0.11)); }\n"
"/* ref (hdworld_prepare): bit 31 valid, 30 has margin, 29 sprite, 28 mirror, 24..27 flags,\n"
"   22..23 slice, 11..21 atlas y / 2, 0..10 atlas x / 2. o = offset in 2x texels from the ref's texel. */\n"
"float4 texel(uint r, int2 o) { return atlas.Load(int4(int(r & 0x7ff) * 2 + o.x, int((r >> 11) & 0x7ff) * 2 + o.y, int((r >> 22) & 3), 0)); }\n"
"/* 1 where a game pixel is shadow: a shadow drawn over it (flag) or a sprite's own shadow pixel\n"
"   (alpha 0.5 in the atlas, set by the cut) */\n"
"float is_shadow(int2 p) {\n"
"  uint q = wref.Load(int3(p, 0));\n"
"  if ((q & 0x80000000) == 0) return 0;\n"
"  if ((q >> 24) & 1) return 1;\n"
"  float a = texel(q, int2(0, 0)).a;\n"
"  return (a > 0.25 && a < 0.75) ? 1 : 0;\n"
"}\n"
"/* the upscaled GROUND at game pixel ip (sub-texel f), taken from the nearest ground-tile reference up to\n"
"   3 game pixels away (tiles continue seamlessly, so its texel shifted to here is the ground under a sprite) */\n"
"bool ground_at(int2 ip, int2 f, out float3 g) {\n"
"  g = float3(0, 0, 0);\n"
"  for (int ring = 1; ring <= 3; ring++)\n"
"    for (int dy = -ring; dy <= ring; dy++)\n"
"      for (int dx = -ring; dx <= ring; dx++) {\n"
"        if (max(abs(dx), abs(dy)) != ring) continue;\n"
"        uint q = wref.Load(int3(ip + int2(dx, dy), 0));\n"
"        if ((q & 0x80000000) == 0 || (q & (1u << 29))) continue;\n"
"        float4 t = texel(q, int2(-dx, -dy) * 2 + f);\n"
"        if (t.a >= 0.75) { g = t.rgb; return true; }\n"
"      }\n"
"  return false;\n"
"}\n"
"/* shadow coverage of game pixel p: 3x3 tent over is_shadow (no local arrays: wined3d crashed on them) */\n"
"float shadow_cov(int2 p) {\n"
"  float s = 0;\n"
"  for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)\n"
"    s += (dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1) * is_shadow(p + int2(dx, dy));\n"
"  return s / 16;\n"
"}\n"
"Texture2D shadowmap : register(t12);\n"
"float ps_shadow(V i) : SV_Target { return shadow_cov(int2(i.pos.xy)); }\n"
"float edge_cov(float a) { return a > 0.999 ? 1 : saturate((a * 255 - 192) / 62); }   /* 192..254 -> 0..1 */\n"
"int2 sub_texel(uint r, int2 f) { return (r & (1u << 28)) ? int2(1 - f.x, f.y) : f; }   /* mirrored: flip */\n"
"/* the army colour of a sprite texel (flags of its reference q): the upscale's brightness on the army's ramp */\n"
"float3 recol(float3 c, uint q) {\n"
"  uint fl = (q >> 24) & 15, army = fl >> 2;\n"
"  if (!(fl & 2)) return c;\n"
"  /* the game maps green shade i to the army's shade i (palette 208+i -> 224+i ...), and tan's shades are much\n"
"     lighter than green's: find the texel's place on the GREEN ramp (the upscale is the green army) and take\n"
"     the army's colour there; the texel's own brightness relative to that green shade keeps the detail.\n"
"     (Matching the army ramp by brightness made tan soldiers as dark as green ones - user 2026-11-19) */\n"
"  float L = lum(c), bd = 9; int k = 0;\n"
"  for (int j = 0; j < 16; j++) { float d = abs(lum(ramp[j].rgb) - L); if (d < bd) { bd = d; k = j; } }\n"
"  float3 rc = ramp[army * 16 + k].rgb;\n"
"  /* brighter than the green shade: half of the excess only (the top tan shades are light already -\n"
"     highlights went white) */\n"
"  float s = L / max(lum(ramp[k].rgb), 0.02);\n"
"  if (s > 1) s = 1 + (s - 1) * 0.5;\n"
"  return saturate(rc * clamp(s, 0.6, 1.2));\n"
"}\n"
"/* objectives.c targets, PAINTED ON THE GROUND (user 2026-11-19: \"doesn't fit the style and I'd like it to fit\n"
"   inside the world, so no over top of buildings\"): a dashed yellow field-marking line just inside a target\n"
"   SECTOR's border, a dashed ring + faint fill on a target SPOT (pad). ps_world applies it to ground pixels only,\n"
"   before the shadows - objects stand on / hide the paint, shadows fall over it. Returns the paint coverage. */\n"
"float hash21(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }\n"
"/* tz[]: [0, tc.x) sectors, [tc.x, tc.y) spots, [tc.y, tc.z) boxes of the objects that sit ON a sector border\n"
"   (objectives.c straddlers). 2026-11-19, user: \"make it draw around the building and objects\" - the straight\n"
"   line is hidden where an object stands on it (ground pixels only); around such an object the paint follows\n"
"   the object's OUTLINE on its part INSIDE the sector (a ground pixel within 2 px of a sprite pixel, inside the\n"
"   object's box and inside a sector), so the line detours around it. \"the specific position needs to draw on\n"
"   top of the door threshold\": spots are painted on every pixel (ground false too), sectors on ground only. */\n"
"/* a pixel of THAT object within 2 px. Every screen pixel of one drawn image has the same (atlas xy - screen xy)\n"
"   (wref bits 0..10 x, 11..21 y, 22..23 slice; tk = that value for the object, from hdworld_cell_atlas) - so\n"
"   crates, barrels and rocks that merely lie inside a border object's box are not outlined. (Tried first: any\n"
"   sprite in the box - everything in the yard was outlined; then the painter depth - a building's sort row is\n"
"   not one stable value.) Mirrored images are left out (tk.w = 0). */\n"
"bool near_object(int2 ip, float4 key) {\n"
"  static const int2 o[8] = { int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };\n"
"  if (key.w < 0.5) return false;\n"
"  int kx = (int)key.x, ky = (int)key.y; uint ks = (uint)key.z;\n"
"  for (int k = 0; k < 8; k++) {\n"
"    int2 p = ip + o[k];\n"
"    uint q = wref.Load(int3(p, 0));\n"
"    if ((q & 0xb0000000) != 0xa0000000 || ((q >> 22) & 3) != ks) continue;   /* a sprite pixel, not mirrored, that slice */\n"
"    if ((((int)(q & 0x7ff) - p.x) & 0x7ff) == kx && (((int)((q >> 11) & 0x7ff) - p.y) & 0x7ff) == ky) return true;\n"
"  }\n"
"  return false;\n"
"}\n"
"/* 2026-11-20 (user: \"the area outline should just go under the fences and rocks, etc. only go around buildings.\n"
"   and fade in and glow and then fade out\"): returns x = paint for the GROUND (sector line, building inside\n"
"   edges) - ps_world puts it on the ground colour BEFORE objects are laid over it, so every object (also its\n"
"   soft edge and wire gaps) stands on the line; y = paint on everything (target spots: door thresholds).\n"
"   ta.x = sector strength (objectives.c: fades in, holds, fades out), ta.y = glow: a soft wider halo. */\n"
"float2 zone_paint(float2 gp, int2 ip) {\n"
"  float a = 0, s = 0;\n"
"  float rough = (hash21(floor(gp * 2)) - 0.5) * 0.5;   /* brush edge */\n"
"  int nsec = (int)tc.x, nspot = (int)tc.y, ntot = (int)tc.z;\n"
"  float fade = ta.x, glow = ta.y, sf = max(0.6, fade);\n"
"  bool inside = false;\n"
"  for (int k = 0; k < 32; k++) {\n"
"    if (k >= nspot) break;\n"
"    if (k < nsec) {\n"
"      float2 d = min(gp - tz[k].xy, tz[k].zw - gp);\n"
"      float e = min(d.x, d.y);\n"
"      if (e >= 0) inside = true;\n"
"      if (fade <= 0) continue;\n"
"      float t = d.x < d.y ? gp.y : gp.x;                 /* along the nearest edge */\n"
"      float band = 1 - smoothstep(1.0, 1.6, abs(e - 2.0) + rough);\n"
"      float dash = step(frac(t / 10), 0.62);\n"
"      float halo = (1 - smoothstep(0.0, 6.0, abs(e - 2.0))) * 0.4 * glow;\n"
"      a = max(a, fade * max(0.78 * band * dash, halo));\n"
"    } else {\n"
"      float2 c = (tz[k].xy + tz[k].zw) * 0.5; float r = (tz[k].z - tz[k].x) * 0.5;\n"
"      float2 v = gp - c; float d = length(v);\n"
"      float t = (atan2(v.y, v.x) + 3.14159) * r;\n"
"      float band = 1 - smoothstep(1.0, 1.6, abs(d - r + 1.5) + rough);\n"
"      float halo = (1 - smoothstep(0.0, 6.0, abs(d - r + 1.5))) * 0.4 * glow;\n"
"      s = max(s, sf * max(max(0.78 * band * step(frac(t / 8), 0.62), d < r - 1.5 ? 0.16 : 0), halo));\n"
"    }\n"
"  }\n"
"  /* the INSIDE edge of BUILDINGS on the border (\"have it draw along the inside edge of the buildings that\n"
"     intersect the area\"; objectives.c straddlers: buildings only since 2026-11-20) */\n"
"  if (inside && fade > 0 && ntot > nspot) {\n"
"    bool edge = false;\n"
"    for (int k2 = nspot; k2 < 32; k2++) {\n"
"      if (k2 >= ntot || edge) break;\n"
"      if (gp.x >= tz[k2].x - 3 && gp.y >= tz[k2].y - 3 && gp.x < tz[k2].z + 3 && gp.y < tz[k2].w + 3)\n"
"        edge = near_object(ip, tk[k2]);\n"
"    }\n"
"    if (edge) a = max(a, fade * max(0.78 * step(frac((gp.x + gp.y) / 10), 0.62), 0.3 * glow));\n"
"  }\n"
"  return float2(a, s);\n"
"}\n"
"float3 zpaint(float3 c, float a) { return lerp(c, float3(0.95, 0.8, 0.22) * (0.75 + 0.35 * lum(c)), a); }\n"
"/* building interior (roofs.c roofs_interior_note, 2026-11-20): is picture point gp on a walkable floor tile of\n"
"   the building Sarge is in; is wref q at ip a pixel of that building's images (exact atlas key, unmirrored) */\n"
"/* the floor (r) and door (g) cells, 8 px, 64 x 64 window at ti.xy: a small texture (gpu.c g_imask) read with\n"
"   the LINEAR sampler - bilinear between cell centres smooths the 8 px steps into diagonals (user: \"can the\n"
"   edges be smoothed out?\"); 0.5 = the edge. (Bit lookups in the constant buffer made ps_world too big for\n"
"   the software rasteriser: llvmpipe ran out of memory compiling it, 2026-11-20.) */\n"
"Texture2D imask : register(t19);\n"
"float2 cells_soft(float2 gp) { return imask.SampleLevel(ln, (gp - ti.xy) / 512, 0).rg; }\n"
"bool in_floor(float2 gp) { return cells_soft(gp).r >= 0.5; }\n"
"bool of_building(int2 ip, uint q) {\n"
"  if ((q & 0xb0000000) != 0xa0000000) return false;\n"
"  for (int k = 0; k < 8; k++) {\n"
"    if (k >= (int)ti.w) break;\n"
"    if (((q >> 22) & 3) == (uint)tik[k].z && (((int)(q & 0x7ff) - ip.x) & 0x7ff) == (int)tik[k].x\n"
"        && (((int)((q >> 11) & 0x7ff) - ip.y) & 0x7ff) == (int)tik[k].y) return true;\n"
"  }\n"
"  return false;\n"
"}\n"
"/* the roof over the floor, opened: dark interior; s = strength */\n"
"float3 interior(float3 c, float s) { return lerp(c, c * 0.3 + float3(0.11, 0.09, 0.065), s); }   /* a shaded room, the roof's grain faintly kept */\n"
"/* within 1.5 px of the floor's edge (inside it): the outline of the walkable area */\n"
"float floor_edge(float fs) { return 1 - smoothstep(0.025, 0.07, abs(fs - 0.5)); }   /* thin: the blurred mask changes slowly */\n"
"/* The upscaled battlefield, per window pixel. Each upscaled image is drawn WHOLE (its soft edge and the\n"
"   margin past the original outline), in the game's painter order: where other sprites' images reach this\n"
"   pixel (within 4 game px, ps_near bit 30), the frontmost opaque one (depth = sprite bottom, ps_resolve)\n"
"   is drawn over this pixel's own art, and the nearest one BEHIND is the backdrop under this sprite's soft\n"
"   or missing edge (before: margins only over plain ground, so overlapping rocks kept the old stair-stepped\n"
"   outline and showed the low-res original where the new art is smaller). */\n"
"float4 ps_world(V i) : SV_Target {\n"
"  float2 gp = scroll_uv(i.uv) * mode.zw;\n"
"  int2 ip = int2(floor(gp));\n"
"  uint r = wref.Load(int3(ip, 0));\n"
"  if ((r & 0x80000000) == 0) discard;\n"
"  int2 f = int2(frac(gp) * 2);\n"
"  bool spr = (r & (1u << 29)) != 0;\n"
"  float4 c = texel(r, sub_texel(r, f));\n"
"  /* the front / back images picked for this game pixel (ps_pick), checked at this sub-texel;\n"
"     pick bit 31 = ground-level overlay (route line) here */\n"
"  float4 fc = 0, bc = 0; uint fq = 0, bq = 0; bool hasf = false, hasb = false;\n"
"  uint pk = (r & (1u << 30)) ? wpick.Load(int3(ip, 0)) : 0;\n"
"  bool und = (pk & 0x80000000) != 0, gh = (pk & 0x40000000) != 0;\n"
"  if (pk & 0x100) {\n"
"    int2 d = int2(pk & 15, (pk >> 4) & 15) - 4;\n"
"    uint q = wref.Load(int3(ip + d, 0));\n"
"    int2 o = -d * 2; if (q & (1u << 28)) o.x = -o.x;\n"
"    float4 g = texel(q, o + sub_texel(q, f));\n"
"    if (g.a >= 0.75) { fc = g; fq = q; hasf = true; }\n"
"  }\n"
"  if (pk & 0x20000) {\n"
"    int2 d = int2((pk >> 9) & 15, (pk >> 13) & 15) - 4;\n"
"    uint q = wref.Load(int3(ip + d, 0));\n"
"    int2 o = -d * 2; if (q & (1u << 28)) o.x = -o.x;\n"
"    float4 g = texel(q, o + sub_texel(q, f));\n"
"    if (g.a >= 0.75) { bc = g; bq = q; hasb = true; }\n"
"  }\n"
"  /* the backdrop of this pixel's own sprite: the sprite behind, else the upscaled ground, else the game\n"
"     pixel - only looked up where needed (transparent, shadow-dither or soft-edge pixels of a sprite) */\n"
"  float3 back = 0;\n"
"  float ec0 = c.a >= 0.75 ? edge_cov(c.a) : 0;\n"
"  float2 zp = tc.y > 0.5 ? zone_paint(gp, ip) : float2(0, 0);   /* target paint: x ground (under objects), y spots */\n"
"  if (spr && ec0 < 1) {\n"
"    float3 g;\n"
"    if (hasb) back = recol(bc.rgb, bq);\n"
"    else if (ground_at(ip, f, g)) back = zpaint(g, zp.x);\n"
"    else back = zpaint(tex.Load(int3(ip, 0)).rgb, zp.x);\n"
"  }\n"
"  float3 col;\n"
"  if (!spr) col = zpaint(c.rgb, zp.x);                           /* ground tile (target line painted on it) */\n"
"  else if (c.a < 0.25) col = back;                                /* outside the new art: what is behind */\n"
"  else if (c.a < 0.75) {                                          /* the sprite's own dithered shadow pixel */\n"
"    col = back;\n"
"    if (!hasb) {\n"
"      bool got = false;\n"
"      for (int n = 0; n < 4 && !got; n++) {\n"
"        int2 nb = n == 0 ? int2(1, 0) : n == 1 ? int2(-1, 0) : n == 2 ? int2(0, 1) : int2(0, -1);\n"
"        uint q = wref.Load(int3(ip + nb, 0));\n"
"        if (q & 0x80000000) { float4 g = texel(q, sub_texel(q, f)); if (g.a >= 0.75) { col = recol(g.rgb, q); got = true; } }\n"
"      }\n"
"    }\n"
"  } else {\n"
"    col = recol(c.rgb, r);\n"
"    if (ec0 < 1) col = lerp(back, col, ec0);                      /* soft edge (alpha 192..254) */\n"
"  }\n"
"  /* a figure under tall cover (roofs.c, override mask 3; user 2026-11-19: \"sarge just renders on top of the\n"
"     building ... more ghostly\"): his art at 50 % over the cover. The cover is the image in FRONT (the roof's\n"
"     upscaled art), else the game picture. 2026-11-20 (user: \"not drawing consistently transparent\"): before,\n"
"     the front image was laid over the figure first and the blend mixed roof with roof - Sarge vanished\n"
"     wherever a roof pixel lay within 4 px, i.e. almost everywhere on a thin figure. */\n"
"  float2 fd = ti.z > 0 ? cells_soft(gp) : float2(0, 0);   /* the open floor (r) / doors (g) of the building Sarge is in or near */\n"
"  float fs = fd.r;\n"
"  if (tsg.z > 0) fs *= 1 - smoothstep(tsg.z - 14, tsg.z, length(gp - tsg.xy));   /* the circle round Sarge, soft edge */\n"
"  bool inf = fs >= 0.5;\n"
"  if (gh && spr) {\n"
"    float3 cover = hasf ? recol(fc.rgb, fq) : tex.SampleLevel(ln, gp / mode.zw, 0).rgb;\n"
"    float fa = c.a >= 0.75 ? edge_cov(c.a) : 0, ga = 0.5;\n"
"    if (inf) { cover = interior(cover, ti.z); ga = 0.5 + 0.45 * ti.z; }   /* inside: the figure almost solid */\n"
"    col = lerp(cover, recol(c.rgb, r), ga * fa);\n"
"  } else {\n"
"    if (hasf) col = lerp(col, recol(fc.rgb, fq), edge_cov(fc.a));\n"
"    if (ti.z > 0 && spr && of_building(ip, r)) {\n"
"      /* the roof over the floor opened (soft edge); walls keep their art; outline of the walkable area */\n"
"      col = interior(col, ti.z * smoothstep(0.42, 0.52, fs));\n"
"      col = lerp(col, float3(0.92, 0.86, 0.62), 0.7 * ti.z * floor_edge(fs));\n"
"      /* doors (also the back ones) of the building Sarge is near or in: a soft warm glow on its pixels there */\n"
"      /* (door glow removed 2026-11-20, user: \"why is there yellow in it?\" - entrances show as opened thresholds) */\n"
"    }\n"
"  }\n"
"  /* shadows: dithered in the game; coverage per game pixel (ps_shadow), sampled bilinearly - a smooth\n"
"     gradient, 30 % darker where solid (the game's shade) */\n"
"  col = zpaint(col, zp.y);                                        /* target spots: on everything (door thresholds) */\n"
"  float cov = shadowmap.SampleLevel(ln, gp / mode.zw, 0).r;\n"
"  col *= 1 - 0.3 * cov;\n"
"  /* ground-level overlay (route line): above the ground, behind every object, faint where one covers it */\n"
"  if (und) { float3 game = tex.Load(int3(ip, 0)).rgb; col = (!spr && !hasf) ? game : lerp(col, game, 0.3); }\n"
"  /* the pinned objective box (objectives.c, 2026-11-20, user: \"the mission text doesn't look like it was cleanly\n"
"     cut from the background\"): the UPSCALED battlefield darkened under it; its text is interface (game pixels) */\n"
"  if (td.z > td.x && gp.x >= td.x && gp.y >= td.y && gp.x < td.z && gp.y < td.w) col *= 0.3;\n"
"  return float4(col, 1);\n"
"}\n"
"Texture2D ovl : register(t18);\n"
"float4 ps_overlay(V i) : SV_Target { return ovl.SampleLevel(pt, i.uv, 0); }   /* menutext.c: premultiplied label layer */\n"
"float4 ps_ambient(V i) : SV_Target {\n"
"  float2 uv = 0.5 + (i.uv - 0.5) * amb.xy;\n"
"  float3 c = 0;\n"
"  for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++)\n"
"    c += tex.SampleLevel(ln, uv + float2(x, y) * amb.w, 0).rgb;\n"
"  return float4(c / 25 * amb.z, 1);\n"
"}\n"
"/* Battlefield references resolved on the GPU (hdworld.c hdworld_gpu_inputs), at game size:\n"
"   ps_resolve: raw Ref (e | sx << 16, sy | expect << 16 | flags << 24) + the shown byte + the entry table\n"
"   -> the wref format ps_world reads; ps_near: ground within 4 px of a sprite with a margin gets bit 30. */\n"
"Texture2D<uint2> refs : register(t6);\n"
"Texture2D<uint> fbytes : register(t7);\n"
"Texture2D<uint2> ents : register(t8);\n"
"Texture2D<uint> wref0 : register(t9);\n"
"/* Gameplay picture coloured on the GPU (present.c uploads the 8-bit pixels, not 32-bit colours):\n"
"   colour = palette[index]; alpha 0 = interface (mask or one of our boxes) when the battlefield is\n"
"   read moved (sub-pixel scroll, zoom), else 1. Rendered into the picture texture the others read. */\n"
"Texture2D<uint> pidx : register(t13);\n"
"Texture2D<uint> pmask : register(t14);\n"
"Texture2D ppal : register(t15);\n"
"bool in_rect(float2 p, float4 r) { return p.x >= r.x && p.y >= r.y && p.x < r.z && p.y < r.w; }\n"
"float4 ps_palette(V i) : SV_Target {\n"
"  int2 ip = int2(i.pos.xy);\n"
"  float4 c = ppal.Load(int3(pidx.Load(int3(ip, 0)), 0, 0));\n"
"  float a = 1;\n"
"  if (pm.x > 0.5) {\n"
"    float2 p = float2(ip);\n"
"    if (pmask.Load(int3(ip, 0)) != 0) a = 0;\n"
"    if (pm.y > 0.5 && in_rect(p, mr[0])) a = 0;\n"
"    if (pm.y > 1.5 && in_rect(p, mr[1])) a = 0;\n"
"    if (pm.y > 2.5 && in_rect(p, mr[2])) a = 0;\n"
"  }\n"
"  return float4(c.rgb, a);\n"
"}\n"
"Texture2D<uint2> maprefs : register(t10);\n"
"Texture2D<uint> over : register(t11);\n"
"/* the reference of screen pixel ip: the screen's own where something was drawn over the battlefield\n"
"   (override mask 1), else the map's through the link that covers it, else none. Mask 2 = a ground-level\n"
"   overlay (route line, hdworld_under_run): the map's reference, its byte is not checked (depth bit 15). */\n"
"bool in_link(float2 p, float4 r) { return p.x >= r.x && p.y >= r.y && p.x < r.z && p.y < r.w; }\n"
"uint2 ref_at(int2 ip) {\n"
"  uint ov = over.Load(int3(ip, 0));\n"
"  if (ov == 1 || ov == 3) return refs.Load(int3(ip, 0));   /* 3 = a ghost figure (roofs.c): the screen's own reference */\n"
"  float2 p = float2(ip);\n"
"  float n = lo[0].z;\n"
"  if (n > 0.5 && in_link(p, lk[0])) return maprefs.Load(int3(ip + int2(lo[0].xy), 0));\n"
"  if (n > 1.5 && in_link(p, lk[1])) return maprefs.Load(int3(ip + int2(lo[1].xy), 0));\n"
"  if (n > 2.5 && in_link(p, lk[2])) return maprefs.Load(int3(ip + int2(lo[2].xy), 0));\n"
"  if (n > 3.5 && in_link(p, lk[3])) return maprefs.Load(int3(ip + int2(lo[3].xy), 0));\n"
"  return uint2(0, 0);\n"
"}\n"
"/* depth (second target, R16_UINT): a sprite pixel's painter order = its sprite's bottom row on the screen + 1\n"
"   (the game draws objects bottom-sorted), ground 0; bit 15 = ground-level overlay (route line) */\n"
"struct RO { uint w : SV_Target0; uint d : SV_Target1; };\n"
"RO ps_resolve(V i) {\n"
"  RO o; o.w = 0; o.d = 0;\n"
"  int2 ip = int2(i.pos.xy);\n"
"  uint ovv = over.Load(int3(ip, 0));\n"
"  /* ta.z: a menu or pop-up is drawn straight onto the screen (hud.c, primary surface: the override mask never sees\n"
"     it). Ghost (3) and route (2) pixels skip the byte check - they showed THROUGH the pause menu (2026-11-20). */\n"
"  bool mo = ta.z > 0.5;\n"
"  bool und = !mo && ovv == 2, gh = !mo && ovv == 3;\n"
"  for (int pi = 0; pi < 4; pi++) {   /* inside an open panel: the panel, never battlefield art (colour matches gave specks) */\n"
"    if (pi >= (int)ta.w) break;\n"
"    if (ip.x >= tp[pi].x && ip.y >= tp[pi].y && ip.x < tp[pi].z && ip.y < tp[pi].w) return o;\n"
"  }\n"
"  if (und) o.d = 0x8000;\n"
"  if (gh) o.d = 0x4000;   /* ghost figure seen through tall cover: ps_world blends it over the game picture */\n"
"  uint2 q = ref_at(ip);\n"
"  uint e = q.x & 0xffff;\n"
"  if (e == 0) return o;\n"
"  if (!und && !gh && fbytes.Load(int3(ip, 0)) != ((q.y >> 16) & 255)) return o;   /* ghost: the byte shown is the cover's again */\n"
"  e -= 1;\n"
"  uint2 en = ents.Load(int3(e & 255, e >> 8, 0));\n"
"  if ((en.x & 0x80000000) == 0) return o;\n"
"  uint sx = q.x >> 16, sy = q.y & 0xffff, w = en.y & 0xffff, mir = (en.y >> 16) & 1;\n"
"  /* modulo the slice size: the entry position is the corner of the WHOLE image, which may lie outside the\n"
"     slice when only its drawn part was put into the atlas (hdworld_upload) */\n"
"  uint ax = ((en.x & 0xfff) + (mir ? w - 1 - sx : sx) * 2) & 0xfff, ay = (((en.x >> 12) & 0xfff) + sy * 2) & 0xfff;\n"
"  o.w = 0x80000000 | (ax >> 1) | ((ay >> 1) << 11) | (((en.x >> 24) & 3) << 22) | (((q.y >> 24) & 15) << 24)\n"
"         | (mir << 28) | (((en.y >> 17) & 1) << 29) | (((en.y >> 18) & 1) << 30);\n"
"  /* low objects (rocks, bushes: entry bit 31) sort below every standing sprite (+0x2000 for those):\n"
"     Sarge stands on top of a rock's upscaled edge, trees still cover him by painter order */\n"
"  if ((en.y >> 17) & 1) {\n"
"    int bot = clamp(ip.y - (int)sy + (int)((en.y >> 19) & 0x7ff) + 1, 1, 0x1fff);\n"
"    /* turret (bit 30): above its hull at the same row; low (bit 31): below standing sprites */\n"
"    o.d |= (uint)((en.y & 0x80000000) ? bot : (en.y & 0x40000000) ? bot + 0x2080 : bot + 0x2000);\n"
"  }\n"
"  return o;\n"
"}\n"
"/* bit 30 (near): another sprite with a margin within 4 px - ps_world looks for its edge here */\n"
"uint ps_near(V i) : SV_Target {\n"
"  int2 ip = int2(i.pos.xy);\n"
"  uint r = wref0.Load(int3(ip, 0));\n"
"  if ((r & 0x80000000) == 0) return r;\n"
"  r &= ~0x40000000u;\n"
"  if (wdepth.Load(int3(ip, 0)) & 0xC000) return r | 0x40000000;   /* route line / ghost figure: ps_pick / ps_world look */\n"
"  for (int dy = -4; dy <= 4; dy++)\n"
"    for (int dx = -4; dx <= 4; dx++) {\n"
"      if (dx == 0 && dy == 0) continue;\n"
"      uint q = wref0.Load(int3(ip + int2(dx, dy), 0));\n"
"      if ((q & 0xe0000000) == 0xe0000000 && (zm.w > 0.5 ? other_sprite(r, q, dx, dy) : !(r & (1u << 29)))) return r | 0x40000000;\n"
"    }\n"
"  return r;\n"
"}\n"
"/* Per game pixel near another sprite (bit 30): which neighbouring image is drawn in FRONT of this pixel\n"
"   (frontmost depth above its own, opaque at any of the pixel's 4 sub-texels) and which is the nearest one\n"
"   BEHIND (sprite pixels only). Packed: front dx+4 | dy+4 << 4 | 0x100, back the same << 9. ps_world then\n"
"   reads only these two (the 9x9 search at game size, not per window pixel). */\n"
"float max_a(uint q, int2 o) {\n"
"  return max(max(texel(q, o).a, texel(q, o + int2(1, 0)).a), max(texel(q, o + int2(0, 1)).a, texel(q, o + int2(1, 1)).a));\n"
"}\n"
"uint ps_pick(V i) : SV_Target {\n"
"  int2 ip = int2(i.pos.xy);\n"
"  uint r = wref.Load(int3(ip, 0));\n"
"  if ((r & 0xc0000000) != 0xc0000000) return 0;\n"
"  uint dd = wdepth.Load(int3(ip, 0));\n"
"  int dO = (int)(dd & 0x3fff);\n"
"  bool spr = (r & (1u << 29)) != 0;\n"
"  int fd = dO, bd = -1; uint fo = 0, bo = 0;\n"
"  uint und = ((dd & 0x8000) ? 0x80000000u : 0u) | ((dd & 0x4000) ? 0x40000000u : 0u);   /* + bit 30: ghost figure */\n"
"  for (int dy = -4; dy <= 4; dy++)\n"
"    for (int dx = -4; dx <= 4; dx++) {\n"
"      if (dx == 0 && dy == 0) continue;\n"
"      uint q = wref.Load(int3(ip + int2(dx, dy), 0));\n"
"      if ((q & 0xe0000000) != 0xe0000000 || !other_sprite(r, q, dx, dy)) continue;\n"
"      int qd = (int)(wdepth.Load(int3(ip + int2(dx, dy), 0)) & 0x3fff);\n"
"      bool front = qd > fd, back = spr && qd < dO && qd > bd;\n"
"      if (!front && !back) continue;\n"
"      int2 o = int2(-dx, -dy) * 2;\n"
"      if (q & (1u << 28)) o.x = -o.x;\n"
"      if (max_a(q, o) < 0.75) continue;\n"
"      uint code = (uint)(dx + 4) | ((uint)(dy + 4) << 4) | 0x100;\n"
"      if (front) { fd = qd; fo = code; } else { bd = qd; bo = code; }\n"
"    }\n"
"  return fo | (bo << 9) | und;\n"
"}\n";

typedef HRESULT (WINAPI *D3DCompileFn)(const void *src, SIZE_T len, const char *name, const void *defines,
                                        void *include, const char *entry, const char *target, UINT f1, UINT f2,
                                        ID3DBlob **code, ID3DBlob **errors);

typedef struct { float mode[4], amb[4], hd[4], op[4], uvr[4], opid[4], sub[4], zm[4], ramp[64][4], lk[4][4], lo[4][4], pm[4], mr[3][4], tz[32][4], tc[4], tk[32][4], ta[4], td[4], tp[4][4], ti[4]; float tik[8][4], tsg[4]; } Params;

#define AMBIENT_DIM   0.3f      /* fill brightness */
#define AMBIENT_BLUR  0.05f     /* blur step, in texture coordinates */

static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps_scale, *g_ps_ambient, *g_ps_hd, *g_ps_sprite, *g_ps_world, *g_ps_resolve, *g_ps_near, *g_ps_shadow, *g_ps_palette, *g_ps_pick;
static ID3D11Texture2D *g_shadow; static ID3D11ShaderResourceView *g_shadow_srv; static ID3D11RenderTargetView *g_shadow_rtv;
/* GPU reference resolve (hdworld.c): raw refs, shown bytes, entry table -> wref0 -> (near) -> g_wref */
static ID3D11Texture2D *g_refs, *g_fbytes, *g_ents, *g_wref0, *g_maprefs, *g_over, *g_mapmove;
static ID3D11ShaderResourceView *g_maprefs_srv, *g_over_srv, *g_mapmove_srv;   /* g_mapmove: scratch for map scrolls */
static int g_map_w, g_map_h;
static RECT g_over_prev;                          /* override box uploaded last frame */
static ID3D11ShaderResourceView *g_refs_srv, *g_fbytes_srv, *g_ents_srv, *g_wref0_srv;
static ID3D11RenderTargetView *g_wref0_rtv, *g_wref_rtv;
static ID3D11Texture2D *g_depth; static ID3D11ShaderResourceView *g_depth_srv; static ID3D11RenderTargetView *g_depth_rtv;   /* painter depth (ps_resolve target 1) */
static ID3D11Texture2D *g_pick; static ID3D11ShaderResourceView *g_pick_srv; static ID3D11RenderTargetView *g_pick_rtv;      /* ps_pick output */
static int g_ents_rows;
static ID3D11Device *g_device;                  /* for hdworld uploads */
static ID3D11Texture2D *g_atlas, *g_wref;        /* hdworld.c: 2x atlas (array) and per-pixel references */
static ID3D11ShaderResourceView *g_atlas_srv, *g_wref_srv;
static int g_wref_w, g_wref_h;
static ID3D11Texture2D *g_idtex;                 /* id map of upscaled pictures (hdsprites.c) */
static ID3D11ShaderResourceView *g_idtex_srv;
static int g_id_w, g_id_h;
typedef struct { unsigned serial; ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; DWORD used; } SpriteTex;
#define SPRITE_CACHE 48
static SpriteTex g_sprites[SPRITE_CACHE];
static ID3D11Texture2D *g_hd;                    /* HD menu background (hdart.c) */
static ID3D11ShaderResourceView *g_hd_srv;
static int g_hd_serial = -1;
static ID3D11SamplerState *g_point, *g_linear;
static ID3D11Buffer *g_cb;
static ID3D11Texture2D *g_src;                   /* the game picture */
static ID3D11ShaderResourceView *g_src_srv;
static ID3D11RenderTargetView *g_src_rtv;           /* ps_palette renders the gameplay picture into g_src */
/* the indexed gameplay picture (present.c gpu_set_indexed): 8-bit pixels, palette, interface mask */
static ID3D11Texture2D *g_idx_tex, *g_pal_tex, *g_mask_tex;
static ID3D11ShaderResourceView *g_idx_srv, *g_pal_srv, *g_mask_srv;
static struct { const BYTE *idx; uint32_t lut[256]; const BYTE *mask; int mask_lo, mask_hi; RECT rects[3]; int nrects; } g_in;
static int g_mask_fresh;                          /* g_mask_tex was (re)made: upload it whole */
static int g_idx_ok;                              /* g_idx_tex holds this frame's pixels (draw_world reuses it) */
static int g_tex_w, g_tex_h;
static int g_state;                              /* 0 = not tried, 1 = ready, -1 = unavailable */

#define RELEASE(p, T) do { if (p) { T##_Release(p); (p) = NULL; } } while (0)

static void release_textures(void)
{
    RELEASE(g_src_srv, ID3D11ShaderResourceView);
    RELEASE(g_src_rtv, ID3D11RenderTargetView);
    RELEASE(g_idx_srv, ID3D11ShaderResourceView); RELEASE(g_idx_tex, ID3D11Texture2D);
    RELEASE(g_mask_srv, ID3D11ShaderResourceView); RELEASE(g_mask_tex, ID3D11Texture2D);
    RELEASE(g_pal_srv, ID3D11ShaderResourceView); RELEASE(g_pal_tex, ID3D11Texture2D);
    RELEASE(g_src, ID3D11Texture2D);
    RELEASE(g_hd_srv, ID3D11ShaderResourceView);
    RELEASE(g_hd, ID3D11Texture2D);
    g_hd_serial = -1;
    RELEASE(g_idtex_srv, ID3D11ShaderResourceView);
    RELEASE(g_idtex, ID3D11Texture2D);
    g_id_w = g_id_h = 0;
    for (int i = 0; i < SPRITE_CACHE; i++) { RELEASE(g_sprites[i].srv, ID3D11ShaderResourceView); RELEASE(g_sprites[i].tex, ID3D11Texture2D); g_sprites[i].serial = 0; }
    g_tex_w = g_tex_h = 0;
}

static void release_world_inputs(void)
{
    RELEASE(g_refs_srv, ID3D11ShaderResourceView); RELEASE(g_refs, ID3D11Texture2D);
    RELEASE(g_fbytes_srv, ID3D11ShaderResourceView); RELEASE(g_fbytes, ID3D11Texture2D);
    RELEASE(g_ents_srv, ID3D11ShaderResourceView); RELEASE(g_ents, ID3D11Texture2D);
    RELEASE(g_wref0_rtv, ID3D11RenderTargetView); RELEASE(g_wref0_srv, ID3D11ShaderResourceView); RELEASE(g_wref0, ID3D11Texture2D);
    RELEASE(g_wref_rtv, ID3D11RenderTargetView); RELEASE(g_wref_srv, ID3D11ShaderResourceView); RELEASE(g_wref, ID3D11Texture2D);
    RELEASE(g_shadow_rtv, ID3D11RenderTargetView); RELEASE(g_shadow_srv, ID3D11ShaderResourceView); RELEASE(g_shadow, ID3D11Texture2D);
    RELEASE(g_depth_rtv, ID3D11RenderTargetView); RELEASE(g_depth_srv, ID3D11ShaderResourceView); RELEASE(g_depth, ID3D11Texture2D);
    RELEASE(g_pick_rtv, ID3D11RenderTargetView); RELEASE(g_pick_srv, ID3D11ShaderResourceView); RELEASE(g_pick, ID3D11Texture2D);
    RELEASE(g_over_srv, ID3D11ShaderResourceView); RELEASE(g_over, ID3D11Texture2D);
    RELEASE(g_maprefs_srv, ID3D11ShaderResourceView); RELEASE(g_maprefs, ID3D11Texture2D);
    RELEASE(g_mapmove_srv, ID3D11ShaderResourceView); RELEASE(g_mapmove, ID3D11Texture2D);
    g_wref_w = g_wref_h = 0; g_ents_rows = 0; g_map_w = g_map_h = 0;
}
/* menu label layer (menutext.c): see draw_overlay */
static ID3D11Texture2D *g_imask; static ID3D11ShaderResourceView *g_imask_srv;   /* building floor / doors (roofs.c) */
static ID3D11PixelShader *g_ps_overlay;
static ID3D11Texture2D *g_ovl;
static ID3D11ShaderResourceView *g_ovl_srv;
static ID3D11BlendState *g_blend_pm;
static int g_ovl_w, g_ovl_h;
void gpu_release(void)
{
    release_textures();
    RELEASE(g_ps_overlay, ID3D11PixelShader);
    RELEASE(g_imask_srv, ID3D11ShaderResourceView);
    RELEASE(g_imask, ID3D11Texture2D);
    RELEASE(g_ovl_srv, ID3D11ShaderResourceView);
    RELEASE(g_ovl, ID3D11Texture2D);
    RELEASE(g_blend_pm, ID3D11BlendState);
    g_ovl_w = g_ovl_h = 0;
    RELEASE(g_vs, ID3D11VertexShader);
    RELEASE(g_ps_scale, ID3D11PixelShader);
    RELEASE(g_ps_ambient, ID3D11PixelShader);
    RELEASE(g_ps_hd, ID3D11PixelShader);
    RELEASE(g_ps_sprite, ID3D11PixelShader);
    RELEASE(g_ps_world, ID3D11PixelShader);
    RELEASE(g_ps_resolve, ID3D11PixelShader);
    RELEASE(g_ps_near, ID3D11PixelShader);
    RELEASE(g_ps_shadow, ID3D11PixelShader);
    RELEASE(g_ps_pick, ID3D11PixelShader);
    RELEASE(g_ps_palette, ID3D11PixelShader);
    release_world_inputs();
    RELEASE(g_atlas_srv, ID3D11ShaderResourceView); RELEASE(g_atlas, ID3D11Texture2D);
    RELEASE(g_wref_srv, ID3D11ShaderResourceView); RELEASE(g_wref, ID3D11Texture2D);
    g_wref_w = g_wref_h = 0;
    RELEASE(g_point, ID3D11SamplerState);
    RELEASE(g_linear, ID3D11SamplerState);
    RELEASE(g_cb, ID3D11Buffer);
    g_state = 0;
}

/* ---------------------------------------------------------------- menu label layer (menutext.c, 2026-11-20) */
static unsigned g_ovl_ver;
int gpu_overlay_ready(void) { return g_ps_overlay != NULL; }
static void draw(ID3D11DeviceContext *ctx, ID3D11PixelShader *ps, float x, float y, float w, float h);
/* over the menu picture at dest (w x h game px): the labels, premultiplied, at window resolution */
static void draw_overlay(ID3D11Device *dev, ID3D11DeviceContext *ctx, RECT dest, int w, int h)
{
    if (!g_ps_overlay) return;
    int dw = dest.right - dest.left, dh = dest.bottom - dest.top;
    const uint32_t *pix = NULL; unsigned ver = 0; RECT dirty;
    if (dw <= 0 || dh <= 0 || !menutext_overlay(dw, dh, w, h, &pix, &ver, &dirty) || !pix) return;
    int fresh = 0;
    if (!g_ovl || g_ovl_w != dw || g_ovl_h != dh) {
        if (g_ovl_srv) { ID3D11ShaderResourceView_Release(g_ovl_srv); g_ovl_srv = NULL; }
        if (g_ovl) { ID3D11Texture2D_Release(g_ovl); g_ovl = NULL; }
        D3D11_TEXTURE2D_DESC td = { (UINT)dw, (UINT)dh, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &g_ovl))) { g_ovl = NULL; return; }
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_ovl, NULL, &g_ovl_srv))) { ID3D11Texture2D_Release(g_ovl); g_ovl = NULL; return; }
        g_ovl_w = dw; g_ovl_h = dh; fresh = 1;
    }
    /* only the part that changed (2026-11-20: uploading the whole window-sized layer per hover stuttered) */
    if (fresh) ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_ovl, 0, NULL, pix, dw * 4, 0);
    else if (!IsRectEmpty(&dirty)) {
        D3D11_BOX box = { (UINT)dirty.left, (UINT)dirty.top, 0, (UINT)dirty.right, (UINT)dirty.bottom, 1 };
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_ovl, 0, &box, pix + (size_t)dirty.top * dw + dirty.left, dw * 4, 0);
    }
    g_ovl_ver = ver;
    if (!g_blend_pm) {
        D3D11_BLEND_DESC bd; memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(ID3D11Device_CreateBlendState(dev, &bd, &g_blend_pm))) { g_blend_pm = NULL; return; }
    }
    ID3D11DeviceContext_OMSetBlendState(ctx, g_blend_pm, NULL, 0xffffffff);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 18, 1, &g_ovl_srv);
    draw(ctx, g_ps_overlay, (float)dest.left, (float)dest.top, (float)dw, (float)dh);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 18, 1, &none);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffff);
}

static ID3DBlob *compile(D3DCompileFn fn, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = fn(SHADERS, sizeof SHADERS - 1, "armymen-mod", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        mlog("gpu: shader %s failed (%#lx): %s", entry, hr, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (code) ID3D10Blob_Release(code);
        code = NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

/* Compiles the shaders and makes the fixed objects; returns 1 when the GPU path can be used. */
int gpu_init(ID3D11Device *dev)
{
    if (g_state) return g_state > 0;
    g_state = -1;
    HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
    if (!lib) lib = LoadLibraryA("d3dcompiler_43.dll");
    D3DCompileFn fn = lib ? (D3DCompileFn)GetProcAddress(lib, "D3DCompile") : NULL;
    if (!fn) { mlog("gpu: no d3dcompiler - scaling on the CPU"); return 0; }
    ID3DBlob *vs = compile(fn, "vs", "vs_4_0"), *sc = compile(fn, "ps_scale", "ps_4_0"), *am = compile(fn, "ps_ambient", "ps_4_0"), *hd = compile(fn, "ps_hd", "ps_4_0"), *sp = compile(fn, "ps_sprite", "ps_4_0"), *wo = compile(fn, "ps_world", "ps_4_0"), *rs = compile(fn, "ps_resolve", "ps_4_0"), *nr = compile(fn, "ps_near", "ps_4_0"), *shd = compile(fn, "ps_shadow", "ps_4_0"), *pp = compile(fn, "ps_palette", "ps_4_0"), *pk = compile(fn, "ps_pick", "ps_4_0");
    int ok = vs && sc && am && hd && sp && wo && rs && nr && shd && pp && pk;
    g_device = dev;
    if (ok) ok = SUCCEEDED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &g_vs))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(sc), ID3D10Blob_GetBufferSize(sc), NULL, &g_ps_scale))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(am), ID3D10Blob_GetBufferSize(am), NULL, &g_ps_ambient))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(hd), ID3D10Blob_GetBufferSize(hd), NULL, &g_ps_hd))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(sp), ID3D10Blob_GetBufferSize(sp), NULL, &g_ps_sprite))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(wo), ID3D10Blob_GetBufferSize(wo), NULL, &g_ps_world))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(rs), ID3D10Blob_GetBufferSize(rs), NULL, &g_ps_resolve))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(nr), ID3D10Blob_GetBufferSize(nr), NULL, &g_ps_near))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(shd), ID3D10Blob_GetBufferSize(shd), NULL, &g_ps_shadow))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pp), ID3D10Blob_GetBufferSize(pp), NULL, &g_ps_palette))
              && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pk), ID3D10Blob_GetBufferSize(pk), NULL, &g_ps_pick));
    if (vs) ID3D10Blob_Release(vs);
    if (sc) ID3D10Blob_Release(sc);
    if (am) ID3D10Blob_Release(am);
    if (hd) ID3D10Blob_Release(hd);
    if (sp) ID3D10Blob_Release(sp);
    if (wo) ID3D10Blob_Release(wo);
    if (rs) ID3D10Blob_Release(rs);
    if (nr) ID3D10Blob_Release(nr);
    if (shd) ID3D10Blob_Release(shd);
    if (pp) ID3D10Blob_Release(pp);
    if (pk) ID3D10Blob_Release(pk);
    {   /* the menu label layer (menutext.c) - optional: without it the game draws its own labels */
        ID3DBlob *ov = ok ? compile(fn, "ps_overlay", "ps_4_0") : NULL;
        if (ov) { ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ov), ID3D10Blob_GetBufferSize(ov), NULL, &g_ps_overlay); ID3D10Blob_Release(ov); }
    }
    if (ok) {
        D3D11_SAMPLER_DESC s = { 0 };
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        ok = SUCCEEDED(ID3D11Device_CreateSamplerState(dev, &s, &g_point));
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ok = ok && SUCCEEDED(ID3D11Device_CreateSamplerState(dev, &s, &g_linear));
        D3D11_BUFFER_DESC b = { sizeof(Params), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(dev, &b, NULL, &g_cb));
    }
    if (!ok) { gpu_release(); g_state = -1; mlog("gpu: shaders unavailable - scaling on the CPU"); return 0; }
    g_state = 1;
    mlog("gpu: scaling on the graphics card");
    return 1;
}

static int ensure_texture(ID3D11Device *dev, int w, int h)
{
    if (g_src && w == g_tex_w && h == g_tex_h) return 1;
    release_textures();
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_src)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_src, NULL, &g_src_srv)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)g_src, NULL, &g_src_rtv))) {
        release_textures();
        return 0;
    }
    /* the indexed picture: 8-bit pixels and interface mask (game size), palette (256 x 1) */
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.Format = DXGI_FORMAT_R8_UINT;
    int ok = SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_idx_tex)) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_idx_tex, NULL, &g_idx_srv)) &&
             SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_mask_tex)) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_mask_tex, NULL, &g_mask_srv));
    d.Width = 256; d.Height = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    ok = ok && SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_pal_tex)) &&
         SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_pal_tex, NULL, &g_pal_srv));
    if (!ok) mlog("gpu: no indexed picture textures - colours converted on the CPU");
    g_mask_fresh = 1;
    g_tex_w = w; g_tex_h = h;
    return 1;
}

/* present.c, before gpu_draw: this frame's picture as 8-bit pixels + palette (+ interface mask), or
 * idx = NULL (menus, videos: gpu_draw gets 32-bit colours). The pointers stay valid during gpu_draw. */
void gpu_set_indexed(const BYTE *idx, const uint32_t *lut, const BYTE *mask, int mask_lo, int mask_hi, const RECT *rects, int nrects)
{
    g_in.idx = idx;
    if (!idx) return;
    memcpy(g_in.lut, lut, sizeof g_in.lut);
    g_in.mask = mask; g_in.mask_lo = mask_lo; g_in.mask_hi = mask_hi;
    g_in.nrects = nrects > 3 ? 3 : nrects;
    for (int i = 0; i < g_in.nrects; i++) g_in.rects[i] = rects[i];
}

static void draw(ID3D11DeviceContext *ctx, ID3D11PixelShader *ps, float x, float y, float w, float h);
/* present.c snapshot: can this frame (w x h) go to the GPU as 8-bit pixels? (textures made at that size) */
int gpu_indexed_ready(int w, int h)
{
    static int on = -1;
    if (on < 0) on = GetPrivateProfileIntA("display", "gpu_palette", 1, mod_ini_path());
    return on && g_state > 0 && g_src_rtv && g_idx_tex && g_pal_tex && g_mask_tex && g_ps_palette && w == g_tex_w && h == g_tex_h;
}

/* The palette pass (ps_palette): uploads 1 byte per pixel + the 1 KB palette (+ changed mask rows)
 * and renders the colour picture into g_src on the GPU. Was: 4 bytes per pixel converted on the CPU
 * and a CPU loop merging the interface mask into the alpha. Returns 0 if not available. */
static int palette_pass(ID3D11DeviceContext *ctx, Params *p, int w, int h)
{
    g_idx_ok = 0;
    if (!g_in.idx || !g_idx_tex || !g_pal_tex || !g_mask_tex || !g_src_rtv || !g_ps_palette) return 0;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_idx_tex, 0, NULL, g_in.idx, (UINT)w, 0);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_pal_tex, 0, NULL, g_in.lut, 256 * 4, 0);
    if (g_in.mask) {
        int lo = g_in.mask_lo, hi = g_in.mask_hi;
        if (g_mask_fresh) { lo = 0; hi = h; g_mask_fresh = 0; }
        if (hi > h) hi = h;
        if (lo < 0) lo = 0;
        if (hi > lo) {
            D3D11_BOX b = { 0, (UINT)lo, 0, (UINT)w, (UINT)hi, 1 };
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_mask_tex, 0, &b, g_in.mask + (size_t)lo * w, (UINT)w, 0);
        }
    }
    p->pm[0] = g_in.mask ? 1.0f : 0.0f;
    p->pm[1] = (float)g_in.nrects;
    for (int i = 0; i < g_in.nrects; i++) {
        p->mr[i][0] = (float)g_in.rects[i].left; p->mr[i][1] = (float)g_in.rects[i].top;
        p->mr[i][2] = (float)g_in.rects[i].right; p->mr[i][3] = (float)g_in.rects[i].bottom;
    }
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_cb, 0, NULL, p, 0, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_cb);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);   /* g_src is the target now */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_src_rtv, NULL);
    ID3D11ShaderResourceView *in[3] = { g_idx_srv, g_mask_srv, g_pal_srv };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 13, 3, in);
    draw(ctx, g_ps_palette, 0, 0, (float)w, (float)h);
    ID3D11ShaderResourceView *none3[3] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 13, 3, none3);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    g_idx_ok = 1;
    return 1;
}

static void draw(ID3D11DeviceContext *ctx, ID3D11PixelShader *ps, float x, float y, float w, float h)
{
    D3D11_VIEWPORT vp = { x, y, w, h, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
}

/* ---------------------------------------------------------------- upscaled pictures (hdsprites.c) */

static ID3D11ShaderResourceView *sprite_srv(ID3D11Device *dev, ID3D11DeviceContext *ctx, const HdOp *o)
{
    int lru = 0;
    for (int i = 0; i < SPRITE_CACHE; i++) {
        if (g_sprites[i].serial == o->serial && g_sprites[i].srv) { g_sprites[i].used = GetTickCount(); return g_sprites[i].srv; }
        if (g_sprites[i].used < g_sprites[lru].used) lru = i;
    }
    SpriteTex *t = &g_sprites[lru];
    RELEASE(t->srv, ID3D11ShaderResourceView);
    RELEASE(t->tex, ID3D11Texture2D);
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = o->pw; d.Height = o->ph; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &t->tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t->tex, NULL, &t->srv))) {
        RELEASE(t->srv, ID3D11ShaderResourceView);
        RELEASE(t->tex, ID3D11Texture2D);
        t->serial = 0;
        return NULL;
    }
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)t->tex, 0, NULL, o->pix, o->pw * 4, 0);
    t->serial = o->serial;
    t->used = GetTickCount();
    return t->srv;
}

/* Each upscaled picture drawn over its place in the view, only where the id map shows it. */
static void draw_sprites(ID3D11Device *dev, ID3D11DeviceContext *ctx, Params *p, int w, int h, RECT dest,
                         int n, const HdOp *ops, const BYTE *idmap)
{
    if (!g_idtex || g_id_w != w || g_id_h != h) {
        RELEASE(g_idtex_srv, ID3D11ShaderResourceView);
        RELEASE(g_idtex, ID3D11Texture2D);
        D3D11_TEXTURE2D_DESC d = { 0 };
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8_UINT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_idtex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_idtex, NULL, &g_idtex_srv))) {
            RELEASE(g_idtex_srv, ID3D11ShaderResourceView);
            RELEASE(g_idtex, ID3D11Texture2D);
            return;
        }
        g_id_w = w; g_id_h = h;
    }
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_idtex, 0, NULL, idmap, w, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 2, 1, &g_idtex_srv);
    double sx = (double)(dest.right - dest.left) / w, sy = (double)(dest.bottom - dest.top) / h;
    for (int i = 0; i < n; i++) {
        const HdOp *o = &ops[i];
        ID3D11ShaderResourceView *srv = sprite_srv(dev, ctx, o);
        if (!srv) continue;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 3, 1, &srv);
        p->op[0] = (float)o->x; p->op[1] = (float)o->y; p->op[2] = (float)o->w; p->op[3] = (float)o->h;
        p->uvr[0] = o->u0; p->uvr[1] = o->v0; p->uvr[2] = o->u1; p->uvr[3] = o->v1;
        p->opid[0] = (float)o->id; p->opid[1] = (float)w; p->opid[2] = (float)h;
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_cb, 0, NULL, p, 0, 0);
        draw(ctx, g_ps_sprite, (float)(dest.left + o->x * sx), (float)(dest.top + o->y * sy), (float)(o->w * sx), (float)(o->h * sy));
    }
}

/* ---------------------------------------------------------------- upscaled battlefield (hdworld.c) */

/* Puts one decoded image (BGRA) into the atlas; creates the atlas on first use. */
int gpu_world_put(void *ctxv, int slice, int x, int y, int w, int h, const uint32_t *pix, int pitch)
{
    ID3D11DeviceContext *ctx = ctxv;
    if (!g_device || !ctx) return 0;
    if (!g_atlas) {
        D3D11_TEXTURE2D_DESC d = { 0 };
        d.Width = d.Height = ATLAS_SIZE; d.MipLevels = 1; d.ArraySize = ATLAS_SLICES;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_device, &d, NULL, &g_atlas)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_device, (ID3D11Resource *)g_atlas, NULL, &g_atlas_srv))) {
            RELEASE(g_atlas_srv, ID3D11ShaderResourceView); RELEASE(g_atlas, ID3D11Texture2D);
            mlog("gpu: could not create the upscale atlas");
            return 0;
        }
        mlog("gpu: upscale atlas %d x %dx%d", ATLAS_SLICES, ATLAS_SIZE, ATLAS_SIZE);
    }
    D3D11_BOX box = { (UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_atlas, (UINT)slice, &box, pix, (UINT)pitch, 0);   /* pitch: the rows of the whole image (a cropped part is uploaded) */
    return 1;
}

static int make_tex(ID3D11Device *dev, int w, int h, DXGI_FORMAT f, int rt, ID3D11Texture2D **t, ID3D11ShaderResourceView **srv, ID3D11RenderTargetView **rtv)
{
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0);
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, t))) return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)*t, NULL, srv))) return 0;
    if (rt && FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)*t, NULL, rtv))) return 0;
    return 1;
}

/* The upscaled battlefield. The per-pixel references are resolved on the GPU at game size (ps_resolve,
 * ps_near into g_wref), then ps_world draws the 2x atlas texels at window size. rtv = the back buffer.
 * Uploads per frame: the map's CHANGED rectangles only (g_maprefs stays on the GPU), the screen's own
 * references inside the override box only, the override mask and the shown bytes (1 byte per pixel). */
/* timing trace (present.c): where the display thread's gpu submission goes, last frame (ms / pixels) */
volatile double g_gt_wget, g_gt_wup, g_gt_atlas; volatile int g_gt_dirty_px, g_gt_move_px;
static double gt_now(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)f.QuadPart; }
static void draw_world(ID3D11Device *dev, ID3D11DeviceContext *ctx, Params *p, int w, int h, RECT dest, ID3D11RenderTargetView *rtv)
{
    static HdWorldIn in;
    double tw0 = gt_now();
    int got = g_atlas_srv && hdworld_gpu_world(&in);
    g_gt_wget = gt_now() - tw0;
    if (!got) { static int n; if (g_cfg.log >= 2 && n++ % 600 == 0) mlog("gpu: world skipped: atlas %p", (void *)g_atlas_srv); return; }
    if (in.view.right - in.view.left != w || in.view.bottom - in.view.top != h) { static int n; if (g_cfg.log >= 2 && n++ % 600 == 0) mlog("gpu: world skipped: view %ldx%ld picture %dx%d", in.view.right - in.view.left, in.view.bottom - in.view.top, w, h); hdworld_force_map(); return; }   /* its map changes are lost: all again */
    const uint32_t *ents = in.ents;
    if (!g_wref || g_wref_w != w || g_wref_h != h) {
        release_world_inputs();
        if (!make_tex(dev, w, h, DXGI_FORMAT_R32G32_UINT, 0, &g_refs, &g_refs_srv, NULL) ||
            !make_tex(dev, w, h, DXGI_FORMAT_R8_UINT, 0, &g_fbytes, &g_fbytes_srv, NULL) ||
            !make_tex(dev, w, h, DXGI_FORMAT_R8_UINT, 0, &g_over, &g_over_srv, NULL) ||
            !make_tex(dev, w, h, DXGI_FORMAT_R32_UINT, 1, &g_wref0, &g_wref0_srv, &g_wref0_rtv) ||
            !make_tex(dev, w, h, DXGI_FORMAT_R32_UINT, 1, &g_wref, &g_wref_srv, &g_wref_rtv) ||
            !make_tex(dev, w, h, DXGI_FORMAT_R8_UNORM, 1, &g_shadow, &g_shadow_srv, &g_shadow_rtv) ||
            !make_tex(dev, w, h, DXGI_FORMAT_R16_UINT, 1, &g_depth, &g_depth_srv, &g_depth_rtv) ||
            !make_tex(dev, w, h, DXGI_FORMAT_R32_UINT, 1, &g_pick, &g_pick_srv, &g_pick_rtv)) {
            mlog("gpu: could not create the battlefield reference textures");
            release_world_inputs();
            return;
        }
        g_wref_w = w; g_wref_h = h;
        hdworld_force_entries(); hdworld_force_map();
        SetRect(&g_over_prev, 0, 0, w, h);                 /* new mask texture: clear it all once */
        return;                                           /* everything again next frame */
    }
    if (in.map_w > 0 && (!g_maprefs || g_map_w != in.map_w || g_map_h != in.map_h)) {
        RELEASE(g_maprefs_srv, ID3D11ShaderResourceView); RELEASE(g_maprefs, ID3D11Texture2D);
        RELEASE(g_mapmove_srv, ID3D11ShaderResourceView); RELEASE(g_mapmove, ID3D11Texture2D);   /* new size */
        if (!make_tex(dev, in.map_w, in.map_h, DXGI_FORMAT_R32G32_UINT, 0, &g_maprefs, &g_maprefs_srv, NULL)) {
            RELEASE(g_maprefs_srv, ID3D11ShaderResourceView); RELEASE(g_maprefs, ID3D11Texture2D);
            return;
        }
        g_map_w = in.map_w; g_map_h = in.map_h;
        hdworld_force_map();                              /* this frame's changes may be partial */
        in.ndirty = 0;
        in.nmoves = 0;
    }
    double tu0 = gt_now();
    int rows = (in.nents + 255) / 256;
    if (!g_ents || rows != g_ents_rows) {
        RELEASE(g_ents_srv, ID3D11ShaderResourceView); RELEASE(g_ents, ID3D11Texture2D);
        if (!make_tex(dev, 256, rows > 0 ? rows : 1, DXGI_FORMAT_R32G32_UINT, 0, &g_ents, &g_ents_srv, NULL)) { RELEASE(g_ents, ID3D11Texture2D); hdworld_force_map(); return; }
        g_ents_rows = rows;
        if (!ents) { hdworld_force_entries(); hdworld_force_map(); return; }
    }
    if (ents) {                                           /* changed: whole rows (the last one partly) */
        int full = in.nents / 256, rest = in.nents % 256;
        if (full) { D3D11_BOX b = { 0, 0, 0, 256, (UINT)full, 1 }; ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_ents, 0, &b, ents, 256 * 8, 0); }
        if (rest) { D3D11_BOX b = { 0, (UINT)full, 0, (UINT)rest, (UINT)full + 1, 1 }; ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_ents, 0, &b, ents + (size_t)full * 512, 256 * 8, 0); }
    }
    /* map scrolls first (hdworld.c map_move_locked): GPU to GPU through a scratch texture, as source and
     * destination overlap inside one texture; then the changed rectangles below carry the final references */
    if (in.nmoves && g_maprefs && !g_mapmove
        && !make_tex(dev, g_map_w, g_map_h, DXGI_FORMAT_R32G32_UINT, 0, &g_mapmove, &g_mapmove_srv, NULL)) {
        RELEASE(g_mapmove_srv, ID3D11ShaderResourceView); RELEASE(g_mapmove, ID3D11Texture2D);
        hdworld_force_map();                              /* cannot move: upload everything next frame */
        in.nmoves = 0;
    }
    for (int i = 0; i < in.nmoves && g_maprefs; i++) {
        RECT s = in.move_src[i], d = in.move_dst[i];
        D3D11_BOX b = { (UINT)s.left, (UINT)s.top, 0, (UINT)s.right, (UINT)s.bottom, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)g_mapmove, 0, (UINT)s.left, (UINT)s.top, 0, (ID3D11Resource *)g_maprefs, 0, &b);
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)g_maprefs, 0, (UINT)d.left, (UINT)d.top, 0, (ID3D11Resource *)g_mapmove, 0, &b);
    }
    for (int i = 0; i < in.ndirty && g_maprefs; i++) {    /* the map: changed rectangles only */
        RECT r = in.dirty[i];
        D3D11_BOX b = { (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_maprefs, 0, &b, in.dirty_refs[i], (UINT)(r.right - r.left) * 8, 0);
    }
    RECT ob = in.over_box, vr = { 0, 0, w, h };
    if (IntersectRect(&ob, &ob, &vr)) {                   /* the screen's own references: drawn-over parts only */
        D3D11_BOX b = { (UINT)ob.left, (UINT)ob.top, 0, (UINT)ob.right, (UINT)ob.bottom, 1 };
        const BYTE *src = (const BYTE *)in.screen_refs + (size_t)ob.top * in.screen_pitch + (size_t)ob.left * 8;
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_refs, 0, &b, src, (UINT)in.screen_pitch, 0);
    }
    {   /* override mask: only where it was set this frame or the last (elsewhere the texture stays 0) */
        RECT cur = in.over_box, up;
        if (IsRectEmpty(&cur)) SetRectEmpty(&cur);
        UnionRect(&up, &cur, &g_over_prev);
        if (IntersectRect(&up, &up, &vr)) {
            D3D11_BOX b = { (UINT)up.left, (UINT)up.top, 0, (UINT)up.right, (UINT)up.bottom, 1 };
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_over, 0, &b, in.over + (size_t)up.top * w + up.left, (UINT)w, 0);
        }
        g_over_prev = cur;
    }
    if (!g_idx_ok) ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_fbytes, 0, NULL, in.bytes, (UINT)w, 0);
    g_gt_wup = gt_now() - tu0;
    { int px = 0; for (int i = 0; i < in.ndirty; i++) px += (in.dirty[i].right - in.dirty[i].left) * (in.dirty[i].bottom - in.dirty[i].top); g_gt_dirty_px = px; px = 0; for (int i = 0; i < in.nmoves; i++) px += (in.move_dst[i].right - in.move_dst[i].left) * (in.move_dst[i].bottom - in.move_dst[i].top); g_gt_move_px = px; }
    memset(p->lk, 0, sizeof p->lk); memset(p->lo, 0, sizeof p->lo);
    int nl = g_maprefs ? in.nlinks : 0;
    for (int i = 0; i < nl && i < 4; i++) {
        for (int k = 0; k < 4; k++) p->lk[i][k] = (float)in.link[i][k];
        p->lo[i][0] = (float)in.link[i][4]; p->lo[i][1] = (float)in.link[i][5];
    }
    p->lo[0][2] = (float)nl;
    static int edges = -1;                                /* [display] hd_edges=0: margins over plain ground only (old) */
    if (edges < 0) edges = GetPrivateProfileIntA("display", "hd_edges", 1, mod_ini_path());
    p->zm[3] = edges ? 1.0f : 0.0f;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_cb, 0, NULL, p, 0, 0);

    /* pass 1 and 2 at game size */
    ID3D11ShaderResourceView *none[8] = { 0 };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 4, 8, none);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 16, 2, none);       /* depth and pick become render targets */
    ID3D11RenderTargetView *rt2[2] = { g_wref0_rtv, g_depth_rtv };   /* references + painter depth */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 2, rt2, NULL);
    ID3D11ShaderResourceView *in1[3] = { g_refs_srv, g_idx_ok ? g_idx_srv : g_fbytes_srv, g_ents_srv };   /* the palette pass's pixels if uploaded */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 6, 3, in1);
    ID3D11ShaderResourceView *in2[2] = { g_maprefs_srv, g_over_srv };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 10, 2, in2);
    draw(ctx, g_ps_resolve, 0, 0, (float)w, (float)h);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_wref_rtv, NULL);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 9, 1, &g_wref0_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 16, 1, &g_depth_srv);   /* route-line flag */
    draw(ctx, g_ps_near, 0, 0, (float)w, (float)h);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 6, 6, none);
    /* shadow coverage per game pixel, sampled bilinearly by ps_world (was 36 lookups per window pixel) */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_shadow_rtv, NULL);
    ID3D11ShaderResourceView *in3[2] = { g_atlas_srv, g_wref_srv };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 4, 2, in3);
    draw(ctx, g_ps_shadow, 0, 0, (float)w, (float)h);
    /* front / back neighbour per game pixel (ps_pick): atlas, wref, depth in */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &g_pick_rtv, NULL);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 16, 1, &g_depth_srv);
    draw(ctx, g_ps_pick, 0, 0, (float)w, (float)h);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 4, 2, none);

    /* pass 3: the upscaled battlefield into the back buffer */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    hdworld_ramps(p->ramp);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_cb, 0, NULL, p, 0, 0);
    ID3D11ShaderResourceView *srvs[2] = { g_atlas_srv, g_wref_srv };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 4, 2, srvs);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 12, 1, &g_shadow_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 16, 1, &g_depth_srv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 17, 1, &g_pick_srv);
    draw(ctx, g_ps_world, (float)dest.left, (float)dest.top, (float)(dest.right - dest.left), (float)(dest.bottom - dest.top));
}

/* The HD menu background (hdart.c) as a texture; re-uploaded when it changes. */
static int ensure_hd(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    int w, h, serial;
    const uint32_t *pix = hdart_picture(&w, &h, &serial);
    if (!pix) return 0;
    if (g_hd && serial == g_hd_serial) return 1;
    RELEASE(g_hd_srv, ID3D11ShaderResourceView);
    RELEASE(g_hd, ID3D11Texture2D);
    D3D11_TEXTURE2D_DESC d = { 0 };
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_hd)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_hd, NULL, &g_hd_srv))) {
        RELEASE(g_hd_srv, ID3D11ShaderResourceView);
        RELEASE(g_hd, ID3D11Texture2D);
        return 0;
    }
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_hd, 0, NULL, pix, w * 4, 0);
    g_hd_serial = serial;
    return 1;
}

/* Draws one frame into the back buffer bb (cw x ch). pix = the game picture (w x h, BGRA),
 * dest = where it goes in the window; video = smooth scaling; ambient = fill around it;
 * hd = an HD menu background applies (hdart.c; pix alpha marks the game's own pixels). */
int gpu_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *bb, int cw, int ch,
             const uint32_t *pix, int w, int h, RECT dest, int video, int ambient, int hd,
             int hd_n, const HdOp *hd_ops, const BYTE *hd_idmap, int world,
             int sub_on, float subx, float suby, float zoom, float zcx, float zcy, const TargetZones *zones)
{
    if (g_state <= 0 || !ensure_texture(dev, w, h)) return 0;
    { double ta = gt_now(); hdworld_upload(ctx); g_gt_atlas = gt_now() - ta; }
    ID3D11RenderTargetView *rtv = NULL;
    if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv))) return 0;
    static Params pal_p;                          /* the palette pass's own constants */
    if (!g_in.idx || !palette_pass(ctx, &pal_p, w, h))
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_src, 0, NULL, pix, w * 4, 0);
    if (hd) hd = ensure_hd(dev, ctx);

    int dw = dest.right - dest.left, dh = dest.bottom - dest.top;
    /* ambient fill: the picture zoomed to cover the window, so its middle shows behind */
    float cover = (float)cw / w > (float)ch / h ? (float)cw / w : (float)ch / h;
    /* HD: the wide picture keeps the game picture's height; the game picture is its centre */
    float hd_w = hd ? (float)dh * 16.0f / 9.0f : 0;
    Params p = { { video ? 1.0f : 0.0f, g_cfg.crt ? 1.0f : 0.0f, (float)w, (float)h },
                 { cw / (w * cover), ch / (h * cover), AMBIENT_DIM, AMBIENT_BLUR },
                 { hd ? (hd_w - dw) / 2 / hd_w : 0, (!video && !world && w == 640 && h == 480 && (dw % w || dh % h)) ? 1.0f : 0.0f, (float)dw / w, (float)dh / h } };
    p.sub[0] = subx; p.sub[1] = suby; p.sub[2] = sub_on ? 1.0f : 0.0f;
    p.zm[0] = zcx / w; p.zm[1] = zcy / h; p.zm[2] = zoom > 1.0f ? zoom : 1.0f;
    int nz = zones ? zones->n : 0;
    if (nz > ZONES_MAX) nz = ZONES_MAX;
    for (int i = 0; i < nz; i++) {
        p.tz[i][0] = (float)zones->r[i].left; p.tz[i][1] = (float)zones->r[i].top;
        p.tz[i][2] = (float)zones->r[i].right; p.tz[i][3] = (float)zones->r[i].bottom;
        if (i >= zones->nspot && zones->key[i][2] >= 0) {             /* a border object whose image is known */
            p.tk[i][0] = (float)zones->key[i][0]; p.tk[i][1] = (float)zones->key[i][1];
            p.tk[i][2] = (float)zones->key[i][2]; p.tk[i][3] = 1.0f;
        }
    }
    p.tc[0] = nz ? (float)zones->nsec : 0; p.tc[1] = nz ? (float)zones->nspot : 0; p.tc[2] = (float)nz;
    if (zones) {
        p.ta[0] = zones->fade; p.ta[1] = zones->glow; p.ta[2] = zones->menu_over ? 1.0f : 0.0f;
        int np = zones->npanel < 0 ? 0 : zones->npanel > 4 ? 4 : zones->npanel;
        p.ta[3] = (float)np;
        if (zones->in_s > 0) {                                     /* building interior (roofs.c) */
            p.ti[0] = (float)zones->in_ox; p.ti[1] = (float)zones->in_oy; p.ti[2] = zones->in_s;
            int nk = zones->in_nkey < 0 ? 0 : zones->in_nkey > 8 ? 8 : zones->in_nkey;
            p.ti[3] = (float)nk;
            p.tsg[0] = (float)zones->in_sx; p.tsg[1] = (float)zones->in_sy; p.tsg[2] = (float)zones->in_cr;
            for (int i = 0; i < nk; i++) { p.tik[i][0] = (float)zones->in_key[i][0]; p.tik[i][1] = (float)zones->in_key[i][1]; p.tik[i][2] = (float)zones->in_key[i][2]; }
        }
        for (int i = 0; i < np; i++) {
            p.tp[i][0] = (float)zones->panel[i].left; p.tp[i][1] = (float)zones->panel[i].top;
            p.tp[i][2] = (float)zones->panel[i].right; p.tp[i][3] = (float)zones->panel[i].bottom;
        }
    }
    if (zones && zones->dim_on) {
        p.td[0] = (float)zones->dim.left; p.td[1] = (float)zones->dim.top;
        p.td[2] = (float)zones->dim.right; p.td[3] = (float)zones->dim.bottom;
    }
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_cb, 0, NULL, &p, 0, 0);

    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_cb);
    ID3D11SamplerState *samplers[2] = { g_point, g_linear };
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, samplers);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11ShaderResourceView *srvs[2] = { g_src_srv, hd ? g_hd_srv : NULL };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srvs);

    const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    if (hd) {
        float x = dest.left - (hd_w - dw) / 2;
        if (x > 0 || x + hd_w < cw) draw(ctx, g_ps_ambient, 0, 0, (float)cw, (float)ch);
        draw(ctx, g_ps_hd, x, (float)dest.top, hd_w, (float)dh);
    } else {
        if (ambient && (dw < cw || dh < ch)) draw(ctx, g_ps_ambient, 0, 0, (float)cw, (float)ch);
        draw(ctx, g_ps_scale, (float)dest.left, (float)dest.top, (float)dw, (float)dh);
    }
    if (world && zones && zones->in_s > 0) {             /* building floor (r) / doors (g), 64 x 64 cells (roofs.c) */
        if (!g_imask) {
            D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &g_imask)) &&
                FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_imask, NULL, &g_imask_srv))) { ID3D11Texture2D_Release(g_imask); g_imask = NULL; }
        }
        if (g_imask && g_imask_srv) {
            static BYTE m[64 * 64 * 2]; static int f[66][66];
            memset(f, 0, sizeof f);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) f[y + 1][x + 1] = (zones->in_mask[y * 2 + (x >> 5)] >> (x & 31)) & 1;
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    /* 3x3 [1 2 1] blur of the floor cells (user: "the jagged edges ... turned into a smooth view"):
                     * with the linear sampler the 0.5 edge runs round instead of in 8 px steps */
                    int v = f[y][x] + 2 * f[y][x + 1] + f[y][x + 2] + 2 * f[y + 1][x] + 4 * f[y + 1][x + 1] + 2 * f[y + 1][x + 2]
                          + f[y + 2][x] + 2 * f[y + 2][x + 1] + f[y + 2][x + 2];
                    m[(y * 64 + x) * 2] = (BYTE)(v * 255 / 16);
                    m[(y * 64 + x) * 2 + 1] = (zones->in_door[y * 2 + (x >> 5)] >> (x & 31)) & 1 ? 255 : 0;
                }
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)g_imask, 0, NULL, m, 64 * 2, 0);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 19, 1, &g_imask_srv);
        } else p.ti[2] = 0;
    }
    if (world) draw_world(dev, ctx, &p, w, h, dest, rtv);
    { ID3D11ShaderResourceView *none = NULL; ID3D11DeviceContext_PSSetShaderResources(ctx, 19, 1, &none); }
    if (hd_n > 0) draw_sprites(dev, ctx, &p, w, h, dest, hd_n, hd_ops, hd_idmap);
    if (!video) draw_overlay(dev, ctx, dest, w, h);                /* menu labels (menutext.c), also over a paused mission */
    ID3D11RenderTargetView_Release(rtv);
    return 1;
}
