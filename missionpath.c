/* ============================================================================================
 * missionpath.c - the mission route: from where Sarge starts through the mission's parts in order
 *
 * 2026-11-20, user: "add a toggle to draw a path to the destination. draw it once on mission start and don't
 * dynamically update it. make sure to path find it from where sarge starts to each part of the mission - ie
 * mission 3 is sarge to tank to position".
 *   - WAYPOINTS: per campaign mission id (gpGame +0x74) the places Sarge has to go, in order (stages). Read from
 *     the mission scripts Army Men\Maps\*.txt (tooling/docs/mission-targets.md; lookup 2026-11-20). Kinds: a NAMED
 *     item (its spot at mission start), a PAD (padLocations, the pad's tile corner + 8) or a fixed world POINT
 *     from the script (e.g. des1_3:77 ORDER GreenTank201 GOTO (320,3160) -> the tank waits there). Waypoints of
 *     one stage are visited nearest first (several kills); stages in order.
 *   - built ONCE per mission (new mission / scenario number, or after leaving the play state: Play Again): Sarge's
 *     position on the first play frame is the start; the waypoints are resolved RESOLVE_FRAMES later (the
 *     startup script has placed its items by then). Then one leg piece per frame: path_find (pathfind.c, 8 px
 *     cells, ~40 ms bound) from the leg's current end; a partial answer continues from its last point on the next
 *     frame (long distances), at most LEG_CALLS pieces per leg. A leg "driven" (drive = a vehicle's script name:
 *     mission 3 tank) asks path_find_vehicle first with that vehicle's footprint. A leg not found (no progress,
 *     out of pieces, target closed off) is drawn straight and DASHED from where the search ended.
 *   - never recomputed while things move: only a new mission builds it again.
 * Drawing: modern.c (on the ground, under objects, like the click-to-walk route marker). [hud] mission_path.
 * ============================================================================================ */
#include "common.h"
#include <stdlib.h>
#include <string.h>

#define GAME                 (*(BYTE **)0x4b3f70)
#define GAME_TYPE(g)         (*(int *)((g) + 0x68))          /* 0 = one player */
#define GAME_SCENARIO(g)     (*(int *)((g) + 0x70))
#define GAME_MISSION(g)      (*(int *)((g) + 0x74))
#define SARGE_ITEM           (*(BYTE **)0x697520)
#define PAD_LOC(p)           ((const short *)(0x4e36a0 + 4 * (p)))   /* padLocations[256]: x, y (tile corner) */
#define VEH_TYPE(v)          ((*(int *)(v) >> 16) & 0x3f)    /* as controls.c */

#define RESOLVE_FRAMES 15        /* play frames before the waypoints are read (startup script done) */
#define LEG_CALLS      12        /* path_find pieces per leg (each ~40 ms at most, one per frame) */
#define MIN_PROGRESS   24        /* px a piece must get closer, else the rest of the leg is straight */
#define REACHED        14        /* px: a piece ending this close to the leg's target finishes it */
#define MAX_PTS        600
#define MAX_WP         16

enum { W_ITEM, W_PAD, W_POINT };
typedef struct { int mission, stage, kind; const char *name; int pad, x, y; const char *need; const char *drive; } Way;
/* need: as objectives.c TARGETS ("word" / "!word" in the briefing text; Alpine 1 scripts swapped, open point A).
 * drive: the leg TO this waypoint is driven in that vehicle (path_find_vehicle with its footprint). */
static const Way WAYS[] = {
    { 1, 0, W_ITEM, "TanRifle10" },     { 1, 1, W_PAD, 0, 200 },                         /* des1_1:49-51 */
    { 2, 0, W_PAD, 0, 15 },             { 2, 1, W_PAD, 0, 200 },                         /* des1_2:28 BlueInside = the courier's end point (porch); 93-98 */
    { 3, 0, W_POINT, 0, 0, 320, 3160 }, { 3, 1, W_PAD, 0, 75, 0, 0, 0, "GreenTank201" }, /* des1_3:45, 77, 81-82 */
    { 4, 0, W_POINT, 0, 0, 618, 395 },  { 4, 0, W_POINT, 0, 0, 524, 546 }, { 4, 0, W_POINT, 0, 0, 306, 541 },   /* TanRifle20-22, des2_1:118-120 */
    { 4, 1, W_PAD, 0, 11 },             { 4, 2, W_PAD, 0, 10 },                          /* des2_1:150-160 */
    { 5, 0, W_ITEM, "BigBase" },        { 5, 1, W_PAD, 0, 10 },                          /* des2_2:63-65, 130-133 (Key1 drops at it) */
    { 6, 0, W_PAD, 0, 10 },             { 6, 1, W_POINT, 0, 0, 2390, 2700 }, { 6, 2, W_PAD, 0, 11 },   /* des3_1:11-14, 117-133 */
    { 7, 0, W_ITEM, "Bank" },           { 7, 1, W_PAD, 0, 10 },                          /* des3_2:63-65, 92 */
    { 8, 0, W_PAD, 0, 55 },             { 8, 1, W_PAD, 0, 0 }, { 8, 2, W_POINT, 0, 0, 3970, 2550 },   /* des4_1: escort to Path55, SargeGoal, end (best reading) */
    { 9, 0, W_ITEM, "TanMortar3" }, { 9, 0, W_ITEM, "TanMortar4" }, { 9, 0, W_ITEM, "TanRifle33" }, { 9, 0, W_ITEM, "TanRifle34" },
    { 9, 0, W_ITEM, "TanGrenadier7" }, { 9, 0, W_ITEM, "TanGrenadier9" }, { 9, 0, W_ITEM, "TanRifle35" }, { 9, 1, W_PAD, 0, 99 },   /* des4_2:55-68 */
    { 10, 0, W_ITEM, "truck", 0, 0, 0, "truck" },                                         /* alp1_2:85-88 */
    { 10, 0, W_PAD, 0, 33, 0, 0, "!truck" }, { 10, 1, W_PAD, 0, 22, 0, 0, "!truck" }, { 10, 2, W_PAD, 0, 23, 0, 0, "!truck" },   /* alp1_1:158-182, 101-105 */
    { 11, 0, W_ITEM, "truck", 0, 0, 0, "truck" },
    { 11, 0, W_PAD, 0, 33, 0, 0, "!truck" }, { 11, 1, W_PAD, 0, 22, 0, 0, "!truck" }, { 11, 2, W_PAD, 0, 23, 0, 0, "!truck" },
    { 12, 0, W_ITEM, "Spy1" },                                                           /* alp2_1:53-57, 154-156 */
    { 13, 0, W_ITEM, "TanSpecial101" },                                                  /* alp2_2:44-52 */
    { 14, 0, W_PAD, 0, 32 },                                                             /* alp3_1:226 */
    { 15, 0, W_PAD, 0, 14 },            { 15, 1, W_PAD, 0, 0 },                          /* alp3_2:57-68 */
    { 16, 0, W_ITEM, "radiotower" },                                                     /* alp3_3:41, 87-89 */
    { 17, 0, W_PAD, 0, 20 },            { 17, 0, W_ITEM, "TanBazooka4" }, { 17, 1, W_PAD, 0, 0 },   /* alp4_1:290-291, 395-403, 161-162 */
    { 18, 0, W_PAD, 0, 0 },                                                              /* alp4_2:203-216 */
    { 19, 0, W_PAD, 0, 10 },            { 19, 1, W_PAD, 0, 13 },                         /* bay1_1:137-171 (runner1 to mid1) */
    { 20, 0, W_ITEM, "GrayBazooka103" }, { 20, 0, W_ITEM, "GrayFlamer102" }, { 20, 0, W_ITEM, "colenel" }, { 20, 1, W_PAD, 0, 20 },   /* bay1_2:14-15, 59-71 */
    { 21, 0, W_ITEM, "patrol" },                                                         /* bay2_1:36-37, 92-94 */
    { 22, 0, W_ITEM, "TowerNorth" }, { 22, 0, W_ITEM, "TowerSouth" }, { 22, 0, W_ITEM, "GrayFlamer102" },
    { 22, 0, W_ITEM, "GrayRifle109" }, { 22, 0, W_ITEM, "GrayRifle108" }, { 22, 0, W_ITEM, "GrayBazooka104" }, { 22, 0, W_ITEM, "key" },   /* bay2_2:8-9, 25, 60 */
    { 23, 0, W_ITEM, "TanRifle9" }, { 23, 0, W_ITEM, "TanRifle7" }, { 23, 0, W_ITEM, "TanGrenadier1" },     /* bay3_1:103-111, waves */
    { 23, 1, W_ITEM, "TanGrenadier2" }, { 23, 1, W_ITEM, "TanRifle5" }, { 23, 1, W_ITEM, "TanRifle6" },
    { 23, 2, W_ITEM, "TanRifle1" }, { 23, 2, W_ITEM, "TanRifle3" }, { 23, 2, W_ITEM, "TanBazooka1" },
    { 24, 0, W_ITEM, "TanTank101" }, { 24, 0, W_ITEM, "TanTank102" }, { 24, 0, W_ITEM, "TanTank103" }, { 24, 1, W_PAD, 0, 110 },   /* bay3_2:13, 29-33 */
    { 25, 0, W_PAD, 0, 35, 0, 0, 0, "cargo" },                                           /* bay3_3:34-35, 48 */
    { 26, 0, W_PAD, 0, 10, 0, 0, 0, "cargo" },                                           /* bay4_1:31, 40-41 */
    { 27, 0, W_PAD, 0, 15 },                                                             /* bay4_2:21-22 */
    { 28, 0, W_PAD, 0, 99 },            { 28, 1, W_PAD, 0, 100 },                        /* bay4_3:28, 83-86 */
};
#define NWAYS ((int)(sizeof WAYS / sizeof WAYS[0]))

static MissionPt g_pts[MAX_PTS];
static int g_npts;
static int g_key = -1, g_frames, g_phase;            /* phase 0 wait, 1 legs, 2 done */
static int g_wx[MAX_WP], g_wy[MAX_WP], g_wveh[MAX_WP], g_nwp, g_leg, g_leg_calls, g_cx, g_cy;
static BYTE *g_sarge;

int missionpath_points(const MissionPt **pts)
{
    *pts = g_pts;
    return g_cfg.mission_path && g_phase == 2 ? g_npts : 0;
}

static void add(int x, int y, int dashed, int wp)
{
    if (g_npts >= MAX_PTS) return;
    if (g_npts && g_pts[g_npts - 1].x == x && g_pts[g_npts - 1].y == y) { if (wp) g_pts[g_npts - 1].wp = (BYTE)wp; return; }
    g_pts[g_npts++] = (MissionPt){ (short)x, (short)y, (BYTE)dashed, (BYTE)wp };
}

static int dist2(int ax, int ay, int bx, int by) { int dx = ax - bx, dy = ay - by; return dx * dx + dy * dy; }

/* the waypoints of this mission in visiting order (stages in order, nearest first inside a stage) */
static void resolve(int m, int sx, int sy)
{
    int x[MAX_WP], y[MAX_WP], st[MAX_WP], veh[MAX_WP], n = 0;
    for (int i = 0; i < NWAYS && n < MAX_WP; i++) {
        const Way *w = &WAYS[i];
        if (w->mission != m || (w->need && !objectives_need(w->need))) continue;
        int px = 0, py = 0;
        if (w->kind == W_POINT) { px = w->x; py = w->y; }
        else if (w->kind == W_PAD) {
            if (w->pad < 0 || w->pad > 255) continue;
            const short *p = PAD_LOC(w->pad);
            if (!p[0] && !p[1]) { if (g_cfg.log >= 1) mlog("missionpath: mission %d pad %d has no location", m, w->pad); continue; }
            px = p[0] + 8; py = p[1] + 8;
        } else {
            int raw = 0, s = objectives_item_ground(w->name, &px, &py, &raw, NULL);
            if (s == -2 || s == -1 || (!px && !py)) { if (g_cfg.log >= 1) mlog("missionpath: mission %d item %s %s", m, w->name, s == -2 ? "not found" : s == -1 ? "already done" : "has no spot"); continue; }
            /* 2026-11-20 (user: "moving targets shouldn't get the path"): units, troopers, vehicles move - no waypoint
             * (where a script sends one to a fixed end point, that pad / point is in the table instead) */
            if (raw == 2 || raw == 3 || raw == 5) { if (g_cfg.log >= 1) mlog("missionpath: mission %d item %s moves - no waypoint", m, w->name); continue; }
        }
        if (px <= 0 || py <= 0 || px >= 4096 || py >= 4096) continue;
        veh[n] = -1;
        if (w->drive) {
            BYTE *v = NULL; int vx, vy, raw = 0;
            if (objectives_item_ground(w->drive, &vx, &vy, &raw, &v) != -2 && v && raw == 3) veh[n] = VEH_TYPE(v) - 1;
        }
        x[n] = px; y[n] = py; st[n] = w->stage; n++;
    }
    /* order: stage by stage, each time the nearest one left of the lowest stage */
    BYTE used[MAX_WP] = { 0 };
    int cx = sx, cy = sy;
    g_nwp = 0;
    for (int k = 0; k < n; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (used[i]) continue;
            if (best < 0 || st[i] < st[best] || (st[i] == st[best] && dist2(cx, cy, x[i], y[i]) < dist2(cx, cy, x[best], y[best]))) best = i;
        }
        used[best] = 1;
        g_wx[g_nwp] = x[best]; g_wy[g_nwp] = y[best]; g_wveh[g_nwp] = veh[best]; g_nwp++;
        cx = x[best]; cy = y[best];
        if (g_cfg.log >= 1) mlog("missionpath: waypoint %d at %d,%d (stage %d%s)", g_nwp, x[best], y[best], st[best], veh[best] >= 0 ? ", driven" : "");
    }
}

/* ---------------------------------------------------------------- the walking planner for the mission route
 * 2026-11-20 (user: "the point of the navigation line is to be able to navigate the terrain - so make sure to update
 * it to actually follow a path sarge can follow"): path_find (pathfind.c) is made for one click: ~40 ms and at most
 * 40000 cells, then the NEAREST point so far - chained over long distances it ran into dead ends and the rest of the
 * leg became a straight line through walls. This is its own complete A*: the same walking rule (a cell is free when
 * none of its 4x4 sample points is blocked for Sarge, controls_blocked_at; no corner cutting), the whole map
 * (512 x 512 cells of 8 px), RESUMABLE - a few ms per frame until it is finished - with its own arrays (the click
 * planner and the pointer's reachability test keep theirs) and the cell answers kept for the whole route build. A leg
 * is only "not found" when the target is really closed off; then the found part is kept and the rest dashed.
 * The cell chain is then straightened in sight steps (path_line_free = the click route's line test, Sarge's width),
 * also spread over frames. */
#define WG          512
#define WCELL       8
#define W_BUDGET_MS 6                    /* per frame */
#define W_HEAP      (WG * WG * 8)          /* lazy pushes: a full-map search filled 2 x cells and stopped as "closed off" */
#define WIDE_TYPE   0                    /* VEH_SIZES entry used for the wide route (jeep) */
#define WIDE_CALLS  6                    /* vehicle-planner pieces per walking leg before the fine search */
#define NEAR_COST   6.0f                 /* extra cost of a route cell next to an obstacle */
#define W_LOOKAHEAD 48                   /* cells: the straightening looks at most this far ahead */
static BYTE g_wfree[WG * WG];            /* 0 unknown, 1 free, 2 blocked - kept for the whole route build */
static uint16_t g_wgen[WG * WG], g_wclosed[WG * WG], g_wcurgen;
static float g_wcost[WG * WG];
static int g_wfrom[WG * WG];
static int *g_wheap; static float *g_wheapf; static int g_wnheap;
static int g_wstart, g_wgoal, g_wbest; static float g_wbesth;
static int g_wphase;                     /* 0 idle, 1 searching, 2 straightening, 3 done */
static int g_wchain[WG * WG / 2], g_wnc, g_wi, g_wfound, g_wtx, g_wty, g_wcx, g_wcy;

static int wraw(int c)
{
    if (!g_wfree[c]) {
        int x0 = (c % WG) * WCELL, y0 = (c / WG) * WCELL, blocked = 0;
        int near_start = abs(c % WG - g_wstart % WG) <= 1 && abs(c / WG - g_wstart / WG) <= 1;
        blocked = controls_blocked_at(x0 + WCELL / 2, y0 + WCELL / 2);
        for (int sy = 1; sy < WCELL && !blocked && !near_start; sy += 2)
            for (int sx = 1; sx < WCELL && !blocked; sx += 2) blocked = controls_blocked_at(x0 + sx, y0 + sy);
        g_wfree[c] = blocked ? 2 : 1;
        if (near_start) g_wfree[c] = 0;  /* tested anew for the next leg's start */
        return !blocked;
    }
    return g_wfree[c] == 1;
}
/* 2026-11-20 (user: "the pathing guide needs to actually traverse the area and not cross objects but go around
 * them"): a route cell needs its 8 neighbours free too - the line keeps a full cell (8 px) off every obstacle instead
 * of brushing its edge (it looked like crossing the object's picture). Next to the start / goal only the cell itself. */
static int wfree(int c)
{
    if (!wraw(c)) return 0;
    int x = c % WG, y = c / WG;
    if ((abs(x - g_wstart % WG) <= 2 && abs(y - g_wstart / WG) <= 2) || (abs(x - g_wgoal % WG) <= 2 && abs(y - g_wgoal / WG) <= 2)) return 1;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int nx = x + dx, ny = y + dy;
        if ((dx || dy) && nx >= 0 && ny >= 0 && nx < WG && ny < WG && !wraw(ny * WG + nx)) return 0;
    }
    return 1;
}
/* CENTRED routes (2026-11-20, user: "make sure it is centered between objects and not just right up against
 * things"): a cell's clearance = distance in cells to the nearest cell Sarge cannot stand on (up to CLR_MAX); the
 * route cost grows as the clearance shrinks, so the cheapest route runs down the middle of a passage. */
#define CLR_MAX 5
static BYTE g_wclr[WG * WG];             /* 0 unknown, else clearance + 1 - kept with g_wfree for the route build */
static int wclear(int c)
{
    if (!g_wclr[c]) {
        int x = c % WG, y = c / WG, d = CLR_MAX;
        for (int r = 0; r <= CLR_MAX && d == CLR_MAX; r++)
            for (int dy = -r; dy <= r && d == CLR_MAX; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= WG || ny >= WG || !wraw(ny * WG + nx)) { d = r; break; }
                }
        g_wclr[c] = (BYTE)(d + 1);
    }
    return g_wclr[c] - 1;
}
static float near_cost(int c) { int k = CLR_MAX - wclear(c); return k > 0 ? NEAR_COST * k * k / (float)(CLR_MAX * CLR_MAX) : 0.0f; }
/* straight segment for the straightening: every 4 px walkable and never closer to obstacles than its two ends */
static int wline(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0, n = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 4 + 1;
    int c0 = (y0 / WCELL) * WG + x0 / WCELL, c1 = (y1 / WCELL) * WG + x1 / WCELL;
    int need = wclear(c0) < wclear(c1) ? wclear(c0) : wclear(c1);
    for (int i = 0; i <= n; i++) {
        int x = x0 + dx * i / n, y = y0 + dy * i / n;
        if (x < 0 || y < 0 || x >= WG * WCELL || y >= WG * WCELL) return 0;
        int c = (y / WCELL) * WG + x / WCELL;
        if (!wraw(c) || wclear(c) < need) return 0;
    }
    return 1;
}
static float west(int a, int b)
{
    int dx = abs(a % WG - b % WG), dy = abs(a / WG - b / WG);
    return (float)(dx > dy ? dx + 0.4142 * dy : dy + 0.4142 * dx);
}
static void wpush(int c, float f)
{
    if (g_wnheap >= W_HEAP) return;
    int i = g_wnheap++;
    while (i > 0) { int p = (i - 1) / 2; if (g_wheapf[p] <= f) break; g_wheap[i] = g_wheap[p]; g_wheapf[i] = g_wheapf[p]; i = p; }
    g_wheap[i] = c; g_wheapf[i] = f;
}
static int wpop(void)
{
    int top = g_wheap[0], c = g_wheap[--g_wnheap]; float f = g_wheapf[g_wnheap]; int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i; float mf = f;
        if (l < g_wnheap && g_wheapf[l] < mf) { m = l; mf = g_wheapf[l]; }
        if (r < g_wnheap && g_wheapf[r] < mf) { m = r; mf = g_wheapf[r]; }
        if (m == i) break;
        g_wheap[i] = g_wheap[m]; g_wheapf[i] = g_wheapf[m]; i = m;
    }
    g_wheap[i] = c; g_wheapf[i] = f;
    return top;
}
static int wbegin(int sx, int sy, int tx, int ty)
{
    if (!g_wheap) { g_wheap = malloc(sizeof(int) * W_HEAP); g_wheapf = malloc(sizeof(float) * W_HEAP); }
    if (!g_wheap || !g_wheapf) return 0;
    if (sx < 0 || sy < 0 || tx < 0 || ty < 0 || sx >= WG * WCELL || sy >= WG * WCELL || tx >= WG * WCELL || ty >= WG * WCELL) return 0;
    if (++g_wcurgen == 0) { memset(g_wgen, 0, sizeof g_wgen); memset(g_wclosed, 0, sizeof g_wclosed); g_wcurgen = 1; }
    g_wstart = (sy / WCELL) * WG + sx / WCELL;
    g_wgoal = (ty / WCELL) * WG + tx / WCELL;
    g_wtx = tx; g_wty = ty; g_wcx = sx; g_wcy = sy;
    if (!wraw(g_wgoal)) {                                       /* a pad in a doorway, a soldier's spot: nearest free */
        int best = -1;
        for (int r = 1; r <= 10 && best < 0; r++)
            for (int dy = -r; dy <= r && best < 0; dy++)
                for (int dx = -r; dx <= r && best < 0; dx++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    int x = g_wgoal % WG + dx, y = g_wgoal / WG + dy;
                    if (x >= 0 && y >= 0 && x < WG && y < WG && wraw(y * WG + x)) best = y * WG + x;
                }
        if (best >= 0) { g_wgoal = best; g_wtx = (best % WG) * WCELL + WCELL / 2; g_wty = (best / WG) * WCELL + WCELL / 2; }
    }
    g_wnheap = 0;
    g_wgen[g_wstart] = g_wcurgen; g_wcost[g_wstart] = 0; g_wfrom[g_wstart] = -1;
    g_wbest = g_wstart; g_wbesth = west(g_wstart, g_wgoal);
    wpush(g_wstart, g_wbesth);
    g_wphase = 1; g_wfound = 0;
    return 1;
}
/* one frame's work; 1 = finished (route in out / *n: the straightened points, the last = the leg's end) */
static int wstep(POINT *out, int max, int *n, int *found)
{
    LARGE_INTEGER q0, qf, qn; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf);
    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    int k = 0;
    #define W_TIME_UP() ((++k & 63) == 0 && (QueryPerformanceCounter(&qn), (qn.QuadPart - q0.QuadPart) * 1000 / qf.QuadPart >= W_BUDGET_MS))
    if (g_wphase == 1) {
        while (g_wnheap > 0) {
            if (W_TIME_UP()) return 0;
            int c = wpop();
            if (g_wclosed[c] == g_wcurgen) continue;
            g_wclosed[c] = g_wcurgen;
            if (c == g_wgoal) { g_wfound = 1; break; }
            float h = west(c, g_wgoal);
            if (h < g_wbesth) { g_wbesth = h; g_wbest = c; }
            int cx = c % WG, cy = c / WG;
            for (int d = 0; d < 8; d++) {
                int nx = cx + DX[d], ny = cy + DY[d];
                if (nx < 0 || ny < 0 || nx >= WG || ny >= WG) continue;
                int nb = ny * WG + nx;
                if (g_wclosed[nb] == g_wcurgen || !wraw(nb)) continue;
                if (d >= 4 && (!wraw(cy * WG + nx) || !wraw(ny * WG + cx))) continue;   /* no corner cutting */
                /* passable by Sarge's own rule; a cell next to an obstacle costs extra - the route keeps off objects
                 * where there is room and still fits through narrow doors */
                float g = g_wcost[c] + (d >= 4 ? 1.4142f : 1.0f) + near_cost(nb);
                if (g_wgen[nb] == g_wcurgen && g_wcost[nb] <= g) continue;
                g_wgen[nb] = g_wcurgen; g_wcost[nb] = g; g_wfrom[nb] = c;
                wpush(nb, g + west(nb, g_wgoal));
            }
        }
        int end = g_wfound ? g_wgoal : g_wbest;
        g_wnc = 0;
        for (int c = end; c != -1 && g_wnc < (int)(sizeof g_wchain / sizeof *g_wchain); c = g_wfrom[c]) g_wchain[g_wnc++] = c;
        /* chain[0] = end ... chain[nc-1] = start: walk it from the start */
        g_wi = g_wnc - 1; *n = 0;
        g_wphase = 2;
    }
    if (g_wphase == 2) {
        while (g_wi > 0 && *n < max) {
            if (W_TIME_UP()) return 0;
            /* the farthest chain point ahead (within W_LOOKAHEAD) in Sarge's straight sight */
            int j = g_wi - 1, lim = g_wi - W_LOOKAHEAD < 0 ? 0 : g_wi - W_LOOKAHEAD;
            for (int t = lim; t < g_wi - 1; t++) {
                int px = (g_wchain[t] % WG) * WCELL + WCELL / 2, py = (g_wchain[t] / WG) * WCELL + WCELL / 2;
                if (t == 0 && g_wfound) { px = g_wtx; py = g_wty; }
                if (wline(g_wcx, g_wcy, px, py)) { j = t; break; }
            }
            int px = (g_wchain[j] % WG) * WCELL + WCELL / 2, py = (g_wchain[j] / WG) * WCELL + WCELL / 2;
            if (j == 0 && g_wfound) { px = g_wtx; py = g_wty; }
            out[(*n)++] = (POINT){ px, py };
            g_wcx = px; g_wcy = py; g_wi = j;
        }
        g_wphase = 3;
    }
    #undef W_TIME_UP
    *found = g_wfound;
    return 1;
}

/* one frame's work on the current leg (game thread). Walking legs: the complete resumable planner above. Driven
 * legs (user: "since its expecting to get the tank, make sure the next path is one the tank can take"): the
 * VEHICLE planner only (path_find_vehicle with that vehicle's footprint, 16 px tiles, its own line test), chained
 * from its last point if it stopped short; no walking fallback - where the vehicle cannot go the rest is dashed. */
static void leg_done(int tx, int ty, int reached)
{
    if (!reached && g_cfg.log >= 1) mlog("missionpath: leg %d to %d,%d ends at %d,%d (closed off): the rest dashed", g_leg + 1, tx, ty, g_cx, g_cy);
    add(tx, ty, !reached, g_leg + 1);
    g_cx = tx; g_cy = ty;
    g_leg++; g_leg_calls = 0; g_wphase = 0;
}
static void leg_step(void)
{
    if (g_leg >= g_nwp) { g_phase = 2; if (g_cfg.log >= 1) { mlog("missionpath: done, %d points, %d waypoints", g_npts, g_nwp); char b[600]; int o = 0; for (int i = 0; i < g_npts && o < 560; i++) o += wsprintfA(b + o, " %d,%d%s", g_pts[i].x, g_pts[i].y, g_pts[i].dashed ? "d" : ""); mlog("missionpath: points%s", b); } return; }
    int tx = g_wx[g_leg], ty = g_wy[g_leg];
    static POINT out[MAX_PTS];
    static int wn;
    Guard gd;
    if (g_wveh[g_leg] >= 0) {                                    /* driven */
        volatile int n = 0;
        if (GUARD_TRY(gd, "mission path, vehicle (missionpath.c)")) { n = path_find_vehicle(g_wveh[g_leg], g_cx, g_cy, tx, ty, out, 96); guard_leave(&gd); }
        else n = -1;
        g_leg_calls++;
        int before = dist2(g_cx, g_cy, tx, ty);
        if (n > 0) { for (int i = 0; i < n; i++) add(out[i].x, out[i].y, 0, 0); g_cx = out[n - 1].x; g_cy = out[n - 1].y; }
        int after = dist2(g_cx, g_cy, tx, ty), reached = after <= 32 * 32;
        if (reached || n <= 0 || before - after < MIN_PROGRESS * MIN_PROGRESS || g_leg_calls >= LEG_CALLS) leg_done(tx, ty, reached);
        return;
    }
    if (g_wphase == 0 && g_leg_calls < WIDE_CALLS) {
        /* WIDE ROUTE first (2026-11-20, user: "this should be using same navigation vehicles are using for wider
         * access"): the vehicle planner with the jeep's footprint (16 px tiles, its own clearance) - open lanes and
         * long distances; chained from its end like a driven leg. Only if it cannot get there (a doorway, a gap only
         * Sarge fits through) the fine walking search below takes over from where it stopped. */
        static int leg_npts, leg_cx, leg_cy;
        if (g_leg_calls == 0) { leg_npts = g_npts; leg_cx = g_cx; leg_cy = g_cy; }   /* the leg's start (to drop the wide try) */
        volatile int n = 0;
        if (GUARD_TRY(gd, "mission path, wide (missionpath.c)")) { n = path_find_vehicle(WIDE_TYPE, g_cx, g_cy, tx, ty, out, 96); guard_leave(&gd); }
        else n = -1;
        g_leg_calls++;
        int before = dist2(g_cx, g_cy, tx, ty);
        if (n > 0) { for (int i = 0; i < n; i++) add(out[i].x, out[i].y, 0, 0); g_cx = out[n - 1].x; g_cy = out[n - 1].y; }
        int after = dist2(g_cx, g_cy, tx, ty);
        if (after <= 32 * 32) { leg_done(tx, ty, 1); return; }
        if (n <= 0 || before - after < MIN_PROGRESS * MIN_PROGRESS || g_leg_calls >= WIDE_CALLS) {
            /* the wide route does not get there (e.g. stops outside a yard wall): dropped - the whole leg is planned
             * by the fine walking search from the leg's start (continuing from the wide end made detours) */
            g_npts = leg_npts; g_cx = leg_cx; g_cy = leg_cy;
            g_leg_calls = WIDE_CALLS;
        }
        return;
    }
    if (g_wphase == 0) {                                         /* walking: start the search */
        volatile int ok = 0;
        if (GUARD_TRY(gd, "mission path (missionpath.c)")) { ok = wbegin(g_cx, g_cy, tx, ty); guard_leave(&gd); }
        if (!ok) { leg_done(tx, ty, 0); return; }
        wn = 0;
        return;
    }
    volatile int fin = 0; int found = 0;
    if (GUARD_TRY(gd, "mission path (missionpath.c)")) { fin = wstep(out, MAX_PTS, &wn, &found); guard_leave(&gd); }
    else { leg_done(tx, ty, 0); return; }
    if (!fin) return;                                            /* next frame */
    for (int i = 0; i < wn; i++) add(out[i].x, out[i].y, 0, 0);
    if (wn > 0) { g_cx = out[wn - 1].x; g_cy = out[wn - 1].y; }
    if (g_cfg.log >= 1) mlog("missionpath: leg %d walked: %s, %d points", g_leg + 1, found ? "found" : "closed off", wn);
    leg_done(tx, ty, found || dist2(g_cx, g_cy, tx, ty) <= 32 * 32);
}

void missionpath_frame(void)
{
    BYTE *g = GAME, *s = SARGE_ITEM;
    if (!g || !s || !game_in_play() || GAME_TYPE(g) != 0) {    /* between missions (Play Again too): built anew */
        if (!game_in_play()) { g_key = -1; g_phase = 0; g_npts = 0; }
        return;
    }
    if (!g_cfg.mission_path) return;
    int key = (GAME_MISSION(g) << 16) ^ GAME_SCENARIO(g);
    if (key != g_key || (g_phase == 0 && s != g_sarge)) {       /* first play frame of a mission: Sarge's start */
        g_key = key; g_sarge = s; g_phase = 0; g_frames = 0; g_npts = 0; g_nwp = 0; g_leg = 0; g_leg_calls = 0;
        g_cx = *(short *)(s + 4); g_cy = *(short *)(s + 6);
        memset(g_wfree, 0, sizeof g_wfree); memset(g_wclr, 0, sizeof g_wclr); g_wphase = 0;   /* the cell answers belong to this map */
        add(g_cx, g_cy, 0, 0);
        if (g_cfg.log >= 1) mlog("missionpath: mission %d, Sarge starts at %d,%d", GAME_MISSION(g), g_cx, g_cy);
        return;
    }
    if (g_phase == 0) {
        if (++g_frames < RESOLVE_FRAMES) return;
        resolve(GAME_MISSION(g), g_pts[0].x, g_pts[0].y);
        g_phase = 1;
        return;
    }
    if (g_phase == 1) leg_step();
}
