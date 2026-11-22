/* ============================================================================================
 * fences.c - Sarge breaks wooden fences in his way (2026-11-20, [gameplay] break_fences)
 *
 * User (mission 3: Sarge in a stone-walled yard, a jeep just outside its corner, the only gap closed by a small
 * wooden fence piece): "enter vehicle from here is still being blocked by the fence. the expectation is that the
 * fence will be broken". No game function is rewritten here; the game's own damage path breaks the fence.
 *
 * Breakable = an object (ITEM_TYPE + 1 = 7 / 8 / 9) of category FENCE (5, game table) with movement > 0, strength
 * (+0x1c, common.nStrength) > 0 (0 = indestructible / already destroyed: binary ObjectDamage does nothing
 * then) and no mission script attached (+0xc4 sobj: scripted objects take ObjectWithScriptDamageFunction). NOT:
 * bushes (BUSH 1: BushObjectDamageFunction is empty - a bush cannot be destroyed), walls (6), gates (20 / 21),
 * sandbag rings (ROCK 0), fallen trees (4), buildings.
 *
 *   fences_only_at(x, y, strict)  the point is blocked for Sarge (strict = controls_blocked_at, the planners'
 *                                 rule with the tile margin; else the game's isSargeUntrafficable) and would be
 *                                 free without the breakable fences there. The strict tile test (UntrafficableUnit
 *                                 0x42a230: movement layer >= 15 or a parked vehicle) counts the fence movement found
 *                                 at the tile's centre / corners / the point off the movement layer.
 *                                 pathfind.c (click route, reachability, regions) and missionpath.c (walking legs)
 *                                 treat such cells as passable at an extra cost (a free way of similar length wins).
 *   fences_walk(u, facing, route) controls.c, every frame Sarge is walked in modern controls (before the turn to a
 *                                 free direction): along the WANTED direction, the first point Sarge's own rule blocks
 *                                 within reach. Fence-only -> the fence objects there are broken: route walking
 *                                 (click / walk to a vehicle) at once within ROUTE_REACH px, pushing with movement
 *                                 input (W A S D, hold right) after PUSH_MS against it within PUSH_REACH px.
 *   fences_frame()                gameplay_frame: the same push rule along Sarge's facing for any other walking
 *                                 input (classic controls, test aids) - skipped while fences_walk is active.
 *
 * Breaking = the game's ItemDamage 0x431f60 (__fastcall ecx item, edx damage; stack DMGTYPE_RUNOVER 0 - what a
 * vehicle running a fence over passes (damageItemsInMask 0x418246) -, SPOINT *at, UID Sarge) with the piece's
 * strength + |armor| + 1: GenericObjectDamageFunction steps to the destroyed sprite / movement (Dmg_SetObjectDamage ->
 * AddObjectToMovementLayer), plays the destroyed sound and raises the item's damage / died triggers; pieces linked as
 * sub-objects take the same damage as DMGTYPE_STRUCTURAL (the game's own side effect, as when shot or run over).
 * Crush: the destroyed state may carry crush damage (AddObjectToMovementLayer -> ItemDamage on soldiers at changed
 * tiles, call 0x40cff3 = gameplay.c crush_stub); while a fence is broken here, crush_allowed treats it like
 * fix_squash=1 (a barricade changing shape never crushes Sarge or soldiers), whatever fix_squash says.
 * ============================================================================================ */
#include "common.h"
#include <math.h>
#include <stdio.h>

#define SARGE              (*(BYTE **)0x697520)
#define SARGE_X(u)         (*(short *)((u) + 0x04))
#define SARGE_Y(u)         (*(short *)((u) + 0x06))
#define SARGE_HEALTH(u)    (*(short *)((u) + 0x1c))
#define SARGE_FACING(u)    (*(short *)((u) + 0x2c6))
#define SARGE_AVATAR(u)    (*(BYTE **)((u) + 0x268))
#define SARGE_NPC(u)       (*(BYTE *)((u) + 0x26c))
#define SARGE_VEHICLE(u)   (*(int *)((u) + 0x2d0))
#define INPUT              (*(volatile DWORD *)0x6a547c)
#define INPUT_FORWARD      0x02
#define INPUT_BACK         0x04
#define FACINGS            256

typedef BYTE *(__fastcall *ObjectsAtFn)(short *point);
#define OBJECTS_AT         ((ObjectsAtFn)0x42fca0)      /* GetObjectsIntersectPoint: ITEM list, next at +0x60 */
#define ITEM_NEXT(it)      (*(BYTE **)((it) + 0x60))
#define ITEM_TYPE1(it)     ((int)((*(DWORD *)(it) >> 22) & 0xf))   /* ITEM_TYPE + 1: 3 vehicle, 7/8/9 objects, 10 husk */
#define OBJ_STRENGTH(it)   (*(short *)((it) + 0x1c))
#define OBJ_CATEGORY(it)   (*(short *)((it) + 0x68))
#define OBJ_NUMBER(it)     (*(short *)((it) + 0x6a))
#define OBJ_MOVEMENT(it)   (*(signed char *)((it) + 0x6f))
#define OBJ_ARMOR(it)      (*(signed char *)((it) + 0x79))
#define OBJ_SCRIPT(it)     (*(void **)((it) + 0xc4))
#define CATEGORY_FENCE     5
#define TILE_TRIGGERS      (*(BYTE **)0x4e3560)         /* the_map.triggers */
#define TILE_UNTRAFFICABLE_BIT 0x40
#define MOVEMENT_LAYER     (*(signed char **)0x4e3554)  /* the_map.movement_layer, a byte per 16 px tile */
#define VEHICLE_LAYER      (*(BYTE **)0x507f78)         /* parked vehicles per tile */
#define MAP_TILES_W        (*(unsigned short *)0x4e368c)
#define MOVEMENT_BLOCKS    15

typedef void (__fastcall *ItemDamageFn)(void *item, int damage, int type, short *at, DWORD from_uid);
#define ITEM_DAMAGE        ((ItemDamageFn)0x431f60)
#define DMGTYPE_RUNOVER    0

#define ROUTE_REACH        16      /* px: a route leg breaks the fence this far ahead (before the 14 px turn-away look) */
#define PUSH_REACH         8       /* px: pushing breaks a fence this close */
#define PUSH_MS            350     /* pushing against the same fence this long breaks it */

int g_fences_breaking;             /* gameplay.c crush_allowed: a fence broken here is changing shape */

static int breakable(BYTE *it)
{
    int t = ITEM_TYPE1(it);
    return (t == 7 || t == 8 || t == 9) && OBJ_CATEGORY(it) == CATEGORY_FENCE && OBJ_MOVEMENT(it) > 0 &&
           OBJ_STRENGTH(it) > 0 && !OBJ_SCRIPT(it);
}

/* movement of the breakable fences at (x, y) not yet in seen[] */
static int fence_movement(int x, int y, BYTE **seen, int *nseen, int cap)
{
    short p[2] = { (short)x, (short)y };
    int sum = 0;
    for (BYTE *it = OBJECTS_AT(p); it; it = ITEM_NEXT(it)) {
        if (!breakable(it)) continue;
        int k = 0;
        while (k < *nseen && seen[k] != it) k++;
        if (k < *nseen) continue;
        if (*nseen < cap) seen[(*nseen)++] = it;
        sum += OBJ_MOVEMENT(it);
    }
    return sum;
}

int fences_only_at(int x, int y, int strict)
{
    if (!g_cfg.break_fences || !MAP_TILES_W) return 0;
    const int *sb = (const int *)0x4e3014;                     /* the_map.sargeboundrect: Sarge never leaves it */
    if (sb[2] > sb[0] && sb[3] > sb[1] && (x < sb[0] || y < sb[1] || x >= sb[2] || y >= sb[3])) return 0;
    int tile = (y >> 4) * MAP_TILES_W + (x >> 4), rest = 0, fence = 0, tile_fence = 0;
    if (strict) {
        if (VEHICLE_LAYER && VEHICLE_LAYER[tile]) return 0;
        if (!MOVEMENT_LAYER) return 0;
        if (MOVEMENT_LAYER[tile] >= MOVEMENT_BLOCKS) {         /* the tile itself: free without its fences? */
            static const int PX[5] = { 8, 3, 13, 3, 13 }, PY[5] = { 8, 3, 3, 13, 13 };
            BYTE *seen[8]; int nseen = 0, f = 0;
            for (int k = 0; k < 5; k++) f += fence_movement((x & ~15) + PX[k], (y & ~15) + PY[k], seen, &nseen, 8);
            f += fence_movement(x, y, seen, &nseen, 8);
            if (!f || MOVEMENT_LAYER[tile] - f >= MOVEMENT_BLOCKS) return 0;
            tile_fence = 1;
        }
    } else if (TILE_TRIGGERS && (TILE_TRIGGERS[tile] & TILE_UNTRAFFICABLE_BIT)) return 0;
    short p[2] = { (short)x, (short)y };
    for (BYTE *it = OBJECTS_AT(p); it; it = ITEM_NEXT(it)) {
        int t = ITEM_TYPE1(it);
        if (t == 7 || t == 8 || t == 9) { if (breakable(it)) fence += OBJ_MOVEMENT(it); else rest += OBJ_MOVEMENT(it); }
        else if (t == 3 || t == 10) rest += 16;
    }
    if (rest >= MOVEMENT_BLOCKS) return 0;
    return tile_fence || (fence > 0 && rest + fence >= MOVEMENT_BLOCKS);
}

/* Break every breakable fence piece whose picture holds (x, y). Returns how many. */
static int break_at(BYTE *u, int x, int y, const char *why)
{
    BYTE *list[8]; int n = 0;
    short p[2] = { (short)x, (short)y };
    for (BYTE *it = OBJECTS_AT(p); it && n < 8; it = ITEM_NEXT(it))
        if (breakable(it)) list[n++] = it;                      /* collected first: breaking changes the lists */
    for (int i = 0; i < n; i++) {
        BYTE *it = list[i];
        if (!breakable(it)) continue;                           /* broken with an earlier one (sub-objects) */
        int hp = OBJ_STRENGTH(it), mv = OBJ_MOVEMENT(it), dmg = hp + abs(OBJ_ARMOR(it)) + 1;
        g_fences_breaking = 1;
        ITEM_DAMAGE(it, dmg, DMGTYPE_RUNOVER, p, *(DWORD *)u);
        g_fences_breaking = 0;
        mlog("fences: Sarge at %d,%d broke fence %p (number %d, strength %d -> %d, movement %d -> %d) at %d,%d (%s)%s",
             SARGE_X(u), SARGE_Y(u), it, OBJ_NUMBER(it), hp, OBJ_STRENGTH(it), mv, OBJ_MOVEMENT(it), x, y, why,
             controls_sarge_blocked_at(x, y) ? " - the point is still blocked" : "");
    }
    return n;
}

static void dir_vec(int f, double *dx, double *dy)
{
    controls_facing_vector(f & (FACINGS - 1), dx, dy);
    double l = sqrt(*dx * *dx + *dy * *dy);
    if (l < 1e-6) { double a = (double)(f & (FACINGS - 1)) * 2.0 * 3.14159265358979 / FACINGS; *dx = sin(a); *dy = -cos(a); return; }
    *dx /= l; *dy /= l;
}

/* The first point along facing f (centre line and 3 px either side) that Sarge's own rule blocks within reach,
 * if it is blocked by breakable fences only. */
static int fence_ahead(int x, int y, int f, int reach, int *fx, int *fy)
{
    double dx, dy; dir_vec(f, &dx, &dy);
    static const int SIDE[3] = { 0, 3, -3 };
    for (int s = 0; s < 3; s++)
        for (int i = 1; i <= reach; i++) {
            int px = x + (int)lround(dx * i - dy * SIDE[s]), py = y + (int)lround(dy * i + dx * SIDE[s]);
            if (!controls_sarge_blocked_at(px, py)) continue;
            if (!fences_only_at(px, py, 0)) break;              /* something else first: a wall, a vehicle */
            *fx = px; *fy = py;
            return 1;
        }
    return 0;
}

static int sarge_on_foot(BYTE *u)
{
    return g_cfg.break_fences && u && SARGE_AVATAR(u) && !SARGE_NPC(u) && !SARGE_VEHICLE(u) && SARGE_HEALTH(u) > 0 &&
           SARGE_X(u) > 0 && layout_view_is_gameplay() && !controls_sarge_blocked_at(SARGE_X(u), SARGE_Y(u));
}

static struct { int fx, fy; DWORD since, walk_at; } F;

static void push(BYTE *u, int f, int route, DWORD now)
{
    int fx, fy;
    if (!fence_ahead(SARGE_X(u), SARGE_Y(u), f, route ? ROUTE_REACH : PUSH_REACH, &fx, &fy)) { F.since = 0; return; }
    if (route) {                                   /* only a fence the planned route goes through (Wine 2026-11-20: a route round a
                                                    * fence brushed it for 350 ms and broke it as "pushed") */
        if (path_route_fence_near(fx, fy, 12)) break_at(u, fx, fy, "on his route");
        F.since = 0; return;
    }
    if (!F.since || abs(fx - F.fx) + abs(fy - F.fy) > 12) { F.since = now ? now : 1; F.fx = fx; F.fy = fy; return; }
    if (now - F.since >= PUSH_MS) { break_at(u, fx, fy, "pushed against it"); F.since = 0; }
}

void fences_walk(BYTE *u, int facing, int route)
{
    if (!sarge_on_foot(u)) { F.since = 0; return; }
    DWORD now = clock_game_ms();
    F.walk_at = now ? now : 1;
    push(u, facing, route, now);
}

void fences_dump(void);
void fences_frame(void)
{
    BYTE *u = SARGE;
    DWORD now = clock_game_ms();
    fences_dump();
    if (F.walk_at && now - F.walk_at < 100) return;            /* modern controls feed fences_walk */
    F.walk_at = 0;
    if (!sarge_on_foot(u)) { F.since = 0; return; }
    DWORD in = INPUT;
    if (!(in & (INPUT_FORWARD | INPUT_BACK))) { F.since = 0; return; }
    push(u, SARGE_FACING(u) + ((in & INPUT_FORWARD) ? 0 : FACINGS / 2), 0, now);
}

/* log >= 2: once per mission (2 s in), every FENCE object on the map (16 px scan) with what decides "breakable" -
 * to find test spots and to see which fences the rule takes. */
void fences_dump(void)
{
    static BYTE *dumped; static DWORD seen_at;
    BYTE *u = SARGE;
    if (g_cfg.log < 2 || !u || u == dumped || !SARGE_AVATAR(u) || SARGE_X(u) <= 0 || !layout_view_is_gameplay() || !MAP_TILES_W) return;
    DWORD now = clock_game_ms();
    if (!seen_at || now < seen_at) { seen_at = now ? now : 1; return; }
    if (now - seen_at < 2000) return;
    dumped = u; seen_at = 0;
    static BYTE *seen[400]; int n = 0, nb = 0, w = MAP_TILES_W * 16;
    for (int y = 8; y < 4096 && n < 400; y += 16)
        for (int x = 8; x < w && n < 400; x += 16) {
            short p[2] = { (short)x, (short)y };
            for (BYTE *it = OBJECTS_AT(p); it && n < 400; it = ITEM_NEXT(it)) {
                int t = ITEM_TYPE1(it), k = 0;
                if ((t != 7 && t != 8 && t != 9) || OBJ_CATEGORY(it) != CATEGORY_FENCE) continue;
                while (k < n && seen[k] != it) k++;
                if (k < n) continue;
                seen[n++] = it; nb += breakable(it);
                if (n <= 150)
                    mlog("fences: fence %p number %d at %d,%d strength %d armor %d movement %d script %p -> %s", it, OBJ_NUMBER(it),
                         *(short *)(it + 4), *(short *)(it + 6), OBJ_STRENGTH(it), OBJ_ARMOR(it), OBJ_MOVEMENT(it), OBJ_SCRIPT(it),
                         breakable(it) ? "breakable" : "not breakable");
            }
        }
    mlog("fences: %d fence objects on the map, %d breakable (Sarge at %d,%d)", n, nb, SARGE_X(u), SARGE_Y(u));
}

/* savestate.c: a fence push in progress */
void fences_savestate(void) { SAVESTATE(F); }
