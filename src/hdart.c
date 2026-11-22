/* ============================================================================================
 * hdart.c - widescreen HD menu backgrounds
 *
 * The menu screens are 640x480 pictures: a background (Screens\*.BMP) with the menu panel, text,
 * buttons and cursor drawn over it by the game. Wide HD versions of the 10 backgrounds (16:9,
 * any size, made from 854x480 extensions of the originals) can be put in <game>\widescreen\ with the
 * original file names as .png. Then, for every menu frame:
 *   1. the background is recognised by comparing a grid of samples with the original BMPs;
 *   2. every pixel that still shows the background gets alpha 0 in the converted frame (the game
 *      drew nothing there), every other pixel (panel, text, cursor) keeps alpha 255;
 *   3. gpu.c draws the HD picture over the whole 16:9 area and the game's pixels on top where
 *      alpha is 255 (ps_hd). The 640x480 picture is the centre of the 854x480-equivalent HD picture.
 * Two classes of picture (2026-11-21, one rule each, the same for every terrain):
 *   - pure art (Title_legal, title2, *_load): the wide picture as it is, across the whole width.
 *   - pictures with menu panels painted in (*_misn_select, multihost, multiremote): the game's buttons,
 *     labels and clicks sit on the ORIGINAL panels, but every AI-widened picture moved them (measured
 *     per 64 px tile against the original: alp 1.03x wider, edges -6..+8 px; bay -6..+1 / -4; des
 *     -16..+6 / -12; multi* up to 16 px). So the centre of the HD picture is rebuilt once at load
 *     (compose_centre): the faithful 2x upscale hd\Screens\<name>.bmp (or the original BMP when it is
 *     missing), resampled once (Lanczos-3) to the centre's size, blended into the wide sides over
 *     SEAM_PX game px at both centre edges (no hard cut). Panels / frames are then the original's
 *     geometry at HD resolution, and no original 1x pixel shows where the art matches.
 * PNGs are loaded with WIC (Windows Imaging Component). [display] hd_menus = 1
 * ============================================================================================ */
#define COBJMACROS
#include "common.h"
#include <math.h>
#include <wincodec.h>

static const char *const NAMES[] = { "Title_legal", "alp_load", "alp_misn_select", "bay_load", "bay_misn_select",
                                     "des_load", "des_misn_select", "multihost", "multiremote", "title2" };
#define COUNT ((int)(sizeof NAMES / sizeof NAMES[0]))
#define PW 640
#define PH 480
#define TOL 12                    /* summed colour difference that still counts as "the background" */
#define MATCH_MIN 40              /* % of the background that must show: win/lose radios cover ~44 % */

typedef struct { uint32_t *orig; uint32_t *hd; int hw, hh; int tried_hd; int panels; } Art;
static void compose_centre(Art *a, const char *name);
#define SEAM_PX 32                /* game px at each centre edge where the wide sides fade into the rebuilt centre */
static Art g_art[COUNT];
static int g_loaded_orig, g_current = -1;
static int g_serial;              /* changes when the current HD picture changes (gpu.c re-uploads) */
static volatile int g_active;     /* the last menu frame used an HD background */

int hdart_active(void) { return g_active; }

static const GUID CLSID_WicFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID IID_WicFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID FMT_BGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

void hdart_game_path(char *out, int n, const char *rel)
{
    GetModuleFileNameA(NULL, out, n);
    char *s = strrchr(out, '\\');
    if (s) s[1] = 0;
    lstrcatA(out, rel);
}

/* An 8-bit BMP as 0xAARRGGBB pixels (top-down), or NULL. */
static uint32_t *load_bmp8(const char *path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    BYTE *d = size > 1078 && size < 8u << 20 ? malloc(size) : NULL;
    if (d && (!ReadFile(f, d, size, &got, NULL) || got != size)) { free(d); d = NULL; }
    CloseHandle(f);
    if (!d) return NULL;
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)(d + 14);
    DWORD off = *(DWORD *)(d + 10);
    uint32_t *out = NULL;
    if (d[0] == 'B' && d[1] == 'M' && bi->biBitCount == 8 && bi->biWidth == PW && labs(bi->biHeight) == PH && !bi->biCompression) {
        RGBQUAD *pal = (RGBQUAD *)((BYTE *)bi + bi->biSize);
        int stride = (PW + 3) & ~3;
        if (off + (DWORD)stride * PH <= size && (out = malloc(PW * PH * 4)))
            for (int y = 0; y < PH; y++) {
                const BYTE *row = d + off + (size_t)(bi->biHeight > 0 ? PH - 1 - y : y) * stride;
                for (int x = 0; x < PW; x++) {
                    RGBQUAD c = pal[row[x]];
                    out[y * PW + x] = 0xFF000000u | (uint32_t)c.rgbRed << 16 | (uint32_t)c.rgbGreen << 8 | c.rgbBlue;
                }
            }
    }
    free(d);
    return out;
}

/* A PNG (any format WIC reads) as BGRA, or NULL. */
static uint32_t *load_image(const char *path, int *w, int *h, int keep_alpha)
{
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frm = NULL;
    IWICFormatConverter *conv = NULL;
    uint32_t *out = NULL;
    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    if (SUCCEEDED(CoCreateInstance(&CLSID_WicFactory, NULL, CLSCTX_INPROC_SERVER, &IID_WicFactory, (void **)&fac))
        && SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(fac, wpath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frm))
        && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(fac, &conv))
        && SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frm, &FMT_BGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        IWICFormatConverter_GetSize(conv, &uw, &uh);
        if (uw && uh && uw <= 8192 && uh <= 8192 && (out = malloc((size_t)uw * uh * 4))) {
            if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, uw * 4, uw * uh * 4, (BYTE *)out))) { free(out); out = NULL; }
            else {
                *w = uw; *h = uh;
                if (!keep_alpha) for (size_t i = 0; i < (size_t)uw * uh; i++) out[i] |= 0xFF000000u;
                else for (size_t i = 0; i < (size_t)uw * uh; i++) {          /* premultiplied: soft edges blend without dark fringes */
                    uint32_t p = out[i], a = p >> 24;
                    out[i] = a << 24 | ((p >> 16 & 255) * a / 255) << 16 | ((p >> 8 & 255) * a / 255) << 8 | ((p & 255) * a / 255);
                }
            }
        }
    }
    if (conv) IWICFormatConverter_Release(conv);
    if (frm) IWICBitmapFrameDecode_Release(frm);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (fac) IWICImagingFactory_Release(fac);
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

/* Opaque (menus, BMPs: their alpha byte is not reliable). */
uint32_t *hdart_load_image(const char *path, int *w, int *h) { return load_image(path, w, h, 0); }
/* With the file's transparency, premultiplied (2026-11-18: hdsprites' HUD cuts are see-through PNGs; forced opaque
 * their transparent pixels were solid BLACK - the black boxes on the HUD). PNG only. */
uint32_t *hdart_load_image_alpha(const char *path, int *w, int *h)
{
    const char *dot = strrchr(path, '.');
    return load_image(path, w, h, dot && !lstrcmpiA(dot, ".png"));
}

static void load_originals(void)
{
    g_loaded_orig = 1;
    char path[MAX_PATH], rel[96];
    int n = 0;
    for (int i = 0; i < COUNT; i++) {
        wsprintfA(rel, "widescreen\\%s.png", NAMES[i]);
        hdart_game_path(path, MAX_PATH, rel);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;   /* no HD version: skip */
        wsprintfA(rel, "Screens\\%s.bmp", NAMES[i]);
        hdart_game_path(path, MAX_PATH, rel);
        if ((g_art[i].orig = load_bmp8(path))) n++;
        /* menu panels painted into the art: the centre is rebuilt from the original's geometry (compose_centre) */
        g_art[i].panels = strstr(NAMES[i], "misn_select") || strstr(NAMES[i], "multi");
    }
    mlog("hdart: %d menu backgrounds have a widescreen version", n);
}

static int close_enough(uint32_t a, uint32_t b)
{
    int dr = (int)(a >> 16 & 255) - (int)(b >> 16 & 255), dg = (int)(a >> 8 & 255) - (int)(b >> 8 & 255), db = (int)(a & 255) - (int)(b & 255);
    return abs(dr) + abs(dg) + abs(db) <= TOL;
}

/* Share of a sample grid where the frame shows background i. */
static int match_percent(const uint32_t *pix, int i)
{
    const uint32_t *o = g_art[i].orig;
    int hit = 0, n = 0;
    for (int y = 6; y < PH; y += 12)
        for (int x = 6; x < PW; x += 12, n++) hit += close_enough(pix[y * PW + x], o[y * PW + x]);
    return hit * 100 / n;
}

/* Called for each converted frame (game picture w x h). Returns 1 when an HD background applies;
 * then the frame's alpha marks the game's own pixels (255) and the background (0). */
int hdart_prepare(uint32_t *pix, int w, int h, int menu)
{
    if (!g_cfg.hd_menus || !menu || w != PW || h != PH) { g_active = 0; return 0; }
    if (!g_loaded_orig) load_originals();
    int best = -1, best_p = 0;
    if (g_current >= 0 && g_art[g_current].orig && match_percent(pix, g_current) >= MATCH_MIN) best = g_current;
    else
        for (int i = 0; i < COUNT; i++) {
            if (!g_art[i].orig) continue;
            int p = match_percent(pix, i);
            if (p > best_p) { best_p = p; best = i; }
        }
    if (best >= 0 && best != g_current && best_p && best_p < MATCH_MIN) best = -1;
    if (best < 0) { g_current = -1; g_active = 0; return 0; }
    Art *a = &g_art[best];
    if (!a->hd && !a->tried_hd) {
        a->tried_hd = 1;
        char path[MAX_PATH], rel[96];
        wsprintfA(rel, "widescreen\\%s.png", NAMES[best]);
        hdart_game_path(path, MAX_PATH, rel);
        a->hd = hdart_load_image(path, &a->hw, &a->hh);
        mlog("hdart: %s.png %s (%dx%d)", NAMES[best], a->hd ? "loaded" : "could not be read", a->hw, a->hh);
        if (a->hd && a->panels) compose_centre(a, NAMES[best]);
    }
    if (!a->hd) { g_current = -1; g_active = 0; return 0; }
    g_active = 1;
    if (best != g_current) { g_current = best; g_serial++; }
    /* background = the pixel AND its 3x3 neighbourhood match the original, so single pixels of the
     * game's panel that happen to have the background's colour do not let the HD picture through.
     * The same rule for every picture: on panel screens the HD picture's centre is the original's geometry
     * (compose_centre), so its panels sit exactly under the game's buttons and labels.
     * History: 2026-11-19 the panel screens showed the game's 1x pixels in the whole centre (the AI panels were
     * 10-16 px off: doubled edges, broken BACK / CANCEL bar); 2026-11-21 a "faithful" exception gave Alpine the AI
     * centre with 1x pixels where it differed (dither specks, a column at the left seam, the orange strip) and made
     * it look unlike Desert. Both replaced by compose_centre. */
    const uint32_t *o = a->orig;
    static BYTE m[PW * PH];
    for (int i = 0; i < PW * PH; i++) m[i] = (BYTE)close_enough(pix[i], o[i]);
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
            int bg = 1;
            for (int dy = -1; dy <= 1 && bg; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy < 0 || yy >= PH || xx < 0 || xx >= PW) continue;
                    if (!m[yy * PW + xx]) { bg = 0; break; }
                }
            uint32_t *p = &pix[y * PW + x];
            *p = bg ? (*p & 0x00FFFFFFu) : (*p | 0xFF000000u);
        }
    return 1;
}

/* The current HD picture for gpu.c (serial changes when it changes). */
const uint32_t *hdart_picture(int *w, int *h, int *serial)
{
    if (g_current < 0 || !g_art[g_current].hd) return NULL;
    *w = g_art[g_current].hw; *h = g_art[g_current].hh; *serial = g_serial;
    return g_art[g_current].hd;
}

/* ---------------------------------------------------------------- the centre of panel screens
 * Resampling (once per picture, at load): separable Lanczos-3, opaque BGRA in and out. */
#define LZ_A 3
static double lanczos(double x)
{
    if (x == 0) return 1;
    if (x <= -LZ_A || x >= LZ_A) return 0;
    double px = 3.14159265358979 * x;
    return LZ_A * sin(px) * sin(px / LZ_A) / (px * px);
}

/* Weights of dn output samples over sn input samples: out[d] = sum w[d*nt+k] * in[clamp(first[d]+k)]. */
static float *lanczos_taps(int sn, int dn, int **first, int *ntaps)
{
    double sc = (double)sn / dn, wid = sc > 1 ? sc : 1;          /* downscaling widens the kernel */
    int nt = (int)ceil(2 * LZ_A * wid) + 1;
    float *w = malloc(sizeof(float) * dn * nt);
    *first = malloc(sizeof(int) * dn);
    if (!w || !*first) { free(w); free(*first); *first = NULL; return NULL; }
    for (int d = 0; d < dn; d++) {
        double c = (d + 0.5) * sc - 0.5, sum = 0;
        int f = (int)floor(c - LZ_A * wid) + 1;
        (*first)[d] = f;
        for (int k = 0; k < nt; k++) sum += w[d * nt + k] = (float)lanczos((c - (f + k)) / wid);
        for (int k = 0; k < nt; k++) w[d * nt + k] = (float)(w[d * nt + k] / sum);
    }
    *ntaps = nt;
    return w;
}

/* src (sw x sh) resampled to dst (dw x dh). Returns 0 when out of memory. */
static int resample(const uint32_t *src, int sw, int sh, uint32_t *dst, int dw, int dh)
{
    int *fx = NULL, *fy = NULL, ntx = 0, nty = 0, ok = 0;
    float *wx = lanczos_taps(sw, dw, &fx, &ntx), *wy = lanczos_taps(sh, dh, &fy, &nty);
    float *mid = malloc(sizeof(float) * 3 * (size_t)dw * sh);    /* horizontal pass: sh rows x dw */
    if (wx && wy && mid) {
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < dw; x++) {
                float r = 0, g = 0, b = 0;
                for (int k = 0; k < ntx; k++) {
                    int sx = fx[x] + k;
                    sx = sx < 0 ? 0 : sx >= sw ? sw - 1 : sx;
                    uint32_t p = src[(size_t)y * sw + sx];
                    float w = wx[x * ntx + k];
                    r += w * (p >> 16 & 255); g += w * (p >> 8 & 255); b += w * (p & 255);
                }
                float *m = &mid[((size_t)y * dw + x) * 3];
                m[0] = r; m[1] = g; m[2] = b;
            }
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                float c[3] = { 0, 0, 0 };
                for (int k = 0; k < nty; k++) {
                    int sy = fy[y] + k;
                    sy = sy < 0 ? 0 : sy >= sh ? sh - 1 : sy;
                    const float *m = &mid[((size_t)sy * dw + x) * 3];
                    float w = wy[y * nty + k];
                    c[0] += w * m[0]; c[1] += w * m[1]; c[2] += w * m[2];
                }
                int v[3];
                for (int i = 0; i < 3; i++) v[i] = c[i] < 0 ? 0 : c[i] > 255 ? 255 : (int)(c[i] + 0.5f);
                dst[(size_t)y * dw + x] = 0xFF000000u | (uint32_t)v[0] << 16 | (uint32_t)v[1] << 8 | (uint32_t)v[2];
            }
        ok = 1;
    }
    free(wx); free(wy); free(fx); free(fy); free(mid);
    return ok;
}

/* Panel screens: the HD picture's 640-wide centre (the part gpu.c puts under the game picture: x0 =
 * (hw - 640 s) / 2, s = hh / 480) becomes the faithful upscale of the ORIGINAL - hd\Screens\<name>.bmp (the
 * 2x art, any 4:3 size), else the original BMP itself - resampled once to the centre's size. Within SEAM_PX game
 * px of the centre's left / right edge it fades (smoothstep) from the wide picture to that upscale: the AI sides
 * continue the scene with other geometry, so a hard edge showed as a cut (rocks, the shack, Alpine's dark border).
 * Every panel of the original sits at least 32 game px inside the edges (measured: misn_select from x 75, multi*
 * from x 33), so the fade never touches a panel. */
static void compose_centre(Art *a, const char *name)
{
    double s = a->hh / (double)PH;
    int cw = (int)(PW * s + 0.5), x0 = (a->hw - cw) / 2;
    if (cw <= 0 || x0 < 0 || x0 + cw > a->hw) return;
    char path[MAX_PATH], rel[96];
    wsprintfA(rel, "hd\\Screens\\%s.bmp", name);
    hdart_game_path(path, MAX_PATH, rel);
    int uw = 0, uh = 0;
    uint32_t *up = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? hdart_load_image(path, &uw, &uh) : NULL;
    if (up && (uw * 3 != uh * 4 || uw < PW)) { free(up); up = NULL; }   /* not a 4:3 upscale of the picture */
    const uint32_t *src = up ? up : a->orig;
    int sw = up ? uw : PW, sh = up ? uh : PH;
    uint32_t *c = malloc((size_t)cw * a->hh * 4);
    if (c && resample(src, sw, sh, c, cw, a->hh)) {
        for (int x = 0; x < cw; x++) {
            double gx = (x + 0.5) / s, d = gx < PW - gx ? gx : PW - gx, t = d / SEAM_PX;
            if (t > 1) t = 1;
            int w = (int)(256 * t * t * (3 - 2 * t) + 0.5);             /* weight of the upscale, 0..256 */
            for (int y = 0; y < a->hh; y++) {
                uint32_t *h = &a->hd[(size_t)y * a->hw + x0 + x], u = c[(size_t)y * cw + x];
                if (w >= 256) { *h = u; continue; }
                uint32_t r = ((*h >> 16 & 255) * (256 - w) + (u >> 16 & 255) * w) >> 8;
                uint32_t g = ((*h >> 8 & 255) * (256 - w) + (u >> 8 & 255) * w) >> 8;
                uint32_t b = ((*h & 255) * (256 - w) + (u & 255) * w) >> 8;
                *h = 0xFF000000u | r << 16 | g << 8 | b;
            }
        }
        mlog("hdart: %s: centre rebuilt from %s (%dx%d -> %dx%d at x %d), sides faded in over %d px", name,
             up ? rel : "the original BMP", sw, sh, cw, a->hh, x0, SEAM_PX);
    } else mlog("hdart: %s: centre could not be rebuilt (out of memory) - the wide picture as it is", name);
    free(c);
    free(up);
}

/* ---------------------------------------------------------------- the menu cursor
 * With an HD background the pointer can go anywhere on the wide picture, but the game draws its
 * cursor inside its 640x480 picture. So the game's cursor picture (0x4fe3f8) is turned into a
 * Windows cursor (scaled to the display) and the game's own drawing of it is skipped (hud.c). */
typedef struct { void *image; BYTE pad0[0x14]; RECT src; short hot_x, hot_y; } CursorPic;
#define CURSOR_PIC ((CursorPic *)0x4fe3f8)

HCURSOR hdart_menu_cursor(int scale)
{
    static HCURSOR cur;
    static int cur_scale; static void *cur_image; static RECT cur_src;   /* the picture may change (gameplay command cursors) */
    Surf *s = CURSOR_PIC->image;
    if (cur && cur_scale == scale && cur_image == (void *)s && EqualRect(&cur_src, &CURSOR_PIC->src)) return cur;
    Pal *pal = g_dd ? g_dd->pal : NULL;
    if (!s || !s->bits || !pal || scale < 1) return NULL;
    int w = CURSOR_PIC->src.right - CURSOR_PIC->src.left, h = CURSOR_PIC->src.bottom - CURSOR_PIC->src.top;
    if (w <= 0 || h <= 0 || w > 64 || h > 64) return NULL;
    int W = w * scale, H = h * scale;
    BITMAPV5HEADER bi = { .bV5Size = sizeof bi, .bV5Width = W, .bV5Height = -H, .bV5Planes = 1, .bV5BitCount = 32,
                          .bV5Compression = BI_BITFIELDS, .bV5RedMask = 0x00FF0000, .bV5GreenMask = 0x0000FF00,
                          .bV5BlueMask = 0x000000FF, .bV5AlphaMask = 0xFF000000 };
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    HBITMAP mask = CreateBitmap(W, H, 1, 1, NULL);
    if (!color || !mask || !bits) { if (color) DeleteObject(color); if (mask) DeleteObject(mask); return NULL; }
    uint32_t *px = bits;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            BYTE i = s->bits[(size_t)(CURSOR_PIC->src.top + y / scale) * s->pitch + CURSOR_PIC->src.left + x / scale];
            int key = s->has_ck && i >= s->ck_lo && i <= s->ck_hi;
            px[y * W + x] = key ? 0 : 0xFF000000u | (uint32_t)pal->e[i].peRed << 16 | (uint32_t)pal->e[i].peGreen << 8 | pal->e[i].peBlue;
        }
    ICONINFO ii = { FALSE, (DWORD)(CURSOR_PIC->hot_x * scale), (DWORD)(CURSOR_PIC->hot_y * scale), mask, color };
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    if (!c) return NULL;
    if (cur) DestroyCursor(cur);
    cur = c;
    cur_scale = scale; cur_image = s; cur_src = CURSOR_PIC->src;
    mlog("hdart: cursor from the game picture %p %dx%d hot %d,%d x%d", (void *)s, w, h, CURSOR_PIC->hot_x, CURSOR_PIC->hot_y, scale);
    return cur;
}
