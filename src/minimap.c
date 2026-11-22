/* ============================================================================================
 * minimap.c - the minimap: easier to read blips, picture contrast, hover details (2026-11-20)
 *
 * User: "any blip visible on the minimap, I want to be able to hover over it and see details" and
 * "the minimap is hard to see. any improvements we can make?".
 *
 * The game's strategic map (binary; curstratmap STRATMAP* at 0x691960):
 *   STRATMAP +0x00 mapstate (2 = READY: pins updated, drawn this frame)   +0x04 SPRITE mapsprite (UiPicture)
 *            +0x4c float xscale, +0x50 float yscale (world px -> minimap px)   +0x54 numpins  +0x58 PIN *pins
 *            +0x5c short onscreenloc.x, +0x5e .y (HUD design position of the map picture's corner)
 *   PIN (0x14 bytes) +0x00 ITEM*  +0x04 pintype (1 Sarge, 2 squad UNIT, 3 vehicle, 4 object; 0 unused)
 *            +0x05 whosepin (army + 1; colour index into pincolors 0x69192c[5])  +0x06 isvisible
 *            +0x08 short screenloc.x, +0x0a .y (HUD design px, = onscreenloc + (world - boundrect) * scale)
 *            +0x0c lastupdate, +0x10 blink state
 *   SM_UpdateStrategicMap 0x439080 decides isvisible (army in play, seen by the player / recon, strategic
 *   items always) and sets screenloc; SM_DrawStrategicMap 0x438b40 draws the picture (FUN_00412f70, hud.c
 *   hud_draw_picture), each visible pin through 0x438d30 -> FUN_004057b0 (one pixel; hud.c
 *   hud_minimap_plot), then the viewport box FUN_00405980 (hud.c hud_minimap_rect) - LAST, so that wrap
 *   is where this file takes its per-frame copy of the pins (minimap_note_frame).
 *   Pins as drawn by the shipped exe: own army 2x1 px, others 2x2, strategic items 3x2 (the per-army shapes in
 *   the exe are dead code: it compares the gpGame POINTER with GAMETYPE_ONEPLAYER 0, always "multiplayer").
 *   Blink: shown ~850 ms, hidden ~70-170 ms (PinBlinkRate 0x4b4900).
 * (FUN_00446ad0, listed with the minimap in ui-inventory.md, is a movement test, not a minimap routine.)
 *
 * What this file adds (all from that pin data - no pixel comparison; [hud] switches, Mod Settings MINIMAP page):
 *   minimap_blips=1     bigger blips with a dark outline in bright army colours, drawn over the ENLARGED minimap
 *                       in the finished frame (after layout_scale_corners), at frame resolution; shape by kind:
 *                       squad = square, vehicle = diamond, object / mission item = bigger square with a yellow
 *                       outline. Steady (not blinking). The viewport box gets a dark edge outside it.
 *   minimap_sarge=1     Sarge's blip bigger with a blinking white / yellow frame.
 *   minimap_contrast=1  the map picture darkened, contrast up, colours muted (a palette lookup applied right
 *                       after the game draws the picture, before the pins - so the blips keep their colours).
 *   minimap_hover=1     pointer over a blip: a tooltip with army, kind (squad type and men left, vehicle type,
 *                       object category, "mission item"), health, distance from Sarge ([modern] distance_unit).
 * Positions: pin design px -> frame px with layout_hud_point (layout.c, the same arithmetic as the enlarged
 * copy); the hover test runs in frame px against those same positions (one mapping for drawing and hovering).
 * [debug] minimap_debug=1 logs the pointer and the blips' frame positions once a second (Wine test aid).
 * Lifetimes: the pins are copied when the game draws them (game thread, this frame); nothing is kept across
 * frames except the copy, which is only used when the game drew the minimap since the last finished frame.
 * ============================================================================================ */
#include "common.h"
#include <math.h>

#define CURSTRATMAP      (*(BYTE **)0x691960)   /* STRATMAP*, NULL without a map */
#define SM_STATE(m)      (*(int *)((m) + 0x00))
#define SM_PICTURE(m)    ((void *)((m) + 0x04))
#define SM_NUMPINS(m)    (*(int *)((m) + 0x54))
#define SM_PINS(m)       (*(BYTE **)((m) + 0x58))
#define MAPSTATE_READY   2
#define PIN_SIZE         0x14
#define PIN_ITEM(p)      (*(BYTE **)((p) + 0x00))
#define PIN_TYPE(p)      (*(BYTE *)((p) + 0x04))
#define PIN_ARMY(p)      (*(signed char *)((p) + 0x05))   /* army + 1, 0 = none */
#define PIN_VISIBLE(p)   (*(BYTE *)((p) + 0x06))
#define PIN_X(p)         (*(short *)((p) + 0x08))
#define PIN_Y(p)         (*(short *)((p) + 0x0a))
enum { PIN_SARGE = 1, PIN_UNIT = 2, PIN_VEHICLE = 3, PIN_OBJECT = 4 };
#define ONSCREENMAPRECT  ((const int *)0x4b48f0)   /* l t r b design px of the map picture: 523,389,609,475 */
#define BOUNDRECT        ((const int *)0x4e2ff4)   /* the_map.boundrect (world px) */
#define SARGE_PTR        (*(BYTE **)0x697520)      /* the player's Sarge (ITEM*) */
/* ITEM (binary): +0 uid (army (uid >> 26 & 7) - 1, type (uid >> 22 & 0xf) - 1, class (uid >> 16) & 0x3f),
 * +4 / +6 short position, +0x1c nStrength, +0x1e nMaxStrength, +0x24 bItemFlags (0x10 = strategic) */
#define IT_UID(it)       (*(DWORD *)(it))
#define IT_X(it)         (*(short *)((it) + 4))
#define IT_Y(it)         (*(short *)((it) + 6))
#define IT_HP(it)        (*(short *)((it) + 0x1c))
#define IT_HP_MAX(it)    (*(short *)((it) + 0x1e))
#define IT_FLAGS(it)     (*(BYTE *)((it) + 0x24))
#define ITEM_STRATEGIC   0x10
#define IT_CLASS(it)     ((int)((IT_UID(it) >> 16) & 0x3f))   /* unit type / vehicle type */
/* UNIT (game table): +0x68 troops (men now), +0xe0 initialTroops. OBJECT (game table): +0x68 short category,
 * +0x70 triggers (8 = strategic), +0x74 char owner army */
#define UNIT_MEN(u)      (*(int *)((u) + 0x68))
#define UNIT_MEN0(u)     (*(int *)((u) + 0xe0))
#define OBJ_CATEGORY(o)  (*(short *)((o) + 0x68))
#define OBJ_TRIGGERS(o)  (*(BYTE *)((o) + 0x70))
#define TRIGGER_STRATEGIC 0x08
#define LEGION_STATUS(a) (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))
typedef int (__fastcall *AllianceFn)(int mine, int other);
#define ALLIED           ((AllianceFn)0x41a1e0)     /* ArmyHaveAllianceWith (ecx mine, edx other) */
#define SHADE_40         (*(BYTE **)0x4e2448)        /* x0.40 shade table (tips.c) */

static const char *const ARMY_NAME[] = { "Green", "Tan", "Blue", "Grey" };
static const char *const UNIT_NAME[] = { "Rifle", "Grenade", "Flame", "Bazooka", "Engineer", "Mortar", "Mine", "Special" };
/* indexed by the UID class field ((uid >> 16) & 0x3f) as the game itself names them (FUN_0044a030 debug name: 2 JEEP,
 * 3 TANK, 4 HALFTRACK, 5 CONVOY; 2026-11-21 fix: the table was one off - a tank showed as "Half-track") */
static const char *const VEHICLE_NAME[] = { "Vehicle", "Vehicle", "Jeep", "Tank", "Half-track", "Convoy truck" };
const char *minimap_vehicle_name(DWORD uid) { int c = (int)((uid >> 16) & 0x3f); return c >= 2 && c <= 5 ? VEHICLE_NAME[c] : VEHICLE_NAME[0]; }
static const char *const CATEGORY[] = {
    "Rock", "Bush", "Tree", "Big tree", "Fallen tree", "Fence", "Wall", "Bridge", "Foot bridge", "Explosives",
    "Gun turret", "AA gun", "Base", "Aux base", "Air strip", "Heli pad", "Prison camp", "Building", "Pill box",
    "Power plant", "Gate", "Wall gate", "Radar", "Object", "Overpass", "Special item" };

/* ---------------------------------------------------------------- the per-frame copy */
#define MAX_BLIPS 512
typedef struct {
    short x, y;                 /* pin screenloc (HUD design px) */
    BYTE kind, army;            /* pintype, whosepin (army + 1) */
    BYTE strategic, sarge;      /* mission item; the player's Sarge */
    int wx, wy;                 /* world position */
    short hp, hp_max;
    int sub;                    /* unit / vehicle type, object category */
    int men, men0;              /* squads */
    int owner;                  /* objects: owner army (-1 none) */
    BYTE look, owner_look;      /* colour scheme shown: 0 none (the game: red), 1..4 green tan blue grey (army_look) */
} Blip;
static Blip g_blip[MAX_BLIPS];
static int g_nblip, g_fresh;    /* g_fresh: the game drew the minimap since the last finished frame */
static int g_box[4], g_has_box; /* viewport box (design px) */
static int g_sarge_wx, g_sarge_wy, g_has_sarge;

/* The colour an army is shown in (a mission can give an army another colour): the same table SM_InitStrategicMap
 * 0x4385d0 uses for pincolors - FUN_00426f90 (ecx *(0x4b4188), army -> +0x218 + army * 4): 0 green, 1 tan, 2 blue, 3 grey */
#define ARMY_COLOURS     (*(BYTE **)0x4b4188)
static int army_look(int army)
{
    if (army < 0 || army > 3) return 0;
    BYTE *g = ARMY_COLOURS;
    int v = g && !IsBadReadPtr(g + 0x218, 16) ? *(int *)(g + 0x218 + army * 4) : army;
    return (v >= 0 && v <= 3 ? v : army) + 1;
}

static int item_ok(const BYTE *it, int size) { return it && !IsBadReadPtr(it, size); }   /* UNIT 0xe4, others 0x78 read */

static void copy_pins(void)
{
    g_nblip = 0;
    BYTE *m = CURSTRATMAP;
    if (!m || IsBadReadPtr(m, 0x60) || SM_STATE(m) != MAPSTATE_READY) return;
    int n = SM_NUMPINS(m);
    BYTE *pins = SM_PINS(m);
    if (n <= 0 || n > 4096 || !pins || IsBadReadPtr(pins, (UINT_PTR)n * PIN_SIZE)) return;
    BYTE *me = SARGE_PTR;
    g_has_sarge = item_ok(me, 0x20);
    if (g_has_sarge) { g_sarge_wx = IT_X(me); g_sarge_wy = IT_Y(me); }
    for (int i = 0; i < n && g_nblip < MAX_BLIPS; i++) {
        BYTE *p = pins + (size_t)i * PIN_SIZE, *it = PIN_ITEM(p);
        int kind = PIN_TYPE(p);
        if (!kind || kind > PIN_OBJECT || !PIN_VISIBLE(p) || !item_ok(it, kind == PIN_UNIT ? 0xe4 : 0x78)) continue;
        Blip *b = &g_blip[g_nblip++];
        b->x = PIN_X(p); b->y = PIN_Y(p);
        b->kind = (BYTE)kind; b->army = (BYTE)(PIN_ARMY(p) >= 0 && PIN_ARMY(p) <= 4 ? PIN_ARMY(p) : 0);
        b->strategic = (IT_FLAGS(it) & ITEM_STRATEGIC) || (kind == PIN_OBJECT && (OBJ_TRIGGERS(it) & TRIGGER_STRATEGIC));
        b->sarge = it == me;
        b->wx = IT_X(it); b->wy = IT_Y(it);
        const int *br = BOUNDRECT;                             /* carried item (outside the map): where its pin is */
        if (b->wx < br[0] || b->wx >= br[2] || b->wy < br[1] || b->wy >= br[3]) {
            float xs = *(float *)(m + 0x4c), ys = *(float *)(m + 0x50);
            if (xs > 0 && ys > 0) { b->wx = br[0] + (int)((b->x - *(short *)(m + 0x5c)) / xs); b->wy = br[1] + (int)((b->y - *(short *)(m + 0x5e)) / ys); }
        }
        b->hp = IT_HP(it); b->hp_max = IT_HP_MAX(it);
        b->sub = kind == PIN_OBJECT ? OBJ_CATEGORY(it) : IT_CLASS(it);
        b->men = kind == PIN_UNIT ? UNIT_MEN(it) : 0;
        b->men0 = kind == PIN_UNIT ? UNIT_MEN0(it) : 0;
        b->owner = kind == PIN_OBJECT ? *(signed char *)(it + 0x74) : -1;
        b->look = (BYTE)(b->army ? army_look(b->army - 1) : 0);
        b->owner_look = (BYTE)(b->owner >= 0 ? army_look(b->owner) : 0);
    }
}

/* hud.c hud_minimap_rect (the viewport box, drawn after the pins): copy the pins the game shows this frame */
void minimap_note_frame(int x1, int y1, int x2, int y2)
{
    g_box[0] = x1; g_box[1] = y1; g_box[2] = x2; g_box[3] = y2; g_has_box = 1;
    static int faults;
    if (faults >= 3) { g_nblip = 0; g_fresh = 1; return; }
    Guard g;
    if (GUARD_TRY(g, "minimap pins (minimap.c)")) { copy_pins(); guard_leave(&g); }
    else { g_nblip = 0; if (++faults == 3) mlog("minimap: pin copy failed 3 times - blips / hover off"); }
    g_fresh = 1;
}

/* ---------------------------------------------------------------- colours (palette indices, per palette) */
static BYTE C_FILL[5], C_DARK, C_WHITE, C_YELLOW, C_TARGET;
static BYTE g_lut[256];                              /* minimap picture contrast */
static PALETTEENTRY g_pal_seen[256];
static int g_pal_ok;
static int nearest(const PALETTEENTRY *e, int r, int g, int b)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = e[i].peRed - r, dg = e[i].peGreen - g, db = e[i].peBlue - b, d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int colours(void)
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    if (!p) return 0;
    if (g_pal_ok && !memcmp(g_pal_seen, p->e, sizeof g_pal_seen)) return 1;   /* palette changes per terrain */
    /* a few entries changed (the game animates some colours): only their lookup entries again - the whole table
     * is 256 x 256 distance tests, ~1 ms per frame while colours cycled (2026-11-20 perf) */
    BYTE changed[256]; int nch = 0;
    for (int i = 0; i < 256; i++) { changed[i] = memcmp(&g_pal_seen[i], &p->e[i], sizeof p->e[i]) != 0; nch += changed[i]; }
    memcpy(g_pal_seen, p->e, sizeof g_pal_seen);
    int full = !g_pal_ok || nch > 32;
    const PALETTEENTRY *e = g_pal_seen;
    /* no army: the game drew these (buildings, bridges, objects) red - read as enemies (2026-11-20, user: "a lot of
     * buildings are counting as enemies"): a neutral stone colour */
    C_FILL[0] = (BYTE)nearest(e, 196, 186, 160);
    C_FILL[1] = (BYTE)nearest(e, 70, 255, 70);      /* green */
    C_FILL[2] = (BYTE)nearest(e, 255, 205, 110);    /* tan */
    C_FILL[3] = (BYTE)nearest(e, 80, 160, 255);     /* blue */
    C_FILL[4] = (BYTE)nearest(e, 225, 225, 235);    /* grey */
    C_DARK = (BYTE)nearest(e, 0, 0, 0);
    C_WHITE = (BYTE)nearest(e, 255, 255, 255);
    C_YELLOW = (BYTE)nearest(e, 255, 230, 40);
    C_TARGET = (BYTE)nearest(e, 255, 220, 0);
    /* contrast: colours muted 35 % toward their grey, contrast x1.35 around mid grey, then darkened to 75 %,
     * so the bright blips stand out on every terrain (the desert map is all beige, the bayou all green) */
    for (int i = 0; i < 256; i++) {
        if (!full && !changed[i]) continue;
        int r = e[i].peRed, g = e[i].peGreen, b = e[i].peBlue, l = (r * 30 + g * 59 + b * 11) / 100;
        int c[3] = { r, g, b };
        for (int k = 0; k < 3; k++) {
            int v = l + (c[k] - l) * 65 / 100;
            v = 128 + (v - 128) * 135 / 100;
            c[k] = clamp255(clamp255(v) * 75 / 100);
        }
        g_lut[i] = (BYTE)nearest(e, c[0], c[1], c[2]);
    }
    g_pal_ok = 1;
    return 1;
}

/* ---------------------------------------------------------------- contrast of the map picture */
/* hud.c hud_draw_picture, after a picture was drawn into the back buffer at px, py (final position, 1x):
 * the minimap's own picture (curstratmap->mapsprite) gets the contrast lookup, before the game draws its pins */
void minimap_picture_drawn(void *back, const void *pic, int px, int py, int w, int h)
{
    if (!g_cfg.mm_contrast || !layout_view_is_gameplay()) return;
    BYTE *m = CURSTRATMAP;
    if (!m || pic != SM_PICTURE(m) || !back) return;
    Surf *s = back;
    if (!s->bits || w <= 0 || h <= 0 || w > 256 || h > 256 || !colours()) return;
    int x0 = px < 0 ? 0 : px, y0 = py < 0 ? 0 : py, x1 = px + w > s->w ? s->w : px + w, y1 = py + h > s->h ? s->h : py + h;
    for (int y = y0; y < y1; y++) {
        BYTE *row = s->bits + (size_t)y * s->pitch;
        for (int x = x0; x < x1; x++) row[x] = g_lut[row[x]];
    }
}

/* ---------------------------------------------------------------- drawing into the finished frame */
static Surf *g_b;
static RECT g_clip;                     /* the minimap picture on the frame */
static RECT g_keep[6]; static int g_nkeep;   /* open panels stay on top (as layout_scale_corners) */
static void put(int x, int y, BYTE c)
{
    if (x < g_clip.left || y < g_clip.top || x >= g_clip.right || y >= g_clip.bottom) return;
    for (int k = 0; k < g_nkeep; k++)
        if (x >= g_keep[k].left && x < g_keep[k].right && y >= g_keep[k].top && y < g_keep[k].bottom) return;
    g_b->bits[(size_t)y * g_b->pitch + x] = c;
    hdworld_clear_run(g_b, y, x, x + 1);           /* interface pixel: the upscaled battlefield stays off it */
}
/* a shape around (cx, cy): r = half size; square or diamond; outline 1 px in `edge` */
static void shape(int cx, int cy, int r, int diamond, BYTE fill, BYTE edge)
{
    for (int dy = -r - 1; dy <= r; dy++)
        for (int dx = -r - 1; dx <= r; dx++) {
            /* square: [-r, r) inside, one ring outside; diamond: |dx + .5| + |dy + .5| */
            int in, ring;
            if (diamond) { int d = abs(2 * dx + 1) + abs(2 * dy + 1); in = d <= 2 * r; ring = !in && d <= 2 * r + 3; }
            else { in = dx >= -r && dx < r && dy >= -r && dy < r; ring = !in; }
            if (in) put(cx + dx, cy + dy, fill);
            else if (ring) put(cx + dx, cy + dy, edge);
        }
}
static void frame_rect(int x0, int y0, int x1, int y1, BYTE c)   /* outline of [x0, x1) x [y0, y1) */
{
    for (int x = x0; x < x1; x++) { put(x, y0, c); put(x, y1 - 1, c); }
    for (int y = y0; y < y1; y++) { put(x0, y, c); put(x1 - 1, y, c); }
}

static int blip_unit(int q) { int u = (q * 3 + 64) / 128; return u < 2 ? 2 : u; }   /* half size: 1.5 design px */
static void blip_centre(const Blip *b, int *cx, int *cy) { layout_hud_point(b->x + 1, b->y + 1, cx, cy); }
static int g_hover = -1;                 /* blip under the pointer this frame */

static void map_rect(RECT *r)
{
    const int *m = ONSCREENMAPRECT;
    int l, t, rr, bb;
    layout_hud_point(m[0], m[1], &l, &t);
    layout_hud_point(m[2], m[3], &rr, &bb);
    *r = (RECT){ l, t, rr, bb };
}

static int hover_find(int sx, int sy, int q, int *out, int max);

/* orders.c (2026-11-21): the world point under game-screen point (x, y) on the minimap picture, 0 when not on it. The
 * inverse of the pins' mapping (pin = onscreenloc + (world - boundrect) * scale, design px), the design px from the
 * enlarged picture's frame rectangle (map_rect: layout_hud_point, linear across the piece - the same mapping the blips
 * and the hover use). Clamped to the boundrect (the part of the map the camera can show). */
int minimap_world_at(int x, int y, int *wx, int *wy)
{
    BYTE *m = CURSTRATMAP;
    if (!m || IsBadReadPtr(m, 0x60) || !layout_view_is_gameplay()) return 0;
    RECT r; map_rect(&r);
    POINT pt = { x, y };
    if (r.right <= r.left || r.bottom <= r.top || !PtInRect(&r, pt)) return 0;
    const int *d = ONSCREENMAPRECT, *br = BOUNDRECT;
    float xs = *(float *)(m + 0x4c), ys = *(float *)(m + 0x50);
    if (!(xs > 0) || !(ys > 0)) return 0;
    double dx = d[0] + (x + 0.5 - r.left) * (d[2] - d[0]) / (double)(r.right - r.left);   /* design px (fractional) */
    double dy = d[1] + (y + 0.5 - r.top) * (d[3] - d[1]) / (double)(r.bottom - r.top);
    int X = br[0] + (int)floor((dx - *(short *)(m + 0x5c)) / xs);
    int Y = br[1] + (int)floor((dy - *(short *)(m + 0x5e)) / ys);
    *wx = X < br[0] ? br[0] : X >= br[2] ? br[2] - 1 : X;
    *wy = Y < br[1] ? br[1] : Y >= br[3] ? br[3] - 1 : Y;
    return 1;
}

/* testing aid ([debug] minimap_debug=1): once a second the pointer, the hovered blip and the blips' frame positions */
static void debug_log(void)
{
    static DWORD t; static int dbg = -1;
    if (dbg < 0) dbg = GetPrivateProfileIntA("debug", "minimap_debug", 0, mod_ini_path());
    if (!dbg || GetTickCount() - t < 1000) return;
    t = GetTickCount();
    int px = -1, py = -1;
    controls_pointer(&px, &py);
    mlog("minimap: pointer %d,%d hover %d blips %d", px, py, g_hover, g_nblip);
    for (int i = 0; i < g_nblip && i < 16; i++) {
        int x, y;
        blip_centre(&g_blip[i], &x, &y);
        mlog("minimap:   %d kind %d look %d at %d,%d (design %d,%d)", i, g_blip[i].kind, g_blip[i].look, x, y, g_blip[i].x, g_blip[i].y);
    }
}

void minimap_draw(Surf *b)
{
    int fresh = g_fresh;
    g_fresh = 0;
    g_hover = -1;
    if (!fresh || !b || !b->bits || !layout_view_is_gameplay() || !colours()) { g_nblip = 0; return; }
    g_b = b;
    int cx, cy, q = layout_hud_point(ONSCREENMAPRECT[0], ONSCREENMAPRECT[1], &cx, &cy), u = blip_unit(q);
    /* the map picture plus a margin: a blip / Sarge's frame at the map's edge stays whole (over the grid border)
     * and covers the game's own dot there */
    map_rect(&g_clip);
    InflateRect(&g_clip, u + 4, u + 4);
    { RECT scr = { 0, 0, b->w < g_cfg.width ? b->w : g_cfg.width, b->h < g_cfg.height ? b->h : g_cfg.height }; IntersectRect(&g_clip, &g_clip, &scr); }
    g_nkeep = modmenu_panel_rect(&g_keep[0]) ? 1 : 0;
    g_nkeep += layout_open_panels(g_keep + g_nkeep, 5);
    int sx, sy, hov[1];
    if (g_cfg.mm_hover && !panel_is_open() && controls_pointer(&sx, &sy) && hover_find(sx, sy, q, hov, 1)) g_hover = hov[0];
    debug_log();
    if (g_cfg.mm_blips) {
        if (g_has_box) {                                           /* dark edge outside the game's viewport box */
            int x0, y0, x1, y1;
            layout_hud_point(g_box[0], g_box[1], &x0, &y0);
            layout_hud_point(g_box[2] + 1, g_box[3] + 1, &x1, &y1);
            frame_rect(x0 - 1, y0 - 1, x1 + 1, y1 + 1, C_DARK);
        }
        /* objects first, then squads / vehicles, Sarge last (on top) */
        for (int pass = 0; pass < 3; pass++)
            for (int i = 0; i < g_nblip; i++) {
                const Blip *bl = &g_blip[i];
                int p = bl->sarge ? 2 : bl->kind == PIN_OBJECT || bl->strategic ? 0 : 1;
                if (p != pass || (bl->sarge && g_cfg.mm_sarge)) continue;
                blip_centre(bl, &cx, &cy);
                BYTE fill = C_FILL[bl->look];
                if (bl->kind == PIN_OBJECT || bl->strategic) shape(cx, cy, u + 1, 0, fill, C_TARGET);
                else if (bl->kind == PIN_VEHICLE) shape(cx, cy, u + 1, 1, fill, C_DARK);
                else shape(cx, cy, u, 0, fill, C_DARK);
            }
    }
    if (g_cfg.mm_sarge)
        for (int i = 0; i < g_nblip; i++) {
            const Blip *bl = &g_blip[i];
            if (!bl->sarge) continue;
            blip_centre(bl, &cx, &cy);
            shape(cx, cy, u + 1, 0, C_FILL[bl->look], C_DARK);
            BYTE ring = (GetTickCount() / 400) & 1 ? C_WHITE : C_YELLOW;   /* blinking frame around him */
            frame_rect(cx - u - 4, cy - u - 4, cx + u + 4, cy + u + 4, C_DARK);
            frame_rect(cx - u - 3, cy - u - 3, cx + u + 3, cy + u + 3, ring);
            frame_rect(cx - u - 2, cy - u - 2, cx + u + 2, cy + u + 2, C_DARK);
        }
    if (g_hover >= 0) {                                            /* the blip the tooltip is about */
        blip_centre(&g_blip[g_hover], &cx, &cy);
        int r = u + 3;
        frame_rect(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, C_DARK);
        frame_rect(cx - r, cy - r, cx + r, cy + r, C_WHITE);
    }
}

/* ---------------------------------------------------------------- hover */
/* blips within reach of the pointer (frame px), nearest first */
static int hover_find(int sx, int sy, int q, int *out, int max)
{
    RECT r; map_rect(&r);
    int u = blip_unit(q), reach = u + 3, n = 0;
    POINT pt = { sx, sy };
    InflateRect(&r, reach, reach);
    if (!PtInRect(&r, pt)) return 0;
    int dist[8];
    if (max > 8) max = 8;
    for (int i = 0; i < g_nblip; i++) {
        int cx, cy;
        blip_centre(&g_blip[i], &cx, &cy);
        int dx = sx - cx, dy = sy - cy, d = dx * dx + dy * dy;
        if (abs(dx) > reach || abs(dy) > reach) continue;
        if (g_blip[i].sarge) d += 4;                               /* a unit next to Sarge wins over him */
        int k = n < max ? n++ : max;
        if (k == max && d >= dist[max - 1]) continue;
        if (k == max) k = max - 1;
        while (k > 0 && dist[k - 1] > d) { dist[k] = dist[k - 1]; out[k] = out[k - 1]; k--; }
        dist[k] = d; out[k] = i;
    }
    return n;
}

/* world px -> text in [modern] distance_unit (modern.c: 4 world px = 1 ft) */
static void distance_text(int px, char *s)
{
    static char unit[4];
    if (!unit[0]) { GetPrivateProfileStringA("modern", "distance_unit", "ft", unit, sizeof unit, mod_ini_path()); CharLowerA(unit); }
    double ft = px / 4.0, v = ft; const char *u = "ft";
    if (unit[0] == 'y') { v = ft / 3; u = "yd"; } else if (unit[0] == 'm') { v = ft * 0.3048; u = "m"; }
    wsprintfA(s, "%d %s", (int)(v + 0.5), u);
}

static int blip_distance(const Blip *b)
{
    if (!g_has_sarge) return -1;
    double dx = b->wx - g_sarge_wx, dy = b->wy - g_sarge_wy;
    return (int)sqrt(dx * dx + dy * dy);
}

/* "Tan Rifle squad" / "Sarge (you)" / "Blue Tank" / "Gun turret (mission item)" */
static void blip_title(const Blip *b, char *s)
{
    const char *an = b->look ? ARMY_NAME[b->look - 1] : "", *sep = b->look ? " " : "";   /* named by the colour shown */
    switch (b->kind) {
    case PIN_SARGE:
        if (b->sarge) lstrcpyA(s, "Sarge (you)");
        else wsprintfA(s, "%s%sSarge", an, sep);
        break;
    case PIN_UNIT:
        wsprintfA(s, "%s%s%s squad", an, sep, b->sub >= 0 && b->sub < 8 ? UNIT_NAME[b->sub] : "");
        break;
    case PIN_VEHICLE:
        wsprintfA(s, "%s%s%s", an, sep, minimap_vehicle_name((DWORD)b->sub << 16));
        break;
    default: {
        int o = b->owner_look;
        wsprintfA(s, "%s%s%s", o ? ARMY_NAME[o - 1] : "", o ? " " : "",
                  b->sub >= 0 && b->sub < 26 ? CATEGORY[b->sub] : "Object");
    }
    }
    if (b->strategic && !b->sarge) lstrcatA(s, " - mission item");
}
/* "4/4 men  hp 60/60  420 ft  enemy" */
static void blip_detail(const Blip *b, char *s)
{
    char t[32];
    s[0] = 0;
    if (b->kind == PIN_UNIT && b->men >= 0 && b->men <= 10 && b->men0 > 0 && b->men0 <= 10) { wsprintfA(t, "%d/%d men  ", b->men, b->men0); lstrcatA(s, t); }
    if (b->hp_max > 0 && b->hp >= 0) { wsprintfA(t, "hp %d/%d  ", b->hp, b->hp_max); lstrcatA(s, t); }
    int d = b->sarge ? -1 : blip_distance(b);
    if (d >= 0) { distance_text(d, t); lstrcatA(s, t); lstrcatA(s, "  "); }
    int a = b->kind == PIN_OBJECT ? b->owner : b->army - 1;
    if (b->kind != PIN_OBJECT && a >= 1 && a <= 3 && !b->sarge) lstrcatA(s, LEGION_STATUS(a) && !ALLIED(0, a) ? "enemy" : "ally");   /* objects: owner in the title only */
    int n = lstrlenA(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
}

/* tips.c: a blip is under the pointer this frame - the minimap's own "Map [M]" tip stays away */
int minimap_hovering(void) { return g_cfg.mm_hover && g_hover >= 0; }

/* the tooltip, drawn last (after tips.c, which leaves the minimap to it while a blip is under the pointer) */
void minimap_draw_tip(Surf *b)
{
    int sx, sy, font, colour, idx[3];
    if (!g_cfg.mm_hover || g_hover < 0 || !b || !b->bits || panel_is_open() || !controls_pointer(&sx, &sy)) return;
    if (!objectives_font(&font, &colour)) colour = C_FILL[0];   /* frame colour: the message box ink if known */
    (void)font;
    int cx, cy, q = layout_hud_point(ONSCREENMAPRECT[0], ONSCREENMAPRECT[1], &cx, &cy);
    int n = hover_find(sx, sy, q, idx, 3);
    if (!n) return;
    char line[5][96]; BYTE ink[5]; int nl = 0;
    const Blip *first = &g_blip[idx[0]];
    blip_title(first, line[nl]); ink[nl++] = C_FILL[first->look];
    blip_detail(first, line[nl]); if (line[nl][0]) ink[nl++] = (BYTE)colour;
    for (int i = 1; i < n; i++) {                                  /* others under the pointer: one line each */
        const Blip *o = &g_blip[idx[i]];
        char d[24]; int dd = o->sarge ? -1 : blip_distance(o);
        blip_title(o, line[nl]);
        if (dd >= 0) { distance_text(dd, d); lstrcatA(line[nl], "  "); lstrcatA(line[nl], d); }
        ink[nl++] = C_FILL[o->look];
    }
    int w = 0, lh = 12, h = nl * lh + 4;
    for (int i = 0; i < nl; i++) { int lw = lstrlenA(line[i]) * 6 + 12; if (lw > w) w = lw; }   /* modern.c 5x7 text: 6 px a character */
    /* above-left of the pointer (the minimap is in the bottom-right corner), kept on screen */
    int x0 = sx - w - 8, y0 = sy - h - 8;
    int W = b->w < g_cfg.width ? b->w : g_cfg.width, H = b->h < g_cfg.height ? b->h : g_cfg.height;
    if (x0 < 0) x0 = sx + 14;
    if (x0 + w > W) x0 = W - w;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = sy + 18;
    if (y0 + h > H) y0 = H - h;
    if (y0 < 0) y0 = 0;
    tips_note_rect((RECT){ x0, y0, x0 + w < W ? x0 + w : W, y0 + h < H ? y0 + h : H });
    BYTE *shade = SHADE_40;
    for (int y = y0; y < y0 + h && y < H; y++) {
        BYTE *row = b->bits + (size_t)y * b->pitch;
        for (int x = x0; x < x0 + w && x < W; x++) {
            int edge = y == y0 || y == y0 + h - 1 || x == x0 || x == x0 + w - 1;
            row[x] = edge ? (BYTE)colour : shade ? shade[shade[row[x]]] : C_DARK;
        }
        hdworld_clear_run(b, y, x0, x0 + w < W ? x0 + w : W);        /* interface: the HD battlefield stays off it */
    }
    for (int i = 0; i < nl; i++) {
        modern_text(b, x0 + 6, y0 + 3 + i * lh, line[i], ink[i]);   /* the mod's own text (the game font drew nothing on Windows) */
    }
}
