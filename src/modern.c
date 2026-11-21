/* ============================================================================================
 * modern.c - the modernization features (tooling/docs/modernization-plan.md), each with its own switch
 *
 * Every feature has an entry in FEATURES (ini [modern] <key> = 0/1) and a row on the "MODERN" page of
 * the F10 panel (panel.c). The mod's own keys (dodge roll, edge peek, ...) are in KEYS (ini [keys]
 * <key> = virtual-key code) and can be changed on the panel's "KEYS" page (click a row, press a key).
 * controls.c asks modern_on() for the control features and implements them where Sarge's input is
 * built; this file has the rest:
 *   - overlays drawn into the finished frame (hud.c hud_finish_frame -> modern_draw): hit markers,
 *     damage direction, off-screen arrows, ammo at the reticle, range ring, route of a click-to-walk,
 *     health bars over hurt enemies and target objects, mission stats, ping marker, radial menu. Overlay pixels are
 *     marked as interface for the upscaled battlefield (hdworld_clear_run), else it would paint over them;
 *   - gamepad polling (XInput, loaded at run time);
 *   - screen shake off (the game's shake call 0x42d616 -> modern_shake);
 *   - videos skipped with any key or click (the game only takes Enter / Esc: game binary);
 *   - colour-blind army colours (the tan ramp, palette 224..239, made violet);
 *   - half damage to Sarge.
 * ============================================================================================ */
#include "common.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define SARGE           (*(BYTE **)0x697520)
#define SARGE_VEHICLE(u) (*(int *)((u) + 0x2d0))
#define CAMERA_X        (*(short *)0x4e2de8)
#define CAMERA_Y        (*(short *)0x4e2dea)
#define MAP_NAME        ((const char *)0x4e2f38)
#define SHAKE_TIME      (*(int *)0x4e3034)          /* FUN_0040df00: shake time left (ms) */
#define LEGION_STATUS(a) (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))
#define UNIT_STRIDE     0x2c4
typedef BYTE *(__fastcall *ArmyFn)(int army);
typedef int (__fastcall *AllianceFn)(int mine, int other);
#define ARMY_RECORD     ((ArmyFn)0x41a160)
#define ALLIED          ((AllianceFn)0x41a1e0)

/* ---------------------------------------------------------------- features */

typedef struct { const char *key, *name; char size; int def; int on; } Feature;
static Feature FEATURES[MOD_COUNT] = {
    [MOD_AIM_ASSIST]   = { "aim_assist",    "Aim assist on hovered enemy",          'S', 0 },
    [MOD_ROLL]         = { "dodge_roll",    "Dodge roll key",                       'S', 1 },
    [MOD_STAMINA]      = { "stamina",       "Sprint stamina",                       'S', 0 },
    [MOD_SIDE_BUTTONS] = { "side_buttons",  "Mouse side buttons: dive / prev. weapon",'S', 1 },
    [MOD_SPRINT_TOGGLE]= { "sprint_toggle", "Shift toggles sprint (not hold)",      'S', 0 },
    [MOD_HIT_MARKERS]  = { "hit_markers",   "Hit markers",                          'S', 1 },
    [MOD_DAMAGE_DIR]   = { "damage_dir",    "Damage direction indicator",           'S', 1 },
    [MOD_ARROWS]       = { "edge_arrows",   "Off-screen enemy / objective arrows",  'S', 1 },
    [MOD_AMMO]         = { "ammo_reticle",  "Ammo count at the reticle",            'S', 1 },
    [MOD_RANGE_RING]   = { "range_reticle", "Range shown on the reticle",           'S', 1 },
    [MOD_ROUTE]        = { "route_marker",  "Click-to-walk route marker",           'S', 1 },
    [MOD_PEEK]         = { "edge_peek",     "Edge peek key",                        'S', 1 },
    [MOD_VEHICLE_ZOOM] = { "vehicle_zoom",  "No zoom while driving",                'S', 1 },
    [MOD_NO_SHAKE]     = { "no_shake",      "Turn off screen shake",                'S', 0 },
    [MOD_SKIP_VIDEO]   = { "skip_video",    "Any key / click skips videos",         'S', 1 },
    [MOD_HEALTH_BARS]  = { "health_bars",   "Health bars on hurt enemies / targets",'S', 1 },
    [MOD_STATS]        = { "stats",         "Mission stats (hold key)",             'S', 1 },
    [MOD_RESTART]      = { "restart",       "Restart key (hold 1 s: Play Again)",   'S', 1 },
    [MOD_GAMEPAD]      = { "gamepad",       "Gamepad (twin-stick)",                 'M', 1 },
    [MOD_THROW_POINTER]= { "throw_pointer", "Grenades / mortar land at the pointer",'M', 1 },
    [MOD_THROW_HOLD]   = { "throw_hold",    "Throw on release (hold = aim, circle)", 'M', 1 },
    [MOD_STANCE_HINTS] = { "stance_hints",  "Stance hints while under fire",        'S', 1 },
    [MOD_COLOURBLIND]  = { "colourblind",   "Colour-blind: tan enemies drawn blue", 'M', 0 },
    [MOD_SQUAD_MOUSE]  = { "squad_mouse",   "Ctrl + click squad orders",            'M', 1 },
    [MOD_RADIAL]       = { "radial_menu",   "Radial command menu (hold key)",       'M', 1 },
    [MOD_PING]         = { "ping",          "Ping key (marker + attack now)",       'M', 1 },
    [MOD_HALF_DAMAGE]  = { "half_damage",   "Easier: Sarge takes half damage",      'M', 0 },
};
static int g_loaded;

static void load(void)
{
    if (g_loaded) return;
    g_loaded = 1;
    for (int i = 0; i < MOD_COUNT; i++)
        FEATURES[i].on = GetPrivateProfileIntA("modern", FEATURES[i].key, FEATURES[i].def, mod_ini_path());

}

int modern_on(int f) { if (!g_loaded) load(); return f >= 0 && f < MOD_COUNT && FEATURES[f].on; }
int modern_count(void) { return MOD_COUNT; }
const char *modern_name(int f) { return f >= 0 && f < MOD_COUNT ? FEATURES[f].name : ""; }
char modern_size(int f) { return f >= 0 && f < MOD_COUNT ? FEATURES[f].size : ' '; }

static void colourblind_reapply(void);
static void colourblind_check(void);
void modern_toggle(int f)
{
    if (!g_loaded) load();
    if (f < 0 || f >= MOD_COUNT) return;
    FEATURES[f].on = !FEATURES[f].on;
    WritePrivateProfileStringA("modern", FEATURES[f].key, FEATURES[f].on ? "1" : "0", mod_ini_path());
    mlog("modern: %s %s", FEATURES[f].key, FEATURES[f].on ? "on" : "off");
    if (f == MOD_COLOURBLIND) colourblind_reapply();
}

/* ---------------------------------------------------------------- the mod's keys
 * The old KEYS page ([keys], MK_*) is part of the bindings now (bindings.c: roll, peek, stats, radial, ping,
 * restart; the old [keys] values are taken over as their defaults). */
int modern_key(int k) { return bindings_mk_vk(k); }

/* ---------------------------------------------------------------- drawing into the finished frame */

static Surf *g_b;                              /* the frame being drawn on (modern_draw) */
static int nearest(int r, int g, int b)
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    if (!p) return 255;
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = p->e[i].peRed - r, dg = p->e[i].peGreen - g, db = p->e[i].peBlue - b, d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static BYTE C_WHITE, C_RED, C_YELLOW, C_GREEN, C_BLACK, C_GREY, C_ORANGE, C_SKY, C_PATH, C_PATH_DARK;
static BYTE C_DIMYEL, C_DIMYEL_BG;              /* enemy awareness: the "could see" eye, empty meter cells */
static BYTE C_LANE_FILL, C_LANE_EDGE;            /* mission path band (lane_piece) */
static int nearest_not(int r, int g, int b, const int ex[4])   /* nearest() without the indices in ex */
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    if (!p) return 255;
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        if (i == ex[0] || i == ex[1] || i == ex[2] || i == ex[3]) continue;
        int dr = p->e[i].peRed - r, dg = p->e[i].peGreen - g, db = p->e[i].peBlue - b, d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static BYTE C_ARMY[4];                        /* target marks in the target's army colour: green tan blue grey */

/* Throw distance in a real unit (user: "show distance unit (feet, yards, etc)"): a soldier sprite is ~24 px
 * tall = a 6 ft man, so 4 world px = 1 ft. [modern] distance_unit = ft (default) | yd | m. */
#define WORLD_PX_PER_FOOT 4.0
#define RELOAD_RING_R 14            /* px: reload arc around the pointer */
static void distance_text(int px, int at_limit, char *s, int n)
{
    static char unit[4];
    if (!unit[0]) { GetPrivateProfileStringA("modern", "distance_unit", "ft", unit, sizeof unit, mod_ini_path()); CharLowerA(unit); }
    double ft = px / WORLD_PX_PER_FOOT, v = ft; const char *u = "FT";
    if (unit[0] == 'y') { v = ft / 3; u = "YD"; } else if (unit[0] == 'm') { v = ft * 0.3048; u = "M"; }
    wsprintfA(s, at_limit > 0 ? "%d %s MAX" : at_limit < 0 ? "%d %s MIN" : "%d %s", (int)(v + 0.5), u);   /* at the longest / shortest throw */
}
static void colours(void)
{
    C_WHITE = (BYTE)nearest(255, 255, 255); C_RED = (BYTE)nearest(230, 30, 30); C_YELLOW = (BYTE)nearest(255, 220, 40);
    C_GREEN = (BYTE)nearest(60, 220, 60); C_BLACK = (BYTE)nearest(0, 0, 0); C_GREY = (BYTE)nearest(150, 150, 150);
    C_ORANGE = (BYTE)nearest(255, 140, 20);
    C_DIMYEL = (BYTE)nearest(215, 185, 55); C_DIMYEL_BG = (BYTE)nearest(45, 45, 30);
    C_SKY = (BYTE)nearest(110, 190, 255);
    C_PATH = (BYTE)nearest(245, 245, 235);     /* mission path: white, 2 px + a dark edge (route marker: green dashed; target outline: yellow) */
    C_PATH_DARK = (BYTE)nearest(40, 40, 40);
    {   /* the lane colours: indices of their own (gpu.c blends exactly these bytes), never the rings' or the text's */
        int ex[4] = { C_PATH, C_PATH_DARK, C_WHITE, -1 };
        C_LANE_EDGE = (BYTE)nearest_not(250, 250, 240, ex);
        ex[3] = C_LANE_EDGE;
        C_LANE_FILL = (BYTE)nearest_not(255, 255, 255, ex);
    }
    C_ARMY[0] = (BYTE)nearest(90, 210, 70); C_ARMY[1] = (BYTE)nearest(230, 195, 120);
    C_ARMY[2] = (BYTE)nearest(80, 140, 255); C_ARMY[3] = (BYTE)nearest(195, 195, 205);
}
static int g_hud_top;                         /* layout_hud_top() for this draw (hud_rows_reset) */
/* Interface test per pixel (2026-11-20 perf): above g_hud_top there is no HUD pixel; below it, a whole row of the
 * interface mask is made (layout_hud_mask_row - the same test the zoom mask uses) instead of layout_on_hud_panel +
 * weapons_ui_at per pixel. Without the layout (640x480) the per-pixel test as before.
 * 2026-11-20 later (stutter): the rows were made again in EVERY draw, ~0.04 ms a row, and with enlarged HUD groups
 * the HUD band is the lower half of the screen (minimap x2: rows 310..539; x2.5 / x3: from 252 / 195), so the
 * mission-path band, health bars and labels down there paid for up to 230-345 rows a frame. User's log: this
 * overlay took 0.1-0.2 ms until the mission path appeared and 4.5 ms a frame from then on (of 5.8 ms game work),
 * 10-49 ms in the worst stretch; Wine (Desert 1, minimap x2): ~170 rows = 6.4 of 8.2 ms a frame. Now a row is KEPT
 * between frames while layout_hud_mask_signature() (everything the mask depends on) is unchanged; as a guard
 * against anything that number misses, a kept row is made again after HROW_KEEP (+ up to 63, staggered per row)
 * draws - one or two rows a frame. Wine, same scene: 8.2 -> 1.8 ms a frame (median 7.9 -> 0.8). */
#define HROW_KEEP 120                          /* draws (about 2 s) a kept row is trusted */
static BYTE *g_hrow; static int g_hrow_w, g_hrow_h, g_hrow_on;
static unsigned g_hrow_sig, g_hrow_draw;       /* signature of the kept rows; draw counter */
static unsigned g_hrow_made[2048];             /* draw number + 1 at which row y was made; 0 = not made */
static void hud_rows_reset(void)
{
    g_hud_top = layout_hud_top();
    g_hrow_on = layout_active() && g_cfg.width <= 4096 && g_cfg.height <= 2048;
    if (g_hrow_on && (g_hrow_w != g_cfg.width || g_hrow_h != g_cfg.height)) {
        free(g_hrow); g_hrow = malloc((size_t)g_cfg.width * g_cfg.height);
        g_hrow_w = g_hrow ? g_cfg.width : 0; g_hrow_h = g_hrow ? g_cfg.height : 0;
        memset(g_hrow_made, 0, sizeof g_hrow_made);
    }
    if (!g_hrow) g_hrow_on = 0;
    unsigned sig = g_hrow_on ? layout_hud_mask_signature() : 0;
    if (sig != g_hrow_sig) { g_hrow_sig = sig; memset(g_hrow_made, 0, sizeof g_hrow_made); }
    if (++g_hrow_draw == 0xffffffffu) { g_hrow_draw = 1; memset(g_hrow_made, 0, sizeof g_hrow_made); }
}
static int on_interface(int x, int y)
{
    if (y < g_hud_top) return 0;
    if (!g_hrow_on || y >= g_hrow_h || x < 0 || x >= g_hrow_w) return layout_on_hud_panel(x, y) || weapons_ui_at(x, y);
    BYTE *row = g_hrow + (size_t)y * g_hrow_w;
    unsigned made = g_hrow_made[y];
    if (!made || g_hrow_draw + 1 - made > (unsigned)(HROW_KEEP + (y & 63))) { layout_hud_mask_row(y, row, g_hrow_w); g_hrow_made[y] = g_hrow_draw + 1; }
    return row[x];
}
/* "Under the sprites" drawing (route line / marker): pixels where a world sprite is solid are left
 * alone - the sprite cells near the drawing are collected once (roofs.c registry) and each pixel is
 * tested with the game's sprite-mask test ObjectIntersectPoint (0x42faf0, ecx cell, edx world point). */
typedef int (__fastcall *CellHitFn)(BYTE *cell, short *point);
#define CELL_HIT ((CellHitFn)0x42faf0)
static BYTE *g_under[256]; static int g_nunder;
static int g_route;                        /* px(): drawing the route line (ground level) */
static void under_begin(int wx0, int wy0, int wx1, int wy1)
{
    static BYTE *all[2048];
    int n = roofs_cells_list(all, 2048);
    g_nunder = 0;
    for (int i = 0; i < n && g_nunder < 256; i++) {
        const int *r = (const int *)(all[i] + 0x1c);
        if (r[2] < wx0 || r[0] > wx1 || r[3] < wy0 || r[1] > wy1) continue;
        g_under[g_nunder++] = all[i];
    }
}
static int under_sprite(int x, int y)
{
    if (!g_nunder) return 0;
    int wx = x + CAMERA_X, wy = y + CAMERA_Y + layout_field_shift();
    short p[2] = { (short)wx, (short)wy };
    for (int i = 0; i < g_nunder; i++) {
        const int *r = (const int *)(g_under[i] + 0x1c);
        if (wx >= r[0] && wx < r[2] && wy >= r[1] && wy < r[3] && CELL_HIT(g_under[i], p)) return 1;
    }
    return 0;
}
static void px(int x, int y, BYTE c)
{
    Surf *b = g_b;
    if (!b || x < 0 || y < 0 || x >= g_cfg.width || y >= g_cfg.height || x >= b->w || y >= b->h || on_interface(x, y)) return;
    /* route line (g_route): above the ground, behind every object, faint where one covers it (user).
     * Upscaled battlefield on: the GPU does that (hdworld_under_run - 30 % under objects); otherwise
     * one pixel in three where the original sprite covers it (8-bit picture, no blending). */
    if (g_route && g_cfg.hd_world) {
        /* drawn over the battlefield on the screen itself (mask 1: Sarge drawn again on top of a rock or
         * tree by roofs.c, explosions ...): those are in front of the ground - the line stays behind them */
        BYTE *ov = hdworld_over_mask(b);
        if (ov && ov[(size_t)y * b->w + x] == 1) return;
        b->bits[(size_t)y * b->pitch + x] = c;
        hdworld_under_run(b, y, x, x + 1);
        return;
    }
    if (g_route && under_sprite(x, y) && (x + y) % 3) return;
    b->bits[(size_t)y * b->pitch + x] = c;
    hdworld_clear_run(b, y, x, x + 1);         /* interface pixel: the upscaled battlefield stays off it */
}
static void line(int x0, int y0, int x1, int y1, BYTE c, int dash)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy, n = 0;
    for (int guard = 0; guard < 4096; guard++, n++) {
        if (!dash || (n / dash) % 2 == 0) px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
static void circle(int cx, int cy, int r, BYTE c, int dotted)
{
    int steps = r * 6 + 8;
    for (int i = 0; i < steps; i++) {
        if (dotted && (i / 3) % 2) continue;
        double a = i * 6.2831853 / steps;
        px(cx + (int)lround(cos(a) * r), cy + (int)lround(sin(a) * r), c);   /* world px map 1:1 to the screen */
    }
}
static void box(int x0, int y0, int x1, int y1, BYTE c) { for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) px(x, y, c); }

/* ---------------------------------------------------------------- the mission path band (2026-11-20)
 * A lane as wide as a vehicle (user): LANE_W game px (jeep footprint 0x4b2b20: sides 14 + 22 px, its body about
 * 26 px wide), drawn in row RUNS - per row, the x range of a segment's band (a capsule: the segment widened by r,
 * round ends; or a flat-ended piece for dashes) is computed and filled at once (hdworld_under_fill: ground level,
 * behind objects). The per-pixel route path (px: interface test, mask, under_run per pixel) was the overlay's
 * cost. Two colours the GPU blends see-through (modern_lane -> gpu.c ps_world): fill LANE_FILL_A, edge
 * LANE_EDGE_A. Without the upscaled battlefield (8-bit only): the edge and a sparse hatch, per pixel. */
#define LANE_W        26
#define LANE_EDGE     2           /* px: the edge band inside LANE_W */
#define LANE_DASH_ON  14          /* px along a dashed leg */
#define LANE_DASH_OFF 10
#define LANE_FILL_A   0.30f       /* share of the lane colour over the upscaled ground */
#define LANE_EDGE_A   0.70f
/* gpu.c (display thread): the lane's palette indices and their opacity; x, y = -1: none */
void modern_lane(float out[4])
{
    int ok = g_loaded && C_LANE_FILL != C_LANE_EDGE;
    out[0] = ok ? (float)C_LANE_FILL : -1.0f; out[1] = ok ? (float)C_LANE_EDGE : -1.0f;
    out[2] = LANE_FILL_A; out[3] = LANE_EDGE_A;
}
/* a*x + b in [lo, hi] -> x range (intersected into *x0, *x1) */
static void lane_clip(double a, double b, double lo, double hi, double *x0, double *x1)
{
    if (fabs(a) < 1e-9) { if (b < lo || b > hi) { *x0 = 1; *x1 = 0; } return; }
    double u = (lo - b) / a, v = (hi - b) / a;
    if (u > v) { double t = u; u = v; v = t; }
    if (u > *x0) *x0 = u;
    if (v < *x1) *x1 = v;
}
/* one run of the band on row y: through the ground-level fill, the interface (HUD) left out */
static void lane_run(int y, int xa, int xb, BYTE c, int fill)
{
    Surf *b = g_b;
    if (!b || y < 0 || y >= g_cfg.height || y >= b->h) return;
    if (xa < 0) xa = 0;
    if (xb > g_cfg.width) xb = g_cfg.width;
    if (xb > b->w) xb = b->w;
    if (xb <= xa) return;
    /* 2026-11-20 (user: "the briefing left side seems to fade ... the top looks like its fading"): the lane under the
     * pinned objectives box became dithered low-res pixels in the box (the box shades the lane bytes, the GPU then
     * shows those game pixels there). The lane stays out of the box. */
    RECT ob;
    if (objectives_box(&ob) && y >= ob.top && y < ob.bottom && xb > ob.left && xa < ob.right) {
        if (xa < ob.left) lane_run(y, xa, ob.left, c, fill);
        if (xb > ob.right) lane_run(y, ob.right, xb, c, fill);
        return;
    }
    if (g_cfg.hd_world && y < g_hud_top && hdworld_under_fill(b, y, xa, xb, c)) return;
    if (g_cfg.hd_world && hdworld_over_mask(b)) {             /* HUD rows: the runs between interface pixels */
        int s = -1;
        for (int x = xa; x <= xb; x++) {
            int ok = x < xb && !on_interface(x, y);
            if (ok && s < 0) s = x;
            if (!ok && s >= 0) { hdworld_under_fill(b, y, s, x, c); s = -1; }
        }
        return;
    }
    (void)fill;
    for (int x = xa; x < xb; x++) px(x, y, c);                /* no override mask (yet): per pixel, opaque */
}
/* the band of segment (ax, ay) -> (bx, by) between t0 and t1 (0..1 along it), half width r; caps = round ends */
static void lane_piece(int ax, int ay, int bx, int by, float t0, float t1, int caps, float r, BYTE c, int fill)
{
    double dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy, L = sqrt(L2);
    double px0 = ax + dx * t0, py0 = ay + dy * t0, px1 = ax + dx * t1, py1 = ay + dy * t1;
    int ya = (int)floor((py0 < py1 ? py0 : py1) - r), yb = (int)ceil((py0 > py1 ? py0 : py1) + r);
    if (ya < 0) ya = 0;
    if (yb > g_cfg.height - 1) yb = g_cfg.height - 1;
    for (int y = ya; y <= yb; y++) {
        double yc = y + 0.5, lo = 1e9, hi = -1e9;
        if (L > 1e-6) {                                       /* the straight part: along t0..t1, across |s| <= r */
            double x0 = -1e9, x1 = 1e9;
            lane_clip(dx / L2, ((yc - ay) * dy - ax * dx) / L2, t0, t1, &x0, &x1);
            lane_clip(-dy / L, ((yc - ay) * dx + ax * dy) / L, -r, r, &x0, &x1);
            if (x0 <= x1) { lo = x0; hi = x1; }
        }
        if (caps || L <= 1e-6)                                /* round ends (joints between legs stay closed) */
            for (int k = 0; k < 2; k++) {
                double cx = k ? px1 : px0, cy = k ? py1 : py0, e = yc - cy;
                if (fabs(e) > r) continue;
                double h = sqrt(r * r - e * e);
                if (cx - h < lo) lo = cx - h;
                if (cx + h > hi) hi = cx + h;
            }
        if (hi < lo) continue;
        int xa = (int)ceil(lo - 0.5), xb = (int)floor(hi - 0.5) + 1;   /* pixel centres inside */
        if (xa < -1000000 || xb > 1000000) continue;
        lane_run(y, xa, xb, c, fill);
    }
}

/* 5x7 pixel font: digits, capitals and a few signs (5 bits per row, top row first) */
static const char GLYPHS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:/%.-+ ";
static const BYTE FONT[][7] = {
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},{0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},
    {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e},{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},{0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e},
    {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e},{0x1f,0x01,0x02,0x04,0x08,0x08,0x08},{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
    {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c},
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11},{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},{0x0e,0x11,0x10,0x10,0x10,0x11,0x0e},
    {0x1c,0x12,0x11,0x11,0x11,0x12,0x1c},{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
    {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f},{0x11,0x11,0x11,0x1f,0x11,0x11,0x11},{0x0e,0x04,0x04,0x04,0x04,0x04,0x0e},
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0c},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
    {0x11,0x1b,0x15,0x15,0x11,0x11,0x11},{0x11,0x11,0x19,0x15,0x13,0x11,0x11},{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},
    {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},{0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
    {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},{0x1f,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0e},
    {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},{0x11,0x11,0x11,0x15,0x15,0x15,0x0a},{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
    {0x11,0x11,0x11,0x0a,0x04,0x04,0x04},{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
    {0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00},{0x01,0x02,0x02,0x04,0x08,0x08,0x10},{0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c},{0x00,0x00,0x00,0x1f,0x00,0x00,0x00},{0x00,0x04,0x04,0x1f,0x04,0x04,0x00},
    {0,0,0,0,0,0,0},
};
static int text(int x, int y, const char *s, BYTE c, int shadow)
{
    for (; *s; s++, x += 6) {
        const char *g = strchr(GLYPHS, *s >= 'a' && *s <= 'z' ? *s - 32 : *s);
        if (!g || *s == ' ') continue;
        const BYTE *f = FONT[g - GLYPHS];
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 5; k++)
                if (f[r] & (0x10 >> k)) { if (shadow) px(x + k + 1, y + r + 1, C_BLACK); px(x + k, y + r, c); }
    }
    return x;
}

/* world point -> game screen (the map-bottom shift moves the battlefield rows up) */
static void to_screen(int wx, int wy, int *sx, int *sy) { *sx = wx - CAMERA_X; *sy = wy - CAMERA_Y - layout_field_shift(); }

/* ---------------------------------------------------------------- events from other files */

#define MARK_MS 220
static DWORD g_hit_at;                        /* hit marker (bullets.c modern_note_hit) */
static int g_hit_kill;
static DWORD g_dmg_at; static double g_dmg_angle;  /* damage direction */
static unsigned g_shots, g_hits; static DWORD g_mission_ms; static char g_mission[33];
static DWORD g_ping_at; static int g_ping_x, g_ping_y;

void modern_note_hit(BYTE *item)
{
    g_hits++;
    aware_note_hit(item);                     /* aware.c: the trooper's unit notices Sarge */
    if (!modern_on(MOD_HIT_MARKERS)) return;
    g_hit_at = GetTickCount();
    g_hit_kill = item && *(short *)(item + 0x1c) <= 0;
}
void modern_note_shot(void) { g_shots++; aware_note_shot(); }
void modern_note_damage(int from_x, int from_y)
{
    BYTE *u = SARGE;
    if (!u || !modern_on(MOD_DAMAGE_DIR)) return;
    int dx = from_x - *(short *)(u + 4), dy = from_y - *(short *)(u + 6);
    if (!dx && !dy) return;
    g_dmg_angle = atan2((double)dy, (double)dx);
    g_dmg_at = GetTickCount();
}
void modern_ping(int wx, int wy) { g_ping_x = wx; g_ping_y = wy; g_ping_at = GetTickCount(); }

/* ---------------------------------------------------------------- per frame (game thread) */

static int g_last_health = -1; static BYTE *g_last_sarge;
void modern_frame(void)
{
    if (!g_loaded) load();
    BYTE *u = SARGE;
    colourblind_check();
    if (strncmp(g_mission, MAP_NAME, 32) || u != g_last_sarge) {     /* a new mission (or a new Sarge) */
        if (strncmp(g_mission, MAP_NAME, 32)) { g_shots = g_hits = 0; g_mission_ms = clock_game_ms(); lstrcpynA(g_mission, MAP_NAME, sizeof g_mission); }
        g_last_sarge = u; g_last_health = -1;
    }
    /* Restart: holding the key for a second loses the mission on purpose (the game's own "Succumb" cheat,
     * 0x46b840 in the cheat table 0x4c5f10), which brings up its "Play Again? YES / NO" screen. The
     * game has no direct restart; this is its own path back into the mission. */
    {
        static DWORD down_at; static int done;
        if (modern_on(MOD_RESTART) && GetAsyncKeyState(modern_key(MK_RESTART)) < 0) {
            if (!down_at) down_at = GetTickCount();
            if (!done && GetTickCount() - down_at >= 1000) { done = 1; mlog("modern: restart (Succumb -> Play Again)"); ((VoidFnPtr)0x46b840)(); }
        } else { down_at = 0; done = 0; }
    }
    if (u && modern_on(MOD_HALF_DAMAGE)) {      /* half of every hit given back (difficulty) */
        int h = *(short *)(u + 0x1c);
        if (g_last_health > 0 && h > 0 && h < g_last_health) { h += (g_last_health - h) / 2; *(short *)(u + 0x1c) = (short)h; }
        g_last_health = h;
    } else g_last_health = u ? *(short *)(u + 0x1c) : -1;
    aware_frame();                              /* aware.c: enemy awareness ([modern] enemy_awareness) */
}

/* zoom.c: no zoom while Sarge drives (vehicles move fast and need the whole view) */
int modern_no_zoom_now(void)
{
    BYTE *u = SARGE;
    return modern_on(MOD_VEHICLE_ZOOM) && u && SARGE_VEHICLE(u);
}

/* The game's shake call (0x42d616 -> FUN_0040df00, no arguments): with "no shake" the shake time is
 * cleared first, so the game resets the shake offsets itself. */
void __cdecl modern_shake(void)
{
    if (modern_on(MOD_NO_SHAKE)) SHAKE_TIME = 0;
    ((VoidFnPtr)0x40df00)();
}

/* Window hook: any key or click skips a video (the game's movie handler takes only Enter / Esc). */
int modern_message(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    /* also the title screen after the intro (Title_legal.bmp; the game only takes Esc there - user 2026-11-17) */
    int title = !layout_view_is_gameplay() && !view_is_video() && hdsprites_showing("title_legal");
    if (!modern_on(MOD_SKIP_VIDEO) || !(view_is_video() || hdvideo_playing() || title)) return 0;
    if ((msg == WM_KEYDOWN && wp != VK_RETURN && wp != VK_ESCAPE) || msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        PostMessageA(h, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
        PostMessageA(h, WM_KEYUP, VK_ESCAPE, 0xC0010001);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- colour-blind enemy colour
 * The game colours units through one 256-byte translation table per army (game binary
 * unit_color_tables, exe 0x4f75e8: 4 pointers; UnitColorTable = 0x418ba0): entries 1..16 are the
 * palette indices of the army's 16-colour ramp (green 208.., tan 224.., blue 176.., grey 192..).
 * For red-green colour blindness the tan army's table is pointed at the blue ramp (tan vs green is
 * the hard pair; blue vs green is not). Only units change - the palette is untouched (the tan ramp's
 * palette entries are also used by terrain). The upscaled units follow by themselves: hdworld.c reads
 * the army from the table a sprite is drawn with. Switching off puts the game's values back. */
#define UNIT_COLOR_TABLES ((BYTE **)0x4f75e8)
#define TAN_FIRST  224
#define BLUE_FIRST 176
static BYTE *g_cb_table; static BYTE g_cb_saved[17];
void modern_palette(Pal *p, DWORD start, DWORD n) { (void)p; (void)start; (void)n; }   /* (palette left alone) */
static void colourblind_check(void)
{
    int on = modern_on(MOD_COLOURBLIND);
    if (g_cb_table && !on) {                                   /* switched off: the game's values back */
        memcpy(g_cb_table + 1, g_cb_saved + 1, 16);
        g_cb_table = NULL;
        return;
    }
    if (!on) return;
    if (g_cfg.log >= 3) { static int logged; if (!logged++) for (int a = 0; a < 4; a++) { BYTE *t = UNIT_COLOR_TABLES[a]; mlog("modern: unit colour table %d = %p [1] %d [8] %d", a, t, t ? t[1] : -1, t ? t[8] : -1); } }
    for (int a = 0; a < 4; a++) {
        BYTE *t = UNIT_COLOR_TABLES[a];
        if (!t || t[8] < TAN_FIRST || t[8] >= TAN_FIRST + 16) continue;   /* the tan army's table (as hdworld army_of) */
        if (t != g_cb_table) { memcpy(g_cb_saved, t, sizeof g_cb_saved); g_cb_table = t; mlog("modern: colour-blind: tan units drawn blue"); }
        for (int i = 1; i <= 16; i++)
            if (t[i] >= TAN_FIRST && t[i] < TAN_FIRST + 16) t[i] = (BYTE)(t[i] - TAN_FIRST + BLUE_FIRST);
    }
}
static void colourblind_reapply(void) { colourblind_check(); }

/* ---------------------------------------------------------------- gamepad (XInput) */

typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad pad; } XState;
typedef DWORD (WINAPI *XGetStateFn)(DWORD user, XState *s);
static XGetStateFn g_xget; static int g_xtried;
static ModPad g_pad; static WORD g_pad_prev;

static float axis(SHORT v, int dead)
{
    int a = v < 0 ? -(int)v : v;
    if (a <= dead) return 0;
    float f = (float)(a - dead) / (32767 - dead);
    if (f > 1) f = 1;
    return v < 0 ? -f : f;
}
/* controls.c, every gameplay frame: the pad's state (connected = 0 when there is none). */
const ModPad *modern_pad(void)
{
    memset(&g_pad, 0, sizeof g_pad);
    if (!modern_on(MOD_GAMEPAD)) return &g_pad;
    if (!g_xtried) {
        g_xtried = 1;
        const char *libs[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (int i = 0; i < 3 && !g_xget; i++) {
            HMODULE m = LoadLibraryA(libs[i]);
            if (m) g_xget = (XGetStateFn)GetProcAddress(m, "XInputGetState");
            if (g_xget) mlog("modern: gamepad via %s", libs[i]);
        }
    }
    XState s;
    if (!g_xget || g_xget(0, &s) != 0) { g_pad_prev = 0; return &g_pad; }
    g_pad.connected = 1;
    g_pad.lx = axis(s.pad.lx, 7849); g_pad.ly = -axis(s.pad.ly, 7849);   /* screen: y down */
    g_pad.rx = axis(s.pad.rx, 8689); g_pad.ry = -axis(s.pad.ry, 8689);
    g_pad.lt = s.pad.lt / 255.0f; g_pad.rt = s.pad.rt / 255.0f;
    g_pad.held = s.pad.buttons;
    g_pad.pressed = (WORD)(s.pad.buttons & ~g_pad_prev);
    g_pad_prev = s.pad.buttons;
    return &g_pad;
}

/* ---------------------------------------------------------------- overlays */

/* enemy soldiers (alive, on the map) and enemy vehicles: callback per item with its world centre */
typedef void (*EnemyFn)(BYTE *item, int cx, int cy, int top, int is_vehicle);
static void each_enemy(EnemyFn fn)
{
    for (int a = 1; a <= 3; a++) {
        if (!LEGION_STATUS(a) || ALLIED(0, a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg) continue;
        BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
        for (int g = 0; un && un < end && g < 64; un += UNIT_STRIDE, g++) {
            int mode = *(int *)(un + 0x70);
            if (mode < 3 || mode > 5) continue;
            int last = *(int *)(un + 0xdc);
            for (int i = 0; i <= last && i < 10; i++) {
                BYTE *t = ((BYTE **)(un + 0xb4))[i];
                if (!t || *(int *)(t + 0x68) != 1 || *(short *)(t + 0x1c) <= 0) continue;
                int cx = *(short *)(t + 4), cy = *(short *)(t + 6), top = cy - 24;
                BYTE *av = *(BYTE **)(t + 0x26c), *an = av ? *(BYTE **)(av + 0x1c) : NULL;
                if (an) { const int *r = (const int *)(an + 0x1c); cx = (r[0] + r[2]) / 2; cy = (r[1] + r[3]) / 2; top = r[1]; }
                fn(t, cx, cy, top, 0);
            }
        }
        int nv = *(signed char *)(leg + 0x50);
        for (int i = 0; i < nv && i < 50; i++) {
            BYTE *v = *(BYTE **)(leg + 0x54 + 4 * i);
            if (!v || *(short *)(v + 0x1c) <= 0 || v == capture_vehicle()) continue;   /* Sarge's captured one */
            fn(v, *(short *)(v + 4), *(short *)(v + 6), *(short *)(v + 6) - 30, 1);
        }
    }
}

/* Health bars in the world (MOD_HEALTH_BARS), over every HURT enemy soldier / vehicle (each_enemy) and,
 * since 2026-11-19 (user: "also display health bars for targets"), over every hurt TARGET object too:
 * the things the one-button mouse shoots at (controls_is_target: shooting-range boards, enemy-owned
 * turrets / pillboxes / bases, destructible buildings, gates, radar, explosives ...). Objects come from
 * the drawn-cell registry (roofs_cells_list; cell +0x0c owner item, +0x1c world RECT); big objects are
 * several parts with their own health, so each part gets its own bar over its cell. Items that got a
 * bar this frame are remembered: the bar under the reticle is left out for them (user 2026-11-19: "if
 * there is a health bar on the enemy, don't need to display one on the cursor"). */
#define HEALTH_BAR_W      16             /* bar width in pixels (fill), plus a 1 px black frame */
#define HEALTH_BAR_ABOVE  5              /* pixels above the sprite's top */
#define HEALTH_BARRED_MAX 128            /* items with a world bar per frame */
#define HEALTH_CELLS_MAX  2048
#define CELL_OWNER_OF(c)  (*(BYTE **)((c) + 0x0c))
#define CELL_RECT_OF(c)   ((const int *)((c) + 0x1c))
#define ITEM_RAW_TYPE(it) ((int)((*(DWORD *)(it) >> 22) & 0xf))
#define RAW_ASSET 7                      /* raw item types 7 asset, 8 structure, 9 obstacle = objects */
#define RAW_OBSTACLE 9
static BYTE *g_barred[HEALTH_BARRED_MAX];
static int g_nbarred;

static int has_world_bar(BYTE *it)
{
    for (int i = 0; i < g_nbarred; i++) if (g_barred[i] == it) return 1;
    return 0;
}

/* health as Sarge's HUD shows it (2026-11-20, user: "if the health bars could look like sarge's"): a row of
 * PIPS segments in a dark frame, lit from the left - the first red, the next yellow, the rest green; empty ones
 * dark. (x, y) = the row's top centre. */
#define PIPS 8
static void health_pips(int x, int y, int hp, int mx)
{
    if (mx <= 0) return;
    int lit = hp <= 0 ? 0 : (hp * PIPS + mx - 1) / mx;
    if (lit > PIPS) lit = PIPS;
    int w = PIPS * 3 + 1, x0 = x - w / 2;
    BYTE empty = (BYTE)nearest(40, 52, 36);
    box(x0 - 1, y - 1, x0 + w + 1, y + 5, C_BLACK);
    for (int i = 0; i < PIPS; i++) {
        BYTE c = i >= lit ? empty : i == 0 ? C_RED : i < 4 ? C_YELLOW : C_GREEN;
        box(x0 + 1 + i * 3, y + 1, x0 + 1 + i * 3 + 2, y + 4, c);
    }
}
static void health_bar(BYTE *it, int cx, int cy, int top, int veh)
{
    (void)cy; (void)veh;
    int hp = *(short *)(it + 0x1c), mx = *(short *)(it + 0x1e);
    if (mx <= 0 || hp >= mx || has_world_bar(it)) return;
    int sx, sy; to_screen(cx, top - HEALTH_BAR_ABOVE, &sx, &sy);
    health_pips(sx, sy - 2, hp, mx);
    if (sx >= 0 && sy >= 0 && sx < g_cfg.width && sy < g_cfg.height && g_nbarred < HEALTH_BARRED_MAX)
        g_barred[g_nbarred++] = it;      /* on screen: the reticle bar is not needed for it */
}

/* hurt target objects (not soldiers / vehicles: each_enemy has those): a bar over each drawn part */
static void target_health_bars(void)
{
    static BYTE *cells[HEALTH_CELLS_MAX];
    int n = roofs_cells_list(cells, HEALTH_CELLS_MAX);
    for (int i = 0; i < n; i++) {
        BYTE *ow = CELL_OWNER_OF(cells[i]);
        if (!ow) continue;
        int raw = ITEM_RAW_TYPE(ow);
        if (raw < RAW_ASSET || raw > RAW_OBSTACLE || !controls_is_target(ow)) continue;
        const int *r = CELL_RECT_OF(cells[i]);
        health_bar(ow, (r[0] + r[2]) / 2, (r[1] + r[3]) / 2, r[1], 0);
    }
}

static int g_near_count;
#define ARROW_RANGE 900
static int edge_arrow_at(int wx, int wy, BYTE c, int *outx, int *outy);
static void edge_arrow(int wx, int wy, BYTE c) { int x, y; edge_arrow_at(wx, wy, c, &x, &y); }
/* returns 0 (nothing drawn) when the point is on screen; else the arrow tip in *outx, *outy */
static int edge_arrow_at(int wx, int wy, BYTE c, int *outx, int *outy)
{
    int sx, sy; to_screen(wx, wy, &sx, &sy);
    int W = g_cfg.width, H = g_cfg.height - 60;          /* stay above the HUD */
    /* on screen: no arrow - unless it is behind the enlarged HUD (2026-11-20: a target walking behind the minimap or
     * message box had its marker drawn under the panel, the marker "disappeared") */
    if (sx >= 0 && sy >= 0 && sx < W && sy < H && !layout_on_hud_panel(sx, sy - 14)) return 0;
    double cx = W / 2.0, cy = H / 2.0, dx = sx - cx, dy = sy - cy;
    double t = fmin(fabs(dx) > 1e-6 ? (cx - 10) / fabs(dx) : 1e9, fabs(dy) > 1e-6 ? (cy - 10) / fabs(dy) : 1e9);
    int ax = (int)(cx + dx * t), ay = (int)(cy + dy * t);
    /* the HUD corners are enlarged (x1.5 .. x3): an arrow at the bottom of the left / right edge was drawn under
     * the message box or minimap (2026-11-19, seen in Wine) - slide it up the edge until it is clear of the panel */
    for (int k = 0; k < 400 && ay > 10 && (layout_on_hud_panel(ax, ay) || layout_on_hud_panel(ax + 8, ay + 8) || layout_on_hud_panel(ax - 8, ay + 8)); k++) ay -= 2;
    double a = atan2(dy, dx);
    for (int k = 0; k < 7; k++) {                        /* a small filled triangle pointing outward */
        int bx = ax - (int)(cos(a) * k), by = ay - (int)(sin(a) * k);
        line(bx - (int)(sin(a) * k * 0.6), by + (int)(cos(a) * k * 0.6), bx + (int)(sin(a) * k * 0.6), by - (int)(cos(a) * k * 0.6), c, 0);
    }
    *outx = ax; *outy = ay;
    return 1;
}

/* Mission targets (objectives_marks; user 2026-11-19: "an arrow pointing to where that entity is and how far
 * away it is"): off screen a yellow arrow at the edge with the distance beside it (inward); on screen a small
 * bobbing marker above the target with the distance. Distance = Sarge to the target, in [modern] distance_unit. */
static void target_mark(const ObjMark *m, int sgx_world, int sgy_world)
{
    /* 2026-11-19, user: "the blue soldier tracking would be cool if it was blue (and similar color matching)" */
    BYTE c = m->army >= 0 && m->army < 4 ? C_ARMY[m->army] : C_YELLOW;
    int dx = m->x - sgx_world, dy = m->y - sgy_world;
    char s[24]; distance_text((int)sqrt((double)dx * dx + (double)dy * dy), 0, s, sizeof s);
    int tw = lstrlenA(s) * 6, ax, ay;
    if (edge_arrow_at(m->x, m->y, c, &ax, &ay)) {
        int W = g_cfg.width, H = g_cfg.height - 60;
        double a = atan2((double)(ay - H / 2), (double)(ax - W / 2));
        int tx = ax - (int)(cos(a) * (12 + tw / 2)) - tw / 2, ty = ay - (int)(sin(a) * 14) - 3;   /* beside the arrow, inward */
        if (tx < 2) tx = 2;
        if (tx + tw > W - 2) tx = W - 2 - tw;
        if (ty < 2) ty = 2;
        if (ty > H - 10) ty = H - 10;
        text(tx, ty, s, c, 1);
        return;
    }
    int sx, sy; to_screen(m->x, m->y, &sx, &sy);
    int bob = (int)((GetTickCount() / 120) % 6); if (bob > 3) bob = 6 - bob;
    int y0 = sy - 14 - bob;
    for (int k = 0; k < 6; k++) line(sx - 5 + k, y0 + k, sx + 5 - k, y0 + k, k == 0 ? C_BLACK : c, 0);   /* a filled triangle pointing down */
    text(sx - tw / 2, y0 - 10, s, c, 1);
}

static void enemy_arrow(BYTE *it, int cx, int cy, int top, int veh)
{
    (void)it; (void)top; (void)veh;
    BYTE *u = SARGE;
    if (!u || g_near_count >= 8) return;
    int dx = cx - *(short *)(u + 4), dy = cy - *(short *)(u + 6);
    if (dx * dx + dy * dy > ARROW_RANGE * ARROW_RANGE) return;
    int sx, sy; to_screen(cx, cy, &sx, &sy);
    int on = sx >= 0 && sy >= 0 && sx < g_cfg.width && sy < g_cfg.height;
    /* on screen but hidden by the (enlarged) HUD (2026-11-20, user: "make sure the enemy indicator displays if the enemy
     * is under the UI"): a red marker pointing down, just above the HUD panel at his x */
    if (on && (layout_on_hud_panel(sx, sy) || layout_on_hud_panel(sx, sy - 12))) {
        int my = sy - 12;
        for (int k = 0; k < 400 && my > 10 && (layout_on_hud_panel(sx, my) || layout_on_hud_panel(sx - 6, my) || layout_on_hud_panel(sx + 6, my)); k++) my -= 2;
        for (int k = 0; k < 7; k++) line(sx - 6 + k, my - 8 + k, sx + 6 - k, my - 8 + k, k == 0 ? C_BLACK : C_RED, 0);   /* triangle, tip down */
        g_near_count++;
        return;
    }
    if (on && sy < g_cfg.height - 60) return;
    edge_arrow(cx, cy, C_RED);
    g_near_count++;
}

/* Radial menu (hold its key / the middle button): eight commands around the point where it was opened; move the
 * pointer toward one and release to give it; release in the middle (dead zone) = cancel. Squad orders and air
 * support use the game's own key handlers. 2026-11-20 (user: "the radial menu needs some TLC - its ugly"): restyled
 * (radial_draw: shaded disc, sectors, stencil labels, key hints, CANCEL centre); the centre is
 * moved clear of the HUD and the window edges, the pick is the pointer's MOVE since the opening (so a moved ring
 * still starts on "cancel"). Short labels (the sectors are ~35 px wide). */
static const struct { const char *name; uintptr_t handler; const char *hint; } RADIAL[8] = {
    { "FOLLOW", 0x460dc0, "F1" }, { "ATTACK", 0x460dd0, "F2" }, { "DEFEND", 0x460e00, "F3" }, { "CHARGE", 0x460e30, "" },   /* attack now */
    { "STRIKE", 0x440dc0, "F5" }, { "PARA", 0x440d30, "F6" }, { "RECON", 0x440e50, "F7" }, { "NEXT", 0x460ee0, "TAB" },
};
/* The STANCE ring (2026-11-20, user design): the radial opened with the press on Sarge (middle button) or the
 * pointer on Sarge (radial key) - six moves of Sarge himself, done by controls_stance_command (controls.c).
 * Clockwise from 30 deg (lower right; the ring is turned by RADIAL_A0 so a sector sits at the bottom): ROLL R, DIVE (bottom,
 * 2026-11-21 user: "add dive to the bottom"), PRONE, KNEEL, STAND (top), ROLL L - the stances step down the left side to
 * DIVE. Hints = the current bindings. */
static const struct { const char *name; int cmd, act; } STANCE_RADIAL[6] = {
    { "ROLL R", STANCE_CMD_ROLL_R, ACT_ROLL }, { "DIVE", STANCE_CMD_DIVE, ACT_DIVE }, { "PRONE", STANCE_CMD_PRONE, ACT_PRONE },
    { "KNEEL", STANCE_CMD_KNEEL, ACT_KNEEL }, { "STAND", STANCE_CMD_STAND, ACT_STAND }, { "ROLL L", STANCE_CMD_ROLL_L, ACT_ROLL },
};
static int g_radial_open, g_radial_x, g_radial_y, g_radial_ox, g_radial_oy, g_radial_pick = -1, g_radial_stance, g_radial_centre;
static int radial_n(void) { return g_radial_stance ? 6 : 8; }
static double radial_a0(void) { return g_radial_stance ? 6.2831853 / 12 : 0; }   /* RADIAL_A0: sector 0 starts here (stance ring: 30 deg) */
static const char *radial_name(int i) { return g_radial_stance ? STANCE_RADIAL[i].name : RADIAL[i].name; }
/* the ring's look (drawn by radial_draw in modern_draw_body) */
#define RADIAL_RADIUS 74          /* game px: outer radius of the ring */
#define RADIAL_DEAD   17          /* game px: the centre dead zone (release there = cancel) */
static char g_radial_hint[8][16];
static void radial_hints(void)
{
    for (int i = 0; i < radial_n(); i++) {
        if (g_radial_stance) bindings_short(STANCE_RADIAL[i].act, g_radial_hint[i], sizeof g_radial_hint[i]);
        else if (i == 3) bindings_short(ACT_PING, g_radial_hint[i], sizeof g_radial_hint[i]);   /* ping = attack now too */
        else lstrcpynA(g_radial_hint[i], RADIAL[i].hint, sizeof g_radial_hint[i]);
    }
}
void modern_radial(int open, int px, int py)
{
    if (open && !g_radial_open) {                 /* opened at the pointer; the ring kept clear of the HUD / edges */
        g_radial_open = 1; g_radial_pick = -1; g_radial_centre = 1;
        g_radial_stance = controls_radial_on_sarge();
        g_radial_ox = px; g_radial_oy = py;
        int m = RADIAL_RADIUS + 6, top = layout_hud_top() - RADIAL_RADIUS - 20;
        if (top > g_cfg.height - RADIAL_RADIUS - 20) top = g_cfg.height - RADIAL_RADIUS - 20;
        g_radial_x = px < m ? m : px > g_cfg.width - m ? g_cfg.width - m : px;
        g_radial_y = py > top ? top : py;
        if (g_radial_y < m) g_radial_y = m;
        if (g_cfg.log >= 2) mlog("modern: radial menu open (%s) at %d,%d (ring %d,%d)", g_radial_stance ? "stance ring" : "commands", px, py, g_radial_x, g_radial_y);
        radial_hints();
        return;
    }
    if (open) {
        int dx = px - g_radial_ox, dy = py - g_radial_oy, n = radial_n();
        g_radial_centre = dx * dx + dy * dy < RADIAL_DEAD * RADIAL_DEAD;
        g_radial_pick = g_radial_centre ? -1 : ((int)lround((atan2((double)dy, (double)dx) - radial_a0()) / (6.2831853 / n)) + 2 * n) % n;
        radial_hints();
        return;
    }
    if (g_radial_open) {
        g_radial_open = 0;
        radial_hints();
        if (g_radial_pick >= 0) {
            mlog("modern: radial menu -> %s", radial_name(g_radial_pick));
            if (g_radial_stance) controls_stance_command(STANCE_RADIAL[g_radial_pick].cmd);
            else ((VoidFnPtr)RADIAL[g_radial_pick].handler)();
        } else if (g_cfg.log >= 2) mlog("modern: radial menu cancelled (released in the centre)");
    }
}
int modern_radial_open(void) { return g_radial_open; }

/* The drag stance picker (2026-11-20, user design): press on Sarge and drag up / down - three small pose boxes
 * stacked above him (STAND top, KNEEL, PRONE, DIVE bottom - DIVE 2026-11-21; geometry STANCE_PICKER_* in common.h, the same numbers
 * controls.c picks with), the selected one green, the current pose with a yellow edge; the release picks.
 * Each box: a 9 x 9 silhouette + the name. Drawn from the game's data (Sarge's world position). */
static const char *const POSE_ICON[STANCE_PICKER_BOXES][9] = {
    { "....#....", "...###...", "....#....", "..#####..", ".#.###.#.", "...###...", "...#.#...", "...#.#...", "..##.##.." },   /* stand */
    { ".........", ".........", "....#....", "...###...", "..#####..", "...###...", "...###...", "..#####..", ".##...##." },   /* kneel */
    { ".........", ".........", ".........", ".........", ".........", ".........", "#........", "#########", ".#######." },   /* prone */
    { "......##.", ".....###.", "....###..", "...###...", "..###....", ".###.....", "###......", "##.......", "........." },   /* dive */
};
static void stance_picker_draw(void)
{
    int wx, wy, sel, cur;
    if (!controls_stance_picker(&wx, &wy, &sel, &cur)) return;
    static const char *const NAME[STANCE_PICKER_BOXES] = { "STAND", "KNEEL", "PRONE", "DIVE" };
    int sx, sy; to_screen(wx, wy, &sx, &sy);
    for (int i = 0; i < STANCE_PICKER_BOXES; i++) {
        int cy = sy - STANCE_PICKER_BASE - (STANCE_PICKER_BOXES - 1 - i) * STANCE_PICKER_STEP;     /* box centre */
        int x0 = sx - STANCE_PICKER_W / 2, x1 = sx + STANCE_PICKER_W / 2, y0 = cy - STANCE_PICKER_STEP / 2 + 1, y1 = cy + STANCE_PICKER_STEP / 2;
        BYTE bg = i == sel ? C_GREEN : C_BLACK, fg = i == sel ? C_BLACK : C_WHITE;
        box(x0, y0, x1, y1, bg);
        if (i == cur) { for (int x = x0; x < x1; x++) { px(x, y0, C_YELLOW); px(x, y1 - 1, C_YELLOW); } for (int y = y0; y < y1; y++) { px(x0, y, C_YELLOW); px(x1 - 1, y, C_YELLOW); } }
        for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++) if (POSE_ICON[i][r][c] == '#') px(x0 + 3 + c, y0 + 2 + r, fg);
        text(x0 + 15, y0 + 3, NAME[i], fg, 0);
    }
}

/* The radial ring (2026-11-20 restyle, user: "the radial menu needs some TLC - its ugly"): a darkened see-through
 * disc (the game's x0.40 shade table over the battlefield), N sectors with khaki separators and rim, the hovered
 * sector filled army green with a light outline, stencil-style capitals (the mod's 5x7 font) with the key / button
 * hint under each, the centre a darker dead zone with an X ("CANCEL" while the pointer is there), a caption under
 * the ring. Drawn into the finished frame at game resolution (whole-number output scale: crisp, render rule 3);
 * every pixel through px()/shade() so the HUD stays clear and the upscaled battlefield does not paint over it. */
#define SHADE_040 (*(BYTE **)0x4e2448)          /* palette index -> index of the same colour x0.40 (panel.c uses it) */
static void shade(int x, int y, int times)
{
    Surf *b = g_b; BYTE *t = SHADE_040;
    if (!b || !t || x < 0 || y < 0 || x >= g_cfg.width || y >= g_cfg.height || x >= b->w || y >= b->h || on_interface(x, y)) return;
    BYTE *p = &b->bits[(size_t)y * b->pitch + x];
    while (times-- > 0) *p = t[*p];
    hdworld_clear_run(b, y, x, x + 1);
}
static void text_centred(int cx, int y, const char *s, BYTE c) { text(cx - (int)strlen(s) * 3, y, s, c, 1); }
static void radial_draw(void)
{
    static BYTE olive, khaki, light, ink, red; static Pal *pal_was; static int seen;
    if (!seen || (g_dd && g_dd->pal != pal_was)) {
        olive = (BYTE)nearest(72, 100, 42); khaki = (BYTE)nearest(190, 178, 128); light = (BYTE)nearest(230, 236, 190);
        ink = (BYTE)nearest(236, 228, 196); red = (BYTE)nearest(200, 60, 40);
        pal_was = g_dd ? g_dd->pal : NULL; seen = 1;
    }
    int cx = g_radial_x, cy = g_radial_y, n = radial_n(), R1 = RADIAL_RADIUS, R0 = RADIAL_DEAD;
    const double TAU = 6.2831853;
    for (int y = -R1; y <= R1; y++)
        for (int x = -R1; x <= R1; x++) {
            int d2 = x * x + y * y;
            if (d2 > R1 * R1) continue;
            if (d2 <= R0 * R0) { shade(cx + x, cy + y, 2); continue; }        /* dead zone: darker */
            double a = atan2((double)y, (double)x); if (a < 0) a += TAU;
            int sec = ((int)floor((a - radial_a0()) / (TAU / n) + 0.5) + 2 * n) % n;
            if (sec == g_radial_pick) px(cx + x, cy + y, olive); else shade(cx + x, cy + y, 1);
        }
    for (int i = 0; i < n; i++) {                                          /* separators */
        double a = radial_a0() + (i + 0.5) * TAU / n;
        line(cx + (int)lround(cos(a) * (R0 + 1)), cy + (int)lround(sin(a) * (R0 + 1)), cx + (int)lround(cos(a) * R1), cy + (int)lround(sin(a) * R1), khaki, 0);
    }
    circle(cx, cy, R1, khaki, 0); circle(cx, cy, R1 - 1, khaki, 0); circle(cx, cy, R0, khaki, 0);
    if (g_radial_pick >= 0) {                                              /* light outline of the hovered sector */
        double a0 = radial_a0() + (g_radial_pick - 0.5) * TAU / n, a1 = radial_a0() + (g_radial_pick + 0.5) * TAU / n;
        for (int s = 0; s < 2; s++) { double a = s ? a1 : a0; line(cx + (int)lround(cos(a) * (R0 + 1)), cy + (int)lround(sin(a) * (R0 + 1)), cx + (int)lround(cos(a) * (R1 - 1)), cy + (int)lround(sin(a) * (R1 - 1)), light, 0); }
        int steps = R1 * 2;
        for (int k = 0; k <= steps; k++) {
            double a = a0 + (a1 - a0) * k / steps;
            px(cx + (int)lround(cos(a) * (R1 - 2)), cy + (int)lround(sin(a) * (R1 - 2)), light);
            px(cx + (int)lround(cos(a) * (R0 + 1)), cy + (int)lround(sin(a) * (R0 + 1)), light);
        }
    }
    for (int i = 0; i < n; i++) {                                          /* labels + hints */
        double a = radial_a0() + i * TAU / n; int r = 50;
        int tx = cx + (int)lround(cos(a) * r), ty = cy + (int)lround(sin(a) * r);
        int hot = i == g_radial_pick, two = g_radial_hint[i][0] != 0;
        int top = ty - (two ? 8 : 4);
        text_centred(tx, top, radial_name(i), hot ? light : ink);
        if (two) { char h[16]; lstrcpynA(h, g_radial_hint[i], sizeof h); CharUpperA(h); text_centred(tx, top + 9, h, hot ? ink : khaki); }
    }
    if (g_radial_centre) text_centred(cx, cy - 3, "CANCEL", red);
    else for (int k = -5; k <= 5; k++) { px(cx + k, cy + k, khaki); px(cx + k, cy - k, khaki); px(cx + k + 1, cy + k, khaki); px(cx + k + 1, cy - k, khaki); }
    const char *cap = g_radial_stance ? "STANCE" : "COMMANDS - OPEN ON SARGE: STANCE";
    int cw = (int)strlen(cap) * 6;
    for (int y = cy + R1 + 3; y < cy + R1 + 14; y++) for (int x = cx - cw / 2 - 4; x < cx + cw / 2 + 4; x++) shade(x, y, 2);
    text_centred(cx, cy + R1 + 5, cap, g_radial_stance ? light : khaki);
}
static void modern_draw_body(Surf *b)
{
    BYTE *u = SARGE;
    if (!b || !b->bits || !u || !layout_view_is_gameplay()) return;
    if (!g_loaded) load();
    g_b = b;
    hud_rows_reset();
    colours();
    DWORD now = GetTickCount();
    int sgx, sgy; to_screen(*(short *)(u + 4), *(short *)(u + 6), &sgx, &sgy);

    g_nbarred = 0;
    static double mt[6]; static DWORD mt_at; LARGE_INTEGER q0, q1, qf; QueryPerformanceFrequency(&qf);
    #define MT(i, code) do { QueryPerformanceCounter(&q0); code; QueryPerformanceCounter(&q1); mt[i] += (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart; } while (0)
    if (modern_on(MOD_HEALTH_BARS)) { MT(0, each_enemy(health_bar)); MT(1, target_health_bars()); }
    {   /* a captured enemy vehicle: a green driver in the seat (2026-11-20, user: "just drawing the driver in there") -
         * head and shoulders, a little behind the vehicle's centre along its hull facing, drawn on top */
        int vx, vy, f;
        if (capture_driver_seat(&vx, &vy, &f)) {
            double fx, fy; controls_facing_vector(f, &fx, &fy);
            int sx, sy; to_screen(vx + (int)(fx * 2), vy + (int)(fy * 2) - 6, &sx, &sy);   /* the front seats (Wine: Desert 1 jeep) */
            BYTE dark = (BYTE)nearest(30, 70, 24), mid = (BYTE)nearest(72, 140, 52);
            for (int y = -3; y <= 3; y++) for (int x = -7; x <= 7; x++)          /* shoulders: a flat oval */
                if (x * x * 9 + y * y * 49 <= 441) px(sx + x, sy + 4 + y, (x * x * 9 + y * y * 49 > 300) ? dark : mid);
            for (int y = -4; y <= 4; y++) for (int x = -4; x <= 4; x++)          /* helmet */
                if (x * x + y * y <= 16) px(sx + x, sy - 2 + y, (x * x + y * y > 10) ? dark : (x + y < -2 ? C_GREEN : mid));
        }
    }
    {   /* fire lock / cancel at the pointer (2026-11-20, user: "some indicator would be helpful"): while clicks fire
         * (fire lock after a shot) a shrinking ring + "FIRE"; just cancelled: a red X + "CANCEL" */
        int lock, canc, ppx, ppy;
        controls_fire_state(&lock, &canc);
        if ((lock || canc) && controls_pointer(&ppx, &ppy)) {
            int gx = ppx, gy = ppy;
            if (canc) {
                for (int k = -6; k <= 6; k++) { px(gx + k, gy + k, C_RED); px(gx + k, gy - k, C_RED); px(gx + k + 1, gy + k, C_RED); px(gx + k + 1, gy - k, C_RED); }
                text(gx + 10, gy + 8, "CANCEL", C_RED, 1);
            } else {
                int r = 6 + lock * 8 / 1500;
                circle(gx, gy, r, C_YELLOW, 1);
                text(gx + 10, gy + 8, "FIRE", C_YELLOW, 1);
            }
        }
    }
    {   /* an enemy vehicle at the pointer that Sarge cannot take: why (user: "show if vehicle is not empty on cursor") */
        int wx, wy, vx, vy;
        const char *why = controls_pointer_world(&wx, &wy) ? capture_hover_note(wx, wy, &vx, &vy) : NULL;
        if (why) { int sx, sy; to_screen(vx, vy, &sx, &sy); text(sx - lstrlenA(why) * 3, sy - 34, why, C_YELLOW, 1); }
    }
    if (modern_on(MOD_ARROWS)) {
        g_near_count = 0;
        each_enemy(enemy_arrow);
        int ox, oy;
        ObjMark mk[8]; int nm = objectives_marks(mk, 8);
        for (int i = 0; i < nm; i++) target_mark(&mk[i], *(short *)(u + 4), *(short *)(u + 6));
        if (!nm && objectives_target(&ox, &oy)) edge_arrow(ox, oy, C_YELLOW);   /* no named target: the sector */
    }
    MT(2, missionpath_frame());                         /* built once per mission (missionpath.c), never per frame */
    {   /* The mission path (2026-11-20, [hud] mission_path): Sarge's start -> each waypoint, painted on the ground
         * like the route marker (g_route: under objects). 2026-11-20 (user: "I want the line as wide as a vehicle"): a
         * LANE_W px band (a jeep's width), see-through white fill with a stronger 2 px edge (the GPU blends them by
         * their palette index: modern_lane, gpu.c ps_world); a leg the planner did not find is dashed (flat-ended
         * dashes). Each waypoint: a ring with its number. Drawn as row runs, only segments touching the screen. */
        QueryPerformanceCounter(&q0);
        const MissionPt *mp; int n = missionpath_points(&mp);
        if (n >= 2) {
            g_route = 1;
            int W = g_cfg.width, H = g_cfg.height;
            if (!g_cfg.hd_world) under_begin(CAMERA_X - 8, CAMERA_Y + layout_field_shift() - 8, CAMERA_X + W + 8, CAMERA_Y + layout_field_shift() + H + 8);
            int R = LANE_W / 2, m = R + 2;
            for (int pass = 0; pass < 2; pass++)            /* the edge colour first at full width, then the fill inside */
            for (int i = 0; i + 1 < n; i++) {
                int x0, y0, x1, y1; to_screen(mp[i].x, mp[i].y, &x0, &y0); to_screen(mp[i + 1].x, mp[i + 1].y, &x1, &y1);
                if ((x0 < -m && x1 < -m) || (y0 < -m && y1 < -m) || (x0 >= W + m && x1 >= W + m) || (y0 >= H + m && y1 >= H + m)) continue;
                if (!g_cfg.hd_world) {                      /* 8-bit only (no blending): the old line, 2 px white + a dark edge */
                    int tx = abs(x1 - x0) < abs(y1 - y0), ty = !tx, d = mp[i + 1].dashed ? 4 : 0;
                    if (pass == 0) { line(x0 + 2 * tx, y0 + 2 * ty, x1 + 2 * tx, y1 + 2 * ty, C_PATH_DARK, d); continue; }
                    line(x0, y0, x1, y1, C_PATH, d);
                    line(x0 + tx, y0 + ty, x1 + tx, y1 + ty, C_PATH, d);
                    continue;
                }
                float r = pass ? (float)(R - LANE_EDGE) : (float)R;
                BYTE c = pass ? C_LANE_FILL : C_LANE_EDGE;
                if (!mp[i + 1].dashed) { lane_piece(x0, y0, x1, y1, 0.0f, 1.0f, 1, r, c, pass); continue; }
                double len = sqrt((double)(x1 - x0) * (x1 - x0) + (double)(y1 - y0) * (y1 - y0));
                if (len < 1) continue;
                for (double s = 0; s < len; s += LANE_DASH_ON + LANE_DASH_OFF) {   /* flat-ended dashes; the fill stops short of the ends */
                    double e = s + LANE_DASH_ON < len ? s + LANE_DASH_ON : len, inset = pass ? LANE_EDGE : 0;
                    if (e - s < 6 || e - s <= 2 * inset) continue;   /* no stub at the leg's end */
                    lane_piece(x0, y0, x1, y1, (float)((s + inset) / len), (float)((e - inset) / len), 0, r, c, pass);
                }
            }
            for (int i = 1; i < n; i++) {                   /* waypoint: a solid ring (dark outside, white inside) */
                if (!mp[i].wp) continue;
                int wx, wy; to_screen(mp[i].x, mp[i].y, &wx, &wy);
                if (wx < -20 || wy < -20 || wx > W + 20 || wy > H + 20) continue;
                circle(wx, wy, 9, C_PATH_DARK, 0);
                circle(wx, wy, 8, C_PATH, 0);
                circle(wx, wy, 7, C_PATH, 0);
            }
            g_nunder = 0;
            g_route = 0;
            QueryPerformanceCounter(&q1); mt[3] += (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
            for (int i = 1; i < n; i++) {               /* the numbers: on top (a pad inside a building hid them) */
                if (!mp[i].wp) continue;
                int wx, wy; to_screen(mp[i].x, mp[i].y, &wx, &wy);
                if (wx < -20 || wy < -20 || wx > W + 20 || wy > H + 20) continue;
                char s[4]; wsprintfA(s, "%d", mp[i].wp);
                text(wx + 11, wy - 14, s, C_PATH, 1);
            }
        }
    }
    int rx, ry;
    if (modern_on(MOD_ROUTE)) {
        g_route = 1;
        POINT pts[40]; int n = controls_route(pts, 40);
        if (n >= 2) {                                   /* drawn under Sarge and the other sprites */
            int x0 = pts[0].x, y0 = pts[0].y, x1 = x0, y1 = y0;
            for (int i = 1; i < n; i++) { if (pts[i].x < x0) x0 = pts[i].x; if (pts[i].x > x1) x1 = pts[i].x; if (pts[i].y < y0) y0 = pts[i].y; if (pts[i].y > y1) y1 = pts[i].y; }
            under_begin(x0 - 8, y0 - 8, x1 + 8, y1 + 8);
        }
        for (int i = 0; i + 1 < n; i++) {
            int x0, y0, x1, y1; to_screen(pts[i].x, pts[i].y, &x0, &y0); to_screen(pts[i + 1].x, pts[i + 1].y, &x1, &y1);
            line(x0, y0, x1, y1, C_GREEN, 3);
        }
        if (n >= 2) {
            int ex, ey; to_screen(pts[n - 1].x, pts[n - 1].y, &ex, &ey);
            circle(ex, ey, 5, C_GREEN, 0); line(ex - 2, ey, ex + 2, ey, C_GREEN, 0); line(ex, ey - 2, ex, ey + 2, C_GREEN, 0);
        }
        g_nunder = 0;
        g_route = 0;
    }
    {   /* grenade / mortar: the blast area where the throw would land (user), on the ground like the route;
         * colour per weapon (user): grenade orange, mortar light blue; grey when the pointer is out of reach
         * (the circle sits at the longest / shortest throw). Distance label: Sarge to the burst. */
        int ix, iy, ir, wt, clamped, dist;
        if (controls_throw_preview(&ix, &iy, &ir, &wt, &clamped, &dist)) {
            BYTE c = clamped ? C_GREY : wt == 5 ? C_SKY : (wt == 4 || wt == 6) ? C_RED : C_ORANGE;   /* mortar blue, bazooka red, grenade orange */
            /* on top of everything (2026-11-17, user: drawn on the ground it vanished behind tanks / sandbags) */
            int cx, cy; to_screen(ix, iy, &cx, &cy);
            /* readable on every terrain (2026-11-21, user: "impact circles are too hard to see on some terrains"):
             * a 2 px ring in the weapon colour with a black outline inside and outside, white dashes that turn
             * slowly round it (motion is seen on busy ground), and an outlined centre cross */
            circle(cx, cy, ir + 1, C_BLACK, 0);
            circle(cx, cy, ir, c, 0);
            circle(cx, cy, ir - 1, c, 0);
            circle(cx, cy, ir - 2, C_BLACK, 0);
            {
                int steps = ir * 6 + 8, rot = (int)((GetTickCount() / 40) % (unsigned)steps);
                for (int i = 0; i < steps; i++) {
                    if (((i + rot) / 6) % 4) continue;                  /* 1 dash of 6 steps in every 24 */
                    double a = i * 6.2831853 / steps;
                    px(cx + (int)lround(cos(a) * (ir - 0.5)), cy + (int)lround(sin(a) * (ir - 0.5)), C_WHITE);
                }
            }
            for (int k = -5; k <= 5; k++) {                             /* centre cross, outlined */
                px(cx + k, cy - 1, C_BLACK); px(cx + k, cy + 1, C_BLACK); px(cx - 1, cy + k, C_BLACK); px(cx + 1, cy + k, C_BLACK);
            }
            for (int k = -4; k <= 4; k++) { px(cx + k, cy, c); px(cx, cy + k, c); }
            char s[24]; distance_text(dist, clamped, s, sizeof s);
            text(cx + 6, cy - 12, s, c, 1);
        }
    }
    if (controls_reticle(&rx, &ry)) {
        /* Range on the reticle (no circle): four corner ticks, red while the reticle is within the
         * current weapon's maximum range, grey beyond it; for grenades and mortar (hold-to-aim types 2
         * and 5) also a small bar under it: the distance as a share of the weapon's min..max range. */
        if (modern_on(MOD_RANGE_RING) && controls_reticle_visible()) {
            BYTE *shooter = SARGE_VEHICLE(u) ? (BYTE *)(intptr_t)SARGE_VEHICLE(u) : u;
            int *w = *(int **)(shooter + 0x20);
            if (w) {
                typedef int (__fastcall *RangeFn)(int type);
                int type = w[0], lo = ((RangeFn)0x445c50)(type), hi = ((RangeFn)0x445c60)(type);
                int dx = rx + CAMERA_X - *(short *)(shooter + 4), dy = ry + CAMERA_Y - *(short *)(shooter + 6);
                double d = sqrt((double)dx * dx + (double)dy * dy);
                int sy0 = ry - layout_field_shift();
                if (hi > 0) {
                    int tr = controls_target_in_range();     /* a target under the pointer decides; else the pointer's distance */
                    int ok = tr >= 0 ? tr : d <= hi;
                    if (shooter != u && !vehicles_aim_reachable(shooter, rx + CAMERA_X, ry + CAMERA_Y)) ok = 0;   /* halftrack: outside its gun arc */
                    /* height cues (2026-11-20, user: "provide an indicator to show height differences. and queues on
                     * how to fix it"): what stops the shot (bullets_shot_cue) - ticks yellow, an X there,
                     * the fix under the reticle; and the pointed ground's level against the shooter's ("UP 2") */
                    /* height_rules (bullets_shot_cue, the rules the shot follows): am2 / original (/ modded) also the levels and
                     * the ground stop; original also a round passing over the target and the machine gun's low test
                     * line from a hill; every mode Sarge's own low cover while prone */
                    int wx = rx + CAMERA_X, wy = ry + CAMERA_Y, blk = 0;
                    ShotCue cue;
                    if (type == 2 || type == 3 || type == 5 || !bullets_shot_cue(wx, wy, type, &cue)) memset(&cue, 0, sizeof cue);
                    blk = cue.block;
                    if ((blk == 1 || blk == 2) && (cue.bx - wx) * (cue.bx - wx) + (cue.by - wy) * (cue.by - wy) < 12 * 12) blk = 0;   /* stops at the target itself */
                    int l0 = bullets_level_at(*(short *)(shooter + 4), *(short *)(shooter + 6)), l1 = bullets_level_at(wx, wy);
                    if (cue.levels && l0 >= 0 && l1 >= 0 && l1 != l0) {
                        char hs[16]; wsprintfA(hs, l1 > l0 ? "UP %d" : "DOWN %d", l1 > l0 ? l1 - l0 : l0 - l1);
                        text(rx + 13, sy0 - 4, hs, l1 > l0 ? C_YELLOW : C_WHITE, 1);
                    }
                    const char *hint = NULL; char hb[48];
                    if (blk) {
                        int xs = cue.bx - CAMERA_X, ys = cue.by - CAMERA_Y - layout_field_shift();
                        for (int k = -4; k <= 4; k++) { px(xs + k, ys + k, C_YELLOW); px(xs + k, ys - k, C_YELLOW); px(xs + k + 1, ys + k, C_BLACK); px(xs + k + 1, ys - k, C_BLACK); }
                        if (blk == 3) {
                            char dn[16], upk[16]; controls_stance_keys(dn, upk, sizeof dn);
                            if (cue.raise == 0xb) wsprintfA(hb, "PRONE: KNEEL TO FIRE - %s", upk);
                            else if (cue.raise == 0xc) wsprintfA(hb, "STAND TO FIRE - %s", upk);
                            else lstrcpynA(hb, "COVER BLOCKS: MOVE", sizeof hb);
                            hint = hb;
                        } else hint = blk == 1 ? "CLIFF: GO UP" : "RIDGE: MOVE";
                    } else if (cue.over) { blk = 4; hint = "TOO HIGH: GET LOWER"; }
                    if (cue.mg_low) {                         /* where the machine gun really tests: mg_low px lower */
                        int xs = cue.mx - CAMERA_X, ys = cue.my - CAMERA_Y - layout_field_shift();
                        circle(xs, ys, 3, C_YELLOW, 0);
                        if (!hint) hint = "MG AIMS LOW FROM HILL";
                    }
                    if (hint) text(rx - (int)strlen(hint) * 3, sy0 + 28, hint, C_YELLOW, 1);
                    BYTE c = blk ? C_YELLOW : ok ? C_RED : C_GREY;
                    for (int s = -1; s <= 1; s += 2)
                        for (int t = -1; t <= 1; t += 2)
                            for (int k = 0; k < 3; k++) { px(rx + s * (9 - k), sy0 + t * 9, c); px(rx + s * 9, sy0 + t * (9 - k), c); }
                    (void)lo;   /* no throw-distance bar: it read as a health bar (user); the game shows its own while charging */
                }
            }
        }
        /* The REAL health of the target under the pointer, under the reticle. Big objects are several
         * parts with their own health each, so the bar follows the part the pointer is on. Left out
         * when that target already shows its own bar in the world (hurt: health_bar, user 2026-11-19). */
        BYTE *tg = modern_on(MOD_HEALTH_BARS) ? controls_hover_target() : NULL;
        if (tg && has_world_bar(tg)) tg = NULL;
        if (tg) {
            int hp = *(short *)(tg + 0x1c), mx = *(short *)(tg + 0x1e);
            if (mx > 0 && hp > 0) {
                health_pips(rx, ry - layout_field_shift() + 11, hp, mx);
            }
        }
        ry -= layout_field_shift();
        if (controls_click_hint()) {      /* the rifle: one shot per click - the button is still held */
            int blink = (GetTickCount() / 250) & 1;
            text(rx - 15, ry + 18, "CLICK", blink ? C_YELLOW : C_WHITE, 1);
        }
        {   /* reload: a yellow arc around the pointer fills up while a slow weapon reloads (grey = still to go) */
            double fr;
            if (controls_reload(&fr)) {
                int steps = 72;
                for (int i = 0; i < steps; i++) {
                    double a = -1.5707963 + i * 6.2831853 / steps;
                    int x = rx + (int)lround(cos(a) * RELOAD_RING_R), y = ry + (int)lround(sin(a) * RELOAD_RING_R);
                    if ((double)i / steps < fr) { px(x, y, C_YELLOW); px(x + 1, y, C_YELLOW); }
                    else if (i % 3 == 0) px(x, y, C_GREY);
                }
            }
        }
        if (modern_on(MOD_HIT_MARKERS) && now - g_hit_at < MARK_MS) {
            BYTE c = g_hit_kill ? C_RED : C_WHITE;
            for (int k = 3; k <= 6; k++) { px(rx - k, ry - k, c); px(rx + k, ry - k, c); px(rx - k, ry + k, c); px(rx + k, ry + k, c); }
        }
        if (modern_on(MOD_AMMO) && !SARGE_VEHICLE(u)) {
            int *w = *(int **)(u + 0x20);
            int type = w ? w[0] : 0, ammo = w ? *(short *)((BYTE *)w + 0xc) : 0;
            if (w && type != 9 && ammo >= 0) { char s[8]; wsprintfA(s, "%d", ammo); text(rx + 10, ry + 6, s, ammo ? C_YELLOW : C_RED, 1); }
        }
    }
    if (modern_on(MOD_DAMAGE_DIR) && now - g_dmg_at < 900) {
        for (int k = -6; k <= 6; k++) {                     /* an arc toward the attacker */
            double a = g_dmg_angle + k * 0.06;
            for (int r = 30; r <= 33; r++) px(sgx + (int)lround(cos(a) * r), sgy - 12 + (int)lround(sin(a) * r), C_RED);
        }
    }
    if (modern_on(MOD_STANCE_HINTS)) {   /* under fire (2026-11-20, user): the stance that helps, by height_rules (bullets.c) -
                                          * one line above Sarge while rounds pass near / hit him (2 s after the last) */
        char dn[16], upk[16], hs[64];
        controls_stance_keys(dn, upk, sizeof dn);
        if (bullets_stance_hint(hs, sizeof hs, dn, upk)) text(sgx - (int)strlen(hs) * 3, sgy - 44, hs, C_YELLOW, 1);
    }
    if (modern_on(MOD_PING) && now - g_ping_at < 3000) {
        int sx, sy; to_screen(g_ping_x, g_ping_y, &sx, &sy);
        int r = 4 + (int)((now - g_ping_at) / 150 % 6);
        circle(sx, sy, r, C_ORANGE, 0); circle(sx, sy, 2, C_ORANGE, 0);
    }
    if (modern_on(MOD_STAMINA)) {
        float st = controls_stamina();
        if (st < 0.999f) {
            int x = 396, y = 410;                           /* under the SPRINT label in the Sarge-health box */
            layout_place(&x, &y);
            for (int yy = y; yy < y + 3; yy++) for (int xx = x; xx < x + 36; xx++)
                if (xx >= 0 && yy >= 0 && xx < b->w && yy < b->h)
                    b->bits[(size_t)yy * b->pitch + xx] = xx - x < (int)(st * 36) ? (st < 0.25f ? C_RED : C_YELLOW) : C_BLACK;
        }
    }
    if (modern_on(MOD_STATS) && (GetAsyncKeyState(modern_key(MK_STATS)) < 0)) {
        DWORD ms = clock_game_ms() - g_mission_ms;
        char l1[48], l2[48], l3[48];
        wsprintfA(l1, "MISSION TIME %d:%02d", (int)(ms / 60000), (int)(ms / 1000 % 60));
        wsprintfA(l2, "SHOTS %u  HITS %u", g_shots, g_hits);
        wsprintfA(l3, "ACCURACY %u%%", g_shots ? g_hits * 100 / g_shots : 0);
        int x = g_cfg.width / 2 - 70, y = 60;
        box(x - 6, y - 6, x + 146, y + 34, C_BLACK);
        text(x, y, l1, C_YELLOW, 0); text(x, y + 10, l2, C_WHITE, 0); text(x, y + 20, l3, C_WHITE, 0);
    }
    if (g_radial_open) radial_draw();
    stance_picker_draw();
    g_b = NULL;
    mt[5] += 0; if (GetTickCount() - mt_at > 2000) { if (g_cfg.log >= 2) mlog("modern times 2s (ms): enemy bars %.1f target bars %.1f mpath build %.1f mpath draw %.1f", mt[0], mt[1], mt[2], mt[3]); memset(mt, 0, sizeof mt); mt_at = GetTickCount(); }
}

/* ---------------------------------------------------------------- enemy alerts (2026-11-20, user: "indicator on seeing
 * sarge would be nice too - and indicator when they lose him")
 * An enemy trooper's current target: +0x30 pVictim, its UID copied to +0x2b8 by the trooper AI tick FUN_0043cba0 every
 * tick (0 = none; cleared by FUN_0043b810 / the null-victim path; the line-of-sight test FUN_0043b2e0 failing sets the
 * 150-tick pause +0x278 and FUN_0043d310 keeps the blocked target's UID at +0x2ec). Read once a frame:
 *   +0x2b8 == Sarge's UID or the UID of the vehicle he drives, for ALERT_DEBOUNCE frames -> "!" (red) over him;
 *   it then goes to anything else for ALERT_DEBOUNCE frames -> "?" (yellow), shown ALERT_SHOW_MS and faded by blinking.
 * Enemy = another army in play and not allied with the player (0x41a1e0).
 * Units: legion (army record 0x41a160) +4 .. +8 units (stride 0x2c4, mode +0x70 3..5), troopers +0xb4[0..+0xdc].
 * 2026-11-20 later: part of ENEMY AWARENESS (aware.c, [modern] enemy_awareness; the old key enemy_alerts is its fallback).
 * "Targeting" for the "!" / wedge / "?" now also needs the trooper to have NOTICED Sarge (aware_info level 1); before
 * that, over the sprite (same place as the wedge): a yellow bar filling with his awareness, or - sees Sarge but
 * awareness 0 - a dim yellow eye. Exposure meter: 5 segments right of the SARGE HEALTH label (design EXPO_X, EXPO_Y,
 * the free strip above the SPRINT label / stamina bar): red = noticed and targeting (2 halves each), yellow = could
 * see him (1 half each). */
#define ALERT_MAX       96
#define ALERT_DEBOUNCE  3
#define ALERT_SHOW_MS   1500
#define T_VICTIM_UID(t) (*(DWORD *)((t) + 0x2b8))
typedef BYTE *(__fastcall *AlertArmyFn)(int army);
typedef int (__fastcall *AlertAllyFn)(int a, int b);
static struct { BYTE *t; DWORD uid; int on, cnt, kind; DWORD at; } g_al[ALERT_MAX];
static void alert_mark(int x, int y, int kind)
{
    BYTE c = kind == 1 ? C_RED : C_YELLOW;
    if (kind == 1) {                                   /* "!": a 3 px bar and a dot, black outline */
        for (int yy = -1; yy <= 12; yy++) for (int xx = -2; xx <= 2; xx++) if (yy <= 8 || yy >= 10) px(x + xx, y + yy, C_BLACK);
        for (int yy = 0; yy <= 7; yy++) for (int xx = -1; xx <= 1; xx++) px(x + xx, y + yy, c);
        for (int yy = 10; yy <= 11; yy++) for (int xx = -1; xx <= 1; xx++) px(x + xx, y + yy, c);
    } else {                                           /* "?": text glyph, outlined by the text routine's colour */
        text(x - 3, y, "?", c, 1);
    }
}
/* Exposure EYE (2026-11-21, user: the 5-cell meter right of SARGE HEALTH was taken for the sprint stamina bar - "that
 * should be changed to something else"): an eye beside Sarge's head, only while enemies see him - yellow = could see
 * him (not noticed yet), red = noticed and targeting; the number of those enemies next to it. Nothing while unseen. */
#define EYE_DX   14                /* right of Sarge's position, up from his feet (game px) */
#define EYE_DY   (-30)
static void eye_icon(int x, int y, BYTE c)
{
    /* an almond 13 x 7 with a 3 x 3 pupil, black outline */
    static const char *const EYE[9] = {
        "....#####....",
        "..##ooooo##..",
        ".#ooo###ooo#.",
        "#ooo#ppp#ooo#",
        "#ooo#ppp#ooo#",
        "#ooo#ppp#ooo#",
        ".#ooo###ooo#.",
        "..##ooooo##..",
        "....#####....",
    };
    for (int r = 0; r < 9; r++) for (int k = 0; k < 13; k++) {
        char ch = EYE[r][k];
        if (ch == '.') continue;
        px(x - 6 + k, y - 4 + r, ch == '#' ? C_BLACK : ch == 'p' ? C_BLACK : c);
    }
}
/* targ / could: enemies ON SCREEN (2026-11-21, user: "only have the EYE show if there are enemies on screen") */
static void expo_meter(Surf *b, int targ, int could)
{
    (void)b;
    if (!layout_view_is_gameplay()) return;
    BYTE *s = *(BYTE **)0x697520;
    if (!s) return;
    int n = targ + could;
    if (n <= 0) return;
    int sx, sy;
    to_screen(*(short *)(s + 4), *(short *)(s + 6), &sx, &sy);
    int x = sx + EYE_DX, y = sy + EYE_DY;
    if (x < 8 || y < 8 || y >= g_hud_top - 6) return;
    BYTE c = targ > 0 ? C_RED : C_YELLOW;
    eye_icon(x, y, c);
    char t[8]; wsprintfA(t, "%d", n);
    text(x + 8, y - 4, t, c, 1);
}
static void alerts_draw(Surf *b)
{
    BYTE *s = *(BYTE **)0x697520;
    if (!g_cfg.enemy_awareness || !s || !g_loaded) return;
    int on_targ = 0, on_could = 0;                  /* the eye: enemies on screen that see / target Sarge */
    DWORD me = *(DWORD *)s, veh = *(BYTE **)(s + 0x2d0) ? **(DWORD **)(s + 0x2d0) : 0, now = GetTickCount();
    static int seen[ALERT_MAX];
    memset(seen, 0, sizeof seen);
    for (int a = 1; a <= 3; a++) {
        if (!*(int *)(0x4f7820 + a * 0x19e8 + 0x1990) || ((AlertAllyFn)0x41a1e0)(0, a)) continue;
        BYTE *leg = ((AlertArmyFn)0x41a160)(a);
        if (!leg) continue;
        BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
        for (int g = 0; un && un < end && g < 64; un += 0x2c4, g++) {
            int mode = *(int *)(un + 0x70);
            if (mode < 3 || mode > 5) continue;
            int last = *(int *)(un + 0xdc);
            for (int i = 0; i <= last && i < 10; i++) {
                BYTE *t = ((BYTE **)(un + 0xb4))[i];
                if (!t || *(int *)(t + 0x68) != 1 || *(short *)(t + 0x1c) <= 0) continue;
                int k = -1, free_ = -1;
                for (int j = 0; j < ALERT_MAX; j++) { if (g_al[j].t == t && g_al[j].uid == *(DWORD *)t) { k = j; break; } if (!g_al[j].t && free_ < 0) free_ = j; }
                if (k < 0) { if (free_ < 0) continue; k = free_; memset(&g_al[k], 0, sizeof g_al[k]); g_al[k].t = t; g_al[k].uid = *(DWORD *)t; }
                seen[k] = 1;
                DWORD v = T_VICTIM_UID(t);
                float lvl;
                int aw = aware_info(t, &lvl);               /* -1: not tracked -> level 1 (the old behaviour) */
                int on = v && (v == me || (veh && v == veh)) && lvl >= 1.0f;
                if (on != g_al[k].on) {
                    if (++g_al[k].cnt >= ALERT_DEBOUNCE) {
                        g_al[k].on = on; g_al[k].cnt = 0; g_al[k].kind = on ? 1 : 2; g_al[k].at = now;
                        if (g_cfg.log >= 2) mlog("modern: enemy %08lx %s Sarge", *(DWORD *)t, on ? "spotted" : "lost");
                    }
                } else g_al[k].cnt = 0;
                /* above the sprite's top and its health bar (health_bar: top - HEALTH_BAR_ABOVE - 2 .. +5) */
                int cx = *(short *)(t + 4), top = *(short *)(t + 6) - 24;
                BYTE *av = *(BYTE **)(t + 0x26c), *an = av ? *(BYTE **)(av + 0x1c) : NULL;
                if (an) { const int *r = (const int *)(an + 0x1c); cx = (r[0] + r[2]) / 2; top = r[1]; }
                int sx, sy;
                to_screen(cx, top, &sx, &sy);
                if (sx < 6 || sy < 30 || sx >= b->w - 6 || sy >= g_hud_top) continue;
                {   float lv; int fl = aware_info(t, &lv);          /* on screen: counts for the eye */
                    if (fl >= 0 && (fl & AW_TARGETING)) on_targ++; else if (fl >= 0 && (fl & AW_SEES)) on_could++; }
                if (g_al[k].kind && now - g_al[k].at <= ALERT_SHOW_MS) {
                    if ((now - g_al[k].at) <= ALERT_SHOW_MS * 2 / 3 || !((now / 120) & 1)) alert_mark(sx, sy - 24, g_al[k].kind);   /* fading: blink */
                } else if (g_al[k].on) {   /* still targeting Sarge: a steady red wedge pointing down, outlined */
                    int y0 = sy - 15;
                    for (int r = 0; r < 6; r++) for (int xx = -(5 - r); xx <= 5 - r; xx++) px(sx + xx, y0 + r, (r == 0 || xx == -(5 - r) || xx == 5 - r) ? C_BLACK : C_RED);
                } else if (aw >= 0 && lvl > 0.0f && lvl < 1.0f) {   /* noticing: a bar filling with his awareness */
                    int y0 = sy - 14, fill = (int)(lvl * 11 + 0.5f);
                    for (int yy = 0; yy < 5; yy++) for (int xx = -6; xx <= 6; xx++)
                        px(sx + xx, y0 + yy, (yy == 0 || yy == 4 || xx == -6 || xx == 6) ? C_BLACK : xx + 5 < fill ? C_YELLOW : C_DIMYEL_BG);
                } else if (aw > 0 && (aw & AW_SEES)) {      /* could see him, not noticed: a dim yellow eye */
                    int y0 = sy - 12;
                    static const signed char HALF[5] = { 2, 4, 5, 4, 2 };
                    for (int r = 0; r < 5; r++) for (int xx = -HALF[r]; xx <= HALF[r]; xx++)
                        px(sx + xx, y0 + r, (r == 0 || r == 4 || xx == -HALF[r] || xx == HALF[r]) ? C_BLACK : C_DIMYEL);
                    px(sx, y0 + 2, C_BLACK); px(sx - 1, y0 + 2, C_BLACK); px(sx, y0 + 1, C_BLACK); px(sx, y0 + 3, C_BLACK);   /* pupil */
                }
            }
        }
    }
    for (int j = 0; j < ALERT_MAX; j++) if (g_al[j].t && !seen[j]) g_al[j].t = NULL;   /* dead / gone / new mission */
    expo_meter(b, on_targ, on_could);
}

void modern_draw(Surf *b) { PROF_BEGIN(t); modern_draw_body(b); { Surf *keep = g_b; g_b = b; hud_rows_reset(); { Guard gd; if (GUARD_TRY(gd, "enemy alerts (modern.c)")) { alerts_draw(b); guard_leave(&gd); } } g_b = keep; } PROF_END(t, PR_OVERLAY); }   /* timed (trace) */

/* minimap.c tooltip (2026-11-20: the game's cached message font drew nothing there on Windows - empty box) */
int modern_text(Surf *b, int x, int y, const char *s, BYTE c) { Surf *keep = g_b; g_b = b; hud_rows_reset(); int r = text(x, y, s, c, 1); g_b = keep; return r; }
