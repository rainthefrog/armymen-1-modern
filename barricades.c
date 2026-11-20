/* ============================================================================================
 * barricades.c - Sarge never stays stuck on a barricade (2026-11-20)
 *
 * User: "sarge can still get stuck on barricades - add some robust checking to make sure can't get stuck on
 * barricades". No game function is rewritten here: barricades_frame() runs once per gameplay frame (called from
 * gameplay_frame, i.e. from the keyboard-dispatcher wrap in controls.c, before the game's SargeUpdate 0x446c70)
 * and watches Sarge's position. [controls] sarge_unstick=1 (F10 panel "Sarge: get unstuck").
 *
 * The game's walking rule for Sarge (SargeUpdate 0x446c70, binary SargeUpdate; walk test
 * isSargeUntrafficable 0x446ad0 = controls_sarge_blocked_at): a step is refused when the next animation frame's
 * point is blocked and four slide directions (+-16, +-32 of 256) are blocked too; standing on a blocked point
 * (bIsSargeIllegal) lets him walk freely - so once inside a barricade's mask he can walk straight through it.
 * Barricades are objects (sandbag rings = ROCK, fences, walls, gates, fallen trees), tested by their sprite masks
 * (GetObjectsIntersectPoint 0x42fca0), which have holes and ragged edges: a 2-point look-ahead could call a
 * direction free that the game refuses at the first frame, and Sarge stood still with input held.
 *
 * Three cases, all only on foot, alive, not in a vehicle:
 *   1. INSIDE: Sarge's point is blocked and the blocking is barricades only (without the barricade objects at
 *      that point the rest - ground trigger 16, buildings, vehicles - would be < 15; building interiors, where the
 *      game lets him walk on blocked tiles, are left alone). He is moved out to the nearest roomy free point.
 *   2. WEDGED: walking input held, his position has not changed for STUCK_MS, and a direction within +-45 degrees
 *      of the input (the game's own slide range) is free along a whole 12 px segment: the game refuses what its own rule allows a bit further
 *      on (mask hole / ragged edge). He is moved along that direction (whole free segment).
 *   3. BOXED: input held, no movement, and no direction at all is free: moved to the nearest roomy point.
 * Pushing into a wall (no free direction within +-45 degrees, but free behind him) is NOT stuck: nothing
 * is done, he can turn round.
 *
 * Never through a barricade: every move goes along a straight segment tested pixel by pixel with the game's own
 * rule; once a point of the segment is free, no later point may be blocked (from a free spot = a fully free
 * segment). Moves are at most NUDGE_PX per frame through the game's own placement (FUN_00448a90, the
 * "Telekinetic" cheat routine: position, avatar, map layers), re-tested every frame.
 * ============================================================================================ */
#include "common.h"
#include <math.h>
#include <stdio.h>

#define SARGE              (*(BYTE **)0x697520)
#define SARGE_X(u)         (*(short *)((u) + 0x04))
#define SARGE_Y(u)         (*(short *)((u) + 0x06))
#define SARGE_HEALTH(u)    (*(short *)((u) + 0x1c))
#define SARGE_ACTION(u)    (*(int *)((u) + 0x68))      /* pendingAction: 15..17 dying, 20 die-explode, 22 inactive */
#define SARGE_FACING(u)    (*(short *)((u) + 0x2c6))
#define SARGE_AVATAR(u)    (*(BYTE **)((u) + 0x268))
#define SARGE_NPC(u)       (*(BYTE *)((u) + 0x26c))
#define SARGE_VEHICLE(u)   (*(int *)((u) + 0x2d0))
#define INPUT              (*(volatile DWORD *)0x6a547c)   /* the player's input word */
#define INPUT_FORWARD      0x02
#define INPUT_BACK         0x04
#define FACINGS            256
#define ACTION_RUN         0       /* pendingAction values (exe; binary ACTION): RUN, WALK, WALK_BACKWARDS, SHUFFLE */
#define ACTION_SHUFFLE     3
#define ACTION_STOP        19      /* AVATAR_STOP_IMMEDIATELY: what SargeUpdate sets when it refuses a step (0x446c70) */

typedef void (__fastcall *PlaceUnitFn)(BYTE *unit, int x, int y);
#define PLACE_UNIT         ((PlaceUnitFn)0x448a90)      /* SargeTeleport (the Telekinetic cheat's mover) */
typedef BYTE *(__fastcall *ObjectsAtFn)(short *point);
#define OBJECTS_AT         ((ObjectsAtFn)0x42fca0)      /* GetObjectsIntersectPoint: ITEM list, next at +0x60 */
#define ITEM_NEXT(it)      (*(BYTE **)((it) + 0x60))
#define ITEM_TYPE1(it)     ((int)((*(DWORD *)(it) >> 22) & 0xf))   /* ITEM_TYPE + 1: 3 vehicle, 7/8/9 objects, 10 husk */
#define OBJ_CATEGORY(it)   (*(short *)((it) + 0x68))    /* OBJECTSTRUCT category (game table OBJECT_TYPES) */
#define OBJ_MOVEMENT(it)   (*(signed char *)((it) + 0x6f))
#define TILE_TRIGGERS      (*(BYTE **)0x4e3560)         /* the_map.triggers */
#define TILE_UNTRAFFICABLE_BIT 0x40
#define MAP_TILES_W        (*(unsigned short *)0x4e368c)
#define MOVEMENT_BLOCKS    15

#define STUCK_MS           600     /* input held and no movement this long = stuck */
#define INSIDE_MS          150     /* on a barricade-only blocked point this long = move out */
#define LOOK_PX            12      /* a direction is free if this whole segment is free */
#define SEARCH_PX          32      /* nearest free point: searched this far */
#define ROOM_MIN           4       /* a target point needs this many free directions (of 16) */
#define NUDGE_PX           2       /* at most this far per frame */
#define NUDGE_FRAMES       24      /* a nudge ends after this many frames */

/* Barricade categories (game table): ROCK 0 (sandbag rings are ROCK, height 5), FALLEN_TREE 4, FENCE 5, WALL 6,
 * GATE 20, WALL_GATE 21. Shared with gameplay.c (no squash under them). */
int barricade_category(int c) { return c == 0 || c == 4 || c == 5 || c == 6 || c == 20 || c == 21; }

static int blocked(int x, int y) { return controls_sarge_blocked_at(x, y); }

/* Blocked only because of barricade objects at the point (the same sum as isSargeUntrafficable without them). */
static int barricade_only(int x, int y)
{
    const int *sb = (const int *)0x4e3014;                      /* the_map.sargeboundrect */
    if (sb[2] > sb[0] && sb[3] > sb[1] && (x < sb[0] || y < sb[1] || x >= sb[2] || y >= sb[3])) return 0;
    int tile = (y >> 4) * MAP_TILES_W + (x >> 4), rest = 0, barr = 0;
    if (TILE_TRIGGERS && (TILE_TRIGGERS[tile] & TILE_UNTRAFFICABLE_BIT)) rest = 16;
    short p[2] = { (short)x, (short)y };
    for (BYTE *it = OBJECTS_AT(p); it; it = ITEM_NEXT(it)) {
        int t = ITEM_TYPE1(it);
        if (t == 7 || t == 8 || t == 9) {
            if (barricade_category(OBJ_CATEGORY(it))) barr += OBJ_MOVEMENT(it);
            else rest += OBJ_MOVEMENT(it);
        } else if (t == 3 || t == 10) rest += 16;
    }
    return barr > 0 && rest < MOVEMENT_BLOCKS && rest + barr >= MOVEMENT_BLOCKS;
}

/* The straight segment (x0,y0) -> (x1,y1), pixel by pixel: the end is free and, once a point is free, every later
 * point is free too (out of a barricade, never into or through one). */
static int segment_ok(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0, len = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    int free_seen = !blocked(x0, y0);
    for (int i = 1; i <= len; i++) {
        int b = blocked(x0 + dx * i / len, y0 + dy * i / len);
        if (b && free_seen) return 0;
        if (!b) free_seen = 1;
    }
    return free_seen;
}

static void dir_vec(int f, double *dx, double *dy)
{
    double a = (double)(f & (FACINGS - 1)) * 2.0 * 3.14159265358979 / FACINGS;
    controls_facing_vector(f, dx, dy);
    double l = sqrt(*dx * *dx + *dy * *dy);
    if (l < 1e-6) { *dx = sin(a); *dy = -cos(a); return; }   /* facing table not ready */
    *dx /= l; *dy /= l;
}

/* A direction is free if the whole LOOK_PX segment is free (pixel by pixel). */
static int dir_free(int x, int y, int f)
{
    double dx, dy; dir_vec(f, &dx, &dy);
    for (int i = 1; i <= LOOK_PX; i++)
        if (blocked(x + (int)lround(dx * i), y + (int)lround(dy * i))) return 0;
    return 1;
}
static int room(int x, int y)
{
    int n = 0;
    for (int f = 0; f < FACINGS; f += FACINGS / 16) n += dir_free(x, y, f);
    return n;
}

/* The nearest roomy free point reachable by segment_ok, or 0. */
static int nearest_exit(int x, int y, int *ox, int *oy)
{
    for (int r = 2; r <= SEARCH_PX; r += 2)
        for (int f = 0; f < FACINGS; f += FACINGS / 32) {
            double dx, dy; dir_vec(f, &dx, &dy);
            int tx = x + (int)lround(dx * r), ty = y + (int)lround(dy * r);
            if (blocked(tx, ty) || room(tx, ty) < ROOM_MIN || !segment_ok(x, y, tx, ty)) continue;
            *ox = tx; *oy = ty;
            return 1;
        }
    return 0;
}

/* Something blocks within NEAR_PX ahead (+-45 degrees): only then can a refused step be a barricade problem. */
#define NEAR_PX 20
static int obstacle_ahead(int x, int y, int f)
{
    for (int k = -32; k <= 32; k += 8) {
        double dx, dy; dir_vec(f + k, &dx, &dy);
        for (int i = 1; i <= NEAR_PX; i++)
            if (blocked(x + (int)lround(dx * i), y + (int)lround(dy * i))) return 1;
    }
    return 0;
}

/* The free direction nearest to f within +-32 (45 degrees: the range of the game's own slide tries), or -1. */
static int free_near(int x, int y, int f)
{
    for (int k = 0; k <= 32; k += 8)
        for (int side = 1; side >= -1; side -= 2) {
            int g = (f + side * k) & (FACINGS - 1);
            if (dir_free(x, y, g)) return g;
            if (!k) break;
        }
    return -1;
}

static struct {
    BYTE *who;
    int ax, ay; DWORD anchor_at;        /* position at the start of the current no-movement stretch */
    DWORD inside_since;
    int nudging, tx, ty, frames;        /* moving him to tx, ty */
    int px, py;                         /* where the last nudge put him */
    int moves;                          /* nudges this mission (log) */
} B;

void barricades_reset(void) { memset(&B, 0, sizeof B); }

static void start_nudge(BYTE *u, int tx, int ty, const char *why)
{
    B.nudging = 1; B.tx = tx; B.ty = ty; B.frames = 0; B.moves++; B.px = SARGE_X(u); B.py = SARGE_Y(u);
    mlog("barricades: Sarge %s at %d,%d -> moving to %d,%d (room %d -> %d, facing %d, input %02lx, action %d)", why, SARGE_X(u), SARGE_Y(u),
         tx, ty, room(SARGE_X(u), SARGE_Y(u)), room(tx, ty), SARGE_FACING(u) & (FACINGS - 1), (unsigned long)(INPUT & 0xff), SARGE_ACTION(u));
}

/* log=3: the blocking objects (movement > 0) within 480 px of Sarge, once per Sarge (to stage tests: [debug] teleport_to). */
static void dump_barricades(BYTE *u)
{
    BYTE *seen[64]; int n = 0;
    for (int y = SARGE_Y(u) - 480; y <= SARGE_Y(u) + 480; y += 8)
        for (int x = SARGE_X(u) - 480; x <= SARGE_X(u) + 480; x += 8) {
            if (x < 0 || y < 0) continue;
            short p[2] = { (short)x, (short)y };
            for (BYTE *it = OBJECTS_AT(p); it && n < 64; it = ITEM_NEXT(it)) {
                int t = ITEM_TYPE1(it), k = 0;
                if ((t != 7 && t != 8 && t != 9) || OBJ_MOVEMENT(it) <= 0) continue;
                while (k < n && seen[k] != it) k++;
                if (k < n) continue;
                seen[n++] = it;
                mlog("barricades: object %p category %d number %d at %d,%d movement %d (first seen at %d,%d)", it, OBJ_CATEGORY(it),
                     *(short *)(it + 0x6a), *(short *)(it + 4), *(short *)(it + 6), OBJ_MOVEMENT(it), x, y);
            }
        }
}

/* [debug] barricade_test = x,y[,tx,ty] (testing; the container rig's key presses are too unreliable for this):
 * 2 s into the mission Sarge is made invulnerable (0x69752c), placed at x,y with the game's placement and, with
 * tx,ty, walks toward that point for 7 s (input word FORWARD + facing set here, like modern controls do); his
 * position is logged every 250 ms. */
#define INVULNERABLE (*(volatile BYTE *)0x69752c)
static void staged_test(BYTE *u, DWORD now)
{
    static int phase, x, y, tx, ty, has_t; static DWORD t0, logged;
    if (phase == 0) {
        char s[64] = "";
        GetPrivateProfileStringA("debug", "barricade_test", "", s, sizeof s, mod_ini_path());
        int n = sscanf(s, "%d,%d,%d,%d", &x, &y, &tx, &ty);
        if (n < 2) { phase = 9; return; }
        has_t = n >= 4; phase = 1; t0 = now;
        return;
    }
    if (phase == 1 && now - t0 > 2000) {
        INVULNERABLE = 1;
        PLACE_UNIT(u, x, y);
        mlog("barricades: test: Sarge placed at %d,%d -> %d,%d%s", x, y, SARGE_X(u), SARGE_Y(u), has_t ? ", walking" : "");
        /* [debug] barricade_crush = x,y: the object at that point takes a big explosion (the game's ItemDamage 0x431f60,
         * DMGTYPE_OTHER_EXPLOSION 0x45): its destroyed state is added to the movement layer (crush path 0x40ce70) */
        char c[32] = ""; int cx, cy;
        GetPrivateProfileStringA("debug", "barricade_crush", "", c, sizeof c, mod_ini_path());
        if (sscanf(c, "%d,%d", &cx, &cy) == 2) {
            short p[2] = { (short)cx, (short)cy };
            for (BYTE *it = OBJECTS_AT(p); it; it = ITEM_NEXT(it)) {
                int t = ITEM_TYPE1(it);
                if (t != 7 && t != 8 && t != 9) continue;
                typedef void (__fastcall *ItemDamageFn)(void *item, int damage, int type, short *from, DWORD uid);
                mlog("barricades: test: object %p category %d health %d -> explosion", it, OBJ_CATEGORY(it), *(short *)(it + 0x1c));
                ((ItemDamageFn)0x431f60)(it, 5000, 0x45, p, 0);
                mlog("barricades: test: object %p health now %d", it, *(short *)(it + 0x1c));
                break;
            }
        }
        phase = 2; t0 = now;
        return;
    }
    if (phase == 2) {
        if (now - t0 > 7000) { phase = 9; INPUT &= ~INPUT_FORWARD; mlog("barricades: test: done at %d,%d", SARGE_X(u), SARGE_Y(u)); return; }
        if (has_t && !B.nudging) {
            double vx = tx - SARGE_X(u), vy = ty - SARGE_Y(u), best = -2; int bf = 0;
            for (int f = 0; f < FACINGS; f++) {
                double dx, dy; dir_vec(f, &dx, &dy);
                double d = (dx * vx + dy * vy) / (sqrt(vx * vx + vy * vy) + 1e-9);
                if (d > best) { best = d; bf = f; }
            }
            *(short *)(u + 0x2c6) = (short)bf; *(int *)(u + 0x88) = bf; *(int *)(u + 0x74) = bf;
            INPUT |= INPUT_FORWARD;
        }
        if (now - logged >= 250) { logged = now; mlog("barricades: test: Sarge %d,%d facing %d blocked %d room %d", SARGE_X(u), SARGE_Y(u),
                                                      SARGE_FACING(u) & (FACINGS - 1), blocked(SARGE_X(u), SARGE_Y(u)), room(SARGE_X(u), SARGE_Y(u))); }
    }
}

void barricades_frame(void)
{
    BYTE *u = SARGE;
    DWORD now = clock_game_ms();
    static BYTE *dumped; static DWORD seen_at;
    if (g_cfg.log >= 3 && u && u != dumped && SARGE_AVATAR(u) && SARGE_X(u) > 0 && layout_view_is_gameplay()) {
        if (!seen_at) seen_at = now ? now : 1;
        else if (now - seen_at > 2000) { dumped = u; dump_barricades(u); }
    }
    if (g_cfg.cheat_keys && u && SARGE_AVATAR(u) && SARGE_X(u) > 0 && layout_view_is_gameplay()) staged_test(u, now);
    if (!g_cfg.sarge_unstick || !u || !SARGE_AVATAR(u) || SARGE_NPC(u) || SARGE_VEHICLE(u) || SARGE_HEALTH(u) <= 0 ||
        (SARGE_ACTION(u) >= 15 && SARGE_ACTION(u) <= 17) || SARGE_ACTION(u) >= 20 || !layout_view_is_gameplay()) {
        B.nudging = 0; B.anchor_at = 0; B.inside_since = 0;
        if (u != B.who) { B.who = u; B.moves = 0; }
        return;
    }
    if (u != B.who) { barricades_reset(); B.who = u; }
    int x = SARGE_X(u), y = SARGE_Y(u);
    if (g_cfg.log >= 2) {                                         /* a jump (teleport, placement): the state there */
        static int lx, ly;
        if (abs(x - lx) + abs(y - ly) > 24)
            mlog("barricades: Sarge at %d,%d: blocked %d, barricades only %d, room %d", x, y, blocked(x, y), barricade_only(x, y), room(x, y));
        lx = x; ly = y;
    }

    if (B.nudging && (x != B.px || y != B.py) && !blocked(x, y)) { B.nudging = 0; B.anchor_at = 0; }   /* he walks again by himself */
    if (B.nudging) {
        int dx = B.tx - x, dy = B.ty - y, d = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (!d || ++B.frames > NUDGE_FRAMES) { B.nudging = 0; B.anchor_at = 0; return; }
        int sx = d > NUDGE_PX ? x + dx * NUDGE_PX / d : B.tx, sy = d > NUDGE_PX ? y + dy * NUDGE_PX / d : B.ty;
        if (!segment_ok(x, y, B.tx, B.ty)) {                     /* still on a path that never enters a barricade */
            if (g_cfg.log >= 2) mlog("barricades: nudge step %d,%d -> %d,%d refused (would enter a barricade)", x, y, sx, sy);
            B.nudging = 0; B.anchor_at = 0; return;
        }
        PLACE_UNIT(u, sx, sy);
        B.px = SARGE_X(u); B.py = SARGE_Y(u);
        if (g_cfg.log >= 3) mlog("barricades: nudge %d,%d -> %d,%d (now %d,%d)", x, y, sx, sy, SARGE_X(u), SARGE_Y(u));
        if (SARGE_X(u) == x && SARGE_Y(u) == y) B.nudging = 0;      /* the game refused the placement */
        B.anchor_at = 0;
        return;
    }

    /* 1. inside a barricade's mask (the game would let him walk through it) */
    if (blocked(x, y)) {
        if (!barricade_only(x, y)) { B.inside_since = 0; B.anchor_at = 0; return; }   /* building floor, vehicle: the game's */
        if (!B.inside_since) B.inside_since = now ? now : 1;
        if (now - B.inside_since >= INSIDE_MS) {
            int tx, ty;
            if (nearest_exit(x, y, &tx, &ty)) start_nudge(u, tx, ty, "inside a barricade");
            else if (g_cfg.log >= 2) { static DWORD l; if (now - l > 2000) { l = now; mlog("barricades: Sarge inside a barricade at %d,%d, no exit within %d px", x, y, SEARCH_PX); } }
            B.inside_since = 0;
        }
        return;
    }
    B.inside_since = 0;

    /* 2. / 3. walking input held, but he does not move */
    DWORD in = INPUT;
    if (!(in & (INPUT_FORWARD | INPUT_BACK))) { B.anchor_at = 0; return; }
    if (!B.anchor_at || abs(x - B.ax) + abs(y - B.ay) >= 2) { B.ax = x; B.ay = y; B.anchor_at = now ? now : 1; return; }
    if (now - B.anchor_at < STUCK_MS) return;
    int act = SARGE_ACTION(u);
    if (!(act >= ACTION_RUN && act <= ACTION_SHUFFLE) && act != ACTION_STOP) {   /* standing up, firing, throwing...: not a refused step */
        if (g_cfg.log >= 3) mlog("barricades: Sarge not moving at %d,%d, action %d (not a walk) - left alone", x, y, act);
        B.anchor_at = 0; return;
    }
    B.anchor_at = 0;
    int want = (SARGE_FACING(u) + ((in & INPUT_FORWARD) ? 0 : FACINGS / 2)) & (FACINGS - 1);
    if (!obstacle_ahead(x, y, want)) {                             /* open ground: not a barricade problem (stance, firing...) */
        if (g_cfg.log >= 3) mlog("barricades: Sarge not moving at %d,%d in the open (facing %d, action %d) - left alone", x, y, want, SARGE_ACTION(u));
        return;
    }
    int g = free_near(x, y, want);
    if (g >= 0) {
        double dx, dy; dir_vec(g, &dx, &dy);
        int tx = x + (int)lround(dx * LOOK_PX / 2), ty = y + (int)lround(dy * LOOK_PX / 2);
        if (segment_ok(x, y, tx, ty)) { start_nudge(u, tx, ty, "wedged (the game refuses a free direction)"); return; }
    }
    if (room(x, y) == 0) {
        int tx, ty;
        if (nearest_exit(x, y, &tx, &ty)) { start_nudge(u, tx, ty, "boxed in"); return; }
    }
    static DWORD wall_logged;
    if (g_cfg.log >= 2 && now - wall_logged > 3000) wall_logged = now, mlog("barricades: Sarge not moving at %d,%d (facing %d, input %02lx): a wall ahead, room %d, action %d - left alone",
                             x, y, SARGE_FACING(u) & (FACINGS - 1), (unsigned long)(in & 0xff), room(x, y), SARGE_ACTION(u));
}
