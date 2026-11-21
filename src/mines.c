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
    return 1;
}

int mines_pointer_in_reach(void)
{
    MinePreview p;
    return mines_preview(&p) && p.state != MINE_OUT;
}

int mines_click(int wx, int wy, DWORD now)
{
    BYTE *u = SARGE, *w;
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

int mines_frame(DWORD now, int *face_x, int *face_y)
{
    BYTE *u = SARGE;
    if (!R.phase) return 0;
    if (u != R.sarge || mine_weapon(u) != R.weapon || !layout_view_is_gameplay()) {
        mlog("mines: placement at %d,%d dropped (Sarge / weapon changed)", R.x, R.y);
        R.phase = 0;
        return 0;
    }
    *face_x = R.x; *face_y = R.y;
    if (R.phase == 1) {
        /* hold fire until Sarge aims (the game's state 4), then let go: the release lays the mine */
        INPUT |= INPUT_FIRE;
        if (R.frames++ == 0) INPUT |= INPUT_FIRE_PRESSED;              /* the first frame: "pressed" */
        if ((now - R.at >= PRESS_MIN_MS && SARGE_STATE(u) == STATE_AIMING) || now - R.at >= PRESS_MAX_MS) {
            INPUT &= ~(INPUT_FIRE | INPUT_FIRE_PRESSED);
            R.phase = 2; R.at = now;
            if (g_cfg.log >= 2) mlog("mines: fire released (Sarge state %d)", SARGE_STATE(u));
        }
        return 1;
    }
    if (now - R.at > WAIT_LAY_MS) {
        mlog("mines: no mine laid %d ms after the release (Sarge state %d) - placement at %d,%d dropped", WAIT_LAY_MS, SARGE_STATE(u), R.x, R.y);
        R.phase = 0;
        return 0;
    }
    return 1;
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
