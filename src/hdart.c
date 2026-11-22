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
 * PNGs are loaded with WIC (Windows Imaging Component). [display] hd_menus = 1
 * ============================================================================================ */
#define COBJMACROS
#include "common.h"
#include <wincodec.h>

static const char *const NAMES[] = { "Title_legal", "alp_load", "alp_misn_select", "bay_load", "bay_misn_select",
                                     "des_load", "des_misn_select", "multihost", "multiremote", "title2" };
#define COUNT ((int)(sizeof NAMES / sizeof NAMES[0]))
#define PW 640
#define PH 480
#define TOL 12                    /* summed colour difference that still counts as "the background" */
#define MATCH_MIN 40              /* % of the background that must show: win/lose radios cover ~44 % */

typedef struct { uint32_t *orig; uint32_t *hd; int hw, hh; int tried_hd; BYTE *bad; int keep_ui; int bad_permille; } Art;
static void build_bad_mask(Art *a);
#define HD_CENTRE_FAITHFUL 10      /* per mille of the centre differing: below = the HD centre is used */
static Art g_art[COUNT];
static int g_loaded_orig, g_current = -1;
static int g_serial;              /* changes when the current HD picture changes (gpu.c re-uploads) */
static volatile int g_active;     /* the last menu frame used an HD background */
static int g_mix_centre;          /* [display] hd_menus_mix_centre (read with the originals) */

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
    g_mix_centre = GetPrivateProfileIntA("display", "hd_menus_mix_centre", 0, mod_ini_path());
    char path[MAX_PATH], rel[96];
    int n = 0;
    for (int i = 0; i < COUNT; i++) {
        wsprintfA(rel, "widescreen\\%s.png", NAMES[i]);
        hdart_game_path(path, MAX_PATH, rel);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;   /* no HD version: skip */
        wsprintfA(rel, "Screens\\%s.bmp", NAMES[i]);
        hdart_game_path(path, MAX_PATH, rel);
        if ((g_art[i].orig = load_bmp8(path))) n++;
        /* backgrounds with menu boxes painted into the art: where the AI centre differs, the
         * original is kept (the boxes stay intact); pure art (title, load screens) uses the wide
         * picture everywhere, so it has no seams */
        g_art[i].keep_ui = strstr(NAMES[i], "misn_select") || strstr(NAMES[i], "multi");
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
        build_bad_mask(a);
    }
    if (!a->hd) { g_current = -1; g_active = 0; return 0; }
    g_active = 1;
    if (best != g_current) { g_current = best; g_serial++; }
    /* background = the pixel AND its 3x3 neighbourhood match the original, so single pixels of the
     * game's panel that happen to have the background's colour do not let the HD picture through */
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
            /* Screens with menu boxes painted in the art (mission select, multiplayer): the AI-widened picture is
             * used for the SIDES only; in the 640-wide centre every pixel is the game's (and its faithful 2x upscale,
             * hdsprites). 2026-11-19, user: "the menu has graphical issues": the widened pictures have the green
             * panels 10-16 px away from the original's (des_misn_select: box 60..317 instead of 76..321, bar
             * 156..473 instead of 178..470), and mixing the two per pixel ("original where they differ a lot") gave
             * doubled, ragged panel edges and a broken BACK / CANCEL bar. [display] hd_menus_mix_centre = 1 = the old
             * per-pixel mix. Left: a seam where the widened picture meets the centre (needs wide art made without
             * panels, or the menu renderer - tooling/docs/menu-render-evaluation.md). */
            /* 2026-11-21 (user, Alpine mission select: "this is rendering the original on the HD art"): a picture
             * whose HD centre matches the original (under HD_CENTRE_FAITHFUL per mille of pixels differ, e.g.
             * alp_misn_select 0 %) keeps its HD centre - the original only where it differs; the whole-centre rule
             * stays for pictures with moved panels (des_misn_select). */
            int whole = a->keep_ui && !g_mix_centre && !(a->bad && a->bad_permille < HD_CENTRE_FAITHFUL);
            if (a->keep_ui && (whole || (a->bad && a->bad[y * PW + x]))) bg = 0;
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

/* ---------------------------------------------------------------- where the HD centre differs
 * The AI-extended pictures are not always faithful in the centre (bars, panels drawn a bit
 * differently). Where the HD picture's centre does not match the original background, the game's
 * own pixels are used (bad[] = 1), so menus never look broken; elsewhere the HD art shows. */
#define BAD_TOL 120
static void build_bad_mask(Art *a)
{
    if (a->bad || !a->hd || !a->orig) return;
    a->bad = malloc(PW * PH);
    if (!a->bad) return;
    double s = a->hh / (double)PH;                         /* HD pixels per game pixel */
    double x0 = (a->hw - PW * s) / 2;                      /* the 640-wide centre in the HD picture */
    int n_bad = 0;
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) {
            int hx = (int)(x0 + (x + 0.5) * s), hy = (int)((y + 0.5) * s);
            if (hx < 0) hx = 0;
            if (hx >= a->hw) hx = a->hw - 1;
            if (hy >= a->hh) hy = a->hh - 1;
            uint32_t h = a->hd[hy * a->hw + hx], o = a->orig[y * PW + x];
            int d = abs((int)(h >> 16 & 255) - (int)(o >> 16 & 255)) + abs((int)(h >> 8 & 255) - (int)(o >> 8 & 255))
                  + abs((int)(h & 255) - (int)(o & 255));
            a->bad[y * PW + x] = (BYTE)(d > BAD_TOL);
            n_bad += d > BAD_TOL;
        }
    a->bad_permille = n_bad * 1000 / (PW * PH);
    mlog("hdart: %d.%d%% of the centre differs from the original - %s", a->bad_permille / 10, a->bad_permille % 10,
         a->keep_ui && a->bad_permille >= HD_CENTRE_FAITHFUL && !g_mix_centre ? "menu panels: the whole centre is the original's" : "the original is used where it differs");
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
