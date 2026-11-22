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
#include <math.h>

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


static const SPoint *flat_ground(BYTE *proj, const SPoint *p, SPoint *q, int *raise_to);
static int proj_height(BYTE *proj);
static void note_threat(BYTE *proj, const SPoint *p, int hit);
static void shot_track(BYTE *proj, const SPoint *p, int stopped);
static const SPoint *am2_ground(BYTE *proj, const SPoint *p, int *raise_to);
static int g_am2_height = NO_HEIGHT;                /* height_rules=am2: the round's height (aimed line) during this test */

static int __fastcall bullets_collide_px(void *point, void **hit) {
    BYTE *pr = PROJECTILE;
    if (pr && *(DWORD *)(pr + 0x78)) note_fired(*(DWORD *)(pr + 0x78));
    const SPoint *p = point;
    SPoint q;
    int raise = NO_HEIGHT;
    BYTE *an = PROJ_ANIM;
    if (pr && an && g_cfg.height_rules == HEIGHTS_MODDED) p = vehicle_hills(pr, p, &q, &raise);   /* group 4, HILLS (vehicles) */
    else if (pr && an && g_cfg.height_rules == HEIGHTS_STANCE) p = flat_ground(pr, p, &q, &raise);  /* heights ignored */
    else if (pr && an && g_cfg.height_rules == HEIGHTS_AM2) p = am2_ground(pr, p, &raise);         /* Army Men II aimed height */
    short z0 = 0;
    if (raise != NO_HEIGHT) { z0 = ANIM_Z(an); g_true_height = proj_height(pr); ANIM_Z(an) = (short)raise; }
    if (pr && an) note_threat(pr, p, 0);
    const SPoint *saved = g_point;
    g_point = p;
    int r = PX_CALL(bullets_collide_original((void *)p, hit));
    if (!r) r = leg_retry(p, hit);
    g_point = saved;
    if (raise != NO_HEIGHT) { ANIM_Z(an) = z0; g_true_height = NO_HEIGHT; }
    g_am2_height = NO_HEIGHT;
    if (pr && g_cfg.log >= 2) shot_track(pr, p, r ? (*hit ? 2 : 1) : 0);
    return r;
}
int __fastcall bullets_collide(void *point, void **hit) { PX_BEGIN(px); int r_ = bullets_collide_px(point, hit); PX_END(px, PR_BULLETS); return r_; }   /* timed: stage PR_BULLETS (exclusive, common.h) */

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

/* heights off (2026-11-20, user "Damage + cover"): a KNEELING soldier right behind low cover is half covered - of the
 * rounds that meet that cover near him, every second one is stopped (deterministic alternation, one counter for all;
 * one decision per round and cover object, cached while the round crosses it). */
#define KNEEL_HEIGHT      6
static int is_kneel_near(BYTE *t, void *ctx) { return near_point(t, ctx, COVER_RADIUS) && HEIGHT_NO_ELEV(t) == KNEEL_HEIGHT; }
static BYTE *kneel_hostile_near(const SPoint *p, int army)
{
    for (int a = 0; a <= 3; a++) {
        if (a == army || ALLIED(army, a)) continue;
        BYTE *t = walk_troopers(a, is_kneel_near, (void *)p);
        if (t) return t;
        BYTE *s = SARGE_PTR;
        if (a == ARMY_PLAYER && s && *(short *)(s + 0x1c) > 0 && is_kneel_near(s, (void *)p)) return s;
    }
    return NULL;
}
#define KNEEL_SLOTS 8
static struct { BYTE *proj, *obj; DWORD at; int stop; } g_kneel[KNEEL_SLOTS];
static int g_kneel_flip;
static int kneel_cover_passes(BYTE *proj, BYTE *obj, BYTE *who)
{
    DWORD now = GetTickCount();
    int old = 0;
    for (int i = 0; i < KNEEL_SLOTS; i++) {
        if (g_kneel[i].proj == proj && g_kneel[i].obj == obj && now - g_kneel[i].at < 400) { g_kneel[i].at = now; return !g_kneel[i].stop; }
        if (g_kneel[i].at < g_kneel[old].at) old = i;
    }
    g_kneel_flip ^= 1;
    g_kneel[old].proj = proj; g_kneel[old].obj = obj; g_kneel[old].at = now; g_kneel[old].stop = g_kneel_flip;
    if (g_cfg.log >= 2) mlog("bullets: stance: kneeling %s %p behind cover #%d (category %d): round of weapon %d %s",
                             who == SARGE_PTR ? "Sarge" : "soldier", who, *(short *)(obj + 0x6a), OBJ_CATEGORY(obj),
                             PROJ_WEAPON(proj), g_kneel_flip ? "STOPPED (every second round)" : "passes");
    return !g_kneel_flip;
}

/* 1 = the bullet goes over this low cover */
static int cover_passes(BYTE *proj, BYTE *obj, const SPoint *p)
{
    BYTE *sh = shooter_of(proj);
    if (sh && near_point(sh, p, AT_COVER)) return !item_prone(sh);
    int army = UID_ARMY(*(DWORD *)proj);
    if (prone_hostile_near(p, army)) return 0;
    if (g_cfg.height_rules == HEIGHTS_STANCE) {
        BYTE *k = kneel_hostile_near(p, army);
        if (k) return kneel_cover_passes(proj, obj, k);
    }
    return 1;
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
    int off = g_cfg.height_rules == HEIGHTS_STANCE;
    switch (OBJ_CATEGORY(obj)) {
    /* group 1, BARRICADES ([gameplay] shoot_over; enemy bullets: cover_enemies). height_rules=original: the game's
     * own rule. Heights off: every direct-fire weapon (also bazooka / cannon); with the barricade rules switched
     * off the low cover stops the round like a wall (no height to fly over it). */
    case FENCE: case WALL: case GATE: case WALL_GATE: case ROCK: case FALLEN_TREE:
        if (g_cfg.height_rules == HEIGHTS_ORIGINAL || g_cfg.height_rules == HEIGHTS_AM2 || !low_cover(obj)) return RULE_GAME;   /* am2: no cover rule */
        if (!g_cfg.shoot_over || (!player && !g_cfg.cover_enemies)) return off ? RULE_STOP : RULE_GAME;
        return cover_passes(proj, obj, g_point) ? RULE_PASS : RULE_STOP;
    /* group 2, CORNER SHOTS ([gameplay] corner_shots): buildings, the player's bullets only */
    case GUN_TURRET: case AA_GUN: case BASE: case AUX_BASE: case PRISON_CAMP: case BUILDING:
    case PILL_BOX: case POWER_PLANT: case RADAR_BUILDING:
        if (!player || !g_cfg.corner_shots || !is_bullet(PROJ_WEAPON(proj))) return RULE_GAME;
        if (!inside_footprint(g_point)) return RULE_PASS;      /* the picture, not the footprint (trimmed) */
        if (target_in_front(g_point->x, g_point->y)) return RULE_PASS;   /* a target stands in front of the wall here */
        if (aimed_target_in_front(obj, g_point)) return RULE_PASS;       /* the aimed target is drawn in front of it */
        if (corner_graze(proj, g_point)) return RULE_PASS;     /* not yet BUILDING_DEPTH px into the footprint */
        return RULE_GAME;
    }
    return RULE_GAME;
}

static int direct_fire(int weapon);
/* heights off, an object the rules above leave to the game: what blocks the TRACER stops the round whatever its
 * height - tall objects (own height above STAND_HEIGHT: buildings, trees, boulders), buildings of any height and
 * explosives (category 9, barrels: hit when the tracer crosses them). Other low objects keep the game's own height
 * test. The game's exceptions and its see-through chance (OBJECTSTRUCT +0x78) stay: ObjectHitByMissile itself is
 * called, with a height below everything. */
#define EXPLOSIVES 9
static int blocks_tracer(BYTE *obj)
{
    int c = OBJ_CATEGORY(obj);
    return OBJ_HEIGHT(obj) > STAND_HEIGHT || c == EXPLOSIVES || c == GUN_TURRET || c == AA_GUN || c == BASE || c == AUX_BASE ||
           c == PRISON_CAMP || c == BUILDING || c == PILL_BOX || c == POWER_PLANT || c == RADAR_BUILDING;
}

static int __fastcall bullets_hit_object_px(BYTE *obj, int height) {
    static int faults;                                  /* guarded faults in the rules: after 3 the game's rules only */
    BYTE *proj = PROJECTILE;
    int w = proj ? PROJ_WEAPON(proj) : 0, off = (g_cfg.height_rules == HEIGHTS_STANCE || g_cfg.height_rules == HEIGHTS_AM2) && direct_fire(w);
    if ((!g_cfg.shoot_over && !g_cfg.corner_shots && !off) || faults >= 3 || !obj || !proj || !g_point || (!is_bullet(w) && w != GRENADE_WEAPON && !off))
        return PX_CALL(bullets_hit_object_original(obj, height));
    volatile int rule = RULE_GAME;
    Guard g;
    if (GUARD_TRY(g, "bullet rules (bullets.c)")) { rule = bullet_rule(obj, proj); guard_leave(&g); }
    else if (++faults >= 3) mlog("bullets: the mod's bullet rules are off for this session (3 guarded faults)");
    if (rule == RULE_PASS) return 0;
    if (rule == RULE_STOP) return 1;
    if (off && blocks_tracer(obj)) return PX_CALL(bullets_hit_object_original(obj, -0x7fff));
    if (off && g_am2_height != NO_HEIGHT) return PX_CALL(bullets_hit_object_original(obj, g_am2_height));   /* am2: low objects by the aimed height */
    return PX_CALL(bullets_hit_object_original(obj, height));
}
int __fastcall bullets_hit_object(BYTE *obj, int height) { PX_BEGIN(px); int r_ = bullets_hit_object_px(obj, height); PX_END(px, PR_BULLETS); return r_; }   /* timed: stage PR_BULLETS (exclusive, common.h) */

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

static int __fastcall bullets_stops_px(BYTE *item, BYTE *proj, int height, int distance) {
    if (g_true_height != NO_HEIGHT) height = g_true_height;     /* vehicle_hills / flat_ground raised ANIM_Z only for the ground test */
    /* 2026-11-20 (user: elevation rules are hard "without visual queues" -> "Both"): every army's direct fire hits what
     * its tracer crosses (was the player's only); the ground still stops shots (cliffs, ridges: modern.c shows it).
     * height_rules: modded = this height cap; off = no height at all (any elevation, any pose: the game's other
     * tests - alive, not the shooter's own army, the muzzle distance - stay); original = the game's own test. */
    if (g_cfg.height_rules != HEIGHTS_ORIGINAL && item && proj && direct_fire(PROJ_WEAPON(proj))) {   /* group 4, DOWNHILL HITS */
        int t = UID_RAW_TYPE(*(DWORD *)item);
        if (t == 3 || t == 5 || t == 6) {
            if (g_cfg.height_rules == HEIGHTS_STANCE || g_cfg.height_rules == HEIGHTS_AM2) height = -0x7fff;
            else { int h = GET_ITEM_HEIGHT(item); if (height > h) height = h; }
        }
    }
    int stops = PX_CALL(bullets_stops_original(item, proj, height, distance));
    if (stops && item && proj) { note_for_modern(item, proj); if (item == SARGE_UNIT) note_threat(proj, NULL, 1); }
    return stops;
}
int __fastcall bullets_stops(BYTE *item, BYTE *proj, int height, int distance) { PX_BEGIN(px); int r_ = bullets_stops_px(item, proj, height, distance); PX_END(px, PR_BULLETS); return r_; }   /* timed: stage PR_BULLETS (exclusive, common.h) */

/* ---------------------------------------------------------------- vehicles on hills (group 4, [gameplay] height_rules = modded)
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
    int r = PX_CALL(bullets_collide_original(&c.body, hit));
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


/* ============================================================================================ height_rules
 * [gameplay] height_rules = stance | am2 | original (2026-11-20, user; replaces downhill_hits; round 2: "off" renamed
 * stance - read as an alias -, am2 added, modded only from the ini):
 *   stance   - (default) heights ignored, stance + cover decide:
 *     - every direct-fire round (bullets, machine guns, cannon, bazooka; all armies) hits the first unit it
 *       crosses whatever its elevation or pose (bullets_stops: height below everything);
 *     - the ground never cuts a shot short (flat_ground below); the shot still ends where the game ends it: at
 *       the weapon's range (UpdateProjectile 0x435e20: flown > range) or, for the rounds that sink (rifle
 *       1 / 9 / 10, bazooka 4: height sinks linearly to FLOAT +0x74 over the flight length +0x70), by landing
 *       where the game lands it once they have flown their flight length (the game's own ground test from there);
 *     - tall objects, buildings and barrels stop / take the round where the tracer crosses them (blocks_tracer);
 *       low cover: the barricade rules + kneeling = half covered (kneel_cover_passes);
 *     - stance damage + rear hits x2 (bullets_item_damage) and prone spotting (bullets_can_see) below.
 *   am2      - Army Men II's rules (its FUN_0043bcd0 / 0043bbe0 lookup): a round flies from the shooter's height
 *              toward the height of what it was aimed at (am2_ground); units are hit whatever their height or pose
 *              (as stance); the ground stops the round where it is at least as high as that line (cliffs, ridges
 *              between); low objects by that line, tall ones as stance; no stance damage change, no cover rule, no
 *              prone spotting; rear hits x2.
 *   original - the game's own code: none of the mod's height / ground changes, the low-cover override (group 1)
 *              off; reticle cues predicted with the game's rules (bullets_shot_cue).
 *   modded   - (ini only) the round-1 rules: downhill height cap for all armies (bullets_stops), vehicles on hills
 *              (vehicle_hills), prone-only low cover.
 * Grenades, mortar and flamer are not direct fire: unchanged in every mode.
 * ============================================================================================ */
const char *const HEIGHT_RULE_NAMES[HEIGHTS_COUNT] = { "modern", "modded", "original", "am2" };   /* "modern" was "stance" (2026-11-21, user rename) */
const char *const HEIGHT_RULE_TEXT[HEIGHTS_COUNT] = {
    "modern (heights ignored, stance + cover)", "modded (ini only: hits downhill, ground stops)",
    "original (the game's heights)", "am2 (Army Men II: aimed height, rear x2)" };
int height_rules_parse(const char *v)
{
    for (int i = 0; i < HEIGHTS_COUNT; i++) if (!_stricmp(v, HEIGHT_RULE_NAMES[i])) return i;
    return HEIGHTS_STANCE;                                  /* "stance" / "off" (older names) and anything else = modern */
}
void height_rules_set(int mode)
{
    if (mode < 0 || mode >= HEIGHTS_COUNT) mode = HEIGHTS_STANCE;
    g_cfg.height_rules = mode;
    WritePrivateProfileStringA("gameplay", "height_rules", HEIGHT_RULE_NAMES[mode], mod_ini_path());
    mlog("bullets: height_rules = %s", HEIGHT_RULE_NAMES[mode]);
}
void height_rules_next(void)
{
    int m = g_cfg.height_rules;
    height_rules_set(m == HEIGHTS_STANCE ? HEIGHTS_AM2 : m == HEIGHTS_AM2 ? HEIGHTS_ORIGINAL : HEIGHTS_STANCE);
}

#define SINKS(w)          ((w) == 1 || (w) == 4 || (w) == 9 || (w) == 10)   /* UpdateProjectile: height sinks to +0x74 */
#define PROJ_TRAVELLED(p) (*(int *)((p) + 0x6c))    /* FLOAT_OBJ: distance flown (also StopsMissile's distance) */
#define PROJ_FLIGHT(p)    (*(int *)((p) + 0x70))    /* ProjectileSet param 5: flight length the height sinks over */
#define ANIM_Z_FRAC(a)    (*(float *)((a) + 0xc0))  /* the part of the sink not yet taken off world_z */
#define POSE_KNEEL        0xb
#define POSE_STAND        0xc
#define POSE_PRONE        0xd

/* the round's height as DoFloatCollisionAtPoint (0x435a45) tests it: world_z - the sink fraction (truncated) for the
 * weapons that sink, world_z for the others */
static int proj_height(BYTE *proj)
{
    BYTE *an = PROJ_ANIM;
    if (!an) return 0;
    if (SINKS(PROJ_WEAPON(proj))) return (int)((float)ANIM_Z(an) - ANIM_Z_FRAC(an));
    return ANIM_Z(an);
}

/* heights off: the ground under a direct-fire round never stops it (before its flight length for the sinking ones).
 * Done as vehicle_hills does it: world_z is raised for this one test so the ground test (0x435b66) passes, and units
 * / objects get the real height back in bullets_stops (g_true_height). Vehicle machine guns: tested on the tracer
 * line as in modded (vehicle_hills 1.). */
static const SPoint *flat_ground(BYTE *proj, const SPoint *p, SPoint *q, int *raise_to)
{
    int w = PROJ_WEAPON(proj);
    if (!direct_fire(w)) return p;
    if ((w == 7 || w == 8) && UID_RAW_TYPE(FROM_UID(proj)) == 3) {
        int shooter = ANIM_Z(PROJ_ANIM) - VEHICLE_FIRE_Z;
        if (shooter > 0) { *q = *p; q->y = (short)(p->y - shooter > 0 ? p->y - shooter : 0); p = q; }
    }
    if (SINKS(w) && PROJ_TRAVELLED(proj) >= PROJ_FLIGHT(proj)) return p;     /* flown its length: lands as the game says */
    int e = ground_elev(p->x, p->y);
    if (e < 0) return p;
    int h = proj_height(proj), margin = w == CANNON_WEAPON ? CANNON_GROUND : 0;
    if (h + margin > e) return p;                                          /* the game would not stop it here */
    *raise_to = ANIM_Z(PROJ_ANIM) + (e - margin - h) + 2;
    static BYTE *l; static DWORD t;
    if (log_once(&l, &t, proj)) mlog("bullets: stance: weapon %d (army %d) height %d over ground %d at (%d,%d), flown %d of %d -> flies on",
                                     w, UID_ARMY(*(DWORD *)proj), h, e, p->x, p->y, PROJ_TRAVELLED(proj), PROJ_FLIGHT(proj));
    return p;
}

/* ---------------------------------------------------------------- am2: the aimed line
 * Army Men II aims a round at its target's height. Per projectile (cached at its first test, AM2_SLOTS): start
 * point and height z0 (the game's fire height: soldiers GetItemHeight + 1, vehicles ground + VEHICLE_FIRE_Z), the
 * aimed point and its height zt, the distance between. The height at a point d px from the start is
 * z0 + (zt - z0) * min(d, dist) / dist. Aim:
 *   - Sarge's shots (on foot or his vehicle): the unit under the pointer (its GetItemHeight), else the pointer's
 *     ground + STAND_HEIGHT - the same line the reticle cue draws (bullets_shot_cue);
 *   - other rifles / bazooka (sinking rounds): the game's own line already is this (fire height -> FLOAT +0x74, the
 *     height the shooter aimed at, over the flight length +0x70) - kept;
 *   - other machine guns / cannon (the game keeps their height): the end of the flight along the round's facing
 *     (ANIM +0x9a, tables 0x4ea778 / 0x4e9f78), its ground + STAND_HEIGHT.
 * The ground test (DoFloatCollisionAtPoint 0x435b66: stopped where ground >= height, cannon >= height + 4) then
 * runs with that height (world_z shifted for the test, as vehicle_hills does), and low objects are tested at it
 * (g_am2_height -> bullets_hit_object). Past the flight length the sinking rounds land as in the game. */
#define AM2_SLOTS 16
#define FACING_DX ((const double *)0x4ea778)
#define FACING_DY ((const double *)0x4e9f78)
#define PROJ_FACING(a) (*(short *)((a) + 0x9a))
static struct { BYTE *proj; short x0, y0; int z0, zt, dist, flown, game; DWORD at; } g_am2[AM2_SLOTS];
/* Sarge's aim: the unit under the pointer, else the pointer's ground + standing height. 0 = no pointer. */
static int am2_sarge_aim(int *ax, int *ay, int *zt)
{
    BYTE *t = controls_hover_target();
    if (t) {
        int tt = UID_RAW_TYPE(*(DWORD *)t);
        if ((tt == 3 || tt == 5 || tt == 6) && *(short *)(t + 0x1c) > 0) { *ax = *(short *)(t + 4); *ay = *(short *)(t + 6); *zt = GET_ITEM_HEIGHT(t); return 1; }
    }
    if (!controls_pointer_world(ax, ay)) return 0;
    int e = ground_elev(*ax, *ay);
    *zt = (e < 0 ? 0 : e) + STAND_HEIGHT;
    return 1;
}
static int am2_line_height(int z0, int zt, int dist, int d) { if (dist <= 0 || d >= dist) return zt; return z0 + (zt - z0) * d / dist; }
static const SPoint *am2_ground(BYTE *proj, const SPoint *p, int *raise_to)
{
    int w = PROJ_WEAPON(proj);
    if (!direct_fire(w)) return p;
    DWORD now = GetTickCount();
    int k = -1, old = 0;
    for (int i = 0; i < AM2_SLOTS; i++) {
        if (g_am2[i].proj == proj && now - g_am2[i].at < 700 && PROJ_TRAVELLED(proj) >= g_am2[i].flown) { k = i; break; }
        if (g_am2[i].at < g_am2[old].at) old = i;
    }
    if (k < 0) {                                                       /* a new round: its line */
        k = old;
        g_am2[k].proj = proj; g_am2[k].x0 = p->x; g_am2[k].y0 = p->y; g_am2[k].z0 = proj_height(proj); g_am2[k].game = 0;
        BYTE *s = SARGE_UNIT, *v = s ? *(BYTE **)(s + 0x2d0) : NULL;
        DWORD from = FROM_UID(proj);
        int ax, ay, zt;
        if (s && (from == *(DWORD *)s || (v && from == *(DWORD *)v)) && am2_sarge_aim(&ax, &ay, &zt)) {
            g_am2[k].zt = zt; g_am2[k].dist = (int)sqrt((double)(ax - p->x) * (ax - p->x) + (double)(ay - p->y) * (ay - p->y));
        } else if (SINKS(w)) g_am2[k].game = 1;
        else {
            int f = PROJ_FACING(PROJ_ANIM) & 0xff, left = PROJ_FLIGHT(proj) - PROJ_TRAVELLED(proj);
            if (left < 0) left = 0;
            int ex = p->x + (int)(FACING_DX[f] * left), ey = p->y + (int)(FACING_DY[f] * left), e = ground_elev(ex, ey);
            g_am2[k].zt = (e < 0 ? ground_elev(p->x, p->y) : e) + STAND_HEIGHT; g_am2[k].dist = left;
        }
        if (g_cfg.log >= 2) mlog("bullets: am2 line: weapon %d army %d from (%d,%d) height %d -> %s height %d over %d px",
                                 w, UID_ARMY(*(DWORD *)proj), p->x, p->y, g_am2[k].z0, g_am2[k].game ? "the game's own aim" : "aimed",
                                 g_am2[k].game ? (int)*(BYTE *)(proj + 0x74) : g_am2[k].zt, g_am2[k].game ? PROJ_FLIGHT(proj) : g_am2[k].dist);
    }
    g_am2[k].at = now; g_am2[k].flown = PROJ_TRAVELLED(proj);
    if (g_am2[k].game || (SINKS(w) && PROJ_TRAVELLED(proj) >= PROJ_FLIGHT(proj))) return p;   /* the game's line / landing */
    int dx = p->x - g_am2[k].x0, dy = p->y - g_am2[k].y0;
    int h = am2_line_height(g_am2[k].z0, g_am2[k].zt, g_am2[k].dist, (int)sqrt((double)dx * dx + (double)dy * dy));
    int hg = proj_height(proj);
    g_am2_height = h;
    if (h != hg) *raise_to = ANIM_Z(PROJ_ANIM) + (h - hg);
    int e = ground_elev(p->x, p->y), margin = w == CANNON_WEAPON ? CANNON_GROUND : 0;
    static BYTE *l; static DWORD t;
    if (e >= 0 && h + margin <= e && log_once(&l, &t, proj))
        mlog("bullets: am2: weapon %d (army %d) aimed height %d (game height %d) at ground %d (%d,%d) -> stopped by the ground", w, UID_ARMY(*(DWORD *)proj), h, hg, e, p->x, p->y);
    return p;
}

/* log=2: where each direct-fire shot ended (first and last tested point, why) - checks that heights off keeps the
 * shots' length. A shot is written out once it has not been tested for SHOT_IDLE ms (or its slot is reused). */
#define SHOT_SLOTS 12
#define SHOT_IDLE  700
static struct { BYTE *proj; short x0, y0, x1, y1; int flown, flight, weapon, army, end, h0, h1; DWORD at; } g_shot[SHOT_SLOTS];
static void shot_flush(int i)
{
    if (!g_shot[i].proj) return;
    int dx = g_shot[i].x1 - g_shot[i].x0, dy = g_shot[i].y1 - g_shot[i].y0;
    mlog("bullets: shot end (height_rules %s): weapon %d army %d (%d,%d) -> (%d,%d) = %d px, flown %d, flight length %d, height %d -> %d, %s",
         HEIGHT_RULE_NAMES[g_cfg.height_rules], g_shot[i].weapon, g_shot[i].army, g_shot[i].x0, g_shot[i].y0, g_shot[i].x1, g_shot[i].y1,
         (int)sqrt((double)dx * dx + dy * dy), g_shot[i].flown, g_shot[i].flight, g_shot[i].h0, g_shot[i].h1,
         g_shot[i].end == 2 ? "stopped by a unit / object" : g_shot[i].end == 1 ? "stopped by the GROUND" : "range end");
    g_shot[i].proj = NULL;
}
static void shot_track(BYTE *proj, const SPoint *p, int stopped)
{
    DWORD now = GetTickCount();
    int w = PROJ_WEAPON(proj), k = -1, old = 0;
    for (int i = 0; i < SHOT_SLOTS; i++) {
        if (g_shot[i].proj && now - g_shot[i].at > SHOT_IDLE) shot_flush(i);
        if (g_shot[i].at < g_shot[old].at) old = i;
    }
    if (!direct_fire(w)) return;
    for (int i = 0; i < SHOT_SLOTS; i++)
        if (g_shot[i].proj == proj) { k = i; break; }
    if (k >= 0 && PROJ_TRAVELLED(proj) < g_shot[k].flown) { shot_flush(k); k = -1; }   /* a new shot in that slot */
    if (k < 0) {
        k = old; shot_flush(k);
        g_shot[k].proj = proj; g_shot[k].x0 = p->x; g_shot[k].y0 = p->y; g_shot[k].weapon = w; g_shot[k].army = UID_ARMY(*(DWORD *)proj);
        g_shot[k].end = 0; g_shot[k].h0 = proj_height(proj);
    }
    g_shot[k].x1 = p->x; g_shot[k].y1 = p->y; g_shot[k].flown = PROJ_TRAVELLED(proj); g_shot[k].flight = PROJ_FLIGHT(proj);
    g_shot[k].h1 = proj_height(proj); g_shot[k].at = now;
    if (stopped) { g_shot[k].end = stopped; shot_flush(k); }
}

/* ---------------------------------------------------------------- Sarge under fire (defensive stance hints)
 * An enemy direct-fire round tested within NEAR_MISS px of Sarge (on foot), or stopped at him, is noted with its
 * height (as the game tests it), weapon and where it came from (the shooter, else the round's previous point).
 * bullets_stance_hint() gives modern.c the stance that really helps against it for THREAT_MS. */
#define NEAR_MISS  24
#define THREAT_MS  2000
static DWORD g_rear_at;                             /* Sarge last hit from behind (rear x2, bullets_item_damage) */
static struct { DWORD at; int height, weapon, fx, fy, hit; } g_threat;
static void note_threat(BYTE *proj, const SPoint *p, int hit)
{
    int w = PROJ_WEAPON(proj), army = UID_ARMY(*(DWORD *)proj);
    if (!direct_fire(w) || army <= ARMY_PLAYER || army > 3) return;
    BYTE *s = SARGE_UNIT;
    if (!s || *(BYTE **)(s + 0x2d0) || *(short *)(s + 0x1c) <= 0 || ALLIED(ARMY_PLAYER, army)) return;
    int sx = *(short *)(s + 4), sy = *(short *)(s + 6);
    if (p) { int dx = p->x - sx, dy = p->y - sy; if (dx * dx + dy * dy > NEAR_MISS * NEAR_MISS) return; }
    g_threat.at = GetTickCount();
    g_threat.height = g_true_height != NO_HEIGHT ? g_true_height : proj_height(proj);
    g_threat.weapon = w; g_threat.hit = hit;
    BYTE *sh = shooter_of(proj);
    if (sh) { g_threat.fx = *(short *)(sh + 4); g_threat.fy = *(short *)(sh + 6); }
    else {                                                 /* vehicle gun, tower: back along the round's last step */
        int cx = *(short *)(proj + 4), cy = *(short *)(proj + 6), px0 = *(short *)(proj + 0x10), py0 = *(short *)(proj + 0x12);
        int dx = px0 - cx, dy = py0 - cy; double d = sqrt((double)dx * dx + dy * dy);
        if (d > 0.5) { g_threat.fx = sx + (int)(dx / d * 200); g_threat.fy = sy + (int)(dy / d * 200); }
    }
}

/* low cover (as the barricade rules see it) within maxd px of item `who` on the line toward (tx, ty); *cx, *cy = the
 * point on it. Pictures at the points (roofs_cells_at), the same objects the bullet would meet. */
static BYTE *own_cover(BYTE *who, int tx, int ty, int maxd, int *cx, int *cy)
{
    int x0 = *(short *)(who + 4), y0 = *(short *)(who + 6), dx = tx - x0, dy = ty - y0;
    double d = sqrt((double)dx * dx + dy * dy);
    if (d < 1) return NULL;
    for (int r = 4; r <= maxd && r < d; r += 4) {
        int x = x0 + (int)(dx * r / d), y = y0 + (int)(dy * r / d);
        BYTE *cl[8];
        int n = roofs_cells_at(x, y, cl, 8);
        for (int i = 0; i < n; i++) {
            BYTE *ow = *(BYTE **)(cl[i] + 0x0c);
            if (!ow || ow == who) continue;
            int t = UID_RAW_TYPE(*(DWORD *)ow);
            if (t < 7 || t > 9) continue;
            switch (OBJ_CATEGORY(ow)) {
            case FENCE: case WALL: case GATE: case WALL_GATE: case ROCK: case FALLEN_TREE:
                if (low_cover(ow)) { *cx = x; *cy = y; return ow; }
            }
        }
    }
    return NULL;
}

int bullets_stance_hint(char *buf, int n, const char *down, const char *up)
{
    BYTE *s = SARGE_UNIT;
    DWORD now = GetTickCount();
    static char last[64];
    buf[0] = 0;
    if (s && g_threat.at && now - g_threat.at <= THREAT_MS && !*(BYTE **)(s + 0x2d0) && *(short *)(s + 0x1c) > 0) {
        int pose = HEIGHT_NO_ELEV(s), sx = *(short *)(s + 4), sy = *(short *)(s + 6), e = ground_elev(sx, sy), h = g_threat.height;
        int cx, cy, mode = g_cfg.height_rules;
        BYTE *cov = mode != HEIGHTS_ORIGINAL && mode != HEIGHTS_AM2 && g_cfg.shoot_over ? own_cover(s, g_threat.fx, g_threat.fy, COVER_RADIUS, &cx, &cy) : NULL;
        if (now - g_rear_at <= THREAT_MS) lstrcpynA(buf, "HIT FROM BEHIND: TURN", n);   /* stance / am2: rear hits x2 */
        else if (mode == HEIGHTS_AM2) ;                        /* am2: no stance helps (no cover, no damage share) */
        else if (cov) {                                        /* stance / modded: prone right behind it = cannot be hit */
            if (pose == PRONE_HEIGHT) lstrcpynA(buf, "PRONE BEHIND COVER: SAFE - CANNOT FIRE", n);
            else wsprintfA(buf, "PRONE BEHIND COVER: %s", down);
        } else if (mode == HEIGHTS_STANCE) {                      /* damage: prone 50 %, kneeling 75 % */
            if (pose == STAND_HEIGHT) wsprintfA(buf, "GET DOWN: %s", down);
            else if (pose == KNEEL_HEIGHT) wsprintfA(buf, "GO PRONE: %s", down);
        } else if (mode == HEIGHTS_ORIGINAL && e >= 0 && !SINKS(g_threat.weapon) && h <= e + pose) {
            /* the game's own test (StopsMissile: Sarge is hit while round height <= his height): rounds that keep
             * their height pass over a lower stance. Rifle / bazooka rounds sink to the height they were aimed at,
             * so a lower stance does not help against them - no hint. */
            if (pose == STAND_HEIGHT && h > e + KNEEL_HEIGHT) wsprintfA(buf, "KNEEL: ROUNDS PASS OVER - %s", down);
            else if (pose != PRONE_HEIGHT && h > e + PRONE_HEIGHT) wsprintfA(buf, "GO PRONE: ROUNDS PASS OVER - %s", down);
        }
        if (g_cfg.log >= 2 && lstrcmpA(buf, last))
            mlog("bullets: stance hint \"%s\" (height_rules %s, pose height %d, ground %d, round height %d, weapon %d, %s, cover %s)",
                 buf, HEIGHT_RULE_NAMES[mode], pose, e, h, g_threat.weapon, g_threat.hit ? "hit" : "near miss", cov ? "yes" : "no");
    }
    lstrcpynA(last, buf, sizeof last);
    (void)up;
    return buf[0] != 0;
}

/* ---------------------------------------------------------------- stance damage (heights off)
 * ItemDamage 0x431f60 (__fastcall ecx = item, edx = damage; stack: damage type, SPOINT *at, UID from; ret 12) is the
 * one entry for damage to an item (projectile hits 0x435b44, explosions, crush); it dispatches to SargeDamage 0x447fc0
 * (type 6) / trooper damage 0x43c5a0 (type 5). Damage types (0x4b4570 by weapon): 6 = bullets (weapons 1, 7, 8, 9, 10,
 * 21), 3 bazooka, 4 cannon, 0x41 grenade, 0x42 mortar, 0x45 explosion; 7 flame, 0 / 8 others.
 * height_rules=stance, Sarge and soldiers of every army:  bullets  standing 100 %, kneeling 75 %, prone 50 %;
 *                                                 blasts   prone 75 %, kneeling / standing 100 %;   others 100 %.
 * Fractions are carried per victim (a 3-point rifle hit on a prone soldier: 1, 2, 1, 2 ...), so the share is exact on
 * average; a hit may then do 0. The game's own prone rule (damage - damage / 8, below 8 damage
 * 1 in 7 absorbed; 0x4480b8 Sarge, 0x43c6c4 troopers) is switched off then: its GetPose call answers "standing"
 * for prone (bullets_damage_pose). Kept: the unit-mode reduction of troopers, Sarge's random damage spread. */
ItemDamageWrapFn bullets_item_damage_original;
#define DMG_BULLETS 6
static int blast_damage(int type) { return type == 3 || type == 4 || type == 0x41 || type == 0x42 || type == 0x45; }
#define CARRY_SLOTS 32
static struct { BYTE *item; int rest; } g_carry[CARRY_SLOTS];      /* hundredths of a point not yet dealt, per victim */
static int scaled_damage(BYTE *item, int damage, int pct)
{
    int k = (int)(((uintptr_t)item >> 4) % CARRY_SLOTS);
    if (g_carry[k].item != item) { g_carry[k].item = item; g_carry[k].rest = 0; }
    int v = damage * pct + g_carry[k].rest;
    g_carry[k].rest = v % 100;
    return v / 100;
}
/* REAR HITS x2 (stance and am2; 2026-11-20 user, Army Men II's rule: FUN_0043bcd0 flags a hit whose round travels
 * within REAR_ARC (0x30 of 256) of the way the soldier faces - from behind - and FUN_0043bbe0 doubles it): a round of
 * weapons 1, 7, 8, 9, 10, 21 (damage type 6) that hits a soldier or Sarge. Victim facing = ITEM +8 (the field the
 * game's own direction test 0x432050 reads), round = its ANIM +0x9a, both in the facing tables' 256 steps. Applied
 * on top of the stance share: prone, from behind = 50 % x 2 = 100 %. AM1's own direction rule (0x432050, called at
 * 0x435b10: x2 when the round's direction lies between facing + 0x40 and facing + 0xc0 - wrong for half the
 * facings, its range does not wrap) is switched off in these two modes (bullets_game_double). */
#define REAR_ARC 0x30
/* g_rear_at: Sarge hit from behind (stance hint), declared with g_threat */
static int rear_hit(BYTE *item, int *diff)
{
    BYTE *pr = PROJECTILE, *an = PROJ_ANIM;
    if (!g_point || !pr || !an || !is_bullet(PROJ_WEAPON(pr))) return 0;   /* only inside a bullet's collision test */
    int d = (PROJ_FACING(an) - *(short *)(item + 8)) & 0xff;
    *diff = d;
    return d < REAR_ARC || d > 0x100 - REAR_ARC;
}
static void __fastcall bullets_item_damage_px(BYTE *item, int damage, int type, void *at, DWORD from) {
    if (damage > 0) aware_note_damage(item, from);     /* aware.c: splash / vehicle hits by Sarge count as "hit by Sarge" */
    int mode = g_cfg.height_rules;
    if ((mode == HEIGHTS_STANCE || mode == HEIGHTS_AM2) && item && damage > 0) {
        int t = UID_RAW_TYPE(*(DWORD *)item);
        if ((t == 5 || t == 6) && *(short *)(item + 0x1c) > 0) {
            int pose = HEIGHT_NO_ELEV(item), pct = 100, diff = -1, rear = type == DMG_BULLETS && rear_hit(item, &diff);
            if (mode == HEIGHTS_STANCE) {
                if (type == DMG_BULLETS) pct = pose == PRONE_HEIGHT ? 50 : pose == KNEEL_HEIGHT ? 75 : 100;
                else if (blast_damage(type)) pct = pose == PRONE_HEIGHT ? 75 : 100;
            }
            if (rear) { pct *= 2; if (t == 6) g_rear_at = GetTickCount(); }
            int d = scaled_damage(item, damage, pct);
            if (g_cfg.log >= 2) mlog("bullets: %s damage: %s %p pose height %d, damage type 0x%x: %d -> %d (%d%%)%s, facing %d round %d (diff %d)",
                                     HEIGHT_RULE_NAMES[mode], t == 6 ? "Sarge" : "soldier", item, pose, type, damage, d, pct,
                                     rear ? " REAR x2" : "", *(short *)(item + 8) & 0xff, g_point && PROJ_ANIM ? PROJ_FACING(PROJ_ANIM) & 0xff : -1, diff);
            damage = d;
        }
    }
    PX_GAME(bullets_item_damage_original(item, damage, type, at, from));
}
void __fastcall bullets_item_damage(BYTE *item, int damage, int type, void *at, DWORD from) { PX_BEGIN(px); bullets_item_damage_px(item, damage, type, at, from); PX_END(px, PR_BULLETS); }   /* timed: stage PR_BULLETS (exclusive, common.h) */

typedef int (__fastcall *PoseFn)(void *avatar);
#define GET_POSE          ((PoseFn)0x43a140)       /* avatar (Sarge +0x268, trooper +0x26c) -> 0xb kneel 0xc stand 0xd prone */
static int __fastcall bullets_damage_pose_px(void *avatar) {
    int p = PX_CALL(GET_POSE(avatar));
    return (g_cfg.height_rules == HEIGHTS_STANCE || g_cfg.height_rules == HEIGHTS_AM2) && p == POSE_PRONE ? POSE_STAND : p;
}
int __fastcall bullets_damage_pose(void *avatar) { PX_BEGIN(px); int r_ = bullets_damage_pose_px(avatar); PX_END(px, PR_BULLETS); return r_; }   /* timed: stage PR_BULLETS (exclusive, common.h) */

typedef int (__fastcall *GameDoubleFn)(int damage, int dir, BYTE *item);
#define GAME_DIRECTION_X2 ((GameDoubleFn)0x432050)
static int __fastcall bullets_game_double_px(int damage, int dir, BYTE *item)   /* call 0x435b10: ecx damage, edx dir, stack item; ret 4 */
{
    if (g_cfg.height_rules == HEIGHTS_STANCE || g_cfg.height_rules == HEIGHTS_AM2) return damage;   /* rear x2 instead */
    return PX_CALL(GAME_DIRECTION_X2(damage, dir, item));
}
int __fastcall bullets_game_double(int damage, int dir, BYTE *item) { PX_BEGIN(px); int r_ = bullets_game_double_px(damage, dir, item); PX_END(px, PR_BULLETS); return r_; }   /* timed: stage PR_BULLETS (exclusive, common.h) */

/* ---------------------------------------------------------------- prone is harder to spot (heights off)
 * ItemUpdateSeen 0x44a360 (__fastcall ecx = viewer, edx = item looked at) marks an item seen by the viewer's army
 * (See 0x449a90; newly seen + hostile -> the alert 0x44f080) when CanSeeFromItem 0x44af10 (ecx viewer, edx the item's
 * tile) says so: the tile within the sight box +-SIGHT_X / SIGHT_Y (170 px) of the viewer, then the line of sight.
 * The call 0x44a47d (soldiers, vehicles, Sarge of the other armies as viewers; towers use their own tests) goes
 * through bullets_sight_stub, which passes the item looked at (esi there): a prone soldier or Sarge is noticed only
 * within PRONE_SIGHT_PCT % of the box. Only who notices whom changes: where the game aims or fires without this
 * test (weapon range + line of sight 0x43b2e0) stays the game's. */
#define SIGHT_X           (*(int *)0x4bdf1c)
#define SIGHT_Y           (*(int *)0x4bdf20)
#define PRONE_SIGHT_PCT   60
typedef int (__fastcall *CanSeeFn)(BYTE *viewer, unsigned tile);
#define CAN_SEE_FROM_ITEM ((CanSeeFn)0x44af10)
static int __cdecl bullets_can_see_px(BYTE *viewer, unsigned tile, BYTE *target) {
    if (g_cfg.height_rules == HEIGHTS_STANCE && viewer && target && MAP_TILES_W) {
        int t = UID_RAW_TYPE(*(DWORD *)target);
        if ((t == 5 || t == 6) && *(short *)(target + 0x1c) > 0 && item_prone(target)) {
            int dx = *(short *)(viewer + 4) - (int)(tile % MAP_TILES_W) * 16, dy = *(short *)(viewer + 6) - (int)(tile / MAP_TILES_W) * 16;
            int bx = SIGHT_X * PRONE_SIGHT_PCT / 100, by = SIGHT_Y * PRONE_SIGHT_PCT / 100;
            if (abs(dx) > bx || abs(dy) > by) {
                static DWORD at;
                if (g_cfg.log >= 2 && GetTickCount() - at > 1000 && abs(dx) <= SIGHT_X && abs(dy) <= SIGHT_Y) {
                    at = GetTickCount();
                    mlog("bullets: prone %s %p not noticed by %p at %d,%d px (prone sight box %d x %d, usual %d x %d)",
                         t == 6 ? "Sarge" : "soldier", target, viewer, dx, dy, bx, by, SIGHT_X, SIGHT_Y);
                }
                return 0;
            }
        }
    }
    return aware_filter_seen(viewer, target, PX_CALL(CAN_SEE_FROM_ITEM(viewer, tile)));   /* aware.c: only units that noticed Sarge */
}
int __cdecl bullets_can_see(BYTE *viewer, unsigned tile, BYTE *target) { PX_BEGIN(px); int r_ = bullets_can_see_px(viewer, tile, target); PX_END(px, PR_BULLETS); return r_; }   /* timed: stage PR_BULLETS (exclusive, common.h) */
__asm__(".globl _bullets_sight_stub\n"
        "_bullets_sight_stub:\n"                  /* at 0x44a47d: ecx viewer, edx tile, esi the item looked at */
        "    push %esi\n"
        "    push %edx\n"
        "    push %ecx\n"
        "    call _bullets_can_see\n"
        "    add $12, %esp\n"
        "    ret\n");

/* ---------------------------------------------------------------- shot cues for the reticle (2026-11-20)
 * User: "gameplay wise it makes it much more difficult without visual queues ... provide an indicator to show height
 * differences. and queues on how to fix it". What stops Sarge's shot (on foot or in his vehicle) at the world point
 * (tx, ty), by the active height_rules - the rules bullets.c / the game really apply:
 *   own cover (all modes, on foot): low cover within AT_COVER px on the line - modded / off with the barricade rules:
 *       blocked while he is prone (cover_passes 1.); off without them: a wall; original (and modded without them):
 *       the game's height test, his round (GetItemHeight + 1) against the cover's top (elevation + own height).
 *   ground, modded: on foot height = GetItemHeight(Sarge) + 1 (pose + elevation + 1, FUN_00447c30), stopped where
 *       ground >= height; vehicle: height = ground + VEHICLE_FIRE_Z, stopped where ground >= height + CANNON_GROUND
 *       and the ground there is higher than the ground at the aimed spot (aimed_ground).
 *   ground, original: on foot as modded; vehicle: height = ground + VEHICLE_FIRE_Z; machine guns stopped at ground
 *       >= height, tested on their shadow line (ground elevation px south of the tracer), the cannon at >= height +
 *       CANNON_GROUND; no aimed-spot exemption.
 *   ground, off: never.
 *   over (original): weapons whose round keeps its height (vehicle guns 6 / 7 / 8, 21) pass over the unit under the
 *       pointer when the round is higher than it (StopsMissile 0x435c80: hit while height <= GetItemHeight, troopers
 *       + 2). Rifle / bazooka rounds sink to the height Sarge aims at (+0x2e8 from FUN_00447e10): no cue for them.
 * The ground walk: 4 px steps from the shooter; 1 = cliff (the target's ground is at least as high as the stop: go
 * up), 2 = ridge (higher ground between: move round). Levels: ground / 4. */
int bullets_level_at(int x, int y) { int e = ground_elev(x, y); return e < 0 ? -1 : e / 4; }
int bullets_shot_cue(int tx, int ty, int weapon, ShotCue *c)
{
    memset(c, 0, sizeof *c);
    BYTE *s = SARGE_UNIT, *v = s ? *(BYTE **)(s + 0x2d0) : NULL;
    if (!s) return 0;
    int mode = g_cfg.height_rules;
    BYTE *sh = v ? v : s;
    int x0 = *(short *)(sh + 4), y0 = *(short *)(sh + 6), g0 = ground_elev(x0, y0);
    if (g0 < 0) return 0;
    c->levels = mode != HEIGHTS_STANCE;
    if (!v) {                                                           /* his own low cover */
        int cx, cy;
        BYTE *cov = own_cover(s, tx, ty, AT_COVER, &cx, &cy);
        if (cov) {
            int blocked, raise = POSE_KNEEL;
            if ((mode == HEIGHTS_STANCE || mode == HEIGHTS_MODDED) && g_cfg.shoot_over) blocked = item_prone(s);
            else if (mode == HEIGHTS_STANCE) { blocked = 1; raise = 0; }
            else {
                int top = ground_elev(cx, cy) + OBJ_HEIGHT(cov), z = GET_ITEM_HEIGHT(s) + 1, ax, ay, zt;
                if (mode == HEIGHTS_AM2 && am2_sarge_aim(&ax, &ay, &zt)) {      /* the aimed line at the cover */
                    int dl = (int)sqrt((double)(ax - x0) * (ax - x0) + (double)(ay - y0) * (ay - y0));
                    z = am2_line_height(z, zt, dl, (int)sqrt((double)(cx - x0) * (cx - x0) + (double)(cy - y0) * (cy - y0)));
                }
                blocked = z <= top;
                raise = g0 + KNEEL_HEIGHT + 1 > top ? POSE_KNEEL : g0 + STAND_HEIGHT + 1 > top ? POSE_STAND : 0;
            }
            if (blocked) { c->block = 3; c->bx = cx; c->by = cy; c->raise = raise; return 1; }
        }
    }
    if (mode == HEIGHTS_STANCE) return 1;
    int z, margin = 0, aim = -1, south = 0, zt = NO_HEIGHT, dl = 0;
    if (mode == HEIGHTS_AM2) {                                          /* the aimed line (am2_ground) */
        int ax, ay;
        if (am2_sarge_aim(&ax, &ay, &zt)) dl = (int)sqrt((double)(ax - x0) * (ax - x0) + (double)(ay - y0) * (ay - y0));
        else zt = NO_HEIGHT;
    }
    if (v) {
        z = g0 + VEHICLE_FIRE_Z;
        if (mode == HEIGHTS_AM2) margin = weapon == CANNON_WEAPON ? CANNON_GROUND : 0;
        else if (mode == HEIGHTS_MODDED) {
            margin = CANNON_GROUND;
            for (int dy = -AIM_TILES; dy <= AIM_TILES; dy++)
                for (int dx = -AIM_TILES; dx <= AIM_TILES; dx++) { int e = ground_elev(tx + dx * 16, ty + dy * 16); if (e > aim) aim = e; }
        } else {
            margin = weapon == CANNON_WEAPON ? CANNON_GROUND : 0;
            if (weapon == 7 || weapon == 8) south = g0;                   /* the shadow line */
        }
    } else z = GET_ITEM_HEIGHT(s) + 1;
    int dx = tx - x0, dy = ty - y0, n = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 4;
    for (int i = 1; i <= n; i++) {
        int x = x0 + dx * i / n, y = y0 + dy * i / n + south, e = ground_elev(x, y);
        int zi = zt != NO_HEIGHT ? am2_line_height(z, zt, dl, (int)sqrt((double)(x - x0) * (x - x0) + (double)(y - y0) * (y - y0))) : z;
        if (e < 0 || e < zi + margin || (v && aim >= e)) continue;
        c->bx = x; c->by = y;
        c->block = ground_elev(tx, ty) >= e ? 1 : 2;
        break;
    }
    if (mode == HEIGHTS_ORIGINAL) {
        if (!SINKS(weapon) && direct_fire(weapon)) {
            BYTE *t = controls_hover_target();
            if (t) {
                int tt = UID_RAW_TYPE(*(DWORD *)t), ta = UID_ARMY(*(DWORD *)t);
                if ((tt == 3 || tt == 5 || tt == 6) && ta != ARMY_PLAYER && *(short *)(t + 0x1c) > 0 && z > GET_ITEM_HEIGHT(t) + (tt == 5 ? 2 : 0))
                    c->over = 1;
            }
        }
        if (v && (weapon == 7 || weapon == 8) && g0 > 0) { c->mg_low = g0; c->mx = tx; c->my = ty + g0; }
    }
    { static int lb = -1, lo = -1, lm = -1; if (g_cfg.log >= 2 && (c->block != lb || c->over != lo || c->mg_low != lm)) { lb = c->block; lo = c->over; lm = c->mg_low;
        mlog("bullets: reticle cue (height_rules %s, weapon %d): block %d at %d,%d raise %x, over %d, mg_low %d, levels %d (shooter ground %d, aim ground %d)", HEIGHT_RULE_NAMES[mode], weapon, c->block, c->bx, c->by, c->raise, c->over, c->mg_low, c->levels, g0, ground_elev(tx, ty)); } }
    return 1;
}

/* savestate.c: the per-projectile tables are keyed by projectile address - saved with the projectiles */
void bullets_savestate(void) { SAVESTATE(g_fired); SAVESTATE(g_kneel); SAVESTATE(g_kneel_flip); SAVESTATE(g_graze); SAVESTATE(g_am2); SAVESTATE(g_shot); SAVESTATE(g_rear_at); SAVESTATE(g_threat); SAVESTATE(g_carry); }
