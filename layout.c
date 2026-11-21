/* ============================================================================================
 * layout.c - the game's 640x480 interface layout on any screen size
 *
 * The game's interface (HUD, pop-ups, in-game menus, text) is laid out for 640x480. The battlefield
 * itself already follows the render size. Every place in the game that draws interface or reads
 * the cursor for it (see tooling/docs/ui-inventory.md) asks this file where things go, so a new
 * resolution needs no other change.
 *
 * Design space: 640x480. Each design position belongs to one ANCHOR:
 *   ANCHOR_HUD     rows y >= LAYOUT_HUD_TOP (the bottom panel): bottom of the screen, centred
 *   ANCHOR_CENTER  everything above it (pop-ups, signs, the pause menu): centre of the screen
 * The offset for an anchor is the difference between the design size and the render size
 * (g_cfg.width x g_cfg.height), so it is 0,0 at 640x480. Offsets only apply in gameplay (the whole
 * screen is shown); menus use the view (view.c) instead.
 *
 * Clicks go the other way: layout_pointer_to_game() turns a pointer position into the design
 * position the game expects, for the HUD and for any clickable panel that was drawn recently.
 *
 * Limits: a few places clamp positions to 640x480-based numbers; layout_patch_limits() rewrites
 * them for the render size.
 *
 * Resolution presets: layout_resolve_preset() turns [display] resolution=1080p (etc.) into the
 * render size and the whole-number display scale.
 * ============================================================================================ */
#include "common.h"
#include <string.h>
#include <math.h>

#define BACK_BUFFER_SURF (*(void **)0x4e2e4c)    /* the surface the game draws each frame into */
static int g_field_shift;                       /* battlefield rows moved up this frame */

/* ---------------------------------------------------------------- anchors */

int layout_active(void)
{
    if (g_cfg.width == LAYOUT_DESIGN_W && g_cfg.height == LAYOUT_DESIGN_H) return 0;
    RECT v = view_current();
    return v.left == 0 && v.top == 0 && v.right == g_cfg.width && v.bottom == g_cfg.height;
}

Anchor layout_anchor_at(int design_y)
{
    return design_y >= LAYOUT_HUD_TOP ? ANCHOR_HUD : ANCHOR_CENTER;
}

void layout_offset(Anchor a, int *dx, int *dy)
{
    int extra_w = g_cfg.width > LAYOUT_DESIGN_W ? g_cfg.width - LAYOUT_DESIGN_W : 0;
    int extra_h = g_cfg.height > LAYOUT_DESIGN_H ? g_cfg.height - LAYOUT_DESIGN_H : 0;
    *dx = extra_w / 2;
    *dy = a == ANCHOR_HUD ? extra_h : extra_h / 2;
}

/* ---------------------------------------------------------------- HUD zones
 * The 640-wide HUD panel is laid out in ZONES (design x):
 *   [0, 186)    message box        -> bottom-LEFT corner, enlarged x corner_scale
 *   [186, 434)  logo, weapons, health -> centred in the space between the corners, 1x; split at
 *               LAYOUT_HUD_SPLIT_X (316) for the spare-weapon column (weapons.c, width = gap)
 *   [434, 505)  air support        -> 1x, right next to the minimap
 *   [505, 640)  minimap            -> bottom-RIGHT corner, enlarged x corner_scale
 * Every HUD drawing routine asks layout_hud_dx(design x) for its offset, so the game draws each
 * zone at its 1x place (the corner zones flush with their corners); layout_scale_corners() then
 * enlarges the two corner zones in the finished frame, anchored to their corners (the enlarged
 * copy covers the 1x one). Clicks on the enlarged zones are scaled back (layout_pointer_to_game).
 * [hud] corners = 0 keeps the old centred panel; corner_scale = 1 or 2 (2 only when it fits). */
#define ZONE_MID_X   186
#define ZONE_AIR_X   434
#define ZONE_MAP_X   505
#define ZONE_LOGO_W  54           /* the logo may be covered by the enlarged message box */
#define PANEL_ROWS   (LAYOUT_DESIGN_H - LAYOUT_HUD_PANEL_Y)
#define MINIMAP_L 523             /* onscreenmaprect (0x4b48f0): the minimap picture inside the panel */
#define MINIMAP_T 389
#define MINIMAP_R 609
#define MINIMAP_B 475

int layout_hud_gap(void) { return layout_active() ? weapons_hud_gap() : 0; }

/* HUD GROUPS, each with its own size (2026-11-17, user: "a resize icon on each UI element group so they can
 * all be resized at will"):
 *   G_MSG  message box   design [0, 186)    bottom-left corner          [hud] corner_scale  (1.5)
 *   G_MID  middle part   design [186, 434)  + the spare-weapon gap, centred   [hud] centre_scale (1)
 *   G_AIR  air support   design [434, 505)  beside the minimap          [hud] air_scale     (1)
 *   G_MAP  minimap frame design [505, 625)  map + grid labels           [hud] minimap_scale (2)
 *   knob strip           design [625, 640)  bottom-right, always x KNOB_SCALE (1.5)
 * The game draws every group at 1x where its enlarged copy STARTS (left edge, bottom), so the copy always
 * covers the original; layout_scale_corners then enlarges them in the finished frame (all 1x pictures are
 * read first). A group's size eases to a new value (GROUP_ANIM_MS); while the pointer is over a group a
 * resize icon shows at its top corner - a click cycles 1 / 1.5 / 2 / 2.5 (saved to the ini). Scales are
 * in 64ths. Too wide for the screen: the message box shrinks first, then the middle, air support, minimap. */
#define ZONE_KNOB_X  625
#define ONE          64
#define KNOB_SCALE   96           /* 1.5x */
#define SC(v, q) ((v) * (q) / ONE)         /* design length -> enlarged length */
#define UN(v, q) ((v) * ONE / (q))         /* enlarged offset -> design offset */
#define GROUP_ANIM_MS 220
#define ICON_SIZE    11           /* resize icon art, pixels (drawn scaled to the corner cell) */
#define RESIZE_REPEAT_MS 10000    /* the old icon spot keeps resizing until the pointer leaves it (at most this long) */
enum { G_MSG, G_MID, G_AIR, G_MAP, G_COUNT };

typedef struct { int on, q[G_COUNT], qk, mid_x, mid_src, air_x, map_x, knob_x; } Zones;   /* screen x: where groups go; mid_src = where the game draws the middle at 1x (never left of 186) */

/* a group's size in 64ths: 1.5x .. 3x; on a screen too narrow for every group at 1.5x the floor is 1x (user:
 * "the smaller window should allow the elements to be scaled down and up so long as they fit") */
static int g_floor64 = ONE * 3 / 2;
static int scale64(float scale) { int q = (int)(scale * ONE + 0.5f); return q < g_floor64 ? g_floor64 : q > 3 * ONE ? 3 * ONE : q; }
/* the minimap group locked at one size (2026-11-20, user: "the far right UI element should be locked at 1.5x"):
 * [hud] minimap_lock (default 1.5; 0 = resizable as before). Cycling it (resize icon, Mod Settings) does nothing. */
static float minimap_lock(void)
{
    static float v = -1;
    if (v < 0) { char t[16]; GetPrivateProfileStringA("hud", "minimap_lock", "0", t, sizeof t, mod_ini_path()); v = (float)atof(t); if (v < 0) v = 0; }
    return v;
}
static float *group_cfg(int g)
{
    if (g == G_MAP && minimap_lock() > 0) { static float locked; locked = minimap_lock(); return &locked; }   /* writes are dropped */
    return g == G_MSG ? &g_cfg.hud_corner_scale : g == G_MID ? &g_cfg.hud_centre_scale : g == G_AIR ? &g_cfg.hud_air_scale : &g_cfg.hud_minimap_scale;
}
static const char *const GROUP_KEY[G_COUNT] = { "corner_scale", "centre_scale", "air_scale", "minimap_scale" };
static const char *const GROUP_NAME[G_COUNT] = { "message box", "centre", "air support", "minimap" };

/* the groups' scales now: eased toward the ini values, fixed for the whole frame (the game draws the 1x
 * zones and we enlarge them later in the same frame - both must see the same layout) */
static void scales_now(int *out)
{
    static int from[G_COUNT], to[G_COUNT], cur[G_COUNT], init; static DWORD t0[G_COUNT]; static unsigned frame = ~0u;
    if (!init) { for (int g = 0; g < G_COUNT; g++) from[g] = to[g] = cur[g] = scale64(*group_cfg(g)); init = 1; }
    if (view_frame_count() != frame) {
        frame = view_frame_count();
        DWORD now = GetTickCount();
        for (int g = 0; g < G_COUNT; g++) {
            int want = scale64(*group_cfg(g));
            if (want != to[g]) { from[g] = cur[g]; to[g] = want; t0[g] = now; }
            float t = (float)(now - t0[g]) / GROUP_ANIM_MS;
            if (t >= 1) cur[g] = to[g];
            else { float e = 1 - (1 - t) * (1 - t) * (1 - t); cur[g] = from[g] + (int)((to[g] - from[g]) * e); }   /* ease out */
        }
    }
    memcpy(out, cur, sizeof cur);
}

static int fits_as_is(const int *q);
static Zones zones(void)
{
    Zones z = { 0 };
    for (int g = 0; g < G_COUNT; g++) z.q[g] = ONE;
    z.qk = ONE;
    if (!layout_active() || !g_cfg.hud_corners) return z;
    int w = g_cfg.width, gap = layout_hud_gap();
    int mid_w = ZONE_AIR_X - ZONE_MID_X + gap, air_w = ZONE_MAP_X - ZONE_AIR_X;
    int frame_w = ZONE_KNOB_X - ZONE_MAP_X, knob_w = LAYOUT_DESIGN_W - ZONE_KNOB_X;
    int q[G_COUNT], qk = KNOB_SCALE;
    { int t[G_COUNT]; for (int g = 0; g < G_COUNT; g++) t[g] = ONE * 3 / 2; g_floor64 = fits_as_is(t) ? ONE * 3 / 2 : ONE; }
    scales_now(q);
    /* the knob strip (far right): 1.5x, but at the minimap's scale when that is below 1.5x (user 2026-11-20: "stay 1.5
     * UNLESS the minimap goes to 1 - then it should match") */
    qk = q[G_MAP] < KNOB_SCALE ? q[G_MAP] : KNOB_SCALE;
    /* Too wide for the screen (small window): ALL groups scale down by the same factor until they fit side by
     * side without overlapping (2026-11-17, user: "auto scale with the window size"; the old fit shrank the
     * message box alone over the logo). Never below 1x (the game draws them at 1x underneath). */
    int need = SC(ZONE_MID_X, q[G_MSG]) + SC(mid_w, q[G_MID]) + SC(air_w, q[G_AIR]) + SC(frame_w, q[G_MAP]) + SC(knob_w, qk);
    if (need > w) {
        for (int g = 0; g < G_COUNT; g++) { q[g] = (int)((long long)q[g] * w / need); if (q[g] < ONE) q[g] = ONE; }
        qk = (int)((long long)qk * w / need); if (qk < ONE) qk = ONE;
        for (int i = 0; i < 64; i++) {                         /* rounding: step down until it really fits */
            need = SC(ZONE_MID_X, q[G_MSG]) + SC(mid_w, q[G_MID]) + SC(air_w, q[G_AIR]) + SC(frame_w, q[G_MAP]) + SC(knob_w, qk);
            if (need <= w) break;
            int dec = 0;
            for (int g = 0; g < G_COUNT; g++) if (q[g] > ONE) { q[g]--; dec = 1; }
            if (qk > ONE) { qk--; dec = 1; }
            if (!dec) break;
        }
    }
    {
        int knob_dw = SC(knob_w, qk), map_dw = SC(frame_w, q[G_MAP]), air_dw = SC(air_w, q[G_AIR]);
        int left_end = SC(ZONE_MID_X, q[G_MSG]), right_start = w - knob_dw - map_dw - air_dw;
        int mid_dw = SC(mid_w, q[G_MID]), free_w = right_start - left_end;
        if (free_w < 0) return z;
        z.on = 1; memcpy(z.q, q, sizeof q); z.qk = qk;
        z.mid_x = left_end + (free_w - mid_dw) / 2;
        if (z.mid_x + mid_dw > right_start) z.mid_x = right_start - mid_dw;   /* still short (all at 1x): overlap the box */
        z.mid_src = z.mid_x < ZONE_MID_X ? ZONE_MID_X : z.mid_x;   /* its 1x picture must not overlap the message box's (that box draws into its own 1x area) */
        if (z.q[G_MID] == ONE) z.mid_x = z.mid_src;
        z.knob_x = w - knob_dw;
        z.map_x = z.knob_x - map_dw;
        z.air_x = z.map_x - air_dw;
        return z;
    }
}

int layout_hud_corner_scale(void) { Zones z = zones(); return z.on ? z.q[G_MSG] * 4 / ONE : 4; }   /* quarters */

int layout_hud_dx(int design_x)
{
    int dx, dy, gap = layout_hud_gap();
    layout_offset(ANCHOR_HUD, &dx, &dy);
    Zones z = zones();
    if (!z.on) return design_x < LAYOUT_HUD_SPLIT_X ? dx - gap / 2 : dx + gap - gap / 2;
    if (design_x < ZONE_MID_X) return 0;
    if (design_x < LAYOUT_HUD_SPLIT_X) return z.mid_src - ZONE_MID_X;
    if (design_x < ZONE_AIR_X) return z.mid_src - ZONE_MID_X + gap;
    if (design_x < ZONE_MAP_X) return z.air_x - ZONE_AIR_X;
    if (design_x < ZONE_KNOB_X) return z.map_x - ZONE_MAP_X;
    return z.knob_x - ZONE_KNOB_X;
}

Anchor layout_place(int *x, int *y)
{
    Anchor a = layout_anchor_at(*y);
    if (!layout_active()) return a;
    int dx, dy;
    layout_offset(a, &dx, &dy);
    *x += a == ANCHOR_HUD ? layout_hud_dx(*x) : dx;
    *y += dy;
    return a;
}

/* The enlarged pieces: screen x range of the 1x picture (src_x, w1 wide), scale, group (-1 = knobs).
 * The enlarged copy starts at src_x too (bottom-anchored), so it covers the 1x picture. */
typedef struct { int src_x, dst_x, w1, q, group; } Piece;   /* drawn in this order: the message box LAST (it may cover the logo) */
static int pieces(const Zones *z, Piece *p)
{
    int n = 0, gap = layout_hud_gap();
    if (z->q[G_MID] != ONE) p[n++] = (Piece){ z->mid_src, z->mid_x, ZONE_AIR_X - ZONE_MID_X + gap, z->q[G_MID], G_MID };
    if (z->q[G_AIR] != ONE) p[n++] = (Piece){ z->air_x, z->air_x, ZONE_MAP_X - ZONE_AIR_X, z->q[G_AIR], G_AIR };
    if (z->q[G_MAP] != ONE) p[n++] = (Piece){ z->map_x, z->map_x, ZONE_KNOB_X - ZONE_MAP_X, z->q[G_MAP], G_MAP };
    if (z->qk != ONE) p[n++] = (Piece){ z->knob_x, z->knob_x, LAYOUT_DESIGN_W - ZONE_KNOB_X, z->qk, -1 };
    if (z->q[G_MSG] != ONE || z->mid_x < ZONE_MID_X) p[n++] = (Piece){ 0, 0, ZONE_MID_X, z->q[G_MSG], G_MSG };   /* last: on top (also at 1x when the middle reaches under it) */
    return n;
}
/* a group's screen rect, enlarged (whole panel rows) */
static RECT group_rect(const Zones *z, int g)
{
    int gap = layout_hud_gap(), h = g_cfg.height;
    int x0 = g == G_MSG ? 0 : g == G_MID ? z->mid_x : g == G_AIR ? z->air_x : z->map_x;
    int w1 = g == G_MSG ? ZONE_MID_X : g == G_MID ? ZONE_AIR_X - ZONE_MID_X + gap : g == G_AIR ? ZONE_MAP_X - ZONE_AIR_X : ZONE_KNOB_X - ZONE_MAP_X;
    RECT r = { x0, h - SC(PANEL_ROWS, z->q[g]), x0 + SC(w1, z->q[g]), h };
    return r;
}

/* A game-screen point on an enlarged piece -> the point on its 1x picture. */
static void unscale_with(const Zones *z, int *x, int *y)
{
    if (!z->on) return;
    Piece p[5]; int n = pieces(z, p), h = g_cfg.height;
    for (int i = n - 1; i >= 0; i--) {                          /* the topmost piece first */
        int dw = SC(p[i].w1, p[i].q), dh = SC(PANEL_ROWS, p[i].q);
        if (*x < p[i].dst_x || *x >= p[i].dst_x + dw || *y < h - dh) continue;
        int sx = UN(*x - p[i].dst_x, p[i].q), sy = UN(*y - (h - dh), p[i].q);
        if (sx >= p[i].w1) sx = p[i].w1 - 1;
        if (sy >= PANEL_ROWS) sy = PANEL_ROWS - 1;
        *x = p[i].src_x + sx; *y = h - PANEL_ROWS + sy;
        return;
    }
}
static void unscale_corner(int *x, int *y) { Zones z = zones(); unscale_with(&z, x, y); }
/* The topmost game row any HUD pixel can be on (1x panel or an enlarged group): above it layout_on_hud_panel and
 * weapons_ui_at are always 0 (an unscaled point above every piece is the point itself). modern.c tests that row first
 * - the per-pixel test runs zones() several times (2026-11-20 perf: most of the overlay time). */
int layout_hud_top(void)
{
    int dx, dy; layout_offset(ANCHOR_HUD, &dx, &dy);
    int top = LAYOUT_HUD_PANEL_Y + dy;
    Zones z = zones();
    if (z.on) {
        Piece p[5]; int n = pieces(&z, p), h = g_cfg.height;
        for (int i = 0; i < n; i++) { int t = h - SC(PANEL_ROWS, p[i].q); if (t < top) top = t; }
    }
    return top < 0 ? 0 : top;
}
void layout_unscale(int *x, int *y) { if (layout_view_is_gameplay()) unscale_corner(x, y); }

/* Design x of a game-screen x in the HUD band, or -1 (not on the panel, e.g. the gap). */
static int hud_design_x(int x)
{
    static const int starts[] = { 0, ZONE_MID_X, LAYOUT_HUD_SPLIT_X, ZONE_AIR_X, ZONE_MAP_X, ZONE_KNOB_X, LAYOUT_DESIGN_W };
    for (int i = 0; i < 6; i++) {
        int d = x - layout_hud_dx(starts[i]);
        if (d >= starts[i] && d < starts[i + 1]) return d;
    }
    return -1;
}

/* Is the game-screen point on the HUD panel (any zone, not the gap)? */
static int art_margin(const Surf *art, const RECT *src, int d, int ry);
int layout_on_hud_panel(int x, int y)
{
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    unscale_corner(&x, &y);
    if (y < LAYOUT_HUD_PANEL_Y + dy) return 0;
    int d = hud_design_x(x);
    if (d < 0) return 0;
    /* the see-through margin above the panel art is battlefield (2026-11-17: overlays near the HUD were cut off
     * there, clicks there went to the HUD) */
    void *art_v; RECT src;
    Surf *art = hud_panel_art(&art_v, &src) ? (Surf *)art_v : NULL;
    if (art && art->bits && art->has_ck && src.right - src.left >= LAYOUT_DESIGN_W && src.bottom - src.top >= PANEL_ROWS
        && art_margin(art, &src, d, y - dy - LAYOUT_HUD_PANEL_Y)) return 0;
    return 1;
}

/* ---------------------------------------------------------------- resize icons */

/* the group under a game-screen point (enlarged rects), or -1 */
static int group_at(const Zones *z, int x, int y)
{
    POINT p = { x, y };
    for (int g = 0; g < G_COUNT; g++) { RECT r = group_rect(z, g); if (PtInRect(&r, p)) return g; }
    return -1;
}
/* the resize icon's rect for a group: top-left corner (the message box: top-right, it grows from the left) */
static int art_margin(const Surf *art, const RECT *src, int d, int ry);
/* Do these group scales (64ths) fit the screen width as they are (no shrinking)? */
static int fits_as_is(const int *q)
{
    int w = g_cfg.width, gap = layout_hud_gap();
    int mid_w = ZONE_AIR_X - ZONE_MID_X + gap, air_w = ZONE_MAP_X - ZONE_AIR_X;
    int frame_w = ZONE_KNOB_X - ZONE_MAP_X, knob_w = LAYOUT_DESIGN_W - ZONE_KNOB_X;
    int right_start = w - SC(knob_w, q[G_MAP] < KNOB_SCALE ? q[G_MAP] : KNOB_SCALE) - SC(frame_w, q[G_MAP]) - SC(air_w, q[G_AIR]);
    int free_w = right_start - SC(ZONE_MID_X, q[G_MSG]);
    return free_w >= 0 && free_w >= SC(mid_w, q[G_MID]);   /* side by side, no overlap */
}
/* next size for group g on a click: 1.5 -> 2 -> 2.5 -> 1.5, skipping sizes that do not fit this screen
 * (user 2026-11-17: in a small window the bigger sizes only shrank back - clicks seemed to do nothing) */
static const float SIZES[] = { 1.0f, 1.25f, 1.5f, 2.0f, 2.5f };
static float next_size(int g)
{
    float cur = *group_cfg(g);
    int n = (int)(sizeof SIZES / sizeof SIZES[0]), at = 0;
    for (int i = 0; i < n; i++) if (cur >= SIZES[i] - 0.01f) at = i;
    for (int step = 1; step <= n; step++) {
        float s = SIZES[(at + step) % n];
        if (s * ONE < g_floor64 - 0.5f) continue;              /* below the floor (room for 1.5x everywhere) */
        int q[G_COUNT];
        for (int k = 0; k < G_COUNT; k++) q[k] = scale64(*group_cfg(k));
        q[g] = scale64(s);
        if (fits_as_is(q)) return s;
    }
    /* nothing fits as it is (the window is narrower than the groups at their sizes): cycle anyway - the fit
     * scales the whole row, so the clicked group still grows / shrinks against the others */
    for (int step = 1; step <= n; step++) {
        float s = SIZES[(at + step) % n];
        if (s * ONE >= g_floor64 - 0.5f) return s;
    }
    return cur;
}

/* The resize icon sits exactly in the group's top corner cell (user): a CELL x CELL design-pixel square just
 * inside the frame (below the art's first solid row), scaled with the group. Top-right for the message box. */
#define CELL 14
static RECT icon_rect(const Zones *z, int g)
{
    RECT r = group_rect(z, g), i;
    int q = z->q[g];
    int d0 = g == G_MSG ? 0 : g == G_MID ? ZONE_MID_X : g == G_AIR ? ZONE_AIR_X : ZONE_MAP_X;
    int d1 = g == G_MSG ? ZONE_MID_X : g == G_MID ? LAYOUT_HUD_SPLIT_X : g == G_AIR ? ZONE_MAP_X : ZONE_KNOB_X;
    int ix = g == G_MSG ? d1 - 3 - CELL : d0 + 3;
    int row = 0;
    void *art_v; RECT src;
    Surf *art = hud_panel_art(&art_v, &src) ? (Surf *)art_v : NULL;
    if (art && art->bits && art->has_ck && src.right - src.left >= LAYOUT_DESIGN_W && src.bottom - src.top >= PANEL_ROWS)
        for (int c = ix; c < ix + CELL && c < d1; c++) {
            int t = 0;
            while (t < PANEL_ROWS - 1 && art_margin(art, &src, c, t)) t++;
            if (t > row) row = t;
        }
    int x = r.left + SC(ix - d0, q), y = r.top + SC(row + 3, q), s = SC(CELL, q);
    SetRect(&i, x, y, x + s, y + s);
    return i;
}
static int icons_shown(void) { return layout_view_is_gameplay() && layout_active() && g_cfg.hud_corners && !panel_is_open() && !layout_any_panel_open(); }

/* panel.c, left press: on a shown resize icon -> next size of that group. 1 = taken. */
static RECT last; static int last_g = -1; static DWORD last_at;   /* the last resize click (repeat window) */
int layout_resize_click(int x, int y)
{
    if (!icons_shown()) return 0;
    Zones z = zones();
    if (!z.on) return 0;
    /* a resize moves the icon away from the pointer (the group grows / shrinks from its anchored corner): a
     * click within RESIZE_REPEAT_MS on the last icon's spot is another resize of that group, not a walk */
    POINT p = { x, y };
    int g;
    if (last_g >= 0 && GetTickCount() - last_at < RESIZE_REPEAT_MS && PtInRect(&last, p)) g = last_g;
    else {
        g = group_at(&z, x, y);
        if (g < 0) return 0;
        RECT r = icon_rect(&z, g); InflateRect(&r, 1, 1);
        if (!PtInRect(&r, p)) return 0;
        last = r;
    }
    last_g = g; last_at = GetTickCount();
    float *s = group_cfg(g);
    *s = next_size(g);
    char v[8]; wsprintfA(v, "%d.%d", (int)*s, (int)(*s * 10) % 10);
    WritePrivateProfileStringA("hud", GROUP_KEY[g], v, mod_ini_path());
    mlog("layout: %s x%s", GROUP_NAME[g], v);
    return 1;
}
/* modmenu.c: the minimap's size from the settings screen */
void layout_minimap_cycle(void)
{
    float *s = group_cfg(G_MAP);
    *s = next_size(G_MAP);
    char v[8]; wsprintfA(v, "%d.%d", (int)*s, (int)(*s * 10) % 10);
    WritePrivateProfileStringA("hud", GROUP_KEY[G_MAP], v, mod_ini_path());
}
/* is the point on a shown resize icon (tips.c, controls: interface) */
int layout_resize_icon_at(int x, int y)
{
    if (!icons_shown()) return -1;
    Zones z = zones();
    int g = z.on ? group_at(&z, x, y) : -1;
    if (g < 0) return -1;
    RECT r = icon_rect(&z, g); InflateRect(&r, 1, 1);
    POINT p = { x, y };
    return PtInRect(&r, p) ? g : -1;
}

/* Finished frame, after the pieces are enlarged: the icon of the group under the pointer. A small dark box,
 * a two-headed diagonal arrow (game text green). */
void layout_draw_resize_icon(Surf *b)
{
    int px, py, font, colour;
    if (!b || !b->bits || !icons_shown() || !controls_pointer(&px, &py) || !objectives_font(&font, &colour)) return;
    Zones z = zones();
    if (!z.on) return;
    int g = group_at(&z, px, py);
    if (g < 0) return;
    static const char *const ART[ICON_SIZE] = {
        "#####......", "##.........", "#.#........", "#..#.......", "#...#......", ".....#.....",
        "......#...#", ".......#..#", "........#.#", ".........##", "......#####",
    };
    RECT r = icon_rect(&z, g);
    BYTE *shade = *(BYTE **)0x4e2448;                      /* x0.40 shade table */
    int hot = layout_resize_icon_at(px, py) == g;
    int s = r.right - r.left;                              /* the corner cell, scaled with the group */
    if (s < 6) return;
    int in = s / 7 > 1 ? s / 7 : 1;                         /* margin around the arrow inside the cell */
    for (int y = 0; y < s; y++) {
        int yy = r.top + y;
        if (yy < 0 || yy >= b->h) continue;
        BYTE *row = b->bits + (size_t)yy * b->pitch;
        for (int x = 0; x < s; x++) {
            int xx = r.left + x;
            if (xx < 0 || xx >= b->w) continue;
            int on = 0;
            if (x >= in && y >= in && x < s - in && y < s - in) {
                int ax = (x - in) * ICON_SIZE / (s - 2 * in), ay = (y - in) * ICON_SIZE / (s - 2 * in);
                on = ART[ay][ax] == '#';
            }
            int edge = x == 0 || y == 0 || x == s - 1 || y == s - 1;
            row[xx] = on || (edge && hot) ? (BYTE)colour : shade ? shade[shade[row[xx]]] : row[xx];
        }
        hdworld_clear_run(b, yy, r.left < 0 ? 0 : r.left, r.right > b->w ? b->w : r.right);
    }
}

/* ---------------------------------------------------------------- the zoom's interface mask (present.c) */

/* One row of "interface" bytes, exact: a pixel is interface where the (enlarged) HUD panel art is not
 * see-through (the minimap's picture always), or in the spare-weapon column.
 * 2026-11-20 (stutter): everything that is the same for the whole row is worked out ONCE per row - the zones, the
 * enlarged pieces this row crosses with the 1x row each one shows (what unscale_with did per pixel: pieces() and
 * four divisions for every pixel), the art's margin table. Same answers as before (checked row by row against the
 * old per-pixel code in Wine at several group sizes, sizes animating too), ~0.035 ms -> ~0.004 ms per row. Rows above every
 * HUD piece return at once. Callers: present.c (zoom mask, display thread) and modern.c (overlay pixels; it keeps
 * the rows between frames, layout_hud_mask_signature). */
static int art_margin(const Surf *art, const RECT *src, int d, int ry);
static const short *art_tops(const Surf *art, const RECT *src);
void layout_hud_mask_row(int y, BYTE *row, int w)
{
    memset(row, 0, w);
    if (!layout_active()) return;
    Zones z = zones();
    int dx, dy; layout_offset(ANCHOR_HUD, &dx, &dy);
    int h = g_cfg.height;
    void *art_v; RECT src;
    Surf *art = hud_panel_art(&art_v, &src) ? (Surf *)art_v : NULL;
    if (art && (!art->bits || !art->has_ck || src.right - src.left < LAYOUT_DESIGN_W || src.bottom - src.top < PANEL_ROWS)) art = NULL;
    const short *tops = art ? art_tops(art, &src) : NULL;     /* first solid panel row per design column (rows above = margin) */
    static const int starts[] = { 0, ZONE_MID_X, LAYOUT_HUD_SPLIT_X, ZONE_AIR_X, ZONE_MAP_X, ZONE_KNOB_X, LAYOUT_DESIGN_W };
    int off[6];
    for (int i = 0; i < 6; i++) off[i] = layout_hud_dx(starts[i]);
    RECT wr; int has_w = weapons_ui_rect(&wr);
    /* the enlarged pieces this row crosses, topmost first, each with the 1x row it shows here (unscale_with) */
    struct { int x0, x1, src_x, w1, q, sy; } act[5]; int na = 0;
    if (z.on) {
        Piece p[5]; int n = pieces(&z, p);
        for (int i = n - 1; i >= 0; i--) {
            int dw = SC(p[i].w1, p[i].q), dh = SC(PANEL_ROWS, p[i].q);
            if (y < h - dh) continue;
            int sy = UN(y - (h - dh), p[i].q);
            if (sy >= PANEL_ROWS) sy = PANEL_ROWS - 1;
            act[na].x0 = p[i].dst_x; act[na].x1 = p[i].dst_x + dw; act[na].src_x = p[i].src_x; act[na].w1 = p[i].w1; act[na].q = p[i].q;
            act[na].sy = h - PANEL_ROWS + sy; na++;
        }
    }
    int top1 = LAYOUT_HUD_PANEL_Y + dy;                       /* the 1x panel's rows on the screen: top1 .. top1 + PANEL_ROWS */
    if (!na && (y < top1 || y >= top1 + PANEL_ROWS) && !(has_w && y >= wr.top && y < wr.bottom)) return;   /* no HUD pixel on this row */
    for (int x = 0; x < w; x++) {
        int sx = x, sy = y;
        for (int k = 0; k < na; k++) {
            if (x < act[k].x0 || x >= act[k].x1) continue;
            sx = UN(x - act[k].x0, act[k].q);
            if (sx >= act[k].w1) sx = act[k].w1 - 1;
            sx += act[k].src_x; sy = act[k].sy;
            break;
        }
        if (has_w && sx >= wr.left && sx < wr.right && sy >= wr.top && sy < wr.bottom) {   /* the spare-weapon column: a copy of the weapon column - its margin */
            int ry2 = sy - top1, d2 = WEAPON_COL_SRC_X + (sx - wr.left);
            if (!(tops && ry2 >= 0 && ry2 < PANEL_ROWS && d2 >= 0 && d2 < LAYOUT_DESIGN_W && ry2 < tops[d2])) row[x] = 1;
            continue;
        }
        int ry = sy - top1;
        if (ry < 0 || ry >= PANEL_ROWS) continue;
        int d = -1;
        for (int i = 0; i < 6; i++) { int t = sx - off[i]; if (t >= starts[i] && t < starts[i + 1]) { d = t; break; } }
        if (d < 0) continue;
        if (tops && ry < tops[d]) continue;                   /* (d is inside 0 .. LAYOUT_DESIGN_W here) */
        row[x] = 1;
    }
}
/* Everything layout_hud_mask_row's answer depends on, as one number: the zones (sizes may be animating), the HUD
 * anchor, the panel art (surface, its pixels' address, key, source rectangle) and the spare-weapon column. While it
 * stays the same a row made earlier is still right (modern.c keeps its rows between frames: making ~40..345 rows
 * again every frame was most of its time - user's log 2026-11-20: 4.5 ms a frame, 10-49 ms with large HUD groups). */
unsigned layout_hud_mask_signature(void)
{
    unsigned s = layout_hud_signature();
    #define SIG_MIX(v) (s = (s ^ (unsigned)(v)) * 16777619u)
    static const int starts[] = { 0, ZONE_MID_X, LAYOUT_HUD_SPLIT_X, ZONE_AIR_X, ZONE_MAP_X, ZONE_KNOB_X };
    int dx, dy; layout_offset(ANCHOR_HUD, &dx, &dy);
    Zones z = zones();
    SIG_MIX(layout_active()); SIG_MIX(g_cfg.width); SIG_MIX(g_cfg.height); SIG_MIX(dx); SIG_MIX(dy);
    SIG_MIX(z.mid_x); SIG_MIX(z.mid_src); SIG_MIX(z.air_x); SIG_MIX(z.map_x); SIG_MIX(z.knob_x);
    for (int i = 0; i < 6; i++) SIG_MIX(layout_hud_dx(starts[i]));
    void *art_v; RECT src;
    Surf *art = hud_panel_art(&art_v, &src) ? (Surf *)art_v : NULL;
    SIG_MIX((uintptr_t)art);
    if (art) {
        SIG_MIX((uintptr_t)art->bits); SIG_MIX(art->pitch); SIG_MIX(art->has_ck); SIG_MIX(art->ck_lo); SIG_MIX(art->ck_hi);
        SIG_MIX(src.left); SIG_MIX(src.top); SIG_MIX(src.right); SIG_MIX(src.bottom);
    }
    RECT wr; int has_w = weapons_ui_rect(&wr);
    SIG_MIX(has_w);
    if (has_w) { SIG_MIX(wr.left); SIG_MIX(wr.top); SIG_MIX(wr.right); SIG_MIX(wr.bottom); }
    #undef SIG_MIX
    return s;
}
/* Is design pixel (d, panel row ry) a see-through MARGIN of the panel art (above the first solid pixel of
 * its column)? See-through pixels BELOW that are holes the game fills with other pictures (the logo, the
 * minimap, the message window): they are part of the interface and are copied when enlarged. */
/* the first solid row of every design column of the panel art (rows above it are margin); kept per art picture */
static const short *art_tops(const Surf *art, const RECT *src)
{
    static short top[LAYOUT_DESIGN_W]; static const BYTE *cached_bits; static RECT cached_src;
    if (art->bits != cached_bits || !EqualRect(src, &cached_src)) {
        cached_bits = art->bits; cached_src = *src;
        for (int c = 0; c < LAYOUT_DESIGN_W; c++) {
            int r = 0;
            for (; r < PANEL_ROWS; r++) {
                BYTE v = art->bits[(size_t)(src->top + r) * art->pitch + src->left + c];
                if (!(v >= art->ck_lo && v <= art->ck_hi)) break;
            }
            top[c] = (short)r;
        }
    }
    return top;
}
static int art_top(const Surf *art, const RECT *src, int d)
{
    if (d < 0 || d >= LAYOUT_DESIGN_W) return 0;
    return art_tops(art, src)[d];
}
static int art_margin(const Surf *art, const RECT *src, int d, int ry) { return ry < art_top(art, src, d); }

/* changes whenever the HUD layout does (present.c rebuilds the mask then) */
unsigned layout_hud_signature(void)
{
    Zones z = zones();
    unsigned s = (unsigned)z.on * 2654435761u;
    for (int g = 0; g < G_COUNT; g++) s = s * 31 + (unsigned)z.q[g];
    return s * 31 + (unsigned)z.mid_x * 7 + (unsigned)z.air_x * 13 + (unsigned)z.qk + (unsigned)layout_hud_gap() * 17;
}

/* Finished frame: enlarge the pieces, bottom-anchored. */
int layout_open_panels(RECT *out, int max);
void layout_scale_corners(Surf *b)
{
    Zones z = zones();
    if (!z.on || !b || !b->bits || !layout_view_is_gameplay()) return;
    Piece p[5]; int n = pieces(&z, p);
    if (!n) return;
    int w = g_cfg.width, h = g_cfg.height;
    if (w > b->w || h > b->h) return;
    int dx, dy; layout_offset(ANCHOR_HUD, &dx, &dy);
    /* the 1x pictures, ALL read first (an enlarged copy may cover another piece's 1x picture) */
    static BYTE tmp[5][PANEL_ROWS][LAYOUT_DESIGN_W + 128];
    int y0 = h - PANEL_ROWS;
    for (int i = 0; i < n; i++) {
        if (p[i].w1 > LAYOUT_DESIGN_W + 128 || p[i].src_x < 0 || p[i].src_x + p[i].w1 > w) { p[i].q = 0; continue; }
        for (int y = 0; y < PANEL_ROWS; y++) memcpy(tmp[i][y], b->bits + (size_t)(y0 + y) * b->pitch + p[i].src_x, p[i].w1);
    }
    void *art_v; RECT src;
    Surf *art = hud_panel_art(&art_v, &src) ? (Surf *)art_v : NULL;   /* see-through parts of the panel */
    if (art && (!art->bits || !art->has_ck || src.right - src.left < LAYOUT_DESIGN_W || src.bottom - src.top < PANEL_ROWS)) art = NULL;
    RECT keeps[6]; int nkeep = 0;                                  /* stays on top: Mod Settings, open pop-ups (signs) */
    if (modmenu_panel_rect(&keeps[0])) nkeep = 1;
    nkeep += layout_open_panels(keeps + nkeep, 5);
    static const int starts[] = { 0, ZONE_MID_X, LAYOUT_HUD_SPLIT_X, ZONE_AIR_X, ZONE_MAP_X, ZONE_KNOB_X, LAYOUT_DESIGN_W };
    int off[6];
    for (int i = 0; i < 6; i++) off[i] = layout_hud_dx(starts[i]);
    /* which pixels of the HUD band a piece painted this frame (a later piece's margin must not erase them) */
    static BYTE *painted; static size_t painted_cap;
    int band_y0 = h - SC(PANEL_ROWS, 3 * ONE);
    if (band_y0 < 0) band_y0 = 0;
    size_t need = (size_t)(h - band_y0) * w;
    if (need > painted_cap) { free(painted); painted = calloc(need, 1); painted_cap = painted ? need : 0; }
    if (painted) memset(painted, 0, need);
    for (int i = 0; i < n; i++) {
        int q = p[i].q;
        if (!q) continue;
        int dw = SC(p[i].w1, q), dh = SC(PANEL_ROWS, q), dx0 = p[i].dst_x, dy0 = h - dh;
        if (dx0 < 0 || dx0 + dw > w || dy0 < 0) continue;
        { RECT s1 = { p[i].src_x, h - PANEL_ROWS, p[i].src_x + p[i].w1, h };   /* HD HUD pictures (hdsprites) follow */
          hdsprites_scale_back(s1, dx0, dy0, q); }
        /* per column, once per piece (2026-11-20 perf: the zone search ran per pixel, and the battlefield was put
         * back into the margin ONE pixel per call - each call walks every map link; now whole runs) */
        static int col_sx[3 * LAYOUT_DESIGN_W + 256], col_top[3 * LAYOUT_DESIGN_W + 256];   /* col_top: first solid art row (above = margin) */
        if (dw > (int)(sizeof col_sx / sizeof col_sx[0])) continue;
        for (int x = 0; x < dw; x++) {
            int sx = UN(x, q);
            if (sx >= p[i].w1) sx = p[i].w1 - 1;
            int d = -1, scr = p[i].src_x + sx;
            for (int k = 0; k < 6; k++) { int t = scr - off[k]; if (t >= starts[k] && t < starts[k + 1]) { d = t; break; } }
            col_sx[x] = sx;
            col_top[x] = art ? art_top(art, &src, d >= 0 ? d : WEAPON_COL_SRC_X + (scr - (LAYOUT_HUD_SPLIT_X + off[1]))) : 0;   /* d < 0: the spare-weapon gap = a copy of the weapon column (its own top edge, 2026-11-18) */
        }
        for (int y = 0; y < dh; y++) {
            int sy = UN(y, q);
            if (sy >= PANEL_ROWS) sy = PANEL_ROWS - 1;
            int Y = dy0 + y;
            BYTE *dst = b->bits + (size_t)Y * b->pitch + dx0;
            BYTE *pr = painted && Y - band_y0 >= 0 ? painted + (size_t)(Y - band_y0) * w + dx0 : NULL;
            int run = -1;                       /* start of a run of written pixels */
            int rest = -1;                      /* start of a run of margin pixels to put the battlefield back on */
            for (int x = 0; x < dw; x++) {
                int kept = 0;
                for (int k = 0; k < nkeep && !kept; k++) kept = Y >= keeps[k].top && Y < keeps[k].bottom && dx0 + x >= keeps[k].left && dx0 + x < keeps[k].right;
                /* panel pixels only: in the art's see-through MARGIN the battlefield shows - put back from the
                 * map (the group's own 1x picture lies under it), unless an earlier piece painted it */
                int margin = !kept && sy < col_top[x];
                int restore = margin && pr && !pr[x];
                if (!restore && rest >= 0) { hdworld_restore_field(b, Y, dx0 + rest, dx0 + x); rest = -1; }
                if (kept || margin) {
                    if (run >= 0) { hdworld_clear_run(b, Y, dx0 + run, dx0 + x); run = -1; }
                    if (restore && rest < 0) rest = x;
                    continue;
                }
                dst[x] = tmp[i][sy][col_sx[x]];
                if (pr) pr[x] = 1;
                if (run < 0) run = x;
            }
            if (rest >= 0) hdworld_restore_field(b, Y, dx0 + rest, dx0 + dw);
            /* written pixels are interface now: drop the battlefield's upscale references there (a dark
             * panel pixel could equal the old tree/fence byte, and the HD tree was drawn over the panel) */
            if (run >= 0) hdworld_clear_run(b, Y, dx0 + run, dx0 + dw);
        }
    }
    (void)dx; (void)dy;
}

/* ---------------------------------------------------------------- clickable panels */

/* A panel (pop-up, sign, menu) counts as open while it keeps being drawn, or until the next whole
 * game frame after it was drawn (menus that pause the game are drawn once). */
#define PANEL_LINGER_MS 300
#define MAX_PANELS 4

typedef struct { RECT on_screen; Anchor anchor; DWORD drawn_at; unsigned frame; } Panel;
static Panel g_panels[MAX_PANELS];

void layout_note_panel(Anchor a, const RECT *on_screen)
{
    int w = on_screen->right - on_screen->left, h = on_screen->bottom - on_screen->top;
    if (w < LAYOUT_PANEL_MIN_W || h < LAYOUT_PANEL_MIN_H) return;
    int slot = 0;
    for (int i = 0; i < MAX_PANELS; i++) {
        if (EqualRect(&g_panels[i].on_screen, on_screen)) { slot = i; break; }
        if (g_panels[i].drawn_at < g_panels[slot].drawn_at) slot = i;
    }
    if (g_cfg.log >= 2 && !EqualRect(&g_panels[slot].on_screen, on_screen))
        mlog("layout: panel %ld,%ld %dx%d noted", on_screen->left, on_screen->top, w, h);
    g_panels[slot] = (Panel){ *on_screen, a, GetTickCount(), view_frame_count() };
}

static int panel_open(const Panel *p)
{
    if (!p->drawn_at) return 0;
    return GetTickCount() - p->drawn_at < PANEL_LINGER_MS || p->frame == view_frame_count();
}

/* Is any pop-up / sign / in-game menu up? (zoom.c shows the battlefield unzoomed meanwhile) */
int layout_any_panel_open(void)
{
    for (int i = 0; i < MAX_PANELS; i++) if (panel_open(&g_panels[i])) return 1;
    return 0;
}

/* pictures the layout moved (hud.c): where they are drawn and by how much - a click on one goes to its game
 * position. Kept CLICK_LINGER_MS after the last draw (the game draws such a button once). */
#define MAX_CLICKS 8
#define CLICK_LINGER_MS 30000
static struct { RECT on; int dx, dy; DWORD at; int state; } g_clicks[MAX_CLICKS];
/* 2026-11-20 (user: the win screen "requires enter again" - the mapping held 30 s, CONTINUE is drawn once and the
 * radio speech is longer): an entry now holds while the SAME screen state is up (SCREEN +4), any time */
static int screen_state_now(void) { BYTE *s = *(BYTE **)0x6a5944; return s && !IsBadReadPtr(s, 8) ? *(int *)(s + 4) : -1; }
void layout_note_click(const RECT *on, int dx, int dy)
{
    int slot = 0;
    for (int i = 0; i < MAX_CLICKS; i++) {
        if (EqualRect(&g_clicks[i].on, on)) { slot = i; break; }
        if (g_clicks[i].at < g_clicks[slot].at) slot = i;
    }
    g_clicks[slot].on = *on; g_clicks[slot].dx = dx; g_clicks[slot].dy = dy; g_clicks[slot].at = GetTickCount(); g_clicks[slot].state = screen_state_now();
}
void layout_pointer_to_game(int x, int y, int *gx, int *gy)
{
    *gx = x; *gy = y;
    if (!layout_active()) return;
    {
        POINT p = { x, y };
        DWORD now = GetTickCount();
        for (int i = 0; i < MAX_CLICKS; i++)
            if (g_clicks[i].at && (now - g_clicks[i].at < CLICK_LINGER_MS || g_clicks[i].state == screen_state_now()) && PtInRect(&g_clicks[i].on, p) && !layout_menu_scale()) {
                *gx = x - g_clicks[i].dx; *gy = y - g_clicks[i].dy;
                return;
            }
    }
    int k = layout_menu_scale();
    if (k) {                                                  /* an in-game menu is up: it is everything */
        *gx = LAYOUT_DESIGN_W / 2 + (int)floor((x - g_cfg.width / 2) * 64.0 / k);
        *gy = LAYOUT_DESIGN_H / 2 + (int)floor((y - g_cfg.height / 2) * 64.0 / k);
        return;
    }
    POINT pt = { x, y };
    int dx, dy;
    for (int i = 0; i < MAX_PANELS; i++) {                 /* open panels are on top */
        if (panel_open(&g_panels[i]) && PtInRect(&g_panels[i].on_screen, pt)) {
            layout_offset(g_panels[i].anchor, &dx, &dy);
            *gx = x - dx; *gy = y - dy;
            return;
        }
    }
    layout_offset(ANCHOR_HUD, &dx, &dy);                   /* the HUD band (zones) */
    if (layout_on_hud_panel(x, y)) {
        int ux = x, uy = y;
        unscale_corner(&ux, &uy);
        *gx = hud_design_x(ux);
        *gy = uy - dy;
        return;
    }
    *gy = y + g_field_shift;                               /* the battlefield (moved up at the map's bottom) */
}

/* ---------------------------------------------------------------- battlefield at the map's bottom
 * On wide screens the battlefield runs under the HUD panel (layout_patch_limits). The camera stops
 * at the bottom of the map, so the last rows of the map would stay hidden under the panel. When the
 * camera wants to go further (controls_field_shift_target), the finished battlefield picture is
 * moved UP by that many rows (at most the panel's 94) and the rows below it are black: there is no
 * map there. Pointer positions on the battlefield get the same offset (layout_pointer_to_game,
 * controls.c pointer_on_battlefield), so aiming and walking still land where the pointer is.
 * Wrap of FUN_0042d610 (draw the battlefield: terrain copy + every world sprite); the HUD, pop-ups
 * and minimap are drawn after it (FUN_00424ab0). */
VoidFnPtr layout_draw_battlefield_original;

int layout_field_shift(void) { return g_field_shift; }

static BYTE black_index(void)
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    int best = 0, best_v = 1 << 30;
    for (int i = 0; p && i < 256; i++) {
        int v = p->e[i].peRed + p->e[i].peGreen + p->e[i].peBlue;
        if (v < best_v) { best_v = v; best = i; }
    }
    return (BYTE)best;
}

static void layout_draw_battlefield_body(void)
{
    roofs_new_frame();
    hdsprites_new_frame();                          /* a new game picture: the old picture notes are gone */
    layout_draw_battlefield_original();
    roofs_draw_ghost((Surf *)BACK_BUFFER_SURF);         /* Sarge seen through what covers him (roofs.c) */
    g_field_shift = layout_active() ? controls_field_shift_target() : 0;
    Surf *b = (Surf *)BACK_BUFFER_SURF;
    if (!b || !b->bits) { g_field_shift = 0; return; }
    if (!g_field_shift) { fog_draw(b, 0); return; }
    int rows = b->h, shift = g_field_shift;
    for (int y = 0; y < rows - shift; y++)
        memcpy(b->bits + (size_t)y * b->pitch, b->bits + (size_t)(y + shift) * b->pitch, b->w);
    BYTE black = black_index();
    for (int y = rows - shift; y < rows; y++) memset(b->bits + (size_t)y * b->pitch, black, b->w);
    hdworld_shift_rows(b, shift);
    fog_draw(b, shift);
    if (g_cfg.log >= 3) {
        static DWORD logged;
        if (GetTickCount() - logged > 500) { logged = GetTickCount(); mlog("layout: battlefield moved up %d rows (map bottom)", shift); }
    }
}

/* ---------------------------------------------------------------- limits */

typedef struct { const char *name; uintptr_t va; int size; DWORD design; DWORD value; } Limit;

/* applied[] = the value written last time (the size can change while running: resize.c), so a
 * limit may be rewritten from its design value OR from our own earlier value */
static void patch_limit(const Limit *l, DWORD *applied)
{
    DWORD cur = 0;
    memcpy(&cur, (const void *)l->va, l->size);
    if (cur == l->value) { *applied = cur; return; }
    if (cur != l->design && !(*applied && cur == *applied)) { mlog("limit %-45s SKIPPED: unexpected value %lu", l->name, cur); return; }
    *applied = l->value;
    DWORD old;
    if (!VirtualProtect((void *)l->va, l->size, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy((void *)l->va, &l->value, l->size);
    VirtualProtect((void *)l->va, l->size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)l->va, l->size);
    mlog("limit %-45s %lu -> %lu", l->name, l->design, l->value);
}

void layout_patch_limits(void)
{
    /* (no early return at 640x480: after a resize back to it, the design values are written again) */
    DWORD w = g_cfg.width, h = g_cfg.height;
    /* The battlefield originally ends 94 rows above the bottom (the HUD panel's height). On a screen
     * wider than 640 the 640-wide HUD panel leaves room on both sides, so the battlefield is made
     * full height and the panel is drawn over it: the game shows beside the HUD instead of black. */
    DWORD field = w > LAYOUT_DESIGN_W ? h : h - 94;
    const Limit limits[] = {
        /* FUN_0040c630: battlefield view height = screen height - 94 (add ecx,-0x5e at 0x40c668);
         * the number of tile rows follows from it */
        { "battlefield: view height (imm8)",  0x40c66a, 1, 0xa2, w > LAYOUT_DESIGN_W ? 0x00 : 0xa2 },
        /* FUN_00407ed0: battlefield surface = screen height - 80 (sub edx,0x50 at 0x40831e) ->
         * height + 14 (add edx,0x0e), the same 14 spare rows below the taller view */
        { "battlefield: surface height",      0x40831f, 2, 0x50ea, w > LAYOUT_DESIGN_W ? 0x0ec2 : 0x50ea },
        /* FUN_0043eff0: Sarge's aim point on screen, clamped to the battlefield (640 x 375) */
        { "aim point: max x (cmp si)",        0x43f0f0, 2, 640, w },
        { "aim point: max x (mov edx)",       0x43f0fc, 4, 640, w },
        { "aim point: max x (mov si)",        0x43f122, 2, 640, w },
        { "aim point: max y (cmp ax)",        0x43f17a, 2, 375, field - 11 },
        { "aim point: max y (mov edx)",       0x43f17f, 4, 375, field - 11 },
        { "aim point: max y (mov word)",      0x43f188, 2, 375, field - 11 },
        /* FUN_0043f210: keyboard/joystick-moved cursor, clamped to 8..628 x 8..352 */
        { "keyboard cursor: max x (cmp)",     0x43f22b, 4, 628, w - (640 - 628) },
        { "keyboard cursor: max x (mov)",     0x43f238, 4, 628, w - (640 - 628) },
        { "keyboard cursor: max y (cmp)",     0x43f252, 4, 352, field - 34 },
    };
    static DWORD applied[sizeof limits / sizeof limits[0]];
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) patch_limit(&limits[i], &applied[i]);
}

/* ---------------------------------------------------------------- resolution presets */

typedef struct { const char *name; int w, h; } Preset;
static const Preset presets[] = {
    { "720p",  1280,  720 }, { "900p", 1600,  900 }, { "1080p", 1920, 1080 }, { "1200p", 1920, 1200 },
    { "1440p", 2560, 1440 }, { "2k",   2560, 1440 }, { "1600p", 2560, 1600 }, { "2160p", 3840, 2160 },
    { "4k",    3840, 2160 }, { "dci4k", 4096, 2160 },
};

/* Picks the render size and display scale for a screen/window of out_w x out_h: the largest whole
 * scale that keeps about the 1080p amount of battlefield (540 rows) and at least 640x480. */
static void fit(int out_w, int out_h)
{
    int scale = out_h / LAYOUT_TARGET_ROWS;
    if (scale < 1) scale = 1;
    while (scale > 1 && (out_w / scale < LAYOUT_DESIGN_W || out_h / scale < LAYOUT_DESIGN_H)) scale--;
    g_cfg.scale = scale;
    g_cfg.width = out_w / scale;
    g_cfg.height = out_h / scale;
}

int layout_resolve_preset(const char *name)
{
    if (!name || !*name || !_stricmp(name, "custom")) return 0;
    if (!_stricmp(name, "desktop")) {
        int w, h;
        desktop_size(&w, &h);
        fit(w, h);
        return 1;
    }
    for (size_t i = 0; i < sizeof presets / sizeof presets[0]; i++)
        if (!_stricmp(name, presets[i].name)) {
            fit(presets[i].w, presets[i].h);
            /* The game picture takes the SCREEN's shape (e.g. 1024x540 on a 4096x2160 screen instead
             * of 960x540), so fullscreen is filled without black bars; the height (the amount of
             * battlefield) stays the preset's. */
            int dw, dh;
            desktop_size(&dw, &dh);
            if (dw > 0 && dh > 0) {
                int w = (int)((long long)g_cfg.height * dw / dh + 1) & ~1;
                if (w >= LAYOUT_DESIGN_W) g_cfg.width = w;
            }
            return 1;
        }
    mlog("display: unknown resolution \"%s\" (use 720p, 900p, 1080p, 1200p, 1440p, 1600p, 2160p/4k, dci4k, desktop or custom)", name);
    return 0;
}

/* Gameplay is on screen (the whole game picture is shown, not a menu picture). Unlike
 * layout_active() this is also true at 640x480. */
int layout_view_is_gameplay(void)
{
    RECT v = view_current();
    return v.left == 0 && v.top == 0 && v.right == g_cfg.width && v.bottom == g_cfg.height;
}

void __cdecl layout_draw_battlefield(void) { PROF_BEGIN(t); layout_draw_battlefield_body(); PROF_END(t, PR_FIELD); }   /* timed (trace) */

/* modmenu.c: a HUD group's size (0 message box, 1 centre, 2 air support, 3 minimap): value / next */
float layout_group_scale(int g) { return g >= 0 && g < G_COUNT ? *group_cfg(g) : 1.0f; }
void layout_group_cycle(int g)
{
    if (g < 0 || g >= G_COUNT || (g == G_MAP && minimap_lock() > 0)) return;
    float *s = group_cfg(g);
    *s = next_size(g);
    char v[8]; wsprintfA(v, "%d.%d", (int)*s, (int)(*s * 10) % 10);
    WritePrivateProfileStringA("hud", GROUP_KEY[g], v, mod_ini_path());
    mlog("layout: %s x%s", GROUP_NAME[g], v);
}

/* ---------------------------------------------------------------- in-game menus, bigger (2026-11-17)
 * The game's option screens over a paused mission (pause menu, Controls, Audio, Mod Settings) are laid out for
 * 640x480. With the larger fullscreen picture (2048x1080) they covered a third of the screen and their text was
 * hard to read (user). While one is up, everything it draws on the screen is placed and sized x k around the
 * screen's centre (hud.c: pictures stretched - the HD menu art follows - text with a k times bigger font, fills),
 * and clicks are mapped back (layout_pointer_to_game). Also at 1x every row is centred (rows below 360 used to
 * go with the HUD: the Controls screen was cut off at wide sizes).
 * 2026-11-18 (user: "the menu text needs to be cleaned up for all sizes - like done in FS"): the factor is in
 * 64ths (64 = 1x) and goes in HALF steps - the largest of 1, 1.5, 2, 2.5, 3 at which 640x480 fits. Whole steps
 * left every window below 960 game pixels high (e.g. 1534x808) with the small 1x menu and its small text.
 * [display] menu_scale: 0 = auto, 1..3 fixed. */
int layout_menu_scale(void)
{
    static int cfg = -1;
    if (cfg < 0) cfg = GetPrivateProfileIntA("display", "menu_scale", 0, mod_ini_path());
    if (!layout_active() || !game_menu_up()) return 0;
    int hx = g_cfg.width * 2 / LAYOUT_DESIGN_W, hy = g_cfg.height * 2 / LAYOUT_DESIGN_H;   /* halves that fit */
    int h = cfg > 0 ? cfg * 2 : hx < hy ? hx : hy;
    h = h < 2 ? 2 : h > 6 ? 6 : h;
    return h * 32;
}
/* design point -> game screen point (k from layout_menu_scale, 64ths). Every edge goes through this, so
 * neighbouring pictures and fills meet without gaps at the half steps too. */
static int menu_floor(int v, int k) { v *= k; return v >= 0 ? v / 64 : -((-v + 63) / 64); }
void layout_menu_place(int k, int *x, int *y)
{
    *x = g_cfg.width / 2 + menu_floor(*x - LAYOUT_DESIGN_W / 2, k);
    *y = g_cfg.height / 2 + menu_floor(*y - LAYOUT_DESIGN_H / 2, k);
}

/* controls.c: is a press here a repeat resize click (see layout_resize_click)? */
int layout_resize_repeat_at(int x, int y)
{
    POINT p = { x, y };
    if (last_g >= 0 && !PtInRect(&last, p)) last_g = -1;          /* the pointer left the spot: over */
    return last_g >= 0 && GetTickCount() - last_at < RESIZE_REPEAT_MS;
}

/* layout_scale_corners: the open pop-ups (signs, briefings) - the enlarged HUD groups are not drawn over them */
int layout_open_panels(RECT *out, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_PANELS && n < max; i++) if (panel_open(&g_panels[i])) out[n++] = g_panels[i].on_screen;
    return n;
}

/* weapons.c: is panel row ry (0 = the panel's top) of design column d a see-through margin (battlefield)? */
int layout_art_margin(int d, int ry)
{
    void *art_v; RECT src;
    Surf *art = hud_panel_art(&art_v, &src) ? (Surf *)art_v : NULL;
    if (!art || !art->bits || !art->has_ck || src.right - src.left < LAYOUT_DESIGN_W || src.bottom - src.top < PANEL_ROWS) return 0;
    return ry >= 0 && ry < PANEL_ROWS && art_margin(art, &src, d, ry);
}

/* minimap.c (2026-11-20): a HUD design point -> the first pixel of the finished frame that shows design pixel
 * (x, y) after the groups are enlarged. The inverse of unscale_with with layout_scale_corners' own arithmetic
 * (frame pixel X of a piece shows 1x pixel UN(X - dst_x, q), so the first X showing s is ceil(s * q / 64)): what is
 * drawn over the enlarged minimap lines up with it and with the pointer mapping. Returns the piece's scale in
 * 64ths (64 = not enlarged). */
int layout_hud_point(int design_x, int design_y, int *sx, int *sy)
{
    int x1 = design_x, y1 = design_y;
    if (layout_active()) {
        int dx, dy;
        layout_offset(ANCHOR_HUD, &dx, &dy);
        x1 += layout_hud_dx(design_x);
        y1 += dy;
    }
    *sx = x1; *sy = y1;
    Zones z = zones();
    if (!z.on || !layout_view_is_gameplay()) return ONE;
    Piece p[5]; int n = pieces(&z, p), h = g_cfg.height;
    for (int i = n - 1; i >= 0; i--) {                          /* the topmost piece first, as unscale_with */
        if (x1 < p[i].src_x || x1 >= p[i].src_x + p[i].w1 || y1 < h - PANEL_ROWS || y1 >= h) continue;
        int q = p[i].q, dh = SC(PANEL_ROWS, q);
        *sx = p[i].dst_x + ((x1 - p[i].src_x) * q + ONE - 1) / ONE;
        *sy = h - dh + ((y1 - (h - PANEL_ROWS)) * q + ONE - 1) / ONE;
        return q;
    }
    return ONE;
}
