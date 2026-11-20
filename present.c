/* Shows frames through Direct3D 11 / DXGI on a dedicated display thread.
 *
 * The game draws to the screen surface at odd moments (its cursor is drawn straight onto the
 * visible screen between frames). Showing the screen after every such write, without waiting for
 * the monitor, caused tearing. Now game-side writes only mark the screen dirty; this thread shows
 * the latest screen once per monitor refresh. Writes to the screen surface and the snapshot taken
 * here are serialised with present_lock().
 *
 * The palette lookup happens on the CPU; scaling to the window (any size) happens on the graphics
 * card (gpu.c), or here on the CPU if shaders are unavailable. The game keeps its square pixels;
 * videos are scaled smoothly. */
#define COBJMACROS
#include "common.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_3.h>
#include <stdlib.h>

static const GUID G_Texture2D  = {0x6f15aaf2,0xd208,0x4e89,{0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c}};
static const GUID G_DXGIDevice = {0x54ec77fa,0x1377,0x44e6,{0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c}};
static const GUID G_DXGIFactory= {0x7b7166ec,0x21c7,0x44ae,{0xb2,0x1a,0xc9,0xae,0x32,0x1a,0xe3,0x69}};
static const GUID G_DXGIFactory2={0x50c83a1c,0xe072,0x4c48,{0x87,0xb0,0x36,0x30,0xfa,0x36,0xa6,0xd0}};
static const GUID G_DXGISwapChain2={0xa8be2ac4,0x199f,0x4946,{0xb3,0x31,0x79,0x59,0x9f,0xb9,0x8d,0xe7}};

/* Frame pacing: a flip-model swap chain with a frame-latency waitable object and a maximum latency
 * of ONE frame. The display thread waits on it before taking a frame, so it presents at most once
 * per monitor refresh, always the newest finished game frame, and the game (which waits until its
 * frame has been taken) runs exactly one frame per refresh - like the original's vsync-locked Flip.
 * Without it, Present returned at once with up to three frames queued, the game ran ahead in uneven
 * bursts, and small screen writes (cursor) caused extra presents of the same frame: judder. */
static HANDLE g_waitable;
static UINT g_sc_flags;
static int g_have_slot;        /* waited on g_waitable and not yet presented */

static CRITICAL_SECTION g_cs;
static HANDLE g_thread, g_wake, g_shown;
static volatile LONG g_dirty, g_quit;
static DDraw *volatile g_target;

/* display-thread state */
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain *g_sc;
static HWND g_sc_hwnd;
static int g_sc_w, g_sc_h, g_failed;
static int g_gpu;              /* gpu.c shaders ready: scale on the graphics card */
static uint32_t *g_conv;
static BYTE *g_idx; static size_t g_idx_cap;   /* gameplay: the view's 8-bit pixels for the GPU palette pass */
static int *g_xmap;          /* output column -> source column (snapshot) */
static int g_xmap_cap;
static size_t g_conv_cap;

void present_init(void) { InitializeCriticalSection(&g_cs); }
void present_lock(void) { EnterCriticalSection(&g_cs); }
void present_unlock(void) { LeaveCriticalSection(&g_cs); }

int gpu_init(ID3D11Device *dev);
void gpu_set_indexed(const BYTE *idx, const uint32_t *lut, const BYTE *mask, int mask_lo, int mask_hi, const RECT *rects, int nrects);
int gpu_indexed_ready(int w, int h);
int gpu_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *bb, int cw, int ch,
             const uint32_t *pix, int w, int h, RECT dest, int video, int ambient, int hd,
             int hd_n, const HdOp *hd_ops, const BYTE *hd_idmap, int world,
             int sub_on, float subx, float suby, float zoom, float zcx, float zcy, const TargetZones *zones);
void gpu_release(void);

static void d3d_release(void)
{
    gpu_release();
    g_gpu = 0;
    if (g_waitable) { CloseHandle(g_waitable); g_waitable = NULL; }
    g_have_slot = 0;
    if (g_sc) { IDXGISwapChain_Release(g_sc); g_sc = NULL; }
    if (g_ctx) { ID3D11DeviceContext_Release(g_ctx); g_ctx = NULL; }
    if (g_dev) { ID3D11Device_Release(g_dev); g_dev = NULL; }
    g_sc_hwnd = NULL;
}

static int make_swapchain(HWND hwnd, int w, int h)
{
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory *f = NULL;
    HRESULT hr = ID3D11Device_QueryInterface(g_dev, &G_DXGIDevice, (void **)&xd);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(xd, &ad);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &G_DXGIFactory, (void **)&f);
    IDXGIFactory2 *f2 = NULL;
    if (SUCCEEDED(hr) && SUCCEEDED(IDXGIFactory_QueryInterface(f, &G_DXGIFactory2, (void **)&f2))) {
        DXGI_SWAP_CHAIN_DESC1 d1 = { 0 };
        d1.Width = w; d1.Height = h;
        d1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d1.SampleDesc.Count = 1;
        d1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d1.BufferCount = 2;
        d1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d1.Scaling = DXGI_SCALING_STRETCH;
        d1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        IDXGISwapChain1 *sc1 = NULL;
        HRESULT h2 = IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown *)g_dev, hwnd, &d1, NULL, NULL, &sc1);
        if (SUCCEEDED(h2)) {
            IDXGISwapChain2 *sc2 = NULL;
            if (SUCCEEDED(IDXGISwapChain1_QueryInterface(sc1, &G_DXGISwapChain2, (void **)&sc2))) {
                IDXGISwapChain2_SetMaximumFrameLatency(sc2, 1);
                g_waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
                IDXGISwapChain2_Release(sc2);
            }
            g_sc = (IDXGISwapChain *)sc1;
            g_sc_flags = d1.Flags;
            g_have_slot = 0;
            IDXGIFactory_MakeWindowAssociation(f, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            mlog("swap chain: flip model, frame latency 1, %s", g_waitable ? "paced by the frame-latency wait" : "no waitable object");
        } else mlog("waitable swap chain failed (%#lx), using the plain flip model", h2);
        IDXGIFactory2_Release(f2);
    }
    if (SUCCEEDED(hr) && !g_sc) {
        g_sc_flags = 0;
        DXGI_SWAP_CHAIN_DESC d = { 0 };
        d.BufferDesc.Width = w;
        d.BufferDesc.Height = h;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.OutputWindow = hwnd;
        d.Windowed = TRUE;
        d.BufferCount = 2;
        d.SwapEffect = (DXGI_SWAP_EFFECT)4;   /* FLIP_DISCARD (Windows 10+) */
        hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)g_dev, &d, &g_sc);
        if (FAILED(hr)) {
            mlog("flip-model swap chain failed (%#lx), using legacy blit model", hr);
            d.BufferCount = 1;
            d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)g_dev, &d, &g_sc);
        }
        if (SUCCEEDED(hr)) IDXGIFactory_MakeWindowAssociation(f, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    if (f) IDXGIFactory_Release(f);
    if (ad) IDXGIAdapter_Release(ad);
    if (xd) IDXGIDevice_Release(xd);
    if (FAILED(hr)) { mlog("CreateSwapChain failed %#lx", hr); return 0; }
    g_sc_hwnd = hwnd; g_sc_w = w; g_sc_h = h;
    mlog("swap chain %dx%d on hwnd %p", w, h, hwnd);
    return 1;
}

static int ensure(HWND hwnd, int w, int h)
{
    if (g_failed) return 0;
    if (g_sc && g_sc_hwnd != hwnd) {
        if (g_waitable) { CloseHandle(g_waitable); g_waitable = NULL; }
        g_have_slot = 0;
        IDXGISwapChain_Release(g_sc); g_sc = NULL;
    }
    if (!g_dev) {
        D3D_DRIVER_TYPE types[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
        HRESULT hr = E_FAIL;
        for (int i = 0; i < 2 && FAILED(hr); i++)
            hr = D3D11CreateDevice(NULL, types[i], NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                                   D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx);
        if (FAILED(hr)) { mlog("D3D11CreateDevice failed %#lx", hr); g_failed = 1; return 0; }
        mlog("Direct3D 11 device created");
        g_gpu = gpu_init(g_dev);
    }
    if (!g_sc) return make_swapchain(hwnd, w, h);
    if (w != g_sc_w || h != g_sc_h) {
        HRESULT hr = IDXGISwapChain_ResizeBuffers(g_sc, 0, w, h, DXGI_FORMAT_UNKNOWN, g_sc_flags);
        if (FAILED(hr)) { mlog("ResizeBuffers failed %#lx", hr); return 0; }
        g_sc_w = w; g_sc_h = h;
    }
    return 1;
}

/* One frame ready to show. CPU path: g_conv holds ow x oh pixels that go at ox,oy in a cw x ch
 * window. GPU path (gpu = 1): g_conv holds the game picture (w x h), drawn by gpu.c at dest. */
typedef struct {
    HWND hwnd; int cw, ch, ow, oh, ox, oy;
    int gpu, w, h, video, ambient, hd;
    int hd_n; const HdOp *hd_ops; const BYTE *hd_idmap;   /* upscaled pictures (hdsprites.c) */
    const uint32_t *wref;                                  /* upscaled battlefield (hdworld.c), or NULL */
    int wtake;                                             /* hdworld_take succeeded: build wref after the lock */
    float subx, suby; int sub_on;                         /* sub-pixel scroll of the battlefield (controls.c) */
    float zoom, zcx, zcy;                                 /* battlefield zoom (zoom.c): factor, centre in game pixels */
    TargetZones zones;                                          /* target paint (objectives_zones) */
    int indexed; uint32_t lut[256];                       /* gameplay: g_idx + palette, coloured on the GPU (ps_palette) */
    const BYTE *mask; int mask_lo, mask_hi;               /* interface mask for the GPU and its changed rows */
    RECT rects[3]; int nrects;                            /* our boxes, interface too (objectives, tip, panel) */
    RECT dest;
} Frame;

/* Which game pixels are interface (HUD panel, corner zones, spare-weapon column, our boxes): they
 * stay still while the battlefield is shifted by the sub-pixel scroll or zoomed.
 * The per-pixel test is slow (~250 ns), so: only the bottom band can hold interface (panel rows x the
 * largest corner scale, 3); the band is built in full once per size, then refreshed a budget of pixels
 * per frame; and all of it runs in the display thread BEFORE it takes the present lock (mask_tick) -
 * it used to run under the lock, where the game waited for it: 6 ms a frame at 1024x540, ~25 ms at
 * 2048x1080 (fullscreen ran very slowly). */
#define MASK_BAND   (115 * 3 + 4)       /* design panel rows (480 - 365) x max corner scale, + slack */
#define MASK_BUDGET 20000               /* pixels re-tested per frame */
static BYTE *g_hudmask; static int g_mask_w, g_mask_h, g_mask_y0, g_mask_next, g_mask_ready;
static int g_mask_want_w, g_mask_want_h;
/* rows of the mask that changed since gpu.c last uploaded it (lo >= hi = none) */
static int g_mask_dirty_lo = 1 << 30, g_mask_dirty_hi = 0;
static void mask_rows(int y0, int y1)
{
    static BYTE *tmp; static int tmp_w;
    if (tmp_w < g_mask_w) { free(tmp); tmp = malloc(g_mask_w); tmp_w = tmp ? g_mask_w : 0; }
    if (!tmp) return;
    for (int y = y0; y < y1; y++) {
        BYTE *row = g_hudmask + (size_t)y * g_mask_w;
        /* exact per pixel (layout.c, 2026-11-17): the panel art's see-through parts are battlefield (they
         * zoom with it); was whole rectangles, which left unzoomed patches around the enlarged HUD */
        layout_hud_mask_row(y, tmp, g_mask_w);
        int changed = memcmp(row, tmp, g_mask_w) != 0;
        if (changed) memcpy(row, tmp, g_mask_w);
        if (changed) {
            if (y < g_mask_dirty_lo) g_mask_dirty_lo = y;
            if (y + 1 > g_mask_dirty_hi) g_mask_dirty_hi = y + 1;
        }
    }
}
static void mask_tick(void)                 /* display thread, outside the present lock */
{
    int w = g_mask_want_w, h = g_mask_want_h;
    if (w <= 0 || h <= 0) return;
    if (!g_hudmask || g_mask_w != w || g_mask_h != h) {
        g_mask_ready = 0;
        free(g_hudmask); g_hudmask = calloc((size_t)w * h, 1);
        if (!g_hudmask) { g_mask_w = g_mask_h = 0; return; }
        g_mask_w = w; g_mask_h = h;
        g_mask_y0 = h > MASK_BAND ? h - MASK_BAND : 0;
        mask_rows(g_mask_y0, h);
        g_mask_dirty_lo = 0; g_mask_dirty_hi = h;         /* new size: gpu.c uploads it all */
        g_mask_next = g_mask_y0;
        g_mask_ready = 1;
        return;
    }
    static unsigned sig;                     /* the HUD layout changed (a size animating): the whole band now */
    unsigned s = layout_hud_signature();
    if (s != sig) { sig = s; mask_rows(g_mask_y0, h); g_mask_next = g_mask_y0; return; }
    int rows = MASK_BUDGET / w; if (rows < 1) rows = 1;
    int y1 = g_mask_next + rows; if (y1 > h) y1 = h;
    mask_rows(g_mask_next, y1);
    g_mask_next = y1 >= h ? g_mask_y0 : y1;
}
static const BYTE *hud_mask(int w, int h)   /* under the lock: the mask if it is ready for this size */
{
    g_mask_want_w = w; g_mask_want_h = h;
    return g_mask_ready && g_mask_w == w && g_mask_h == h ? g_hudmask : NULL;
}


/* Performance figures (log=1: every 10 s in gameplay): where a frame's time goes. Stages in ms,
 * averaged: game = time between the game's frames, wait = of that spent waiting for the display,
 * snap = the display thread's copy under the lock (conv / mask / hdworld / hdsprites), draw = gpu
 * calls + Present. */
enum { PF_GAME, PF_WAIT, PF_REFS, PF_SNAP, PF_CONV, PF_MASK, PF_WORLD, PF_SPR, PF_DRAW, PF_N };
static double g_pf[PF_N]; static int g_pf_n[PF_N];
static double pf_now(void)
{
    static LARGE_INTEGER fq; LARGE_INTEGER t;
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)fq.QuadPart;
}
static void pf_add(int k, double ms) { g_pf[k] += ms; g_pf_n[k]++; }
static volatile double g_t_slot, g_t_snap, g_t_draw, g_t_gpu, g_t_present;   /* display thread, last frame (timing trace) */
extern volatile double g_gt_wget, g_gt_wup, g_gt_atlas; extern volatile int g_gt_dirty_px, g_gt_move_px;   /* gpu.c */
static volatile LONGLONG g_prof[PR_N];                  /* game thread: QPC ticks per hook this frame (trace) */
void prof_add(int k, LONGLONG ticks) { if (k >= 0 && k < PR_N) g_prof[k] += ticks; }
static double g_prof_frame[PR_N];                      /* ... of the last finished frame, in ms */
static double g_prof_sum[PR_N]; static int g_prof_sum_n;   /* ... summed over gameplay frames for the 10 s perf line (2026-11-20) */
static volatile LONGLONG g_refs_ticks;                 /* game thread: QPC ticks in hdworld_note_copy this frame */
void present_perf_refs(LONGLONG ticks) { g_refs_ticks += ticks; }
static void pf_report(int w, int h)
{
    static double last;
    double now = pf_now();
    if (!last) { last = now; return; }
    if (now - last < 10000.0 || !g_cfg.log || !layout_view_is_gameplay()) return;
    last = now;
    static const char *nm[PF_N] = { "game", "wait", "refs", "snap", "conv", "mask", "hdworld", "hdspr", "draw" };
    char line[400]; int o = wsprintfA(line, "perf %dx%d:", w, h);
    for (int k = 0; k < PF_N; k++) {
        double a = g_pf_n[k] ? g_pf[k] / g_pf_n[k] : 0;
        o += wsprintfA(line + o, " %s %d.%d", nm[k], (int)a, (int)(a * 10) % 10);
        g_pf[k] = 0; g_pf_n[k] = 0;
    }
    mlog("%s ms", line);
    if (g_prof_sum_n) {                        /* the game thread's mod stages, averaged (spike lines show only the worst frames) */
        static const char *const PN[PR_N] = { "ctl", "regions", "cam", "field", "cells", "blit", "ghost", "hud", "overlay", "shown",
                                              "h.weap", "h.modern", "h.obj", "h.corners", "h.minimap", "h.rest" };
        o = wsprintfA(line, "perf game stages (avg ms over %d frames):", g_prof_sum_n);
        for (int k = 0; k < PR_N; k++) { double a = g_prof_sum[k] / g_prof_sum_n; o += wsprintfA(line + o, " %s %d.%02d", PN[k], (int)a, (int)(a * 100) % 100); g_prof_sum[k] = 0; }
        g_prof_sum_n = 0;
        mlog("%s", line);
    }
}

/* Display timing, written to the log while the timing trace runs (Ctrl+Shift+T): once a second the
 * number of presents, the time between them, and how many came late (> 1.5 refreshes apart). */
static double g_refresh_ms = 1000.0 / 60.0;
static void timing_note(void)
{
    static LARGE_INTEGER last, fq, sec;
    static int n, late; static double worst, sum;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    if (!clock_tracing()) { last.QuadPart = 0; sec.QuadPart = 0; n = late = 0; worst = sum = 0; return; }
    if (!sec.QuadPart) {
        DEVMODEA dm = { 0 }; dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 24)
            g_refresh_ms = 1000.0 / dm.dmDisplayFrequency;
        sec = now;
    }
    if (last.QuadPart) {
        double dt = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)fq.QuadPart;
        n++; sum += dt; if (dt > worst) worst = dt;
        if (dt > 1.5 * g_refresh_ms) late++;
    }
    last = now;
    if ((double)(now.QuadPart - sec.QuadPart) / (double)fq.QuadPart >= 1.0) {
        mlog("display: %d presents/s, avg %d.%d ms, worst %d.%d ms, late %d (refresh %d.%d ms, %s)", n,
             (int)(n ? sum / n : 0), (int)(n ? sum / n * 10 : 0) % 10, (int)worst, (int)(worst * 10) % 10, late,
             (int)g_refresh_ms, (int)(g_refresh_ms * 10) % 10, g_waitable ? "paced" : "not paced");
        n = late = 0; worst = sum = 0; sec = now;
    }
}

static void mark_rect(uint32_t *pix, int w, int h, RECT r)
{
    for (int y = r.top < 0 ? 0 : r.top; y < r.bottom && y < h; y++)
        for (int x = r.left < 0 ? 0 : r.left; x < r.right && x < w; x++) pix[(size_t)y * w + x] &= 0x00ffffff;
}

static int *g_xfrac;          /* per output column: blend weight 0..256 toward the next source column */
static int g_xfrac_cap;

static inline uint32_t mix(uint32_t a, uint32_t b, int w)      /* w = 0..256 toward b */
{
    uint32_t rb = ((a & 0xFF00FF) * (256 - w) + (b & 0xFF00FF) * w) >> 8 & 0xFF00FF;
    uint32_t g = ((a & 0x00FF00) * (256 - w) + (b & 0x00FF00) * w) >> 8 & 0x00FF00;
    return 0xFF000000u | rb | g;
}

static inline uint32_t src_px(const FrameSource *s, int x, int y)
{
    return s->lut[s->bits[(size_t)y * s->pitch + x]];
}

/* Window smaller than the picture: each output pixel = the average of the game pixels it covers. */
static void shrink(Frame *f, const FrameSource *s, int sx0, int sy0, int sw, int sh, int dw, int dh, int dx0, int dy0)
{
    for (int x = 0; x <= f->ow; x++)                 /* column x covers source columns g_xmap[x] .. g_xmap[x+1]-1 */
        g_xmap[x] = (int)((long long)(f->ox + x - dx0) * sw / dw);
    for (int y = 0; y < f->oh; y++) {
        int ya = (int)((long long)(f->oy + y - dy0) * sh / dh);
        int yb = (int)((long long)(f->oy + y + 1 - dy0) * sh / dh);
        if (yb <= ya) yb = ya + 1;
        if (yb > sh) yb = sh;
        uint32_t *row = g_conv + (size_t)y * f->ow;
        for (int x = 0; x < f->ow; x++) {
            int xa = g_xmap[x], xb = g_xmap[x + 1];
            if (xb <= xa) xb = xa + 1;
            if (xb > sw) xb = sw;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = ya; yy < yb; yy++)
                for (int xx = xa; xx < xb; xx++) {
                    uint32_t c = src_px(s, sx0 + xx, sy0 + yy);
                    r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; n++;
                }
            row[x] = n ? 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n) : 0xFF000000u;
        }
    }
}

/* Source position and blend weight for output pixel o (o = 0 at the start of the destination).
 * SMOOTH_OFF: nearest game pixel (square pixels, the game's look); SMOOTH_FULL: bilinear (videos). */
static void map_axis(int o, int src_len, int dst_len, int mode, int *i0, int *w)
{
    if (mode == SMOOTH_OFF) {
        int i = (int)((long long)o * src_len / dst_len);
        *i0 = i < src_len ? i : src_len - 1; *w = 0;
        return;
    }
    double t = (o + 0.5) * src_len / dst_len - 0.5;   /* position in game pixels */
    if (t < 0) t = 0;
    if (t > src_len - 1) t = src_len - 1;
    int i = (int)t;
    *i0 = i;
    *w = (int)((t - i) * 256 + 0.5);
}

/* Under the lock: convert the view (view.c) to 32-bit colour at the size it is shown in the
 * window, keeping only the part that is inside the window. */
static int snapshot(Frame *f)
{
    FrameSource s;
    if (!g_target || !dd_frame_source(g_target, &s)) return 0;
    RECT cr;
    if (!GetClientRect(s.hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return 0;
    ViewLayout L = view_layout(cr.right, cr.bottom);
    RECT vis;
    if (!IntersectRect(&vis, &L.dest, &cr)) return 0;

    f->hwnd = s.hwnd; f->cw = cr.right; f->ch = cr.bottom;
    f->ox = vis.left; f->oy = vis.top;
    f->ow = vis.right - vis.left; f->oh = vis.bottom - vis.top;

    f->gpu = g_gpu;
    if (f->gpu) {
        /* graphics card path: only the game picture is converted here; gpu.c scales it */
        int w = L.view.right - L.view.left, h = L.view.bottom - L.view.top;
        size_t n = (size_t)w * h;
        if (n > g_conv_cap) {
            free(g_conv);
            g_conv = malloc(n * 4);
            g_conv_cap = g_conv ? n : 0;
            if (!g_conv) return 0;
        }
        double t0 = pf_now();
        /* sub-pixel scroll (gameplay): alpha 255 = battlefield (shifted), 0 = interface (still) */
        int whole = !L.video && layout_view_is_gameplay() && L.view.left == 0 && L.view.top == 0;
        /* gameplay: the 8-bit picture goes to the GPU as it is (gpu.c ps_palette looks the colours up and
         * applies the interface mask) - 1 byte per pixel instead of 4, and no conversion loop here */
        f->indexed = whole && gpu_indexed_ready(w, h);    /* first frame / new size: colours on the CPU once */
        if (f->indexed) {
            if (n > g_idx_cap) { free(g_idx); g_idx = malloc(n); g_idx_cap = g_idx ? n : 0; if (!g_idx) return 0; }
            for (int y = 0; y < h; y++) memcpy(g_idx + (size_t)y * w, s.bits + (size_t)y * s.pitch, w);
            memcpy(f->lut, s.lut, sizeof f->lut);
        } else
        for (int y = 0; y < h; y++) {
            const BYTE *src = s.bits + (size_t)(L.view.top + y) * s.pitch + L.view.left;
            uint32_t *row = g_conv + (size_t)y * w;
            for (int x = 0; x < w; x++) row[x] = s.lut[src[x]];
        }
        int sub = whole && controls_shown_subpx(&f->subx, &f->suby) && (f->subx != 0 || f->suby != 0);
        if (!sub) f->subx = f->suby = 0;
        double t1 = pf_now(); pf_add(PF_CONV, t1 - t0);
        float z = zoom_factor();                    /* zoom.c: battlefield zoom (eased) */
        f->zoom = whole && z > 1.001f ? z : 1.0f;
        zoom_centre(&f->zcx, &f->zcy);
        if (whole) objectives_zones(&f->zones); else { f->zones.n = f->zones.nsec = f->zones.nspot = 0; f->zones.dim_on = 0; f->zones.in_s = 0; }
        {   /* a menu / pop-up drawn straight onto the screen (hud.c): ghost and route pixels are byte-checked again */
            f->zones.npanel = layout_open_panels(f->zones.panel, 4);
            f->zones.menu_over = game_menu_up() || f->zones.npanel > 0;
        }
        f->sub_on = sub || f->zoom > 1.0f;          /* the battlefield is read moved: needs the interface mask */
        if (f->sub_on && f->indexed) {               /* the GPU applies the mask (gpu.c ps_palette) */
            const BYTE *mask = hud_mask(w, h);
            if (!mask) { f->sub_on = 0; f->zoom = 1.0f; }
            else {
                f->mask = mask;
                f->mask_lo = g_mask_dirty_lo; f->mask_hi = g_mask_dirty_hi;
                g_mask_dirty_lo = h; g_mask_dirty_hi = 0;
                f->nrects = 0;
                RECT r;
                if (objectives_box(&r)) f->rects[f->nrects++] = r;
                if (tips_rect(&r)) f->rects[f->nrects++] = r;
                if (panel_rect(&r)) f->rects[f->nrects++] = r;
            }
        } else if (f->sub_on) {
            const BYTE *mask = hud_mask(w, h);
            if (!mask) { f->sub_on = 0; f->zoom = 1.0f; }
            else {
                size_t n2 = (size_t)w * h;
                for (size_t i = 0; i < n2; i++) g_conv[i] = (g_conv[i] & 0x00ffffff) | (mask[i] ? 0 : 0xff000000u);
                RECT r;
                if (objectives_box(&r)) mark_rect(g_conv, w, h, r);
                if (tips_rect(&r)) mark_rect(g_conv, w, h, r);
                if (panel_rect(&r)) mark_rect(g_conv, w, h, r);
            }
        }
        double t2 = pf_now(); pf_add(PF_MASK, t2 - t1);
        f->w = w; f->h = h;
        f->dest = L.dest;
        f->video = L.video;
        f->ambient = !layout_view_is_gameplay();      /* menus and videos: fill around them */
        f->hd = hdart_prepare(g_conv, w, h, f->ambient && !L.video);   /* HD menu background (hdart.c) */
        double t3 = pf_now();
        f->hd_n = L.video ? 0 : hdsprites_prepare(s.bits, s.pitch, L.view, &f->hd_ops, &f->hd_idmap);
        double t4 = pf_now(); pf_add(PF_SPR, t4 - t3);
        f->wref = NULL;                                 /* unused: the GPU resolves the references */
        f->wtake = !L.video && layout_view_is_gameplay() && hdworld_take(s.bits, s.pitch, L.view);   /* gpu.c resolves it (hdworld_gpu_inputs) */
        { static int n; if (g_cfg.log >= 2 && n++ % 600 == 0) { RECT vc = view_current(); mlog("present: wtake %d video %d gameplay %d view %ld,%ld,%ld,%ld cfg %dx%d", f->wtake, L.video, layout_view_is_gameplay(), vc.left, vc.top, vc.right, vc.bottom, g_cfg.width, g_cfg.height); } }
        int vw, vh;
        const uint32_t *v = L.video ? hdvideo_frame(&vw, &vh) : NULL;
        if (v && (size_t)vw * vh <= 4096u * 4096u) {        /* upscaled video (hdvideo.c): its frame instead */
            size_t m = (size_t)vw * vh;
            if (m > g_conv_cap) {
                free(g_conv);
                g_conv = malloc(m * 4);
                g_conv_cap = g_conv ? m : 0;
                if (!g_conv) return 0;
            }
            memcpy(g_conv, v, m * 4);
            f->w = vw; f->h = vh;
            int dw = cr.right, dh = (int)((long long)cr.right * vh / vw);   /* fit the window, keep its shape */
            if (dh > cr.bottom) { dh = cr.bottom; dw = (int)((long long)cr.bottom * vw / vh); }
            f->dest = (RECT){ (cr.right - dw) / 2, (cr.bottom - dh) / 2, (cr.right - dw) / 2 + dw, (cr.bottom - dh) / 2 + dh };
            f->hd = 0; f->hd_n = 0;
        }
        return 1;
    }

    size_t need = (size_t)f->ow * f->oh;
    if (need > g_conv_cap) {
        free(g_conv);
        g_conv = malloc(need * 4);
        g_conv_cap = g_conv ? need : 0;
        if (!g_conv) return 0;
    }
    if (f->ow + 1 > g_xmap_cap || f->ow + 1 > g_xfrac_cap) {   /* +1: shrink() reads one past the last column */
        free(g_xmap); free(g_xfrac);
        g_xmap = malloc(sizeof(int) * (size_t)(f->ow + 1));
        g_xfrac = malloc(sizeof(int) * (size_t)(f->ow + 1));
        g_xmap_cap = g_xfrac_cap = g_xmap && g_xfrac ? f->ow + 1 : 0;
        if (!g_xmap_cap) return 0;
    }
    int sx0 = L.view.left, sy0 = L.view.top;
    int sw = L.view.right - L.view.left, sh = L.view.bottom - L.view.top;
    int dw = L.dest.right - L.dest.left, dh = L.dest.bottom - L.dest.top;
    int mode = L.video ? SMOOTH_FULL : SMOOTH_OFF;
    if ((dw < sw || dh < sh) && mode != SMOOTH_OFF) {
        shrink(f, &s, sx0, sy0, sw, sh, dw, dh, L.dest.left, L.dest.top);
        return 1;
    }
    for (int x = 0; x < f->ow; x++) map_axis(f->ox + x - L.dest.left, sw, dw, mode, &g_xmap[x], &g_xfrac[x]);
    for (int y = 0; y < f->oh; y++) {
        int y0, wy;
        map_axis(f->oy + y - L.dest.top, sh, dh, mode, &y0, &wy);
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        const BYTE *r0 = s.bits + (size_t)(sy0 + y0) * s.pitch + sx0;
        const BYTE *r1 = s.bits + (size_t)(sy0 + y1) * s.pitch + sx0;
        uint32_t *row = g_conv + (size_t)y * f->ow;
        for (int x = 0; x < f->ow; x++) {
            int a = g_xmap[x], wx = g_xfrac[x];
            if (!wx && !wy) { row[x] = s.lut[r0[a]]; continue; }
            int b = a + 1 < sw ? a + 1 : a;
            uint32_t top = mix(s.lut[r0[a]], s.lut[r0[b]], wx);
            row[x] = wy ? mix(top, mix(s.lut[r1[a]], s.lut[r1[b]], wx), wy) : top;
        }
    }
    return 1;
}

static void show(const Frame *f)
{
    if (!ensure(f->hwnd, f->cw, f->ch)) return;
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(g_sc, 0, &G_Texture2D, (void **)&bb))) return;
    if (f->gpu) {
        double tg = pf_now();
        gpu_set_indexed(f->indexed ? g_idx : NULL, f->lut, f->sub_on ? f->mask : NULL, f->mask_lo, f->mask_hi, f->rects, f->sub_on ? f->nrects : 0);
        int ok = gpu_draw(g_dev, g_ctx, bb, f->cw, f->ch, g_conv, f->w, f->h, f->dest, f->video, f->ambient, f->hd,
                          f->hd_n, f->hd_ops, f->hd_idmap, f->wtake, f->sub_on, f->subx, f->suby, f->zoom, f->zcx, f->zcy, &f->zones);
        ID3D11Texture2D_Release(bb);
        if (!ok) { g_gpu = 0; mlog("gpu: drawing failed - scaling on the CPU from now on"); return; }
        double tp = pf_now();
        g_t_gpu = tp - tg;                         /* trace: submitting the passes / uploads */
        HRESULT hr = IDXGISwapChain_Present(g_sc, g_cfg.vsync ? 1 : 0, 0);
        g_t_present = pf_now() - tp;               /* trace: Present (waits on the gpu / screen show up here) */
        if (FAILED(hr)) {
            static int warned;
            if (!warned++) mlog("Present failed %#lx", hr);
        }
        return;
    }
    if (f->ow != f->cw || f->oh != f->ch) {
        ID3D11RenderTargetView *rtv = NULL;
        if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb, NULL, &rtv))) {
            const float black[4] = { 0, 0, 0, 1 };
            ID3D11DeviceContext_ClearRenderTargetView(g_ctx, rtv, black);
            ID3D11RenderTargetView_Release(rtv);
        }
    }
    D3D11_BOX box = { (UINT)f->ox, (UINT)f->oy, 0, (UINT)(f->ox + f->ow), (UINT)(f->oy + f->oh), 1 };
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)bb, 0, &box, g_conv, f->ow * 4, 0);
    ID3D11Texture2D_Release(bb);
    HRESULT hr = IDXGISwapChain_Present(g_sc, g_cfg.vsync ? 1 : 0, 0);
    if (FAILED(hr)) {
        static int warned;
        if (!warned++) mlog("Present failed %#lx", hr);
    }
}

static DWORD WINAPI display_thread(LPVOID unused)
{
    mlog("display thread started");
    while (!g_quit) {
        if (!g_dirty) WaitForSingleObject(g_wake, 100);
        if (g_quit) break;
        if (!g_dirty) continue;
        /* take the finished frame FIRST and let the game go on, THEN wait for the refresh slot and draw.
         * Before (2026-11-17 trace on the user's PC) the slot wait came first: the game sat idle through
         * the display's slot wait + draw every frame (work 9 + wait 15 = 24 ms, ~40 fps); now the game's
         * next frame overlaps them. Still one present per refresh: the next take only follows the present. */
        InterlockedExchange(&g_dirty, 0);
        Frame f = { 0 };
        mask_tick();
        double ts = pf_now();
        present_lock();
        int ok = snapshot(&f);
        present_unlock();
        g_t_snap = pf_now() - ts;
        pf_add(PF_SNAP, g_t_snap);
        SetEvent(g_shown);                         /* the game's frame is taken: it may start the next */
        if (ok && g_waitable && !g_have_slot) {    /* the next refresh slot (the previous frame is on screen) */
            double tsl = pf_now();
            WaitForSingleObjectEx(g_waitable, 100, TRUE);
            g_have_slot = 1;
            g_t_slot = pf_now() - tsl;               /* trace: waiting for a free present slot */
        }
        if (g_quit) break;
        if (ok) {
            double td = pf_now();
            show(&f);
            g_t_draw = pf_now() - td;
            pf_add(PF_DRAW, g_t_draw);
            pf_report(f.w, f.h);
            g_have_slot = 0;
            timing_note();
        }
    }
    d3d_release();
    mlog("display thread stopped");
    return 0;
}

void present_request(DDraw *dd, int wait)
{
    g_target = dd;
    if (wait && g_thread) ResetEvent(g_shown);
    InterlockedExchange(&g_dirty, 1);
    if (!g_thread) {
        g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_shown = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_quit = 0;
        g_thread = CreateThread(NULL, 0, display_thread, NULL, 0, NULL);
    }
    SetEvent(g_wake);
    /* end of a game frame: wait (bounded) until it is on screen. This paces the game to the
     * monitor like the original vsync-locked Flip did. */
    static double t_frame, t_refs, t_wait, t_wait_prev;   /* this frame, for the timing trace */
    if (wait) {
        static double prev;
        double tw = pf_now();
        t_frame = prev ? tw - prev : 0;
        static LARGE_INTEGER fq; if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        t_refs = (double)g_refs_ticks * 1000.0 / (double)fq.QuadPart;
        if (prev) { pf_add(PF_GAME, t_frame); pf_add(PF_REFS, t_refs); }
        g_refs_ticks = 0;
        for (int k = 0; k < PR_N; k++) { g_prof_frame[k] = (double)g_prof[k] * 1000.0 / (double)fq.QuadPart; g_prof[k] = 0; }
        if (prev && layout_view_is_gameplay()) { for (int k = 0; k < PR_N; k++) g_prof_sum[k] += g_prof_frame[k]; g_prof_sum_n++; }
        prev = tw;
    }
    t_wait_prev = t_wait; t_wait = 0;          /* t_frame spans the previous frame's wait */
    if (wait && g_cfg.vsync) { double tw = pf_now(); WaitForSingleObject(g_shown, 50); t_wait = pf_now() - tw; pf_add(PF_WAIT, t_wait); }
    else if (wait && g_cfg.fps_limit > 0) {
        /* [display] fps_limit with vsync off (2026-11-17): frames at a fixed rate, e.g. 120 on a 60 Hz monitor
         * (tearing) - tests what a 120 Hz monitor does. The game clock steps by the same period (clock.c).
         * Sleep(1) while more than 2 ms are left, then spin: Sleep alone is too coarse at 8 ms frames. */
        static double next;
        static int timer_set;
        if (!timer_set) {                               /* 1 ms Sleep granularity (default ~15.6 ms) */
            timer_set = 1;
            HMODULE wm = LoadLibraryA("winmm.dll");
            UINT (WINAPI *begin)(UINT) = wm ? (UINT (WINAPI *)(UINT))GetProcAddress(wm, "timeBeginPeriod") : NULL;
            if (begin) begin(1);
            mlog("present: frames capped at %d per second (vsync off)", g_cfg.fps_limit);
        }
        double period = 1000.0 / g_cfg.fps_limit, tw = pf_now();
        if (!next || tw > next + period) next = tw;     /* late (or first): start over from now */
        while (pf_now() < next - 2.0) Sleep(1);
        while (pf_now() < next) YieldProcessor();
        next += period;
        t_wait = pf_now() - tw; pf_add(PF_WAIT, t_wait);
    }
    if (wait) {
        clock_frame_shown(layout_view_is_gameplay());    /* game clock: one refresh per shown frame */
        /* SPIKES are always logged (2026-11-20, user: stutter after the 30 s trace had ended): a gameplay frame
         * longer than [debug] spike_ms (default 40) writes the same trace line, prefixed "spike", at most 4 a
         * second. The stage times (ctl .. shown) are the mod's; work minus their sum is the game's own code. */
        static int spike_ms = -1;
        if (spike_ms < 0) spike_ms = GetPrivateProfileIntA("debug", "spike_ms", 40, mod_ini_path());
        static DWORD spike_sec, spike_n;
        int spike = 0;
        if (spike_ms > 0 && t_frame > spike_ms && layout_view_is_gameplay()) {
            DWORD sec = GetTickCount() / 1000;
            if (sec != spike_sec) { spike_sec = sec; spike_n = 0; }
            spike = spike_n++ < 4;
        }
        if (clock_tracing() || spike) {
            LARGE_INTEGER now, fq;
            QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
            char info[160], line[600];
            controls_trace_info(info, sizeof info);
            /* frame = since the last frame; work = the game's own (frame - its wait); refs = our reference copying;
             * wait = for the display to take the frame; display thread: slot = waiting for a free present slot,
             * snap = copy under the lock, draw = gpu + Present (all ms, x10) */
            wsprintfA(line, "real %d.%03d game %lu | frame %d work %d refs %d wait %d | slot %d snap %d draw %d (gpu %d present %d) (x0.1 ms) | %s",
                      (int)(now.QuadPart * 1000 / fq.QuadPart % 100000), (int)(now.QuadPart * 1000000 / fq.QuadPart % 1000),
                      clock_game_ms(), (int)(t_frame * 10), (int)((t_frame - t_wait_prev) * 10), (int)(t_refs * 10), (int)(t_wait * 10),
                      (int)(g_t_slot * 10), (int)(g_t_snap * 10), (int)(g_t_draw * 10), (int)(g_t_gpu * 10), (int)(g_t_present * 10), info);
            {                                        /* where the game thread's work went (x0.1 ms) */
                static const char *const PN[PR_N] = { "ctl", "regions", "cam", "field", "cells", "blit", "ghost", "hud", "overlay", "shown",
                                                      "h.weap", "h.modern", "h.obj", "h.corners", "h.minimap", "h.rest" };
                int o = lstrlenA(line);
                o += wsprintfA(line + o, " | wget %d wup %d atlas %d mapKpx %d movedKpx %d", (int)(g_gt_wget * 10), (int)(g_gt_wup * 10), (int)(g_gt_atlas * 10), g_gt_dirty_px / 1000, g_gt_move_px / 1000);
                o += wsprintfA(line + o, " |");
                for (int k = 0; k < PR_N && o < 560; k++) o += wsprintfA(line + o, " %s %d", PN[k], (int)(g_prof_frame[k] * 10));
            }
            if (clock_tracing()) clock_trace_frame(line); else mlog("spike: %s", line);
        }
    }
}

void present_shutdown(void)
{
    if (!g_thread) return;
    g_quit = 1;
    SetEvent(g_wake);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    CloseHandle(g_wake);
    CloseHandle(g_shown);
    g_thread = g_wake = g_shown = NULL;
    g_target = NULL;
}
