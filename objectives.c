/* ============================================================================================
 * objectives.c - the mission objective: pinned on screen, its target sector on the minimap
 *
 * The objective is the mission's briefing text (single player): campaign g_Campaigns[g_CampaignIndex]
 * (0x4fe4c0 / 0x4fe71c), scenario and mission by gpGame (*0x4b3f70) ScenarioID +0x70 / MissionID
 * +0x74 through AiScenarioIndex (FUN_004231d0) / AiMissionIndex (FUN_00423210); the briefing voice
 * id is the int at mission +0xc and voiceFindText (FUN_0046a3f0) gives its text. No data file holds
 * a target location: it is only in the text, as "<A-F>,<1-6>" (0, 1 or 2 per mission).
 *   - target sector: the sector cells blink on the HUD minimap (design 523,389, 86x86; the 6x6
 *     sector grid is baked into the minimap art: column = letter, row = number).
 *   - pinned objective: the text stays in a small box at the top left of the battlefield.
 *   - the message box (ReceiveDashMessage FUN_00441300, wrapped) no longer gets the objective text,
 *     so it only shows game notifications.
 * [hud] pin_objective = 1, target_sector = 1
 * ============================================================================================ */
#include "common.h"
#include <math.h>
#include <string.h>

#define GAME                 (*(BYTE **)0x4b3f70)
#define GAME_TYPE(g)         (*(int *)((g) + 0x68))          /* 0 = one player */
#define GAME_SCENARIO(g)     (*(int *)((g) + 0x70))
#define GAME_MISSION(g)      (*(int *)((g) + 0x74))
#define CAMPAIGNS            ((BYTE **)0x4fe4c0)
#define CAMPAIGN_INDEX       (*(int *)0x4fe71c)
#define BACK_BUFFER          (*(void **)0x4e2e4c)
#define SHADE_40             (*(BYTE **)0x4e2448)             /* x0.40 shade table */

typedef int (__fastcall *IndexFn)(BYTE *owner, int id);
typedef const char *(__fastcall *VoiceTextFn)(int id);
#define AI_SCENARIO_INDEX    ((IndexFn)0x4231d0)
#define AI_MISSION_INDEX     ((IndexFn)0x423210)
#define VOICE_FIND_TEXT      ((VoiceTextFn)0x46a3f0)

#define MAP_X 523
#define MAP_Y 389
#define MAP_S 86

/* ---------------------------------------------------------------- the objective text */

static char g_text[1024];          /* current objective (colour byte stripped), "" = none */
static int g_have_key, g_key_s, g_key_m;
static int g_sectors, g_sec_c[2], g_sec_r[2];

static const char *strip(const char *t) { return (t && (BYTE)*t >= 1 && (BYTE)*t < 32) ? t + 1 : t; }

static void parse_sectors(void)
{
    g_sectors = 0;
    /* [debug] sector_test = "C,1": that sector instead of the text's (test rig: see the highlight at once) */
    char test[8] = "";
    GetPrivateProfileStringA("debug", "sector_test", "", test, sizeof test, mod_ini_path());
    if (test[0] >= 'A' && test[0] <= 'F' && test[1] == ',' && test[2] >= '1' && test[2] <= '6') {
        g_sec_c[0] = test[0] - 'A'; g_sec_r[0] = test[2] - '1'; g_sectors = 1; return;
    }
    for (const char *p = g_text; *p && g_sectors < 2; p++) {
        if (p[0] >= 'A' && p[0] <= 'F' && p[1] == ',' && p[2] >= '1' && p[2] <= '6'
            && (p == g_text || !IsCharAlphaNumericA(p[-1])) && !IsCharAlphaNumericA(p[3])) {
            g_sec_c[g_sectors] = p[0] - 'A';
            g_sec_r[g_sectors] = p[2] - '1';
            g_sectors++;
        }
    }
}

static void refresh(void)
{
    BYTE *g = GAME;
    if (!g || GAME_TYPE(g) != 0) { g_text[0] = 0; g_sectors = 0; g_have_key = 0; return; }
    int s = GAME_SCENARIO(g), m = GAME_MISSION(g);
    if (g_have_key && s == g_key_s && m == g_key_m) return;
    g_have_key = 1; g_key_s = s; g_key_m = m;
    g_text[0] = 0; g_sectors = 0;
    int ci = CAMPAIGN_INDEX;
    if (ci < 0 || ci > 8) return;
    BYTE *camp = CAMPAIGNS[ci];
    if (!camp) return;
    int si = AI_SCENARIO_INDEX(camp, s);
    if (si < 0 || si > 32) return;
    BYTE *scen = camp + 0xd4 + si * 0x3f0;
    int mi = AI_MISSION_INDEX(scen, m);
    if (mi < 0 || mi > 4) return;
    BYTE *mis = scen + 0xd0 + mi * 0xc8;
    const char *t = strip(VOICE_FIND_TEXT(*(int *)(mis + 0xc)));
    if (!t) return;
    lstrcpynA(g_text, t, sizeof g_text);
    parse_sectors();
    if (g_cfg.log >= 1) mlog("objective: scenario %d mission %d: %d sector(s) \"%.60s...\"", s, m, g_sectors, g_text);
}

/* ---------------------------------------------------------------- message box filter */

DashMsgFn objectives_dash_message_original;

void __fastcall objectives_dash_message(const char *msg, void *unit)
{
    if (g_cfg.pin_objective && msg) {
        refresh();
        if (g_text[0] && !lstrcmpA(strip(msg), g_text)) return;     /* pinned instead */
    }
    objectives_dash_message_original(msg, unit);
}

/* The message box's font and colour (hud.c reports text drawn there), reused for the pinned box. */
static int g_font = -1, g_colour, g_any_font = -1, g_any_colour;
void objectives_note_text(int x, int y, int font, int colour)
{
    if (g_any_font < 0) { g_any_font = font; g_any_colour = colour; }   /* any HUD text: a fallback */
    if (x < 0 || x >= 186 || y < LAYOUT_HUD_PANEL_Y) return;
    g_font = font; g_colour = colour;
}

/* The font and colour for the mod's own text (panel.c): the message box's, else any HUD text's. */
int objectives_font(int *font, int *colour)
{
    if (g_font >= 0) { *font = g_font; *colour = g_colour; return 1; }
    if (g_any_font >= 0) { *font = g_any_font; *colour = g_any_colour; return 1; }
    return 0;
}

/* Where the pinned objective box was last drawn (game screen), if it is on screen. */
static RECT g_box;
static DWORD g_box_time;
int objectives_box(RECT *r)
{
    if (!g_box_time || GetTickCount() - g_box_time > 300) return 0;
    *r = g_box;
    return 1;
}

/* ---------------------------------------------------------------- drawing */

static int nearest_colour(int r, int g, int b)
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    if (!p) return 255;
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = p->e[i].peRed - r, dg = p->e[i].peGreen - g, db = p->e[i].peBlue - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void frame(Surf *b, int x0, int y0, int x1, int y1, BYTE c)
{
    if (x0 < 0 || y0 < 0 || x1 > b->w || y1 > b->h || x1 <= x0 || y1 <= y0) return;
    for (int x = x0; x < x1; x++) { b->bits[(size_t)y0 * b->pitch + x] = c; b->bits[(size_t)(y1 - 1) * b->pitch + x] = c; }
    for (int y = y0; y < y1; y++) { b->bits[(size_t)y * b->pitch + x0] = c; b->bits[(size_t)y * b->pitch + x1 - 1] = c; }
}

static void draw_sectors(Surf *b)
{
    if (!g_cfg.target_sector || !g_sectors || (GetTickCount() / 400) % 2) return;   /* blink */
    static int colour = -1;
    if (colour < 0) colour = nearest_colour(255, 230, 40);
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    int ox = layout_hud_dx(MAP_X) + MAP_X, oy = MAP_Y + dy;
    for (int i = 0; i < g_sectors; i++) {
        int x0 = ox + g_sec_c[i] * MAP_S / 6, x1 = ox + (g_sec_c[i] + 1) * MAP_S / 6 + 1;
        int y0 = oy + g_sec_r[i] * MAP_S / 6, y1 = oy + (g_sec_r[i] + 1) * MAP_S / 6 + 1;
        frame(b, x0, y0, x1, y1, (BYTE)colour);
    }
}

#define PIN_X     6
#define PIN_Y     6
#define PIN_W     280
#define PIN_LINE  11
#define PIN_MAX_H 200             /* rows of the box (16 lines at most) */
#define PIN_CHAR  7               /* the message font is about 6.6 px per character (6 clipped the line ends, 2026-11-20) */

static TargetZones g_zones;                           /* target sectors, spots, border objects of the last finished frame (zones_note) */
static void draw_pinned(Surf *b)
{
    g_zones.dim_on = 0;
    if (!g_cfg.pin_objective || !g_text[0] || g_font < 0 || !hud_draw_string_original) return;
    /* word-wrap into lines of at most PIN_W / PIN_CHAR characters */
    static char lines[16][64];
    int n = 0, max = (PIN_W - 8) / PIN_CHAR;
    if (max > 63) max = 63;
    const char *p = g_text;
    while (*p && n < 16) {
        while (*p == ' ') p++;
        int len = lstrlenA(p), cut = len <= max ? len : max;
        if (len > max) { int k = cut; while (k > 0 && p[k] != ' ') k--; if (k > 0) cut = k; }
        lstrcpynA(lines[n++], p, cut + 1);
        p += cut;
    }
    int h = 8 + (n + 1) * PIN_LINE;
    if (PIN_X + PIN_W > b->w || PIN_Y + h > b->h) return;
    /* Upscaled battlefield (2026-11-20, user: "the mission text doesn't look like it was cleanly cut from the
     * background"): darkening the game's own pixels made the box show the LOW-RES battlefield (the GPU keeps the
     * upscaled art only where the byte is unchanged) next to the upscaled one around it. Now the bytes stay, the
     * GPU darkens the upscaled picture under the box (gpu.c td), and only the text's pixels are marked interface.
     * Plain picture: darkened in the bytes as before. */
    BYTE *ov = hdworld_over_mask(b);
    static BYTE before[PIN_MAX_H][PIN_W];
    if (h > PIN_MAX_H) h = PIN_MAX_H;
    if (ov) {
        for (int y = 0; y < h; y++) memcpy(before[y], b->bits + (size_t)(PIN_Y + y) * b->pitch + PIN_X, PIN_W);
        g_zones.dim = (RECT){ PIN_X, PIN_Y, PIN_X + PIN_W, PIN_Y + h };
        g_zones.dim_on = 1;
    } else {
        BYTE *shade = SHADE_40;
        if (shade)
            for (int y = PIN_Y; y < PIN_Y + h; y++) {
                BYTE *row = b->bits + (size_t)y * b->pitch;
                for (int x = PIN_X; x < PIN_X + PIN_W; x++) row[x] = shade[shade[row[x]]];
            }
    }
    g_box = (RECT){ PIN_X, PIN_Y, PIN_X + PIN_W, PIN_Y + h };
    g_box_time = GetTickCount();
    int clip[2] = { PIN_W - 8, PIN_LINE + 2 };
    hud_draw_string_original(b, PIN_X + 4, PIN_Y + 4, "OBJECTIVE", g_font, clip, g_colour);
    for (int i = 0; i < n; i++)
        hud_draw_string_original(b, PIN_X + 4, PIN_Y + 4 + (i + 1) * PIN_LINE, lines[i], g_font, clip, g_colour);
    if (ov)                                           /* the text's pixels: interface (shown as the game drew them) */
        for (int y = 0; y < h; y++) {
            const BYTE *row = b->bits + (size_t)(PIN_Y + y) * b->pitch + PIN_X;
            for (int x = 0; x < PIN_W; x++) {
                if (row[x] == before[y][x]) continue;
                int x0 = x; while (x < PIN_W && row[x] != before[y][x]) x++;
                hdworld_clear_run(b, PIN_Y + y, PIN_X + x0, PIN_X + x);
            }
        }
}

static void zones_note(void);
void objectives_draw(Surf *b)
{
    if (!b || !b->bits || !layout_view_is_gameplay()) { g_zones.n = g_zones.nsec = g_zones.nspot = 0; g_zones.dim_on = 0; return; }
    refresh();
    zones_note();
    draw_sectors(b);
    draw_pinned(b);
}

/* modern.c (off-screen arrow): the middle of the objective's first target sector, in world pixels.
 * The 6x6 sector grid divides the playable area the_map.boundrect (ints at 0x4e2ff4: l t r b). */
int objectives_target(int *wx, int *wy)
{
    if (!g_sectors) return 0;
    const int *b = (const int *)0x4e2ff4;
    if (b[2] <= b[0] || b[3] <= b[1]) return 0;
    *wx = b[0] + (2 * g_sec_c[0] + 1) * (b[2] - b[0]) / 12;
    *wy = b[1] + (2 * g_sec_r[0] + 1) * (b[3] - b[1]) / 12;
    return 1;
}

/* The target sector(s) on the battlefield (2026-11-19, user: "the area is highlighted on the map - can we
 * faintly highlight the area in the game"): their rectangles in game-picture pixels of the current frame
 * (world - camera, rows moved up at the map bottom like everything else). present.c takes them with the
 * frame; gpu.c tints them (ps_world on the upscaled battlefield, ps_scale otherwise). [hud] target_area. */
#define CAMERA_X             (*(short *)0x4e2de8)
#define CAMERA_Y             (*(short *)0x4e2dea)
#define BOUNDRECT            ((const int *)0x4e2ff4)   /* the_map.boundrect l t r b: the 6x6 grid divides it */
#define SPOT_RADIUS 20                                  /* game px around a target pad */
/* Objects that sit ON a sector border, as boxes in game-picture pixels (gpu.c zone_paint: the target line goes
 * around them; user 2026-11-19: "make it draw around the building and objects"). From the drawn-cell registry
 * (roofs_cells_list): world objects (raw type 7..9, not pick-ups) whose picture crosses a border of a target
 * sector, plus - twice over - parts of buildings touching a box already taken (a building is several objects).
 * On screen only, at most what fits in ZONES_MAX. Guarded: it walks cells the mod does not own. */
#define STRADDLE_CELLS 1024
/* 2026-11-20 (user: "the area outline should just go under the fences and rocks, etc. only go around buildings ...
 * the barbwire is some of the line issues"): only buildings get the detour along their inside edge; every other
 * object simply stands on the painted line (ground pixels only). game table categories: BASE 12, AUX_BASE 13,
 * PRISON_CAMP 16, BUILDING 17, PILL_BOX 18, POWER_PLANT 19, RADAR_BUILDING 22 (not turrets, air strips, gates). */
static int is_building_cat(int cat)
{
    return cat == 12 || cat == 13 || (cat >= 16 && cat <= 19) || cat == 22;
}
static int straddlers(RECT *out, int (*key)[3], int n, int nsec, int cx, int cy)
{
    static BYTE *cells[STRADDLE_CELLS];
    static BYTE taken[STRADDLE_CELLS];
    int nc = roofs_cells_list(cells, STRADDLE_CELLS), first = n;
    RECT screen = { -64, -64, g_cfg.width + 64, g_cfg.height + 64 };
    memset(taken, 0, sizeof taken);
    for (int pass = 0; pass < 3 && n < ZONES_MAX; pass++) {
        int upto = n;                                       /* boxes known before this pass */
        for (int i = 0; i < nc && n < ZONES_MAX; i++) {
            if (taken[i]) continue;
            BYTE *ow = *(BYTE **)(cells[i] + 0x0c);
            if (!ow) continue;
            int raw = (int)((*(DWORD *)ow >> 22) & 0xf), cat = *(short *)(ow + 0x68);
            if (raw < 7 || raw > 9 || !is_building_cat(cat)) continue;        /* buildings only (2026-11-20) */
            const int *w = (const int *)(cells[i] + 0x1c);
            RECT r = { w[0] - cx, w[1] - cy, w[2] - cx, w[3] - cy }, t;
            if (r.right - r.left < 10 || r.bottom - r.top < 10 || !IntersectRect(&t, &r, &screen)) continue;
            int take = 0;
            if (pass == 0) {
                for (int k = 0; k < nsec && !take; k++) {
                    RECT big = out[k], small = out[k];
                    InflateRect(&big, 2, 2); InflateRect(&small, -3, -3);
                    int inside = r.left >= small.left && r.top >= small.top && r.right <= small.right && r.bottom <= small.bottom;
                    take = IntersectRect(&t, &big, &r) && !inside;
                }
            } else {                                                          /* a building part beside a taken box */
                RECT g = r; InflateRect(&g, 2, 2);
                for (int k = first; k < upto && !take; k++) take = IntersectRect(&t, &g, &out[k]);
            }
            if (take) {
                /* one box per IMAGE drawn through this cell (a building = several): what tells that image's pixels
                 * on the GPU is (its atlas corner - the screen corner it was drawn at), hdworld_cell_image */
                taken[i] = 1;
                for (int idx = 0; idx < 8 && n < ZONES_MAX; idx++) {
                    int hx, hy, sl, wl, wt;
                    int got = hdworld_cell_image(cells[i], idx, &hx, &hy, &sl, &wl, &wt);
                    if (got < 0) break;
                    if (!got) continue;
                    key[n][0] = (hx - (wl - cx)) & 0x7ff; key[n][1] = (hy - (wt - cy)) & 0x7ff; key[n][2] = sl;
                    out[n++] = r;
                }
            }
        }
    }
    return n;
}

/* The sector line is not shown all the time (2026-11-20, user: "fade in and glow and then fade out"): it fades in
 * (FADE_IN_MS), holds with a pulsing glow (HOLD_MS), fades out (FADE_OUT_MS). Started by a new target sector
 * (mission start, next objective) and by Sarge stepping into a target sector. Game clock: pauses stop it.
 * Target spots (pads) stay visible (at 60 %) and glow along. [hud] target_area_fade=0: always shown, no glow. */
#define FADE_IN_MS   600
#define HOLD_MS      4000                 /* [hud] target_area_hold_ms */
#define FADE_OUT_MS  2000
#define CYCLE_GAP_MS 2500                 /* hidden between two cycles */
#define GLOW_PERIOD_MS 1400
static void zones_fade(TargetZones *z, int sarge_inside, int in_view)
{
    static int cfg = -1, last_sig = -1, was_inside, was_in_view, hold = -1;
    if (hold < 0) hold = GetPrivateProfileIntA("hud", "target_area_hold_ms", HOLD_MS, mod_ini_path());
    static DWORD start; static int started;
    if (cfg < 0) cfg = GetPrivateProfileIntA("hud", "target_area_fade", 1, mod_ini_path());
    if (!cfg) { z->fade = 1; z->glow = 0; return; }
    int sig = g_sectors | g_sec_c[0] << 2 | g_sec_r[0] << 5 | g_sec_c[1] << 8 | g_sec_r[1] << 11 | (GAME ? GAME_MISSION(GAME) : 0) << 14;
    DWORD now = clock_game_ms();
    /* 2026-11-20 (user: "the area highlight isn't displaying anymore"): also each time a target sector comes into view */
    if (sig != last_sig || (sarge_inside && !was_inside) || (in_view && !was_in_view)) { start = now; started = 1; last_sig = sig; }
    was_inside = sarge_inside; was_in_view = in_view;
    /* 2026-11-20 (user: "the area highlight should continue to fade in, glow and fade out. not only once"): it
     * cycles while there is a target - a new target / entering the sector / coming into view restart the cycle */
    int period = FADE_IN_MS + hold + FADE_OUT_MS + CYCLE_GAP_MS;
    int t = started ? (int)((now - start) % (DWORD)period) : 1 << 30;
    float f = t < 0 ? 0 : t < FADE_IN_MS ? (float)t / FADE_IN_MS : t < FADE_IN_MS + hold ? 1.0f
            : t < FADE_IN_MS + hold + FADE_OUT_MS ? 1.0f - (float)(t - FADE_IN_MS - HOLD_MS) / FADE_OUT_MS : 0.0f;
    float g = t >= 0 && t < FADE_IN_MS + hold ? 0.5f - 0.5f * cosf(6.2831853f * (float)t / GLOW_PERIOD_MS) : 0.0f;
    z->fade = f; z->glow = g * f;
}

static void zones_now(TargetZones *z)
{
    z->n = z->nsec = z->nspot = 0;
    if (!g_cfg.target_area || !layout_view_is_gameplay()) return;
    const int *b = BOUNDRECT;
    if (b[2] <= b[0] || b[3] <= b[1]) return;
    int cx = CAMERA_X, cy = CAMERA_Y + layout_field_shift(), n = 0;
    for (int i = 0; i < g_sectors && i < 2; i++) {
        z->r[n].left   = b[0] + g_sec_c[i] * (b[2] - b[0]) / 6 - cx;
        z->r[n].right  = b[0] + (g_sec_c[i] + 1) * (b[2] - b[0]) / 6 - cx;
        z->r[n].top    = b[1] + g_sec_r[i] * (b[3] - b[1]) / 6 - cy;
        z->r[n].bottom = b[1] + (g_sec_r[i] + 1) * (b[3] - b[1]) / 6 - cy;
        n++;
    }
    z->nsec = n;
    {   /* Sarge inside a target sector (picture coordinates, like the boxes) */
        BYTE *s = *(BYTE **)0x697520; int in = 0;
        if (s) { int sx = *(short *)(s + 4) - cx, sy = *(short *)(s + 6) - cy;
                 for (int i = 0; i < n; i++) in |= sx >= z->r[i].left && sx < z->r[i].right && sy >= z->r[i].top && sy < z->r[i].bottom; }
        int view = 0;
        for (int i = 0; i < n; i++) view |= z->r[i].right > 0 && z->r[i].bottom > 0 && z->r[i].left < g_cfg.width && z->r[i].top < g_cfg.height - 60;
        zones_fade(z, in, view);
    }
    /* the pads to reach now (objectives_marks, T_PAD): spots after the sectors */
    ObjMark mk[8]; int nm = objectives_marks(mk, 8);
    for (int i = 0; i < nm && n < z->nsec + 4; i++) {
        if (mk[i].kind != 3) continue;                     /* T_PAD */
        int x = mk[i].x - cx, y = mk[i].y - cy;
        z->r[n].left = x - SPOT_RADIUS; z->r[n].right = x + SPOT_RADIUS;
        z->r[n].top = y - SPOT_RADIUS; z->r[n].bottom = y + SPOT_RADIUS;
        n++;
    }
    z->nspot = n;
    /* the objects on a sector border: the line follows their inside edge */
    if (z->nsec) {
        static int faults;
        volatile int total = n;
        Guard g;
        if (faults < 3) {
            if (GUARD_TRY(g, "target outline: objects on the border (objectives.c)")) { total = straddlers(z->r, z->key, n, z->nsec, cx, cy); guard_leave(&g); }
            else faults++;
        }
        n = total;
    }
    z->n = n;
}

/* taken when the frame is finished (objectives_draw, game thread), read by present.c under the frame lock:
 * the camera then is the one the picture was drawn with */
static void zones_note(void)
{
    zones_now(&g_zones);
    roofs_interior_note(&g_zones, CAMERA_X, CAMERA_Y + layout_field_shift());   /* Sarge inside a building (roofs.c) */
}
void objectives_zones(TargetZones *out) { *out = g_zones; }

/* ---------------------------------------------------------------- mission targets (objective arrow)
 * 2026-11-19, user: "some missions focus on a specific entity - can we add an arrow pointing to where that entity
 * is and how far away it is?". What each campaign mission's win depends on was read from the mission scripts
 * (the shipped Maps\*.txt; table and line references: tooling/docs/mission-targets.md). Per mission id
 * (GAME_MISSION) a list of targets; at run time each is found through the game's own tables:
 *   - a NAMED item (script name, e.g. "TanRifle10", "Key1", "truck"): ItemNames[256][32] at 0x69bbc0, count
 *     0x69dbc4, NamedItemPointers[256] at 0x699bf8 (filled by InitItemNames 0x451ca0 at mission start);
 *   - a PAD (physical number): padLocations[256] at 0x4e36a0 (x, y shorts, tile corner in world pixels; 0 = none;
 *     ProcessPadLayer 0x40db10).
 * A target is DONE (no arrow) when: unit dead (UNIT +0x70 mode 0 / 6), trooper / vehicle strength 0, object
 * destroyed or picked up (OBJECTSTRUCT +0x70 triggers bit 4: SargeDoPickupWeapon 0x449330 sets it); hidden while
 * not on the map (unit in HQ / vehicle, object UNDEPLOYED 0x20). A pad is done once Sarge has been within
 * PAD_REACHED px of it (remembered for the mission). Stages: targets of stage 1 show only when no stage-0
 * target is left (e.g. "pick up the key, THEN go to the pad").
 * need: the entry applies only when the briefing text contains that word ("!word": does not contain it) -
 * the two Alpine 1 scripts (alp1_1 / alp1_2) are swapped between the code and their headers (open point A). */
#define NAMES          ((const char (*)[32])0x69bbc0)
#define NAMES_COUNT    (*(int *)0x69dbc4)
#define NAMED_ITEMS    ((BYTE **)0x699bf8)
#define PAD_LOC(p)     ((const short *)(0x4e36a0 + 4 * (p)))
#define SARGE_ITEM     (*(BYTE **)0x697520)
#define RAW_TYPE(it)   ((int)((*(DWORD *)(it) >> 22) & 0xf))      /* 2 unit, 3 vehicle, 5 trooper, 6 Sarge, 7..9 objects */
#define PAD_REACHED    40

enum { T_KILL, T_GET, T_GOTO, T_PAD };      /* destroy / pick up / a unit to look after / Sarge (or escort) to a pad */
typedef struct { int mission, stage, kind; const char *name; int pad; const char *need; const char *after; } Target;
static const Target TARGETS[] = {
    { 1, 0, T_KILL, "TanRifle10" },        { 1, 1, T_PAD, 0, 200 },
    { 2, 0, T_KILL, "BlueRifle102" }, { 2, 0, T_GET, "BlueNote2", 0, 0, "BlueRifle102" }, { 2, 1, T_PAD, 0, 200 },
    { 3, 0, T_PAD, 0, 75 },
    { 4, 0, T_KILL, "TanRifle20" }, { 4, 0, T_KILL, "TanRifle21" }, { 4, 0, T_KILL, "TanRifle22" }, { 4, 1, T_PAD, 0, 10 },
    { 5, 0, T_GET, "Key1" },               { 5, 1, T_PAD, 0, 10 },
    { 6, 0, T_PAD, 0, 11 },
    { 7, 0, T_PAD, 0, 10 },
    { 8, 0, T_GET, "BlueNote1" },
    { 9, 0, T_PAD, 0, 99 },
    { 10, 0, T_KILL, "truck", 0, "truck" }, { 10, 0, T_GOTO, "GreenRifle201", 0, "!truck" }, { 10, 1, T_PAD, 0, 23, "!truck" },
    { 11, 0, T_KILL, "truck", 0, "truck" }, { 11, 0, T_GOTO, "GreenRifle201", 0, "!truck" }, { 11, 1, T_PAD, 0, 23, "!truck" },
    { 12, 0, T_KILL, "Spy1" },             { 12, 0, T_GET, "spynote", 0, 0, "Spy1" },
    { 13, 0, T_KILL, "TanSpecial101" },    { 13, 0, T_GET, "key", 0, 0, "TanSpecial101" },
    { 14, 0, T_PAD, 0, 32 },
    { 15, 0, T_PAD, 0, 14 },
    { 16, 0, T_KILL, "radiotower" },
    { 17, 0, T_PAD, 0, 30 },
    { 19, 0, T_KILL, "runner1" }, { 19, 0, T_KILL, "runner2" }, { 19, 0, T_GET, "note", 0, 0, "runner1|runner2" },
    { 20, 0, T_KILL, "GrayBazooka103" }, { 20, 0, T_KILL, "GrayFlamer102" }, { 20, 0, T_KILL, "colenel" }, { 20, 0, T_GET, "note2", 0, 0, "colenel" }, { 20, 1, T_PAD, 0, 20 },
    { 21, 0, T_GET, "lure" },
    { 22, 0, T_KILL, "TowerNorth" }, { 22, 0, T_KILL, "TowerSouth" }, { 22, 0, T_KILL, "GrayFlamer102" },
    { 22, 0, T_KILL, "GrayRifle109" }, { 22, 0, T_KILL, "GrayRifle108" }, { 22, 0, T_KILL, "GrayBazooka104" }, { 22, 0, T_GET, "key" },
    { 23, 0, T_KILL, "TanRifle9" }, { 23, 0, T_KILL, "TanRifle7" }, { 23, 0, T_KILL, "TanGrenadier1" },
    { 23, 0, T_KILL, "TanGrenadier2" }, { 23, 0, T_KILL, "TanRifle5" }, { 23, 0, T_KILL, "TanRifle6" },
    { 23, 0, T_KILL, "TanRifle1" }, { 23, 0, T_KILL, "TanRifle3" }, { 23, 0, T_KILL, "TanBazooka1" },
    { 24, 0, T_KILL, "TanTank101" }, { 24, 0, T_KILL, "TanTank102" }, { 24, 0, T_KILL, "TanTank103" }, { 24, 1, T_PAD, 0, 110 },
    { 25, 0, T_PAD, 0, 35 },
    { 26, 0, T_PAD, 0, 10 },
    { 27, 0, T_PAD, 0, 15 },
    { 28, 0, T_PAD, 0, 99 }, { 28, 1, T_PAD, 0, 100 },
};
#define NTARGETS ((int)(sizeof TARGETS / sizeof TARGETS[0]))
static unsigned char g_pad_done[NTARGETS];
static int g_pad_mission = -1;

static BYTE *named_item(const char *name)
{
    int n = NAMES_COUNT;
    if (n <= 0 || n > 256) return NULL;
    for (int k = 0; k < n; k++)
        if (!lstrcmpA(NAMES[k], name)) { BYTE *it = NAMED_ITEMS[k]; return it && !IsBadReadPtr(it, 0xe0) ? it : NULL; }
    return NULL;
}

/* where a named item is now: 1 = shown (world point near the top of its picture), 0 = not on the map now, -1 = done */
static int item_state(BYTE *it, int *x, int *y)
{
    int raw = RAW_TYPE(it);
    *x = *(short *)(it + 4); *y = *(short *)(it + 6);
    if (raw == 2) {                                         /* unit: its first live trooper */
        int mode = *(int *)(it + 0x70);
        if (mode == 0 || mode == 6) return -1;
        if (mode == 2) {                                    /* riding in a vehicle (2026-11-20, user: "the indicator for
                                                             * the spy will sometimes disappear"): point at the vehicle.
                                                             * UNITSTRUCT pVehicle +0xac (game table; pTroops +0xb4) */
            BYTE *v = *(BYTE **)(it + 0xac);
            if (!v || IsBadReadPtr(v, 0x20) || RAW_TYPE(v) != 3) return 0;
            *x = *(short *)(v + 4); *y = *(short *)(v + 6) - 30;
            return 1;
        }
        if (mode < 3) return 0;                             /* in HQ (not deployed) */
        int last = *(int *)(it + 0xdc);
        for (int i = 0; i <= last && i < 10; i++) {
            BYTE *t = ((BYTE **)(it + 0xb4))[i];
            if (!t || IsBadReadPtr(t, 0x70) || *(int *)(t + 0x68) != 1 || *(short *)(t + 0x1c) <= 0) continue;
            *x = *(short *)(t + 4); *y = *(short *)(t + 6) - 24;
            return 1;
        }
        return -1;
    }
    if (raw == 5) { if (*(int *)(it + 0x68) != 1 || *(short *)(it + 0x1c) <= 0) return -1; *y -= 24; return 1; }
    if (raw == 3) { if (*(short *)(it + 0x1c) <= 0) return -1; *y -= 30; return 1; }
    if (raw >= 7 && raw <= 9) {                             /* objects: destroyed / picked up, or not placed yet */
        BYTE trig = it[0x70];
        if (trig & 0x04) return -1;
        if (trig & 0x20) return 0;
        if (*(short *)(it + 0x1e) > 0 && *(short *)(it + 0x1c) <= 0) return -1;
        return 1;
    }
    return (*x || *y) ? 1 : 0;
}

/* "a|b": true once any of the named carriers is done (dead). The scripts drop an objective item where its carrier
 * dies (des1_2 "killed_BlueRifle102 -> PLACEOBJECT BlueNote2 BlueRifle102"); until then the item waits at a stale
 * spot on the map, so it is not pointed at. */
static int after_done(const char *after)
{
    char buf[96], *p = buf;
    lstrcpynA(buf, after, sizeof buf);
    for (;;) {
        char *bar = strchr(p, '|');
        if (bar) *bar = 0;
        int x, y; BYTE *it = named_item(p);
        if (it && item_state(it, &x, &y) < 0) return 1;
        if (!bar) return 0;
        p = bar + 1;
    }
}

static int need_ok(const char *need)
{
    if (!need) return 1;
    int neg = need[0] == '!';
    char low[1024], word[32];
    lstrcpynA(low, g_text, sizeof low); CharLowerA(low);
    lstrcpynA(word, need + neg, sizeof word); CharLowerA(word);
    return (strstr(low, word) != NULL) != neg;
}

/* modern.c: the objective's targets to point at now (world points), up to max; 0 = none (then the sector arrow) */
int objectives_marks(ObjMark *out, int max)
{
    BYTE *g = GAME, *s = SARGE_ITEM;
    if (!g || GAME_TYPE(g) != 0 || !s) return 0;
    refresh();
    int m = GAME_MISSION(g);
    if (m != g_pad_mission) { memset(g_pad_done, 0, sizeof g_pad_done); g_pad_mission = m; }
    int sx = *(short *)(s + 4), sy = *(short *)(s + 6), n = 0;
    static int logged_m = -1; static DWORD logged_at;
    if (g_cfg.log >= 1 && NAMES_COUNT > 0 && (logged_m != m || GetTickCount() - logged_at > 30000)) {   /* what each target resolves to */
        logged_m = m; logged_at = GetTickCount();
        for (int i = 0; i < NTARGETS; i++) {
            const Target *t = &TARGETS[i];
            if (t->mission != m) continue;
            if (t->kind == T_PAD) { const short *p = PAD_LOC(t->pad); mlog("objective target: pad %d at %d,%d%s", t->pad, p[0], p[1], g_pad_done[i] ? " (reached)" : ""); continue; }
            BYTE *it = named_item(t->name); int x = 0, y = 0, st = it ? item_state(it, &x, &y) : -2;
            mlog("objective target: %s %s: %s at %d,%d (raw type %d, need %s)", t->kind == T_GET ? "get" : t->kind == T_GOTO ? "unit" : "kill", t->name,
                 st == -2 ? "NOT FOUND" : st < 0 ? "done" : st ? "shown" : "not on the map", x, y, it ? RAW_TYPE(it) : -1, need_ok(t->need) ? "ok" : "no");
        }
    }
    for (int stage = 0; stage < 2; stage++) {
        int left = 0;
        for (int i = 0; i < NTARGETS; i++) {
            const Target *t = &TARGETS[i];
            if (t->mission != m || t->stage != stage || !need_ok(t->need)) continue;
            int x, y, st, army = -1;
            if (t->kind == T_PAD) {
                if (t->pad < 0 || t->pad > 255 || g_pad_done[i]) continue;
                const short *p = PAD_LOC(t->pad);
                if (!p[0] && !p[1]) continue;
                x = p[0] + 8; y = p[1] + 8;
                int dx = x - sx, dy = y - sy;
                if (dx * dx + dy * dy < PAD_REACHED * PAD_REACHED) { g_pad_done[i] = 1; continue; }
                st = 1;
            } else {
                BYTE *it = named_item(t->name);
                if (!it) continue;
                st = item_state(it, &x, &y);
                int raw = RAW_TYPE(it);
                if (raw == 2 || raw == 3 || raw == 5 || raw == 6) army = (int)((*(DWORD *)it >> 26) & 7) - 1;
                if (st < 0 && t->kind == T_GOTO) st = 0;
                if (st < 0) continue;
                if (t->after && !after_done(t->after)) st = 0;      /* still carried: PLACEOBJECT at the carrier on its death */
            }
            left++;
            if (st > 0 && n < max) { out[n].x = x; out[n].y = y; out[n].kind = t->kind; out[n].army = army; n++; }
        }
        if (left) break;                                    /* this stage is not finished: later stages wait */
    }
    return n;
}
