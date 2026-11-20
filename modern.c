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
    [MOD_SIDE_BUTTONS] = { "side_buttons",  "Mouse side buttons switch weapons",    'S', 1 },
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
    modern_keys_load();
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

/* ---------------------------------------------------------------- the mod's keys (rebindable) */

typedef struct { const char *key, *name; int def; int vk; } ModKey;
static ModKey KEYS[MK_COUNT] = {
    [MK_ROLL]    = { "dodge_roll", "Dodge roll",                'Z' },
    [MK_PEEK]    = { "edge_peek",  "Edge peek (hold)",          'E' },
    [MK_STATS]   = { "stats",      "Mission stats (hold)",      'I' },
    [MK_RADIAL]  = { "radial",     "Radial menu (hold)",        'B' },
    [MK_PING]    = { "ping",       "Ping",                      'T' },
    [MK_RESTART] = { "restart",    "Restart mission (hold 1 s)", VK_F12 },
};
static int g_capture = -1;                    /* waiting for a key for this row (panel) */

void modern_keys_load(void)
{
    for (int i = 0; i < MK_COUNT; i++) KEYS[i].vk = GetPrivateProfileIntA("keys", KEYS[i].key, KEYS[i].def, mod_ini_path());
}
int modern_key(int k) { if (!g_loaded) load(); return k >= 0 && k < MK_COUNT ? KEYS[k].vk : 0; }
int modern_key_count(void) { return MK_COUNT; }
void modern_key_text(int k, char *buf, int n)
{
    char kn[32] = "?";
    UINT sc = MapVirtualKeyA((UINT)KEYS[k].vk, MAPVK_VK_TO_VSC);
    if (!sc || !GetKeyNameTextA((LONG)(sc << 16), kn, sizeof kn)) wsprintfA(kn, "key %d", KEYS[k].vk);
    if (g_capture == k) lstrcpynA(kn, "press a key...", sizeof kn);
    _snprintf(buf, n, "%-26s %s", KEYS[k].name, kn);
    buf[n - 1] = 0;
}
void modern_key_capture(int k) { g_capture = k >= 0 && k < MK_COUNT ? k : -1; }
int modern_key_capturing(void) { return g_capture >= 0; }
/* panel.c: a key was pressed while capturing (Esc cancels). Returns 1 when it was used. */
int modern_key_pressed(int vk)
{
    if (g_capture < 0) return 0;
    if (vk != VK_ESCAPE) {
        KEYS[g_capture].vk = vk;
        char v[16]; wsprintfA(v, "%d", vk);
        WritePrivateProfileStringA("keys", KEYS[g_capture].key, v, mod_ini_path());
        mlog("modern: key %s = %d", KEYS[g_capture].key, vk);
    }
    g_capture = -1;
    return 1;
}

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
    C_SKY = (BYTE)nearest(110, 190, 255);
    C_PATH = (BYTE)nearest(245, 245, 235);     /* mission path: white, 2 px + a dark edge (route marker: green dashed; target outline: yellow) */
    C_PATH_DARK = (BYTE)nearest(40, 40, 40);
    C_ARMY[0] = (BYTE)nearest(90, 210, 70); C_ARMY[1] = (BYTE)nearest(230, 195, 120);
    C_ARMY[2] = (BYTE)nearest(80, 140, 255); C_ARMY[3] = (BYTE)nearest(195, 195, 205);
}
static int on_interface(int x, int y) { return layout_on_hud_panel(x, y) || weapons_ui_at(x, y); }
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
    if (!modern_on(MOD_HIT_MARKERS)) return;
    g_hit_at = GetTickCount();
    g_hit_kill = item && *(short *)(item + 0x1c) <= 0;
}
void modern_note_shot(void) { g_shots++; }
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

static void health_bar(BYTE *it, int cx, int cy, int top, int veh)
{
    (void)cy; (void)veh;
    int hp = *(short *)(it + 0x1c), mx = *(short *)(it + 0x1e);
    if (mx <= 0 || hp >= mx || has_world_bar(it)) return;
    int sx, sy; to_screen(cx, top - HEALTH_BAR_ABOVE, &sx, &sy);
    int w = HEALTH_BAR_W, f = hp * w / mx;
    box(sx - w / 2 - 1, sy - 1, sx + w / 2 + 1, sy + 3, C_BLACK);
    box(sx - w / 2, sy, sx - w / 2 + f, sy + 2, hp * 3 < mx ? C_RED : C_GREEN);
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
    if (sx >= 0 && sy >= 0 && sx < g_cfg.width && sy < g_cfg.height - 60) return;
    edge_arrow(cx, cy, C_RED);
    g_near_count++;
}

/* Radial menu (hold its key): eight commands around the point where it was opened; release over one
 * to give it. Squad orders and air support use the game's own key handlers. */
static const struct { const char *name; uintptr_t handler; } RADIAL[8] = {
    { "FOLLOW", 0x460dc0 }, { "ATTACK", 0x460dd0 }, { "DEFEND", 0x460e00 }, { "ATTACK NOW", 0x460e30 },
    { "AIR STRIKE", 0x440dc0 }, { "PARATROOPS", 0x440d30 }, { "RECON", 0x440e50 }, { "NEXT UNIT", 0x460ee0 },
};
static int g_radial_open, g_radial_x, g_radial_y, g_radial_pick = -1;
void modern_radial(int open, int px, int py)
{
    if (open && !g_radial_open) {                 /* opened: centred on the pointer, kept on the battlefield */
        g_radial_open = 1; g_radial_pick = -1;
        g_radial_x = px < 90 ? 90 : px > g_cfg.width - 90 ? g_cfg.width - 90 : px;
        g_radial_y = py < 60 ? 60 : py > g_cfg.height - 170 ? g_cfg.height - 170 : py;
        return;
    }
    if (open) {
        int dx = px - g_radial_x, dy = py - g_radial_y;
        g_radial_pick = dx * dx + dy * dy < 14 * 14 ? -1 : ((int)lround(atan2((double)dy, (double)dx) / (6.2831853 / 8)) + 8) % 8;
        return;
    }
    if (g_radial_open) {
        g_radial_open = 0;
        if (g_radial_pick >= 0) {
            mlog("modern: radial menu -> %s", RADIAL[g_radial_pick].name);
            ((VoidFnPtr)RADIAL[g_radial_pick].handler)();
        }
    }
}
int modern_radial_open(void) { return g_radial_open; }

static void modern_draw_body(Surf *b)
{
    BYTE *u = SARGE;
    if (!b || !b->bits || !u || !layout_view_is_gameplay()) return;
    if (!g_loaded) load();
    g_b = b;
    colours();
    DWORD now = GetTickCount();
    int sgx, sgy; to_screen(*(short *)(u + 4), *(short *)(u + 6), &sgx, &sgy);

    g_nbarred = 0;
    if (modern_on(MOD_HEALTH_BARS)) { each_enemy(health_bar); target_health_bars(); }
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
    missionpath_frame();                                /* built once per mission (missionpath.c), never per frame */
    {   /* The mission path (2026-11-20, [hud] mission_path): Sarge's start -> each waypoint, painted on the ground
         * like the route marker (g_route: under objects), white, 2 px with a dark edge, solid; a leg the planner did not find is
         * dashed. Each waypoint: a small ring with its number. Only segments touching the screen are drawn. */
        const MissionPt *mp; int n = missionpath_points(&mp);
        if (n >= 2) {
            g_route = 1;
            int W = g_cfg.width, H = g_cfg.height;
            if (!g_cfg.hd_world) under_begin(CAMERA_X - 8, CAMERA_Y + layout_field_shift() - 8, CAMERA_X + W + 8, CAMERA_Y + layout_field_shift() + H + 8);
            for (int pass = 0; pass < 2; pass++)            /* the dark edges first: a joint never cuts the white line */
            for (int i = 0; i + 1 < n; i++) {
                int x0, y0, x1, y1; to_screen(mp[i].x, mp[i].y, &x0, &y0); to_screen(mp[i + 1].x, mp[i + 1].y, &x1, &y1);
                if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= W && x1 >= W) || (y0 >= H && y1 >= H)) continue;
                int tx = abs(x1 - x0) < abs(y1 - y0), ty = !tx, d = mp[i + 1].dashed ? 4 : 0;   /* 2 px wide + a dark edge */
                if (pass == 0) { line(x0 + 2 * tx, y0 + 2 * ty, x1 + 2 * tx, y1 + 2 * ty, C_PATH_DARK, d); continue; }
                line(x0, y0, x1, y1, C_PATH, d);
                line(x0 + tx, y0 + ty, x1 + tx, y1 + ty, C_PATH, d);
            }
            for (int i = 1; i < n; i++) {
                if (!mp[i].wp) continue;
                int wx, wy; to_screen(mp[i].x, mp[i].y, &wx, &wy);
                if (wx < -20 || wy < -20 || wx > W + 20 || wy > H + 20) continue;
                circle(wx, wy, 6, C_PATH, 0);
                circle(wx, wy, 5, C_PATH, 0);
            }
            g_nunder = 0;
            g_route = 0;
            for (int i = 1; i < n; i++) {               /* the numbers: on top (a pad inside a building hid them) */
                if (!mp[i].wp) continue;
                int wx, wy; to_screen(mp[i].x, mp[i].y, &wx, &wy);
                if (wx < -20 || wy < -20 || wx > W + 20 || wy > H + 20) continue;
                char s[4]; wsprintfA(s, "%d", mp[i].wp);
                text(wx + 8, wy - 12, s, C_PATH, 1);
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
            circle(cx, cy, ir, c, 1);
            circle(cx, cy, 3, c, 0);
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
                    BYTE c = (tr >= 0 ? tr : d <= hi) ? C_RED : C_GREY;
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
                int sy0 = ry - layout_field_shift(), w = 16, f = hp > mx ? w : (hp * w + mx - 1) / mx;
                for (int x = -1; x <= w; x++) for (int y = 11; y <= 14; y++) px(rx - w / 2 + x, sy0 + y, C_BLACK);
                for (int x = 0; x < f; x++) for (int y = 12; y <= 13; y++) px(rx - w / 2 + x, sy0 + y, hp * 3 < mx ? C_RED : C_GREEN);
            }
        }
        ry -= layout_field_shift();
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
    if (g_radial_open) {
        int cx = g_radial_x, cy = g_radial_y;
        circle(cx, cy, 14, C_GREY, 1);
        for (int i = 0; i < 8; i++) {
            double a = i * 6.2831853 / 8;
            int tx = cx + (int)(cos(a) * 52), ty = cy + (int)(sin(a) * 40);
            int w = (int)strlen(RADIAL[i].name) * 6;
            box(tx - w / 2 - 3, ty - 3, tx + w / 2 + 2, ty + 10, i == g_radial_pick ? C_GREEN : C_BLACK);
            text(tx - w / 2, ty, RADIAL[i].name, i == g_radial_pick ? C_BLACK : C_WHITE, 0);
        }
    }
    g_b = NULL;
}

void modern_draw(Surf *b) { PROF_BEGIN(t); modern_draw_body(b); PROF_END(t, PR_OVERLAY); }   /* timed (trace) */

/* minimap.c tooltip (2026-11-20: the game's cached message font drew nothing there on Windows - empty box) */
int modern_text(Surf *b, int x, int y, const char *s, BYTE c) { Surf *keep = g_b; g_b = b; int r = text(x, y, s, c, 1); g_b = keep; return r; }
