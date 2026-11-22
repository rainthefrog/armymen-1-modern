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

enum { UNKNOWN = 0, FREE = 1, BLOCKED = 2, FENCE = 3 };
static BYTE g_state[GRID * GRID];       /* per search: UNKNOWN / FREE / BLOCKED / FENCE (passable: Sarge breaks it, fences.c) */
#define FENCE_COST  10.0f               /* extra cost per fence cell entered (8 px): a way round up to ~30 cells (240 px) longer wins */
static BYTE g_closed[GRID * GRID];
static BYTE g_seen[GRID * GRID];        /* g_cost / g_from valid in this search */
static float g_cost[GRID * GRID];
static int g_from[GRID * GRID];
static int g_touched[MAX_EXPAND * 9 + 256]; static int g_ntouched;

static int g_start = -1;               /* this search's start cell: next to it only the centre is tested */
/* A cell is free when none of a 4x4 grid of points inside it (every 2 px) is blocked: fences are 1-2 px
 * thin and slipped between the old centre + 4 sample points, so routes ran straight through them.
 * Next to Sarge's own cell only its centre is tested (he may stand right against a fence). */
/* [gameplay] break_fences (fences.c, 2026-11-20): a sample point blocked only by breakable wooden fences does not
 * block; a cell with such points (and no others) is FENCE - passable at FENCE_COST, Sarge breaks the fence there.
 * Shared with the regions sweep (path_cell_class) and the mission route (missionpath.c). */
int path_cell_class(int x0, int y0, int centre_only)
{
    int fence = 0;
    #define PC_TEST(x, y) do { if (controls_blocked_at(x, y)) { if (fences_only_at(x, y, 1)) fence = 1; else return BLOCKED; } } while (0)
    PC_TEST(x0 + CELL / 2, y0 + CELL / 2);
    for (int sy = 1; sy < CELL && !centre_only; sy += 2)
        for (int sx = 1; sx < CELL; sx += 2) PC_TEST(x0 + sx, y0 + sy);
    #undef PC_TEST
    return fence ? FENCE : FREE;
}
static int cell_free(int c)
{
    if (g_state[c] == UNKNOWN) {
        int x0 = (c % GRID) * CELL, y0 = (c / GRID) * CELL;
        int near_start = g_start >= 0 && abs(c % GRID - g_start % GRID) <= 1 && abs(c / GRID - g_start / GRID) <= 1;
        g_state[c] = (BYTE)path_cell_class(x0, y0, near_start);
        if (g_ntouched < (int)(sizeof g_touched / sizeof *g_touched)) g_touched[g_ntouched++] = c;
    }
    return g_state[c] == FREE || g_state[c] == FENCE;
}

/* The fence cells the last click route (path_find) goes through: fences.c breaks a fence at once only there. */
#define ROUTE_FENCES 32
static POINT g_route_fence[ROUTE_FENCES]; static int g_nroute_fence;
int path_route_fence_near(int x, int y, int r)
{
    for (int i = 0; i < g_nroute_fence; i++)
        if (abs(g_route_fence[i].x - x) <= r && abs(g_route_fence[i].y - y) <= r) return 1;
    return 0;
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

static int path_find_body(int sx, int sy, int tx, int ty, POINT *out, int max)
{
    g_nroute_fence = 0;
    if (!in_map(sx, sy) || !in_map(tx, ty) || max < 1) return 0;
    reset();
    int start = (sy / CELL) * GRID + sx / CELL, goal = (ty / CELL) * GRID + tx / CELL;
    g_start = start;
    if (cell_free(goal) && g_state[goal] == FENCE) g_state[goal] = BLOCKED;   /* a click on a fence: walk next to it */
    if (!cell_free(goal)) {                     /* clicked on an obstacle: nearest free cell to it */
        int best = -1;
        for (int r = 1; r <= 6 && best < 0; r++)
            for (int dy = -r; dy <= r && best < 0; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    int x = goal % GRID + dx, y = goal / GRID + dy;
                    if (x < 0 || y < 0 || x >= GRID || y >= GRID) continue;
                    if (cell_free(y * GRID + x) && g_state[y * GRID + x] == FREE) { best = y * GRID + x; break; }
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
            float g = g_cost[c] + (k >= 4 ? 1.4142f : 1.0f) + (g_state[n] == FENCE ? FENCE_COST : 0.0f);
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
    for (int k = 0; k < nc && g_nroute_fence < ROUTE_FENCES; k++)          /* fences on the route (fences.c breaks them) */
        if (g_state[chain[k]] == FENCE) g_route_fence[g_nroute_fence++] = (POINT){ (chain[k] % GRID) * CELL + CELL / 2, (chain[k] / GRID) * CELL + CELL / 2 };
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
    if (g_cfg.log >= 2 || (g_nroute_fence && g_cfg.log >= 1)) mlog("path: %d,%d -> %d,%d: %s after %d cells, %d waypoints, %d fence cells (break_fences)", sx, sy, tx, ty, found ? "found" : "partial", expanded, n, g_nroute_fence);
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
static int path_reachable_body(int sx, int sy, int tx, int ty, int budget_ms)
{
    if (!in_map(sx, sy) || !in_map(tx, ty)) return 0;
    /* the whole-map areas, when built: "same area" is trusted; "different" is checked with the floods below
     * (the areas can be a sweep old - a vehicle moved away), which decide unless they run out of time */
    int known = path_regions_reachable(sx, sy, tx, ty);
    if (known == 1) return 1;
    reset();
    int start = (sy / CELL) * GRID + sx / CELL, goal = (ty / CELL) * GRID + tx / CELL;
    g_start = start;
    if (cell_free(goal) && g_state[goal] == FENCE) g_state[goal] = BLOCKED;
    if (!cell_free(goal)) {                       /* on an obstacle: the walk would end next to it */
        int best = -1;
        for (int r = 1; r <= 6 && best < 0; r++)
            for (int dy = -r; dy <= r && best < 0; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (abs(dx) != r && abs(dy) != r) continue;
                    int x = goal % GRID + dx, y = goal / GRID + dy;
                    if (x < 0 || y < 0 || x >= GRID || y >= GRID) continue;
                    if (cell_free(y * GRID + x) && g_state[y * GRID + x] == FREE) { best = y * GRID + x; break; }
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
    return path_cell_class(x0, y0, 0) == BLOCKED;           /* break_fences: fence-only cells connect (fences.c) */
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
static int g_vbox[4], g_vself_x, g_vself_y, g_vself_f = -1;   /* start pose; facing -1 = unknown */
static BYTE g_vtest[VGRID * VGRID], g_vblock[VGRID * VGRID];      /* per tile, bit d: direction d tested / blocked */
static float g_vcost[VGRID * VGRID];
static int g_vfrom[VGRID * VGRID];
static BYTE g_vclosed[VGRID * VGRID], g_vseen[VGRID * VGRID];
static int g_vheap[VEH_MAX_EXPAND * 8 + 16];
static float g_vheapf[VEH_MAX_EXPAND * 8 + 16];
static int g_vnheap;

/* Objects that hold up a vehicle on a tile (2026-11-20, user: "vehicles can get stuck on objects"): the game's own
 * vehicle collision (game binary damageItemsInMask, exe 0x417ef0) counts, per tile the footprint newly covers, every
 * asset / structure / obstacle with movement > 0 at the tile centre (GetObjectsIntersectPoint 0x42fca0), and wrecks;
 * a move adding more than one such tile is refused. Sandbag walls, rocks, buildings are objects, not movement-layer
 * tiles, so routes went straight through them and the vehicle crept into them a tile at a time until it was wedged.
 * Bushes (class 1), fences (5) and explosives (9) are run over (damaged), so they do not count. Cached per search. */
typedef BYTE *(__fastcall *VObjectsAtFn)(short *point);
#define V_OBJECTS_AT   ((VObjectsAtFn)0x42fca0)          /* GetObjectsIntersectPoint: ITEM list, next at +0x60 */
#define V_ITEM_NEXT(it) (*(BYTE **)((it) + 0x60))
#define V_ITEM_TYPE1(it) ((int)((*(DWORD *)(it) >> 22) & 0xf))   /* ITEM_TYPE + 1 (UID bits) */
#define V_ITEM_CLASS(it) ((int)((*(DWORD *)(it) >> 16) & 0x3f) - 1)
#define V_OBJ_MOVEMENT(it) (*(signed char *)((it) + 0x6f))
static BYTE g_vobj[VGRID * VGRID];                     /* per search: 0 unknown, 1 free, 2 object */
static int vtile_object_now(int tx, int ty)
{
    short p[2] = { (short)(tx * VTILE + 8), (short)(ty * VTILE + 8) };
    for (BYTE *it = V_OBJECTS_AT(p); it; it = V_ITEM_NEXT(it)) {
        int t = V_ITEM_TYPE1(it), c = V_ITEM_CLASS(it);
        if (t == 10) return 1;                                         /* husk (wreck) */
        if ((t == 7 || t == 8 || t == 9) && V_OBJ_MOVEMENT(it) > 0 && c != 1 && c != 5 && c != 9) return 1;
    }
    return 0;
}
static int vtile_object(int tx, int ty)
{
    if (!g_cfg.vehicle_unstick) return 0;                              /* switch off: the old rule (tiles only) */
    if (tx < 0 || ty < 0 || tx >= VGRID || ty >= VGRID) return 1;
    BYTE *s = &g_vobj[ty * VGRID + tx];
    if (!*s) *s = vtile_object_now(tx, ty) ? 2 : 1;
    return *s == 2;
}

/* The vehicle's own tiles in the parked-vehicle layer (a vehicle standing still adds itself, game binary
 * AddVehicleToVehicleLayer): with the start facing known, the tiles under its footprint at the start (+ 8 px);
 * otherwise everything within 64 px of the start (the old rule - it also hid the vehicles parked next to it). */
static int vtile_self(int tx, int ty)
{
    double cx = tx * VTILE + 8 - g_vself_x, cy = ty * VTILE + 8 - g_vself_y;
    if (g_vself_f < 0) return fabs(cx) <= 64 && fabs(cy) <= 64;
    double dx, dy; controls_facing_vector(g_vself_f, &dx, &dy);
    double f = cx * dx + cy * dy, s = -cx * dy + cy * dx;
    return f <= g_vbox[0] + 8 && f >= -g_vbox[1] - 8 && s <= g_vbox[3] + 8 && s >= -g_vbox[2] - 8;
}

/* Tiles a vehicle drives OVER (2026-11-20, user: mission 3 route dashed - the tank waits at (320,3160) inside a yard
 * of wooden fences, and the tile test closed the yard off): an object of class bush (1) or fence (5) makes its
 * tiles untrafficable in the movement layer (>= 15), but VehicleUpdate lets a vehicle onto them (fewer than 4
 * untrafficable tiles under the mask) and damageItemsInMask runs them over. Such a tile counts as free for vehicle
 * routes when every object found on it (centre + 4 points 4 px in from the corners) is a bush / fence and there is
 * at least one. Cached per search (g_vcrush: 0 unknown, 1 no, 2 yes). */
static BYTE g_vcrush[VGRID * VGRID];
static int g_vcrush_used;                                              /* per search, for the log */
static int g_vcrush_on = 1;                                            /* off: walking routes that borrow the vehicle planner (missionpath.c wide try) */
void path_vehicle_crush(int on) { g_vcrush_on = on; }
static int vtile_crushable(int tx, int ty)
{
    if (tx < 0 || ty < 0 || tx >= VGRID || ty >= VGRID) return 0;
    BYTE *s = &g_vcrush[ty * VGRID + tx];
    if (!*s) {
        static const int PX[5] = { 8, 4, 12, 4, 12 }, PY[5] = { 8, 4, 4, 12, 12 };
        int crush = 0, other = 0;
        for (int k = 0; k < 5 && !other; k++) {
            short p[2] = { (short)(tx * VTILE + PX[k]), (short)(ty * VTILE + PY[k]) };
            for (BYTE *it = V_OBJECTS_AT(p); it; it = V_ITEM_NEXT(it)) {
                int t = V_ITEM_TYPE1(it), c = V_ITEM_CLASS(it);
                if (t == 10) { other = 1; break; }
                if ((t == 7 || t == 8 || t == 9) && V_OBJ_MOVEMENT(it) > 0) { if (c == 1 || c == 5) crush = 1; else { other = 1; break; } }
            }
        }
        *s = crush && !other ? 2 : 1;
        if (*s == 2) g_vcrush_used++;
    }
    return *s == 2;
}

static int vtile_bad(int tx, int ty)
{
    int w = TILES_W, h = TILES_H;
    if (tx < 0 || ty < 0 || tx >= w || ty >= h || !MOVE_LAYER) return 1;
    if (MOVE_LAYER[ty * w + tx] >= 15 && !(g_vcrush_on && vtile_crushable(tx, ty))) return 1;
    if (VEH_LAYER && VEH_LAYER[ty * w + tx] && !vtile_self(tx, ty)) return 1;
    if (vtile_object(tx, ty)) return 1;
    return 0;
}

/* bad tiles under the footprint at world (x, y), heading (dx, dy) */
static int vfoot_count(double x, double y, double dx, double dy, int cap)   /* stops counting above cap */
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
            if (vtile_bad(tx, ty) && ++bad > cap) return bad;
        }
    return bad;
}
static int vfoot_bad(double x, double y, double dx, double dy) { return vfoot_count(x, y, dx, dy, VEH_BAD_TILES); }

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
/* 2026-11-21 (stutter hunt 2): the search as a resumable job so it can also run in SLICES. vsearch_begin = set-up;
 * vsearch_run = the phases, each resumable: 0 the straight-line test, 1 the A* expansions, 2 the route back and the
 * string pulling (one vline_ok per step), 3 done. The state stays in the statics above / VS. path_find_vehicle runs
 * them all at once (exactly as before); path_find_vehicle_begin / _step (missionpath.c's wide routes at mission start:
 * one search was 50-105 ms in ONE frame on the user's PC, "partial after 25233 tiles") give the same result over
 * several frames, at most about budget_ms each. */
#define VS_OUT_MAX 256
static struct {
    int type, sx, sy, tx, ty, start, goal, expanded, found, nearest, w, h, max, phase, result;
    float nearest_h;
    int nc, i, j, n; double cx, cy;                     /* phase 2: string pulling */
    POINT out[VS_OUT_MAX];
} VS;
static int vs_chain[VGRID * VGRID / 4];
static unsigned g_vsearch_gen;                  /* +1 per search begun: a sliced search sees when another one ran */
static void vsearch_begin(int type, int sx, int sy, int tx, int ty, int max)
{
    g_vsearch_gen++;
    VS.type = type; VS.sx = sx; VS.sy = sy; VS.tx = tx; VS.ty = ty; VS.max = max < VS_OUT_MAX ? max : VS_OUT_MAX;
    VS.phase = 0; VS.result = 0;
    const short *sz = (const short *)(intptr_t)(VEH_SIZES + 8 * (type >= 0 && type < 6 ? type : 0));
    for (int i = 0; i < 4; i++) g_vbox[i] = sz[i];
    if (!g_vbox[0] && !g_vbox[1]) { g_vbox[0] = 50; g_vbox[1] = 34; g_vbox[2] = 22; g_vbox[3] = 23; }
    g_vself_x = sx; g_vself_y = sy; g_vself_f = -1;
    memset(g_vobj, 0, sizeof g_vobj); memset(g_vcrush, 0, sizeof g_vcrush); g_vcrush_used = 0;                           /* objects may have moved / been destroyed */
    int w = TILES_W, h = TILES_H;
    VS.w = w; VS.h = h;
    if (w <= 0 || h <= 0 || w > VGRID || h > VGRID || !MOVE_LAYER || VS.max < 1) { VS.phase = 3; VS.result = 0; }
}
static int vs_time_up(const LARGE_INTEGER *q0, double budget_ms)
{
    if (budget_ms < 0) return 0;
    static LARGE_INTEGER qf; LARGE_INTEGER qn;
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&qn);
    return (double)(qn.QuadPart - q0->QuadPart) * 1000.0 / (double)qf.QuadPart >= budget_ms;
}
static int vsearch_run(double budget_ms)        /* 1 = done (VS.result points in VS.out); 0 = the budget (ms, < 0 = none) ran out */
{
    LARGE_INTEGER q0; QueryPerformanceCounter(&q0);
    int sx = VS.sx, sy = VS.sy, tx = VS.tx, ty = VS.ty;
    if (VS.phase == 0) {
        if (vline_ok(sx, sy, tx, ty)) {
            VS.out[0] = (POINT){ tx, ty };
            if (g_cfg.log >= 2) mlog("path: vehicle %d,%d -> %d,%d: straight", sx, sy, tx, ty);
            VS.phase = 3; VS.result = 1;
            return 1;
        }
        memset(g_vtest, 0, sizeof g_vtest); memset(g_vblock, 0, sizeof g_vblock);
        memset(g_vclosed, 0, sizeof g_vclosed); memset(g_vseen, 0, sizeof g_vseen);
        g_vnheap = 0;
        int start = (sy / VTILE) * VGRID + sx / VTILE, goal = (ty / VTILE) * VGRID + tx / VTILE;
        g_vcost[start] = 0; g_vfrom[start] = -1; g_vseen[start] = 1;
        vpush(start, vest(start, goal));
        VS.start = start; VS.goal = goal; VS.expanded = 0; VS.found = 0; VS.nearest = start; VS.nearest_h = vest(start, goal);
        VS.phase = 1;
    }
    if (VS.phase == 1) {
        int w = VS.w, h = VS.h, goal = VS.goal;
        while (g_vnheap > 0 && VS.expanded < VEH_MAX_EXPAND) {
            if ((VS.expanded & 127) == 0 && vs_time_up(&q0, budget_ms)) return 0;
            int c = vpop();
            if (g_vclosed[c]) continue;
            g_vclosed[c] = 1; VS.expanded++;
            if (c == goal) { VS.found = 1; break; }
            float hh = vest(c, goal);
            if (hh < VS.nearest_h) { VS.nearest_h = hh; VS.nearest = c; }
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
        int end = VS.found ? VS.goal : VS.nearest;
        if (end == VS.start) { mlog("path: vehicle %d,%d -> %d,%d: no route (%d tiles searched)", sx, sy, tx, ty, VS.expanded); VS.phase = 3; VS.result = 0; return 1; }
        VS.nc = 0;
        for (int c = end; c != -1 && VS.nc < (int)(sizeof vs_chain / sizeof *vs_chain); c = g_vfrom[c]) vs_chain[VS.nc++] = c;
        VS.n = 0; VS.i = VS.nc - 1; VS.j = 0; VS.cx = sx; VS.cy = sy;
        VS.phase = 2;
    }
    if (VS.phase == 2) {
        int found = VS.found;
        while (VS.i > 0 && VS.n < VS.max) {
            for (; VS.j < VS.i; VS.j++) {
                if (vs_time_up(&q0, budget_ms)) return 0;           /* (before each line test: they are the cost here) */
                double px = (vs_chain[VS.j] % VGRID) * VTILE + 8, py = (vs_chain[VS.j] / VGRID) * VTILE + 8;
                if (VS.j == 0 && found) { px = tx; py = ty; }
                if (vline_ok(VS.cx, VS.cy, px, py)) break;
            }
            int j = VS.j;
            if (j == VS.i) j = VS.i - 1;
            double px = (vs_chain[j] % VGRID) * VTILE + 8, py = (vs_chain[j] / VGRID) * VTILE + 8;
            if (j == 0 && found) { px = tx; py = ty; }
            VS.out[VS.n++] = (POINT){ (LONG)px, (LONG)py };
            VS.cx = px; VS.cy = py; VS.i = j; VS.j = 0;
        }
        mlog("path: vehicle type %d (box %d,%d,%d,%d) %d,%d -> %d,%d: %s after %d tiles, %d waypoints, %d fence / bush tiles driven over", VS.type, g_vbox[0], g_vbox[1], g_vbox[2], g_vbox[3], sx, sy, tx, ty, found ? "found" : "partial", VS.expanded, VS.n, g_vcrush_used);
        VS.phase = 3; VS.result = VS.n;
    }
    return 1;
}
static int vsearch_copy(POINT *out, int max)
{
    int n = VS.result < max ? VS.result : max;
    if (n > 0) memcpy(out, VS.out, sizeof(POINT) * n);
    return n > 0 ? n : 0;
}
static int path_find_vehicle_body(int type, int sx, int sy, int tx, int ty, POINT *out, int max)
{
    vsearch_begin(type, sx, sy, tx, ty, max);
    vsearch_run(-1);
    return vsearch_copy(out, max);
}
/* sliced: begin, then step once per frame until it returns >= 0 (the route's point count, as path_find_vehicle).
 * A search begun elsewhere meanwhile (vehicles.c) shares the statics: the sliced one then starts over. */
static struct { int on, type, sx, sy, tx, ty, max; unsigned gen; } VR;
static int path_find_vehicle_begin_body(int type, int sx, int sy, int tx, int ty, int max)
{
    VR.on = 1; VR.type = type; VR.sx = sx; VR.sy = sy; VR.tx = tx; VR.ty = ty; VR.max = max;
    vsearch_begin(type, sx, sy, tx, ty, max);
    VR.gen = g_vsearch_gen;
    return 1;
}
static int path_find_vehicle_step_body(double budget_ms, POINT *out, int max)
{
    if (!VR.on) return 0;
    if (VR.gen != g_vsearch_gen) {                       /* another search used the statics: start over */
        vsearch_begin(VR.type, VR.sx, VR.sy, VR.tx, VR.ty, VR.max);
        VR.gen = g_vsearch_gen;
    }
    if (!vsearch_run(budget_ms)) return -1;              /* next frame */
    VR.on = 0;
    return vsearch_copy(out, max < VR.max ? max : VR.max);
}


/* ---------------------------------------------------------------- vehicle routes with reversing (2026-11-20, user:
 * "vehicle navigation needs to be reworked to allow for reversing to reorient the vehicle to fit through gaps").
 * Hybrid A* over (x, y, heading): a state is a pose (float x, y; heading 0..31 = game facing / 8, the game's own
 * mask resolution - game binary rounds the facing to a multiple of 8 for vehiclemasks), closed per 8 px cell and
 * heading. Motions from a pose: forward or backward g_ha_step px along the heading, straight or turning one heading
 * step on the way (the vehicle turns 8 facings per 60 ms while it moves), and for vehicles that can turn on the spot
 * (Sarge's tank and halftrack; jeep and convoy only turn while moving - VehicleUpdate scales their spin by speed)
 * one heading step without moving. Backward motion costs HA_REV_COST times more, a change of direction
 * HA_SWITCH_COST px, so reversing is used where it is needed (lining up with a gap, backing out of a corner).
 * Each pose on the way is tested with the vehicle's oriented footprint (vfoot_bad, the same test as the tile
 * routes: movement layer, parked vehicles, blocking objects; at most VEH_BAD_TILES), cached per cell and heading.
 * Heuristic: distance to the goal over 16 px tiles around blocked tiles (one Dijkstra from the goal per plan), at
 * least the straight distance. The search runs in slices (vehicles.c: a few ms per gameplay frame) and stops
 * after HA_MAX_NODES poses; then the pose that got closest is used (partial route, the driver plans again from
 * its end). */
#define HA_CELL        8
#define HA_GRID        (VGRID * VTILE / HA_CELL)        /* 512 cells per side */
#define HA_HEADS       32
#define HA_MAX_NODES   60000
#define HA_HASH        (1 << 17)
/* backward motion cost factor: 4 (2026-11-21, user: "clicking behind a vehicle makes them reverse to the position. make them
 * turn around and go to the position" - at 1.6 backing 200 px (320) beat turning round (tank spin ~144 + 200)); short
 * backing to line up with a gap stays cheap enough. [controls] vehicle_reverse_cost (x10, default 40) */
static float ha_rev_cost(void) { static float c = -1; if (c < 0) { int v = GetPrivateProfileIntA("controls", "vehicle_reverse_cost", 40, mod_ini_path()); c = v < 10 ? 1.0f : v / 10.0f; } return c; }
#define HA_REV_COST    ha_rev_cost()
#define HA_SWITCH_COST 40.0f
#define HA_TURN_COST   3.0f
#define HA_SPIN_COST   9.0f
#define HA_GOAL        20.0f                          /* px: a pose this close to the target is there */
#define HA_SUBSTEPS    4
#define HA_ESCAPE      56.0f                          /* cost (px) from the start within which a wedged start may escape */
typedef struct { float x, y, g, f; int parent; signed char h, dir; } HaNode;
static HaNode g_ha[HA_MAX_NODES];
static int g_ha_n;
static int g_ha_hkey[HA_HASH], g_ha_hnode[HA_HASH];      /* (cell, heading) + 1 -> newest node; key 0 = empty */
static unsigned g_ha_ftest[HA_GRID * HA_GRID], g_ha_fbad[HA_GRID * HA_GRID];   /* bit h: footprint tested / bad */
static float g_ha_h2d[VGRID * VGRID];
static double g_ha_dx[HA_HEADS], g_ha_dy[HA_HEADS];
static int g_ha_heap[HA_MAX_NODES * 2], g_ha_nheap;
static float g_ha_tx, g_ha_ty;
static int g_ha_type, g_ha_spin, g_ha_step, g_ha_best, g_ha_running, g_ha_expanded;
static BYTE g_ha_closed_bits[(HA_GRID * HA_GRID * HA_HEADS) / 8];
static int g_ha_phase, g_ha_sx, g_ha_sy, g_ha_h0, g_ha_box[4], g_ha_self[3], g_ha_retargeted;   /* 0 = tile distances, 1 = search */

static int ha_foot_ok(float x, float y, int h)
{
    if (x < 0 || y < 0) return 0;
    int cx = (int)x / HA_CELL, cy = (int)y / HA_CELL;
    if (cx >= HA_GRID || cy >= HA_GRID) return 0;
    int c = cy * HA_GRID + cx;
    h &= HA_HEADS - 1;
    unsigned bit = 1u << h;
    if (!(g_ha_ftest[c] & bit)) {
        g_ha_ftest[c] |= bit;
        if (vfoot_bad(cx * HA_CELL + HA_CELL / 2, cy * HA_CELL + HA_CELL / 2, g_ha_dx[h], g_ha_dy[h]) > VEH_BAD_TILES)
            g_ha_fbad[c] |= bit;
    }
    return !(g_ha_fbad[c] & bit);
}
/* Near the start (motions within HA_ESCAPE px of cost) a vehicle that already stands on blocked tiles (wedged, or
 * against a fence after a partial route) may move through poses no worse than its start: otherwise every motion
 * from a blocked start fails and it can never plan its way out (Wine 2026-11-20: jeep against the Boot Camp wire). */
static int g_ha_start_bad;
static int ha_foot_escape(float x, float y, int h)
{
    if (ha_foot_ok(x, y, h)) return 1;
    if (g_ha_start_bad <= VEH_BAD_TILES || x < 0 || y < 0) return 0;
    return vfoot_count(x, y, g_ha_dx[h & (HA_HEADS - 1)], g_ha_dy[h & (HA_HEADS - 1)], g_ha_start_bad) <= g_ha_start_bad;
}
static void ha_push(int n)
{
    if (g_ha_nheap >= (int)(sizeof g_ha_heap / sizeof *g_ha_heap)) return;
    int i = g_ha_nheap++; float f = g_ha[n].f;
    while (i > 0) { int p = (i - 1) / 2; if (g_ha[g_ha_heap[p]].f <= f) break; g_ha_heap[i] = g_ha_heap[p]; i = p; }
    g_ha_heap[i] = n;
}
static int ha_pop(void)
{
    int top = g_ha_heap[0], n = g_ha_heap[--g_ha_nheap], i = 0;
    float f = g_ha[n].f;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i; float mf = f;
        if (l < g_ha_nheap && g_ha[g_ha_heap[l]].f < mf) { m = l; mf = g_ha[g_ha_heap[l]].f; }
        if (r < g_ha_nheap && g_ha[g_ha_heap[r]].f < mf) { m = r; }
        if (m == i) break;
        g_ha_heap[i] = g_ha_heap[m]; i = m;
    }
    g_ha_heap[i] = n;
    return top;
}
static int ha_key(float x, float y, int h) { return (((int)y / HA_CELL) * HA_GRID + (int)x / HA_CELL) * HA_HEADS + (h & (HA_HEADS - 1)); }
static float ha_heur(float x, float y)
{
    float dx = x - g_ha_tx, dy = y - g_ha_ty, e = sqrtf(dx * dx + dy * dy);
    int tx = (int)x / VTILE, ty = (int)y / VTILE;
    float t = (x >= 0 && y >= 0 && tx < VGRID && ty < VGRID) ? g_ha_h2d[ty * VGRID + tx] : 1e9f;
    if (t >= 1e8f) t = e + 400.0f;                      /* not connected on the tile grid: discourage, do not forbid */
    return t > e ? t : e;
}
/* the node for (x, y, h) to be (re)opened with cost g, or -1 when a cheaper one exists / it is closed / full */
static int ha_node_for(float x, float y, int h, float g)
{
    if (x < 0 || y < 0 || (int)x / HA_CELL >= HA_GRID || (int)y / HA_CELL >= HA_GRID) return -1;
    int key = ha_key(x, y, h);
    if (g_ha_closed_bits[key >> 3] & (1 << (key & 7))) return -1;
    if (g_ha_n >= HA_MAX_NODES) return -1;
    unsigned s = ((unsigned)(key + 1) * 2654435761u) & (HA_HASH - 1);
    for (int probe = 0; probe < 64; probe++, s = (s + 1) & (HA_HASH - 1)) {
        if (g_ha_hkey[s] == key + 1) {
            if (g_ha[g_ha_hnode[s]].g <= g) return -1;
            g_ha_hnode[s] = g_ha_n;
            return g_ha_n++;
        }
        if (!g_ha_hkey[s]) {
            g_ha_hkey[s] = key + 1; g_ha_hnode[s] = g_ha_n;
            return g_ha_n++;
        }
    }
    return -1;
}

/* Start a plan for vehicle `type` (VEH_TYPE - 1) from pose (sx, sy, facing 0..255) to (tx, ty).
 * can_spin: the vehicle turns without moving. step: px per motion. Returns 0 if it cannot start. */
int path_vehicle_plan_begin(int type, int sx, int sy, int facing, int tx, int ty, int can_spin, int step)
{
    g_vsearch_gen++;                            /* shares the tile search statics: a sliced path_find_vehicle starts over */
    const short *sz = (const short *)(intptr_t)(VEH_SIZES + 8 * (type >= 0 && type < 6 ? type : 0));
    for (int i = 0; i < 4; i++) g_vbox[i] = sz[i];
    if (!g_vbox[0] && !g_vbox[1]) { g_vbox[0] = 50; g_vbox[1] = 34; g_vbox[2] = 22; g_vbox[3] = 23; }
    g_vself_x = sx; g_vself_y = sy; g_vself_f = facing & 255;
    int w = TILES_W, hh = TILES_H;
    g_ha_running = 0;
    if (w <= 0 || hh <= 0 || w > VGRID || hh > VGRID || !MOVE_LAYER) return 0;
    if (tx < 0 || ty < 0 || tx / VTILE >= w || ty / VTILE >= hh) return 0;
    memset(g_vobj, 0, sizeof g_vobj); memset(g_vcrush, 0, sizeof g_vcrush); g_vcrush_used = 0;
    memset(g_ha_ftest, 0, sizeof g_ha_ftest); memset(g_ha_fbad, 0, sizeof g_ha_fbad);
    memset(g_ha_hkey, 0, sizeof g_ha_hkey); memset(g_ha_closed_bits, 0, sizeof g_ha_closed_bits);
    for (int h = 0; h < HA_HEADS; h++) controls_facing_vector(h * (256 / HA_HEADS), &g_ha_dx[h], &g_ha_dy[h]);
    g_ha_type = type; g_ha_spin = can_spin; g_ha_step = step < 8 ? 8 : step;
    g_ha_tx = (float)tx; g_ha_ty = (float)ty;
    /* tile distance to the goal around blocked tiles (Dijkstra, 8 neighbours) - reuses the tile route's heap */
    for (int i = 0; i < VGRID * VGRID; i++) g_ha_h2d[i] = 1e9f;
    memset(g_vclosed, 0, sizeof g_vclosed);
    g_vnheap = 0;
    int goal = (ty / VTILE) * VGRID + tx / VTILE;
    g_ha_h2d[goal] = 0; vpush(goal, 0);
    g_ha_sx = sx; g_ha_sy = sy; g_ha_h0 = ((facing + 4) & 255) / (256 / HA_HEADS);
    memcpy(g_ha_box, g_vbox, sizeof g_ha_box); g_ha_self[0] = sx; g_ha_self[1] = sy; g_ha_self[2] = g_vself_f;
    g_ha_start_bad = vfoot_count(sx, sy, g_ha_dx[g_ha_h0], g_ha_dy[g_ha_h0], 64);   /* wedged at the start: see ha_foot_escape */
    g_ha_phase = 0; g_ha_retargeted = 0;   /* the Dijkstra and the search run in path_vehicle_plan_step slices */
    g_ha_running = 1;
    return 1;
}

/* the search's start, once the tile distances are done */
static int ha_start(void)
{
    g_ha_n = 0; g_ha_nheap = 0; g_ha_expanded = 0;
    int h0 = g_ha_h0;
    int n0 = ha_node_for((float)g_ha_sx, (float)g_ha_sy, h0, 0);
    if (n0 < 0) return 0;
    HaNode *s = &g_ha[n0];
    s->x = (float)g_ha_sx; s->y = (float)g_ha_sy; s->g = 0; s->h = (signed char)h0; s->dir = 0; s->parent = -1;
    s->f = ha_heur(s->x, s->y);
    g_ha_best = n0;
    ha_push(n0);
    g_ha_phase = 1;
    return 1;
}

/* Run the search for up to budget_ms. Returns 0 = still searching, 1 = found, 2 = gave up (closest pose), -1 = none. */
static int path_vehicle_plan_step_body(double budget_ms)
{
    g_vsearch_gen++;                            /* (as in path_vehicle_plan_begin) */
    if (!g_ha_running) return -1;
    LARGE_INTEGER t0, t, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);
    /* other routes (missionpath.c driven legs) share the footprint state between slices: this plan's back */
    memcpy(g_vbox, g_ha_box, sizeof g_vbox); g_vself_x = g_ha_self[0]; g_vself_y = g_ha_self[1]; g_vself_f = g_ha_self[2];
    if (g_ha_phase == 0) {          /* tile distance to the goal around blocked tiles (Dijkstra, 8 neighbours) */
        int w = TILES_W, hh = TILES_H, it = 0;
        while (g_vnheap > 0) {
            if ((++it & 255) == 0) {
                QueryPerformanceCounter(&t);
                if ((double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart > budget_ms) return 0;
            }
            int c = vpop();
            if (g_vclosed[c]) continue;
            g_vclosed[c] = 1;
            int cx = c % VGRID, cy = c / VGRID;
            for (int d = 0; d < 8; d++) {
                int nx = cx + VDX[d], ny = cy + VDY[d];
                if (nx < 0 || ny < 0 || nx >= w || ny >= hh) continue;
                int n = ny * VGRID + nx;
                if (g_vclosed[n] || vtile_bad(nx, ny)) continue;
                float g = g_ha_h2d[c] + (d >= 4 ? 1.4142f : 1.0f) * VTILE;
                if (g < g_ha_h2d[n]) { g_ha_h2d[n] = g; vpush(n, g); }
            }
        }
        /* the target cannot be reached on the tile grid (behind a fence, inside a building): drive to the reachable
         * tile nearest to it instead (flood from the start), so the search does not burn its whole budget and the
         * driver does not plan again and again into the obstacle (Wine 2026-11-20, Boot Camp wire) */
        int st = (g_ha_sy / VTILE) * VGRID + g_ha_sx / VTILE;
        float sd = 1e9f;                /* the start tile itself may be blocked (against a fence): its neighbours too */
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++) {
                int n = st + oy * VGRID + ox;
                if (n >= 0 && n < VGRID * VGRID && g_ha_h2d[n] < sd) sd = g_ha_h2d[n];
            }
        if (sd >= 1e8f && !g_ha_retargeted) {
            static int q[VGRID * VGRID];
            int qh = 0, qt = 0, best = st;
            float bd = 1e30f;
            memset(g_vseen, 0, sizeof g_vseen);
            q[qt++] = st; g_vseen[st] = 1;
            while (qh < qt) {
                int c = q[qh++], cx = c % VGRID, cy = c / VGRID;
                float ex = cx * VTILE + 8 - g_ha_tx, ey = cy * VTILE + 8 - g_ha_ty, e = ex * ex + ey * ey;
                if (e < bd) { bd = e; best = c; }
                for (int d = 0; d < 8; d++) {
                    int nx = cx + VDX[d], ny = cy + VDY[d];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= hh) continue;
                    int n = ny * VGRID + nx;
                    if (g_vseen[n] || vtile_bad(nx, ny)) continue;
                    g_vseen[n] = 1; q[qt++] = n;
                }
            }
            g_ha_retargeted = 1;
            mlog("path: vehicle target %.0f,%.0f not reachable - nearest reachable %d,%d", g_ha_tx, g_ha_ty, (best % VGRID) * VTILE + 8, (best / VGRID) * VTILE + 8);
            g_ha_tx = (float)((best % VGRID) * VTILE + 8); g_ha_ty = (float)((best / VGRID) * VTILE + 8);
            for (int i = 0; i < VGRID * VGRID; i++) g_ha_h2d[i] = 1e9f;
            memset(g_vclosed, 0, sizeof g_vclosed);
            g_vnheap = 0;
            g_ha_h2d[best] = 0; vpush(best, 0);
            return 0;                   /* the distances again, next slice */
        }
        if (!ha_start()) { g_ha_running = 0; return -1; }
    }
    float best_h = ha_heur(g_ha[g_ha_best].x, g_ha[g_ha_best].y);
    int iter = 0;
    while (g_ha_nheap > 0) {
        if ((++iter & 31) == 0) {
            QueryPerformanceCounter(&t);
            if ((double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart > budget_ms) return 0;
        }
        int c = ha_pop();
        HaNode cn = g_ha[c];
        int key = ha_key(cn.x, cn.y, cn.h);
        if (g_ha_closed_bits[key >> 3] & (1 << (key & 7))) continue;
        g_ha_closed_bits[key >> 3] |= (BYTE)(1 << (key & 7));
        g_ha_expanded++;
        float dx = cn.x - g_ha_tx, dy = cn.y - g_ha_ty;
        if (dx * dx + dy * dy <= HA_GOAL * HA_GOAL) { g_ha_best = c; g_ha_running = 0; return 1; }
        float hb = ha_heur(cn.x, cn.y);
        if (hb < best_h) { best_h = hb; g_ha_best = c; }
        if (g_ha_n >= HA_MAX_NODES - 16) break;
        for (int dir = 1; dir >= -1; dir -= 2)
            for (int dh = -1; dh <= 1; dh++) {
                float x = cn.x, y = cn.y; int ok = 1;
                for (int k = 1; k <= HA_SUBSTEPS && ok; k++) {
                    /* heading in the middle of this part (for the move) and at its end (for the footprint) */
                    int hm = (cn.h + (int)lroundf((float)dh * (2 * k - 1) / (2 * HA_SUBSTEPS))) & (HA_HEADS - 1);
                    int hk = (cn.h + (int)lroundf((float)dh * k / HA_SUBSTEPS)) & (HA_HEADS - 1);
                    x += (float)(dir * g_ha_dx[hm] * g_ha_step / HA_SUBSTEPS);
                    y += (float)(dir * g_ha_dy[hm] * g_ha_step / HA_SUBSTEPS);
                    ok = cn.g < HA_ESCAPE ? ha_foot_escape(x, y, hk) : ha_foot_ok(x, y, hk);
                }
                if (!ok) continue;
                int nh = (cn.h + dh) & (HA_HEADS - 1);
                float g = cn.g + g_ha_step * (dir < 0 ? HA_REV_COST : 1.0f) + (dh ? HA_TURN_COST : 0)
                        + (cn.dir && cn.dir != dir ? HA_SWITCH_COST : 0);
                int n = ha_node_for(x, y, nh, g);
                if (n < 0) continue;
                HaNode *m = &g_ha[n];
                m->x = x; m->y = y; m->h = (signed char)nh; m->dir = (signed char)dir; m->parent = c; m->g = g;
                m->f = g + ha_heur(x, y);
                ha_push(n);
            }
        if (g_ha_spin)
            for (int dh = -1; dh <= 1; dh += 2) {
                int nh = (cn.h + dh) & (HA_HEADS - 1);
                if (!(cn.g < HA_ESCAPE ? ha_foot_escape(cn.x, cn.y, nh) : ha_foot_ok(cn.x, cn.y, nh))) continue;
                float g = cn.g + HA_SPIN_COST;
                int n = ha_node_for(cn.x, cn.y, nh, g);
                if (n < 0) continue;
                HaNode *m = &g_ha[n];
                m->x = cn.x; m->y = cn.y; m->h = (signed char)nh; m->dir = 0; m->parent = c; m->g = g;
                m->f = g + ha_heur(cn.x, cn.y);
                ha_push(n);
            }
    }
    g_ha_running = 0;
    return g_ha[g_ha_best].parent >= 0 ? 2 : -1;
}

/* The result: poses after the start up to the end (the first max of them). Returns the count. */
int path_vehicle_plan_result(VehPose *out, int max)
{
    int total = 0, nrev = 0, nspin = 0;
    for (int c = g_ha_best; c >= 0 && g_ha[c].parent >= 0; c = g_ha[c].parent) total++;
    int n = total > max ? max : total, skip = total - n, i = n;
    for (int c = g_ha_best; c >= 0 && g_ha[c].parent >= 0; c = g_ha[c].parent) {
        if (skip > 0) { skip--; continue; }
        i--;
        out[i].x = (short)lroundf(g_ha[c].x); out[i].y = (short)lroundf(g_ha[c].y);
        out[i].facing = (short)(g_ha[c].h * (256 / HA_HEADS)); out[i].dir = g_ha[c].dir;
        nrev += out[i].dir < 0; nspin += out[i].dir == 0;
    }
    mlog("path: vehicle plan (reverse) type %d -> %.0f,%.0f: %d poses (%d backward, %d on the spot), %d expanded, %d nodes, %d fence / bush tiles seen",
         g_ha_type, g_ha_tx, g_ha_ty, n, nrev, nspin, g_ha_expanded, g_ha_n, g_vcrush_used);
    return n;
}

/* Tiles under the footprint of vehicle type (VEH_TYPE - 1) at (x, y, facing) that block it by the game's own rule
 * (movement layer >= 15, or a blocking object as vtile_object - not the parked-vehicle layer: a vehicle standing
 * still is in it itself). vehicles.c: how deep a vehicle is wedged. */
int path_vehicle_overlap(int type, double x, double y, int facing, int *objects)
{
    const short *sz = (const short *)(intptr_t)(VEH_SIZES + 8 * (type >= 0 && type < 6 ? type : 0));
    int box[4]; for (int i = 0; i < 4; i++) box[i] = sz[i];
    if (!box[0] && !box[1]) { box[0] = 50; box[1] = 34; box[2] = 22; box[3] = 23; }
    double dx, dy; controls_facing_vector(facing, &dx, &dy);
    double px = -dy, py = dx;
    int reach = box[0]; for (int i = 1; i < 4; i++) if (box[i] > reach) reach = box[i];
    int w = TILES_W, h = TILES_H, bad = 0, obj = 0;
    if (objects) *objects = 0;
    if (!MOVE_LAYER || w <= 0 || h <= 0) return 0;
    for (int ty = (int)(y - reach) / VTILE; ty <= (int)(y + reach) / VTILE; ty++)
        for (int tx = (int)(x - reach) / VTILE; tx <= (int)(x + reach) / VTILE; tx++) {
            double cx = tx * VTILE + 8 - x, cy = ty * VTILE + 8 - y;
            double f = cx * dx + cy * dy, s = cx * px + cy * py;
            if (f > box[0] || f < -box[1] || s > box[3] || s < -box[2]) continue;
            if (tx < 0 || ty < 0 || tx >= w || ty >= h || MOVE_LAYER[ty * w + tx] >= 15) bad++;
            else if (vtile_object_now(tx, ty)) { bad++; obj++; }
        }
    if (objects) *objects = obj;
    return bad;
}

void path_regions_step(void) { PROF_BEGIN(t); path_regions_step_body(); PROF_END(t, PR_REGIONS); }   /* timed (trace) */

/* missionpath.c: the same straight-walk test as the click route (Sarge's width, every pixel) */
int path_line_free(int x0, int y0, int x1, int y1) { return line_free(x0, y0, x1, y1); }

/* timed (trace stage "path", 2026-11-21): the outermost search only (they call each other) */
static int g_path_depth;
#define PATH_TIMED(call) do { if (g_path_depth) return call; g_path_depth++; PROF_BEGIN(t); int r_ = call; PROF_END(t, PR_PATH); g_path_depth--; return r_; } while (0)
int path_find(int sx, int sy, int tx, int ty, POINT *out, int max) { PATH_TIMED(path_find_body(sx, sy, tx, ty, out, max)); }
int path_reachable(int sx, int sy, int tx, int ty, int budget_ms) { PATH_TIMED(path_reachable_body(sx, sy, tx, ty, budget_ms)); }
int path_find_vehicle(int type, int sx, int sy, int tx, int ty, POINT *out, int max) { PATH_TIMED(path_find_vehicle_body(type, sx, sy, tx, ty, out, max)); }
int path_find_vehicle_begin(int type, int sx, int sy, int tx, int ty, int max) { PATH_TIMED(path_find_vehicle_begin_body(type, sx, sy, tx, ty, max)); }
int path_find_vehicle_step(double budget_ms, POINT *out, int max) { PATH_TIMED(path_find_vehicle_step_body(budget_ms, out, max)); }
int path_vehicle_plan_step(double budget_ms) { PATH_TIMED(path_vehicle_plan_step_body(budget_ms)); }
