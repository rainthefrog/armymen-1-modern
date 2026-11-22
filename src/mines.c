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
 *     point (MineLay snaps it to the tile itself). A click beyond the ring walks there and lays it (round 3,
 *     below; a tile that is not placeable: the normal walk); a hold walks as before; keyboard / right-button fire lays at the muzzle as the original;
 *   - why not walk Sarge to the spot and lay at his feet: the game never lays at the feet but at the hand point
 *     of the animation frame (in front of him, direction-dependent), so walking there would still miss the
 *     clicked tile; and MineLay itself only takes a point - nothing in it depends on where Sarge stands - so
 *     giving it the clicked point is the game's own creation with one argument changed.
 *
 * Rewrites / wraps:
 *   call 0x445f44 (in FUN_00445c70, case 11) -> mines_lay. Callee FUN_00446600 MineLay: __fastcall ecx = WEAPON*,
 *   edx = army index, stack = SPOINT packed (x low word, y high word), ret 4. Kept: the original is always called
 *   (any unit, any weapon); changed: for Sarge's own current weapon with a placement asked by a click, the point.
 *   FUN_004398d0 (the mine trigger) -> mines_step (hook_jmp, 2026-11-21, sweeper rules below).
 * Switches: [controls] mine_click = 1 (default; 0 = the original behaviour, nothing drawn); sweeper_click (below).
 *
 * MINESWEEPER (2026-11-21, user picked rework proposal 2; [controls] sweeper_click = 1, 0 = only the original sweeper)
 * The original (exe): weapon type 20, special, unlimited. Only while SELECTED: every avatar update scans the 20 tiles
 * round the muzzle tile (FUN_004399d0 -> FUN_00439bb0) and reveals hidden mines (+0x70 &= ~0x10, +0x7c &=
 * ~0x40000000, FUN_0042d4e0(&+0x7c, 1)); with FIRE HELD it disarms them: FUN_0040d500 (effect 0x7f at the mine),
 * FUN_0044da60(x, y, 0) (2 s marker), FUN_00446390 (sound + FUN_004463b0: off the map with FUN_0040e560, +0x70 |=
 * 0x64 = used, FUN_0042c880(old cell) refreshes the AI's mine cells). A Sarge whose sweeper is selected never sets a
 * mine off (FUN_004398d0; only a Sarge reaches that test - troopers / vehicles are passed as NULL and always set
 * mines off) - REMOVED with sweeper_click on (2026-11-21 user decision, mines_step). The hold-fire disarm is kept.
 * Walking with the sweeper selected is slower in the original (its own animation group): normal speed + sprint with
 * sweeper_click on (mines_sweeper_walk_mul, controls.c controls_move).
 * Added here (the mine trigger rewrite mines_step; the rest from the per-frame controls and the overlay):
 *   - a mine the detector REVEALS (it was hidden) is DEACTIVATED: stays, visible ("SAFE"), collectable, never goes off
 *     again under anyone (2026-11-21, "deactivated mines" below; checked by mines_step);
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
#define TYPE_EXPLOSIVE   12
#define STACK_MAX        99                            /* ammo cap of every weapon: the game's MakeWeapon cap = the HUD's two digits */

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

/* Round 3 (2026-11-21, user: "I want to left click and put anywhere ... and I want to be able to pick them back up"):
 *   - a tap BEYOND the ring on a placeable tile walks Sarge there along the normal click route and lays the mine on that
 *     tile once he is within LAY_ARRIVE of it (F below; MINE_FAR in the preview = "WALK + LAY"). Cancelled by another
 *     tap, fire, a weapon change, or the walk ending short. A tap beyond the ring on a tile that is not placeable is the
 *     normal walk, as before;
 *   - a tap on one of Sarge's OWN laid mines (his mine weapon's charge, or a charge of his emptied pack) walks there and
 *     picks it up - any time on foot, whatever weapon is selected (easiest: no weapon change needed), no sweeper needed:
 *     the sweeper's collect path without its disarm effects, and never destroyed (full / no free box: it stays). */
#define LAY_ARRIVE       40      /* walk + lay: lay once Sarge is this close to the tile (well inside MINE_REACH) */
#define LAY_WALK_TIMEOUT_MS 20000
#define LAY_IDLE_MS      600     /* not walking and not there yet this long: given up */
static struct { int active, x, y; BYTE *weapon; DWORD at, idle_since; } F;

/* a tile beyond the ring: placeable once Sarge is there? (the reach and "no way" tests wait until he is) */
static int far_state(BYTE *w, int tx, int ty)
{
    if (W_AMMO(w) <= 0 || free_charges(w, NULL) <= 0) return MINE_EMPTY;
    if (tx <= 0 || ty <= 0 || controls_sarge_blocked_at(tx, ty)) return MINE_BLOCKED;
    if (mine_near(tx, ty)) return MINE_CLOSE;
    return MINE_FAR;
}

static void start_lay(BYTE *u, BYTE *w, int tx, int ty, int wx, int wy, DWORD now)
{
    R.phase = 1; R.frames = 0; R.x = tx; R.y = ty; R.at = now; R.sarge = u; R.weapon = w;
    mlog("mines: lay at tile %d,%d (pointer %d,%d, Sarge %d,%d, %d px away), %d mines", tx, ty, wx, wy, SARGE_X(u), SARGE_Y(u),
         (int)sqrt((double)(wx - SARGE_X(u)) * (wx - SARGE_X(u)) + (double)(wy - SARGE_Y(u)) * (wy - SARGE_Y(u))), W_AMMO(w));
}

static int sweeper_active(BYTE *u);                /* minesweeper (below) */
static BYTE *mine_at(int wx, int wy);
static BYTE *own_hover(BYTE *u, int wx, int wy, int *ok);

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
    if (!controls_pointer_world(&wx, &wy)) return 1;
    p->state = spot_state(u, w, wx, wy, &p->x, &p->y);
    if (p->state == MINE_OUT && far_state(w, p->x, p->y) == MINE_FAR) p->state = MINE_FAR;   /* a tap walks there + lays */
    int ok;
    if (p->state != MINE_OUT && ((sweeper_active(u) && mine_at(wx, wy)) || own_hover(u, wx, wy, &ok)))
        p->state = MINE_OUT;                                            /* on a mine: the disarm / pick-up pointer */
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
static struct { int obj, pickup; DWORD at, idle_since; } D = { -1 };   /* a walk to a mine in progress: disarm (sweeper) or pick up (own) */

static int laid_mine(BYTE *o)
{
    return OBJ_CATEGORY(o) == CAT_WEAPON_OBJ && OBJ_NUMBER(o) == TYPE_MINE && !(OBJ_FLAGS(o) & (OF_IN_BAG | OF_USED)) && OBJ_CELL(o);
}
static int objects_ok(void) { return OBJECTS && OBJECT_COUNT > 0 && OBJECT_COUNT < 20000; }

/* ---------------------------------------------------------------- deactivated mines (2026-11-21)
 * User: "Mines - no - don't add avoidance of mines. they should be deactivated." A mine the detector REVEALS (it was
 * hidden: enemy mines, Boot Camp's field mines) is disarmed in place: it stays on the map, visible, collectable with a
 * click, but goes off under NOBODY any more (Sarge, his squad, the enemy - chosen: "disarmed in place" for everyone).
 * Mines that were never hidden (the ones Sarge or his army laid) stay armed: they are traps for the enemy. The set is
 * kept here by object index (the game has no such flag: every bit of +0x70 has a meaning) and checked by the mine
 * trigger rewrite mines_step (FUN_004398d0); new mission / Play Again (the game clock starts again) = empty. */
#define MAX_OBJECTS      20000
#define GAME_PTR         (*(BYTE **)0x4b3f70)
#define GAME_CLOCK       (*(DWORD *)(GAME_PTR + 0x44))   /* the clock the 6000 arm test uses (laid time +0xcc) */
static BYTE g_safe[MAX_OBJECTS / 8], g_was_hidden[MAX_OBJECTS / 8];
static struct { BYTE *objects; int count, key; DWORD clock; } g_safe_for;
static int bit_get(const BYTE *b, int i) { return i >= 0 && i < MAX_OBJECTS && (b[i >> 3] >> (i & 7) & 1); }
static void bit_set(BYTE *b, int i, int on) { if (i < 0 || i >= MAX_OBJECTS) return; if (on) b[i >> 3] |= (BYTE)(1 << (i & 7)); else b[i >> 3] &= (BYTE)~(1 << (i & 7)); }
static void safe_sync(void)
{
    if (!GAME_PTR) return;
    int key = (*(int *)(GAME_PTR + 0x74) << 16) ^ *(int *)(GAME_PTR + 0x70);
    DWORD now = GAME_CLOCK;
    if (OBJECTS != g_safe_for.objects || OBJECT_COUNT != g_safe_for.count || key != g_safe_for.key || now + 1000 < g_safe_for.clock) {
        memset(g_safe, 0, sizeof g_safe); memset(g_was_hidden, 0, sizeof g_was_hidden);
        g_safe_for.objects = OBJECTS; g_safe_for.count = OBJECT_COUNT; g_safe_for.key = key;
    }
    g_safe_for.clock = now;
}
static int obj_index(BYTE *o);
static int mine_safe(BYTE *o) { return sweeper_enabled() && bit_get(g_safe, obj_index(o)); }
static void mine_deactivate(BYTE *o, const char *how)
{
    int i = obj_index(o);
    if (bit_get(g_safe, i)) return;
    bit_set(g_safe, i, 1);
    mlog("sweeper: mine #%d at %d,%d (army %d) DEACTIVATED (%s) - it no longer goes off", i, OBJ_X(o), OBJ_Y(o), OBJ_ARMY(o), how);
}
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

/* The mine pack (pickup item) whose charge array holds object o, or NULL. A LAID mine's +0x20 is cleared by
 * FUN_00446530, so its owner is found by its object number in the arrays the mod can reach: Sarge's carried mine weapon
 * (also a pickup item) and every mine pack of the map (an enemy unit's own weapon record is not reachable). */
static BYTE *pack_holding(BYTE *o)
{
    for (int i = 0; i < OBJECT_COUNT; i++) {
        BYTE *p = object(i), *rec = OBJ_OWNER(p);
        if (OBJ_CATEGORY(p) != CAT_PICKUP || !rec || IsBadReadPtr(rec, 0x20) || W_TYPE(rec) != TYPE_MINE) continue;
        if (charge_slot(rec, o) >= 0) return p;
    }
    return NULL;
}
static BYTE *owner_weapon(BYTE *o)
{
    BYTE *w = weapons_carried(TYPE_MINE, NULL);
    if (w && charge_slot(w, o) >= 0) return w;
    BYTE *p = pack_holding(o);
    if (p) return OBJ_OWNER(p);
    BYTE *rec = OBJ_OWNER(o);                          /* a charge still in a bag keeps its owner */
    return rec && !IsBadReadPtr(rec, 0x20) && W_TYPE(rec) == TYPE_MINE ? rec : NULL;
}

/* what a disarm of mine o would do */
static int sweep_plan(BYTE *o)
{
    BYTE *w = weapons_carried(TYPE_MINE, NULL);
    if (w) {
        if (W_AMMO(w) < 0 || W_AMMO(w) >= STACK_MAX) return SWEEP_FULL;   /* 99; the array grows (put_in) */
        return SWEEP_COLLECT;
    }
    if (!dead_pack()) return SWEEP_NO_PACK;
    if (!weapons_room_for_special()) return SWEEP_NO_ROOM;
    return SWEEP_COLLECT;
}

/* Off the map, back in the bag (FUN_004463b0 without the "used" bit). */
static void bag_mine(BYTE *o)
{
    int cell = OBJ_CELL(o);
    bit_set(g_safe, obj_index(o), 0); bit_set(g_was_hidden, obj_index(o), 0);   /* laid again later: armed as any new mine */
    if (cell) OFF_THE_MAP(o);
    OBJ_FLAGS(o) = (BYTE)((OBJ_FLAGS(o) | OF_IN_BAG) & ~(OF_USED | OF_PLACED | OF_HIDDEN));
    OBJ_FLAGS2(o) &= ~OF2_HIDDEN;
    if (cell) MINE_CELLS(cell);
}

/* Growing charge arrays (2026-11-21, user: "mines are not stacking fully - no limits on those ... 99 is the cap"): a mine
 * or explosive weapon's charges are WEAPON +0x1c, short[nMaxAmmo] (+0xe) made by MakeWeapon 0x446190 with the exe's own
 * allocator (0x4708e0, cdecl size) and freed by FUN_00446350 with the exe's free (0x470870, cdecl ptr). The only
 * readers of the array are MineLay 0x446600 / the explosive placement 0x4463e0 (both loop nMaxAmmo entries and read the
 * object of each, so every entry must stay a real object number) and FUN_00446350 (free). To take more charges than
 * the weapon was made with, the array is made one entry longer with the game's allocator, the old one freed with the
 * game's free and nMaxAmmo raised; a charge that leaves an array without a used-up object to swap back is removed from
 * it (the entries after it move up, nMaxAmmo - 1). Ammo itself stops at STACK_MAX. */
typedef void *(__cdecl *GameAllocFn)(int size);
typedef void (__cdecl *GameFreeFn)(void *p);
#define GAME_ALLOC       ((GameAllocFn)0x4708e0)
#define GAME_FREE        ((GameFreeFn)0x470870)
static int append_charge(BYTE *w, int obj)            /* the new slot, or -1 */
{
    int n = W_MAX(w);
    short *old = W_CHARGES(w);
    if (n < 0 || n >= 0x7ff0 || (old && n > 0 && IsBadReadPtr(old, (UINT_PTR)n * 2))) return -1;
    short *grown = (short *)GAME_ALLOC((n + 1) * 2);
    if (!grown) return -1;
    if (old && n > 0) memcpy(grown, old, (size_t)n * 2);
    grown[n] = (short)obj;
    W_CHARGES(w) = grown; W_MAX(w) = (short)(n + 1);
    if (old) GAME_FREE(old);
    return n;
}
static void remove_charge(BYTE *w, int j)
{
    short *c = W_CHARGES(w);
    int n = W_MAX(w);
    if (!c || j < 0 || j >= n) return;
    memmove(c + j, c + j + 1, (size_t)(n - j - 1) * 2);
    W_MAX(w) = (short)(n - 1);
}

/* Put mine o into weapon w: its own slot; else a used-up slot (the used-up object goes to o's old owner in o's place);
 * else a new slot at the end of a grown array (o leaves its old owner's array). */
static int put_in(BYTE *w, BYTE *o, int *slot, int bag_too)
{
    int k = charge_slot(w, o);
    if (k < 0) {
        BYTE *old = owner_weapon(o);
        int j = old && old != w && !IsBadReadPtr(old, 0x20) && W_TYPE(old) == TYPE_MINE ? charge_slot(old, o) : -1;
        k = spare_slot(w, bag_too);
        if (k >= 0) {
            short *c = W_CHARGES(w);
            BYTE *spent = object(c[k]);
            if (j >= 0) { W_CHARGES(old)[j] = (short)obj_index(spent); OBJ_OWNER(spent) = old; }
            c[k] = (short)obj_index(o);
        } else {
            k = append_charge(w, obj_index(o));
            if (k < 0) return 0;
            if (j >= 0) remove_charge(old, j);
        }
        OBJ_OWNER(o) = w;
    }
    *slot = k;
    return 1;
}


/* One of Sarge's OWN laid mines: a charge of his carried mine weapon, or of his emptied (dropped, used) mine pack. */
static int own_mine(BYTE *u, BYTE *o)
{
    BYTE *w = weapons_carried(TYPE_MINE, NULL);
    if (w && charge_slot(w, o) >= 0) return 1;
    BYTE *p = pack_holding(o);
    return p && (OBJ_FLAGS(p) & OF_USED) && OBJ_ARMY(o) == UID_ARMY(u);
}

/* Can an own mine be picked up now (never destroyed when not): room in the carried mine weapon, or his emptied pack
 * revived into a free box. */
static int pickup_ok(BYTE *o)
{
    BYTE *w = weapons_carried(TYPE_MINE, NULL);
    if (w) return W_AMMO(w) >= 0 && W_AMMO(w) < STACK_MAX;
    return pack_holding(o) != NULL && weapons_room_for_special();
}

/* The own mine under the pointer, without the sweeper (with it, the sweeper's disarm pointer covers own mines). */
static BYTE *own_hover(BYTE *u, int wx, int wy, int *ok)
{
    if (!enabled() || !u || SARGE_VEHICLE(u) || !objects_ok() || !layout_view_is_gameplay() || MENU_CURSOR_SHOWN || sweeper_active(u)) return NULL;
    BYTE *o = mine_at(wx, wy);
    if (!o || !own_mine(u, o)) return NULL;
    *ok = pickup_ok(o);
    return o;
}

/* Sarge is next to mine o. pickup = 0: the sweeper's disarm - the game's disarm effects, then collected, or destroyed as
 * the original when it cannot be. pickup = 1: his own mine picked up (no sweeper) - a click sound, collected, and left
 * where it is when it cannot be. */
static void take_mine(BYTE *u, BYTE *o, int pickup)
{
    if (!laid_mine(o)) return;
    int x = OBJ_X(o), y = OBJ_Y(o), army = OBJ_ARMY(o), plan = sweep_plan(o), k = -1;
    BYTE *w = weapons_carried(TYPE_MINE, NULL), *pack = NULL;
    const char *who = pickup ? "mines" : "sweeper";
    if (pickup) {
        if (!w) { pack = pack_holding(o); plan = pack && weapons_room_for_special() ? SWEEP_COLLECT : SWEEP_NO_ROOM; }
        if (plan != SWEEP_COLLECT) { mlog("mines: own mine #%d at %d,%d not picked up (%s) - it stays", obj_index(o), x, y,
                                          plan == SWEEP_FULL ? "mine weapon full" : "no free box"); return; }
        PLAY_SOUND(SND_WEAPON_ACTIVATE, NULL);
    } else {
        DISARM_EFFECT(o);
        DISARM_MARK(x, y, 0);
    }
    int ammo = w ? W_AMMO(w) : 0;
    if (plan == SWEEP_COLLECT && !w) {                  /* revive an emptied pack (his own one for a pick-up) with this charge */
        if (!pack) pack = dead_pack();
        w = pack ? OBJ_OWNER(pack) : NULL;
        if (w && put_in(w, o, &k, 1)) {
            W_AMMO(w) = 1;
            OBJ_FLAGS(pack) &= (BYTE)~OF_USED;
            PICK_UP(u, pack, 0);
            if (weapons_carried(TYPE_MINE, NULL) != w) {  /* refused: undo */
                OBJ_FLAGS(pack) |= OF_USED; W_AMMO(w) = 0;
                mlog("%s: the revived mine pack #%d was not taken", who, obj_index(pack));
                plan = SWEEP_NO_ROOM;
            }
        } else plan = SWEEP_NO_PACK;
    } else if (plan == SWEEP_COLLECT) {
        if (put_in(w, o, &k, free_charges(w, NULL) > ammo)) W_AMMO(w) = (short)(ammo + 1); else plan = SWEEP_FULL;
    }
    if (plan != SWEEP_COLLECT) {
        if (pickup) { mlog("mines: own mine #%d at %d,%d not picked up - it stays", obj_index(o), x, y); return; }
        MINE_DESTROY(o);
        mlog("sweeper: mine #%d at %d,%d (army %d) disarmed and destroyed (%s)", obj_index(o), x, y, army,
             plan == SWEEP_FULL ? "mine weapon full" : plan == SWEEP_NO_ROOM ? "no free box" : "no mine weapon / pack");
        return;
    }
    bag_mine(o);
    mlog("%s: mine #%d at %d,%d (army %d) %s into %s slot %d of %d, ammo %d -> %d, free charges %d", who,
         obj_index(o), x, y, army, pickup ? "PICKED UP" : "disarmed and COLLECTED", pack ? "a revived mine pack," : "Sarge's mine weapon,",
         k, W_MAX(w), pack ? 0 : ammo, W_AMMO(w), free_charges(w, NULL));
}
static void sweep_take(BYTE *u, BYTE *o) { take_mine(u, o, 0); }

/* every frame: the detector (reveal + beeps), the disarm walk */
static int sweeper_frame(BYTE *u, DWORD now, int walking)
{
    int stop = 0;
    SW.carried = sweeper_active(u);
    if (!SW.carried) { SW.count = 0; return 0; }
    SW.selected = SARGE_WEAPON(u) && W_TYPE(SARGE_WEAPON(u)) == TYPE_SWEEPER;
    if (SW.selected) { short m[2]; MUZZLE_POINT(u, m); SW.plate_x = m[0]; SW.plate_y = m[1]; }
    safe_sync();
    int sx = SARGE_X(u), sy = SARGE_Y(u), count = 0, nearest = 1 << 30, fresh = 0;
    for (int i = 0; i < OBJECT_COUNT; i++) {
        BYTE *o = object(i);
        if (!laid_mine(o)) continue;
        int hidden = (OBJ_FLAGS(o) & OF_HIDDEN) || (OBJ_FLAGS2(o) & OF2_HIDDEN);
        if (hidden) bit_set(g_was_hidden, i, 1);          /* hidden once: revealing it (ours or the game's sweep) deactivates it */
        int d = dist(sx, sy, OBJ_X(o), OBJ_Y(o));
        if (d > SWEEP_RADIUS) continue;
        if (bit_get(g_was_hidden, i)) mine_deactivate(o, hidden ? "revealed by the detector" : "revealed, in detector range");
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
    return stop;
}

/* every frame: a walk to a mine (sweeper disarm / own pick-up) - done when Sarge is within DISARM_REACH */
static int mine_walk_frame(BYTE *u, DWORD now, int walking)
{
    if (D.obj < 0 || !u) return 0;
    const char *who = D.pickup ? "mines" : "sweeper";
    BYTE *o = D.obj < OBJECT_COUNT ? object(D.obj) : NULL;
    int d = o ? dist(SARGE_X(u), SARGE_Y(u), OBJ_X(o), OBJ_Y(o)) : 0;
    if (!o || !laid_mine(o)) { mlog("%s: the mine to %s is gone", who, D.pickup ? "pick up" : "disarm"); D.obj = -1; }
    else if (!D.pickup && !SW.carried) { mlog("sweeper: no sweeper any more - disarm walk dropped"); D.obj = -1; }
    else if (d <= DISARM_REACH) { D.obj = -1; take_mine(u, o, D.pickup); return 1; }
    else if (now - D.at > DISARM_TIMEOUT_MS) { mlog("%s: walk to the mine timed out", who); D.obj = -1; }
    else if (walking) D.idle_since = now;
    else if (now - D.idle_since > DISARM_IDLE_MS) { mlog("%s: the walk ended %d px from the mine - nothing done", who, d); D.obj = -1; }
    return 0;
}

/* a click on a visible mine with the sweeper carried: 1 = done here, 2 = walk to (*walk_x, *walk_y) first */
/* a click on a visible mine: with the sweeper carried any mine (disarm / collect), without it Sarge's own mine (pick up).
 * 1 = done here, 2 = walk to (*walk_x, *walk_y) first, 0 = not on such a mine */
static int mine_object_click(BYTE *u, int wx, int wy, DWORD now, int *walk_x, int *walk_y)
{
    D.obj = -1;
    if (!u || SARGE_VEHICLE(u) || !objects_ok() || !layout_view_is_gameplay()) return 0;
    int ok, pickup = 0;
    BYTE *o = sweeper_active(u) ? mine_at(wx, wy) : NULL;
    if (!o) { o = own_hover(u, wx, wy, &ok); pickup = 1; }
    if (!o) return 0;
    int sx = SARGE_X(u), sy = SARGE_Y(u), d = dist(sx, sy, OBJ_X(o), OBJ_Y(o));
    if (pickup) mlog("mines: click on own mine #%d at %d,%d (%d px away) - %s", obj_index(o), OBJ_X(o), OBJ_Y(o), d, ok ? "pick up" : "cannot (full / no free box)");
    else {
        static const char *PLAN[] = { "collect", "destroy (full)", "destroy (no mine weapon / pack)", "destroy (no free box)" };
        mlog("sweeper: click on mine #%d at %d,%d (%d px away) - %s", obj_index(o), OBJ_X(o), OBJ_Y(o), d, PLAN[sweep_plan(o)]);
    }
    if (pickup && !ok) return 1;                         /* on his mine, nothing to do: no walk either */
    if (d <= DISARM_REACH) { take_mine(u, o, pickup); return 1; }
    *walk_x = OBJ_X(o) + (sx - OBJ_X(o)) * DISARM_SHORT / d;
    *walk_y = OBJ_Y(o) + (sy - OBJ_Y(o)) * DISARM_SHORT / d;
    D.obj = obj_index(o); D.pickup = pickup; D.at = D.idle_since = now;
    return 2;
}

/* Walking with the sweeper SELECTED (2026-11-21, user: normal speed + sprint): the game walks Sarge with the sweeper's
 * own animation group (avatar +0 = 4; the rifle's is 0), whose moving actions are slower (ANIM +0xac speed 60 against the
 * rifle's 88, Wine Boot Camp: ~85 against ~150 px/s). controls.c controls_move (AnimMove wrap) multiplies this step by
 * the factor returned here, so the walk is the rifle's: the rifle group's speed for the same action as last seen
 * (recorded here from every rifle-group step), else SWEEP_WALK_NORMAL / SWEEP_WALK_SLOW, x SWEEP_WALK_CADENCE. The sprint comes on top
 * (controls.c). Only while sweeper_click is on and the sweeper is the current weapon; the hold-fire sweep (state 7) does
 * not move, so it stays as it is. */
#define SARGE_AVATAR(u)      (*(BYTE **)((u) + 0x268))
#define AV_GROUP(a)          (*(int *)(a))             /* animation group (FUN_0043a0f0 table index): 0 rifle, 4 sweeper */
#define AV_ACTION(a)         (*(int *)((a) + 0x14))    /* 0..3 = the moving actions (FUN_004474c0 cases 0..3) */
#define AV_GROUP_RIFLE       0
#define AV_GROUP_SWEEPER     4
#define SWEEP_WALK_NORMAL    88                        /* the rifle group's walk speed (Wine log) ... */
#define SWEEP_WALK_SLOW      60                        /* ... and the sweeper group's */
#define SWEEP_WALK_CADENCE   1.05f                     /* the sweeper walk still covered ~5 % less ground at the same ANIM speed (its
                                                        * steps; Wine walking down: x1.47 132-136, x1.61 150-167, rifle 140-155 px/s) */
float mines_sweeper_walk_mul(BYTE *u, int speed)
{
    static short normal[4];
    BYTE *av = u ? SARGE_AVATAR(u) : NULL;
    if (!av || IsBadReadPtr(av, 0x18) || speed <= 0) return 1.0f;
    int act = AV_ACTION(av);
    if (act < 0 || act > 3) return 1.0f;
    if (AV_GROUP(av) == AV_GROUP_RIFLE) { normal[act] = (short)speed; return 1.0f; }
    if (AV_GROUP(av) != AV_GROUP_SWEEPER || !sweeper_enabled() || !SARGE_WEAPON(u) || W_TYPE(SARGE_WEAPON(u)) != TYPE_SWEEPER) return 1.0f;
    float want = normal[act] > 0 ? (float)normal[act] : (float)speed * SWEEP_WALK_NORMAL / SWEEP_WALK_SLOW;
    float mul = want / (float)speed * SWEEP_WALK_CADENCE;
    if (g_cfg.log >= 2) {
        static DWORD at;
        if (GetTickCount() - at > 2000) { at = GetTickCount(); mlog("sweeper: walking with the sweeper selected - speed %d -> %.0f (x%.2f)", speed, want, mul); }
    }
    return mul > 1.0f ? mul : 1.0f;
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
        m[n].safe = (char)mine_safe(o);
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

/* modern.c: one of Sarge's own mines under the pointer (no sweeper): where, and whether a click picks it up */
int mines_pickup_hover(int *x, int *y, int *ok)
{
    int wx, wy;
    BYTE *o = controls_pointer_world(&wx, &wy) ? own_hover(SARGE, wx, wy, ok) : NULL;
    if (!o) return 0;
    *x = OBJ_X(o); *y = OBJ_Y(o);
    return 1;
}

int mines_pointer_in_reach(void)
{
    SweepInfo si;
    int x, y, ok;
    if (mines_sweep_info(&si) && si.hover) return 1;      /* on a mine: the disarm pointer (modern.c) */
    if (mines_pickup_hover(&x, &y, &ok)) return 1;        /* on his own mine: the pick-up pointer */
    MinePreview p;
    return mines_preview(&p) && p.state != MINE_OUT;
}

int mines_click(int wx, int wy, DWORD now, int *walk_x, int *walk_y)
{
    BYTE *u = SARGE, *w;
    if (F.active) { mlog("mines: walk + lay at %d,%d cancelled (another click)", F.x, F.y); F.active = 0; }
    int sw = mine_object_click(u, wx, wy, now, walk_x, walk_y);
    if (sw) return sw;
    if (!active_for(u) || !(w = mine_weapon(u))) return 0;
    int tx, ty, st = spot_state(u, w, wx, wy, &tx, &ty);
    if (st == MINE_OUT) {                                               /* beyond the ring: walk there and lay */
        if (far_state(w, tx, ty) != MINE_FAR || R.phase) return 0;      /* not placeable: the normal walk */
        F.active = 1; F.x = tx; F.y = ty; F.weapon = w; F.at = F.idle_since = now;
        *walk_x = tx; *walk_y = ty;
        mlog("mines: walk + lay at tile %d,%d (Sarge %d,%d, %d px away)", tx, ty, SARGE_X(u), SARGE_Y(u), dist(SARGE_X(u), SARGE_Y(u), tx, ty));
        return 2;
    }
    if (st != MINE_OK) {
        static const char *WHY[] = { "", "", "blocked ground", "no way there", "another mine too close", "no mines left" };
        mlog("mines: click at %d,%d (tile %d,%d): not placeable - %s", wx, wy, tx, ty, WHY[st]);
        return 1;                                                       /* inside the ring: no walk either */
    }
    if (R.phase) { mlog("mines: click at %d,%d ignored - still laying the last one", wx, wy); return 1; }
    start_lay(u, w, tx, ty, wx, wy, now);
    return 1;
}

/* every frame: walk + lay - lay once Sarge is close enough */
static int far_lay_frame(BYTE *u, DWORD now, int walking)
{
    if (!F.active) return 0;
    BYTE *w = mine_weapon(u);
    int d = u ? dist(SARGE_X(u), SARGE_Y(u), F.x, F.y) : 0;
    const char *why = NULL;
    if (!u || w != F.weapon) why = "weapon changed";
    else if (!R.phase && (INPUT & INPUT_FIRE)) why = "fired";
    else if (now - F.at > LAY_WALK_TIMEOUT_MS) why = "timed out";
    else if (d <= LAY_ARRIVE) {
        F.active = 0;
        int tx, ty, st = spot_state(u, w, F.x, F.y, &tx, &ty);
        if (st == MINE_OK && !R.phase) { start_lay(u, w, tx, ty, F.x, F.y, now); return 1; }
        mlog("mines: walk + lay: at the tile %d,%d it is not placeable (%d) - nothing laid", F.x, F.y, st);
        return 1;
    }
    else if (walking) F.idle_since = now;
    else if (now - F.idle_since > LAY_IDLE_MS) why = "the walk ended short";
    if (why) { mlog("mines: walk + lay at %d,%d cancelled (%s, %d px away)", F.x, F.y, why, d); F.active = 0; }
    return 0;
}

int mines_frame(DWORD now, int walking, int *face_x, int *face_y)
{
    BYTE *u = SARGE;
    int stop = sweeper_frame(u, now, walking) ? MF_STOP_WALK : 0;
    if (mine_walk_frame(u, now, walking)) stop |= MF_STOP_WALK;
    if (far_lay_frame(u, now, walking)) stop |= MF_STOP_WALK;
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

static void __fastcall mines_lay_px(BYTE *weapon, int army, DWORD point) {
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
    PX_GAME(MINE_LAY(weapon, army, point));
    if (mine) {
        int total, after = free_charges(weapon, &total);
        /* FUN_00447c30 takes the ammo off right after this call */
        mlog("mines: %s at %d,%d (%s %d,%d; Sarge %d,%d), charges free %d -> %d of %d, ammo %d -> %d",
             after < before ? "mine laid" : "NO mine laid (no free charge)",
             snap((short)LOWORD(point)), snap((short)HIWORD(point)), asked ? "clicked, game would have used" : "the game's spot",
             (short)LOWORD(game_point), (short)HIWORD(game_point), SARGE_X(u), SARGE_Y(u), before, after, total, ammo, ammo > 0 ? ammo - 1 : 0);
    }
}
void __fastcall mines_lay(BYTE *weapon, int army, DWORD point) { PX_BEGIN(px); mines_lay_px(weapon, army, point); PX_END(px, PR_MINES); }   /* timed: stage PR_MINES (exclusive, common.h) */

/* ---------------------------------------------------------------- the mine trigger (2026-11-21)
 * Rewrite of FUN_004398d0 (hook_jmp; caller FUN_00439800 "something moves", once per object found at the mover's
 * point that is a mine: call 0x439886). __fastcall ecx = the mine object, edx = the mover if it is a Sarge (an
 * "avatar" item, UID type 6; NULL for troopers, vehicles, everything else), no stack args, plain ret.
 * The original:
 *   mover is a Sarge and HIS army's Sarge has the minesweeper selected (FUN_00448e60(army) -> FUN_00448d70: the type of
 *     his current weapon == 20): a hidden mine is revealed (+0x7c &= ~0x40000000, +0x70 &= ~0x10, FUN_0042d4e0(&+0x7c,
 *     1)); the player's Sarge (army 0) with fire held disarms it (FUN_00446390); either way it does NOT go off -
 *     "a Sarge with the sweeper selected is mine-proof";
 *   else: the mine's army in play (FUN_0041ac10(+0x74)) and laid more than 6000 game ms ago (clock gpGame +0x44, laid
 *     at +0xcc) -> it goes off (FUN_00439970).
 * Kept: all of that while [controls] sweeper_click = 0, and with it on the reveal and the hold-fire disarm (the
 * original sweep). Changed with sweeper_click on: (B1) the sweeper no longer makes Sarge mine-proof - a mine he steps
 * on goes off unless it is disarmed; (B3) a DEACTIVATED mine (revealed by the detector, above; a mine revealed here
 * by stepping on it with the sweeper selected counts too) never goes off, under anyone. */
typedef int (__fastcall *IntFn)(int v);
#define SARGE_WEAPON_OF  ((IntFn)0x448e60)            /* army -> that army's Sarge's current weapon slot */
#define WEAPON_TYPE_OF   ((IntFn)0x448d70)            /* weapon slot -> weapon type */
#define ARMY_ACTIVE      ((IntFn)0x41ac10)            /* army index -> in play (bit 0) */
#define MINE_BLOW        ((ObjFn)0x439970)            /* the mine goes off: off the map (0x446390) + blast 0x434b90 */
#define OBJ_LAID_AT(o)   (*(DWORD *)((o) + 0xcc))
#define MINE_ARM_MS      6000                         /* 0x1770 */
static void __fastcall mines_step_px(BYTE *mine, DWORD *who) {
    int mod = sweeper_enabled();
    if (who) {
        int army = (int)((*who >> 26) & 7) - 1;
        if (WEAPON_TYPE_OF(SARGE_WEAPON_OF(army)) == TYPE_SWEEPER) {
            int was_hidden = (OBJ_FLAGS2(mine) & OF2_HIDDEN) != 0;
            if (was_hidden) {                              /* the game's reveal */
                OBJ_FLAGS2(mine) &= ~OF2_HIDDEN;
                OBJ_FLAGS(mine) &= (BYTE)~OF_HIDDEN;
                OBJ_DIRTY(&OBJ_FLAGS2(mine), 1);
            }
            if (army == 0 && (INPUT & INPUT_FIRE)) {       /* the original hold-fire sweep: disarmed (destroyed) */
                if (mod) mlog("sweeper: Sarge steps on mine #%d with fire held - disarmed as the original", obj_index(mine));
                MINE_DESTROY(mine);
                return;
            }
            if (!mod) return;                              /* original: mine-proof */
            if (was_hidden) { safe_sync(); mine_deactivate(mine, "revealed under Sarge's sweeper"); }
        }
    }
    if (mod) {
        safe_sync();
        if (mine_safe(mine)) {
            static int last; static DWORD at;
            if (obj_index(mine) != last || GetTickCount() - at > 2000) {
                last = obj_index(mine); at = GetTickCount();
                mlog("sweeper: %s steps on DEACTIVATED mine #%d at %d,%d - no blast", who ? "a Sarge" : "a soldier / vehicle", last, OBJ_X(mine), OBJ_Y(mine));
            }
            return;
        }
    }
    if ((ARMY_ACTIVE(OBJ_ARMY(mine)) & 1) && GAME_CLOCK - OBJ_LAID_AT(mine) > MINE_ARM_MS) {
        if (mod) mlog("sweeper: mine #%d at %d,%d (army %d) goes off under %s", obj_index(mine), OBJ_X(mine), OBJ_Y(mine), OBJ_ARMY(mine),
                      who ? "a Sarge" : "a soldier / vehicle");
        MINE_BLOW(mine);
    }
}
void __fastcall mines_step(BYTE *mine, DWORD *who) { PX_BEGIN(px); mines_step_px(mine, who); PX_END(px, PR_MINES); }   /* timed: stage PR_MINES (exclusive, common.h) */

/* weapons.c (mine pack top-up, 2026-11-21): how many charges of this mine weapon really lie in the bag - its ammo may
 * never go above this (the top-up used to raise only the ammo count: "more ammo than free charges") */
int mines_free_charges(BYTE *weapon) { return weapon ? free_charges(weapon, NULL) : 0; }
/* weapons.c (mine / explosive pack top-up): move up to n FREE charges of the pack weapon `from` into `to`: into a
 * used-up slot of `to` (the two object numbers are swapped), else at the end of `to`'s grown array (append_charge; the
 * charge leaves `from`'s array). Each array keeps pointing at real objects only. Ammo is NOT changed here; returns how
 * many were moved (the caller adds them to `to`'s ammo, at most STACK_MAX, and takes them from the pack's). */
int mines_transfer(BYTE *to, BYTE *from, int n)
{
    if (!to || !from || to == from || !OBJECTS) return 0;
    int moved = 0;
    while (moved < n) {
        short *cf = W_CHARGES(from);
        int nf = W_MAX(from), j = -1;
        if (!cf || nf <= 0 || IsBadWritePtr(cf, (UINT_PTR)nf * 2)) break;
        for (int i = 0; i < nf && j < 0; i++) {
            if (cf[i] < 0 || cf[i] >= OBJECT_COUNT) continue;
            BYTE f = OBJ_FLAGS(object(cf[i]));
            if ((f & OF_IN_BAG) && !(f & OF_USED)) j = i;
        }
        if (j < 0) break;
        short obj = cf[j];
        int k = used_slot(to);
        if (k >= 0) { short t = W_CHARGES(to)[k]; W_CHARGES(to)[k] = obj; cf[j] = t; OBJ_OWNER(object(t)) = from; }
        else { if (append_charge(to, obj) < 0) break; remove_charge(from, j); }
        OBJ_OWNER(object(obj)) = to;
        moved++;
    }
    if (moved && g_cfg.log >= 1) mlog("mines: %d charge(s) moved between packs (type %d) - array now %d long", moved, W_TYPE(to), W_MAX(to));
    return moved;
}

