/* ============================================================================================
 * pathfind.c - routes for "walk to the clicked spot" (controls.c)
 *
 * The original Sarge has no path finding for the player (the click-to-walk is ours). Army Men II
 * walks a clicked spot with real path finding (FUN_0042b290 -> FUN_0043a0a0), so we do the same:
 * A* over CELL x CELL world-pixel cells, 8 neighbours, using Sarge's own walking rule
 * (controls_blocked_at = ground tile untrafficable, or fences / walls / buildings / vehicles at
 * the point - the same test the game uses for Sarge). A cell is free when its centre and four
 * points CLEAR px around it are free (Sarge's body). Diagonal steps need both side cells free
 * (no corner cutting through a fence post). The cell grid covers the whole map (4096 px = 512
 * cells); cells are only tested when the search reaches them, at most MAX_EXPAND per search.
 * The route is then straightened (string pulling with a sampled line test) into waypoints.
 * If the target itself is blocked, the nearest free cell to it is used; if no route is found
 * within the budget, the caller falls back to walking straight with the old obstacle steering.
 * ============================================================================================ */
#include "common.h"
#include <math.h>
#include <stdlib.h>

#define CELL        8
#define GRID        512                 /* cells per side: 4096 world px */
#define CLEAR       4
#define MAX_EXPAND  40000               /* ~50 ms worst case, once per click */

enum { UNKNOWN = 0, FREE = 1, BLOCKED = 2 };
static BYTE g_state[GRID * GRID];       /* per search: UNKNOWN / FREE / BLOCKED */
static BYTE g_closed[GRID * GRID];
static BYTE g_seen[GRID * GRID];        /* g_cost / g_from valid in this search */
static float g_cost[GRID * GRID];
static int g_from[GRID * GRID];
static int g_touched[MAX_EXPAND * 9 + 256]; static int g_ntouched;

static int g_start = -1;               /* this search's start cell: next to it only the centre is tested */
/* A cell is free when none of a 4x4 grid of points inside it (every 2 px) is blocked: fences are 1-2 px
 * thin and slipped between the old centre + 4 sample points, so routes ran straight through them.
 * Next to Sarge's own cell only its centre is tested (he may stand right against a fence). */
static int cell_free(int c)
{
    if (g_state[c] == UNKNOWN) {
        int x0 = (c % GRID) * CELL, y0 = (c / GRID) * CELL;
        int near_start = g_start >= 0 && abs(c % GRID - g_start % GRID) <= 1 && abs(c / GRID - g_start / GRID) <= 1;
        int blocked = controls_blocked_at(x0 + CELL / 2, y0 + CELL / 2);
        for (int sy = 1; sy < CELL && !blocked && !near_start; sy += 2)
            for (int sx = 1; sx < CELL && !blocked; sx += 2)
                blocked = controls_blocked_at(x0 + sx, y0 + sy);
        g_state[c] = blocked ? BLOCKED : FREE;
        if (g_ntouched < (int)(sizeof g_touched / sizeof *g_touched)) g_touched[g_ntouched++] = c;
    }
    return g_state[c] == FREE;
}

/* binary heap of cells by f = cost + estimate */
static int g_heap[MAX_EXPAND * 8 + 16]; static float g_heapf[MAX_EXPAND * 8 + 16]; static int g_nheap;
static void heap_push(int c, float f)
{
    if (g_nheap >= (int)(sizeof g_heap / sizeof *g_heap)) return;
    int i = g_nheap++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (g_heapf[p] <= f) break;
        g_heap[i] = g_heap[p]; g_heapf[i] = g_heapf[p]; i = p;
    }
    g_heap[i] = c; g_heapf[i] = f;
}
static int heap_pop(void)
{
    int top = g_heap[0];
    int c = g_heap[--g_nheap]; float f = g_heapf[g_nheap];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i; float mf = f;
        if (l < g_nheap && g_heapf[l] < mf) { m = l; mf = g_heapf[l]; }
        if (r < g_nheap && g_heapf[r] < mf) { m = r; mf = g_heapf[r]; }
        if (m == i) break;
        g_heap[i] = g_heap[m]; g_heapf[i] = g_heapf[m]; i = m;
    }
    g_heap[i] = c; g_heapf[i] = f;
    return top;
}

static float estimate(int a, int b)            /* octile distance in cells */
{
    int dx = abs(a % GRID - b % GRID), dy = abs(a / GRID - b / GRID);
    return (float)(dx > dy ? dx + 0.4142 * dy : dy + 0.4142 * dx);
}

static void reset(void)
{
    for (int i = 0; i < g_ntouched; i++) { int c = g_touched[i]; g_state[c] = UNKNOWN; g_closed[c] = 0; g_seen[c] = 0; }
    g_ntouched = 0; g_nheap = 0;
}

/* straight walkable line between two world points (sampled every 3 px, with Sarge's clearance) */
static int line_free(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int n = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) + 1;          /* every pixel: fences are 1-2 px thin */
    double len = sqrt((double)dx * dx + (double)dy * dy);
    int ox = len > 0 ? (int)(-dy * CLEAR / len) : 0, oy = len > 0 ? (int)(dx * CLEAR / len) : 0;   /* sideways */
    for (int i = 1; i <= n; i++) {
        int x = x0 + dx * i / n, y = y0 + dy * i / n;
        if (controls_blocked_at(x, y) || controls_blocked_at(x + ox, y + oy) || controls_blocked_at(x - ox, y - oy)) return 0;
    }
    return 1;
}

static int in_map(int x, int y) { return x >= 0 && y >= 0 && x < GRID * CELL && y < GRID * CELL; }

int path_find(int sx, int sy, int tx, int ty, POINT *out, int max)
{
    if (!in_map(sx, sy) || !in_map(tx, ty) || max < 1) return 0;
    reset();
    int start = (sy / CELL) * GRID + sx / CELL, goal = (ty / CELL) * GRID + tx / CELL;
    g_start = start;
    if (!cell_free(goal)) {                     /* clicked on an obstacle: nearest free cell to it */
        int best = -1;
        for (int r = 1; r <= 6 && best < 0; r++)
            for (int dy = -r; dy <= r && best < 0; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    int x = goal % GRID + dx, y = goal / GRID + dy;
                    if (x < 0 || y < 0 || x >= GRID || y >= GRID) continue;
                    if (cell_free(y * GRID + x)) { best = y * GRID + x; break; }
                }
        if (best < 0) { if (g_cfg.log >= 2) mlog("path: %d,%d -> %d,%d: the spot and all around it are blocked", sx, sy, tx, ty); return 0; }
        goal = best;
        tx = (goal % GRID) * CELL + CELL / 2; ty = (goal / GRID) * CELL + CELL / 2;
    }
    g_state[start] = FREE;                      /* Sarge stands here, whatever the test says */
    if (g_ntouched < (int)(sizeof g_touched / sizeof *g_touched)) g_touched[g_ntouched++] = start;
    if (line_free(sx, sy, tx, ty)) {                                          /* nothing in the way */
        if (g_cfg.log >= 2) mlog("path: %d,%d -> %d,%d: straight", sx, sy, tx, ty);
        out[0] = (POINT){ tx, ty }; return 1;
    }

    g_cost[start] = 0; g_from[start] = -1; g_seen[start] = 1;
    heap_push(start, estimate(start, goal));
    int expanded = 0, found = 0, nearest = start; float nearest_h = estimate(start, goal);
    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    LARGE_INTEGER q0, qf, qn; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf);
    while (g_nheap > 0 && expanded < MAX_EXPAND) {
        if ((expanded & 255) == 0 && expanded) {      /* at most ~40 ms on one click: then the nearest point so far */
            QueryPerformanceCounter(&qn);
            if ((qn.QuadPart - q0.QuadPart) * 1000 / qf.QuadPart > 40) break;
        }
        int c = heap_pop();
        if (g_closed[c]) continue;
        g_closed[c] = 1; expanded++;
        if (c == goal) { found = 1; break; }
        float h = estimate(c, goal);
        if (h < nearest_h) { nearest_h = h; nearest = c; }
        int cx = c % GRID, cy = c / GRID;
        for (int k = 0; k < 8; k++) {
            int nx = cx + DX[k], ny = cy + DY[k];
            if (nx < 0 || ny < 0 || nx >= GRID || ny >= GRID) continue;
            int n = ny * GRID + nx;
            if (g_closed[n] || !cell_free(n)) continue;
            if (k >= 4 && (!cell_free(cy * GRID + nx) || !cell_free(ny * GRID + cx))) continue;   /* no corner cutting */
            float g = g_cost[c] + (k >= 4 ? 1.4142f : 1.0f);
            /* first visit or a cheaper way: (re)push; stale heap entries are skipped by g_closed */
            if (g_seen[n] && g_cost[n] <= g) continue;
            g_seen[n] = 1;
            g_cost[n] = g; g_from[n] = c;
            heap_push(n, g + estimate(n, goal));
        }
    }
    int end = found ? goal : nearest;
    if (end == start) { if (g_cfg.log >= 2) mlog("path: %d,%d -> %d,%d: boxed in (%d cells searched)", sx, sy, tx, ty, expanded); return 0; }
    /* cells from the end back to the start */
    static int chain[MAX_EXPAND + 2]; int nc = 0;
    for (int c = end; c != -1 && nc < (int)(sizeof chain / sizeof *chain); c = g_from[c]) chain[nc++] = c;
    if (nc < 2 || chain[nc - 1] != start) { if (g_cfg.log >= 2) mlog("path: route too long (%d cells)", nc); return 0; }
    /* string pulling: from the current point, jump to the farthest chain point in straight sight */
    int n = 0, cx = sx, cy = sy, i = nc - 1;
    while (i > 0 && n < max) {
        int j = 0;
        for (j = 0; j < i; j++) {               /* farthest first (chain[0] = the end) */
            int px = (chain[j] % GRID) * CELL + CELL / 2, py = (chain[j] / GRID) * CELL + CELL / 2;
            if (j == 0 && found) { px = tx; py = ty; }
            if (line_free(cx, cy, px, py)) break;
        }
        if (j == i) j = i - 1;                  /* nothing farther in sight: the next cell */
        int px = (chain[j] % GRID) * CELL + CELL / 2, py = (chain[j] / GRID) * CELL + CELL / 2;
        if (j == 0 && found) { px = tx; py = ty; }
        out[n++] = (POINT){ px, py };
        cx = px; cy = py; i = j;
    }
    if (g_cfg.log >= 2) mlog("path: %d,%d -> %d,%d: %s after %d cells, %d waypoints", sx, sy, tx, ty, found ? "found" : "partial", expanded, n);
    return n;
}

/* ---------------------------------------------------------------- can Sarge get there at all?
 * For the walk pointer (controls.c: a "no walk" pointer over spots he cannot reach, and a click there
 * does not walk). Two flood fills at once, one from Sarge and one from the target (both with the
 * route's walking rule and no corner cutting): they meet = reachable; either runs out of cells first =
 * the target is closed off from Sarge (across water, inside a fence) - also when Sarge's side is the
 * whole open map, because the target's small enclosed side runs out first.
 * Returns 1 reachable, 0 unreachable, -1 not decided within budget_ms (treat as reachable). */
#define SIDE_A 1
#define SIDE_B 2
int path_regions_reachable(int sx, int sy, int tx, int ty);
int path_reachable(int sx, int sy, int tx, int ty, int budget_ms)
{
    if (!in_map(sx, sy) || !in_map(tx, ty)) return 0;
    /* the whole-map areas, when built: "same area" is trusted; "different" is checked with the floods below
     * (the areas can be a sweep old - a vehicle moved away), which decide unless they run out of time */
    int known = path_regions_reachable(sx, sy, tx, ty);
    if (known == 1) return 1;
    reset();
    int start = (sy / CELL) * GRID + sx / CELL, goal = (ty / CELL) * GRID + tx / CELL;
    g_start = start;
    if (!cell_free(goal)) {                       /* on an obstacle: the walk would end next to it */
        int best = -1;
        for (int r = 1; r <= 6 && best < 0; r++)
            for (int dy = -r; dy <= r && best < 0; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    int x = goal % GRID + dx, y = goal / GRID + dy;
                    if (x < 0 || y < 0 || x >= GRID || y >= GRID) continue;
                    if (cell_free(y * GRID + x)) { best = y * GRID + x; break; }
                }
        if (best < 0) return 0;
        goal = best;
    }
    g_state[start] = FREE;
    if (g_ntouched < (int)(sizeof g_touched / sizeof *g_touched)) g_touched[g_ntouched++] = start;
    if (start == goal) return 1;
    /* queues: side A in g_heap, side B in g_from (both large enough for MAX_EXPAND cells) */
    int *qa = g_heap, *qb = g_from, ha = 0, ta = 0, hb = 0, tb = 0;
    const int qmax = MAX_EXPAND;
    g_closed[start] = SIDE_A; qa[ta++] = start;
    g_closed[goal] = SIDE_B; qb[tb++] = goal;
    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    LARGE_INTEGER q0, qf, qn; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf);
    int steps = 0, result = -1;
    while (result < 0) {
        if (ha == ta || hb == tb) { result = 0; break; }        /* one side is closed off */
        if (ta >= qmax || tb >= qmax) break;                      /* too big to decide */
        if ((++steps & 63) == 0) {
            QueryPerformanceCounter(&qn);
            if ((qn.QuadPart - q0.QuadPart) * 1000 / qf.QuadPart >= budget_ms) break;
        }
        int side = (ta - ha) <= (tb - hb) ? SIDE_A : SIDE_B;    /* grow the smaller frontier */
        int c = side == SIDE_A ? qa[ha++] : qb[hb++];
        int cx = c % GRID, cy = c / GRID;
        for (int k = 0; k < 8; k++) {
            int nx = cx + DX[k], ny = cy + DY[k];
            if (nx < 0 || ny < 0 || nx >= GRID || ny >= GRID) continue;
            int n = ny * GRID + nx;
            if (g_closed[n] == side || !cell_free(n)) continue;
            if (k >= 4 && (!cell_free(cy * GRID + nx) || !cell_free(ny * GRID + cx))) continue;
            if (g_closed[n]) { result = 1; break; }               /* the other side's cell: they meet */
            g_closed[n] = (BYTE)side;
            if (g_ntouched < (int)(sizeof g_touched / sizeof *g_touched)) g_touched[g_ntouched++] = n;
            if (side == SIDE_A) { if (ta < qmax) qa[ta++] = n; } else { if (tb < qmax) qb[tb++] = n; }
        }
    }
    if (result < 0 && known == 0) result = 0;
    if (g_cfg.log >= 2) mlog("path: reach %d,%d -> %d,%d: %s (%d / %d cells)", sx, sy, tx, ty,
                             result == 1 ? "yes" : result == 0 ? "NO" : "undecided", ta, tb);
    return result;
}

/* ---------------------------------------------------------------- walkable regions of the whole map
 * (user 2026-11-17: "I do like the unreachable check idea" - for far spots the two-sided flood above runs out
 * of its 6 ms and answers "undecided"). A background sweep classifies every 8 px cell with the walking rule
 * (the 4x4 point grid, without the next-to-Sarge exception) in slices of REGION_SLICE_MS per gameplay frame,
 * then labels the connected free areas (8 neighbours, no corner cutting) - also in slices - and publishes the
 * labels; then it starts over, so opened gates / destroyed walls show up after one sweep (a few seconds).
 * path_reachable asks the labels first: same label = reachable, different = not; not built yet = the floods.
 * A new map (other size or tile layer) throws the labels away. */
#define REGION_SLICE_MS 0.5                 /* per gameplay frame (was 1.5: the whole sweep ran every frame forever) */
#define REGION_REST_MS 3000                 /* between two whole sweeps */
static DWORD g_rrest_until;
static BYTE  g_rstate[GRID * GRID];          /* sweep: 0 unknown, 1 free, 2 blocked */
static unsigned short g_rwork[GRID * GRID];  /* labels being built */
static unsigned short g_rlabel[GRID * GRID]; /* published labels (0 = blocked) */
static int g_rready, g_rphase, g_rnext, g_rqh, g_rqt, g_rcount;
static int *g_rqueue;                         /* BFS queue (GRID*GRID ints, allocated once) */
static uintptr_t g_rmap;                      /* which map the labels are for */
#define MAP_TILES_PTR (*(uintptr_t *)0x4e3554)
#define MAP_TILES_W   (*(unsigned short *)0x4e368c)

static int region_cell_blocked(int c)
{
    int x0 = (c % GRID) * CELL, y0 = (c / GRID) * CELL;
    if (controls_blocked_at(x0 + CELL / 2, y0 + CELL / 2)) return 1;
    for (int sy = 1; sy < CELL; sy += 2)
        for (int sx = 1; sx < CELL; sx += 2)
            if (controls_blocked_at(x0 + sx, y0 + sy)) return 1;
    return 0;
}

void path_regions_reset(void) { g_rready = 0; g_rphase = 0; g_rnext = 0; }

/* one slice of the sweep; call once per gameplay frame */
static void path_regions_step_body(void)
{
    uintptr_t map = MAP_TILES_PTR ^ ((uintptr_t)MAP_TILES_W << 20);
    if (map != g_rmap) { g_rmap = map; path_regions_reset(); }
    if (g_rphase == 0 && g_rnext == 0 && g_rready && GetTickCount() < g_rrest_until) return;   /* resting between sweeps */
    if (!g_rqueue && !(g_rqueue = (int *)malloc(sizeof(int) * GRID * GRID))) return;
    LARGE_INTEGER q0, qn, qf; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf);
    #define OUT_OF_TIME() (QueryPerformanceCounter(&qn), (double)(qn.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart > REGION_SLICE_MS)
    if (g_rphase == 0) {                                       /* classify cells */
        while (g_rnext < GRID * GRID) {
            for (int n = 0; n < 64 && g_rnext < GRID * GRID; n++, g_rnext++) g_rstate[g_rnext] = region_cell_blocked(g_rnext) ? 2 : 1;
            if (OUT_OF_TIME()) return;
        }
        memset(g_rwork, 0, sizeof g_rwork);
        g_rphase = 1; g_rnext = 0; g_rcount = 0; g_rqh = g_rqt = 0;
    }
    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    while (g_rphase == 1) {                                    /* label: flood each unlabelled free cell */
        if (g_rqh == g_rqt) {
            while (g_rnext < GRID * GRID && (g_rstate[g_rnext] != 1 || g_rwork[g_rnext])) g_rnext++;
            if (g_rnext >= GRID * GRID) {                      /* done: publish */
                memcpy(g_rlabel, g_rwork, sizeof g_rlabel);
                if (!g_rready && g_cfg.log >= 1) mlog("path: walkable regions ready (%d areas)", g_rcount);
                g_rready = 1; g_rphase = 0; g_rnext = 0;
                g_rrest_until = GetTickCount() + REGION_REST_MS;
                return;
            }
            if (g_rcount < 65535) g_rcount++;
            g_rwork[g_rnext] = (unsigned short)g_rcount;
            g_rqueue[g_rqt++] = g_rnext;
        }
        for (int n = 0; n < 256 && g_rqh < g_rqt; n++) {
            int c = g_rqueue[g_rqh++], cx = c % GRID, cy = c / GRID;
            for (int k = 0; k < 8; k++) {
                int nx = cx + DX[k], ny = cy + DY[k];
                if (nx < 0 || ny < 0 || nx >= GRID || ny >= GRID) continue;
                int m = ny * GRID + nx;
                if (g_rstate[m] != 1 || g_rwork[m]) continue;
                if (k >= 4 && (g_rstate[cy * GRID + nx] != 1 || g_rstate[ny * GRID + cx] != 1)) continue;
                g_rwork[m] = g_rwork[c];
                g_rqueue[g_rqt++] = m;
            }
        }
        if (g_rqh == g_rqt) { g_rqh = g_rqt = 0; }
        if (OUT_OF_TIME()) return;
    }
    #undef OUT_OF_TIME
}

/* the region of a world point: its cell, else the nearest labelled cell within r cells (0 = none) */
static int region_near(int x, int y, int r)
{
    if (!in_map(x, y)) return 0;
    int c0 = (y / CELL) * GRID + x / CELL;
    if (g_rlabel[c0]) return g_rlabel[c0];
    for (int d = 1; d <= r; d++)
        for (int dy = -d; dy <= d; dy++)
            for (int dx = -d; dx <= d; dx++) {
                if (abs(dx) != d && abs(dy) != d) continue;
                int cx = x / CELL + dx, cy = y / CELL + dy;
                if (cx < 0 || cy < 0 || cx >= GRID || cy >= GRID) continue;
                if (g_rlabel[cy * GRID + cx]) return g_rlabel[cy * GRID + cx];
            }
    return 0;
}

/* 1 = same walkable area, 0 = different / closed off, -1 = labels not ready (use the floods) */
int path_regions_reachable(int sx, int sy, int tx, int ty)
{
    if (!g_rready) return -1;
    int a = region_near(sx, sy, 2), b = region_near(tx, ty, 6);   /* Sarge may stand against a fence; a click on an obstacle walks next to it */
    if (!a) return -1;
    return b && a == b;
}

/* ---------------------------------------------------------------- vehicle routes (user 2026-11-17: "vehicle
 * navigation was really really bad in both armymen1 and 2 ... path finding with respect to the vehicle's
 * bounding box"). The game's own rule (binary VehicleUpdate, exe 0x4176f0; lookup 2026-11-17):
 * the footprint is an oriented box around the vehicle's position - front / back / left / right px per type,
 * vehiclesizes at 0x4b2b20 (short[6][4]: 1 jeep 57,7,14,22; 2 tank 51,34,22,23; 3 halftrack, 4 convoy
 * 50,40,20,20) - indexed by the game enum = our VEH_TYPE - 1 (Boot Camp tank: VEH_TYPE 3) - sampled at 16 px tile centres; a tile is bad when the movement layer (*0x4e3554, a byte per
 * tile, width 0x4e368c, height 0x4e2ef0) is >= 15; the player's move is refused at 4+ bad tiles. Parked
 * vehicles are in a second layer (*0x507f78, non-zero = occupied). Here: A* over tiles with the footprint
 * turned to each step's direction, at most VEH_BAD_TILES bad tiles (stricter than the game, so routes keep
 * off walls), parked vehicles bad except near the vehicle's own start; then string pulling with the same
 * footprint test along each straight leg. */
#define VTILE 16
#define VGRID 256
#define VEH_SIZES     0x4b2b20
#define MOVE_LAYER    (*(signed char **)0x4e3554)
#define VEH_LAYER     (*(signed char **)0x507f78)
#define TILES_W       (*(unsigned short *)0x4e368c)
#define TILES_H       (*(unsigned short *)0x4e2ef0)
#define VEH_BAD_TILES 1
#define VEH_MAX_EXPAND 30000
static int g_vbox[4], g_vself_x, g_vself_y;
static BYTE g_vtest[VGRID * VGRID], g_vblock[VGRID * VGRID];      /* per tile, bit d: direction d tested / blocked */
static float g_vcost[VGRID * VGRID];
static int g_vfrom[VGRID * VGRID];
static BYTE g_vclosed[VGRID * VGRID], g_vseen[VGRID * VGRID];
static int g_vheap[VEH_MAX_EXPAND * 8 + 16];
static float g_vheapf[VEH_MAX_EXPAND * 8 + 16];
static int g_vnheap;

static int vtile_bad(int tx, int ty)
{
    int w = TILES_W, h = TILES_H;
    if (tx < 0 || ty < 0 || tx >= w || ty >= h || !MOVE_LAYER) return 1;
    if (MOVE_LAYER[ty * w + tx] >= 15) return 1;
    if (VEH_LAYER && VEH_LAYER[ty * w + tx] && (abs(tx * VTILE + 8 - g_vself_x) > 64 || abs(ty * VTILE + 8 - g_vself_y) > 64)) return 1;
    return 0;
}

/* bad tiles under the footprint at world (x, y), heading (dx, dy) */
static int vfoot_bad(double x, double y, double dx, double dy)
{
    double l = sqrt(dx * dx + dy * dy);
    if (l < 1e-6) { dx = 0; dy = -1; l = 1; }
    dx /= l; dy /= l;
    double px = -dy, py = dx;                                   /* sideways */
    int reach = g_vbox[0];
    for (int i = 1; i < 4; i++) if (g_vbox[i] > reach) reach = g_vbox[i];
    int bad = 0;
    for (int ty = (int)(y - reach) / VTILE; ty <= (int)(y + reach) / VTILE; ty++)
        for (int tx = (int)(x - reach) / VTILE; tx <= (int)(x + reach) / VTILE; tx++) {
            double cx = tx * VTILE + 8 - x, cy = ty * VTILE + 8 - y;
            double f = cx * dx + cy * dy, s = cx * px + cy * py;
            if (f > g_vbox[0] || f < -g_vbox[1] || s > g_vbox[3] || s < -g_vbox[2]) continue;
            if (vtile_bad(tx, ty) && ++bad > VEH_BAD_TILES) return bad;
        }
    return bad;
}

static const int VDX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, VDY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
static int vcell_ok(int c, int d)                               /* the vehicle on tile c, moving in direction d */
{
    if (!(g_vtest[c] & (1 << d))) {
        g_vtest[c] |= (BYTE)(1 << d);
        if (vfoot_bad((c % VGRID) * VTILE + 8, (c / VGRID) * VTILE + 8, VDX[d], VDY[d]) > VEH_BAD_TILES) g_vblock[c] |= (BYTE)(1 << d);
    }
    return !(g_vblock[c] & (1 << d));
}
static int vline_ok(double x0, double y0, double x1, double y1)
{
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    int n = (int)(len / 6) + 1;
    for (int i = 1; i <= n; i++) if (vfoot_bad(x0 + dx * i / n, y0 + dy * i / n, dx, dy) > VEH_BAD_TILES) return 0;
    return 1;
}
static void vpush(int c, float f)
{
    if (g_vnheap >= (int)(sizeof g_vheap / sizeof *g_vheap)) return;
    int i = g_vnheap++;
    while (i > 0) { int p = (i - 1) / 2; if (g_vheapf[p] <= f) break; g_vheap[i] = g_vheap[p]; g_vheapf[i] = g_vheapf[p]; i = p; }
    g_vheap[i] = c; g_vheapf[i] = f;
}
static int vpop(void)
{
    int top = g_vheap[0], c = g_vheap[--g_vnheap];
    float f = g_vheapf[g_vnheap];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        float mf = f;
        if (l < g_vnheap && g_vheapf[l] < mf) { m = l; mf = g_vheapf[l]; }
        if (r < g_vnheap && g_vheapf[r] < mf) { m = r; mf = g_vheapf[r]; }
        if (m == i) break;
        g_vheap[i] = g_vheap[m]; g_vheapf[i] = g_vheapf[m]; i = m;
    }
    g_vheap[i] = c; g_vheapf[i] = f;
    return top;
}
static float vest(int a, int b)                                 /* octile distance in tiles */
{
    int dx = abs(a % VGRID - b % VGRID), dy = abs(a / VGRID - b / VGRID);
    return (float)(dx > dy ? dx + 0.4142 * dy : dy + 0.4142 * dx);
}

/* Route for a vehicle of `type` from (sx, sy) to (tx, ty): waypoints into out, returns their count. */
int path_find_vehicle(int type, int sx, int sy, int tx, int ty, POINT *out, int max)
{
    const short *sz = (const short *)(intptr_t)(VEH_SIZES + 8 * (type >= 0 && type < 6 ? type : 0));
    for (int i = 0; i < 4; i++) g_vbox[i] = sz[i];
    if (!g_vbox[0] && !g_vbox[1]) { g_vbox[0] = 50; g_vbox[1] = 34; g_vbox[2] = 22; g_vbox[3] = 23; }
    g_vself_x = sx; g_vself_y = sy;
    int w = TILES_W, h = TILES_H;
    if (w <= 0 || h <= 0 || w > VGRID || h > VGRID || !MOVE_LAYER || max < 1) return 0;
    if (vline_ok(sx, sy, tx, ty)) {
        out[0] = (POINT){ tx, ty };
        if (g_cfg.log >= 2) mlog("path: vehicle %d,%d -> %d,%d: straight", sx, sy, tx, ty);
        return 1;
    }
    memset(g_vtest, 0, sizeof g_vtest); memset(g_vblock, 0, sizeof g_vblock);
    memset(g_vclosed, 0, sizeof g_vclosed); memset(g_vseen, 0, sizeof g_vseen);
    g_vnheap = 0;
    int start = (sy / VTILE) * VGRID + sx / VTILE, goal = (ty / VTILE) * VGRID + tx / VTILE;
    g_vcost[start] = 0; g_vfrom[start] = -1; g_vseen[start] = 1;
    vpush(start, vest(start, goal));
    int expanded = 0, found = 0, nearest = start;
    float nearest_h = vest(start, goal);
    while (g_vnheap > 0 && expanded < VEH_MAX_EXPAND) {
        int c = vpop();
        if (g_vclosed[c]) continue;
        g_vclosed[c] = 1; expanded++;
        if (c == goal) { found = 1; break; }
        float hh = vest(c, goal);
        if (hh < nearest_h) { nearest_h = hh; nearest = c; }
        int cx = c % VGRID, cy = c / VGRID;
        for (int d = 0; d < 8; d++) {
            int nx = cx + VDX[d], ny = cy + VDY[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            int n = ny * VGRID + nx;
            if (g_vclosed[n] || !vcell_ok(n, d)) continue;
            float g = g_vcost[c] + (d >= 4 ? 1.4142f : 1.0f);
            if (g_vseen[n] && g_vcost[n] <= g) continue;
            g_vseen[n] = 1; g_vcost[n] = g; g_vfrom[n] = c;
            vpush(n, g + vest(n, goal));
        }
    }
    int end = found ? goal : nearest;
    if (end == start) { mlog("path: vehicle %d,%d -> %d,%d: no route (%d tiles searched)", sx, sy, tx, ty, expanded); return 0; }
    static int chain[VGRID * VGRID / 4];
    int nc = 0;
    for (int c = end; c != -1 && nc < (int)(sizeof chain / sizeof *chain); c = g_vfrom[c]) chain[nc++] = c;
    int n = 0, i = nc - 1;
    double cx = sx, cy = sy;
    while (i > 0 && n < max) {
        int j;
        for (j = 0; j < i; j++) {
            double px = (chain[j] % VGRID) * VTILE + 8, py = (chain[j] / VGRID) * VTILE + 8;
            if (j == 0 && found) { px = tx; py = ty; }
            if (vline_ok(cx, cy, px, py)) break;
        }
        if (j == i) j = i - 1;
        double px = (chain[j] % VGRID) * VTILE + 8, py = (chain[j] / VGRID) * VTILE + 8;
        if (j == 0 && found) { px = tx; py = ty; }
        out[n++] = (POINT){ (LONG)px, (LONG)py };
        cx = px; cy = py; i = j;
    }
    mlog("path: vehicle type %d (box %d,%d,%d,%d) %d,%d -> %d,%d: %s after %d tiles, %d waypoints", type, g_vbox[0], g_vbox[1], g_vbox[2], g_vbox[3], sx, sy, tx, ty, found ? "found" : "partial", expanded, n);
    return n;
}

void path_regions_step(void) { PROF_BEGIN(t); path_regions_step_body(); PROF_END(t, PR_REGIONS); }   /* timed (trace) */
