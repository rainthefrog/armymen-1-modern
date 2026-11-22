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
 *     mission 3 tank) is planned by the vehicle hybrid planner (drive_step). A leg not found (no progress,
 *     out of pieces, target closed off) is drawn straight and DASHED from where the search ended.
 *   - never recomputed while things move: only a new mission builds it again.
 *   - 2026-11-21: targets close together = one waypoint at their centre; only the CURRENT leg is drawn, the next one
 *     once that part of the mission is done; walking legs vary a little (seeded); Play Again keeps the route, an entry
 *     from the menus makes a new one. See "waypoints = target groups", "variation" and "restart or fresh entry".
 * Drawing: modern.c (on the ground, under objects, like the click-to-walk route marker). [hud] mission_path.
 * ============================================================================================ */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
#define MAX_PTS        1600      /* all legs, plus a variant being tried (2026-11-21) */
#define SNAP_CELL      8         /* = WCELL, WG below (the walking planner grid) */
#define SNAP_GRID      512
#define MAX_WP         16

enum { W_ITEM, W_PAD, W_POINT };
/* join (2026-11-21, user: "some of the missions (like cross the bridge) should be combined with the next one (go to the weapon
 * depot) and some should be split like get the tank and the cross the bridge"): this waypoint's leg is SHOWN together with
 * the next leg (one continuous path); each still advances on its own. Splits = extra W_POINT stops in the table. */
typedef struct { int mission, stage, kind; const char *name; int pad, x, y; const char *need; const char *drive; int post; const char *until; int join; const char *escort; } Way;
/* escort (2026-11-21): while this waypoint is the current one, the lane is that VEHICLE's own route (ESCORTS below). */
/* need: as objectives.c TARGETS ("word" / "!word" in the briefing text; Alpine 1 scripts swapped, open point A).
 * drive: the leg TO this waypoint is driven in that vehicle (path_find_vehicle with its footprint).
 * post (2026-11-21): a unit that HOLDS ITS POST - the script orders it "GOTO <its own name>" (des4_2:25-31,
 *   bay2_2:36-39, bay1_2:45-46, alp2_2:33, alp4_1:53): taken at its spot at mission start like an object. Other
 *   units, troopers and vehicles move: no waypoint (2026-11-20, user: "moving targets shouldn't get the path").
 * until: a W_POINT is done when all these named items are done ("a|b|c"), else when Sarge reaches it. */
#define POST(m, s, n) { m, s, W_ITEM, n, 0, 0, 0, 0, 0, 1, 0 }
static const Way WAYS[] = {
    /* TanRifle10 patrols the bridge into town (patrol.c, 2026-11-21 user): the waypoint is the bridge, done when he is dead */
    { 1, 0, W_POINT, 0, 0, 2520, 350, 0, 0, 0, "TanRifle10", 1 }, { 1, 1, W_PAD, 0, 200 },   /* join: bridge + depot shown as one path */   /* des1_1:49-51 */
    { 2, 0, W_PAD, 0, 15 },             { 2, 1, W_PAD, 0, 200 },                         /* des1_2:28 BlueInside = the courier's end point (porch); 93-98 */
    { 3, 0, W_POINT, 0, 0, 320, 3160 },
    { 3, 1, W_POINT, 0, 0, 2470, 2600, 0, "GreenTank201" },   /* split: drive to the Burgess Bridge (west end; objects 2406..3601 x 2188..2671), then across */ { 3, 1, W_PAD, 0, 75, 0, 0, 0, "GreenTank201" }, /* des1_3:45, 77, 81-82 */
    /* the squad TanRifle20-22 (des2_1:118-120: 618,395 / 524,546 / 306,541) - one rendezvous at their middle
     * (2026-11-20, user: "if [multiple targets], get the middle of these and use that as the target position") */
    { 4, 0, W_POINT, 0, 0, 483, 494, 0, 0, 0, "TanRifle20|TanRifle21|TanRifle22" },   /* done: the three killed */
    { 4, 1, W_PAD, 0, 11 },             { 4, 2, W_PAD, 0, 10 },                          /* des2_1:150-160 */
    { 5, 0, W_ITEM, "BigBase" },        { 5, 1, W_PAD, 0, 10 },                          /* des2_2:63-65, 130-133 (Key1 drops at it) */
    { 6, 0, W_PAD, 0, 10 },             { 6, 1, W_POINT, 0, 0, 2390, 2700, 0, 0, 0, 0, 0, "GreenHalftrack1" }, { 6, 2, W_PAD, 0, 11 },   /* des3_1:11-14, 117-133 */
    { 7, 0, W_ITEM, "Bank" },           { 7, 1, W_PAD, 0, 10, 0, 0, 0, 0, 0, 0, 0, "GreenHalftrack1" },   /* des3_2:63-65, 92, 168-170 */
    { 8, 0, W_PAD, 0, 55, 0, 0, 0, 0, 0, 0, 0, "GreenHalftrack3" }, { 8, 1, W_PAD, 0, 0 }, { 8, 2, W_POINT, 0, 0, 3970, 2550 },   /* des4_1: escort to Path55, SargeGoal, end (best reading) */
    POST(9, 0, "TanMortar3"), POST(9, 0, "TanMortar4"), POST(9, 0, "TanRifle33"), POST(9, 0, "TanRifle34"),
    POST(9, 0, "TanGrenadier7"), POST(9, 0, "TanGrenadier9"), POST(9, 0, "TanRifle35"), { 9, 1, W_PAD, 0, 99 },   /* des4_2:55-68 */
    { 10, 0, W_ITEM, "truck", 0, 0, 0, "truck" },                                         /* alp1_2:85-88 */
    { 10, 0, W_PAD, 0, 33, 0, 0, "!truck" }, { 10, 1, W_PAD, 0, 22, 0, 0, "!truck" }, { 10, 2, W_PAD, 0, 23, 0, 0, "!truck" },   /* alp1_1:158-182, 101-105 */
    { 11, 0, W_ITEM, "truck", 0, 0, 0, "truck" },
    { 11, 0, W_PAD, 0, 33, 0, 0, "!truck" }, { 11, 1, W_PAD, 0, 22, 0, 0, "!truck" }, { 11, 2, W_PAD, 0, 23, 0, 0, "!truck" },
    { 12, 0, W_ITEM, "Spy1" },                                                           /* alp2_1:53-57, 154-156 */
    POST(13, 0, "TanSpecial101"),                                                /* alp2_2:44-52 */
    { 14, 0, W_PAD, 0, 32 },                                                             /* alp3_1:226 */
    { 15, 0, W_PAD, 0, 14 },            { 15, 1, W_PAD, 0, 0 },                          /* alp3_2:57-68 */
    { 16, 0, W_ITEM, "radiotower" },                                                     /* alp3_3:41, 87-89 */
    { 17, 0, W_PAD, 0, 20 },            POST(17, 0, "TanBazooka4"), { 17, 1, W_PAD, 0, 0 },   /* alp4_1:290-291, 395-403, 161-162 */
    { 18, 0, W_PAD, 0, 0 },                                                              /* alp4_2:203-216 */
    { 19, 0, W_PAD, 0, 10 },            { 19, 1, W_PAD, 0, 13 },                         /* bay1_1:137-171 (runner1 to mid1) */
    POST(20, 0, "GrayBazooka103"), POST(20, 0, "GrayFlamer102"), { 20, 0, W_ITEM, "colenel" }, { 20, 1, W_PAD, 0, 20 },   /* bay1_2:14-15, 59-71 */
    { 21, 0, W_ITEM, "patrol" },                                                         /* bay2_1:36-37, 92-94 */
    { 22, 0, W_ITEM, "TowerNorth" }, { 22, 0, W_ITEM, "TowerSouth" }, POST(22, 0, "GrayFlamer102"),
    POST(22, 0, "GrayRifle109"), POST(22, 0, "GrayRifle108"), POST(22, 0, "GrayBazooka104"), { 22, 0, W_ITEM, "key" },   /* bay2_2:8-9, 25, 60 */
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
static int g_dphase, g_dpieces, g_dh, g_dfacing0[MAX_WP];   /* driven legs (drive_step): phase 0 begin a plan, 1 planning, 2 tile planner; pieces; heading; vehicle hull at resolve */
static BYTE *g_sarge;

/* ---------------------------------------------------------------- waypoints = target groups, one leg shown
 * 2026-11-21, user: "if there are multiple target points at a location, calculate the center between them and use
 * that. I want only one path displayed at a time, so from A to B. once that part of the mission is complete, display
 * the next one."
 *   - GROUPS: the resolved targets of one stage are grouped by distance (repeatedly the two closest groups of the
 *     same stage and the same drive merge while their centres are <= CLUSTER_PX apart and every member stays within
 *     CLUSTER_R of the merged centre). Each group is ONE waypoint at the mean of its members' spots; a group of
 *     several is moved to the nearest cell with an open 3x3 (<= SNAP_CELLS) when its centre is shut in (a building,
 *     rocks). Order: stages in order, inside a stage the nearest group next (as before). Mission 4 keeps its hand-set
 *     centre of TanRifle20-22 (one W_POINT, done when the three are dead).
 *   - LEGS: leg k runs from waypoint k-1 (leg 0: Sarge's start) to waypoint k = g_pts[g_leg_first[k] ..
 *     g_leg_first[k + 1]]. Only the CURRENT leg is drawn (to the first waypoint not done); [hud] mission_path_all=1
 *     draws all legs as before.
 *   - DONE (checked per frame for the current waypoint only; a member once done stays done):
 *       item member: objectives_item_ground says done (dead / destroyed / picked up: objectives.c item_state);
 *       pad member: Sarge (his vehicle while he drives) within REACH_PX (REACH_VEH) of the pad or of the waypoint;
 *       point member: all its "until" items done, or (no until) Sarge within REACH_PX;
 *       and: the NEXT leg is driven and Sarge is in a vehicle (mission 3: the tank is taken).
 *     A waypoint is done when all its members are; then the next one's leg shows (log=1: "waypoint N done"). */
#define CLUSTER_PX  400          /* about half a screen: "one location" */
#define CLUSTER_R   320
#define SNAP_CELLS  24
#define REACH_PX    48
#define REACH_VEH   64
#define MAX_MEM     16
static int g_leg_first[MAX_WP + 1];      /* g_pts index of each leg's first point (the previous waypoint) */
static const Way *g_mem[MAX_MEM]; static BYTE g_mem_wp[MAX_MEM], g_mem_done[MAX_MEM]; static int g_nmem;
static int g_cur;                        /* the waypoint the shown leg leads to */
static int wcell_open(int c);
static void route_done(void);

static int wp_join(int k)                /* a member of waypoint k has join: its leg is shown with the next one */
{
    for (int i = 0; i < g_nmem; i++) if (g_mem_wp[i] == k && g_mem[i]->join) return 1;
    return 0;
}
static int escort_lane(const MissionPt **pts);          /* escort section below: the vehicle's lane (0 = not active) */
static int escort_wp_done(int k, const char **why);     /* -1: k is no escort waypoint now (normal rules), else 0 / 1 */
static int g_build_serial, g_esc_shown;                  /* for missionpath_points_key */
int missionpath_points(const MissionPt **pts)
{
    *pts = g_pts;
    g_esc_shown = 0;
    if (!g_cfg.mission_path || g_phase != 2) return 0;
    if (g_cfg.mission_path_all) return g_npts;
    if (g_cur >= g_nwp) return 0;
    { int n = escort_lane(pts); if (n >= 2) { g_esc_shown = 1; return n; } *pts = g_pts; }
    int end = g_cur;
    while (end + 1 < g_nwp && wp_join(end)) end++;          /* joined legs: one continuous path */
    int a = g_leg_first[g_cur], b = g_leg_first[end + 1];
    if (a < 0 || b <= a || b >= g_npts) return 0;
    *pts = g_pts + a;
    return b - a + 1;
}
/* which leg is shown (modern.c restarts the lane draw-in when it changes): the escort lane's first point moves with the
 * vehicle every frame, so the old "other first point" test would restart the draw-in all the time */
int missionpath_points_key(void)
{
    return (g_build_serial << 12) ^ (g_cur << 2) ^ (g_esc_shown ? 1 : 0) ^ (g_cfg.mission_path_all ? 2 : 0);
}

static void add(int x, int y, int dashed, int wp)
{
    if (g_npts >= MAX_PTS) return;
    if (g_npts && g_pts[g_npts - 1].x == x && g_pts[g_npts - 1].y == y) { if (wp) g_pts[g_npts - 1].wp = (BYTE)wp; return; }
    g_pts[g_npts++] = (MissionPt){ (short)x, (short)y, (BYTE)dashed, (BYTE)wp };
}

/* A pad is an AREA of map tiles (2026-11-21, user, mission 4: "this mission endpoint should be center. i believe there
 * are 4 target points here"): padLocations (0x4e36a0) keeps only the LAST walkable tile ProcessPadLayer 0x40db10 found.
 * The pad layers: pads 0..7 = bits of the byte layer at *0x4e354c, pads 8..255 = the byte value of the layer at
 * *0x4e3550; tile count *0x6083c8, map width in tiles 0x4e368c. The waypoint is the centre of all the pad's tiles. */
#define PAD_BITS_LAYER  (*(BYTE **)0x4e354c)
#define PAD_BYTE_LAYER  (*(BYTE **)0x4e3550)
#define MAP_TILE_COUNT  (*(unsigned *)0x6083c8)
#define MAP_TILES_W     (*(unsigned short *)0x4e368c)
static void pad_centre(int pad, int *x, int *y)
{
    BYTE *layer = pad < 8 ? PAD_BITS_LAYER : PAD_BYTE_LAYER;
    unsigned n = MAP_TILE_COUNT, w = MAP_TILES_W;
    if (!layer || !w || !n || n > 1024 * 1024 || IsBadReadPtr(layer, n)) return;
    long long sx = 0, sy = 0; int k = 0;
    for (unsigned t = 0; t < n; t++) {
        BYTE v = layer[t];
        if (pad < 8 ? !(v & (1 << pad)) : v != pad) continue;
        sx += (t % w) * 16 + 8; sy += (t / w) * 16 + 8; k++;
    }
    if (k <= 1) return;
    if (g_cfg.log >= 1) mlog("missionpath: pad %d = %d tiles, centre %d,%d (padLocations %d,%d)", pad, k, (int)(sx / k), (int)(sy / k), *x, *y);
    *x = (int)(sx / k); *y = (int)(sy / k);
}

/* Sarge (world x, y) stands on one of the pad's tiles - the game's own "on_<pad>" test (2026-11-21: "pad reached" was
 * 48 px of the pad's centre; a big pad area (mission 4 pad 11: 252 tiles) is entered long before that) */
static int on_pad(int pad, int x, int y)
{
    BYTE *layer = pad < 8 ? PAD_BITS_LAYER : PAD_BYTE_LAYER;
    unsigned n = MAP_TILE_COUNT, w = MAP_TILES_W;
    if (!layer || !w || x < 0 || y < 0) return 0;
    unsigned t = (unsigned)(y / 16) * w + (unsigned)(x / 16);
    if (t >= n || IsBadReadPtr(layer + t, 1)) return 0;
    return pad < 8 ? (layer[t] & (1 << pad)) != 0 : layer[t] == pad;
}

static int dist2(int ax, int ay, int bx, int by) { int dx = ax - bx, dy = ay - by; return dx * dx + dy * dy; }

/* the nearest cell with a fully open 3x3 (strict rule) within r cells of x, y; 1 = found (x, y moved there if needed) */
static int snap_open(int *x, int *y, int r)
{
    int cx = *x / SNAP_CELL, cy = *y / SNAP_CELL;
    for (int d = 0; d <= r; d++)
        for (int dy = -d; dy <= d; dy++)
            for (int dx = -d; dx <= d; dx++) {
                if (abs(dx) != d && abs(dy) != d) continue;
                int gx = cx + dx, gy = cy + dy, all = 1;
                if (gx < 1 || gy < 1 || gx >= SNAP_GRID - 1 || gy >= SNAP_GRID - 1) continue;
                for (int ey = -1; ey <= 1 && all; ey++) for (int ex = -1; ex <= 1 && all; ex++) all = wcell_open((gy + ey) * SNAP_GRID + gx + ex);
                if (all) { if (d) { *x = gx * SNAP_CELL + SNAP_CELL / 2; *y = gy * SNAP_CELL + SNAP_CELL / 2; } return 1; }
            }
    return 0;
}

/* the waypoints of this mission in visiting order (stages in order, nearest group first inside a stage) */
static void resolve(int m, int sx, int sy)
{
    int x[MAX_MEM], y[MAX_MEM], st[MAX_MEM], veh[MAX_MEM], fac[MAX_MEM], n = 0;
    const Way *way[MAX_MEM];
    for (int i = 0; i < NWAYS && n < MAX_MEM; i++) {
        const Way *w = &WAYS[i];
        if (w->mission != m || (w->need && !objectives_need(w->need))) continue;
        int px = 0, py = 0;
        if (w->kind == W_POINT) { px = w->x; py = w->y; }
        else if (w->kind == W_PAD) {
            if (w->pad < 0 || w->pad > 255) continue;
            const short *p = PAD_LOC(w->pad);
            if (!p[0] && !p[1]) { if (g_cfg.log >= 1) mlog("missionpath: mission %d pad %d has no location", m, w->pad); continue; }
            px = p[0] + 8; py = p[1] + 8;
            pad_centre(w->pad, &px, &py);
        } else {
            int raw = 0, s = objectives_item_ground(w->name, &px, &py, &raw, NULL);
            if (s == -2 || s == -1 || (!px && !py)) { if (g_cfg.log >= 1) mlog("missionpath: mission %d item %s %s", m, w->name, s == -2 ? "not found" : s == -1 ? "already done" : "has no spot"); continue; }
            /* 2026-11-20 (user: "moving targets shouldn't get the path"): units, troopers, vehicles move - no waypoint
             * (where a script sends one to a fixed end point, that pad / point is in the table instead). A unit that
             * holds its post (w->post) counts where it stands, if it is on the map now. */
            int at_post = w->post && (raw == 2 || raw == 5) && s == 1;
            if (!at_post && (raw == 2 || raw == 3 || raw == 5)) { if (g_cfg.log >= 1) mlog("missionpath: mission %d item %s %s - no waypoint", m, w->name, w->post ? "not on the map" : "moves"); continue; }
        }
        if (px <= 0 || py <= 0 || px >= 4096 || py >= 4096) continue;
        veh[n] = -1; fac[n] = 0;
        if (w->drive) {
            BYTE *v = NULL; int vx, vy, raw = 0;
            if (objectives_item_ground(w->drive, &vx, &vy, &raw, &v) != -2 && v && raw == 3) { veh[n] = VEH_TYPE(v) - 1; fac[n] = *(short *)(v + 8) & 255; }
        }
        x[n] = px; y[n] = py; st[n] = w->stage; way[n] = w; n++;
    }
    /* groups: grp[i] = the group of entry i (its first entry); member count and coordinate sums per group */
    int grp[MAX_MEM], gn[MAX_MEM], gsx[MAX_MEM], gsy[MAX_MEM];
    for (int i = 0; i < n; i++) { grp[i] = i; gn[i] = 1; gsx[i] = x[i]; gsy[i] = y[i]; }
    for (;;) {
        int ba = -1, bb = -1, bd = 0;
        for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) {
            if (!gn[a] || !gn[b] || st[a] != st[b] || veh[a] != veh[b]) continue;
            int d = dist2(gsx[a] / gn[a], gsy[a] / gn[a], gsx[b] / gn[b], gsy[b] / gn[b]);
            if (d > CLUSTER_PX * CLUSTER_PX || (ba >= 0 && d >= bd)) continue;
            int mx = (gsx[a] + gsx[b]) / (gn[a] + gn[b]), my = (gsy[a] + gsy[b]) / (gn[a] + gn[b]), ok = 1;
            for (int i = 0; i < n && ok; i++) if ((grp[i] == a || grp[i] == b) && dist2(x[i], y[i], mx, my) > CLUSTER_R * CLUSTER_R) ok = 0;
            if (ok) { ba = a; bb = b; bd = d; }
        }
        if (ba < 0) break;
        for (int i = 0; i < n; i++) if (grp[i] == bb) grp[i] = ba;
        gn[ba] += gn[bb]; gsx[ba] += gsx[bb]; gsy[ba] += gsy[bb]; gn[bb] = 0;
    }
    /* order: stage by stage, each time the nearest group left of the lowest stage */
    int cx = sx, cy = sy;
    g_nwp = 0; g_nmem = 0;
    for (;;) {
        int best = -1, bx = 0, by = 0;
        for (int a = 0; a < n; a++) {
            if (!gn[a]) continue;
            int ax = gsx[a] / gn[a], ay = gsy[a] / gn[a];
            if (best < 0 || st[a] < st[best] || (st[a] == st[best] && dist2(cx, cy, ax, ay) < dist2(cx, cy, bx, by))) { best = a; bx = ax; by = ay; }
        }
        if (best < 0 || g_nwp >= MAX_WP) break;
        int k = g_nwp, members = gn[best], ox = bx, oy = by;
        if (members > 1) {                                     /* a centre in a building / rocks: the nearest open spot */
            Guard gd;
            if (GUARD_TRY(gd, "mission path, centre snap (missionpath.c)")) { snap_open(&bx, &by, SNAP_CELLS); guard_leave(&gd); }
            if (g_cfg.log >= 1 && (bx != ox || by != oy)) mlog("missionpath: group centre %d,%d is shut in: moved to %d,%d", ox, oy, bx, by);
        }
        g_wx[k] = bx; g_wy[k] = by; g_wveh[k] = veh[best]; g_dfacing0[k] = fac[best];
        char names[200]; int no = 0; names[0] = 0;
        for (int i = 0; i < n; i++) {
            if (grp[i] != best || g_nmem >= MAX_MEM) continue;
            g_mem[g_nmem] = way[i]; g_mem_wp[g_nmem] = (BYTE)k; g_mem_done[g_nmem] = 0; g_nmem++;
            if (no < 180) no += snprintf(names + no, sizeof names - no, " %s", way[i]->kind == W_ITEM ? way[i]->name : way[i]->kind == W_PAD ? "pad" : "point");
        }
        gn[best] = 0; g_nwp++;
        cx = bx; cy = by;
        if (g_cfg.log >= 1) mlog("missionpath: waypoint %d at %d,%d (stage %d, %d target(s):%s%s)", g_nwp, bx, by, st[best], members, names, veh[best] >= 0 ? ", driven" : "");
    }
}

/* Sarge's point for the done tests: his vehicle's while he drives (2), on foot (1) */
static int sarge_at(int *x, int *y)
{
    BYTE *s = SARGE_ITEM;
    if (!s) return 0;
    BYTE *v = *(BYTE **)(s + 0x2d0);
    if (v && !IsBadReadPtr(v, 8)) { *x = *(short *)(v + 4); *y = *(short *)(v + 6); return 2; }
    *x = *(short *)(s + 4); *y = *(short *)(s + 6);
    return 1;
}
static int names_done(const char *list)                 /* "a|b": every one found is done (and at least one found) */
{
    char buf[96], *p = buf; int found = 0;
    lstrcpynA(buf, list, sizeof buf);
    for (;;) {
        char *bar = strchr(p, '|');
        if (bar) *bar = 0;
        int x, y, raw, s = objectives_item_ground(p, &x, &y, &raw, NULL);
        if (s != -2) { found++; if (s != -1) return 0; }
        if (!bar) return found > 0;
        p = bar + 1;
    }
}
static int wp_done(int k, const char **why)
{
    { int e = escort_wp_done(k, why); if (e >= 0) return e; }
    int sx, sy, how = sarge_at(&sx, &sy);
    if (!how) return 0;
    int r = how == 2 ? REACH_VEH : REACH_PX, near_wp = dist2(sx, sy, g_wx[k], g_wy[k]) <= r * r, all = 1, any = 0;
    /* only a stop reached ON FOOT (the vehicle pickup) - not a stop of a driven leg (2026-11-21: mission 3's bridge stop was
     * skipped the moment Sarge got into the tank) */
    if (k + 1 < g_nwp && g_wveh[k] < 0 && g_wveh[k + 1] >= 0 && how == 2) { *why = "Sarge is in a vehicle for the driven leg"; return 1; }
    for (int i = 0; i < g_nmem; i++) {
        if (g_mem_wp[i] != k) continue;
        any = 1;
        if (g_mem_done[i]) continue;
        const Way *w = g_mem[i]; int d = 0;
        if (w->kind == W_ITEM) { int x, y, raw; d = objectives_item_ground(w->name, &x, &y, &raw, NULL) == -1; if (d) *why = "target done"; }
        else if (w->kind == W_PAD) {
            const short *p = PAD_LOC(w->pad);
            d = near_wp || dist2(sx, sy, p[0] + 8, p[1] + 8) <= r * r || on_pad(w->pad, sx, sy); if (d) *why = "pad reached";
        } else if (w->until) { d = names_done(w->until); if (d) *why = "its targets done"; }
        else { d = near_wp || dist2(sx, sy, w->x, w->y) <= r * r; if (d) *why = "point reached"; }
        if (d) g_mem_done[i] = 1; else all = 0;
    }
    return any && all;
}
static void progress(void)
{
    while (g_cur < g_nwp) {
        const char *why = "";
        if (!wp_done(g_cur, &why)) break;
        g_cur++;
        if (g_cfg.log >= 1) {
            if (g_cur < g_nwp) mlog("missionpath: waypoint %d done (%s): leg %d to %d,%d shown", g_cur, why, g_cur + 1, g_wx[g_cur], g_wy[g_cur]);
            else mlog("missionpath: waypoint %d done (%s): the last one - no leg shown", g_cur, why);
        }
    }
}

/* ---------------------------------------------------------------- escort: the lane follows the vehicle (2026-11-21)
 * User: "for the following the halftrack mission - have the mission path follow the halftrack path and don't start the
 * next leg until the halftrack gets there". The half-track missions (Army Men\Maps):
 *   - mission 8 (des4_1, the escort): GreenHalftrack3 is sent pad to pad by the script (ORDER GOTO Path15, on_Path16 ->
 *     Path22, on_Path23 -> Path26, 27 -> 29, 30 -> 35, 36 -> 39, 40 -> 45, 46 -> 48, 49 -> 50; on_Path55 = MadeIt;
 *     des4_1:269-347; a goal not reached is ordered again 5 s later);
 *   - mission 6 (des3_1:132-136): GreenHalftrack1 GOTO (2390, 2700), the win is the half-track on pad 11 (TripLine);
 *   - mission 7 (des3_2:168-170): GreenHalftrack1 GOTO (509, 3760), the win is the half-track AND Sarge on pad 10.
 * While the escort waypoint is the current one and the vehicle is alive, the lane shown is:
 *   1. the vehicle's position (VEHICLE +4 / +6), then the route the GAME's vehicle AI follows now: VEHICLE +0xe4 aiGoal
 *      (SPOINT, the script ORDER's goal: FUN_0041d940 stores it; 0 = none, cleared on arrival in FUN_00430140),
 *      VEHICLE +0xe0 = the AI controller (0x1e4 bytes, FUN_00430040; +0x1c0 back pointer to the vehicle) whose route
 *      record starts at +0 (FUN_004377c0 / FUN_004378f0 / FUN_00437b20): +0x00 goal SPOINT, +0x04 done flag, +0x0c the
 *      next route point, +0x10 count of the points after it, +0x14 those points (SPOINTs, <= 97; the game's planner
 *      FUN_0042b0d0 fills them, FUN_00437b20 pops the next one), then the goal itself;
 *   2. then the script's later goals in order (ESCORTS), each piece planned once by the vehicle tile planner
 *      (path_find_vehicle with the vehicle's footprint) when the escort starts - the game gives those orders later;
 *   3. where the vehicle has no route now (waiting for the next order, the 5 s retry, the soldiers boarding) or its
 *      goal is not one of the script goals: a piece from it (or its goal) to the next script goal, planned the same way
 *      (resumable, one call per frame, again when the vehicle moved > ESC_REPLAN_PX).
 *   Rebuilt every frame (the vehicle moves); no variation (it is the vehicle's real path). The goal index only moves on:
 *   to a script goal the vehicle is ordered to, or past the one it stands on.
 * DONE: the vehicle on its finish pad (the script's own win test), plus Sarge on it where the script asks that too;
 *   Sarge reaching the waypoint does not count. If the vehicle is destroyed / gone the mission is lost (the game restarts
 *   it): the last lane stays and the waypoint is never done - no switch to Sarge's own leg (user 2026-11-21). */
typedef struct { int pad, x, y; } EscGoal;               /* pad >= 0: that pad's centre, else the point */
typedef struct { int mission; const char *vehicle; int n; EscGoal goal[12]; int finish_pad, sarge_too; } Escort;
static const Escort ESCORTS[] = {
    { 6, "GreenHalftrack1", 1, { { -1, 2390, 2700 } }, 11, 0 },
    { 7, "GreenHalftrack1", 1, { { -1, 509, 3760 } }, 10, 1 },
    { 8, "GreenHalftrack3", 10, { { 15 }, { 22 }, { 26 }, { 29 }, { 35 }, { 39 }, { 45 }, { 48 }, { 50 }, { 55 } }, 55, 0 },
};
#define ESC_MAX        13                /* script goals + the waypoint */
#define ESC_PTS        1200
#define ESC_MATCH_PX   64                /* the vehicle's goal is this script goal (the game snaps the goal to a legal tile) */
#define ESC_AT_PX      48                /* the vehicle is at a point goal */
#define ESC_REPLAN_PX  48
#define ESC_REPLAN_MS  500
#define ESC_LOG_MS     2000
#define AI_CTRL(v)     (*(BYTE **)((v) + 0xe0))
#define AI_GOAL(v)     (*(DWORD *)((v) + 0xe4))
#define RT_GOAL(c)     (*(DWORD *)((c) + 0x00))
#define RT_DONE(c)     (*(BYTE *)((c) + 0x04))
#define RT_NEXT(c)     (*(DWORD *)((c) + 0x0c))
#define RT_COUNT(c)    (*(int *)((c) + 0x10))
#define RT_PTS(c)      ((const DWORD *)((c) + 0x14))
#define RT_VEHICLE(c)  (*(BYTE **)((c) + 0x1c0))
#define RT_MAX         97
#define CTRL_SIZE      0x1e4
#define SPX(p)         ((short)((p) & 0xffff))
#define SPY(p)         ((short)((p) >> 16))

typedef struct { MissionPt *pts; int max, n, cx, cy, tx, ty, calls, fin; } Plan;   /* a chained tile-planner run */
static const Escort *g_esc; static int g_esc_wp = -1, g_esc_type, g_esc_ng, g_esc_idx, g_esc_dead, g_esc_logged_on;
static int g_esc_gx[ESC_MAX], g_esc_gy[ESC_MAX], g_esc_gpad[ESC_MAX];
static MissionPt g_tail[ESC_PTS]; static int g_tail_first[ESC_MAX + 1], g_tail_seg; static Plan g_tplan;   /* goal i-1 -> goal i */
static MissionPt g_bbuf[2][ESC_PTS / 2]; static int g_bshow = -1, g_bshow_n, g_bshow_to, g_bshow_fx, g_bshow_fy;   /* bridge shown */
/* 2026-11-21 (user: "halftrack path - just draw it once at mission start. don't update it"): the escort lane is built
 * until its planned pieces are done (or ESC_FREEZE_MS after it first showed), then frozen for the rest of the escort. */
#define ESC_FREEZE_MS 4000
static int g_esc_frozen; static DWORD g_esc_on_t;
static Plan g_bplan; static int g_bplan_on, g_bplan_to, g_bplan_fx, g_bplan_fy; static DWORD g_bplan_t;          /* bridge being planned */
static MissionPt g_lane[ESC_PTS]; static int g_lane_n; static DWORD g_esc_log_t;

static void plan_begin(Plan *p, MissionPt *buf, int max, int fx, int fy, int tx, int ty)
{
    p->pts = buf; p->max = max; p->n = 0; p->cx = fx; p->cy = fy; p->tx = tx; p->ty = ty; p->calls = 0; p->fin = 0;
}
static void plan_add(Plan *p, int x, int y, int dashed)
{
    if (p->n >= p->max) return;
    if (p->n && p->pts[p->n - 1].x == x && p->pts[p->n - 1].y == y) return;
    p->pts[p->n++] = (MissionPt){ (short)x, (short)y, (BYTE)dashed, 0 };
}
static void plan_step(Plan *p, int type)                 /* one path_find_vehicle call; p->fin when the piece is finished */
{
    if (p->fin) return;
    POINT out[96]; volatile int n = 0; Guard gd;
    if (GUARD_TRY(gd, "mission path, escort (missionpath.c)")) { n = path_find_vehicle(type, p->cx, p->cy, p->tx, p->ty, out, 96); guard_leave(&gd); }
    else n = -1;
    p->calls++;
    int before = dist2(p->cx, p->cy, p->tx, p->ty);
    if (n > 0) { for (int i = 0; i < n; i++) plan_add(p, out[i].x, out[i].y, 0); p->cx = out[n - 1].x; p->cy = out[n - 1].y; }
    int after = dist2(p->cx, p->cy, p->tx, p->ty), reached = after <= 32 * 32;
    if (reached || n <= 0 || before - after < MIN_PROGRESS * MIN_PROGRESS || p->calls >= LEG_CALLS) {
        if (!reached && g_cfg.log >= 1) mlog("missionpath: escort piece to %d,%d ends at %d,%d (%s) - rest dashed", p->tx, p->ty, p->cx, p->cy,
                                             n <= 0 ? "no route" : p->calls >= LEG_CALLS ? "out of pieces" : "no progress");
        if (reached && p->n) p->n--;                      /* the last point becomes the target itself */
        plan_add(p, p->tx, p->ty, !reached);
        p->fin = 1;
    }
}

/* the escort vehicle now: NULL when this mission has none or it is destroyed / gone (then g_esc_dead) */
static BYTE *esc_vehicle(void)
{
    if (!g_esc || g_esc_dead) return NULL;
    int x, y, raw = 0; BYTE *v = NULL;
    int s = objectives_item_ground(g_esc->vehicle, &x, &y, &raw, &v);
    if (s == 1 && v && raw == 3 && *(short *)(v + 0x1c) > 0) return v;
    if (s == -1 || s == -2) {
        g_esc_dead = 1;
        if (g_cfg.log >= 1) mlog("missionpath: escort %s %s - its last lane stays, waypoint %d not done (the mission is lost)", g_esc->vehicle, s == -1 ? "destroyed" : "gone", g_esc_wp + 1);
    }
    return NULL;
}
static void escort_setup(void)                           /* the route is ready (built or reused): find this mission's escort */
{
    g_esc = NULL; g_esc_wp = -1; g_esc_dead = 0; g_esc_idx = 0; g_tail_seg = 0; g_lane_n = 0; g_esc_logged_on = 0; g_esc_frozen = 0; g_esc_on_t = 0;
    g_bshow = -1; g_bplan_on = 0;
    BYTE *g = GAME;
    if (!g) return;
    int m = GAME_MISSION(g);
    for (int i = 0; i < g_nmem && g_esc_wp < 0; i++) if (g_mem[i]->escort) {
        for (int e = 0; e < (int)(sizeof ESCORTS / sizeof *ESCORTS); e++)
            if (ESCORTS[e].mission == m && !lstrcmpA(ESCORTS[e].vehicle, g_mem[i]->escort)) { g_esc = &ESCORTS[e]; g_esc_wp = g_mem_wp[i]; }
    }
    if (!g_esc) return;
    int x, y, raw = 0; BYTE *v = NULL;
    if (objectives_item_ground(g_esc->vehicle, &x, &y, &raw, &v) != 1 || !v || raw != 3) {
        if (g_cfg.log >= 1) mlog("missionpath: escort %s not on the map - normal leg", g_esc->vehicle);
        g_esc = NULL; g_esc_wp = -1; return;
    }
    g_esc_type = VEH_TYPE(v) - 1;
    g_esc_ng = 0;
    for (int i = 0; i < g_esc->n && g_esc_ng < ESC_MAX - 1; i++) {
        const EscGoal *eg = &g_esc->goal[i]; int px = eg->x, py = eg->y;
        if (eg->pad >= 0) {
            const short *p = PAD_LOC(eg->pad);
            if (!p[0] && !p[1]) { if (g_cfg.log >= 1) mlog("missionpath: escort pad %d has no location - skipped", eg->pad); continue; }
            px = p[0] + 8; py = p[1] + 8; pad_centre(eg->pad, &px, &py);
        }
        g_esc_gx[g_esc_ng] = px; g_esc_gy[g_esc_ng] = py; g_esc_gpad[g_esc_ng] = eg->pad; g_esc_ng++;
    }
    int k = g_esc_wp;                                     /* the waypoint itself last, when it is not the last goal */
    if (!g_esc_ng || dist2(g_esc_gx[g_esc_ng - 1], g_esc_gy[g_esc_ng - 1], g_wx[k], g_wy[k]) > ESC_AT_PX * ESC_AT_PX) {
        g_esc_gx[g_esc_ng] = g_wx[k]; g_esc_gy[g_esc_ng] = g_wy[k]; g_esc_gpad[g_esc_ng] = g_esc->finish_pad; g_esc_ng++;
    }
    g_tail_first[0] = 0; g_tail_first[1] = 0; g_tail_seg = 1;
    if (g_esc_ng > 1) plan_begin(&g_tplan, g_tail, ESC_PTS, g_esc_gx[0], g_esc_gy[0], g_esc_gx[1], g_esc_gy[1]);
    if (g_cfg.log >= 1) {
        char b[300]; int o = 0; b[0] = 0;
        for (int i = 0; i < g_esc_ng && o < 260; i++) o += wsprintfA(b + o, " %d,%d%s", g_esc_gx[i], g_esc_gy[i], g_esc_gpad[i] >= 0 ? "(pad)" : "");
        mlog("missionpath: escort %s (vehicle type %d) for waypoint %d, finish pad %d%s, script goals:%s", g_esc->vehicle, g_esc_type, g_esc_wp + 1,
             g_esc->finish_pad, g_esc->sarge_too ? " (with Sarge)" : "", b);
    }
}
static int esc_at_goal(int i, int x, int y)              /* x, y at / on script goal i */
{
    if (g_esc_gpad[i] >= 0 && on_pad(g_esc_gpad[i], x, y)) return 1;
    return dist2(x, y, g_esc_gx[i], g_esc_gy[i]) <= ESC_AT_PX * ESC_AT_PX;
}
static int esc_goal_index(int x, int y)                  /* the script goal (from the current one on) this point is, else -1 */
{
    for (int i = g_esc_idx; i < g_esc_ng; i++)
        if ((g_esc_gpad[i] >= 0 && on_pad(g_esc_gpad[i], x, y)) || dist2(x, y, g_esc_gx[i], g_esc_gy[i]) <= ESC_MATCH_PX * ESC_MATCH_PX) return i;
    for (int i = g_esc_idx; i < g_esc_ng; i++) {          /* a pad goal whose padLocations tile (the game's GOTO point) is off its centre */
        if (g_esc_gpad[i] < 0) continue;
        const short *p = PAD_LOC(g_esc_gpad[i]);
        if (dist2(x, y, p[0] + 8, p[1] + 8) <= ESC_MATCH_PX * ESC_MATCH_PX) return i;
    }
    return -1;
}
/* the game's route points (16 px tiles, 8-way steps) thinned for drawing: from each kept point, the farthest point
 * with every point between within ESC_THIN_PX of the straight line is the next one kept; lane_add'ed (the first, the
 * vehicle, is in the lane already); rp[n - 1] stays the last point */
#define ESC_THIN_PX 20.0
static void lane_add(int x, int y, int dashed);
static void esc_thin(POINT *rp, int n)
{
    int j = 0;
    while (j < n - 1) {
        int k = j + 1;
        for (int t = j + 2; t < n; t++) {
            double sx = rp[t].x - rp[j].x, sy = rp[t].y - rp[j].y, l = sqrt(sx * sx + sy * sy); int ok = l > 0.5;
            for (int m = j + 1; m < t && ok; m++) if (fabs((rp[m].x - rp[j].x) * sy - (rp[m].y - rp[j].y) * sx) / l > ESC_THIN_PX) ok = 0;
            if (!ok) break;
            k = t;
        }
        lane_add(rp[k].x, rp[k].y, 0);
        j = k;
    }
}
static void lane_add(int x, int y, int dashed)
{
    if (g_lane_n >= ESC_PTS) return;
    if (g_lane_n && g_lane[g_lane_n - 1].x == x && g_lane[g_lane_n - 1].y == y) return;
    g_lane[g_lane_n++] = (MissionPt){ (short)x, (short)y, (BYTE)dashed, 0 };
}
/* the piece from fx, fy to script goal `to`: the planned one when it fits, else planning starts (straight dashed meanwhile) */
static void lane_bridge(int fx, int fy, int to)
{
    DWORD now = GetTickCount();
    int fits = g_bshow >= 0 && g_bshow_to == to && dist2(fx, fy, g_bshow_fx, g_bshow_fy) <= ESC_REPLAN_PX * ESC_REPLAN_PX;
    if (!fits && (!g_bplan_on || g_bplan_to != to) && (now - g_bplan_t >= ESC_REPLAN_MS || g_bshow < 0 || g_bshow_to != to)) {
        int buf = g_bshow == 0 ? 1 : 0;
        plan_begin(&g_bplan, g_bbuf[buf], ESC_PTS / 2, fx, fy, g_esc_gx[to], g_esc_gy[to]);
        g_bplan_on = 1; g_bplan_to = to; g_bplan_fx = fx; g_bplan_fy = fy; g_bplan_t = now;
    }
    if (g_bshow >= 0 && g_bshow_to == to) {               /* the last planned piece (until the new one is ready) */
        int s = 0;                                        /* from its point nearest to fx, fy: the part already driven is dropped */
        for (int i = 1; i < g_bshow_n; i++) if (dist2(fx, fy, g_bbuf[g_bshow][i].x, g_bbuf[g_bshow][i].y) < dist2(fx, fy, g_bbuf[g_bshow][s].x, g_bbuf[g_bshow][s].y)) s = i;
        for (int i = s; i < g_bshow_n; i++) lane_add(g_bbuf[g_bshow][i].x, g_bbuf[g_bshow][i].y, g_bbuf[g_bshow][i].dashed);
    } else lane_add(g_esc_gx[to], g_esc_gy[to], 1);
}
static void lane_tail(int from)                          /* the planned pieces after script goal `from` */
{
    for (int s = from + 1; s < g_esc_ng; s++) {
        if (s >= g_tail_seg) { lane_add(g_esc_gx[s], g_esc_gy[s], 1); continue; }   /* not planned yet: straight, dashed */
        for (int i = g_tail_first[s]; i < g_tail_first[s + 1]; i++) lane_add(g_tail[i].x, g_tail[i].y, g_tail[i].dashed);
    }
}
static void escort_frame(void)                           /* game thread, every play frame once the route is ready */
{
    int keep = g_lane_n;                                  /* 2026-11-21 (user): a destroyed escort loses the mission - */
    g_lane_n = 0;                                         /* its last lane stays, no switch to Sarge's own leg */
    if (!g_esc || g_esc_ng < 1) return;
    if (g_esc_dead || g_esc_frozen) { g_lane_n = keep; return; }
    if (g_tail_seg < g_esc_ng && g_esc_ng > 1) {          /* the script pieces: one planner call per frame */
        plan_step(&g_tplan, g_esc_type);                  /* piece s = g_tail[g_tail_first[s] .. g_tail_first[s + 1]) */
        if (g_tplan.fin) {
            int base = g_tail_first[g_tail_seg] + g_tplan.n;
            g_tail_first[g_tail_seg + 1] = base;
            if (g_cfg.log >= 1) mlog("missionpath: escort script piece %d (%d,%d -> %d,%d): %d points", g_tail_seg, g_esc_gx[g_tail_seg - 1], g_esc_gy[g_tail_seg - 1],
                                     g_esc_gx[g_tail_seg], g_esc_gy[g_tail_seg], g_tplan.n);
            g_tail_seg++;
            if (g_tail_seg < g_esc_ng)                    /* the next piece continues in the same buffer */
                plan_begin(&g_tplan, g_tail + base, ESC_PTS - base, g_esc_gx[g_tail_seg - 1], g_esc_gy[g_tail_seg - 1], g_esc_gx[g_tail_seg], g_esc_gy[g_tail_seg]);
        }
    }
    if (g_bplan_on) {                                     /* the bridge being planned: one call per frame */
        plan_step(&g_bplan, g_esc_type);
        if (g_bplan.fin) {
            g_bshow = g_bplan.pts == g_bbuf[0] ? 0 : 1; g_bshow_n = g_bplan.n; g_bshow_to = g_bplan_to; g_bshow_fx = g_bplan_fx; g_bshow_fy = g_bplan_fy;
            g_bplan_on = 0;
            if (g_cfg.log >= 1) mlog("missionpath: escort piece from %d,%d to goal %d (%d,%d): %d points", g_bplan_fx, g_bplan_fy, g_bplan_to + 1, g_esc_gx[g_bplan_to], g_esc_gy[g_bplan_to], g_bshow_n);
        }
    }
    if (g_cur != g_esc_wp) return;                        /* the lane only while its waypoint is the current one */
    BYTE *v = esc_vehicle();
    if (!v) { if (g_esc_dead) g_lane_n = keep; return; }
    int vx = *(short *)(v + 4), vy = *(short *)(v + 6);
    while (g_esc_idx < g_esc_ng - 1 && esc_at_goal(g_esc_idx, vx, vy)) {
        g_esc_idx++;
        if (g_cfg.log >= 1) mlog("missionpath: escort %s at %d,%d passed script goal %d: next %d,%d", g_esc->vehicle, vx, vy, g_esc_idx, g_esc_gx[g_esc_idx], g_esc_gy[g_esc_idx]);
    }
    DWORD goal = AI_GOAL(v); BYTE *c = AI_CTRL(v);
    int live = SPX(goal) != 0 && c && !IsBadReadPtr(c, CTRL_SIZE) && RT_VEHICLE(c) == v && !RT_DONE(c);
    int gi = SPX(goal) ? esc_goal_index(SPX(goal), SPY(goal)) : -1;
    if (gi > g_esc_idx) {
        if (g_cfg.log >= 1) mlog("missionpath: escort %s ordered to %d,%d = script goal %d", g_esc->vehicle, SPX(goal), SPY(goal), gi + 1);
        g_esc_idx = gi;
    }
    lane_add(vx, vy, 0);
    int rn = 0;
    if (live) {                                           /* the game's own route: next point, the points after it, the goal */
        POINT rp[RT_MAX + 2];
        rp[rn++] = (POINT){ vx, vy };
        DWORD nx = RT_NEXT(c);
        if (SPX(nx)) rp[rn++] = (POINT){ SPX(nx), SPY(nx) };
        int cnt = RT_COUNT(c);
        if (cnt < 0) cnt = 0;
        if (cnt > RT_MAX) cnt = RT_MAX;
        for (int i = 0; i < cnt; i++) { DWORD p = RT_PTS(c)[i]; if (SPX(p)) rp[rn++] = (POINT){ SPX(p), SPY(p) }; }
        esc_thin(rp, rn);                                 /* the game's 16 px tile steps zig-zag: straight runs kept as one */
        int ex = rp[rn - 1].x, ey = rp[rn - 1].y;
        rn--;                                             /* (log: the game's points, without the vehicle) */
        /* the game plans at most ~97 points ahead (FUN_0042b0d0, 2000 nodes) and plans on from there later: the rest to
         * the goal is planned here (a straight line would cross the canyon walls) */
        if (dist2(ex, ey, SPX(goal), SPY(goal)) > ESC_AT_PX * ESC_AT_PX && gi >= 0) lane_bridge(ex, ey, gi);
        else lane_add(SPX(goal), SPY(goal), 0);
        if (gi >= 0) lane_tail(gi);
        else { lane_bridge(SPX(goal), SPY(goal), g_esc_idx); lane_tail(g_esc_idx); }
    } else {
        lane_bridge(vx, vy, g_esc_idx);
        lane_tail(g_esc_idx);
    }
    if (g_lane_n >= 2) g_lane[g_lane_n - 1].wp = (BYTE)(g_esc_wp + 1);
    if (!g_esc_on_t) g_esc_on_t = GetTickCount();
    if (g_lane_n >= 2 && ((g_tail_seg >= g_esc_ng && !g_bplan_on) || GetTickCount() - g_esc_on_t >= ESC_FREEZE_MS)) {
        g_esc_frozen = 1;
        if (g_cfg.log >= 1) mlog("missionpath: escort lane frozen (%d points) - drawn once, not updated", g_lane_n);
    }
    if (!g_esc_logged_on && g_cfg.log >= 1) { g_esc_logged_on = 1; mlog("missionpath: escort lane on: leg %d follows %s from %d,%d", g_esc_wp + 1, g_esc->vehicle, vx, vy); }
    DWORD now = GetTickCount();
    if (g_cfg.log >= 1 && now - g_esc_log_t >= ESC_LOG_MS) {
        g_esc_log_t = now;
        char b[200]; int o = 0; b[0] = 0;
        for (int i = 1; i < g_lane_n && i <= 8 && o < 170; i++) o += wsprintfA(b + o, " %d,%d%s", g_lane[i].x, g_lane[i].y, g_lane[i].dashed ? "d" : "");
        mlog("missionpath: escort %s at %d,%d goal %d,%d (%s, game route %d points, script goal %d of %d): lane %d points:%s%s", g_esc->vehicle, vx, vy,
             SPX(goal), SPY(goal), live ? "driving" : "no route", rn, g_esc_idx + 1, g_esc_ng, g_lane_n, b, g_lane_n > 9 ? " ..." : "");
    }
}
static int escort_lane(const MissionPt **pts)
{
    if (!g_esc || g_cur != g_esc_wp || g_lane_n < 2) return 0;
    *pts = g_lane;
    return g_lane_n;
}
static int escort_wp_done(int k, const char **why)
{
    if (!g_esc || k != g_esc_wp) return -1;
    BYTE *v = esc_vehicle();
    if (!v) return g_esc_dead ? 0 : -1;                  /* destroyed: never done (the game loses the mission and restarts it) */
    int vx = *(short *)(v + 4), vy = *(short *)(v + 6), on = on_pad(g_esc->finish_pad, vx, vy);
    if (!on && g_esc->finish_pad < 0) on = dist2(vx, vy, g_wx[k], g_wy[k]) <= ESC_AT_PX * ESC_AT_PX;
    if (on && g_esc->sarge_too) { int sx, sy; on = sarge_at(&sx, &sy) && on_pad(g_esc->finish_pad, sx, sy); }
    if (on) *why = g_esc->sarge_too ? "the escorted vehicle and Sarge on the finish pad" : "the escorted vehicle on the finish pad";
    return on;
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
#define WIDE_BUDGET_MS 6.0               /* per frame for one wide search (as W_BUDGET_MS) */
static int g_wide_on;                    /* a sliced wide search is running (reset with every new build) */
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

static int g_relax, g_walk_npts, g_walk_cx, g_walk_cy;                      /* 1 = this leg's second walking search (relaxed cell rule) */
/* cell class: 1 free, 2 blocked, 3 a breakable fence only ([gameplay] break_fences, fences.c: passable at W_FENCE_COST,
 * Sarge breaks it there; it counts as an obstacle for the clearance and the straightening) */
#define W_FENCE_COST 10.0f
static int wclass(int c)
{
    if (!g_wfree[c]) {
        int x0 = (c % WG) * WCELL, y0 = (c / WG) * WCELL, cls = 1;
        int near_start = abs(c % WG - g_wstart % WG) <= 1 && abs(c / WG - g_wstart / WG) <= 1;
        if (g_relax) {                   /* second try: the game's own Sarge rule (object masks), centre + 4 points 2 px out */
            static const int RX[5] = { WCELL / 2, 2, 6, 2, 6 }, RY[5] = { WCELL / 2, 2, 2, 6, 6 };
            for (int k = 0; k < (near_start ? 1 : 5) && cls != 2; k++)
                if (controls_sarge_blocked_at(x0 + RX[k], y0 + RY[k])) cls = fences_only_at(x0 + RX[k], y0 + RY[k], 0) ? 3 : 2;
        } else cls = path_cell_class(x0, y0, near_start);   /* 1 free, 2 blocked, 3 fence (pathfind.c) */
        g_wfree[c] = (BYTE)cls;
        if (near_start) { g_wfree[c] = 0; return cls; }   /* tested anew for the next leg's start */
    }
    return g_wfree[c];
}
static int wraw(int c) { return wclass(c) != 2; }
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
                    if (nx < 0 || ny < 0 || nx >= WG || ny >= WG || wclass(ny * WG + nx) != 1) { d = r; break; }
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
        if (wclass(c) != 1 || wclear(c) < need) return 0;   /* never straight through a fence cell */
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
/* a whole cell free by this search's rule (strict or relaxed), with no near-start exemption - for the start snap */
static int wcell_open(int c)
{
    int x0 = (c % WG) * WCELL, y0 = (c / WG) * WCELL;
    for (int sy = 1; sy < WCELL; sy += 2)
        for (int sx = 1; sx < WCELL; sx += 2)
            if (g_relax ? controls_sarge_blocked_at(x0 + sx, y0 + sy) : controls_blocked_at(x0 + sx, y0 + sy)) return 0;
    return 1;
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
    /* 2026-11-20 (user, mission 4: legs from TanRifle21/22's spots in the rocks / gorge were dashed straight over the
     * terrain, "should be straight connection" / "back tracking, so pathing shouldn't be going through terrain"): a
     * waypoint inside rocks left the search no way out of its first cell (0 points). Start from the nearest cell that
     * has a fully open 3x3 around it (so the search can leave it); the line still begins at the waypoint. */
    {
        int ok = 1, sxc = g_wstart % WG, syc = g_wstart / WG;
        for (int dy = -1; dy <= 1 && ok; dy++) for (int dx = -1; dx <= 1 && ok; dx++) {
            int x = sxc + dx, y = syc + dy;
            if (x < 0 || y < 0 || x >= WG || y >= WG || !wcell_open(y * WG + x)) ok = 0;
        }
        for (int r = 1; r <= 12 && !ok; r++)
            for (int dy = -r; dy <= r && !ok; dy++)
                for (int dx = -r; dx <= r && !ok; dx++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    int x = sxc + dx, y = syc + dy, all = 1;
                    if (x < 1 || y < 1 || x >= WG - 1 || y >= WG - 1) continue;
                    for (int ey = -1; ey <= 1 && all; ey++) for (int ex = -1; ex <= 1 && all; ex++) all = wcell_open((y + ey) * WG + x + ex);
                    if (all) {
                        ok = 1; g_wstart = y * WG + x;
                        g_wcx = x * WCELL + WCELL / 2; g_wcy = y * WCELL + WCELL / 2;
                        if (g_cfg.log >= 1) mlog("missionpath: leg start %d,%d is shut in: searching from %d,%d", sx, sy, g_wcx, g_wcy);
                    }
                }
    }
    int gopen = 1;                                              /* the goal needs an open 3x3 too (else: a gap in rocks) */
    for (int ey = -1; ey <= 1 && gopen; ey++) for (int ex = -1; ex <= 1 && gopen; ex++) {
        int x = g_wgoal % WG + ex, y = g_wgoal / WG + ey;
        gopen = x >= 0 && y >= 0 && x < WG && y < WG && wcell_open(y * WG + x);
    }
    if (!gopen) {                                               /* a pad in a doorway, a soldier's spot: nearest open */
        int best = -1;
        for (int r = 1; r <= 12 && best < 0; r++)
            for (int dy = -r; dy <= r && best < 0; dy++)
                for (int dx = -r; dx <= r && best < 0; dx++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    int x = g_wgoal % WG + dx, y = g_wgoal / WG + dy;
                    if (x < 1 || y < 1 || x >= WG - 1 || y >= WG - 1) continue;
                    int all = 1;                    /* a cell with an open 3x3 round it, not a gap inside the rocks */
                    for (int ey = -1; ey <= 1 && all; ey++) for (int ex = -1; ex <= 1 && all; ex++) all = wcell_open((y + ey) * WG + x + ex);
                    if (all) best = y * WG + x;
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
                float g = g_wcost[c] + (d >= 4 ? 1.4142f : 1.0f) + near_cost(nb) + (wclass(nb) == 3 ? W_FENCE_COST : 0.0f);
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
/* ---------------------------------------------------------------- variation (2026-11-21)
 * User: "would be neat to have the paths change randomly - so calculate on mission load. keep the same path on
 * restarts. but if coming in from the main menu, change it. Make sure the paths don't go completely out of the way,
 * but if possible to have a little bit of variation". Per WALKING leg (driven legs: always the vehicle's shortest):
 *   1. the shortest route A -> B (the planners above, as before);
 *   2. a VIA point V: at a random 35..65 % along A-B, moved sideways by a random 10..24 % of |AB| (40..360 px, random
 *      side), moved to the nearest open 3x3 (<= VIA_SNAP cells); then A -> V and V -> B with the same planners;
 *   3. the variant is kept only when both pieces were found and its length <= VAR_MAX_PCT % of the shortest's; else a
 *      second via with half the offset; else the shortest. Legs shorter than VAR_MIN_PX keep the shortest.
 * The random numbers come from the mission's seed (g_seed, xorshift per leg and try): the same seed gives the same
 * vias. [hud] mission_path_variation=0: always the shortest. */
#define VAR_MIN_PX   320
#define VAR_MAX_PCT  118
#define VAR_TRIES    2
#define VIA_SNAP     16
static unsigned g_seed;
static int g_vstage;                     /* 0 shortest, 1 A -> via, 2 via -> B */
static int g_vtry, g_vx, g_vy, g_leg_s0, g_leg_e0, g_leg_ax, g_leg_ay;

static unsigned rnd(unsigned *s) { unsigned x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x ? x : 0x9e3779b9u; }
static double run_len(int ax, int ay, int a, int b)       /* A, then g_pts[a .. b) */
{
    double l = 0; int px = ax, py = ay;
    for (int i = a; i < b; i++) { l += sqrt((double)dist2(px, py, g_pts[i].x, g_pts[i].y)); px = g_pts[i].x; py = g_pts[i].y; }
    return l;
}
static int run_dashed(int a, int b) { for (int i = a; i < b; i++) if (g_pts[i].dashed) return 1; return 0; }
static int pick_via(void)
{
    int ax = g_leg_ax, ay = g_leg_ay, bx = g_wx[g_leg], by = g_wy[g_leg];
    double dx = bx - ax, dy = by - ay, d = sqrt(dx * dx + dy * dy);
    if (d < VAR_MIN_PX) return 0;
    unsigned s = g_seed ^ (0x9e3779b9u * (unsigned)(g_leg + 1)) ^ (0x85ebca6bu * (unsigned)(g_vtry + 1));
    for (int i = 0; i < 4; i++) rnd(&s);
    double t = 0.35 + 0.30 * (rnd(&s) % 1000) / 1000.0;
    double off = (0.10 + 0.14 * (rnd(&s) % 1000) / 1000.0) * d / (g_vtry + 1);
    if (off < 40) off = 40;
    if (off > 360) off = 360;
    if (rnd(&s) & 1) off = -off;
    int vx = (int)(ax + t * dx - off * dy / d), vy = (int)(ay + t * dy + off * dx / d);
    const int *br = (const int *)0x4e2ff4;                 /* the_map.boundrect l t r b */
    if (br[2] > br[0] && br[3] > br[1]) {
        if (vx < br[0] + 32) vx = br[0] + 32;
        if (vy < br[1] + 32) vy = br[1] + 32;
        if (vx > br[2] - 32) vx = br[2] - 32;
        if (vy > br[3] - 32) vy = br[3] - 32;
    }
    if (vx < 16 || vy < 16 || vx >= 4080 || vy >= 4080) return 0;
    Guard gd; volatile int ok = 0;
    if (GUARD_TRY(gd, "mission path, via (missionpath.c)")) { ok = snap_open(&vx, &vy, VIA_SNAP); guard_leave(&gd); }
    if (!ok) { if (g_cfg.log >= 1) mlog("missionpath: leg %d variant %d: no open ground near the via point", g_leg + 1, g_vtry + 1); return 0; }
    g_vx = vx; g_vy = vy;
    if (g_cfg.log >= 1) mlog("missionpath: leg %d variant %d: via %d,%d (%.0f %% along, %+.0f px aside)", g_leg + 1, g_vtry + 1, vx, vy, t * 100, off);
    return 1;
}
static void piece_reset(void) { g_leg_calls = 0; g_wphase = 0; g_dphase = 0; g_dpieces = 0; }
static void leg_begin(void) { g_vstage = 0; g_vtry = 0; g_leg_s0 = g_npts; g_leg_ax = g_cx; g_leg_ay = g_cy; g_leg_first[g_leg] = g_npts - 1; }
static void leg_finish(void)
{
    g_cx = g_wx[g_leg]; g_cy = g_wy[g_leg];
    g_leg++; piece_reset();
    g_leg_first[g_leg] = g_npts - 1;
    if (g_leg < g_nwp) leg_begin();
}
static void variant_try_next(void)                          /* the variant failed: back to the shortest, maybe once more */
{
    g_npts = g_leg_e0;
    if (++g_vtry < VAR_TRIES && pick_via()) { g_vstage = 1; g_cx = g_leg_ax; g_cy = g_leg_ay; piece_reset(); return; }
    if (g_cfg.log >= 1) mlog("missionpath: leg %d: the shortest route kept", g_leg + 1);
    leg_finish();
}
/* the current piece's target: the via point while planning A -> V, else the waypoint */
static void piece_target(int *tx, int *ty) { if (g_vstage == 1) { *tx = g_vx; *ty = g_vy; } else { *tx = g_wx[g_leg]; *ty = g_wy[g_leg]; } }
static int g_dend_leg = -1, g_dend_x, g_dend_y, g_dend_h;   /* where a driven leg really ended (pose), for the next driven leg */
static void leg_done(int tx, int ty, int reached)
{
    if (g_wveh[g_leg] >= 0) { g_dend_leg = g_leg; g_dend_x = g_cx; g_dend_y = g_cy; g_dend_h = g_dh; }
    if (!reached && g_cfg.log >= 1) mlog("missionpath: leg %d%s to %d,%d ends at %d,%d (closed off): the rest dashed", g_leg + 1, g_vstage ? " (variant)" : "", tx, ty, g_cx, g_cy);
    add(tx, ty, !reached, g_vstage == 1 ? 0 : g_leg + 1);
    g_cx = tx; g_cy = ty;
    piece_reset();
    if (g_vstage == 0) {
        g_leg_e0 = g_npts;
        if (g_cfg.mission_path_variation && g_wveh[g_leg] < 0 && !run_dashed(g_leg_s0, g_leg_e0) && pick_via()) {
            g_vstage = 1; g_cx = g_leg_ax; g_cy = g_leg_ay; return;
        }
        leg_finish(); return;
    }
    if (g_vstage == 1) {
        if (!reached || run_dashed(g_leg_e0, g_npts)) { if (g_cfg.log >= 1) mlog("missionpath: leg %d variant %d: via not reached", g_leg + 1, g_vtry + 1); variant_try_next(); return; }
        g_vstage = 2; return;                               /* on from the via point */
    }
    double l0 = run_len(g_leg_ax, g_leg_ay, g_leg_s0, g_leg_e0), l1 = run_len(g_leg_ax, g_leg_ay, g_leg_e0, g_npts);
    int ok = reached && !run_dashed(g_leg_e0, g_npts) && l1 * 100 <= l0 * VAR_MAX_PCT;
    if (g_cfg.log >= 1) mlog("missionpath: leg %d variant %d: %.0f px against the shortest %.0f px (%.0f %%): %s", g_leg + 1, g_vtry + 1, l1, l0, l0 > 0 ? l1 * 100 / l0 : 0, ok ? "taken" : reached ? "too long" : "not found");
    if (!ok) { variant_try_next(); return; }
    int nv = g_npts - g_leg_e0;                              /* the variant replaces the shortest */
    memmove(g_pts + g_leg_s0, g_pts + g_leg_e0, sizeof(MissionPt) * nv);
    g_npts = g_leg_s0 + nv;
    leg_finish();
}
/* ---------------------------------------------------------------- driven legs: the vehicle's own planner
 * 2026-11-20 (user, mission 3 screenshot: the leg driven in GreenTank201 to pad 75 was a straight dashed line over a
 * gorge and rocks - "this path drawing straight over terrain instead of navigating the terrain"): driven legs asked
 * path_find_vehicle only (one ~30000-tile A* per call, chained from its nearest point - a dead end on the near side
 * of a gorge gave no progress and the rest was dashed). Now the HYBRID planner the click-to-drive uses (pathfind.c
 * path_vehicle_plan_*: x, y, heading with forward / backward / on-the-spot motions, the vehicle's oriented
 * footprint at every pose, heuristic = tile distance AROUND blocked tiles, so it heads for the bridge), resumable:
 * D_BUDGET_MS per frame until it is done. A plan that runs out of poses (status 2, the closest pose) is continued
 * from that pose (heading kept) up to D_PIECES times. Only if the planner cannot start or gives no route at all, the
 * old tile planner (path_find_vehicle, chained) tries. A leg is dashed only when:
 *   - the target is not reachable for the vehicle even on the 16 px tile grid (the planner then drives to the
 *     nearest reachable tile and says "found" there: we compare with the real target), or
 *   - neither planner gets on (no progress). Every case is logged with its reason.
 * The poses (every 16-24 px) are thinned for drawing: a point is kept where the route bends more than D_TOL px from
 * the straight line, and at every change of direction (a planned reverse shows as a back-and-forth).
 * The planner's state is shared with vehicles.c (click to drive): used here only while Sarge is on foot (the
 * route is built in the mission's first seconds); in a vehicle the old tile planner is used. */
#define D_BUDGET_MS  6.0
#define D_PIECES     8
#define D_TOL        4.0
#define D_STEP_TANK  16                  /* px per planned motion, as vehicles.c STEP_TANK / STEP_JEEP (Wine: 32 could not get near the mission 3 pad) */
#define D_STEP_JEEP  24
#define D_MAX_POSES  4000
#define D_ARRIVE     48                  /* px: a driven leg ending this close to its target reached it (the planner stops 20 px off a pose it can stand on; a pad by a wall is further) */

static int drive_spins(int type) { return type != 1 && type != 4; }     /* VEH_TYPE - 1: 1 jeep, 4 convoy turn only moving */

/* thinned poses -> route points (from the current end g_cx, g_cy) */
static void add_poses(const VehPose *p, int n)
{
    double ax = g_cx, ay = g_cy;                                 /* the anchor: the last point kept */
    for (int j = 0; j < n; ) {
        int k = j;                                              /* the farthest pose k with all poses between in D_TOL */
        for (int t = j + 1; t < n; t++) {
            if (p[t].dir != p[j].dir) break;                     /* the last pose before a change of direction is kept */
            double sx = p[t].x - ax, sy = p[t].y - ay, l = sqrt(sx * sx + sy * sy);
            int ok = l > 0.5;
            for (int m = j; m < t && ok; m++) {
                double d = fabs((p[m].x - ax) * sy - (p[m].y - ay) * sx) / l;
                if (d > D_TOL) ok = 0;
            }
            if (!ok) break;
            k = t;
        }
        add(p[k].x, p[k].y, 0, 0);
        ax = p[k].x; ay = p[k].y; j = k + 1;
    }
    if (n > 0) { g_cx = p[n - 1].x; g_cy = p[n - 1].y; g_dh = p[n - 1].facing & 255; }
}

static void drive_step(int tx, int ty)
{
    int type = g_wveh[g_leg];
    static VehPose poses[D_MAX_POSES];
    static POINT out[MAX_PTS];
    Guard gd;
    BYTE *s = SARGE_ITEM;
    if (g_dphase < 2 && s && *(BYTE **)(s + 0x2d0)) {             /* Sarge in a vehicle: the planner may be his */
        if (g_cfg.log >= 1) mlog("missionpath: leg %d: Sarge is driving - tile planner instead", g_leg + 1);
        g_dphase = 2;
    }
    if (g_dphase == 0) {
        volatile int ok = 0;
        if (GUARD_TRY(gd, "mission path, vehicle plan (missionpath.c)")) {
            ok = path_vehicle_plan_begin(type, g_cx, g_cy, g_dh, tx, ty, drive_spins(type), drive_spins(type) ? D_STEP_TANK : D_STEP_JEEP);
            guard_leave(&gd);
        }
        if (!ok) {
            if (g_cfg.log >= 1) mlog("missionpath: leg %d (vehicle type %d): planner could not start at %d,%d - tile planner", g_leg + 1, type, g_cx, g_cy);
            g_dphase = 2; return;
        }
        g_dphase = 1;
        return;
    }
    if (g_dphase == 1) {
        volatile int r = -1;
        if (GUARD_TRY(gd, "mission path, vehicle plan (missionpath.c)")) { r = path_vehicle_plan_step(D_BUDGET_MS); guard_leave(&gd); }
        if (r == 0) return;                                      /* next frame */
        g_dpieces++;
        if (r < 0) {
            if (g_cfg.log >= 1) mlog("missionpath: leg %d (vehicle type %d): no vehicle route from %d,%d heading %d%s", g_leg + 1, type, g_cx, g_cy, g_dh,
                                     g_dpieces == 1 ? " - tile planner" : " - rest dashed");
            if (g_dpieces == 1) { g_dphase = 2; return; }
            leg_done(tx, ty, 0); return;
        }
        int bx = g_cx, by = g_cy, bh = g_dh, bn = g_npts;
        int n = path_vehicle_plan_result(poses, D_MAX_POSES), full = n;
        add_poses(poses, n);
        int left = dist2(g_cx, g_cy, tx, ty), moved = dist2(bx, by, g_cx, g_cy);
        if (g_cfg.log >= 1) mlog("missionpath: leg %d (vehicle type %d) piece %d: %s, %d of %d poses kept, ends at %d,%d (%d px from the target)", g_leg + 1, type, g_dpieces,
                                 r == 1 ? "found" : "out of poses (closest)", n, full, g_cx, g_cy, (int)sqrt((double)left));
        if (left <= D_ARRIVE * D_ARRIVE) { leg_done(tx, ty, 1); return; }
        if (r == 1) {                                            /* "found" the nearest tile the vehicle can reach */
            if (g_cfg.log >= 1) mlog("missionpath: leg %d: target %d,%d not reachable for the vehicle (nearest reachable %d,%d) - rest dashed", g_leg + 1, tx, ty, g_cx, g_cy);
            leg_done(tx, ty, 0); return;
        }
        if (moved < MIN_PROGRESS * MIN_PROGRESS) { g_npts = bn; g_cx = bx; g_cy = by; g_dh = bh; }   /* its poses only shuffle on the spot: dropped */
        if (moved < MIN_PROGRESS * MIN_PROGRESS || g_dpieces >= D_PIECES) {
            if (g_cfg.log >= 1) mlog("missionpath: leg %d: vehicle planner %s after %d pieces at %d,%d - rest dashed", g_leg + 1,
                                     moved < MIN_PROGRESS * MIN_PROGRESS ? "makes no progress" : "out of pieces", g_dpieces, g_cx, g_cy);
            leg_done(tx, ty, 0); return;
        }
        g_dphase = 0;                                            /* continue from the end pose */
        return;
    }
    /* the tile planner (path_find_vehicle), chained from its last point */
    volatile int n = 0;
    if (GUARD_TRY(gd, "mission path, vehicle (missionpath.c)")) { n = path_find_vehicle(type, g_cx, g_cy, tx, ty, out, 96); guard_leave(&gd); }
    else n = -1;
    g_leg_calls++;
    int before = dist2(g_cx, g_cy, tx, ty);
    if (n > 0) { for (int i = 0; i < n; i++) add(out[i].x, out[i].y, 0, 0); g_cx = out[n - 1].x; g_cy = out[n - 1].y; }
    int after = dist2(g_cx, g_cy, tx, ty), reached = after <= 32 * 32;
    if (!reached && (n <= 0 || before - after < MIN_PROGRESS * MIN_PROGRESS || g_leg_calls >= LEG_CALLS) && g_cfg.log >= 1)
        mlog("missionpath: leg %d: tile planner %s at %d,%d - rest dashed", g_leg + 1, n <= 0 ? "found no route" : g_leg_calls >= LEG_CALLS ? "out of pieces" : "makes no progress", g_cx, g_cy);
    if (reached || n <= 0 || before - after < MIN_PROGRESS * MIN_PROGRESS || g_leg_calls >= LEG_CALLS) leg_done(tx, ty, reached);
}

static void leg_step(void)
{
    if (g_leg >= g_nwp) { route_done(); return; }
    int tx, ty; piece_target(&tx, &ty);
    static POINT out[MAX_PTS];
    static int wn;
    Guard gd;
    if (g_wveh[g_leg] >= 0) {                                    /* driven: drive_step */
        if (g_dphase == 0 && g_dpieces == 0 && g_leg_calls == 0) {
            g_dh = g_dfacing0[g_leg];
            /* a driven leg after a driven leg (2026-11-21, mission 3 bridge stop): start from that leg's real end pose -
             * the waypoint itself with the vehicle's parked heading gave the planner no start ("no progress") */
            if (g_leg > 0 && g_dend_leg == g_leg - 1) { g_cx = g_dend_x; g_cy = g_dend_y; g_dh = g_dend_h; }
            if (g_cfg.log >= 1) mlog("missionpath: leg %d driven (vehicle type %d) from %d,%d heading %d to %d,%d", g_leg + 1, g_wveh[g_leg], g_cx, g_cy, g_dh, tx, ty);
        }
        drive_step(tx, ty);
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
        /* 2026-11-21 (stutter hunt 2): the search runs in slices of WIDE_BUDGET_MS a frame (pathfind.c
         * path_find_vehicle_begin / _step) - in one call it took 50-105 ms in ONE frame on the user's PC (a via point
         * the jeep cannot reach: "partial after 25233 tiles"). Same search, same result, spread over frames. */
        static int wide_key[4];                                 /* the sliced search (g_wide_on): from, to */
        int key[4] = { g_cx, g_cy, tx, ty };
        if (g_wide_on && memcmp(key, wide_key, sizeof key)) g_wide_on = 0;   /* another leg / mission meanwhile */
        path_vehicle_crush(0);                                  /* Sarge cannot cross fences a vehicle runs over */
        if (GUARD_TRY(gd, "mission path, wide (missionpath.c)")) {
            if (!g_wide_on) { path_find_vehicle_begin(WIDE_TYPE, g_cx, g_cy, tx, ty, 96); g_wide_on = 1; memcpy(wide_key, key, sizeof key); }
            n = path_find_vehicle_step(WIDE_BUDGET_MS, out, 96);
            guard_leave(&gd);
        } else { n = -1; g_wide_on = 0; }
        path_vehicle_crush(1);
        if (g_wide_on && n == -1) return;                         /* not done yet: on next frame */
        g_wide_on = 0;
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
        g_walk_npts = g_npts; g_walk_cx = g_cx; g_walk_cy = g_cy;
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
    if (g_cfg.log >= 1) mlog("missionpath: leg %d walked%s: %s, %d points", g_leg + 1, g_relax ? " (relaxed)" : "", found ? "found" : "closed off", wn);
    if (!found && !g_relax && dist2(g_cx, g_cy, tx, ty) > 32 * 32) {
        /* 2026-11-20 (user: "this is still going directly through objects and walls"): the strict rule (16 sample
         * points per 8 px cell, the strict tile test) closes gaps the game lets Sarge through (yard openings, doors) -
         * the leg was dashed straight through walls. Second search with the game's own Sarge rule. */
        g_npts = g_walk_npts; g_cx = g_walk_cx; g_cy = g_walk_cy;
        g_relax = 1; memset(g_wfree, 0, sizeof g_wfree); memset(g_wclr, 0, sizeof g_wclr); g_wphase = 0;
        return;
    }
    if (g_relax) { g_relax = 0; memset(g_wfree, 0, sizeof g_wfree); memset(g_wclr, 0, sizeof g_wclr); }
    leg_done(tx, ty, found || dist2(g_cx, g_cy, tx, ty) <= 32 * 32);
}

/* [debug] skip_to_mission=N (testing aid, 2026-11-20): every campaign mission with a lower MissionID is won
 * SKIP_AFTER_FRAMES play frames after its start through the game's own "Triumph" cheat handler (table 0x4c5f10
 * entry 1, 0x46b850: gpGame +0x38 == 1 -> FUN_00424870(ecx 0) = mission won). The rig then clicks / presses
 * through the debrief and briefing screens. Mission 3 (Desert 1_3) is not reachable through the menus otherwise. */
#define CHEAT_TRIUMPH      ((VoidFnPtr)0x46b850)
#define SKIP_AFTER_FRAMES  45
static int g_start_target;       /* start_mission=N not first of its scenario: won through to N (below) */
static void debug_skip_mission(BYTE *g)
{
    static int want = -1, key = -1, frames;
    if (want < 0) want = GetPrivateProfileIntA("debug", "skip_to_mission", 0, mod_ini_path());
    int target = want > g_start_target ? want : g_start_target;
    if (target <= 0 || GAME_MISSION(g) >= target) return;
    int k = (GAME_MISSION(g) << 16) ^ GAME_SCENARIO(g);
    if (k != key) { key = k; frames = 0; }
    if (++frames != SKIP_AFTER_FRAMES) return;
    mlog("missionpath: [debug] skip_to_mission=%d: mission %d won (Triumph)", target, GAME_MISSION(g));
    CHEAT_TRIUMPH();
}

/* [debug] start_mission=N (testing aid, 2026-11-21): campaign mission N (MissionID: Desert 1-9, Alpine 10-18,
 * Bayou 19-28) is started DIRECTLY from the campaign select screen (screen state 2, reached by One Player ->
 * player -> NEXT), once per run: no earlier missions played, no debrief chain. Only the mission's own briefing
 * follows. How: the state table's UPDATE entry of state 2 (0x4d3558 + 2*20 + 12, ScrnUpdate 0x462a70) is our
 * start_update (same context as a button callback: the game's menu loop, __fastcall ecx = the screen, result
 * ignored by the caller). On its first call it does what the game's own two clicks would do:
 *   - campaign button callback 0x463e80: g_CampaignIndex 0x4fe71c, current campaign 0x6a5a90, gpGame +0x6c
 *     campaign id; FUN_004190b0 (ecx campaign, edx scenario, push mission index; the "last chosen" record);
 *   - scenario button callback 0x4640a0 from 0x464170 on: gpGame +0x70 scenario id, +0x74 mission id, +0x78 = 1,
 *     the scenario / mission indices 0x4fe720 / 0x4fe724, map name = mission +0x65 + ".AMM" into the CString
 *     gpGame +0x54 (CString::operator= 0x48c869, thiscall), SetPath(0x4b03f0), map file check FUN_004450c0,
 *     then ScrnSetState(-1), FUN_00406290 (clear the screen), FUN_004094b0 (start the mission: briefing, load).
 * Kept: the game's own loading, briefing, mission end and the next mission after a win. Changed: the mission is
 * found by its id in the loaded campaign.cpn tables (no menu unlock needed); its status (and its scenario's /
 * campaign's) is set to 5 (available) when it was 0, as the game's own advance (FUN_00418f90) does.
 * A mission that is NOT the first of its scenario (e.g. 3 = Desert 1_3, 2nd/3rd part on the same map) cannot be
 * started cold: it needs what the previous part leaves (the game's callback refuses without the player's save
 * "<player>_<id-1>.asf"; started anyway, Wine 2026-11-21: crash at 0x43ef55, [0x4f7944 +0x20] NULL). So the
 * scenario's FIRST mission is started and the rest is won through with skip_to_mission's Triumph (g_start_target):
 * the rig clicks one debrief + briefing per part (at most 2). Table layout (verified in 0x463e80 / 0x4640a0 / FUN_00419140): campaign +4 id (u16),
 * +0xd0 scenario count, scenario i at +0xd4 + i*0x3f0 (+0 status, +4 id, +0xcc mission count), mission j at
 * scenario +0xd0 + j*0xc8 (+0 status, +8 id, +0x65 map name). */
#define CAMPAIGN_COUNT     (*(int *)0x4fe714)
#define CAMPAIGNS          ((BYTE **)0x4fe4c0)
#define CAMPAIGN_INDEX     (*(int *)0x4fe71c)
#define SCENARIO_INDEX     (*(int *)0x4fe720)
#define MISSION_INDEX      (*(int *)0x4fe724)
#define CUR_CAMPAIGN       (*(BYTE **)0x6a5a90)          /* the campaign the scenario screen shows */
#define GAME_CAMPAIGN(g)   (*(int *)((g) + 0x6c))
#define GAME_LOAD_SAVE(g)  (*(int *)((g) + 0x78))        /* 1: load the carried-over army save (mission index > 0) */
#define GAME_MAP_NAME(g)   ((void *)((g) + 0x54))        /* CString */
#define CAMP_ID(c)         (*(unsigned short *)((c) + 4))
#define CAMP_NSCEN(c)      (*(int *)((c) + 0xd0))
#define CAMP_SCEN(c, i)    ((c) + 0xd4 + (i) * 0x3f0)
#define SCEN_ID(s)         (*(unsigned short *)((s) + 4))
#define SCEN_NMIS(s)       (*(int *)((s) + 0xcc))
#define SCEN_MIS(s, j)     ((s) + 0xd0 + (j) * 0xc8)
#define MIS_ID(m)          (*(unsigned short *)((m) + 8))
#define MIS_MAP(m)         ((const char *)((m) + 0x65))
#define STATE_TABLE_UPDATE_2 (0x4d3558 + 2 * 20 + 12)    /* SCRNstateTable[2].update (campaign select) */
#define SCRN_UPDATE_ORIG   0x462a70                      /* ScrnUpdate */
#define MAPS_PATH          ((const void *)0x4b03f0)      /* SetPath argument used before the map check */
typedef LONG (__fastcall *ScrnUpdFn)(BYTE *screen);
typedef void (__fastcall *SetChoiceFn)(int camp, int scen, int mission);   /* + 1 stack arg (callee pops) */
typedef void (__fastcall *CStrAssignFn)(void *str, int edx, const char *s); /* thiscall, 1 stack arg */
typedef BOOL (__fastcall *MapExistsFn)(const char *name);
typedef void (__fastcall *PathFn)(const void *path);
typedef void (__fastcall *StateFn)(int state);
#define SET_CHOICE    ((void *)0x4190b0)
#define CSTR_ASSIGN   ((CStrAssignFn)0x48c869)
#define MAP_EXISTS    ((MapExistsFn)0x4450c0)
#define SET_PATH_FN   ((PathFn)0x420e40)
#define SCRN_STATE    ((StateFn)0x462680)
#define CLEAR_SCREEN  ((VoidFnPtr)0x406290)
#define START_MISSION ((VoidFnPtr)0x4094b0)             /* uses globals only (ecx = pMainFrame at the call sites) */

static int g_start_done;

static void __attribute__((noinline)) call_set_choice(int c, int s, int m)
{
    /* FUN_004190b0: ecx campaign, edx scenario, [esp+4] mission index, ret 4 */
    __asm__ volatile ("push %2\n\tcall *%3" : "+c"(c), "+d"(s) : "r"(m), "r"(SET_CHOICE) : "eax", "memory");
}

static int start_mission_now(int want)
{
    BYTE *g = GAME;
    if (!g || GAME_TYPE(g) != 0 || CAMPAIGN_COUNT <= 0 || CAMPAIGN_COUNT > 8) return 0;
    char tab[256]; int tn = 0;                                   /* the mission ids per scenario, for the log */
    tab[0] = 0;
    for (int c = 0; c < CAMPAIGN_COUNT && tn < 200; c++) {
        BYTE *camp = CAMPAIGNS[c];
        if (!camp || CAMP_NSCEN(camp) <= 0 || CAMP_NSCEN(camp) > 16) continue;
        for (int s = 0; s < CAMP_NSCEN(camp) && tn < 200; s++) {
            BYTE *scen = CAMP_SCEN(camp, s);
            if (SCEN_NMIS(scen) <= 0 || SCEN_NMIS(scen) > 4) continue;
            for (int m = 0; m < SCEN_NMIS(scen); m++)
                tn += snprintf(tab + tn, sizeof tab - tn, "%s%d", m ? "," : (tn ? " | " : ""), MIS_ID(SCEN_MIS(scen, m)));
        }
    }
    mlog("missionpath: [debug] start_mission: campaign missions by scenario: %s", tab);
    for (int c = 0; c < CAMPAIGN_COUNT; c++) {
        BYTE *camp = CAMPAIGNS[c];
        if (!camp || CAMP_NSCEN(camp) <= 0 || CAMP_NSCEN(camp) > 16) continue;
        for (int s = 0; s < CAMP_NSCEN(camp); s++) {
            BYTE *scen = CAMP_SCEN(camp, s);
            if (SCEN_NMIS(scen) <= 0 || SCEN_NMIS(scen) > 4) continue;
            for (int m = 0; m < SCEN_NMIS(scen); m++) {
                BYTE *mis = SCEN_MIS(scen, m);
                if (MIS_ID(mis) != want) continue;
                if (m > 0) {                                     /* later part of a scenario: from its first part */
                    g_start_target = want;
                    mlog("missionpath: [debug] start_mission=%d is part %d of its scenario: starting mission %d, "
                         "then %d win(s) (Triumph, debrief clicks needed)", want, m + 1, MIS_ID(SCEN_MIS(scen, 0)), m);
                    m = 0; mis = SCEN_MIS(scen, 0);
                }
                char map[128];
                snprintf(map, sizeof map, "%s.AMM", MIS_MAP(mis));
                CAMPAIGN_INDEX = c; CUR_CAMPAIGN = camp; GAME_CAMPAIGN(g) = CAMP_ID(camp);
                SCENARIO_INDEX = s; MISSION_INDEX = m;
                GAME_SCENARIO(g) = SCEN_ID(scen); GAME_MISSION(g) = MIS_ID(mis); GAME_LOAD_SAVE(g) = 1;
                if (*(int *)camp == 0) *(int *)camp = 5;
                if (*(int *)scen == 0) *(int *)scen = 5;
                if (*(int *)mis == 0) *(int *)mis = 5;
                call_set_choice(c, s, m);
                CSTR_ASSIGN(GAME_MAP_NAME(g), 0, map);
                SET_PATH_FN(MAPS_PATH);
                if (!MAP_EXISTS(*(const char **)GAME_MAP_NAME(g))) {
                    mlog("missionpath: [debug] start_mission=%d: map %s not found - normal menus", want, map);
                    return 0;
                }
                mlog("missionpath: [debug] start_mission=%d: campaign %d scenario %d mission %d (%s) started directly",
                     want, c, s, m, map);
                SCRN_STATE(-1);
                CLEAR_SCREEN();
                START_MISSION();
                return 1;
            }
        }
    }
    mlog("missionpath: [debug] start_mission=%d: no such mission in the campaign tables - normal menus", want);
    return 0;
}

static LONG __fastcall start_update(BYTE *screen)
{
    if (!g_start_done) {
        g_start_done = 1;                                       /* once per run: later visits are the normal screen */
        int want = GetPrivateProfileIntA("debug", "start_mission", 0, mod_ini_path());
        if (want > 0 && start_mission_now(want)) return 1;
    }
    return ((ScrnUpdFn)SCRN_UPDATE_ORIG)(screen);
}

void missionpath_install(void)
{
    if (GetPrivateProfileIntA("debug", "start_mission", 0, mod_ini_path()) <= 0) return;
    DWORD cur = *(DWORD *)STATE_TABLE_UPDATE_2, mine = (DWORD)(uintptr_t)start_update;
    if (*(DWORD *)(STATE_TABLE_UPDATE_2 - 12) != 2 || cur != SCRN_UPDATE_ORIG) { mlog("missionpath: state table entry 2 unexpected - start_mission off"); return; }
    mlog("patch %-55s %s", "missionpath: [debug] start_mission (state 2 update)", write_mem(STATE_TABLE_UPDATE_2, &mine, 4) ? "applied" : "FAILED");
}

/* ---------------------------------------------------------------- restart or fresh entry (2026-11-21)
 * User: "keep the same path on restarts. but if coming in from the main menu, change it."
 *   - missionpath_screen_note (ddraw.c dd_present, game thread, every frame incl. menus) watches the game's screen
 *     state (SCREEN* 0x6a5944 +4; SCRN_*_STATE from the exe's names: 0 title, 1 main menu, 2 campaign select,
 *     3 callsign, 4 mission select) and the game state (0x423d00, 5 = play). A frame outside play with screen state
 *     0..4 sets g_menu_seen; leaving play (game state != 5) marks the route stale (g_left_play: built anew at the next
 *     play frame - before, only a gameplay-view frame outside play did that).
 *   - building a route: FRESH when g_menu_seen is set or the mission id differs from the last built one (a won
 *     mission's next part, skip_to_mission's Triumph, [debug] start_mission passes state 2 too): a NEW seed
 *     (QueryPerformanceCounter ^ GetTickCount, or [debug] mission_path_seed=N when not 0). Otherwise (Play Again:
 *     the same mission reloaded from the death / mission-over screen, no menu state between) a RESTART: the same
 *     seed, and the finished route of that mission (kept in g_keep) is reused as it was when Sarge starts within
 *     KEEP_START_PX of where he started then - identical points, no planning; else it is planned again with the
 *     same seed. In memory only (a new game session = fresh). */
#define GAME_STATE_FN   ((int (__fastcall *)(void *, int))0x423d00)   /* thiscall: the game's state */
#define GAME_STATE_PLAY 5
#define SCREEN_FULL     (*(BYTE **)0x6a5944)                          /* SCREEN*: +4 its state */
#define KEEP_START_PX   24
static int g_menu_seen = 1, g_left_play = 1, g_last_mission = -1;
static struct { int valid, mission, sx, sy, npts, nwp; unsigned seed; MissionPt pts[MAX_PTS]; int wx[MAX_WP], wy[MAX_WP], wveh[MAX_WP], first[MAX_WP + 1];
                const Way *mem[MAX_MEM]; BYTE mem_wp[MAX_MEM]; int nmem; } g_keep;

void missionpath_screen_note(void)
{
    static int last_scr = -99, last_gs = -99;
    BYTE *sc = SCREEN_FULL, *g = GAME;
    int scr = sc && !IsBadReadPtr(sc, 8) ? *(int *)(sc + 4) : -1;
    int gs = g ? GAME_STATE_FN(g, 0) : -1;
    if (scr != last_scr || gs != last_gs) {
        if (g_cfg.log >= 1) mlog("missionpath: screen state %d -> %d, game state %d -> %d", last_scr, scr, last_gs, gs);
        last_scr = scr; last_gs = gs;
    }
    if (gs != GAME_STATE_PLAY) {
        g_left_play = 1;
        if (scr >= 0 && scr <= 4 && !g_menu_seen) { g_menu_seen = 1; if (g_cfg.log >= 1) mlog("missionpath: menu screen %d seen - the next mission gets a new route", scr); }
    }
}

static void route_done(void)
{
    g_phase = 2; g_cur = 0;
    if (g_cfg.log >= 1) {
        mlog("missionpath: done, %d points, %d waypoints (seed %08x)", g_npts, g_nwp, g_seed);
        for (int k = 0; k < g_nwp; k++) {
            char b[600]; int o = 0;
            for (int i = g_leg_first[k]; i <= g_leg_first[k + 1] && i < g_npts && o < 560; i++) o += wsprintfA(b + o, " %d,%d%s", g_pts[i].x, g_pts[i].y, g_pts[i].dashed ? "d" : "");
            mlog("missionpath: leg %d (%.0f px):%s", k + 1, run_len(g_pts[g_leg_first[k]].x, g_pts[g_leg_first[k]].y, g_leg_first[k] + 1, g_leg_first[k + 1] + 1), b);
        }
    }
    BYTE *g = GAME;
    g_keep.valid = 1; g_keep.mission = g ? GAME_MISSION(g) : -1; g_keep.sx = g_pts[0].x; g_keep.sy = g_pts[0].y; g_keep.seed = g_seed;
    g_keep.npts = g_npts; memcpy(g_keep.pts, g_pts, sizeof(MissionPt) * g_npts);
    g_keep.nwp = g_nwp; memcpy(g_keep.wx, g_wx, sizeof g_wx); memcpy(g_keep.wy, g_wy, sizeof g_wy); memcpy(g_keep.wveh, g_wveh, sizeof g_wveh);
    memcpy(g_keep.first, g_leg_first, sizeof g_leg_first);
    g_keep.nmem = g_nmem; memcpy(g_keep.mem, g_mem, sizeof g_mem); memcpy(g_keep.mem_wp, g_mem_wp, sizeof g_mem_wp);
    g_build_serial++; escort_setup();
}
static int route_reuse(int m, int sx, int sy)
{
    if (!g_keep.valid || g_keep.mission != m || g_keep.seed != g_seed || dist2(sx, sy, g_keep.sx, g_keep.sy) > KEEP_START_PX * KEEP_START_PX) return 0;
    g_npts = g_keep.npts; memcpy(g_pts, g_keep.pts, sizeof(MissionPt) * g_npts);
    g_nwp = g_keep.nwp; memcpy(g_wx, g_keep.wx, sizeof g_wx); memcpy(g_wy, g_keep.wy, sizeof g_wy); memcpy(g_wveh, g_keep.wveh, sizeof g_wveh);
    memcpy(g_leg_first, g_keep.first, sizeof g_leg_first);
    g_nmem = g_keep.nmem; memcpy(g_mem, g_keep.mem, sizeof g_mem); memcpy(g_mem_wp, g_keep.mem_wp, sizeof g_mem_wp); memset(g_mem_done, 0, sizeof g_mem_done);
    g_phase = 2; g_cur = 0;
    if (g_cfg.log >= 1) mlog("missionpath: restart of mission %d: the same route (seed %08x, %d points, %d waypoints)", m, g_seed, g_npts, g_nwp);
    g_build_serial++; escort_setup();
    return 1;
}

void missionpath_frame(void)
{
    BYTE *g = GAME, *s = SARGE_ITEM;
    if (g && s && game_in_play() && GAME_TYPE(g) == 0) debug_skip_mission(g);
    if (!g || !s || !game_in_play() || GAME_TYPE(g) != 0) {    /* between missions (Play Again too): built anew */
        if (!game_in_play()) { g_key = -1; g_phase = 0; g_npts = 0; g_wide_on = 0; }
        return;
    }
    if (!g_cfg.mission_path) return;
    int key = (GAME_MISSION(g) << 16) ^ GAME_SCENARIO(g);
    if (key != g_key || g_left_play || (g_phase == 0 && s != g_sarge)) {   /* first play frame of a mission: Sarge's start */
        int m = GAME_MISSION(g);
        if (g_menu_seen || m != g_last_mission) {
            int fixed = GetPrivateProfileIntA("debug", "mission_path_seed", 0, mod_ini_path());
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            unsigned old = g_seed;
            g_seed = fixed ? (unsigned)fixed : (unsigned)q.LowPart ^ (GetTickCount() * 2654435761u) ^ (unsigned)(m * 40503);
            if (!g_seed) g_seed = 1;
            if (g_cfg.log >= 1) mlog("missionpath: mission %d entered %s: new seed %08x (was %08x)", m, g_menu_seen ? "from the menus" : "as the next mission", g_seed, old);
            g_menu_seen = 0; g_keep.valid = 0;
        } else if (key != g_key || g_left_play) {
            if (g_cfg.log >= 1) mlog("missionpath: mission %d restarted (no menu between): seed %08x kept", m, g_seed);
        }
        g_last_mission = m; g_left_play = 0;
        g_key = key; g_sarge = s; g_phase = 0; g_frames = 0; g_npts = 0; g_nwp = 0; g_leg = 0; g_leg_calls = 0; g_dphase = 0; g_dpieces = 0;
        g_nmem = 0; g_cur = 0; g_vstage = 0; g_vtry = 0; g_relax = 0; g_wide_on = 0; g_esc = NULL; g_esc_wp = -1; g_lane_n = 0; g_esc_frozen = 0; g_esc_on_t = 0;
        g_cx = *(short *)(s + 4); g_cy = *(short *)(s + 6);
        memset(g_wfree, 0, sizeof g_wfree); memset(g_wclr, 0, sizeof g_wclr); g_wphase = 0;   /* the cell answers belong to this map */
        add(g_cx, g_cy, 0, 0);
        if (g_cfg.log >= 1) mlog("missionpath: mission %d, Sarge starts at %d,%d", m, g_cx, g_cy);
        return;
    }
    if (g_phase == 0) {
        if (++g_frames < RESOLVE_FRAMES) return;
        if (route_reuse(GAME_MISSION(g), g_pts[0].x, g_pts[0].y)) return;
        resolve(GAME_MISSION(g), g_pts[0].x, g_pts[0].y);
        g_phase = 1; g_leg = 0; g_dend_leg = -1;
        if (g_nwp > 0) leg_begin();
        return;
    }
    if (g_phase == 1) leg_step();
    if (g_phase == 2) { progress(); escort_frame(); }
}

/* savestate.c: the waypoint the shown leg leads to (re-checked every frame, only moves on) */
void missionpath_savestate(void) { SAVESTATE(g_cur); }
