/* ============================================================================================
 * drops.c - enemy soldiers drop their weapon where they die; the kind of the hovered enemy soldier
 * (2026-11-21, user: "mouseover on enemies should tell me rifleman, flameman, mortarman, etc. and if they are not
 * basic rifleman, they should drop the weapon they are carrying with default ammo count")
 *
 * Game data used (exe, checked in the disassembly):
 *   - Soldier kind = the trooper's type, kept in its UID: type = ((uid >> 16) & 0x3f) - 1 (TrooperInit 0x43b480 /
 *     0x43b4f0 make the UID with FUN_00449bd0 -> FUN_00427ab0 from it). The exe's names (0x4bb318..0x4bb36c):
 *     RifleMen GrenadeMen FlameMen BazookaMen SweeperMen MortarMen MinerMen SpecialMen.
 *   - Its weapon: FUN_0043d570 (from TrooperInit) makes it with MakeWeapon 0x446190 into the trooper's own record at
 *     +0x280, pointer at +0x20 (WEAPON +0 = weapon type); the type comes from the table 0x5042c4[trooper type]
 *     (FUN_00427f80). Weapon types: 1 trooper rifle (Wine Desert 1: every RifleMan), 2 grenade, 3 flamethrower,
 *     4 bazooka, 5 mortar, 9 Sarge's rifle, 10 auto rifle, 11 mines, 12 explosives, 20 minesweeper. The table is still 0
 *     at map load (filled later), so the spare kinds fall back to a fixed list then (below).
 *   - The game has NO drop on death (troopers' weapons are records inside the trooper, never map objects; the only
 *     items appearing at a dead soldier are script PLACEOBJECTs of named mission items).
 *   - Map weapon pickups are OBJECTSTRUCTs (0xd0 bytes, array 0x4e30ec, count 0x4e30e8) of category 0x19 (special
 *     item) whose number (+0x6a) IS the weapon type; FUN_0040c1d0 (object init, from FUN_0040a140 at map load) makes
 *     their WEAPON record with MakeWeapon(edx = number, ammo = byte +0x72; 0 = the game's default: 3, flamethrower x6
 *     = 18; rifles / sweeper unlimited; cap 99) into +0x20. The object array is made ONCE per map: FUN_0040a6b0
 *     (called at 0x40b745 by the map loader) appends the mine / explosive charge objects (category 9) for the mine /
 *     explosive pickups (+0x72 charges each, 0 = 5), then FUN_0040a140 (0x424360) sorts the array by category
 *     (per-category start 0x4e31c0[c], count 0x4e318c[c], number limit 0x4e30f0[c]) and inits every object.
 *
 * What the mod does:
 *   - SPARE PICKUPS made the game's own way at map load (hook_call 0x40b745 -> drops_load_objects, before the
 *     original FUN_0040a6b0): for every non-rifle weapon an enemy trooper type carries (table 0x5042c4), POOL spare
 *     category-0x19 objects of that weapon number are appended to the object array exactly like FUN_0040a6b0 appends
 *     the mine charges (in the bag: +0x70 0x20, no position = not on the map, army 0xff, ammo byte 0 = default).
 *     The game then counts their mine charges, sorts them in, makes their WEAPON records and UIDs like any map
 *     pickup. FUN_0040a140 keeps the order inside a category and 0x19 sorts last,
 *     so the spares are the last objects of the array; the ones dropped are remembered per map load (the loader moves
 *     their 0,0 start to 17,20 / cell 257 - no position marker).
 *   - DROP: every frame (gameplay_frame -> drops_frame) the enemy armies' troopers are walked (legion +4/+8 UNIT
 *     array, stride 0x2c4, +0x70 mode 3..5, +0xb4 pTroops, +0xdc last; trooper +0x68 mode 1 = alive, +0x1c health);
 *     one that was alive and now has health <= 0 drops a spare of his weapon at his body (his position +4/+6), placed
 *     like Sarge's own drop (FUN_00449190: flags 0x20 -> 0x40, FUN_00449bf0 position + cell, +0x7c bit 0, draw
 *     position +0x8c, redraw FUN_0042d4e0, time +0xcc) - so walking over it is the game's own pickup (FUN_004495c0 ->
 *     weapons.c boxes, stacking to 99). FUN_00414770 / FUN_00414400 of the original drop are NOT called: they treat
 *     +0x7c as a full ANIM and write up to +0x11c, past the 0xd0 object (harmless between objects, past the end of
 *     the array for the last one - our spares are the last objects); their effect on the object (bit 0, +0x8c/+0x8e
 *     position, +0x90 old position, redraw) is done directly.
 *     Not dropped: allies / Sarge's squad, plain riflemen (weapon 1), a body on a spot where Sarge cannot stand
 *     (water, walls - the game's Sarge test, tried at the body and up to 24 px round it) or outside the map's Sarge
 *     rectangle (0x4e3014). Pool used up: logged, nothing dropped.
 *   - HOVER LABEL: drops_hover_label() for modern.c - the kind of the enemy soldier the fire pointer is on
 *     (controls_hover_target: the same target test as the fire cursor), by his weapon.
 * Switches: [gameplay] weapon_drops = 1 (read at every map load), weapon_drop_pool = 8 (spares per weapon type),
 *           weapon_drop_ammo = 0 (ammo byte of the spares; 0 = the game's default, MakeWeapon: 3, flamethrower 18);
 *           [modern] enemy_labels = 1 (MOD_ENEMY_LABELS, F10 MODERN tab).
 * ============================================================================================ */
#include "common.h"

#define OBJECTS          (*(BYTE **)0x4e30ec)
#define OBJECT_COUNT     (*(int *)0x4e30e8)
#define OBJECT_SIZE      0xd0
#define CAT_COUNT        ((short *)0x4e318c)              /* objects per category */
#define CAT_NUMBERS      ((short *)0x4e30f0)              /* highest number + 1 per category */
#define CAT_SPECIAL_ITEM 0x19                             /* weapon / medkit pickups: number = weapon type */
#define TROOPER_WEAPONS  ((const int *)0x5042c4)          /* weapon type per trooper type (FUN_00427f80) */
#define TROOPER_TYPES    8
#define GAME_PTR         (*(BYTE **)0x4b3f70)             /* +0x44 game clock */
#define SARGE_RECT       ((const int *)0x4e3014)          /* the_map.sargeboundrect */
#define LEGION_STATUS(a) (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))
#define UNIT_STRIDE      0x2c4

#define O_CELL(o)        (*(int *)((o) + 0x0c))
#define O_WEAPON(o)      (*(BYTE **)((o) + 0x20))
#define O_CATEGORY(o)    (*(short *)((o) + 0x68))
#define O_NUMBER(o)      (*(short *)((o) + 0x6a))
#define O_FLAGS(o)       (*(BYTE *)((o) + 0x70))
#define O_FLAGS_B(o)     (*(BYTE *)((o) + 0x71))
#define O_AMMO_BYTE(o)   (*(BYTE *)((o) + 0x72))
#define O_ARMY(o)        (*(BYTE *)((o) + 0x74))
#define O_DRAW(o)        (*(DWORD *)((o) + 0x7c))
#define O_START_X(o)     (*(short *)((o) + 0x8c))
#define O_START_Y(o)     (*(short *)((o) + 0x8e))
#define O_TIME(o)        (*(DWORD *)((o) + 0xcc))
#define OF_USED          0x04
#define OF_IN_BAG        0x20
#define OF_PLACED        0x40
#define W_TYPE(w)        (*(int *)(w))
#define W_AMMO(w)        (*(short *)((w) + 0x0c))
#define W_MAX(w)         (*(short *)((w) + 0x0e))

#define T_MODE(t)        (*(int *)((t) + 0x68))
#define T_HEALTH(t)      (*(short *)((t) + 0x1c))
#define T_X(t)           (*(short *)((t) + 4))
#define T_Y(t)           (*(short *)((t) + 6))
#define T_WEAPON(t)      (*(BYTE **)((t) + 0x20))
#define UID_ARMY(u)      ((int)(((u) >> 26) & 7) - 1)
#define UID_KIND(u)      ((int)(((u) >> 22) & 0xf) - 1)  /* 4 trooper, 5 sarge */
#define UID_TTYPE(u)     ((int)(((u) >> 16) & 0x3f) - 1) /* trooper type */
#define KIND_TROOPER     4
#define KIND_SARGE       5
#define WT_RIFLE         9

typedef void (__fastcall *VoidFn2)(void);
typedef BYTE *(__fastcall *ArmyFn)(int army);
typedef int (__fastcall *AllianceFn)(int mine, int other);
typedef void (__fastcall *SetPosFn)(BYTE *obj, short *point);
typedef void (__fastcall *DirtyFn)(void *draw, int how);
typedef void *(__cdecl *GameAllocFn)(int size);
typedef void (__cdecl *GameFreeFn)(void *p);
#define ADD_CHARGES      ((VoidFn2)0x40a6b0)              /* the original: mine / explosive charge objects */
#define ARMY_RECORD      ((ArmyFn)0x41a160)
#define ALLIED           ((AllianceFn)0x41a1e0)
#define SET_POSITION     ((SetPosFn)0x449bf0)              /* +4/+6 and the cell +0xc (as the drop FUN_00449190) */
#define OBJ_DIRTY        ((DirtyFn)0x42d4e0)               /* redraw (FUN_00414400's last step) */
#define GAME_ALLOC       ((GameAllocFn)0x4708e0)
#define GAME_FREE        ((GameFreeFn)0x470870)

static const char *kind_name(BYTE *t);
static int g_on = 1;                                       /* [gameplay] weapon_drops, read at each map load */
static int g_spares;                                       /* spares added at the last map load */
static BYTE g_given[256];                                 /* spares already dropped (index from the first spare) */

static int droppable(int wt)
{
    switch (wt) { case 2: case 3: case 4: case 5: case 10: case 11: case 12: case 20: return 1; default: return 0; }
}

static const char *weapon_name(int wt)
{
    switch (wt) {
    case 2: return "GRENADES"; case 3: return "FLAMETHROWER"; case 4: return "BAZOOKA"; case 5: return "MORTAR";
    case 9: return "RIFLE"; case 10: return "AUTO RIFLE"; case 11: return "MINES"; case 12: return "EXPLOSIVES";
    case 20: return "MINESWEEPER"; default: return "?";
    }
}

/* ---------------------------------------------------------------- map load: the spare pickups */

static void __cdecl drops_load_objects(void)
{
    g_on = GetPrivateProfileIntA("gameplay", "weapon_drops", 1, mod_ini_path());
    int pool = GetPrivateProfileIntA("gameplay", "weapon_drop_pool", 8, mod_ini_path());
    if (pool > 32) pool = 32;
    int ammo = GetPrivateProfileIntA("gameplay", "weapon_drop_ammo", 0, mod_ini_path());   /* 0 = the game's default */
    if (ammo < 0 || ammo > 99) ammo = 0;
    int n = OBJECT_COUNT;
    BYTE *old = OBJECTS;
    /* the mission's own pickups (the game's ammo for each: the item byte +0x72, 0 = MakeWeapon's default) */
    if (g_cfg.log >= 1 && old && n > 0 && n < 20000) {
        char s[400]; int k = 0;
        for (int i = 0; i < n && k < (int)sizeof s - 24; i++) {
            BYTE *o = old + i * OBJECT_SIZE;
            if (O_CATEGORY(o) == CAT_SPECIAL_ITEM) k += wsprintfA(s + k, " %d:%d", O_NUMBER(o), O_AMMO_BYTE(o));
        }
        s[k] = 0;
        mlog("drops: map pickups (weapon:ammo byte, 0 = default):%s", k ? s : " none");
    }
    int types[TROOPER_TYPES + 8], nt = 0;
    if (g_on && pool > 0 && old && n > 0 && n < 20000 && !IsBadReadPtr(TROOPER_WEAPONS, TROOPER_TYPES * 4)) {
        char s[160]; int k = 0;
        for (int t = 0; t < TROOPER_TYPES; t++) {
            int wt = TROOPER_WEAPONS[t], dup = 0;
            k += wsprintfA(s + k, " %d", wt);
            for (int j = 0; j < nt; j++) dup |= types[j] == wt;
            if (droppable(wt) && !dup) types[nt++] = wt;
        }
        mlog("drops: trooper type -> weapon:%s", s);
        if (!nt) {                                         /* the table is filled later (0 at map load): the kinds the game has */
            static const int KNOWN[] = { 2, 3, 4, 5, 11, 20 };   /* grenades, flamethrower, bazooka, mortar, mines, minesweeper */
            for (size_t j = 0; j < sizeof KNOWN / sizeof KNOWN[0]; j++) types[nt++] = KNOWN[j];
        }
    }
    int add = nt * pool;
    if (add > (int)sizeof g_given) add = 0;
    g_spares = 0;
    BYTE *grown = add ? (BYTE *)GAME_ALLOC((n + add) * OBJECT_SIZE) : NULL;
    if (add && !grown) mlog("drops: no memory for %d spare pickups - no weapon drops this mission", add);
    if (grown) {
        /* as FUN_0040a6b0 grows the array for the mine charges: new block, old objects copied, old block freed */
        memset(grown, 0, (size_t)(n + add) * OBJECT_SIZE);
        memcpy(grown, old, (size_t)n * OBJECT_SIZE);
        BYTE tmpl_76 = 0;                                  /* damage record (+0x76) of the map's pickups, if any */
        for (int i = 0; i < n; i++) if (O_CATEGORY(old + i * OBJECT_SIZE) == CAT_SPECIAL_ITEM) { tmpl_76 = old[i * OBJECT_SIZE + 0x76]; break; }
        GAME_FREE(old);
        OBJECTS = grown;
        for (int j = 0; j < nt; j++)
            for (int k = 0; k < pool; k++) {
                BYTE *o = grown + (n + j * pool + k) * OBJECT_SIZE;
                O_CATEGORY(o) = CAT_SPECIAL_ITEM;
                O_NUMBER(o) = (short)types[j];
                O_FLAGS(o) = OF_IN_BAG;
                O_FLAGS_B(o) = 0;
                O_ARMY(o) = 0xff;
                o[0x76] = tmpl_76;
                O_AMMO_BYTE(o) = (BYTE)ammo;                /* 0 = the game's default for this pickup (MakeWeapon) */
            }
        OBJECT_COUNT = n + add;
        g_spares = add; memset(g_given, 0, sizeof g_given);
        CAT_COUNT[CAT_SPECIAL_ITEM] = (short)(CAT_COUNT[CAT_SPECIAL_ITEM] + add);
        for (int j = 0; j < nt; j++) if (CAT_NUMBERS[CAT_SPECIAL_ITEM] <= types[j]) CAT_NUMBERS[CAT_SPECIAL_ITEM] = (short)(types[j] + 1);
        char s[120]; int k = 0;
        for (int j = 0; j < nt; j++) k += wsprintfA(s + k, " %s", weapon_name(types[j]));
        mlog("drops: %d spare pickups added (%d each:%s), objects %d -> %d", add, pool, k ? s : "", n, n + add);
    } else if (g_on) mlog("drops: no droppable enemy weapons (or drops off) - nothing added");
    ADD_CHARGES();
}

/* ---------------------------------------------------------------- the drop */

static int objects_ok(void) { return OBJECTS && OBJECT_COUNT > 0 && OBJECT_COUNT < 20000; }

static BYTE *free_spare(int wt)
{
    /* FUN_0040a140 sorts by category keeping the order inside one, and 0x19 is the last category: our spares (appended
     * last) are the last g_spares objects of the array. Their start position is NOT a marker (the loader moves the
     * 0,0 start to 17,20, cell 257 - Wine), so the ones given out are remembered (g_given, per map load). */
    int n = OBJECT_COUNT, first = n - g_spares;
    if (!g_spares || first < 0) return NULL;
    for (int i = first; i < n; i++) {
        BYTE *o = OBJECTS + i * OBJECT_SIZE;
        if (g_given[i - first] || O_CATEGORY(o) != CAT_SPECIAL_ITEM || O_NUMBER(o) != wt) continue;
        if ((O_FLAGS(o) & (OF_USED | OF_PLACED)) || !(O_FLAGS(o) & OF_IN_BAG)) continue;
        BYTE *w = O_WEAPON(o);
        if (!w || IsBadReadPtr(w, 0x20) || W_TYPE(w) != wt) continue;
        g_given[i - first] = 1;
        return o;
    }
    return NULL;
}

static int spot_ok(int x, int y)
{
    const int *r = SARGE_RECT;
    if (x < r[0] + 8 || y < r[1] + 8 || x > r[2] - 8 || y > r[3] - 8) return 0;
    return !controls_sarge_blocked_at(x, y);
}

static int find_spot(int *x, int *y)
{
    static const signed char RING[][2] = { {0,0}, {8,0}, {-8,0}, {0,8}, {0,-8}, {8,8}, {-8,8}, {8,-8}, {-8,-8},
        {16,0}, {-16,0}, {0,16}, {0,-16}, {16,16}, {-16,16}, {16,-16}, {-16,-16}, {24,0}, {-24,0}, {0,24}, {0,-24} };
    for (size_t i = 0; i < sizeof RING / sizeof RING[0]; i++)
        if (spot_ok(*x + RING[i][0], *y + RING[i][1])) { *x += RING[i][0]; *y += RING[i][1]; return 1; }
    return 0;
}

static void place(BYTE *o, int x, int y)
{
    /* Sarge's drop FUN_00449190 (0x449224..0x4492a1) for an item with ammo, minus its two ANIM calls (above) */
    O_FLAGS_B(o) &= (BYTE)~0x40;
    O_FLAGS(o) = (BYTE)((O_FLAGS(o) & ~OF_IN_BAG) | OF_PLACED);
    short p[2] = { (short)x, (short)y };
    SET_POSITION(o, p);
    O_DRAW(o) |= 1;                                        /* FUN_00414770 */
    *(short *)(o + 0x90) = 10; *(short *)(o + 0x92) = 10;   /* FUN_00414400: old draw position = the drop's 10,10 */
    O_START_X(o) = (short)x; O_START_Y(o) = (short)y;
    OBJ_DIRTY(&O_DRAW(o), 0);
    O_TIME(o) = GAME_PTR ? *(DWORD *)(GAME_PTR + 0x44) : 0;
}

static void drop_for(BYTE *t, DWORD uid, int x, int y)
{
    BYTE *w = T_WEAPON(t);
    int wt = w && !IsBadReadPtr(w, 0x10) ? W_TYPE(w) : -1;
    if (!droppable(wt)) return;                              /* riflemen keep their rifle */
    int ox = x, oy = y;
    if (!find_spot(&x, &y)) { mlog("drops: %s soldier died at %d,%d - no spot to stand on there (water / blocked / edge), no drop", weapon_name(wt), ox, oy); return; }
    BYTE *o = free_spare(wt);
    if (!o && g_cfg.log >= 2) {
        int shown = 0;
        for (int i = 0; i < OBJECT_COUNT && shown < 12; i++) {
            BYTE *q = OBJECTS + i * OBJECT_SIZE, *qw;
            if (O_CATEGORY(q) != CAT_SPECIAL_ITEM || O_NUMBER(q) != wt) continue;
            qw = O_WEAPON(q); shown++;
            mlog("drops:   #%d flags %02x %02x cell %d start %d,%d pos %d,%d weapon %p ammo %d/%d", i, O_FLAGS(q), O_FLAGS_B(q), O_CELL(q),
                 O_START_X(q), O_START_Y(q), *(short *)(q + 4), *(short *)(q + 6), qw, qw ? W_AMMO(qw) : 0, qw ? W_MAX(qw) : 0);
        }
    }
    if (!o) { mlog("drops: %s soldier died at %d,%d - no spare %s pickup left, no drop", weapon_name(wt), x, y, weapon_name(wt)); return; }
    place(o, x, y);
    BYTE *ow = O_WEAPON(o);
    mlog("drops: army %d %s soldier died at %d,%d -> %s pickup #%d at %d,%d, ammo %d", UID_ARMY(uid), weapon_name(wt), ox, oy,
         weapon_name(wt), (int)((o - OBJECTS) / OBJECT_SIZE), x, y, W_AMMO(ow));
}

#define MAX_TRACK 768
static struct { BYTE *t; DWORD uid; short x, y; } g_alive[MAX_TRACK], g_now[MAX_TRACK];
static int g_nalive;
static unsigned g_run;

static int enemy_army(int a) { return a >= 1 && a <= 3 && LEGION_STATUS(a) && !ALLIED(0, a); }

void drops_frame(void)
{
    unsigned run = gameplay_run(NULL, NULL);
    if (run != g_run) { g_run = run; g_nalive = 0; }
    if (!g_on || !objects_ok()) { g_nalive = 0; return; }
    int nn = 0;
    for (int a = 1; a <= 3; a++) {
        if (!enemy_army(a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg) continue;
        BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
        for (int guard = 0; un && un < end && guard < 64; un += UNIT_STRIDE, guard++) {
            int mode = *(int *)(un + 0x70);
            if (mode < 3 || mode > 5) continue;
            int last = *(int *)(un + 0xdc);
            if (last < 0 || last > 9) last = 9;
            for (int i = 0; i <= last && nn < MAX_TRACK; i++) {
                BYTE *t = ((BYTE **)(un + 0xb4))[i];
                if (!t || T_MODE(t) != 1 || T_HEALTH(t) <= 0) continue;
                g_now[nn].t = t; g_now[nn].uid = *(DWORD *)t; g_now[nn].x = T_X(t); g_now[nn].y = T_Y(t); nn++;
            }
        }
    }
    static unsigned listed;                                  /* once per run (log >= 2): the enemy soldiers by kind */
    if (g_cfg.log >= 2 && listed != run && nn) {
        listed = run;
        for (int j = 0; j < nn && j < 80; j++) {
            BYTE *t = g_now[j].t, *w = T_WEAPON(t);
            mlog("drops:   enemy army %d trooper type %d (%s) weapon %d at %d,%d", UID_ARMY(g_now[j].uid), UID_TTYPE(g_now[j].uid),
                 kind_name(t), w ? W_TYPE(w) : -1, g_now[j].x, g_now[j].y);
        }
    }
    static DWORD seen_ms;                                   /* log >= 3, every 2 s: enemies near Sarge on screen (testing) */
    if (g_cfg.log >= 3 && GetTickCount() - seen_ms > 2000 && *(BYTE **)0x697520) {
        BYTE *sg = *(BYTE **)0x697520; int cx = *(short *)0x4e2de8, cy = *(short *)0x4e2dea;
        seen_ms = GetTickCount();
        mlog("drops: sarge at %d,%d screen %d,%d", T_X(sg), T_Y(sg), T_X(sg) - cx, T_Y(sg) - cy - layout_field_shift());
        for (int j = 0; j < nn; j++) {
            int dx = g_now[j].x - T_X(sg), dy = g_now[j].y - T_Y(sg);
            if (dx * dx + dy * dy < 250 * 250) mlog("drops:   near: %s at %d,%d screen %d,%d", kind_name(g_now[j].t), g_now[j].x, g_now[j].y, g_now[j].x - cx, g_now[j].y - cy - layout_field_shift());
        }
    }
    /* who was alive last frame and is not now: dead (health <= 0, same soldier) -> drop */
    for (int i = 0; i < g_nalive; i++) {
        int still = 0;
        for (int j = 0; j < nn && !still; j++) still = g_now[j].t == g_alive[i].t;
        if (still) continue;
        BYTE *t = g_alive[i].t;
        if (IsBadReadPtr(t, 0x284) || *(DWORD *)t != g_alive[i].uid || T_HEALTH(t) > 0) continue;   /* gone another way */
        int x = T_X(t), y = T_Y(t);
        if (x <= 0 || y <= 0 || x > 4095 || y > 4095) { x = g_alive[i].x; y = g_alive[i].y; }
        drop_for(t, g_alive[i].uid, x, y);
    }
    memcpy(g_alive, g_now, sizeof g_now[0] * (size_t)nn);
    g_nalive = nn;
}

/* ---------------------------------------------------------------- the hover label */

static const char *kind_name(BYTE *t)
{
    BYTE *w = T_WEAPON(t);
    int wt = w && !IsBadReadPtr(w, 0x10) ? W_TYPE(w) : -1;
    switch (wt) {
    case 2: return "GRENADIER"; case 3: return "FLAMETHROWER"; case 4: return "BAZOOKA"; case 5: return "MORTARMAN";
    case 1: case 9: return "RIFLEMAN"; case 10: return "AUTO RIFLEMAN"; case 11: return "MINER"; case 12: return "DEMOLITIONS";
    case 20: return "MINESWEEPER";
    }
    static const char *BY_TYPE[TROOPER_TYPES] = { "RIFLEMAN", "GRENADIER", "FLAMETHROWER", "BAZOOKA", "MINESWEEPER",
                                                  "MORTARMAN", "MINER", "SPECIAL" };
    int tt = UID_TTYPE(*(DWORD *)t);
    return tt >= 0 && tt < TROOPER_TYPES ? BY_TYPE[tt] : "SOLDIER";
}

/* The enemy soldier under the fire pointer: his kind, and the top middle of his sprite (world px). */
int drops_hover_label(const char **name, int *wx, int *wy)
{
    BYTE *t = controls_hover_target();
    if (!t || IsBadReadPtr(t, 0x284)) return 0;
    DWORD uid = *(DWORD *)t;
    int kind = UID_KIND(uid);
    if ((kind != KIND_TROOPER && kind != KIND_SARGE) || !enemy_army(UID_ARMY(uid)) || T_HEALTH(t) <= 0) return 0;
    *name = kind == KIND_SARGE ? "SARGE" : kind_name(t);
    *wx = T_X(t); *wy = T_Y(t) - 28;
    BYTE *av = kind == KIND_TROOPER ? *(BYTE **)(t + 0x26c) : NULL;
    BYTE *an = av && !IsBadReadPtr(av, 0x20) ? *(BYTE **)(av + 0x1c) : NULL;
    if (an && !IsBadReadPtr(an, 0x2c)) {
        const int *r = (const int *)(an + 0x1c);
        if (r[2] > r[0] && r[3] > r[1] && r[3] - r[1] < 200) { *wx = (r[0] + r[2]) / 2; *wy = r[1]; }
    }
    return 1;
}

void drops_install(void)
{
    hook_call("drops: map load - spare weapon pickups (call 0x40b745 -> FUN_0040a6b0)", 0x40b745, 0x40a6b0, (void *)drops_load_objects);
}
