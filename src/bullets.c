/* ============================================================================================
 * bullets.c - shoot over fences and walls, and past the top of buildings
 *
 * A projectile steps along its GROUND point (UpdateProjectile 0x435e20) and at each step asks
 * DoFloatCollisionAtPoint (0x435a10) what it hits: every object whose PICTURE covers that point
 * (GetObjectsIntersectPoint), filtered by StopsMissile (0x435c80) -> ObjectHitByMissile (0x40e600,
 * binary): an object stops the bullet when the bullet flies no higher than the object.
 * So fences and walls stop rifle fire like solid walls, and a building stops bullets anywhere its
 * picture reaches - including the roof drawn ABOVE (behind) its base, where Sarge stands hidden.
 *
 * Rifle-type bullets of the player (weapon types 1, 7, 8, 9, 10, 21 = the exe's own bullet set):
 *   - fences, walls and gates never stop them (walking is still blocked: that is the movement map);
 *   - buildings and other structures stop them only on map tiles they block for walking (the
 *     movement map, the same test as UntrafficableUnit 0x42a230): their real footprint.
 * Grenades, bazooka, mortar and flame, and enemy fire, are unchanged.
 *   DoFloatCollisionAtPoint  wrapped: remembers the point being tested
 *   ObjectHitByMissile       wrapped: __fastcall ecx = object, dx = bullet height (short)
 * [gameplay] shoot_over = 1
 * ============================================================================================ */
#include "common.h"

#define PROJECTILE        (*(BYTE **)0x6918ec)     /* the projectile being updated (FLOAT_OBJ) */
#define PROJ_WEAPON(p)    (*(int *)((p) + 0x7c))
#define UID_ARMY(uid)     ((int)(((uid) >> 26) & 7) - 1)
#define ARMY_PLAYER       0
#define OBJ_CATEGORY(o)   (*(short *)((o) + 0x68))
#define MOVEMENT_LAYER    (*(signed char **)0x4e3554)
#define MAP_TILES_W       (*(unsigned short *)0x4e368c)
#define MAP_TILES_H       (*(int *)0x4e2fa0)
#define BLOCKED           15                        /* movement value from which a tile blocks walking */

enum { ROCK = 0, FALLEN_TREE = 4, FENCE = 5, WALL = 6, GUN_TURRET = 10, AA_GUN, BASE, AUX_BASE, PRISON_CAMP = 16, BUILDING, PILL_BOX,
       POWER_PLANT, GATE, WALL_GATE, RADAR_BUILDING };

typedef struct { short x, y; } SPoint;
static const SPoint *g_point;                       /* the ground point being tested, or NULL */

CollideFn bullets_collide_original;
HitObjFn bullets_hit_object_original;

static int leg_retry(const SPoint *p, void **hit);

/* Who fired lately (2026-11-19, user: "enemies in buildings should only show *after* they start firing"): every
 * projectile passes DoFloatCollisionAtPoint while it flies; its shooter (FLOAT_OBJ +0x78 fromUID) is noted with
 * the time. roofs.c shows a covered enemy only while bullets_fired_within() says so. */
#define FIRED_SLOTS 32
static struct { DWORD uid, at; } g_fired[FIRED_SLOTS];
static void note_fired(DWORD uid)
{
    DWORD now = GetTickCount();
    int k = 0;
    for (int i = 0; i < FIRED_SLOTS; i++) {
        if (g_fired[i].uid == uid) { g_fired[i].at = now; return; }
        if (g_fired[i].at < g_fired[k].at) k = i;
    }
    g_fired[k].uid = uid; g_fired[k].at = now;
}
int bullets_fired_within(DWORD uid, DWORD ms)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < FIRED_SLOTS; i++) if (g_fired[i].uid == uid && g_fired[i].at && now - g_fired[i].at <= ms) return 1;
    return 0;
}

static const SPoint *vehicle_hills(BYTE *proj, const SPoint *p, SPoint *q, int *raise_to);
#define PROJ_ANIM         (*(BYTE **)0x691900)     /* ANIM of the projectile being updated (FLOAT_OBJ +0x68) */
#define ANIM_Z(a)         (*(short *)((a) + 0x18)) /* world_z: the height weapons 6/7/8 are tested at */
#define NO_HEIGHT         (-0x7fff)
static int g_true_height = NO_HEIGHT;               /* the shot's real height while ANIM_Z is raised (bullets_stops) */


int __fastcall bullets_collide(void *point, void **hit)
{
    BYTE *pr = PROJECTILE;
    if (pr && *(DWORD *)(pr + 0x78)) note_fired(*(DWORD *)(pr + 0x78));
    const SPoint *p = point;
    SPoint q;
    int raise = NO_HEIGHT;
    BYTE *an = PROJ_ANIM;
    if (pr && an && g_cfg.downhill_hits) p = vehicle_hills(pr, p, &q, &raise);    /* group 4, HILLS (vehicles) */
    short z0 = 0;
    if (raise != NO_HEIGHT) { z0 = ANIM_Z(an); g_true_height = z0; ANIM_Z(an) = (short)raise; }
    const SPoint *saved = g_point;
    g_point = p;
    int r = bullets_collide_original((void *)p, hit);
    if (!r) r = leg_retry(p, hit);
    g_point = saved;
    if (raise != NO_HEIGHT) { ANIM_Z(an) = z0; g_true_height = NO_HEIGHT; }
    return r;
}

#define GRENADE_WEAPON 2                 /* game table ARMY_GRENADE */
static int is_bullet(int weapon)
{
    return weapon == 1 || weapon == 7 || weapon == 8 || weapon == 9 || weapon == 10 || weapon == 21;
}

static int tile_blocked(const SPoint *p)
{
    signed char *layer = MOVEMENT_LAYER;
    int w = MAP_TILES_W, h = MAP_TILES_H;
    if (!layer || p->x < 0 || p->y < 0) return 1;
    int tx = p->x >> 4, ty = p->y >> 4;
    if (tx >= w || ty >= h) return 1;
    return layer[ty * w + tx] >= BLOCKED;
}

/* ---------------------------------------------------------------- low cover protects only a PRONE soldier
 * 2026-11-19, user: "the selected fence is blocking shots. I only want shots blocked if the target is prone behind
 * it" (Desert m2, hover: the sandbag ring is category ROCK). Low cover = fences, walls, gates, fallen trees, and
 * rocks no taller than a standing soldier (OBJECTSTRUCT +0x73 own height, STAND_HEIGHT 8; boulder piles are
 * taller and still stop fire). It stops the player's rifle bullet only when an enemy soldier lies prone within
 * COVER_RADIUS px of the point where the bullet meets it. GetItemHeightNoElevation 0x449e90 (__fastcall ecx item):
 * a soldier's pose height, PRONE 4 / KNEEL 6 / STAND 8 (game binary). Soldiers: army record 0x41a160, units from
 * +4 to +8 (stride 0x2c4), UNIT +0x70 mode 3..5 on the map, +0xb4 troopers, +0xdc last; TROOPER +0x68 1 = alive. */
typedef int (__fastcall *ItemFn)(void *item);
typedef BYTE *(__fastcall *ArmyFn)(int army);
#define HEIGHT_NO_ELEV    ((ItemFn)0x449e90)
#define ARMY_RECORD       ((ArmyFn)0x41a160)
typedef int (__fastcall *AllianceFn)(int mine, int other);
#define ALLIED            ((AllianceFn)0x41a1e0)
#define LEGION_STATUS(a)  (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))
#define UNIT_STRIDE       0x2c4
#define OBJ_HEIGHT(o)     ((int)(o)[0x73])
#define PRONE_HEIGHT      4
#define STAND_HEIGHT      8
#define COVER_RADIUS      28

/* ---- the barricade rules, the same for every army (2026-11-19, user: "make the enemy have the same rules for
 * barricades - right behind and prone, can't hit and can't fire. kneeling and standing - can shoot over. standing
 * right next to it, can shoot over"; before: only the player's bullets, enemy fire stopped at every fence).
 * A rifle bullet of any army meets low cover at point P:
 *   1. the SHOOTER is within AT_COVER px of P (he is at the barricade): prone -> stopped (cannot fire over it);
 *      kneeling / standing -> passes, also when a prone soldier lies behind it (he shoots down over it);
 *   2. else a PRONE soldier of an army hostile to the bullet lies within COVER_RADIUS px of P -> stopped;
 *   3. else it passes.
 * Shooter = FLOAT_OBJ +0x78 fromUID: Sarge (0x697520) or a trooper found by uid; unknown (vehicle gun, tower)
 * -> rules 2 and 3. Sarge counts as a soldier of army 0. */
#define SARGE_PTR         (*(BYTE **)0x697520)
#define FROM_UID(p)       (*(DWORD *)((p) + 0x78))
#define AT_COVER          32
static int item_prone(BYTE *it) { return HEIGHT_NO_ELEV(it) == PRONE_HEIGHT; }

typedef int (*TrooperFn)(BYTE *t, void *ctx);          /* non-zero = stop, that trooper is returned */
static BYTE *walk_troopers(int army, TrooperFn fn, void *ctx)
{
    if (!LEGION_STATUS(army)) return NULL;
    BYTE *leg = ARMY_RECORD(army);
    if (!leg) return NULL;
    BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
    for (int g = 0; un && un < end && g < 64; un += UNIT_STRIDE, g++) {
        int mode = *(int *)(un + 0x70);
        if (mode < 3 || mode > 5) continue;
        int last = *(int *)(un + 0xdc);
        for (int i = 0; i <= last && i < 10; i++) {
            BYTE *t = ((BYTE **)(un + 0xb4))[i];
            if (!t || *(int *)(t + 0x68) != 1 || *(short *)(t + 0x1c) <= 0) continue;
            if (fn(t, ctx)) return t;
        }
    }
    return NULL;
}

static int near_point(BYTE *it, const SPoint *p, int radius)
{
    int dx = *(short *)(it + 4) - p->x, dy = *(short *)(it + 6) - p->y;
    return dx * dx + dy * dy <= radius * radius;
}
static int is_prone_near(BYTE *t, void *ctx) { return near_point(t, ctx, COVER_RADIUS) && item_prone(t); }
static int has_uid(BYTE *t, void *ctx) { return *(DWORD *)t == *(DWORD *)ctx; }

static int prone_hostile_near(const SPoint *p, int army)
{
    for (int a = 0; a <= 3; a++) {
        if (a == army || ALLIED(army, a)) continue;
        if (walk_troopers(a, is_prone_near, (void *)p)) return 1;
        BYTE *s = SARGE_PTR;
        if (a == ARMY_PLAYER && s && *(short *)(s + 0x1c) > 0 && is_prone_near(s, (void *)p)) return 1;
    }
    return 0;
}

static BYTE *shooter_of(BYTE *proj)
{
    DWORD uid = FROM_UID(proj);
    BYTE *s = SARGE_PTR;
    if (!uid) return NULL;
    if (s && *(DWORD *)s == uid) return s;
    static DWORD last_uid; static BYTE *last; static DWORD last_at;
    if (uid == last_uid && GetTickCount() - last_at < 500) return last;      /* one bullet asks many times */
    BYTE *t = NULL;
    for (int a = 0; a <= 3 && !t; a++) t = walk_troopers(a, has_uid, &uid);
    last_uid = uid; last = t; last_at = GetTickCount();
    return t;
}

/* 1 = the bullet goes over this low cover */
static int cover_passes(BYTE *proj, const SPoint *p)
{
    BYTE *sh = shooter_of(proj);
    if (sh && near_point(sh, p, AT_COVER)) return !item_prone(sh);
    return !prone_hostile_near(p, UID_ARMY(*(DWORD *)proj));
}

static int low_cover(BYTE *obj)
{
    switch (OBJ_CATEGORY(obj)) {
    case FENCE: case WALL: case GATE: case WALL_GATE: case FALLEN_TREE: return 1;
    case ROCK: {
        static unsigned char logged[512];
        int num = *(short *)(obj + 0x6a), h = OBJ_HEIGHT(obj);
        if (num >= 0 && num < 512 && !logged[num]) { logged[num] = 1; mlog("bullets: rock #%d height %d -> %s", num, h, h <= STAND_HEIGHT ? "low cover" : "solid"); }
        return h <= STAND_HEIGHT;
    }
    }
    return 0;
}

/* 2026-11-19, user (Desert m2 screenshot: Sarge's shot along the front wall caught on the building corner): the
 * 16 px movement tiles reach past the drawn wall base, so a shot that grazes a corner or the foot of a wall was
 * stopped in front of it. The footprint is shrunk by FOOT_TRIM px on every side: the point and the four points
 * FOOT_TRIM away must all be on blocked tiles. A building is many tiles, so it still stops fire that goes in. */
#define FOOT_TRIM 6
static int target_in_front(int px, int py);
static int sarge_at_cover(const SPoint *p);
static int corner_graze(BYTE *proj, const SPoint *p);
static int inside_footprint(const SPoint *p)
{
    static const signed char d[5][2] = { { 0, 0 }, { -FOOT_TRIM, 0 }, { FOOT_TRIM, 0 }, { 0, -FOOT_TRIM }, { 0, FOOT_TRIM } };
    for (int i = 0; i < 5; i++) {
        SPoint q = { (short)(p->x + d[i][0]), (short)(p->y + d[i][1]) };
        if (!tile_blocked(&q)) return 0;
    }
    return 1;
}

/* What the mod's rules say about this bullet and this object: RULE_PASS = flies on, RULE_STOP = stopped here,
 * RULE_GAME = the game's own rule decides. */
enum { RULE_GAME, RULE_PASS, RULE_STOP };
static int aimed_target_in_front(BYTE *obj, const SPoint *p);
static int bullet_rule(BYTE *obj, BYTE *proj)
{
    int player = UID_ARMY(*(DWORD *)proj) == ARMY_PLAYER;
    /* Sarge's GRENADES (weapon 2; 2026-11-20, user: "throwing a grenade here blows up on the edge of the building",
     * "this is sarge throwing the grenade"): over a building's PICTURE but outside its trimmed footprint (roof
     * overhang, picture margin) the grenade flies on; at the footprint (the wall) the game decides. Nothing else. */
    if (PROJ_WEAPON(proj) == GRENADE_WEAPON) {
        int c = OBJ_CATEGORY(obj);
        int building = c == BASE || c == AUX_BASE || c == PRISON_CAMP || c == BUILDING || c == PILL_BOX || c == POWER_PLANT || c == RADAR_BUILDING;
        return player && g_cfg.corner_shots && building && !inside_footprint(g_point) ? RULE_PASS : RULE_GAME;
    }
    switch (OBJ_CATEGORY(obj)) {
    /* group 1, BARRICADES ([gameplay] shoot_over; enemy bullets: cover_enemies) */
    case FENCE: case WALL: case GATE: case WALL_GATE: case ROCK: case FALLEN_TREE:
        if (!g_cfg.shoot_over || (!player && !g_cfg.cover_enemies) || !low_cover(obj)) return RULE_GAME;
        return cover_passes(proj, g_point) ? RULE_PASS : RULE_STOP;
    /* group 2, CORNER SHOTS ([gameplay] corner_shots): buildings, the player's bullets only */
    case GUN_TURRET: case AA_GUN: case BASE: case AUX_BASE: case PRISON_CAMP: case BUILDING:
    case PILL_BOX: case POWER_PLANT: case RADAR_BUILDING:
        if (!player || !g_cfg.corner_shots) return RULE_GAME;
        if (!inside_footprint(g_point)) return RULE_PASS;      /* the picture, not the footprint (trimmed) */
        if (target_in_front(g_point->x, g_point->y)) return RULE_PASS;   /* a target stands in front of the wall here */
        if (aimed_target_in_front(obj, g_point)) return RULE_PASS;       /* the aimed target is drawn in front of it */
        if (corner_graze(proj, g_point)) return RULE_PASS;     /* not yet BUILDING_DEPTH px into the footprint */
        return RULE_GAME;
    }
    return RULE_GAME;
}

int __fastcall bullets_hit_object(BYTE *obj, int height)
{
    static int faults;                                  /* guarded faults in the rules: after 3 the game's rules only */
    BYTE *proj = PROJECTILE;
    if ((!g_cfg.shoot_over && !g_cfg.corner_shots) || faults >= 3 || !obj || !proj || !g_point || (!is_bullet(PROJ_WEAPON(proj)) && PROJ_WEAPON(proj) != GRENADE_WEAPON))
        return bullets_hit_object_original(obj, height);
    volatile int rule = RULE_GAME;
    Guard g;
    if (GUARD_TRY(g, "bullet rules (bullets.c)")) { rule = bullet_rule(obj, proj); guard_leave(&g); }
    else if (++faults >= 3) mlog("bullets: the mod's bullet rules are off for this session (3 guarded faults)");
    if (rule == RULE_PASS) return 0;
    if (rule == RULE_STOP) return 1;
    return bullets_hit_object_original(obj, height);
}

/* ---------------------------------------------------------------- shooting downhill
 * StopsMissile (0x435c80, __fastcall ecx = item, edx = projectile, stack height, distance, ret 8)
 * lets a unit stop a projectile only when the projectile flies no higher than the unit. Rifle and
 * bazooka rounds sink toward their target height, but the tank cannon (type 6) and machine guns
 * (7, 8) keep the height they were fired at - from high ground they pass over units below.
 * For the player's direct-fire projectiles the height is capped at the unit's height, so a unit
 * in the line of fire is hit wherever it stands. GetItemHeight = 0x449f20 (__fastcall ecx item). */
#define UID_RAW_TYPE(uid) ((int)(((uid) >> 22) & 0xf))  /* ITEM_TYPE + 1: 3 vehicle, 5 trooper, 6 sarge */
typedef int (__fastcall *ItemHeightFn)(void *item);
#define GET_ITEM_HEIGHT ((ItemHeightFn)0x449f20)

StopsFn bullets_stops_original;

/* modern.c events, from the projectile that just stopped (FLOAT_OBJ: +0 uid, +4/+6 position, +0x78 the
 * uid of whoever fired it): Sarge's shot stopped at a soldier, vehicle or target board = a hit (hit
 * marker, stats); an enemy projectile stopped at Sarge = damage from that side (direction indicator). */
#define SARGE_UNIT      (*(BYTE **)0x697520)
#define PROJ_FROM(p)    (*(DWORD *)((p) + 0x78))
static void note_for_modern(BYTE *item, BYTE *proj)
{
    BYTE *s = SARGE_UNIT;
    if (!s) return;
    if (PROJ_FROM(proj) == *(DWORD *)s && item != s) {
        int t = UID_RAW_TYPE(*(DWORD *)item);
        short num = *(short *)(item + 0x6a);
        int board = (t >= 7 && t <= 9) && ((num >= 351 && num <= 353) || num == 367 || num == 368);
        if (((t == 3 || t == 5 || t == 6) && UID_ARMY(*(DWORD *)item) != ARMY_PLAYER) || board) modern_note_hit(item);
    } else if ((item == s || item == *(BYTE **)(s + 0x2d0)) && UID_ARMY(*(DWORD *)proj) != ARMY_PLAYER) {   /* Sarge or the vehicle he drives (2026-11-20) */
        modern_note_damage(*(short *)(proj + 4), *(short *)(proj + 6));
    }
}

static int direct_fire(int weapon) { return is_bullet(weapon) || weapon == 4 || weapon == 6; }

int __fastcall bullets_stops(BYTE *item, BYTE *proj, int height, int distance)
{
    if (g_true_height != NO_HEIGHT) height = g_true_height;     /* vehicle_hills raised ANIM_Z only for the ground test */
    /* 2026-11-20 (user: elevation rules are hard "without visual queues" -> "Both"): every army's direct fire hits what
     * its tracer crosses (was the player's only); the ground still stops shots (cliffs, ridges: modern.c shows it) */
    if (g_cfg.downhill_hits && item && proj && direct_fire(PROJ_WEAPON(proj))) {   /* group 4, DOWNHILL HITS */
        int t = UID_RAW_TYPE(*(DWORD *)item);
        if (t == 3 || t == 5 || t == 6) {
            int h = GET_ITEM_HEIGHT(item);
            if (height > h) height = h;
        }
    }
    int stops = bullets_stops_original(item, proj, height, distance);
    if (stops && item && proj) note_for_modern(item, proj);
    return stops;
}

/* ---------------------------------------------------------------- vehicles on hills (group 4, [gameplay] downhill_hits)
 * 2026-11-20, user: "all vehicles should be able to shoot enemies on 'higher' or 'lower' [ground] without issue. was
 * fixed for tanks but needs to be fixed for everything." Game facts (binary VehicleFireWeapon,
 * game binary ProjectileSet / UpdateProjectile / DoFloatCollisionAtPoint, checked in the exe 0x435250 / 0x435e20 /
 * 0x435a10):
 *   - a vehicle fires at z = its ground elevation + VEHICLE_FIRE_Z (8), whatever it aims at; the shot keeps that
 *     height (weapons 6, 7, 8 take ANIM world_z, +0x18; dest_height is not used for them).
 *   - ProjectileSet places the shot's ground point (shadow) z px SOUTH of the muzzle (y + z_init); the tracer is
 *     drawn z px above it, on the muzzle line. The terrain is drawn flat, so the elevation part of z is not a
 *     real offset on screen.
 *   - tank cannon / bazooka (MISSILE_FLOAT) are tested at the DRAWN point (shadow y - world_z): they hit what the
 *     tracer crosses. Jeep MMG (7) / halftrack HMG (8) (RIFLE_FLOAT) are tested at the shadow point: from a hill of
 *     elevation E their test line runs E + 8 px south of the tracer and misses what the player aims at - with the
 *     downhill height cap above, the tank hit soldiers below, the machine guns did not.
 *   - ground: after the items, the shot stops where the tile's elevation ((elevation layer 0x4e355c & 0xf0) >> 2)
 *     is >= its height; the cannon only at >= height + 4. A shot fired from low ground at a soldier on a plateau
 *     stops at the plateau's edge.
 * Changed, for the player's army's vehicle shots (fromUID item type vehicle; also Sarge's captured one):
 *   1. machine guns: the point is moved up by the shooter's elevation (world_z - VEHICLE_FIRE_Z): the test line
 *      lies 8 px below the tracer as on flat ground (the game's own offset there).
 *   2. ground: machine guns get the cannon's margin (ground stops them only at >= height + 4), and any of these
 *      shots (6, 7, 8) flies on over ground no higher than the ground at the spot Sarge aims at (pointer, tiles
 *      within AIM_TILES of it; only for the vehicle Sarge drives). Ground higher than that - a ridge between -
 *      still stops it. Done by raising ANIM world_z for that call only (DoFloatCollisionAtPoint reads it once,
 *      0x435a6f) and giving units / objects the real height back in bullets_stops (g_true_height).
 * Enemy vehicles, soldiers' and Sarge's own weapons are unchanged. */
#define VEHICLE_FIRE_Z    8                         /* game binary: GetElevationHeightAtItem + 8 */
#define CANNON_WEAPON     6
#define CANNON_GROUND     4                         /* game binary: the cannon stops at ground >= height + 4 */
#define ELEV_LAYER        (*(BYTE **)0x4e355c)
#define AIM_TILES         1                         /* the aimed spot's ground: max over +-1 tile (a soldier's feet) */

static int ground_elev(int x, int y)                /* -1 off the map */
{
    BYTE *layer = ELEV_LAYER;
    int w = MAP_TILES_W, h = MAP_TILES_H;
    if (!layer || x < 0 || y < 0) return -1;
    int tx = x >> 4, ty = y >> 4;
    if (tx >= w || ty >= h) return -1;
    return (layer[ty * w + tx] & 0xf0) >> 2;
}

static int aimed_ground(BYTE *proj)                 /* ground elevation at Sarge's aim, -1 = not his vehicle / no aim */
{
    BYTE *s = SARGE_UNIT, *v = s ? *(BYTE **)(s + 0x2d0) : NULL;
    int ax, ay, best = -1;
    if (!v || *(DWORD *)v != FROM_UID(proj) || !controls_pointer_world(&ax, &ay)) return -1;
    for (int dy = -AIM_TILES; dy <= AIM_TILES; dy++)
        for (int dx = -AIM_TILES; dx <= AIM_TILES; dx++) {
            int e = ground_elev(ax + dx * 16, ay + dy * 16);
            if (e > best) best = e;
        }
    return best;
}

static int log_once(BYTE **last, DWORD *at, BYTE *proj)   /* one line per shot */
{
    DWORD now = GetTickCount();
    if (g_cfg.log < 2 || (*last == proj && now - *at < 1500)) return 0;
    *last = proj; *at = now;
    return 1;
}

static const SPoint *vehicle_hills(BYTE *proj, const SPoint *p, SPoint *q, int *raise_to)
{
    int weapon = PROJ_WEAPON(proj);
    DWORD from = FROM_UID(proj);
    if ((weapon != CANNON_WEAPON && weapon != 7 && weapon != 8) || UID_RAW_TYPE(from) != 3)   /* every army's vehicles (2026-11-20) */
        return p;
    int z = ANIM_Z(PROJ_ANIM), shooter = z - VEHICLE_FIRE_Z;
    if (weapon != CANNON_WEAPON && shooter > 0) {                       /* 1. the test line follows the tracer */
        *q = *p;
        q->y = (short)(p->y - shooter > 0 ? p->y - shooter : 0);
        static BYTE *l1; static DWORD t1;
        if (log_once(&l1, &t1, proj)) mlog("bullets: vehicle shot (weapon %d) from elevation %d: tested %d px up (%d,%d -> %d,%d)", weapon, shooter, shooter, p->x, p->y, q->x, q->y);
        p = q;
    }
    int e = ground_elev(p->x, p->y);
    if (e < 0 || e < z) return p;                                       /* the game would not stop it here */
    if (e < z + CANNON_GROUND && weapon != CANNON_WEAPON) {            /* 2. the cannon's margin */
        static BYTE *l3; static DWORD t3;
        if (log_once(&l3, &t3, proj)) mlog("bullets: vehicle shot (weapon %d, height %d) at ground %d (%d,%d) -> flies on (cannon margin)", weapon, z, e, p->x, p->y);
        *raise_to = e + 1;
        return p;
    }
    if (e >= z + CANNON_GROUND) {
        int aim = aimed_ground(proj);
        static BYTE *l2; static DWORD t2;
        if (aim >= e) *raise_to = e + 1;                                /* 2. up to the aimed spot's ground */
        if (log_once(&l2, &t2, proj)) mlog("bullets: vehicle shot (weapon %d, height %d) at ground %d (%d,%d), aimed ground %d -> %s",
                                           weapon, z, e, p->x, p->y, aim, aim >= e ? "flies on" : "stopped (the game)");
    }
    return p;
}

/* ---------------------------------------------------------------- legs count (2026-11-19, user: "the hitboxes on
 * enemies feel off - shooting at legs is missing")
 * The bullet tests its ground point every 2 px (UpdateProjectile, FLOAT_STEP) against the soldier's PIXELS
 * (GetObjectsIntersectPoint, UseRectIntersect 0): two thin legs with a gap between them let a shot slip past.
 * When the player's rifle bullet hits nothing at a point inside the lower body of an enemy soldier's picture
 * (middle half of its width, below LEG_FROM of its height), the game's own test is run once more at the
 * soldier's torso (the picture's middle): same damage, height test and explosion as a body hit. */
#define LEG_FROM 45                                 /* % of the picture height from the top */
typedef struct { const SPoint *p; SPoint body; } LegCtx;
static int legs_hold_point(BYTE *t, void *ctx)
{
    LegCtx *c = ctx;
    BYTE *av = *(BYTE **)(t + 0x26c), *an = av ? *(BYTE **)(av + 0x1c) : NULL;
    if (!an) return 0;
    const int *w = (const int *)(an + 0x1c);
    const SPoint *p = c->p;
    if (p->x < w[0] || p->x >= w[2] || p->y < w[1] || p->y >= w[3]) return 0;
    int wd = w[2] - w[0], ht = w[3] - w[1];
    if (p->x < w[0] + wd / 4 || p->x >= w[2] - wd / 4 || p->y < w[1] + ht * LEG_FROM / 100) return 0;
    c->body.x = (short)((w[0] + w[2]) / 2); c->body.y = (short)(w[1] + ht * 2 / 5);
    return 1;
}
static int leg_retry(const SPoint *p, void **hit)
{
    static int faults;
    BYTE *proj = PROJECTILE;
    if (!g_cfg.leg_hits || faults >= 3 || !proj || !is_bullet(PROJ_WEAPON(proj)) || UID_ARMY(*(DWORD *)proj) != ARMY_PLAYER) return 0;   /* group 3, LEG HITS */
    LegCtx c = { p, { 0, 0 } };
    volatile int found = 0;
    Guard g;
    if (GUARD_TRY(g, "leg hits (bullets.c)")) {             /* the mod's own walk over the soldier lists */
        for (int a = 1; a <= 3 && !found; a++)
            if (!ALLIED(0, a) && walk_troopers(a, legs_hold_point, &c)) found = 1;
        guard_leave(&g);
    } else { faults++; found = 0; }
    if (!found) return 0;
    const SPoint *saved = g_point;                           /* the game's own test, at his torso (not guarded) */
    g_point = &c.body;
    int r = bullets_collide_original(&c.body, hit);
    g_point = saved;
    return r;
}

/* in front of a building: an enemy soldier, enemy vehicle or explosive (barrel) whose picture holds this point
 * (2026-11-19, user: a barrel at the foot of a wall - "I should be able to shoot behind a corner"). The bullet
 * then passes the building here and can reach it. */
static int target_in_front(int px, int py)
{
    BYTE *cl[8];
    int n = roofs_cells_at(px, py, cl, 8);
    for (int i = 0; i < n; i++) {
        BYTE *ow = *(BYTE **)(cl[i] + 0x0c);
        if (!ow) continue;
        int t = UID_RAW_TYPE(*(DWORD *)ow);
        if ((t == 3 || t == 5) && UID_ARMY(*(DWORD *)ow) != ARMY_PLAYER && *(short *)(ow + 0x1c) > 0) return 1;
        if (t >= 7 && t <= 9 && OBJ_CATEGORY(ow) == 9 && !(ow[0x70] & 0x04)) return 1;   /* EXPLOSIVES, not destroyed */
    }
    return 0;
}

/* Sarge right at a fence / wall / sandbags (2026-11-19, user: "if sarge is right next to the fence or barrier, he
 * can shoot over it into an enemy prone"): the point where the bullet meets the cover is within SARGE_AT_COVER
 * px of Sarge - the barrier does not stop it, also not for a prone enemy behind it. */
#define SARGE_AT_COVER 32
static int sarge_at_cover(const SPoint *p)
{
    BYTE *s = SARGE_UNIT;
    if (!s) return 0;
    int dx = *(short *)(s + 4) - p->x, dy = *(short *)(s + 6) - p->y;
    return dx * dx + dy * dy <= SARGE_AT_COVER * SARGE_AT_COVER;
}

/* Corners (2026-11-19, user screenshot: Sarge inside the walled yard shooting past the corner of the building at
 * a soldier behind sandbags - "another corner shot that I should be able to make"): a building stops the bullet
 * only once it has gone BUILDING_DEPTH px through the (trimmed) footprint. Per projectile the run of footprint
 * points is measured; a point that does not follow the last one (more than 4 px away) starts a new run. A shot
 * clipping a corner leaves the footprint before that; one aimed into a wall stops 16 px inside it. */
#define BUILDING_DEPTH 16
#define GRAZE_SLOTS 8
static struct { BYTE *proj; short x, y; int depth; DWORD at; } g_graze[GRAZE_SLOTS];
static int corner_graze(BYTE *proj, const SPoint *p)
{
    int k = -1, old = 0;
    for (int i = 0; i < GRAZE_SLOTS; i++) {
        if (g_graze[i].proj == proj) { k = i; break; }
        if (g_graze[i].at < g_graze[old].at) old = i;
    }
    if (k < 0) { k = old; g_graze[k].proj = proj; g_graze[k].depth = -1; }
    int dx = p->x - g_graze[k].x, dy = p->y - g_graze[k].y, d2 = dx * dx + dy * dy;
    if (g_graze[k].depth < 0 || d2 > 16) g_graze[k].depth = 0;           /* a new run through a footprint */
    else if (d2) g_graze[k].depth += d2 <= 4 ? 2 : 3;                     /* bullet steps are 2 px */
    g_graze[k].x = p->x; g_graze[k].y = p->y; g_graze[k].at = GetTickCount();
    return g_graze[k].depth < BUILDING_DEPTH;
}

/* The AIMED target is drawn in front of this building (2026-11-19, user screenshot: reticle on a barrel at the foot
 * of a wall, Sarge beside it - "should be able to shoot the barrel here"). The bullet's ground line runs along
 * the wall's footprint for much longer than a corner graze. What the player sees decides: the pointer is on the
 * picture of an enemy soldier / enemy vehicle / explosive, that picture is drawn IN FRONT of this building part
 * (the game's sort key: ANIMCELL world_pos.y, +0x12; the building part's own cell lies at OBJECTSTRUCT +0x7c),
 * and the bullet is within AIM_NEAR px of the pointer: the building does not stop it there. A target BEHIND the
 * building (sorted before it, hidden by it) does not qualify. */
#define OBJ_CELL  0x7c
#define AIM_NEAR  56
static int aimed_target_in_front(BYTE *obj, const SPoint *p)
{
    int ax, ay;
    if (!controls_pointer_world(&ax, &ay)) return 0;
    int dx = p->x - ax, dy = p->y - ay;
    if (dx * dx + dy * dy > AIM_NEAR * AIM_NEAR) return 0;
    BYTE *bc = obj + OBJ_CELL;
    if (*(BYTE **)(bc + 0x0c) != obj) return 0;               /* not the layout expected: leave it to the game */
    int by = *(short *)(bc + 0x12);
    BYTE *cl[8];
    int n = roofs_cells_at(ax, ay, cl, 8);
    for (int i = 0; i < n; i++) {
        BYTE *ow = *(BYTE **)(cl[i] + 0x0c);
        if (!ow || ow == obj) continue;
        int t = UID_RAW_TYPE(*(DWORD *)ow), target = 0;
        if ((t == 3 || t == 5) && UID_ARMY(*(DWORD *)ow) != ARMY_PLAYER && *(short *)(ow + 0x1c) > 0) target = 1;
        if (t >= 7 && t <= 9 && OBJ_CATEGORY(ow) == 9 && !(ow[0x70] & 0x04)) target = 1;
        if (target && *(short *)(cl[i] + 0x12) >= by) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- shot prediction for the reticle (2026-11-20)
 * User: "gameplay wise it makes it much more difficult without visual queues ... provide an indicator to show height
 * differences. and queues on how to fix it". The ground rule of a shot from Sarge (on foot or in his vehicle) to the
 * world point (tx, ty), as the game + vehicle_hills apply it:
 *   on foot: height = GetItemHeight(Sarge) + 1 (pose + elevation + 1, FUN_00447c30); stopped where ground >= height;
 *   vehicle: height = ground + VEHICLE_FIRE_Z; stopped where ground >= height + CANNON_GROUND and the ground there is
 *            higher than the ground at the aimed spot (aimed_ground).
 * Walked in 4 px steps from the shooter. Returns 0 = clear, 1 = cliff (the target's ground is at least as high as
 * the stop: go up), 2 = ridge (higher ground between: move round); *bx, *by = the stop point. Levels: ground / 4. */
int bullets_level_at(int x, int y) { int e = ground_elev(x, y); return e < 0 ? -1 : e / 4; }
int bullets_shot_block(int tx, int ty, int *bx, int *by)
{
    BYTE *s = SARGE_UNIT, *v = s ? *(BYTE **)(s + 0x2d0) : NULL;
    if (!s || !g_cfg.downhill_hits) return 0;
    BYTE *sh = v ? v : s;
    int x0 = *(short *)(sh + 4), y0 = *(short *)(sh + 6), g0 = ground_elev(x0, y0), z, margin;
    if (g0 < 0) return 0;
    int aim = -1;
    if (v) {
        z = g0 + VEHICLE_FIRE_Z; margin = CANNON_GROUND;
        for (int dy = -AIM_TILES; dy <= AIM_TILES; dy++)
            for (int dx = -AIM_TILES; dx <= AIM_TILES; dx++) { int e = ground_elev(tx + dx * 16, ty + dy * 16); if (e > aim) aim = e; }
    } else { z = GET_ITEM_HEIGHT(s) + 1; margin = 0; }
    int dx = tx - x0, dy = ty - y0, n = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 4;
    for (int i = 1; i <= n; i++) {
        int x = x0 + dx * i / n, y = y0 + dy * i / n, e = ground_elev(x, y);
        if (e < 0 || e < z + margin || (v && aim >= e)) continue;
        *bx = x; *by = y;
        int tg = ground_elev(tx, ty);
        return tg >= e ? 1 : 2;
    }
    return 0;
}
