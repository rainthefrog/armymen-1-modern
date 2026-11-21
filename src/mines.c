/* ============================================================================================
 * mines.c - mine placement with the mouse (2026-11-21, user: "make it show a radius around sarge that I can
 * place mines and then I can left click to place them in the radius")
 *
 * How the game lays a mine (exe, checked in the disassembly):
 *   - The mine weapon (type 11) is an aim-type weapon (WEAPON +0x26 = 1, MakeWeapon 0x446190): pressing fire puts
 *     Sarge in his aiming state (4), RELEASING it plays the shot / laying animation (state 7, FUN_004474c0), whose
 *     shot frame calls FUN_00447c30 (fire) -> FUN_00445c70 (projectile per weapon type) -> case 11 ->
 *     call 0x445f44 -> FUN_00446600 MineLay. FUN_00447c30 takes one ammo off afterwards (and drops the empty
 *     weapon at 0).
 *   - The spot is the MUZZLE point of the laying frame (FUN_00447a80: Sarge's position + the frame's hand point),
 *     snapped to the middle of its 16 px tile (FUN_0042ffa0: (v & ~15) + 8).
 *   - The mines themselves are map objects made when the map loads (OBJECTSTRUCT category 9 "weapon object",
 *     number 11) and kept "in the bag" (+0x70 bit 0x20) until laid. MakeWeapon takes as many free ones as the
 *     weapon's ammo (99 at most) into the weapon's charge array (WEAPON +0x1c = short[nMaxAmmo] of object
 *     numbers, owner WEAPON* in the object's +0x20). MineLay takes the first charge still in the bag (0x20 set,
 *     0x04 "used" clear), and FUN_00446530 puts it on the map: position +4/+8c, cell +0xc, owner army +0x74, laid
 *     at +0xcc (game clock), 0x20 cleared, 0x40 set; an enemy's mine is hidden (+0x70 0x10, +0x7c 0x40000000)
 *     unless the Omniscient cheat is on. FUN_00437640 plays the laying sound (0x14) there.
 *   - A laid mine goes off when anything moves onto its tile (FUN_00439800 -> FUN_004398d0) once it is older than
 *     6000 ms (0x1770) - Sarge too, his own mines are not safe for him. Sarge with the minesweeper (type 20)
 *     selected reveals mines in the 20 tiles round his muzzle tile and, with fire held, disarms them
 *     (FUN_004399d0 / FUN_00439bb0; minesweeper research in the task report).
 *
 * What the mod does (modern controls, on foot, mines selected):
 *   - a placement ring of MINE_REACH px round Sarge and a marker at the pointer's tile when the pointer is inside
 *     it (modern.c draws both from mines_preview): red X = a mine goes there, grey X + the reason = not there
 *     (a blocked tile / water - the game's own Sarge test -, the way from Sarge to the spot blocked, another mine
 *     too close, no mines left);
 *   - a left click inside the ring (controls.c: a tap that would have been a walk) lays the mine THERE: Sarge stops,
 *     turns to the spot and lays it with the game's own routine - the fire press / release below, so the game's
 *     aiming / laying animation, sound, ammo count, charge array and the empty weapon drop are all its own. Only
 *     the spot changes: the wrap of the call 0x445f44 hands MineLay the clicked point instead of the muzzle
 *     point (MineLay snaps it to the tile itself). A click outside the ring is the normal click (walk); a hold
 *     inside it walks as before; keyboard / right-button fire lays at the muzzle as the original;
 *   - why not walk Sarge to the spot and lay at his feet: the game never lays at the feet but at the hand point
 *     of the animation frame (in front of him, direction-dependent), so walking there would still miss the
 *     clicked tile; and MineLay itself only takes a point - nothing in it depends on where Sarge stands - so
 *     giving it the clicked point is the game's own creation with one argument changed.
 *
 * Rewrites / wraps:
 *   call 0x445f44 (in FUN_00445c70, case 11) -> mines_lay. Callee FUN_00446600 MineLay: __fastcall ecx = WEAPON*,
 *   edx = army index, stack = SPOINT packed (x low word, y high word), ret 4. Kept: the original is always called
 *   (any unit, any weapon); changed: for Sarge's own current weapon with a placement asked by a click, the point.
 * No other game patch. Switch: [controls] mine_click = 1 (default; 0 = the original behaviour, nothing drawn).
 *
 * MINESWEEPER (2026-11-21, user picked rework proposal 2; [controls] sweeper_click = 1, 0 = only the original sweeper)
 * The original (exe): weapon type 20, special, unlimited. Only while SELECTED: every avatar update scans the 20 tiles
 * round the muzzle tile (FUN_004399d0 -> FUN_00439bb0) and reveals hidden mines (+0x70 &= ~0x10, +0x7c &=
 * ~0x40000000, FUN_0042d4e0(&+0x7c, 1)); with FIRE HELD it disarms them: FUN_0040d500 (effect 0x7f at the mine),
 * FUN_0044da60(x, y, 0) (2 s marker), FUN_00446390 (sound + FUN_004463b0: off the map with FUN_0040e560, +0x70 |=
 * 0x64 = used, FUN_0042c880(old cell) refreshes the AI's mine cells). A unit of an army whose Sarge holds the
 * sweeper never sets a mine off (FUN_004398d0) - kept as it is. The original hold-fire disarm is kept too.
 * Added here (no game patch, all from the per-frame controls and the overlay):
 *   - detector: while Sarge CARRIES the sweeper (any of the six boxes, selected or not - weapons_carried) every laid
 *     mine within SWEEP_RADIUS of him is revealed the game's way; a newly revealed one beeps (the game's
 *     WEAPON_ACTIVATE click, FUN_00403030, rate-limited); with the sweeper SELECTED it also beeps faster the nearer
 *     the closest mine (a metal detector). The radius is drawn faintly while it is carried (that is where it works),
 *     laid mines in view get rings (enemy red, own green), the plate in front of Sarge sends out pulses while
 *     selected, the HUD box of the sweeper gets a lamp + the count in range, the pointer a small detector pip;
 *   - click to disarm: a left tap on a visible laid mine (any distance) walks Sarge along the normal click route to
 *     a point DISARM_SHORT px short of it and, within DISARM_REACH, runs the game's disarm effects (0x40d500,
 *     0x44da60) and COLLECTS the mine instead of destroying it: off the map with FUN_0040e560 (as FUN_004463b0, but
 *     without the "used" bit), back in the bag (0x20 set, 0x04 / 0x40 / hidden clear), cells refreshed, and put in
 *     Sarge's mine weapon: his own mine goes back into its own slot; another mine refills a slot whose mine is
 *     used up (the two object numbers are swapped between the arrays, so the old owner's array never holds a live
 *     charge twice); ammo + 1. No mine weapon carried: a used-up (emptied, dropped) mine pack of this map is revived
 *     with that one charge and picked up through the game's own pickup (0x4495c0 -> weapons.c) - nothing new is
 *     created (no free object / record to make one from). Full (ammo == max or no used slot), no pack to revive or
 *     no free box: disarmed and destroyed as the original (FUN_00446390). Enemy and own mines both count.
 * ============================================================================================ */
#include "common.h"
#include <math.h>

#define SARGE            (*(BYTE **)0x697520)
#define SARGE_X(u)       (*(short *)((u) + 0x04))
#define SARGE_Y(u)       (*(short *)((u) + 0x06))
#define SARGE_WEAPON(u)  (*(BYTE **)((u) + 0x20))     /* the current WEAPON record */
#define SARGE_STATE(u)   (*(int *)((u) + 0x68))       /* 4 aiming, 7 shot / laying animation */
#define SARGE_VEHICLE(u) (*(int *)((u) + 0x2d0))
#define UID_ARMY(u)      ((int)((*(DWORD *)(u) >> 26) & 7) - 1)
#define STATE_AIMING     4
#define MENU_CURSOR_SHOWN (*(int *)0x4fe448)

#define INPUT              (*(volatile DWORD *)0x6a547c)   /* the player input word (controls.c) */
#define INPUT_FIRE         0x00001
#define INPUT_FIRE_PRESSED 0x10000

/* WEAPON record (MakeWeapon 0x446190) */
#define W_TYPE(w)        (*(int *)(w))
#define W_AMMO(w)        (*(short *)((w) + 0x0c))
#define W_MAX(w)         (*(short *)((w) + 0x0e))
#define W_CHARGES(w)     (*(short **)((w) + 0x1c))    /* mines / explosives: object numbers of the charges */
#define TYPE_MINE        11

/* the map's objects (OBJECTSTRUCT, 0xd0 bytes each) */
#define OBJECTS          (*(BYTE **)0x4e30ec)
#define OBJECT_COUNT     (*(int *)0x4e30e8)
#define OBJECT_SIZE      0xd0
#define OBJ_X(o)         (*(short *)((o) + 0x04))
#define OBJ_Y(o)         (*(short *)((o) + 0x06))
#define OBJ_CATEGORY(o)  (*(short *)((o) + 0x68))
#define OBJ_NUMBER(o)    (*(short *)((o) + 0x6a))
#define OBJ_FLAGS(o)     (*(BYTE *)((o) + 0x70))
#define OBJ_ARMY(o)      (*(signed char *)((o) + 0x74))
#define CAT_WEAPON_OBJ   9                            /* mines (number 11) and explosives (12) */
#define OF_USED          0x04                         /* gone off / disarmed */
#define OF_HIDDEN        0x10                         /* not drawn (an enemy's mine) */
#define OF_IN_BAG        0x20                         /* not on the map (a free charge) */

typedef void (__fastcall *MineLayFn)(BYTE *weapon, int army, DWORD point);
#define MINE_LAY         ((MineLayFn)0x446600)
#define TILE             16                           /* MineLay snaps to the middle of a 16 px tile */

/* the placement rules (world px) */
#define MINE_REACH       80      /* ring radius round Sarge: about two body lengths (a soldier is ~24 px) */
#define MINE_SPACING     16      /* another mine this close to the tile's middle (= the same tile): no */
#define MINE_LINE_FROM   10      /* the way to the spot is tested from here out (Sarge's own spot may overlap) */
#define MINE_LINE_STEP   4
#define PRESS_MIN_MS     60      /* fire held at least this long (the game must see the press) ... */
#define PRESS_MAX_MS     600     /* ... and until Sarge aims, at most this long; the release lays the mine */
#define WAIT_LAY_MS      2000    /* no mine laid this long after the release: the request is dropped */

static int g_on = -1;
static int enabled(void)
{
    if (g_on < 0) g_on = GetPrivateProfileIntA("controls", "mine_click", 1, mod_ini_path()) != 0;
    return g_on && g_cfg.controls_modern;
}

/* a placement asked by a click: the press / release that makes the game lay it, then MineLay takes the point */
static struct {
    int phase;                   /* 0 none, 1 fire pressed, 2 released - waiting for MineLay */
    int x, y;                    /* tile middle (world) */
    DWORD at;                    /* phase start */
    int frames;                  /* frames in phase 1 */
    BYTE *sarge, *weapon;
} R;

static BYTE *mine_weapon(BYTE *u)
{
    if (!u || SARGE_VEHICLE(u)) return NULL;
    BYTE *w = SARGE_WEAPON(u);
    if (!w || IsBadReadPtr(w, 0x20) || W_TYPE(w) != TYPE_MINE) return NULL;
    return w;
}

static BYTE *object(int i) { return OBJECTS + (size_t)i * OBJECT_SIZE; }

/* free charges in the weapon's array (MineLay's own test) */
static int free_charges(BYTE *w, int *total)
{
    short *c = W_CHARGES(w);
    int n = W_MAX(w), f = 0;
    if (total) *total = n;
    if (!c || n <= 0 || !OBJECTS || IsBadReadPtr(c, (UINT_PTR)n * 2)) return 0;
    for (int i = 0; i < n; i++) {
        if (c[i] < 0 || c[i] >= OBJECT_COUNT) continue;
        BYTE f8 = OBJ_FLAGS(object(c[i]));
        if ((f8 & OF_IN_BAG) && !(f8 & OF_USED)) f++;
    }
    return f;
}

/* a laid mine that the player can see within MINE_SPACING of (x, y) (an enemy's hidden mine is not given away) */
static int mine_near(int x, int y)
{
    BYTE *o = OBJECTS;
    int n = OBJECT_COUNT;
    if (!o || n <= 0 || n > 20000) return 0;
    for (int i = 0; i < n; i++, o += OBJECT_SIZE) {
        if (OBJ_CATEGORY(o) != CAT_WEAPON_OBJ || OBJ_NUMBER(o) != TYPE_MINE) continue;
        BYTE f = OBJ_FLAGS(o);
        if (f & (OF_IN_BAG | OF_USED | OF_HIDDEN)) continue;
        int dx = OBJ_X(o) - x, dy = OBJ_Y(o) - y;
        if (dx * dx + dy * dy < MINE_SPACING * MINE_SPACING) return 1;
    }
    return 0;
}

static int snap(int v) { return (v & ~(TILE - 1)) + TILE / 2; }

/* Can a mine go on the tile under (wx, wy)? *tx, *ty = the tile's middle. */
static int spot_state(BYTE *u, BYTE *w, int wx, int wy, int *tx, int *ty)
{
    int sx = SARGE_X(u), sy = SARGE_Y(u);
    *tx = snap(wx); *ty = snap(wy);
    long dx = wx - sx, dy = wy - sy;
    if (dx * dx + dy * dy > (long)MINE_REACH * MINE_REACH) return MINE_OUT;
    if (W_AMMO(w) <= 0 || free_charges(w, NULL) <= 0) return MINE_EMPTY;
    if (*tx <= 0 || *ty <= 0 || controls_sarge_blocked_at(*tx, *ty)) return MINE_BLOCKED;   /* the game's test: tile + objects */
    {   /* the way from Sarge to the spot (no mines laid through a wall / fence) */
        double lx = *tx - sx, ly = *ty - sy, len = sqrt(lx * lx + ly * ly);
        for (double d = MINE_LINE_FROM; d < len; d += MINE_LINE_STEP)
            if (controls_sarge_blocked_at(sx + (int)lround(lx * d / len), sy + (int)lround(ly * d / len))) return MINE_NO_WAY;
    }
    if (mine_near(*tx, *ty)) return MINE_CLOSE;
    return MINE_OK;
}

static int sweeper_active(BYTE *u);                /* minesweeper (below) */
static BYTE *mine_at(int wx, int wy);

static int active_for(BYTE *u)
{
    return enabled() && u && layout_view_is_gameplay() && !MENU_CURSOR_SHOWN && mine_weapon(u) != NULL;
}

int mines_preview(MinePreview *p)
{
    BYTE *u = SARGE;
    if (!active_for(u)) return 0;
    BYTE *w = mine_weapon(u);
    p->sarge_x = SARGE_X(u); p->sarge_y = SARGE_Y(u);
    p->reach = MINE_REACH;
    p->left = W_AMMO(w);
    p->busy = R.phase != 0;
    p->state = MINE_OUT;
    int wx, wy;
    if (controls_pointer_world(&wx, &wy)) p->state = spot_state(u, w, wx, wy, &p->x, &p->y);
    if (p->state != MINE_OUT && sweeper_active(u) && mine_at(wx, wy)) p->state = MINE_OUT;   /* on a mine: the sweeper's disarm pointer */
    return 1;
}

/* ---------------------------------------------------------------- minesweeper */

#define TYPE_SWEEPER     20
#define CAT_PICKUP       0x19                          /* a weapon item lying on the map / carried (+0x20 = WEAPON*) */
#define OBJ_CELL(o)      (*(int *)((o) + 0x0c))        /* map cell index, 0 = not on the map */
#define OBJ_OWNER(o)     (*(BYTE **)((o) + 0x20))      /* mine: the WEAPON whose charge it is; pickup: its WEAPON */
#define OBJ_FLAGS2(o)    (*(DWORD *)((o) + 0x7c))
#define OF_PLACED        0x40
#define OF2_HIDDEN       0x40000000                    /* an enemy's mine not seen yet */

typedef void (__fastcall *ObjFn)(BYTE *obj);
typedef void (__fastcall *MarkFn)(int x, int y, int kind);
typedef void (__fastcall *CellFn)(int cell);
typedef void (__fastcall *DirtyFn)(DWORD *flags2, int on);
typedef void (__fastcall *SoundFn)(int id, void *where);
typedef void (__fastcall *PickupFn)(BYTE *unit, BYTE *item, int arg);
typedef void (__fastcall *MuzzleFn)(BYTE *unit, short *out);
#define DISARM_EFFECT    ((ObjFn)0x40d500)             /* effect 0x7f at the object (FUN_00439bb0's disarm) */
#define DISARM_MARK      ((MarkFn)0x44da60)            /* 2 s marker at x, y (ecx, edx, stack 0; ret 4) */
#define MINE_DESTROY     ((ObjFn)0x446390)             /* the original disarm: sound, off the map, used */
#define OFF_THE_MAP      ((ObjFn)0x40e560)             /* position / cell cleared, 0x60 set, redraw */
#define MINE_CELLS       ((CellFn)0x42c880)            /* the AI's mine cells round a cell, made again */
#define OBJ_DIRTY        ((DirtyFn)0x42d4e0)           /* as FUN_00439bb0 after a reveal: ecx = &+0x7c, edx = 1 */
#define PLAY_SOUND       ((SoundFn)0x403030)           /* ecx = sound id, edx = position source (NULL = full volume) */
#define PICK_UP          ((PickupFn)0x4495c0)          /* Sarge walks over an item (weapons.c wraps it) */
#define MUZZLE_POINT     ((MuzzleFn)0x447a80)          /* the sweeper plate / hand point of this animation frame */
#define SND_WEAPON_ACTIVATE 1040                      /* game table WEAPON_ACTIVATE: a short click (the detector beep) */

#define SWEEP_RADIUS     96      /* detector reach round Sarge (world px; the original scans ~2 tiles round the plate) */
#define DISARM_REACH     28      /* within this of the mine Sarge disarms it */
#define DISARM_SHORT     18      /* the walk ends this far short of the mine (its tile is 16 px: never onto it) */
#define MINE_HIT_R       11      /* a click / hover this close to a mine is on it */
#define DISARM_TIMEOUT_MS 20000
#define DISARM_IDLE_MS   600     /* not walking and not in reach this long: given up */
#define BEEP_NEW_GAP_MS  300     /* newly revealed mines: at most one beep this often */
#define BEEP_FAR_MS      1400    /* selected sweeper: beep period with the nearest mine at the radius ... */
#define BEEP_NEAR_MS     250     /* ... and right next to it */

static int g_sw_on = -1;
static int sweeper_enabled(void)
{
    if (g_sw_on < 0) g_sw_on = GetPrivateProfileIntA("controls", "sweeper_click", 1, mod_ini_path()) != 0;
    return g_sw_on && g_cfg.controls_modern;
}

static struct {                 /* this frame (mines_frame) */
    int carried, selected, count, nearest, plate_x, plate_y;
    DWORD beep_at, new_beep_at;
} SW;
static struct { int obj; DWORD at, idle_since; } D = { -1 };   /* a disarm walk in progress */

static int laid_mine(BYTE *o)
{
    return OBJ_CATEGORY(o) == CAT_WEAPON_OBJ && OBJ_NUMBER(o) == TYPE_MINE && !(OBJ_FLAGS(o) & (OF_IN_BAG | OF_USED)) && OBJ_CELL(o);
}
static int objects_ok(void) { return OBJECTS && OBJECT_COUNT > 0 && OBJECT_COUNT < 20000; }
static int dist(int x0, int y0, int x1, int y1) { double dx = x1 - x0, dy = y1 - y0; return (int)sqrt(dx * dx + dy * dy); }
static int obj_index(BYTE *o) { return (int)((o - OBJECTS) / OBJECT_SIZE); }

static int sweeper_active(BYTE *u)
{
    return sweeper_enabled() && u && !SARGE_VEHICLE(u) && layout_view_is_gameplay() && !MENU_CURSOR_SHOWN && objects_ok() &&
           weapons_carried(TYPE_SWEEPER, NULL) != NULL;
}

/* the visible laid mine nearest (wx, wy) within MINE_HIT_R, or NULL */
static BYTE *mine_at(int wx, int wy)
{
    BYTE *best = NULL; int bd = MINE_HIT_R + 1;
    for (int i = 0; i < OBJECT_COUNT; i++) {
        BYTE *o = object(i);
        if (!laid_mine(o) || (OBJ_FLAGS(o) & OF_HIDDEN)) continue;
        int d = dist(wx, wy, OBJ_X(o), OBJ_Y(o));
        if (d < bd) { bd = d; best = o; }
    }
    return best;
}

static int charge_slot(BYTE *w, BYTE *o)          /* index of the object in the weapon's charge array, or -1 */
{
    short *c = w ? W_CHARGES(w) : NULL;
    int n = w ? W_MAX(w) : 0, id = obj_index(o);
    if (!c || n <= 0 || IsBadReadPtr(c, (UINT_PTR)n * 2)) return -1;
    for (int i = 0; i < n; i++) if (c[i] == id) return i;
    return -1;
}
/* a slot whose mine is used up (gone off / disarmed), or -1; bag_too: also a charge still in the bag (an emptied pack:
 * its ammo is 0, so those charges are out of reach anyway) */
static int spare_slot(BYTE *w, int bag_too)
{
    short *c = W_CHARGES(w);
    int n = W_MAX(w);
    if (!c || n <= 0 || IsBadReadPtr(c, (UINT_PTR)n * 2)) return -1;
    for (int pass = 0; pass < (bag_too ? 2 : 1); pass++)         /* used-up slots first */
        for (int i = 0; i < n; i++) {
            if (c[i] < 0 || c[i] >= OBJECT_COUNT) continue;
            BYTE f = OBJ_FLAGS(object(c[i]));
            if (pass == 0 ? (f & OF_USED) != 0 : (f & OF_IN_BAG) != 0) return i;
        }
    return -1;
}
static int used_slot(BYTE *w) { return spare_slot(w, 0); }

/* An emptied mine pack of this map (dropped at 0 ammo: the game marks it used, its record and array stay), not
 * carried - the only way to hand Sarge a mine weapon without making objects the game does not know. */
static BYTE *dead_pack(void)
{
    for (int i = 0; i < OBJECT_COUNT; i++) {
        BYTE *o = object(i), *w = OBJ_OWNER(o);
        if (OBJ_CATEGORY(o) != CAT_PICKUP || !w || IsBadReadPtr(w, 0x20) || W_TYPE(w) != TYPE_MINE) continue;
        if (!(OBJ_FLAGS(o) & OF_USED) || W_AMMO(w) > 0 || W_MAX(w) <= 0 || spare_slot(w, 1) < 0) continue;
        return o;
    }
    return NULL;
}

/* what a disarm of mine o would do */
static int sweep_plan(BYTE *o)
{
    BYTE *w = weapons_carried(TYPE_MINE, NULL);
    if (w) {
        if (W_AMMO(w) < 0 || W_AMMO(w) >= W_MAX(w)) return SWEEP_FULL;
        if (charge_slot(w, o) >= 0 || spare_slot(w, free_charges(w, NULL) > W_AMMO(w)) >= 0) return SWEEP_COLLECT;
        return SWEEP_FULL;
    }
    if (!dead_pack()) return SWEEP_NO_PACK;
    if (!weapons_room_for_special()) return SWEEP_NO_ROOM;
    return SWEEP_COLLECT;
}

/* Off the map, back in the bag (FUN_004463b0 without the "used" bit). */
static void bag_mine(BYTE *o)
{
    int cell = OBJ_CELL(o);
    if (cell) OFF_THE_MAP(o);
    OBJ_FLAGS(o) = (BYTE)((OBJ_FLAGS(o) | OF_IN_BAG) & ~(OF_USED | OF_PLACED | OF_HIDDEN));
    OBJ_FLAGS2(o) &= ~OF2_HIDDEN;
    if (cell) MINE_CELLS(cell);
}

/* Put mine o into weapon w: its own slot, or swapped into slot k (the used-up object goes to o's old owner). */
static int put_in(BYTE *w, BYTE *o, int *slot, int bag_too)
{
    int k = charge_slot(w, o);
    if (k < 0) {
        k = spare_slot(w, bag_too);
        if (k < 0) return 0;
        short *c = W_CHARGES(w);
        BYTE *spent = object(c[k]), *old = OBJ_OWNER(o);
        int j = old && old != w && !IsBadReadPtr(old, 0x20) && W_TYPE(old) == TYPE_MINE ? charge_slot(old, o) : -1;
        if (j >= 0) { W_CHARGES(old)[j] = (short)obj_index(spent); OBJ_OWNER(spent) = old; }
        c[k] = (short)obj_index(o);
        OBJ_OWNER(o) = w;
    }
    *slot = k;
    return 1;
}

/* Disarm mine o (Sarge is next to it): the game's disarm effects, then collected - or destroyed as the original. */
static void sweep_take(BYTE *u, BYTE *o)
{
    if (!laid_mine(o)) return;
    int x = OBJ_X(o), y = OBJ_Y(o), army = OBJ_ARMY(o), plan = sweep_plan(o), k = -1;
    DISARM_EFFECT(o);
    DISARM_MARK(x, y, 0);
    BYTE *w = weapons_carried(TYPE_MINE, NULL), *pack = NULL;
    int ammo = w ? W_AMMO(w) : 0;
    if (plan == SWEEP_COLLECT && !w) {                  /* revive an emptied pack with this one charge */
        pack = dead_pack();
        w = pack ? OBJ_OWNER(pack) : NULL;
        if (w && put_in(w, o, &k, 1)) {
            W_AMMO(w) = 1;
            OBJ_FLAGS(pack) &= (BYTE)~OF_USED;
            PICK_UP(u, pack, 0);
            if (weapons_carried(TYPE_MINE, NULL) != w) {  /* refused: undo, destroy as the original */
                OBJ_FLAGS(pack) |= OF_USED; W_AMMO(w) = 0;
                mlog("sweeper: the revived mine pack #%d was not taken - mine destroyed", obj_index(pack));
                plan = SWEEP_NO_ROOM;
            }
        } else plan = SWEEP_NO_PACK;
    } else if (plan == SWEEP_COLLECT) {
        if (put_in(w, o, &k, free_charges(w, NULL) > ammo)) W_AMMO(w) = (short)(ammo + 1); else plan = SWEEP_FULL;
    }
    if (plan != SWEEP_COLLECT) {
        MINE_DESTROY(o);
        mlog("sweeper: mine #%d at %d,%d (army %d) disarmed and destroyed (%s)", obj_index(o), x, y, army,
             plan == SWEEP_FULL ? "mine weapon full" : plan == SWEEP_NO_ROOM ? "no free box" : "no mine weapon / pack");
        return;
    }
    bag_mine(o);
    mlog("sweeper: mine #%d at %d,%d (army %d) disarmed and COLLECTED into %s slot %d of %d, ammo %d -> %d, free charges %d",
         obj_index(o), x, y, army, pack ? "a revived mine pack," : "Sarge's mine weapon,", k, W_MAX(w), pack ? 0 : ammo, W_AMMO(w),
         free_charges(w, NULL));
}

/* every frame: the detector (reveal + beeps), the disarm walk */
static int sweeper_frame(BYTE *u, DWORD now, int walking)
{
    int stop = 0;
    SW.carried = sweeper_active(u);
    if (!SW.carried) { SW.count = 0; D.obj = -1; return 0; }
    SW.selected = SARGE_WEAPON(u) && W_TYPE(SARGE_WEAPON(u)) == TYPE_SWEEPER;
    if (SW.selected) { short m[2]; MUZZLE_POINT(u, m); SW.plate_x = m[0]; SW.plate_y = m[1]; }
    int sx = SARGE_X(u), sy = SARGE_Y(u), count = 0, nearest = 1 << 30, fresh = 0;
    for (int i = 0; i < OBJECT_COUNT; i++) {
        BYTE *o = object(i);
        if (!laid_mine(o)) continue;
        int d = dist(sx, sy, OBJ_X(o), OBJ_Y(o));
        if (d > SWEEP_RADIUS) continue;
        count++;
        if (d < nearest) nearest = d;
        if ((OBJ_FLAGS(o) & OF_HIDDEN) || (OBJ_FLAGS2(o) & OF2_HIDDEN)) {     /* the game's reveal (FUN_00439bb0) */
            OBJ_FLAGS2(o) &= ~OF2_HIDDEN;
            OBJ_FLAGS(o) &= (BYTE)~OF_HIDDEN;
            OBJ_DIRTY(&OBJ_FLAGS2(o), 1);
            fresh++;
            mlog("sweeper: revealed mine #%d at %d,%d (army %d), %d px from Sarge", i, OBJ_X(o), OBJ_Y(o), OBJ_ARMY(o), d);
        }
    }
    SW.count = count; SW.nearest = count ? nearest : -1;
    if (fresh && now - SW.new_beep_at >= BEEP_NEW_GAP_MS) {
        PLAY_SOUND(SND_WEAPON_ACTIVATE, NULL); SW.new_beep_at = SW.beep_at = now;
        if (g_cfg.log >= 2) mlog("sweeper: beep (new mine)");
    } else if (SW.selected && count) {                   /* metal detector: faster the nearer */
        DWORD period = BEEP_NEAR_MS + (DWORD)((BEEP_FAR_MS - BEEP_NEAR_MS) * (nearest > SWEEP_RADIUS ? SWEEP_RADIUS : nearest) / SWEEP_RADIUS);
        if (now - SW.beep_at >= period) { PLAY_SOUND(SND_WEAPON_ACTIVATE, NULL); SW.beep_at = now; if (g_cfg.log >= 3) mlog("sweeper: beep (%d px, every %lu ms)", nearest, period); }
    }
    if (D.obj >= 0) {                                   /* walking to a mine to disarm it */
        BYTE *o = D.obj < OBJECT_COUNT ? object(D.obj) : NULL;
        if (!o || !laid_mine(o)) { mlog("sweeper: the mine to disarm is gone"); D.obj = -1; }
        else if (dist(sx, sy, OBJ_X(o), OBJ_Y(o)) <= DISARM_REACH) { stop = 1; sweep_take(u, o); D.obj = -1; }
        else if (now - D.at > DISARM_TIMEOUT_MS) { mlog("sweeper: disarm walk timed out"); D.obj = -1; }
        else if (walking) D.idle_since = now;
        else if (now - D.idle_since > DISARM_IDLE_MS) { mlog("sweeper: the walk ended %d px from the mine - no disarm", dist(sx, sy, OBJ_X(o), OBJ_Y(o))); D.obj = -1; }
    }
    return stop;
}

/* a click on a visible mine with the sweeper carried: 1 = done here, 2 = walk to (*walk_x, *walk_y) first */
static int sweeper_click(BYTE *u, int wx, int wy, DWORD now, int *walk_x, int *walk_y)
{
    D.obj = -1;
    if (!sweeper_active(u)) return 0;
    BYTE *o = mine_at(wx, wy);
    if (!o) return 0;
    int sx = SARGE_X(u), sy = SARGE_Y(u), d = dist(sx, sy, OBJ_X(o), OBJ_Y(o));
    static const char *PLAN[] = { "collect", "destroy (full)", "destroy (no mine weapon / pack)", "destroy (no free box)" };
    mlog("sweeper: click on mine #%d at %d,%d (%d px away) - %s", obj_index(o), OBJ_X(o), OBJ_Y(o), d, PLAN[sweep_plan(o)]);
    if (d <= DISARM_REACH) { sweep_take(u, o); return 1; }
    *walk_x = OBJ_X(o) + (sx - OBJ_X(o)) * DISARM_SHORT / d;
    *walk_y = OBJ_Y(o) + (sy - OBJ_Y(o)) * DISARM_SHORT / d;
    D.obj = obj_index(o); D.at = D.idle_since = now;
    return 2;
}

int mines_sweep_info(SweepInfo *si)
{
    BYTE *u = SARGE;
    memset(si, 0, sizeof *si);
    if (!SW.carried || !sweeper_active(u)) return 0;
    si->sarge_x = SARGE_X(u); si->sarge_y = SARGE_Y(u); si->radius = SWEEP_RADIUS;
    si->selected = SW.selected; si->count = SW.count; si->nearest = SW.nearest;
    si->plate_x = SW.plate_x; si->plate_y = SW.plate_y;
    si->walking_to = D.obj >= 0;
    int wx, wy;
    BYTE *o = controls_pointer_world(&wx, &wy) ? mine_at(wx, wy) : NULL;
    si->hover = o != NULL;
    if (o) { si->hover_x = OBJ_X(o); si->hover_y = OBJ_Y(o); si->hover_plan = sweep_plan(o); si->hover_own = charge_slot(weapons_carried(TYPE_MINE, NULL), o) >= 0; }
    return 1;
}

int mines_sweep_marks(SweepMark *m, int max, int x0, int y0, int x1, int y1)
{
    BYTE *u = SARGE;
    if (!SW.carried || !u) return 0;
    int n = 0;
    BYTE *mw = weapons_carried(TYPE_MINE, NULL);
    for (int i = 0; i < OBJECT_COUNT && n < max; i++) {
        BYTE *o = object(i);
        if (!laid_mine(o) || (OBJ_FLAGS(o) & OF_HIDDEN)) continue;
        int x = OBJ_X(o), y = OBJ_Y(o);
        if (x < x0 || x > x1 || y < y0 || y > y1) continue;
        m[n].x = (short)x; m[n].y = (short)y; m[n].own = mw && charge_slot(mw, o) >= 0;   /* Sarge's own charge (army 0 field mines in Boot Camp are not his) */
        m[n].in_range = dist(SARGE_X(u), SARGE_Y(u), x, y) <= SWEEP_RADIUS;
        n++;
    }
    return n;
}

int mines_sweep_hud(int *count, int *nearest)
{
    if (!SW.carried) return 0;
    *count = SW.count; *nearest = SW.nearest;
    return 1;
}

int mines_pointer_in_reach(void)
{
    SweepInfo si;
    if (mines_sweep_info(&si) && si.hover) return 1;      /* on a mine: the disarm pointer (modern.c) */
    MinePreview p;
    return mines_preview(&p) && p.state != MINE_OUT;
}

int mines_click(int wx, int wy, DWORD now, int *walk_x, int *walk_y)
{
    BYTE *u = SARGE, *w;
    int sw = sweeper_click(u, wx, wy, now, walk_x, walk_y);
    if (sw) return sw;
    if (!active_for(u) || !(w = mine_weapon(u))) return 0;
    int tx, ty, st = spot_state(u, w, wx, wy, &tx, &ty);
    if (st == MINE_OUT) return 0;                                       /* outside the ring: the normal click */
    if (st != MINE_OK) {
        static const char *WHY[] = { "", "", "blocked ground", "no way there", "another mine too close", "no mines left" };
        mlog("mines: click at %d,%d (tile %d,%d): not placeable - %s", wx, wy, tx, ty, WHY[st]);
        return 1;                                                       /* inside the ring: no walk either */
    }
    if (R.phase) { mlog("mines: click at %d,%d ignored - still laying the last one", wx, wy); return 1; }
    R.phase = 1; R.frames = 0; R.x = tx; R.y = ty; R.at = now; R.sarge = u; R.weapon = w;
    mlog("mines: lay at tile %d,%d (pointer %d,%d, Sarge %d,%d, %d px away), %d mines", tx, ty, wx, wy, SARGE_X(u), SARGE_Y(u),
         (int)sqrt((double)(wx - SARGE_X(u)) * (wx - SARGE_X(u)) + (double)(wy - SARGE_Y(u)) * (wy - SARGE_Y(u))), W_AMMO(w));
    return 1;
}

int mines_frame(DWORD now, int walking, int *face_x, int *face_y)
{
    BYTE *u = SARGE;
    int stop = sweeper_frame(u, now, walking) ? MF_STOP_WALK : 0;
    if (!R.phase) return stop;
    if (u != R.sarge || mine_weapon(u) != R.weapon || !layout_view_is_gameplay()) {
        mlog("mines: placement at %d,%d dropped (Sarge / weapon changed)", R.x, R.y);
        R.phase = 0;
        return stop;
    }
    *face_x = R.x; *face_y = R.y;
    stop |= MF_FACE | MF_STOP_WALK;
    if (R.phase == 1) {
        /* hold fire until Sarge aims (the game's state 4), then let go: the release lays the mine */
        INPUT |= INPUT_FIRE;
        if (R.frames++ == 0) INPUT |= INPUT_FIRE_PRESSED;              /* the first frame: "pressed" */
        if ((now - R.at >= PRESS_MIN_MS && SARGE_STATE(u) == STATE_AIMING) || now - R.at >= PRESS_MAX_MS) {
            INPUT &= ~(INPUT_FIRE | INPUT_FIRE_PRESSED);
            R.phase = 2; R.at = now;
            if (g_cfg.log >= 2) mlog("mines: fire released (Sarge state %d)", SARGE_STATE(u));
        }
        return stop;
    }
    if (now - R.at > WAIT_LAY_MS) {
        mlog("mines: no mine laid %d ms after the release (Sarge state %d) - placement at %d,%d dropped", WAIT_LAY_MS, SARGE_STATE(u), R.x, R.y);
        R.phase = 0;
        return stop & ~MF_FACE;
    }
    return stop;
}

void __fastcall mines_lay(BYTE *weapon, int army, DWORD point)
{
    BYTE *u = SARGE;
    int mine = u && weapon && weapon == SARGE_WEAPON(u) && !IsBadReadPtr(weapon, 0x20) && W_TYPE(weapon) == TYPE_MINE;
    int asked = 0;
    DWORD game_point = point;
    if (mine && R.phase && weapon == R.weapon && army == UID_ARMY(u)) {
        /* Sarge may have shuffled a little since the click: the spot must still be in reach and free */
        int tx, ty, st = spot_state(u, weapon, R.x, R.y, &tx, &ty);
        if (st == MINE_OK) { point = (DWORD)(WORD)R.x | (DWORD)(WORD)R.y << 16; asked = 1; }
        else mlog("mines: the clicked tile %d,%d is no longer placeable (%d) - laid where the game puts it", R.x, R.y, st);
        R.phase = 0;
    }
    int before = mine ? free_charges(weapon, NULL) : 0, ammo = mine ? W_AMMO(weapon) : 0;
    MINE_LAY(weapon, army, point);
    if (mine) {
        int total, after = free_charges(weapon, &total);
        /* FUN_00447c30 takes the ammo off right after this call */
        mlog("mines: %s at %d,%d (%s %d,%d; Sarge %d,%d), charges free %d -> %d of %d, ammo %d -> %d",
             after < before ? "mine laid" : "NO mine laid (no free charge)",
             snap((short)LOWORD(point)), snap((short)HIWORD(point)), asked ? "clicked, game would have used" : "the game's spot",
             (short)LOWORD(game_point), (short)HIWORD(game_point), SARGE_X(u), SARGE_Y(u), before, after, total, ammo, ammo > 0 ? ammo - 1 : 0);
    }
}
