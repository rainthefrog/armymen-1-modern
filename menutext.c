/* ============================================================================================
 * menutext.c - menu labels (and Mod Settings sliders) drawn by the mod at window resolution
 *               (menu renderer, step 1; tooling/docs/menu-render-evaluation.md section 8)
 *
 * 2026-11-20, user: "the font needs to be bigger, and moving the mouse over it will change the text color. it
 * really needs to be cleanly implemented to fix rendering issues". Every label of the game's menus goes through
 * ONE routine: FUN_00413a30 (hud.c hud_draw_gdi_text: ecx = surface, edx = x; stack y, text, font id, size out),
 * onto the SCREEN surface with GAME_FONTS[font] in LABEL_COLOUR (set by SBtnDisplay 0x45f020 from
 * SBTNcolorTable 0x4cf8e0 just before; Wine: 0x67856f normal, 0x14fff2 played, 0xffffff focus, 0x263528 not
 * available, 0 titles). The label is NOT drawn into the game's picture any more - the HD background under it
 * stays untouched (the "eaten letters" and the hover smear came from the game redrawing text in place) - it is
 * recorded (text, position, font, colour) and gpu.c draw_overlay lays all labels as one premultiplied layer over
 * the picture, rendered here with GDI at the window's pixel size:
 *   - font = the game's LOGFONT x window scale x [display] menu_text_scale (1.25), antialiased; all-capital
 *     words (BACK, NEXT: on button pictures) x1.0;
 *   - position: a label of a TEXT_CENTER button is centred in the button's ParentBox (the game's own button data,
 *     list 0x6a5058 / count 0x6a546c; user: "the label positioning isn't uniform anymore" - centring on each
 *     label's own measured box was off by a few px per row); other labels keep their left edge;
 *   - colour = the game's colour for the state; dark greens lifted to be readable (black titles stay), a soft
 *     shadow under light text.
 * Lifetime: a label is replaced by one drawn over its box (the game redraws a label on every state change), dropped
 * by a picture drawn over it (menutext_note_blit), and ALL are dropped when the screen state changes (user: "back
 * seems to keep the previous menu" - on Windows the new screen's background did not come through the blit path)
 * or Mod Settings rebuilds its page (menutext_clear).
 * Screens: the full-screen menus, and the option screens over a paused mission (user: "the pause menu wasn't
 * updated"; game_menu_up: positions / sizes through layout_menu_place x k, as hud.c does for the game's text).
 * Sliders (modmenu.c; user: "change the mod options that are not buttons to be sliders"): track, stops, knob and
 * the value, drawn in the same layer from menutext_set_sliders.
 * '#'-coded labels stay the game's. Only while gpu.c can draw the layer (gpu_overlay_ready).
 * [display] menu_text=1, menu_text_scale=1.25.
 * ============================================================================================ */
#include "common.h"
#include <math.h>

#define LABEL_COLOUR  (*(COLORREF *)0x4e9f70)
#define GAME_FONTS    ((HFONT *)0x4e9f48)
#define GAME_CLIENT_SCREEN_RECT (*(RECT *)0x4e2dd8)   /* windowed: client origin; fullscreen: 0,0 */
#define SCREEN        (*(void ***)0x4e2e48)
#define SCRN_FULL     (*(BYTE **)0x6a5944)            /* SCREEN*: +4 state */
#define BTN_LIST      ((BYTE **)0x6a5058)
#define BTN_COUNT     (*(int *)0x6a546c)
#define BTN_TEXT_CENTER 0x04000000
#define BTN_PARENT    0x2fc                           /* SBOX shorts x, y, cx, cy */
#define BTN_TEXTBOX   0x304
#define BTN_NULLBOX   0x30c                           /* the picture of an icon button (SBOX) */
#define MT_MAX        96
#define NO_CENTRE     (-100000)
#define CURSOR_BLIT   40                              /* blits this small (the cursor's save-under) do not drop labels */

typedef struct { char text[96]; int x, y, w, h, font, cx; float fk; COLORREF colour; } Label;
static Label g_lab[MT_MAX];
static int g_n;
static MtSlider g_sl[MT_SLIDERS];
static int g_nsl, g_sl_state = -1;
static volatile LONG g_ver = 1;
static CRITICAL_SECTION g_cs;
static int g_cs_ok;
static HDC g_measure;
static int g_state = -1, g_in_play_menu = -1;

static void lock(void) { if (!g_cs_ok) { InitializeCriticalSection(&g_cs); g_cs_ok = 1; } EnterCriticalSection(&g_cs); }
static void unlock(void) { LeaveCriticalSection(&g_cs); }

static float text_scale(void)
{
    static float s = -1;
    if (s < 0) {
        char v[16];
        GetPrivateProfileStringA("display", "menu_text_scale", "1.25", v, sizeof v, mod_ini_path());
        s = (float)atof(v);
        if (s < 0.8f || s > 2.0f) s = 1.25f;
    }
    return s;
}

static int screens_on(void) { return !layout_view_is_gameplay() || game_menu_up(); }

void menutext_clear(void)
{
    lock();
    if (g_n || g_nsl) { g_n = 0; g_nsl = 0; InterlockedIncrement(&g_ver); }
    unlock();
}

/* a new screen (state), or the in-game menus opened / closed: every label of the old one is gone */
static void track_screen(void)
{
    BYTE *s = SCRN_FULL;
    int st = s && !IsBadReadPtr(s, 8) ? *(int *)(s + 4) : -1, pm = layout_view_is_gameplay() ? game_menu_up() : 0;
    if (st != g_state || pm != g_in_play_menu) {               /* labels only: the sliders carry their own state */
        g_state = st; g_in_play_menu = pm;
        lock(); if (g_n) { g_n = 0; InterlockedIncrement(&g_ver); } unlock();
    }
}

/* the game's TEXT_CENTER button for this label (same text, its TextBox at the drawn position): the centre of its
 * ParentBox, in the label's coordinates */
static int button_centre(const char *text, int dx, int dy, int *centre)
{
    int n = BTN_COUNT;
    if (n <= 0 || n > 260) return 0;
    for (int i = 0; i < n; i++) {
        BYTE *b = BTN_LIST[i];
        if (!b || IsBadReadPtr(b, 0x338)) continue;
        DWORD at = *(DWORD *)b;
        if (lstrcmpA((const char *)(b + 0x44), text)) continue;
        int icon = (at & 0x00800000) != 0;                 /* a picture button: centred on its picture (user: "text on the buttons need to be centered") */
        if (!icon && (!(at & BTN_TEXT_CENTER) || !(at & 0x08000000))) continue;
        const short *tb = (const short *)(b + BTN_TEXTBOX), *pb = (const short *)(b + (icon ? BTN_NULLBOX : BTN_PARENT));
        if (abs(tb[1] - dy) > 2 && abs(tb[0] - dx) > 2) continue;       /* another button with the same text */
        if (pb[2] <= 0) continue;
        /* the picture box of an icon button may be relative to the button (ParentBox + position + NullBox): take the
         * placement that holds the text's centre (2026-11-20, user: "the text is not exactly centered over buttons";
         * the box as stored put labels in the far left corner) */
        int bx = pb[0], by = pb[1];
        int tcx = tb[0] + tb[2] / 2, tcy = tb[1] + tb[3] / 2;
        int holds = tcx >= bx - 2 && tcx <= bx + pb[2] + 2 && tcy >= by - 2 && tcy <= by + pb[3] + 2;
        if (!holds && icon) {
            const short *par = (const short *)(b + BTN_PARENT), *pos = (const short *)(b + 0x40);
            bx = par[0] + pos[0] + pb[0]; by = par[1] + pos[1] + pb[1];
            holds = tcx >= bx - 2 && tcx <= bx + pb[2] + 2 && tcy >= by - 2 && tcy <= by + pb[3] + 2;
        }
        if (g_cfg.log >= 2) { static int n; if (n++ < 40) mlog("menutext: button \"%s\" text %d,%d %dx%d box %d,%d %dx%d (%s) %s", text, tb[0], tb[1], tb[2], tb[3], bx, by, pb[2], pb[3], icon ? "icon" : "text", holds ? "centred" : "left as drawn"); }
        if (!holds) continue;
        *centre = dx + (bx + pb[2] / 2 - tb[0]);
        return 1;
    }
    return 0;
}

/* hud.c hud_draw_gdi_text: 1 = recorded (the game's TextOut is skipped), size_out filled like the original */
int menutext_capture(void *surface, int x, int y, const char *text, int font, int *size_out)
{
    if (!g_cfg.menu_text || surface != SCREEN || !screens_on() || !gpu_overlay_ready()) return 0;
    if (!text || !text[0] || text[0] == '#' || font < 0 || font >= 8 || !GAME_FONTS[font]) return 0;
    if (!g_measure) g_measure = CreateCompatibleDC(NULL);
    if (!g_measure) return 0;
    track_screen();
    HGDIOBJ old = SelectObject(g_measure, GAME_FONTS[font]);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32A(g_measure, text, lstrlenA(text), &sz);
    SelectObject(g_measure, old);
    if (size_out) { size_out[0] = sz.cx; size_out[1] = sz.cy; }   /* in menu units, as the game lays out */
    if (dd_is_windowed()) { x -= GAME_CLIENT_SCREEN_RECT.left; y -= GAME_CLIENT_SCREEN_RECT.top; }
    int centre = NO_CENTRE;
    int have_centre = button_centre(text, x, y, &centre);
    Label l = { { 0 }, x, y, sz.cx, sz.cy, font, NO_CENTRE, 1.0f, LABEL_COLOUR };
    int k = layout_view_is_gameplay() ? layout_menu_scale() : 0;
    if (k) {                                                    /* an in-game menu: x k around the centre (layout.c) */
        int qx = x + sz.cx, qy = y + sz.cy, cyy = y;
        layout_menu_place(k, &l.x, &l.y); layout_menu_place(k, &qx, &qy);
        if (have_centre) layout_menu_place(k, &centre, &cyy);
        l.w = qx - l.x; l.h = qy - l.y; l.fk = k / 64.0f;
    }
    if (have_centre) l.cx = centre;
    lstrcpynA(l.text, text, sizeof l.text);
    lock();
    RECT nr = { l.x, l.y, l.x + l.w, l.y + l.h };
    int same = 0;
    /* a label drawn over another one's box replaces it - only when they cover MOSTLY the same area (half the smaller
     * box). 2026-11-20 (user: "highlight on menus will sometimes stop working"): any touch counted before; list rows
     * are 20 px apart with ~23 px text boxes, so redrawing the row left behind (normal colour) removed the newly
     * highlighted neighbour */
    for (int i = 0; i < g_n; i++) {
        RECT r = { g_lab[i].x, g_lab[i].y, g_lab[i].x + g_lab[i].w, g_lab[i].y + g_lab[i].h }, t;
        if (!IntersectRect(&t, &r, &nr)) continue;
        long ov = (long)(t.right - t.left) * (t.bottom - t.top);
        long a1 = (long)(r.right - r.left) * (r.bottom - r.top), a2 = (long)(nr.right - nr.left) * (nr.bottom - nr.top);
        if (ov * 2 < (a1 < a2 ? a1 : a2)) continue;
        if (!memcmp(&g_lab[i], &l, sizeof l)) { same = 1; continue; }
        g_lab[i] = g_lab[--g_n]; i--;
    }
    if (!same && g_n < MT_MAX) { g_lab[g_n++] = l; InterlockedIncrement(&g_ver); }
    unlock();
    /* the game's picture does not change any more when a label does (it is not drawn into it): wake the display
     * thread like a screen write would (2026-11-20, user: "label hover on main menu doesn't work at all") */
    if (!same && g_dd) present_request(g_dd, 0);
    if (g_cfg.log >= 2) { static int n; if (n++ < 200) mlog("menutext: %s %d,%d %dx%d centre %d colour %08lx \"%.40s\"", same ? "same" : "label", l.x, l.y, l.w, l.h, l.cx, l.colour, text); }
    return 1;
}

/* surface.c: a picture was drawn onto the screen at r (screen surface px): labels inside it are gone */
void menutext_note_blit(const RECT *r, int desktop)
{
    if (!g_n || !r || (r->right - r->left <= CURSOR_BLIT && r->bottom - r->top <= CURSOR_BLIT)) return;
    RECT c = *r;
    if (desktop && dd_is_windowed()) OffsetRect(&c, -GAME_CLIENT_SCREEN_RECT.left, -GAME_CLIENT_SCREEN_RECT.top);
    lock();
    int dropped = 0;
    for (int i = 0; i < g_n; i++) {
        const Label *l = &g_lab[i];
        if (l->x >= c.left && l->y >= c.top && l->x + l->w <= c.right && l->y + l->h <= c.bottom) { g_lab[i] = g_lab[--g_n]; i--; dropped++; }
    }
    if (dropped) InterlockedIncrement(&g_ver);
    unlock();
}

/* modmenu.c, every frame Mod Settings is up: the sliders (design px), replacing the last set */
void menutext_set_sliders(const MtSlider *s, int n, int state)
{
    if (n < 0) n = 0;
    if (n > MT_SLIDERS) n = MT_SLIDERS;
    MtSlider t[MT_SLIDERS];
    memset(t, 0, sizeof t);
    int k = layout_view_is_gameplay() ? layout_menu_scale() : 0;
    for (int i = 0; i < n; i++) {
        t[i] = s[i];
        t[i].fk = 1.0f;
        if (k) {
            int x0 = t[i].r.left, y0 = t[i].r.top, x1 = t[i].r.right, y1 = t[i].r.bottom;
            layout_menu_place(k, &x0, &y0); layout_menu_place(k, &x1, &y1);
            SetRect(&t[i].r, x0, y0, x1, y1); t[i].fk = k / 64.0f;
        }
    }
    lock();
    int changed = n != g_nsl || state != g_sl_state || memcmp(g_sl, t, sizeof t[0] * n);
    if (changed) { memcpy(g_sl, t, sizeof t[0] * n); g_nsl = n; g_sl_state = state; InterlockedIncrement(&g_ver); }
    unlock();
    if (changed && g_dd) present_request(g_dd, 0);
}

static void colour_rgb(COLORREF c, int *r, int *g, int *b)
{
    if ((c >> 24) == 1 && g_dd && g_dd->pal) {            /* PALETTEINDEX */
        const PALETTEENTRY *e = &g_dd->pal->e[c & 0xff];
        *r = e->peRed; *g = e->peGreen; *b = e->peBlue;
    } else { *r = GetRValue(c); *g = GetGValue(c); *b = GetBValue(c); }
    int lum = (*r * 30 + *g * 59 + *b * 11) / 100;
    if (lum > 8 && lum < 80) {                            /* dark green "not available": lifted to be readable */
        int add = 80 - lum;
        *r = min(255, *r + add); *g = min(255, *g + add); *b = min(255, *b + add);
    }
}

/* ---------------------------------------------------------------- the layer (display thread) */
static uint32_t *g_out; static BYTE *g_cov; static int g_ow, g_oh;
static HDC g_dc; static HBITMAP g_bmp; static uint32_t *g_bits;

static void put(int x, int y, int r, int g, int b, int a)      /* premultiplied; keeps the more opaque */
{
    if (x < 0 || y < 0 || x >= g_ow || y >= g_oh || a <= 0) return;
    if (a > 255) a = 255;
    uint32_t *p = &g_out[(size_t)y * g_ow + x];
    if ((int)(*p >> 24) >= a) return;
    *p = ((uint32_t)a << 24) | ((uint32_t)(r * a / 255) << 16) | ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
}
static void put_over(int x, int y, int r, int g, int b, int a)  /* premultiplied "over" (knob over track) */
{
    if (x < 0 || y < 0 || x >= g_ow || y >= g_oh || a <= 0) return;
    if (a > 255) a = 255;
    uint32_t *p = &g_out[(size_t)y * g_ow + x], d = *p;
    int ia = 255 - a;
    int oa = a + (int)(d >> 24) * ia / 255;
    int orr = r * a / 255 + (int)((d >> 16) & 0xff) * ia / 255, og = g * a / 255 + (int)((d >> 8) & 0xff) * ia / 255, ob = b * a / 255 + (int)(d & 0xff) * ia / 255;
    *p = ((uint32_t)min(oa, 255) << 24) | ((uint32_t)min(orr, 255) << 16) | ((uint32_t)min(og, 255) << 8) | (uint32_t)min(ob, 255);
}
static void fill_rect_aa(float x0, float y0, float x1, float y1, int r, int g, int b, int a)
{
    for (int y = (int)floorf(y0); y < (int)ceilf(y1); y++)
        for (int x = (int)floorf(x0); x < (int)ceilf(x1); x++) {
            float cx = fminf(x + 1.0f, x1) - fmaxf((float)x, x0), cy = fminf(y + 1.0f, y1) - fmaxf((float)y, y0);
            if (cx > 0 && cy > 0) put(x, y, r, g, b, (int)(a * cx * cy));
        }
}
static void disc(float cx, float cy, float rad, int r, int g, int b, int a)
{
    for (int y = (int)(cy - rad - 1); y <= (int)(cy + rad + 1); y++)
        for (int x = (int)(cx - rad - 1); x <= (int)(cx + rad + 1); x++) {
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float c = rad + 0.5f - d;
            if (c > 0) put_over(x, y, r, g, b, (int)(a * (c > 1 ? 1 : c)));
        }
}

/* ---------------------------------------------------------------- cached text (2026-11-20)
 * User: "The new menus are stuttering when hover over". Before, every hover colour change rebuilt the WHOLE layer:
 * two window-sized buffers cleared, every label drawn again with GDI, the whole texture uploaded (several ms -> a
 * dropped frame per change). Now a label's text is rendered with GDI ONCE per (text, font, size) into a small
 * coverage bitmap (cache below); a change redraws only the items whose rectangle changed, from the cache, and
 * gpu.c uploads only that dirty box. */
#define TC_MAX 160
typedef struct { char text[96]; HFONT font; int w, h; BYTE *cov; DWORD used; } TextCov;
static TextCov g_tc[TC_MAX];

static const TextCov *text_cov(const char *s, HFONT f)
{
    int free_i = -1, old_i = 0;
    for (int i = 0; i < TC_MAX; i++) {
        if (g_tc[i].cov && g_tc[i].font == f && !lstrcmpA(g_tc[i].text, s)) { g_tc[i].used = GetTickCount(); return &g_tc[i]; }
        if (!g_tc[i].cov && free_i < 0) free_i = i;
        if (g_tc[i].used < g_tc[old_i].used) old_i = i;
    }
    int i = free_i >= 0 ? free_i : old_i;
    TextCov *t = &g_tc[i];
    free(t->cov); t->cov = NULL;
    SelectObject(g_dc, f);
    SIZE sz; GetTextExtentPoint32A(g_dc, s, lstrlenA(s), &sz);
    int w = sz.cx + 4, h = sz.cy + 4;
    if (w > g_ow) w = g_ow;
    if (h > g_oh) h = g_oh;
    if (w <= 0 || h <= 0) return NULL;
    for (int y = 0; y < h; y++) memset(g_bits + (size_t)y * g_ow, 0, (size_t)w * 4);
    TextOutA(g_dc, 2, 2, s, lstrlenA(s));
    GdiFlush();
    t->cov = malloc((size_t)w * h);
    if (!t->cov) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = g_bits[(size_t)y * g_ow + x];
            t->cov[y * w + x] = (BYTE)max((int)(p & 0xff), max((int)((p >> 8) & 0xff), (int)((p >> 16) & 0xff)));
        }
    for (int y = 0; y < h; y++) memset(g_bits + (size_t)y * g_ow, 0, (size_t)w * 4);
    lstrcpynA(t->text, s, sizeof t->text); t->font = f; t->w = w; t->h = h; t->used = GetTickCount();
    return t;
}
static void tc_flush(void) { for (int i = 0; i < TC_MAX; i++) { free(g_tc[i].cov); g_tc[i].cov = NULL; } }

/* the game's font f scaled (cached, a few sizes per font) */
static HFONT font_px(int f, float scale_y, float scale_x)
{
    static HFONT fonts[8][4]; static int fh[8][4]; static HFONT src[8][4]; static int next[8];
    if (f < 0 || f >= 8) return NULL;
    HFONT gf = GAME_FONTS[f];
    LOGFONTA lf;
    if (!gf || !GetObjectA(gf, sizeof lf, &lf)) return NULL;
    int want = (int)(lf.lfHeight * scale_y + (lf.lfHeight < 0 ? -0.5f : 0.5f));
    for (int i = 0; i < 4; i++) if (fonts[f][i] && fh[f][i] == want && src[f][i] == gf) return fonts[f][i];
    int i = next[f]; next[f] = (i + 1) & 3;
    if (fonts[f][i]) {                                     /* cached coverage made with this font is stale */
        for (int k = 0; k < TC_MAX; k++) if (g_tc[k].font == fonts[f][i]) { free(g_tc[k].cov); g_tc[k].cov = NULL; }
        DeleteObject(fonts[f][i]);
    }
    lf.lfHeight = want;
    lf.lfWidth = (int)(lf.lfWidth * scale_x + 0.5f);
    lf.lfQuality = ANTIALIASED_QUALITY;
    fonts[f][i] = CreateFontIndirectA(&lf); fh[f][i] = want; src[f][i] = gf;
    return fonts[f][i];
}

/* one item of the layer: a label or a slider, with the rectangle it covers (window px, shadow included) */
typedef struct { int kind; RECT r; const TextCov *tc; int tx, ty, cr, cg, cb, sh; MtSlider s; float x0, x1, my, u, kr; const TextCov *vtc; } Item;
#define IT_MAX (MT_MAX + MT_SLIDERS)
static Item g_prev[IT_MAX]; static int g_nprev = -1;

static void draw_cov(const TextCov *t, int tx, int ty, int r, int g, int b, int sh, const RECT *clip)
{
    if (!t || !t->cov) return;
    int x0 = max(tx, clip->left), y0 = max(ty, clip->top), x1 = min(tx + t->w + sh, clip->right), y1 = min(ty + t->h + sh, clip->bottom);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int lx = x - tx, ly = y - ty;
            int a = lx < t->w && ly < t->h ? t->cov[ly * t->w + lx] : 0;
            int sx2 = lx - sh, sy2 = ly - sh;
            int s2 = sh && sx2 >= 0 && sy2 >= 0 && sx2 < t->w && sy2 < t->h ? t->cov[sy2 * t->w + sx2] * 140 / 255 : 0;
            int ta = a + s2 * (255 - a) / 255;
            if (!ta) continue;
            uint32_t o = ((uint32_t)ta << 24) | ((uint32_t)(r * a / 255) << 16) | ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
            uint32_t *pp = &g_out[(size_t)y * g_ow + x];
            if ((*pp >> 24) < (uint32_t)ta) *pp = o;
        }
}

static void draw_item(const Item *it, const RECT *clip)
{
    if (it->kind == 0) { draw_cov(it->tc, it->tx, it->ty, it->cr, it->cg, it->cb, it->sh, clip); return; }
    const MtSlider *s = &it->s;
    float x0 = it->x0, x1 = it->x1, my = it->my, u = it->u, th = fmaxf(2.0f, u * 1.5f), kr = it->kr;
    fill_rect_aa(x0 - 1, my - th * 0.5f - 1, x1 + 1, my + th * 0.5f + 1, 10, 22, 12, 220);
    fill_rect_aa(x0, my - th * 0.5f, x1, my + th * 0.5f, 0x6f, 0x85, 0x67, 255);
    for (int t = 0; t < s->steps && s->steps > 1; t++) {
        float tx = x0 + (x1 - x0) * t / (s->steps - 1);
        fill_rect_aa(tx - u * 0.5f, my - th * 1.8f, tx + u * 0.5f, my + th * 1.8f, 0x6f, 0x85, 0x67, 255);
    }
    float kx = s->steps > 1 ? x0 + (x1 - x0) * s->pos / (s->steps - 1) : x0;
    disc(kx + u * 0.5f, my + u * 0.5f, kr, 0, 0, 0, 130);
    if (s->active) disc(kx, my, kr, 0xff, 0xff, 0xff, 255);
    else disc(kx, my, kr, 0xf2, 0xff, 0x14, 255);
    if (it->vtc) draw_cov(it->vtc, (int)(x1 + kr + u * 4), (int)(my - it->vtc->h * 0.5f), 0xf2, 0xff, 0x14, it->sh, clip);
}

static int item_same(const Item *a, const Item *b)
{
    if (a->kind != b->kind || !EqualRect(&a->r, &b->r)) return 0;
    if (a->kind == 0) return a->tc == b->tc && a->tx == b->tx && a->ty == b->ty && a->cr == b->cr && a->cg == b->cg && a->cb == b->cb;
    return !memcmp(&a->s, &b->s, sizeof a->s);
}

/* gpu.c (display thread): the layer for a picture of pw x ph game px shown at dw x dh window px.
 * 1 = there is a layer (*pix: dw x dh premultiplied BGRA; *ver changes when it does; *dirty = the part that
 * changed since the last call that returned this layer, empty = nothing); 0 = none. */
int menutext_overlay(int dw, int dh, int pw, int ph, const uint32_t **pix, unsigned *ver, RECT *dirty)
{
    static LONG built = -1;
    static int bw, bh;
    SetRectEmpty(dirty);
    if (!g_cfg.menu_text || dw <= 0 || dh <= 0 || pw <= 0 || ph <= 0 || dw > 8192 || dh > 8192) return 0;
    if (!screens_on()) { if (g_n || g_nsl) menutext_clear(); g_nprev = -1; return 0; }
    /* 2026-11-20 (user: "the menu bleeds over into the mission start"): the mission's loading / briefing pictures are
     * not menu screens and did not always replace the labels - they stayed until the next label. Every frame: a new
     * screen state, or a mission in play without one of its option screens up, drops all labels. */
    {
        BYTE *sc = SCRN_FULL;
        int st = sc && !IsBadReadPtr(sc, 8) ? *(int *)(sc + 4) : -1;
        static int last_st = -1;
        if (st != last_st) { last_st = st; if (g_n) { lock(); g_n = 0; InterlockedIncrement(&g_ver); unlock(); } }
        if (game_in_play() && !game_menu_up() && (g_n || g_nsl)) menutext_clear();
        if (!g_n && !g_nsl) { g_nprev = -1; return 0; }
    }
    if (!g_n && !g_nsl) { g_nprev = -1; return 0; }
    LONG v = g_ver;
    if (v == built && g_ow == dw && g_oh == dh && g_out && g_nprev >= 0) { *pix = g_out; *ver = (unsigned)v; return 1; }
    int full = g_nprev < 0;
    if (g_ow != dw || g_oh != dh || !g_out) {
        free(g_out); free(g_cov);
        g_out = malloc((size_t)dw * dh * 4); g_cov = malloc(1);
        g_ow = dw; g_oh = dh; full = 1;
        if (!g_out) { g_ow = g_oh = 0; return 0; }
        tc_flush();
    }
    if (!g_dc) g_dc = CreateCompatibleDC(NULL);
    if (!g_dc) return 0;
    if (!g_bmp || bw != dw || bh != dh) {
        if (g_bmp) DeleteObject(g_bmp);
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), dw, -dh, 1, 32, BI_RGB } };
        g_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, (void **)&g_bits, NULL, 0);
        if (!g_bmp) { bw = bh = 0; return 0; }
        SelectObject(g_dc, g_bmp);
        memset(g_bits, 0, (size_t)dw * dh * 4);
        bw = dw; bh = dh; full = 1;
        tc_flush();
    }
    SetBkMode(g_dc, TRANSPARENT);
    SetTextColor(g_dc, RGB(255, 255, 255));
    float sx = (float)dw / pw, sy = (float)dh / ph, k = text_scale();
    int vx = 0, vy = 0;
    if (!layout_view_is_gameplay()) { RECT view = view_current(); if (view.right > view.left) { vx = view.left; vy = view.top; } }
    static Label lab[MT_MAX]; static MtSlider sl[MT_SLIDERS]; int n, ns;
    lock(); n = g_n; memcpy(lab, g_lab, sizeof lab[0] * n); ns = g_nsl; memcpy(sl, g_sl, sizeof sl[0] * ns); built = g_ver; unlock();
    { BYTE *sc = SCRN_FULL; int st = sc && !IsBadReadPtr(sc, 8) ? *(int *)(sc + 4) : -1; if (st != g_sl_state) ns = 0; }   /* Esc left that screen */
    /* this frame's items */
    static Item items[IT_MAX]; int ni = 0;
    for (int i = 0; i < n && ni < IT_MAX; i++) {
        const Label *l = &lab[i];
        int caps = lstrlenA(l->text) >= 2;
        for (const char *c = l->text; *c && caps; c++) caps = !(*c >= 'a' && *c <= 'z');
        float kl = (caps ? 1.0f : k) * l->fk;
        HFONT f = font_px(l->font, sy * kl, sx * kl);
        const TextCov *t = f ? text_cov(l->text, f) : NULL;
        if (!t) continue;
        Item *it = &items[ni++]; memset(it, 0, sizeof *it);
        it->kind = 0; it->tc = t;
        colour_rgb(l->colour, &it->cr, &it->cg, &it->cb);
        int lum = (it->cr * 30 + it->cg * 59 + it->cb * 11) / 100;
        if (lum > 60) { it->sh = (int)(sx * 0.6f + 0.5f); if (it->sh < 1) it->sh = 1; }
        float cy = (l->y - vy + l->h * 0.5f) * sy, tw = (float)(t->w - 4);
        it->tx = (l->cx != NO_CENTRE ? (int)((l->cx - vx) * sx - tw * 0.5f) : (int)((l->x - vx) * sx)) - 2;
        it->ty = (int)(cy - (t->h - 4) * 0.5f) - 2;
        SetRect(&it->r, it->tx, it->ty, it->tx + t->w + it->sh, it->ty + t->h + it->sh);
    }
    for (int i = 0; i < ns && ni < IT_MAX; i++) {
        const MtSlider *s = &sl[i];
        Item *it = &items[ni++]; memset(it, 0, sizeof *it);
        it->kind = 1; it->s = *s;
        it->x0 = (s->r.left - vx) * sx; it->x1 = (s->r.right - vx) * sx; it->my = ((s->r.top + s->r.bottom) * 0.5f - vy) * sy;
        it->u = sy * s->fk; it->kr = fmaxf(4.0f, it->u * 4.5f);
        it->sh = (int)(sx * 0.6f + 0.5f); if (it->sh < 1) it->sh = 1;
        HFONT f = font_px(4, it->u * 1.1f, sx * s->fk * 1.1f);
        it->vtc = f && s->value[0] ? text_cov(s->value, f) : NULL;
        int vw = it->vtc ? it->vtc->w + it->sh : 0;
        SetRect(&it->r, (int)(it->x0 - it->kr - 3), (int)(it->my - it->kr - 4), (int)(it->x1 + it->kr + it->u * 4 + vw + 3), (int)(it->my + it->kr + it->u + 4));
        if (it->vtc) { int vh = it->vtc->h / 2 + 2; it->r.top = min(it->r.top, (int)it->my - vh); it->r.bottom = max(it->r.bottom, (int)it->my + vh + it->sh); }
    }
    /* what changed: rectangles of items that appeared, moved, changed or went */
    RECT d = { 0 };
    if (full) SetRect(&d, 0, 0, dw, dh);
    else {
        for (int i = 0; i < ni; i++) {
            int found = 0;
            for (int j = 0; j < g_nprev && !found; j++) found = item_same(&items[i], &g_prev[j]);
            if (!found) UnionRect(&d, &d, &items[i].r);
        }
        for (int j = 0; j < g_nprev; j++) {
            int found = 0;
            for (int i = 0; i < ni && !found; i++) found = item_same(&items[i], &g_prev[j]);
            if (!found) UnionRect(&d, &d, &g_prev[j].r);
        }
    }
    for (int pass = 0; pass < 3 && !IsRectEmpty(&d); pass++)   /* an item touching it is redrawn whole (no double blend) */
        for (int i = 0; i < ni; i++) { RECT t; if (IntersectRect(&t, &items[i].r, &d)) UnionRect(&d, &d, &items[i].r); }
    RECT all = { 0, 0, dw, dh };
    IntersectRect(&d, &d, &all);
    if (!IsRectEmpty(&d)) {
        for (int y = d.top; y < d.bottom; y++) memset(g_out + (size_t)y * dw + d.left, 0, (size_t)(d.right - d.left) * 4);
        for (int i = 0; i < ni; i++) { RECT t; if (IntersectRect(&t, &items[i].r, &d)) draw_item(&items[i], &d); }
    }
    memcpy(g_prev, items, sizeof items[0] * ni); g_nprev = ni;
    *dirty = d;
    *pix = g_out; *ver = (unsigned)built;
    return 1;
}
