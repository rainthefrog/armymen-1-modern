/* ============================================================================================
 * weapons.c - six open weapon boxes, ammo up to 99, the mouse wheel and the HUD weapon columns
 *
 * The game gives Sarge three weapon slots (unit +0x27c: weapon record per slot, +0x288: the picked
 * up item object per slot, +0x2c4: current slot; FUN_00448df0 selects a slot). FUN_004492b0 (source
 * WeaponSlot) puts every weapon in one of them by kind: 0 = main (the rifle, or the auto rifle,
 * type 10), 1 = heavy, 2 = special. FUN_004495c0 (game, walking over an item)
 * refuses a weapon while its slot is full ("fully loaded").
 *
 * Here Sarge has SIX boxes on the HUD, in screen order A1 A2 A3 (the panel's own column) and
 * B1 B2 B3 (a new column next to it, wide screens; layout.c splits the panel for it):
 *   - A1 is the rifle. The auto rifle UPGRADES it (the game's own rule: it is only picked up while
 *     A1 still holds the plain rifle); no rifle is kept.
 *   - The other five boxes are OPEN: any heavy or special weapon goes into the first free one.
 *     The game still needs a weapon in its kind's slot to use it, so using a box swaps its weapon
 *     into that slot; the weapon that was there stays in ITS box (g_slot_box: which box shows each
 *     slot's weapon; g_store: the weapons not in a slot).
 *   - A weapon type that is already carried is not taken twice: its ammo is ADDED to the carried
 *     one, up to MAX_AMMO (99, the game's own cap in MakeWeapon); the emptied pickup is removed by
 *     the game's drop routine (an item dropped with no ammo is destroyed); what does not fit stays in the pack.
 *     Every weapon, mines and explosives too, up to 99 (2026-11-21): their charge objects move with the ammo and
 *     their charge array grows (mines.c mines_transfer). An empty carried weapon takes ammo too.
 *   - mouse wheel (modern controls): steps through the boxes holding a weapon in screen order.
 *   - left click on a box: use that weapon; right click: drop it (not the rifle).
 *   - after a drop, another carried weapon of that kind moves into the slot (it keeps its box).
 * Boxes are forgotten when Sarge dies or a new mission run starts; at the next part of the same scenario (and its
 * Play Again) they are rebuilt from a plain record taken at the win ("carry-over to the next part" below).
 * [controls] six_weapons = 1 (with modern controls; the keys 1/2/3 still pick the kinds).
 * ============================================================================================ */
#include "common.h"

#define SARGE              (*(BYTE **)0x697520)
#define SLOTS              3
#define BOXES              6                                      /* A1 A2 A3 B1 B2 B3 */
#define SLOT_TYPE(u, c)    (*(void **)((u) + 0x27c + 4 * (c)))   /* weapon record */
#define SLOT_ITEM(u, c)    (*(void **)((u) + 0x288 + 4 * (c)))   /* picked-up item object, or NULL */
#define CUR_SLOT(u)        (*(signed char *)((u) + 0x2c4))
#define SARGE_HEALTH(u)    (*(short *)((u) + 0x1c))
#define SARGE_VEHICLE(u)   (*(int *)((u) + 0x2d0))
#define ITEM_WEAPON(item)  (*(BYTE **)((BYTE *)(item) + 0x20))
#define ITEM_TYPE(item)    (*(int *)ITEM_WEAPON(item))
#define W_AMMO(rec)        (*(short *)((BYTE *)(rec) + 0xc))      /* game: nAmmo, nMaxAmmo */
#define W_MAX_AMMO(rec)    (*(short *)((BYTE *)(rec) + 0xe))
#define TYPE_MAIN_ALT      10                                     /* the auto rifle */
#define TYPE_MINE          11
#define TYPE_EXPLOSIVE     12
#define MAX_AMMO           99
#define INPUT              (*(volatile DWORD *)0x6a547c)
#define INPUT_FIRE         0x1

typedef int (__fastcall *CategoryFn)(void *item);
/* the item and its weapon record readable - 2026-11-21 CRASH (user, entering mission 7 after mission 6: EXCEPTION at
 * 0x4492b3 in WeaponSlot FUN_004492b0 called from weapons_draw_markers): an item with NO weapon record (+0x20 NULL,
 * while the next mission's items are being restored) passed, because the old test then re-checked the item itself */
static int safe_item(void *it)
{
    if (!it || IsBadReadPtr(it, 0x40)) return 0;
    void *w = *(void **)((BYTE *)it + 0x20);
    return w && !IsBadReadPtr(w, 0x20);
}
typedef int (__fastcall *SelectSlotFn)(BYTE *unit, int slot);
typedef void (__fastcall *DropFn)(BYTE *unit, int slot, DWORD where);
#define ITEM_CATEGORY_RAW  ((CategoryFn)0x4492b0)
#define ITEM_CATEGORY(it)  (safe_item(it) ? ITEM_CATEGORY_RAW(it) : -1)   /* a freed item: no call into the game */
#define SELECT_SLOT        ((SelectSlotFn)0x448df0)
#define DROP_SLOT          ((DropFn)0x4496b0)    /* as the Drop Item key (0x4611a0); slot 0 never drops */
#define RESET_THROW_CHARGE ((VoidFnPtr)0x43e6c0)

typedef struct { void *type, *item; } Weapon;
static Weapon g_store[BOXES];                   /* weapons kept in a box (not in a game slot) */
static int g_slot_box[SLOTS] = { 0, -1, -1 };   /* box showing each slot's weapon (-1 = slot empty) */
static BYTE *g_owner;                           /* the Sarge the boxes belong to */
static void *g_seen[SLOTS];                     /* slot items at the end of the last frame (drops) */

static int six(void) { return g_cfg.controls_modern && g_cfg.six_weapons; }
static int have_room(void);
static int boxes(void) { return have_room() ? BOXES : SLOTS; }   /* 640 wide: column A only */
static int box_row(int b) { return b % SLOTS; }
static int box_col(int b) { return b / SLOTS; }

static void forget(void)
{
    memset(g_store, 0, sizeof g_store);
    g_slot_box[0] = 0; g_slot_box[1] = g_slot_box[2] = -1;
}

/* Boxes belong to one Sarge; a new one (new mission) or a dead one has none. */
static int g_carry_pending;                     /* the next part's start: the weapons of the last part are put back (carry_apply) */
static void carry_new_run(int scenario, int mission);
static void carry_apply(BYTE *u);
static int valid(BYTE *u)
{
    /* 2026-11-20 crash (user log: EXCEPTION in 0x4492b0 called from here at a mission start): the new mission's
     * Sarge can sit at the SAME address - the boxes of the last mission (freed items) were kept. Also forgotten
     * on a new mission / scenario number now.
     * 2026-11-21 crash (user: 0x43fdc6, the HUD weapon icon read Sarge's current weapon type as "ens/"): mission 7 lost
     * with Sarge alive and entered again from the menus - same Sarge (the static legion record), same mission number,
     * so the boxes of the lost run were kept and a freed weapon record went back into a slot. Now keyed on the mission
     * RUN (gameplay_run: any (re)start, incl. Play Again and the same mission again).
     * 2026-11-21 carry-over: the boxes are still forgotten on every new run (no pointer survives a run), but at the next
     * part of the same scenario (and its Play Again) they are rebuilt from the plain record taken at the win (below). */
    static unsigned run;
    int s, m;
    unsigned r = gameplay_run(&s, &m);
    if (!u || u != g_owner || SARGE_HEALTH(u) <= 0 || r != run) {
        if (r != run && (g_store[1].type || g_store[2].type || g_store[3].type || g_store[4].type || g_store[5].type))
            mlog("weapons: boxes of the last mission run forgotten (run %u)", r);
        if (r != run) carry_new_run(s, m);
        forget(); g_owner = u; run = r; return 0;
    }
    return 1;
}

static void remember(BYTE *u) { for (int c = 0; c < SLOTS; c++) g_seen[c] = SLOT_ITEM(u, c); }

/* The slot whose weapon box b shows, or -1. */
static int slot_of_box(int b)
{
    for (int c = 0; c < SLOTS; c++) if (g_slot_box[c] == b) return c;
    return -1;
}

static Weapon box_weapon(BYTE *u, int b)
{
    int c = slot_of_box(b);
    if (c >= 0) return (Weapon){ SLOT_TYPE(u, c), SLOT_ITEM(u, c) };
    return g_store[b];
}

static int box_free(BYTE *u, int b) { return b > 0 && slot_of_box(b) < 0 && !g_store[b].type; }

static int first_free_box(BYTE *u)
{
    for (int b = 1; b < boxes(); b++) if (box_free(u, b)) return b;
    return -1;
}

/* Keep the boxes in step with the game's slots (a weapon the game put in a slot by itself, e.g.
 * at mission start, gets its kind's own A box, or the first free one). */
static void sync(BYTE *u)
{
    g_slot_box[0] = 0;
    for (int c = 1; c < SLOTS; c++) {
        if (!SLOT_ITEM(u, c)) { g_slot_box[c] = -1; continue; }
        if (g_slot_box[c] >= 0) continue;
        g_slot_box[c] = box_free(u, c) ? c : first_free_box(u);
    }
}

/* Use the weapon of box b: swap it into its kind's slot if needed, then select the slot. */
static int activate(BYTE *u, int b)
{
    Weapon w = box_weapon(u, b);
    if (!w.type) return 0;
    allies_cancel("weapon chosen");               /* allies.c: choosing a weapon puts the medkit away */
    int c = slot_of_box(b);
    if (c < 0) {                                 /* kept in the box: swap into its kind's slot */
        c = ITEM_CATEGORY(w.item);
        if (c < 1 || c >= SLOTS) return 0;
        if (SLOT_ITEM(u, c) && g_slot_box[c] >= 0)
            g_store[g_slot_box[c]] = (Weapon){ SLOT_TYPE(u, c), SLOT_ITEM(u, c) };
        SLOT_TYPE(u, c) = w.type;
        SLOT_ITEM(u, c) = w.item;
        g_store[b] = (Weapon){ 0 };
        g_slot_box[c] = b;
    }
    CUR_SLOT(u) = -1;                            /* FUN_00448df0 does nothing for the current slot */
    int ok = SELECT_SLOT(u, c);
    RESET_THROW_CHARGE();
    remember(u);
    return ok;
}

/* ---------------------------------------------------------------- carry-over to the next part
 * 2026-11-21 (user: "sometimes the weapons don't carry over to a new mission on the same map. make sure they do").
 * What the game does: winning a part writes the army file (FUN_0046a970 -> FUN_0041a3e0). For Sarge it holds only
 * position, health and his vehicle (record 0x8005) - NOT his weapons. The weapons go with the OBJECT records written
 * at its end (FUN_0046b5c0, "OBJS", 0x1c bytes per map object whose +0x70 has bit 0x40: UID, health, +0x70/+0x71
 * flags, position, ammo = WEAPON +0xc). A picked-up item has +0x70 |= 0x60 (0x20 = in the bag) and +0x71 |= 0x40
 * (carried by Sarge; FUN_00449330).
 *   - The next part normally CONTINUES on the loaded map (Wine: same object table, no army file read): Sarge's three
 *     slots simply stay. Only the mod's boxes were lost - valid() forgets them on every new run (the boxes not in a
 *     game slot = g_store: B1..B3, unused A boxes). Wine 2026-11-21 Desert 1 -> 2: flamethrower 99, mortar 15,
 *     grenades 8 forgotten, only the bazooka (in the heavy slot) left.
 *   - Play Again of a later part (and every load of a later part) reloads the map: the army file is read
 *     (FUN_0041a970: SargeDeploy FUN_004487e0 -> FUN_004489c0 empties the heavy / special slots, then FUN_0046b6c0 ->
 *     FUN_0046b750 per object: flags + ammo back, and an item carried by Sarge is picked up again with
 *     FUN_004495c0(1)). That pickup keeps one weapon per kind (a full slot refuses): the other carried weapons stay
 *     "in the bag" but in no slot = lost. Mine / explosive packs get their ORIGINAL charges back from MakeWeapon at
 *     the map load (charges moved in by mines_transfer are gone; ammo > charges). Map unload frees every item's
 *     WEAPON (FUN_00446350), so nothing from the last run may be kept by pointer.
 * Here: at the army file write (capture_army_save -> weapons_army_saved) every carried weapon is recorded as plain
 * data (type, ammo, box, slot, its object number + UID, the charge array); at the first play frame of the next part
 * (or its Play Again: scenario same, mission = recorded + 1) the boxes are rebuilt from it: each item is looked up
 * again in the CURRENT object table by number and UID (the same map file: the same objects), its ammo / charges set,
 * and it goes back into its game slot or its box. The current weapon is selected again. Any other run (a new
 * scenario, the scenario's first part again) drops the record. [controls] carry_weapons = 1 (0 = as before). */
#define OBJECTS_C       (*(BYTE **)0x4e30ec)
#define OBJECT_COUNT_C  (*(int *)0x4e30e8)
#define OBJECT_SIZE_C   0xd0
#define OBJ_UID_C(o)    (*(DWORD *)(o))
#define OBJ_CAT_C(o)    (*(short *)((o) + 0x68))
#define OBJ_F70(o)      (*((BYTE *)(o) + 0x70))
#define CAT_PICKUP_C    0x19                     /* a weapon item (lying on the map or carried) */
#define OF_USED_C       0x04                     /* gone (dropped empty, used up) */
#define OF_IN_BAG_C     0x20                     /* carried */
#define OF_SAVE_C       0x40                     /* written to the army file's object records (FUN_0046b5c0) */
#define W_CHARGES_C(r)  (*(short **)((BYTE *)(r) + 0x1c))
#define CARRY_CHARGES   256
#define CARRY_MAX       (SLOTS + BOXES)
typedef struct { int type, box, slot, obj; DWORD uid; short ammo, max; int ncharges; short charges[CARRY_CHARGES]; } CarryItem;
static struct { int valid, scen, mission, cur_slot, cur_box, n; CarryItem w[CARRY_MAX]; } g_carry;

static int carry_on(void)
{
    static int cfg = -1;
    if (cfg < 0) cfg = GetPrivateProfileIntA("controls", "carry_weapons", 1, mod_ini_path());
    return cfg && six();
}
static int obj_number(void *item)
{
    BYTE *o = OBJECTS_C;
    if (!o || !item || (BYTE *)item < o || (BYTE *)item >= o + (size_t)OBJECT_COUNT_C * OBJECT_SIZE_C) return -1;
    return (int)(((BYTE *)item - o) / OBJECT_SIZE_C);
}
static const char *box_name(int b)
{
    static const char *n[BOXES] = { "A1", "A2", "A3", "B1", "B2", "B3" };
    return b >= 0 && b < BOXES ? n[b] : "--";
}

static void carry_record(CarryItem *e, void *rec, void *item, int box, int slot)
{
    memset(e, 0, sizeof *e);
    e->type = *(int *)rec; e->box = box; e->slot = slot;
    e->ammo = W_AMMO(rec); e->max = W_MAX_AMMO(rec);
    e->obj = obj_number(item);
    e->uid = e->obj >= 0 ? OBJ_UID_C((BYTE *)item) : 0;
    if ((e->type == TYPE_MINE || e->type == TYPE_EXPLOSIVE) && W_CHARGES_C(rec) && e->max > 0) {
        int n = e->max < CARRY_CHARGES ? e->max : CARRY_CHARGES;
        if (!IsBadReadPtr(W_CHARGES_C(rec), (UINT_PTR)n * 2)) { memcpy(e->charges, W_CHARGES_C(rec), (size_t)n * 2); e->ncharges = n; }
    }
}

/* capture.c, at the army file write (a part was won; the next part of the scenario follows) */
void weapons_army_saved(void)
{
    BYTE *u = SARGE;
    int s, m;
    gameplay_run(&s, &m);
    g_carry.valid = 0;
    if (!carry_on() || !u || SARGE_HEALTH(u) <= 0) return;
    valid(u); sync(u);
    g_carry.scen = s; g_carry.mission = m; g_carry.n = 0;
    g_carry.cur_slot = CUR_SLOT(u);
    g_carry.cur_box = g_carry.cur_slot >= 0 && g_carry.cur_slot < SLOTS ? g_slot_box[g_carry.cur_slot] : -1;
    for (int c = 0; c < SLOTS; c++) {
        void *rec = SLOT_TYPE(u, c), *it = SLOT_ITEM(u, c);
        if (!rec || IsBadReadPtr(rec, 0x20) || (c > 0 && !safe_item(it))) continue;
        carry_record(&g_carry.w[g_carry.n++], rec, safe_item(it) ? it : NULL, c ? g_slot_box[c] : 0, c);
    }
    for (int b = 0; b < BOXES; b++) {
        if (!g_store[b].type || !safe_item(g_store[b].item) || g_carry.n >= CARRY_MAX) continue;
        carry_record(&g_carry.w[g_carry.n++], g_store[b].type, g_store[b].item, b, -1);
    }
    char line[400];
    int len = 0;
    line[0] = 0;
    for (int i = 0; i < g_carry.n && len < (int)sizeof line - 48; i++) {
        CarryItem *e = &g_carry.w[i];
        len += wsprintfA(line + len, " %s:type %d ammo %d%s", box_name(e->box), e->type, e->ammo, e->slot >= 0 ? "*" : "");
        if (e->ncharges) len += wsprintfA(line + len, " (%d charges)", e->ncharges);
    }
    g_carry.valid = 1;
    mlog("weapons: part won (scenario %d mission %d) - weapons recorded for the next part:%s (* = in a game slot; current %s)",
         s, m, g_carry.n ? line : " none", box_name(g_carry.cur_box));
}

/* a new mission run: is it the next part (or its Play Again) of the recorded one? */
static void carry_new_run(int scenario, int mission)
{
    if (!g_carry.valid) return;
    if (scenario == g_carry.scen && mission == g_carry.mission + 1) {
        g_carry_pending = 1;
        mlog("weapons: scenario %d mission %d starts - the %d weapon(s) of mission %d will be put back", scenario, mission, g_carry.n, g_carry.mission);
    } else {
        g_carry_pending = 0; g_carry.valid = 0;
        mlog("weapons: scenario %d mission %d is not the next part of %d / %d - recorded weapons dropped", scenario, mission, g_carry.scen, g_carry.mission);
    }
}

/* the recorded item in the current object table (same number, same UID, still a weapon of that type), or NULL */
static BYTE *carry_find(const CarryItem *e)
{
    BYTE *o = OBJECTS_C;
    if (!o || e->obj < 0 || e->obj >= OBJECT_COUNT_C) return NULL;
    o += (size_t)e->obj * OBJECT_SIZE_C;
    if (OBJ_UID_C(o) != e->uid || OBJ_CAT_C(o) != CAT_PICKUP_C || !safe_item(o) || ITEM_TYPE(o) != e->type) return NULL;
    return o;
}

static void carry_apply(BYTE *u)
{
    if (!g_carry.valid || !carry_on()) return;
    void *had_rec[SLOTS] = { 0 }, *had_item[SLOTS] = { 0 };
    for (int c = 1; c < SLOTS; c++) {                 /* what the game put into the slots itself */
        had_rec[c] = SLOT_TYPE(u, c); had_item[c] = SLOT_ITEM(u, c);
        SLOT_TYPE(u, c) = NULL; SLOT_ITEM(u, c) = NULL;
    }
    forget();
    int back = 0, lost = 0;
    for (int i = 0; i < g_carry.n; i++) {
        const CarryItem *e = &g_carry.w[i];
        if (e->slot == 0) {                           /* the rifle slot: the auto rifle is an item, the plain rifle is not */
            BYTE *o = e->obj >= 0 ? carry_find(e) : NULL;
            if (o && SLOT_TYPE(u, 0) != ITEM_WEAPON(o)) {
                SLOT_TYPE(u, 0) = ITEM_WEAPON(o); SLOT_ITEM(u, 0) = o;
                if (!(OBJ_F70(o) & OF_IN_BAG_C)) OBJ_F70(o) |= OF_IN_BAG_C | OF_SAVE_C;
                mlog("weapons:   A1 type %d (item #%d) back in the rifle slot", e->type, e->obj);
            }
            continue;
        }
        BYTE *o = carry_find(e);
        if (!o || (OBJ_F70(o) & OF_USED_C)) {
            mlog("weapons:   %s type %d ammo %d: item #%d (uid %08lx) %s - not put back", box_name(e->box), e->type, e->ammo, e->obj, e->uid,
                 o ? "used up" : "not in this map");
            lost++;
            continue;
        }
        BYTE *rec = ITEM_WEAPON(o);
        if (!(OBJ_F70(o) & OF_IN_BAG_C)) OBJ_F70(o) |= OF_IN_BAG_C | OF_SAVE_C;   /* not restored as carried: it is again */
        if (e->ncharges) {
            int k = mines_set_charges(rec, e->charges, e->ncharges);
            if (k != e->ncharges) mlog("weapons:   %s type %d: %d of %d recorded charges found", box_name(e->box), e->type, k, e->ncharges);
        } else if (W_MAX_AMMO(rec) < e->max) W_MAX_AMMO(rec) = e->max;
        int was = W_AMMO(rec);
        W_AMMO(rec) = e->ammo;
        int c = e->slot, b = e->box;
        if (c >= 1 && c < SLOTS && !SLOT_ITEM(u, c) && ITEM_CATEGORY(o) == c) {
            SLOT_TYPE(u, c) = rec; SLOT_ITEM(u, c) = o;
            if (b < 1 || b >= boxes() || slot_of_box(b) >= 0 || g_store[b].type) b = first_free_box(u);
            g_slot_box[c] = b;
        } else {
            if (b < 1 || b >= boxes() || !box_free(u, b)) b = first_free_box(u);
            if (b < 0) { mlog("weapons:   type %d: no free box - not put back", e->type); lost++; continue; }
            g_store[b] = (Weapon){ rec, o };
        }
        back++;
        char extra[48] = "";
        if (e->ncharges) wsprintfA(extra, ", %d charge(s) in the bag", mines_free_charges(rec));
        mlog("weapons:   %s type %d ammo %d (was %d%s) item #%d%s", box_name(b), e->type, e->ammo, was, extra, e->obj,
             c >= 1 && c < SLOTS && SLOT_ITEM(u, c) == o ? " in its game slot" : "");
    }
    for (int c = 1; c < SLOTS; c++) {                 /* something the game had in a slot that is not in the record: keep it */
        if (!had_item[c] || !safe_item(had_item[c])) continue;
        int found = 0;
        for (int b = 0; b < BOXES && !found; b++) found = g_store[b].item == had_item[c];
        for (int k = 1; k < SLOTS && !found; k++) found = SLOT_ITEM(u, k) == had_item[c];
        if (found) continue;
        if (!SLOT_ITEM(u, c)) { SLOT_TYPE(u, c) = had_rec[c]; SLOT_ITEM(u, c) = had_item[c]; g_slot_box[c] = -1; }
        else { int b = first_free_box(u); if (b >= 0) g_store[b] = (Weapon){ had_rec[c], had_item[c] }; }
        mlog("weapons:   type %d (in a game slot, not recorded) kept", ITEM_TYPE(had_item[c]));
    }
    sync(u);
    int cur = g_carry.cur_slot;                       /* the weapon in use at the win */
    if (cur < 0 || cur >= SLOTS || !SLOT_TYPE(u, cur)) cur = 0;
    if (!SARGE_VEHICLE(u) && *(void **)(u + 0x20) != SLOT_TYPE(u, cur)) { CUR_SLOT(u) = -1; SELECT_SLOT(u, cur); RESET_THROW_CHARGE(); }
    remember(u);
    mlog("weapons: %d weapon(s) of mission %d put back%s", back, g_carry.mission, lost ? " (some lost: see above)" : "");
}

/* ---------------------------------------------------------------- pickup */

TouchFn weapons_touch_original;

/* The box holding a weapon of this type, or -1. */
static int carried_box(BYTE *u, int type)
{
    for (int b = 0; b < boxes(); b++) {
        Weapon w = box_weapon(u, b);
        if (w.type && *(int *)w.type == type) return b;
    }
    return -1;
}

/* Take the item's ammo into the carried weapon of the same type (up to MAX_AMMO) and let the game
 * remove the emptied item: it is picked up into its slot for a moment and dropped with no ammo. */
static void merge_ammo(BYTE *u, void *item, int arg, int dup_box)
{
    int c = ITEM_CATEGORY(item);
    int cur = CUR_SLOT(u);
    Weapon held = { SLOT_TYPE(u, c), SLOT_ITEM(u, c) };
    SLOT_TYPE(u, c) = NULL; SLOT_ITEM(u, c) = NULL;
    PX_GAME(weapons_touch_original(u, item, arg));
    if (SLOT_ITEM(u, c) != item) {               /* not picked up after all */
        SLOT_TYPE(u, c) = held.type; SLOT_ITEM(u, c) = held.item;
        return;
    }
    Weapon dup = slot_of_box(dup_box) == c ? held : g_store[dup_box];
    BYTE *rec = ITEM_WEAPON(item);
    int total = W_AMMO(dup.type) + W_AMMO(rec);
    if (total > MAX_AMMO) total = MAX_AMMO;
    W_AMMO(dup.type) = (short)total;
    int charges = *(int *)dup.type == TYPE_MINE || *(int *)dup.type == TYPE_EXPLOSIVE;   /* nMaxAmmo = their array length */
    if (!charges && W_MAX_AMMO(dup.type) < total) W_MAX_AMMO(dup.type) = (short)total;
    W_AMMO(rec) = 0;
    DROP_SLOT(u, c, *(DWORD *)(u + 4));          /* empty: the game destroys it */
    SLOT_TYPE(u, c) = held.type; SLOT_ITEM(u, c) = held.item;
    if (cur >= 0 && cur < SLOTS && CUR_SLOT(u) != cur) { CUR_SLOT(u) = -1; SELECT_SLOT(u, cur); }
    if (g_cfg.log >= 2) mlog("weapons: type %d ammo added, now %d", ITEM_TYPE(item), total);
    remember(u);
}

static void __fastcall weapons_touch_px(BYTE *unit, void *item, int arg) {
    /* arg 1 = the army file load putting a carried item back (FUN_0046b750, after a map reload): the boxes still hold
     * the last run's items (freed with that map) - forgotten; the game's own pickup runs (one weapon per kind), the
     * rest of the boxes is rebuilt at the first play frame (carry_apply) */
    if (arg) {
        if (g_cfg.log >= 1) mlog("weapons: army file load puts item #%d (type %d) back - the game's pickup", obj_number(item), safe_item(item) ? ITEM_TYPE(item) : -1);
        if (unit == SARGE) forget();
        PX_GAME(weapons_touch_original(unit, item, arg));
        return;
    }
    BYTE *u = SARGE;
    if (!six() || unit != u || !item || SARGE_VEHICLE(u)) { PX_GAME(weapons_touch_original(unit, item, arg)); return; }
    valid(u);
    sync(u);
    int c = ITEM_CATEGORY(item), type = ITEM_TYPE(item);
    if (c == 0) { PX_GAME(weapons_touch_original(unit, item, arg)); remember(u); return; }   /* auto rifle upgrade, ammo, health ... */

    int dup = carried_box(u, type);
    if (dup >= 0) {                              /* the same weapon again: take its ammo */
        BYTE *rec = ITEM_WEAPON(item);
        Weapon d = box_weapon(u, dup);
        /* Every weapon stacks up to MAX_AMMO (2026-11-21, user: "mines are not stacking fully ... same for all weapons ...
         * 99 is the cap"); the rest stays in the pack on the ground (its ammo lowered). An EMPTY carried weapon takes ammo
         * too; unlimited ones (ammo < 0) take nothing (the pickup stays, as the original "fully loaded").
         * Mines / explosives: their charges are objects kept in an array (WEAPON +0x1c, nMaxAmmo long): the pack's real
         * charges move into the carried weapon (mines_transfer: into used-up slots, else the array is grown with the
         * game's allocator), so ammo always matches charges in the bag - not limited by the weapon's own nMaxAmmo any more. */
        if (type == TYPE_MINE || type == TYPE_EXPLOSIVE) {
            int have = W_AMMO(d.type), cap = MAX_AMMO, got = W_AMMO(rec);
            if (have < 0 || have >= cap || got <= 0) return;
            int want = cap - have < got ? cap - have : got;
            int take = mines_transfer(d.type, rec, want);
            if (take <= 0) return;
            if (take < got) {
                W_AMMO(rec) = (short)(got - take); W_AMMO(d.type) = (short)(have + take);
                OBJ_F70(item) |= OF_SAVE_C;                /* the pack keeps its lowered ammo / charges in the army file */
                if (g_cfg.log >= 1) mlog("weapons: type %d topped up %d -> %d (cap %d), %d left in the pack", type, have, have + take, cap, got - take);
                remember(u);
                return;
            }
            merge_ammo(u, item, arg, dup);               /* all of it moved: the emptied pack is removed */
            return;
        }
        int have = W_AMMO(d.type), got = W_AMMO(rec);
        if (got <= 0 || have < 0 || have >= MAX_AMMO) return;
        if (have + got > MAX_AMMO) {                     /* only part fits: the rest stays in the pack */
            W_AMMO(rec) = (short)(have + got - MAX_AMMO); W_AMMO(d.type) = MAX_AMMO;
            OBJ_F70(item) |= OF_SAVE_C;                    /* the pack keeps its lowered ammo in the army file (a reload gave it all back) */
            if (W_MAX_AMMO(d.type) < MAX_AMMO) W_MAX_AMMO(d.type) = MAX_AMMO;
            if (g_cfg.log >= 1) mlog("weapons: type %d topped up %d -> %d (cap), %d left in the pack", type, have, MAX_AMMO, have + got - MAX_AMMO);
            remember(u);
            return;
        }
        merge_ammo(u, item, arg, dup);
        return;
    }
    int b = first_free_box(u);
    if (b < 0) return;                           /* all boxes full: leave it lying */
    int cur = CUR_SLOT(u), stashed = 0;
    Weapon before = { SLOT_TYPE(u, c), SLOT_ITEM(u, c) };
    if (before.item) {                           /* the slot's weapon stays in its box */
        g_store[g_slot_box[c]] = before;
        SLOT_TYPE(u, c) = NULL; SLOT_ITEM(u, c) = NULL;
        stashed = 1;
    }
    int old_box = g_slot_box[c];
    PX_GAME(weapons_touch_original(unit, item, arg));
    if (SLOT_ITEM(u, c) != item) {               /* not picked up: put things back */
        if (stashed) { SLOT_TYPE(u, c) = before.type; SLOT_ITEM(u, c) = before.item; g_store[old_box] = (Weapon){ 0 }; }
        return;
    }
    g_slot_box[c] = b;
    if (stashed && cur == c) activate(u, old_box);   /* keep using the weapon that was in use */
    if (g_cfg.log >= 2) mlog("weapons: picked up type %d (kind %d) into box %c%d", type, c, box_col(b) ? 'B' : 'A', box_row(b) + 1);
    remember(u);
}
void __fastcall weapons_touch(BYTE *unit, void *item, int arg) { PX_BEGIN(px); weapons_touch_px(unit, item, arg); PX_END(px, PR_CHOOKS); }   /* timed: stage PR_CHOOKS (exclusive, common.h) */

/* ---------------------------------------------------------------- per frame */

/* After a drop (the slot's item is gone), another carried weapon of that kind moves into the
 * slot - it keeps its box. */
static void track_current(BYTE *u);
void weapons_frame(void)
{
    BYTE *u = SARGE;
    if (u && !SARGE_VEHICLE(u)) track_current(u);
    if (!six() || !valid(u)) { memset(g_seen, 0, sizeof g_seen); return; }
    if (g_carry_pending && layout_view_is_gameplay()) { g_carry_pending = 0; carry_apply(u); }   /* in play: after the game's own restore */
    for (int c = 1; c < SLOTS; c++) {
        if (!g_seen[c] || SLOT_ITEM(u, c)) continue;          /* no drop */
        g_slot_box[c] = -1;
        for (int b = 1; b < boxes(); b++) {
            if (!g_store[b].item || ITEM_CATEGORY(g_store[b].item) != c) continue;
            SLOT_TYPE(u, c) = g_store[b].type;
            SLOT_ITEM(u, c) = g_store[b].item;
            g_store[b] = (Weapon){ 0 };
            g_slot_box[c] = b;
            if (CUR_SLOT(u) == c) { CUR_SLOT(u) = -1; SELECT_SLOT(u, c); }
            if (g_cfg.log >= 2) mlog("weapons: kind %d dropped, box %d moves into the slot", c, b);
            break;
        }
    }
    sync(u);
    remember(u);
}

/* ---------------------------------------------------------------- mouse wheel */

static int current_box(BYTE *u)
{
    int cur = CUR_SLOT(u);
    if (cur < 0 || cur >= SLOTS || g_slot_box[cur] < 0) return 0;
    return g_slot_box[cur];
}

/* Quick swap (Q): back to the box (six weapons) or slot used before the current one. */
static int g_cur_seen = -1, g_prev = -1;
static void track_current(BYTE *u)
{
    int c = six() ? current_box(u) : CUR_SLOT(u);
    if (c != g_cur_seen) { g_prev = g_cur_seen; g_cur_seen = c; }
}

void weapons_quick_swap(void)
{
    BYTE *u = SARGE;
    if (!u || SARGE_VEHICLE(u) || (INPUT & INPUT_FIRE)) return;
    track_current(u);
    if (g_prev < 0) return;
    if (six()) {
        valid(u); sync(u);
        if (box_weapon(u, g_prev).type) activate(u, g_prev);
    } else if (g_prev < SLOTS && SLOT_ITEM(u, g_prev)) {
        allies_cancel("weapon chosen");
        SELECT_SLOT(u, g_prev);
    }
    track_current(u);
    if (g_cfg.log >= 2) mlog("weapons: quick swap -> %d", g_cur_seen);
}

/* Steps through the boxes holding a weapon in screen order A1 A2 A3 B1 B2 B3, and the medkit box (2026-11-22: a stop
 * of its own - in its free box, or after B3 when it is the small header box); notches > 0 = wheel up. */
static int kit_box(BYTE *u);
#define KIT_SMALL       (-2)           /* kit_box: the small box in the column header (below) */
static int kit_at(BYTE *u, int x, int y);
void weapons_wheel(int notches)
{
    BYTE *u = SARGE;
    if (!notches || !u || SARGE_VEHICLE(u) || (INPUT & INPUT_FIRE)) return;
    if (!six()) {                                 /* three slots: only the medkit is stepped here (the game has no wheel) */
        if (allies_heal_mode()) allies_cancel("mouse wheel");
        else if (kit_box(u) != -1) allies_select_heal(1, "mouse wheel");
        return;
    }
    valid(u);
    sync(u);
    int n = boxes(), kit = kit_box(u), step = notches > 0 ? -1 : 1;
    int stops = n + (kit == KIT_SMALL);           /* stop n = the small kit box */
    int kit_stop = kit == KIT_SMALL ? n : kit;     /* -1 = no kit */
    int pos = allies_heal_mode() && kit_stop >= 0 ? kit_stop : current_box(u);
    for (int i = 1; i < stops; i++) {
        int b = ((pos + step * i) % stops + stops) % stops;
        if (b == kit_stop) {
            allies_select_heal(1, "mouse wheel");
            if (g_cfg.log >= 2) mlog("weapons: wheel -> medkit box");
            return;
        }
        if (b < n && box_weapon(u, b).type && activate(u, b)) {
            if (g_cfg.log >= 2) mlog("weapons: wheel -> box %c%d", box_col(b) ? 'B' : 'A', box_row(b) + 1);
            return;
        }
    }
    allies_cancel("mouse wheel");                 /* nothing else to step to */
}

/* ---------------------------------------------------------------- HUD boxes */

/* HUD weapon boxes: widget records at DAT_00696eac + 0x44 (main) / 0x4c (heavy) / 0x50 (special);
 * widget +8 -> data, data +0xc / +0xe = design x / y of the icon (249, 400 / 421 / 442), data +0x10
 * = icon picture table indexed like the game does (FUN_0043fd40, FUN_004402b0). */
#define HUD_WIDGETS  (*(BYTE **)0x696eac)
static const int WIDGET_OFS[SLOTS] = { 0x44, 0x4c, 0x50 };
#define COL_SRC_X    243        /* the weapon column in the panel picture: x and width */
#define COL_SRC_W    74
#define PANEL_H      115
#define BOX_ICON_X   249        /* icon x in the panel's weapon column */
#define BOX_W        64         /* a box around an icon (for clicks, the highlight, repainting) */
#define BOX_H        23

typedef HRESULT (WINAPI *BltFastFn)(void *, DWORD, DWORD, void *, RECT *, DWORD);

static int have_room(void) { return g_cfg.width >= LAYOUT_DESIGN_W + COL_SRC_W; }

int weapons_hud_gap(void) { return six() && have_room() ? COL_SRC_W : 0; }

static BYTE *widget_data(int c)
{
    BYTE *widget = HUD_WIDGETS ? *(BYTE **)(HUD_WIDGETS + WIDGET_OFS[c]) : NULL;
    return widget ? *(BYTE **)(widget + 8) : NULL;
}

static int icon_y(int row)        /* design y of a box row's icon */
{
    BYTE *d = widget_data(row);
    return d ? *(short *)(d + 0xe) : 400 + 21 * row;
}

/* Game-screen x of the left edge of column col (A = in the panel, B = in the gap). */
static int column_x(int col)
{
    int a = COL_SRC_X + layout_hud_dx(COL_SRC_X);
    return col ? LAYOUT_HUD_SPLIT_X + layout_hud_dx(LAYOUT_HUD_SPLIT_X - 1) : a;
}

/* Game-screen rectangle of box b. */
static RECT box_rect(int b)
{
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    int x = column_x(box_col(b)) + BOX_ICON_X - COL_SRC_X - 3, y = icon_y(box_row(b)) + dy - 2;
    return (RECT){ x, y, x + BOX_W, y + BOX_H };
}

int weapons_ui_rect(RECT *r)        /* the spare-weapon column on the 1x HUD */
{
    if (!weapons_hud_gap() || !layout_view_is_gameplay()) return 0;
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    SetRect(r, column_x(1), LAYOUT_HUD_PANEL_Y + dy, column_x(1) + COL_SRC_W, LAYOUT_HUD_PANEL_Y + dy + PANEL_H);
    return 1;
}
int weapons_ui_at(int x, int y)     /* a game-screen point (the HUD's middle may be enlarged: layout_unscale) */
{
    RECT r;
    if (!weapons_ui_rect(&r)) return 0;
    layout_unscale(&x, &y);
    POINT p = { x, y };
    if (!PtInRect(&r, p)) return 0;
    return !layout_art_margin(COL_SRC_X + (x - r.left), y - r.top);   /* its margin (it copies the weapon column) is battlefield */
}

/* Which box is at a game-screen point, or -1. */
static int box_at(int x, int y)
{
    layout_unscale(&x, &y);                      /* the HUD's middle may be enlarged */
    POINT p = { x, y };
    for (int b = 0; b < boxes(); b++) {
        RECT r = box_rect(b);
        if (PtInRect(&r, p)) return b;
    }
    return -1;
}

/* Left click on the HUD: use the weapon in that box. */
void weapons_click_at(int x, int y)
{
    BYTE *u = SARGE;
    if (u && layout_view_is_gameplay() && kit_at(u, x, y)) {   /* the medkit box: select it (again: put it away, like H) */
        if (allies_heal_mode()) allies_cancel("medkit box clicked again");
        else allies_select_heal(1, "medkit box clicked");
        return;
    }
    int b = box_at(x, y);
    if (b < 0 || !six() || !u || SARGE_VEHICLE(u) || !layout_view_is_gameplay()) return;
    valid(u);
    sync(u);
    if (activate(u, b) && g_cfg.log >= 2) mlog("weapons: box %c%d clicked", box_col(b) ? 'B' : 'A', box_row(b) + 1);
}

/* Right click on the HUD: drop the weapon in that box (not the rifle). */
void weapons_drop_at(int x, int y)
{
    BYTE *u = SARGE;
    int b = box_at(x, y);
    if (b <= 0 || !g_cfg.controls_modern || !u || SARGE_VEHICLE(u) || (INPUT & INPUT_FIRE) || !layout_view_is_gameplay()) return;
    if (six()) { valid(u); sync(u); }
    if (!box_weapon(u, b).item) return;          /* empty */
    if (six() && !activate(u, b)) return;
    int c = six() ? slot_of_box(b) : b;
    if (c <= 0) return;
    DROP_SLOT(u, c, *(DWORD *)(u + 4));
    RESET_THROW_CHARGE();
    if (g_cfg.log >= 2) mlog("weapons: box %c%d dropped", box_col(b) ? 'B' : 'A', box_row(b) + 1);
}

/* ---------------------------------------------------------------- drawing */

static int nearest_colour(int r, int g, int b)
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    if (!p) return 255;
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = p->e[i].peRed - r, dg = p->e[i].peGreen - g, db = p->e[i].peBlue - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static int weapon_kind(Weapon w) { return w.item ? ITEM_CATEGORY(w.item) : 0; }

/* The weapon's icon (from its kind's widget picture table) at box b. */
static void draw_icon(Weapon w, int b)
{
    if (!w.type) return;
    int kind = weapon_kind(w);
    if (kind < 0 || kind >= SLOTS) return;
    BYTE *d = widget_data(kind);
    if (!d || !*(void ***)(d + 0x10)) return;
    int type = *(int *)w.type;
    int state = kind == 0 ? (type == TYPE_MAIN_ALT ? 2 : 1) : type;
    if (state < 0 || state > 0x40) return;
    void *pic = (*(void ***)(d + 0x10))[state];
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    if (pic) hud_draw_picture_raw(pic, column_x(box_col(b)) + BOX_ICON_X - COL_SRC_X, icon_y(box_row(b)) + dy);
}

/* Copies box b's row of the panel's weapon column (its empty artwork) to box b on the screen. */
static void paint_box(Surf *back, void *art, RECT src, int b)
{
    RECT r = box_rect(b);
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    int sy = r.top - dy - LAYOUT_HUD_PANEL_Y, sx = BOX_ICON_X - 3;   /* panel picture coordinates */
    RECT s = { src.left + sx, src.top + sy, src.left + sx + BOX_W, src.top + sy + BOX_H };
    ((BltFastFn)back->vtbl[7])(back, r.left, r.top, art, &s, DDBLTFAST_WAIT);
}

static void frame_box(Surf *back, RECT r, BYTE colour)
{
    if (r.left < 0 || r.top < 0 || r.right > back->w || r.bottom > back->h) return;
    for (int x = r.left; x < r.right; x++) {
        back->bits[(size_t)r.top * back->pitch + x] = colour;
        back->bits[(size_t)(r.bottom - 1) * back->pitch + x] = colour;
    }
    for (int y = r.top; y < r.bottom; y++) {
        back->bits[(size_t)y * back->pitch + r.left] = colour;
        back->bits[(size_t)y * back->pitch + r.right - 1] = colour;
    }
}

/* ---------------------------------------------------------------- ammo counts
 * The game writes the heavy and special ammo counts into two strings (HUD widget DAT_00696eac+0x58,
 * FUN_004402b0: "%d" of ammo % 100, or the string at 0x696ee0 when empty) and draws them with
 * FUN_0044b760 inside the weapon column. hud.c tells us where and with which font and colour
 * (weapons_note_text), so the same text can be drawn for any box. */
#define AMMO_EMPTY_TEXT ((const char *)0x696ee0)
typedef struct { int seen, dx, dy, font, colour; } AmmoText;   /* dx, dy: from the box's icon */
static AmmoText g_ammo_text[SLOTS];

/* Called by hud.c for every string drawn into the back buffer (design coordinates). Only learned
 * while the slots show in their own boxes, so the offsets belong to the right kind. */
void weapons_note_text(int x, int y, int font, int colour)
{
    if (x < COL_SRC_X || x >= COL_SRC_X + COL_SRC_W || y < LAYOUT_HUD_PANEL_Y || y >= LAYOUT_HUD_PANEL_Y + PANEL_H) return;
    int best = -1, best_d = 1 << 30;
    for (int c = 1; c < SLOTS; c++) {             /* the box whose icon row is nearest above the text */
        int d = y - icon_y(c);
        if (d >= -4 && d < best_d) { best_d = d; best = c; }
    }
    if (best < 0 || best_d > BOX_H) return;
    if (!g_ammo_text[best].seen && g_cfg.log >= 2) mlog("weapons: ammo text of kind %d at %d,%d (font %d colour %d)", best, x, y, font, colour);
    g_ammo_text[best] = (AmmoText){ 1, x - BOX_ICON_X, y - icon_y(best), font, colour };
}

/* ---------------------------------------------------------------- unlimited ammo
 * 2026-11-20 (user screenshot, captured jeep in Desert 2: "weapon on jeep missing ammo count"): vehicle weapons
 * (Sarge +0x2d0 -> vehicle +0x20 -> WEAPON; types 6 = tank cannon, 7 / 8 = machine guns) can carry nAmmo -1 =
 * unlimited (the Desert jeep: type 7, -1 / -1, Wine log). The game's widget +0x48 (FUN_004402b0 at 0x4405f4)
 * prints "%d" only for ammo >= 0, otherwise the empty string 0x696ee0 - so the black ammo box that is part of the
 * vehicle weapon icon (widget +0x44 states 3 / 4) stayed empty: the black square. An infinity mark is drawn into
 * that box instead, in the colour the game prints its digits with (a solid palette index, FUN_00404660).
 * The box is found in the game picture from the text position (the black pixels around it), so the mark sits
 * centred in whatever box art is there; nothing is drawn where no dark box is. */
#define VEH_WEAPON(v)        (*(BYTE **)((BYTE *)(v) + 0x20))
#define VTYPE_LARGE_CANNON   6          /* widget +0x44 state 3 */
#define VTYPE_GUN_LAST       8          /* 7..8: state 4 */
#define AMMO_TEXT_DX         42         /* the game's ammo text: design 291,402 / 423 / 443 = icon + (42, 2) */
#define AMMO_TEXT_DY         2
#define AMMO_COLOUR          250        /* the game's colour for the ammo digits (FUN_0043fd40 -> FUN_0044b760) */
static const char *const INFINITY_MARK[] = {
    ".##...##.",
    "#..#.#..#",
    "#...#...#",
    "#..#.#..#",
    ".##...##.",
};
#define MARK_W 9
#define MARK_H 5

static int dark_index(int i)
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    return p && p->e[i].peRed + p->e[i].peGreen + p->e[i].peBlue < 90;
}

/* (x, y): game-screen position where the game would print the count. */
static void draw_unlimited(Surf *back, int x, int y, int colour)
{
    int sx = x + 3, sy = y + 3;                   /* inside the box, past the frame */
    if (!back || !back->bits || sx < 1 || sy < 1 || sx >= back->w - 1 || sy >= back->h - 1) return;
#define PX(px, py) back->bits[(size_t)(py) * back->pitch + (px)]
    BYTE k = PX(sx, sy);
    if (!dark_index(k)) return;
    int l = sx, r = sx, t = sy, b = sy;
    while (l > 0 && sx - l < 24 && PX(l - 1, sy) == k) l--;
    while (r < back->w - 1 && r - sx < 24 && PX(r + 1, sy) == k) r++;
    while (t > 0 && sy - t < 16 && PX(sx, t - 1) == k) t--;
    while (b < back->h - 1 && b - sy < 16 && PX(sx, b + 1) == k) b++;
    int w = r - l + 1, h = b - t + 1;
    if (w < MARK_W + 2 || h < MARK_H + 2) return;
    int ox = l + (w - MARK_W) / 2, oy = t + (h - MARK_H) / 2;
    for (int j = 0; j < MARK_H; j++)
        for (int i = 0; i < MARK_W; i++)
            if (INFINITY_MARK[j][i] == '#') PX(ox + i, oy + j) = (BYTE)colour;
#undef PX
}

/* Sarge's vehicle weapon with unlimited ammo: the mark in the main (A1) box, where the game draws its icon. */
static void draw_vehicle_unlimited(Surf *back)
{
    BYTE *u = SARGE;
    if (!u || !back || !HUD_WIDGETS || !layout_view_is_gameplay()) return;
    BYTE *v = (BYTE *)SARGE_VEHICLE(u);
    if (!v || IsBadReadPtr(v, 0x24)) return;
    BYTE *w = VEH_WEAPON(v);
    if (!w || IsBadReadPtr(w, 0x10)) return;
    int type = *(int *)w;
    if (type < VTYPE_LARGE_CANNON || type > VTYPE_GUN_LAST || W_AMMO(w) >= 0) return;
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    AmmoText *t = g_ammo_text[1].seen ? &g_ammo_text[1] : g_ammo_text[2].seen ? &g_ammo_text[2] : NULL;
    int tx = t ? t->dx : AMMO_TEXT_DX, ty = t ? t->dy : AMMO_TEXT_DY, colour = t ? t->colour : AMMO_COLOUR;
    draw_unlimited(back, column_x(0) + BOX_ICON_X - COL_SRC_X + tx, icon_y(0) + dy + ty, colour);
}

static void draw_ammo(Surf *back, Weapon w, int b)
{
    int kind = weapon_kind(w);
    if (kind < 1 || kind >= SLOTS) return;
    AmmoText *t = &g_ammo_text[kind];
    if (!t->seen && g_ammo_text[3 - kind].seen) t = &g_ammo_text[3 - kind];   /* same layout */
    if (!t->seen || !w.type || !hud_draw_string_original) return;
    int ammo = W_AMMO(w.type);
    char buf[16];
    if (ammo < 1) lstrcpynA(buf, AMMO_EMPTY_TEXT, sizeof buf);
    else wsprintfA(buf, "%d", ammo % 100);
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    int col = box_col(b);
    int x = column_x(col) + BOX_ICON_X - COL_SRC_X + t->dx, y = icon_y(box_row(b)) + dy + t->dy;
    int clip[2] = { column_x(col) + COL_SRC_W - x, BOX_H };
    if (clip[0] <= 0) return;
    if (ammo < 0) { draw_unlimited(back, x, y, t->colour); return; }
    PX_GAME(hud_draw_string_original(back, x, y, buf, t->font, clip, t->colour));
}

/* The game drew each slot's weapon (and its ammo, and the highlight on the current slot) in that
 * slot's own A box. Every A box that shows something else is repainted from the panel art with
 * its real weapon; column B is drawn from scratch; the highlight goes to the box in use.
 * Neighbouring boxes share frame rows, so the game's own highlight is saved first and put back. */
static void draw_columns(Surf *back, BYTE *u)
{
    void *art;
    RECT src;
    if (!hud_panel_art(&art, &src)) return;
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    int bx = column_x(1), by = LAYOUT_HUD_PANEL_Y + dy;
    int room = have_room();
    if (room && (bx < 0 || bx + COL_SRC_W > back->w || by + PANEL_H > back->h)) return;
    RECT s = { src.left + COL_SRC_X, src.top, src.left + COL_SRC_X + COL_SRC_W, src.top + PANEL_H };

    if (!u) {                                     /* no living Sarge: just the empty column */
        if (room) ((BltFastFn)back->vtbl[7])(back, bx, by, art, &s, DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY);
        return;
    }
    int cur = CUR_SLOT(u);
    int cur_box = cur >= 0 && cur < SLOTS ? g_slot_box[cur] : -1;
    int heal = allies_heal_mode();                             /* the medkit box has the highlight (draw_kit_box) */
    int keep = !heal && cur >= 0 && cur < SLOTS && cur_box == cur;   /* the game's highlight is right */
    RECT kr = keep ? box_rect(cur) : (RECT){ 0, 0, 0, 0 };
    static BYTE saved[BOX_H][BOX_W];
    if (keep && (kr.left < 0 || kr.top < 0 || kr.right > back->w || kr.bottom > back->h)) keep = 0;
    if (keep) for (int y = 0; y < BOX_H; y++) memcpy(saved[y], back->bits + (size_t)(kr.top + y) * back->pitch + kr.left, BOX_W);

    if (room) ((BltFastFn)back->vtbl[7])(back, bx, by, art, &s, DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY);
    int repaint[SLOTS] = { 0 };
    for (int k = 1; k < SLOTS; k++)
        if (g_slot_box[k] != k) { repaint[k] = 1; paint_box(back, art, src, k); }
    if (heal && cur >= 0 && cur < SLOTS && cur_box == cur && !repaint[cur]) { repaint[cur] = 1; paint_box(back, art, src, cur); }   /* its highlight off */
    if (keep) for (int y = 0; y < BOX_H; y++) memcpy(back->bits + (size_t)(kr.top + y) * back->pitch + kr.left, saved[y], BOX_W);

    for (int b = 0; b < boxes(); b++) {
        if (box_col(b) == 0 && !repaint[box_row(b)]) continue;   /* the game drew it right */
        Weapon w = box_weapon(u, b);
        draw_icon(w, b);
        draw_ammo(back, w, b);
    }
    if (!heal && !keep && cur_box > 0) frame_box(back, box_rect(cur_box), (BYTE)nearest_colour(40, 240, 40));
}

/* The minesweeper's box (2026-11-21, mines.c detector): a lamp in its top-left corner - dark green = on, nothing in
 * range; blinking red = mines within the detector radius (faster the nearer the closest), with their count beside
 * it. Drawn every frame into the game's own HUD rows (the game redraws the boxes each frame), black-outlined. */
#define TYPE_SWEEPER   20
#define LAMP_BLINK_FAR 900       /* ms per blink with the nearest mine at the edge of the radius ... */
#define LAMP_BLINK_NEAR 160      /* ... and right next to Sarge */
static const BYTE DIGIT3x5[10][5] = {
    { 7,5,5,5,7 }, { 2,6,2,2,7 }, { 7,1,7,4,7 }, { 7,1,3,1,7 }, { 5,5,7,1,1 },
    { 7,4,7,1,7 }, { 7,4,7,5,7 }, { 7,1,1,2,2 }, { 7,5,7,5,7 }, { 7,5,7,1,7 },
};
static void put_px(Surf *back, int x, int y, BYTE c)
{
    if (x >= 0 && y >= 0 && x < back->w && y < back->h) back->bits[(size_t)y * back->pitch + x] = c;
}
static void draw_sweeper_lamp(Surf *back, BYTE *u)
{
    int count, nearest;
    if (!u || !mines_sweep_hud(&count, &nearest)) return;
    int b = carried_box(u, TYPE_SWEEPER);
    if (b < 0) return;
    RECT r = box_rect(b);
    int cx = r.left + 7, cy = r.top + 6;
    BYTE black = (BYTE)nearest_colour(0, 0, 0), fill;
    if (count > 0) {
        int d = nearest < 0 ? 96 : nearest > 96 ? 96 : nearest;
        DWORD period = LAMP_BLINK_NEAR + (DWORD)((LAMP_BLINK_FAR - LAMP_BLINK_NEAR) * d / 96);
        fill = (GetTickCount() % period) < period / 2 ? (BYTE)nearest_colour(255, 50, 30) : (BYTE)nearest_colour(90, 18, 12);
    } else fill = (BYTE)nearest_colour(40, 110, 40);
    for (int y = -4; y <= 4; y++)
        for (int x = -4; x <= 4; x++) {
            int q = x * x + y * y;
            if (q <= 6) put_px(back, cx + x, cy + y, fill);
            else if (q <= 16) put_px(back, cx + x, cy + y, black);
        }
    if (count > 0) {                              /* the count beside it, yellow on a black edge */
        char s[4]; wsprintfA(s, "%d", count > 99 ? 99 : count);
        BYTE ink = (BYTE)nearest_colour(255, 220, 40);
        int x = cx + 6;
        for (const char *p = s; *p; p++, x += 4)
            for (int row = 0; row < 5; row++)
                for (int k = 0; k < 3; k++)
                    if (DIGIT3x5[*p - '0'][row] & (4 >> k)) {
                        for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
                            int rr = row + oy, kk = k + ox;
                            if (rr < 0 || rr > 4 || kk < 0 || kk > 2 || !(DIGIT3x5[*p - '0'][rr] & (4 >> kk))) put_px(back, x + k + ox, cy - 2 + row + oy, black);
                        }
                    }
        x = cx + 6;
        for (const char *p = s; *p; p++, x += 4)
            for (int row = 0; row < 5; row++)
                for (int k = 0; k < 3; k++)
                    if (DIGIT3x5[*p - '0'][row] & (4 >> k)) put_px(back, x + k, cy - 2 + row, ink);
    }
}

/* mines.c: the record (and item) of a carried weapon of this type - in a game slot or kept in a box */
BYTE *weapons_carried(int type, void **item)
{
    BYTE *u = SARGE;
    if (!u || SARGE_HEALTH(u) <= 0) return NULL;
    for (int c = 0; c < SLOTS; c++) {
        BYTE *w = SLOT_TYPE(u, c);
        if (w && !IsBadReadPtr(w, 0x10) && *(int *)w == type) { if (item) *item = SLOT_ITEM(u, c); return w; }
    }
    if (six() && u == g_owner)
        for (int b = 0; b < BOXES; b++) {
            BYTE *w = g_store[b].type;
            if (w && !IsBadReadPtr(w, 0x10) && *(int *)w == type) { if (item) *item = g_store[b].item; return w; }
        }
    return NULL;
}

/* mines.c: would a special weapon (not carried yet) be taken if picked up now? (weapons_touch: a free box;
 * the original: an empty special slot) */
int weapons_room_for_special(void)
{
    BYTE *u = SARGE;
    if (!u || SARGE_VEHICLE(u)) return 0;
    if (!six()) return SLOT_ITEM(u, 2) == NULL;
    valid(u); sync(u);
    return first_free_box(u) >= 0;
}

/* ---------------------------------------------------------------- the medkit box
 * 2026-11-22 (user: "health kit should probably be a menu slot instead of tacked on somewhere"): the carried medkits
 * (allies.c) are a box of the weapon column like a weapon, no longer a badge beside the vehicle pips.
 *   - Picture: the game's own HUD medkit icon - the HUD weapon widget's picture table (data +0x10, the same table the
 *     game draws the heavy / special boxes from) entry HUD_PIC_MEDKIT (23; Hud_006 frame 7, the white cross with the
 *     black count box, 57x20 like every weapon icon; Wine log 2026-11-22: entry 23 sits between the explosive (12) and
 *     the minesweeper (20) pictures). Drawn with hud_draw_picture_raw like the B-column icons, so the HD HUD art
 *     (hd\hud\Hud_006_d7) replaces it the same way. The kit count goes where a weapon shows its ammo (same font,
 *     colour and offset as the game's ammo text, weapons_note_text).
 *   - Which box: the LAST free one in screen order (B3, B2, B1, A3, A2 - A1 is the rifle). Weapons fill boxes from the
 *     front (first_free_box), so the kit sits at the end of the wheel order and the two rarely meet; a box is never
 *     taken from a weapon - a pickup goes into the kit's box as into any free box and the kit moves on.
 *   - All six boxes hold weapons: a seventh SMALL box in the header strip of column B, over its "WEAPONS" label (a copy
 *     of column A's, the one place in the column that holds no weapon box; 640 wide: column A's header). There is no
 *     room for a 20-row icon there (12 rows between the plate's top edge and the first box), so the small box is a
 *     black count box like the game's own with a 9x9 cross and the count. It is the last stop of the wheel.
 *   - Selecting it (wheel stop, left click on it, H, gamepad shoulder buttons = the wheel) selects the medkit (heal mode,
 *     allies.c); choosing a weapon puts it away (activate -> allies_cancel). While selected the kit box gets the
 *     highlight frame and the weapon box loses the game's highlight (repainted from the panel art).
 *   - No kits, in a vehicle, Sarge down, classic controls or [gameplay] ally_heal = 0: no box. */
#define HUD_PIC_MEDKIT   23            /* the weapon widget picture table entry with the medkit icon */
#define KIT_NONE        (-1)           /* KIT_SMALL (-2) = the small box in the column header: defined at weapons_wheel */
#define KIT_SMALL_DX     9             /* small box: from the column's left edge (between the label screws) */
#define KIT_SMALL_Y      386           /* design y (the plate's top edge is 384, the first box frame 398) */
#define KIT_SMALL_W      50
#define KIT_SMALL_H      12

static int kit_shown(BYTE *u)
{
    return u && SARGE_HEALTH(u) > 0 && !SARGE_VEHICLE(u) && g_cfg.controls_modern && allies_kits() > 0;
}
static int kit_boxes(void) { return six() ? boxes() : SLOTS; }
static int kit_box_free(BYTE *u, int b) { return six() ? box_free(u, b) : b > 0 && b < SLOTS && !SLOT_ITEM(u, b); }
/* where the kit box is now: a box index, KIT_SMALL, or KIT_NONE */
static int kit_box(BYTE *u)
{
    if (!kit_shown(u)) return KIT_NONE;
    if (six()) { valid(u); sync(u); }
    for (int b = kit_boxes() - 1; b >= 1; b--) if (kit_box_free(u, b)) return b;
    return KIT_SMALL;
}
static RECT kit_small_rect(void)
{
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    int x = column_x(six() && have_room() ? 1 : 0) + KIT_SMALL_DX, y = KIT_SMALL_Y + dy;
    return (RECT){ x, y, x + KIT_SMALL_W, y + KIT_SMALL_H };
}
static RECT kit_box_rect(int k)
{
    if (k == KIT_SMALL) return kit_small_rect();
    if (six()) return box_rect(k);
    int dx, dy;                                   /* three boxes: the game slots, column A */
    layout_offset(ANCHOR_HUD, &dx, &dy);
    int x = column_x(0) + BOX_ICON_X - COL_SRC_X - 3, y = icon_y(k) + dy - 2;
    return (RECT){ x, y, x + BOX_W, y + BOX_H };
}
/* is (x, y) (game-screen, may be enlarged) on the kit box? */
static int kit_at(BYTE *u, int x, int y)
{
    int k = kit_box(u);
    if (k == KIT_NONE) return 0;
    RECT r = kit_box_rect(k);
    layout_unscale(&x, &y);
    POINT p = { x, y };
    return PtInRect(&r, p);
}
int weapons_kit_at(int x, int y) { BYTE *u = SARGE; return u && layout_view_is_gameplay() && kit_at(u, x, y); }
static void *kit_picture(void)
{
    BYTE *d = widget_data(2);
    void **tab = d ? *(void ***)(d + 0x10) : NULL;
    if (!tab || IsBadReadPtr(tab + HUD_PIC_MEDKIT, sizeof(void *))) return NULL;
    return tab[HUD_PIC_MEDKIT];
}
static AmmoText *kit_text(void) { return g_ammo_text[2].seen ? &g_ammo_text[2] : g_ammo_text[1].seen ? &g_ammo_text[1] : NULL; }
static void kit_count_text(Surf *back, int x, int y, int clip_w, int clip_h, int n)
{
    AmmoText *t = kit_text();
    if (!hud_draw_string_original || clip_w <= 0) return;
    char buf[8];
    wsprintfA(buf, "%d", n % 100);
    int clip[2] = { clip_w, clip_h };
    PX_GAME(hud_draw_string_original(back, x, y, buf, t ? t->font : 0, clip, t ? t->colour : AMMO_COLOUR));
}
/* the game's highlight frame on the current weapon's box goes while the medkit is selected (the A box is repainted
 * from the panel art with its weapon; six weapons: draw_columns does this itself) */
static void unhighlight_slot(Surf *back, BYTE *u)
{
    void *art;
    RECT src;
    int cur = CUR_SLOT(u);
    if (six() || cur < 0 || cur >= SLOTS || !hud_panel_art(&art, &src)) return;
    paint_box(back, art, src, cur);
    Weapon w = { SLOT_TYPE(u, cur), SLOT_ITEM(u, cur) };
    draw_icon(w, cur);
    draw_ammo(back, w, cur);
}
static void draw_kit_box(Surf *back, BYTE *u)
{
    int k = kit_box(u);
    if (k == KIT_NONE) return;
    int heal = allies_heal_mode(), n = allies_kits();
    if (heal) unhighlight_slot(back, u);
    BYTE hi = (BYTE)nearest_colour(40, 240, 40);
    RECT r = kit_box_rect(k);
    if (r.left < 0 || r.top < 0 || r.right > back->w || r.bottom > back->h) return;
    if (k == KIT_SMALL) {
        BYTE black = (BYTE)nearest_colour(0, 0, 0), edge = (BYTE)nearest_colour(20, 28, 18);
        for (int y = r.top; y < r.bottom; y++)
            for (int x = r.left; x < r.right; x++) put_px(back, x, y, black);
        frame_box(back, r, heal ? hi : edge);
        int x0 = r.left + 3, y0 = r.top + (KIT_SMALL_H - 9) / 2;
        BYTE plate = (BYTE)nearest_colour(235, 235, 235), cross = (BYTE)nearest_colour(225, 25, 25);
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 9; x++) {
                if ((x == 0 || x == 8) && (y == 0 || y == 8)) continue;        /* rounded plate corners */
                int c = (x >= 3 && x <= 5 && y >= 1 && y <= 7) || (y >= 3 && y <= 5 && x >= 1 && x <= 7);
                put_px(back, x0 + x, y0 + y, c ? cross : plate);
            }
        kit_count_text(back, x0 + 14, r.top + 2, r.right - 2 - (x0 + 14), BOX_H, n);   /* clip height as the ammo text: the font cell is taller than the box (12 rows clipped every digit) */
        return;
    }
    int dx, dy;
    layout_offset(ANCHOR_HUD, &dx, &dy);
    int col = six() ? box_col(k) : 0, row = six() ? box_row(k) : k;
    int ix = column_x(col) + BOX_ICON_X - COL_SRC_X, iy = icon_y(row) + dy;
    void *pic = kit_picture();
    if (pic) hud_draw_picture_raw(pic, ix, iy);
    AmmoText *t = kit_text();
    int tx = ix + (t ? t->dx : AMMO_TEXT_DX), ty = iy + (t ? t->dy : AMMO_TEXT_DY);
    kit_count_text(back, tx, ty, column_x(col) + COL_SRC_W - tx, BOX_H, n);
    if (heal) frame_box(back, r, hi);
}

/* Called with the finished game picture (before it is shown). */
void weapons_draw_markers(Surf *back)
{
    BYTE *u = SARGE;
    draw_vehicle_unlimited(back);                 /* also with classic controls / five-weapon HUD */
    if (!back || !HUD_WIDGETS || !layout_view_is_gameplay()) return;
    int alive = u && SARGE_HEALTH(u) > 0;
    if (six()) {
        if (alive) { valid(u); sync(u); }
        /* the column stays on screen while Sarge is dead or respawning (empty: the boxes are cleared) */
        draw_columns(back, alive ? u : NULL);
        if (alive) draw_sweeper_lamp(back, u);
    }
    if (alive) draw_kit_box(back, u);
}

/* Keys 1-6 (modern controls): use box b directly, screen order A1 A2 A3 B1 B2 B3 (b = key - 1). */
void weapons_select_box(int b)
{
    BYTE *u = SARGE;
    if (!u || SARGE_VEHICLE(u) || (INPUT & INPUT_FIRE) || b < 0 || b >= boxes()) return;
    valid(u);
    sync(u);
    if (box_weapon(u, b).type && activate(u, b) && g_cfg.log >= 2)
        mlog("weapons: key %d -> box %c%d", b + 1, box_col(b) ? 'B' : 'A', box_row(b) + 1);
}

/* savestate.c: the boxes are part of the mission state (saved with a quicksave, put back on a load) */
void weapons_savestate(void) { SAVESTATE(g_store); SAVESTATE(g_slot_box); SAVESTATE(g_owner); SAVESTATE(g_seen); SAVESTATE(g_cur_seen); SAVESTATE(g_prev); }

/* ---------------------------------------------------------------- test aids ([debug] cheat_keys; controls.c keys)
 * Ctrl+W pick up a weapon item, Ctrl+Shift+W a mine / explosive pack, Ctrl+V win the part (dump first), Ctrl+U Sarge dies.
 * weapons_debug_dump logs Sarge's slots and the boxes (record, type, ammo, charges, item number / UID / flags). */
#define OBJECTS_D       (*(BYTE **)0x4e30ec)
#define OBJECT_COUNT_D  (*(int *)0x4e30e8)
#define OBJECT_SIZE_D   0xd0
static int obj_idx_d(void *it) { BYTE *o = OBJECTS_D; return o && (BYTE *)it >= o && (BYTE *)it < o + (size_t)OBJECT_COUNT_D * OBJECT_SIZE_D ? (int)(((BYTE *)it - o) / OBJECT_SIZE_D) : -1; }
static void dump_w(const char *tag, void *rec, void *item)
{
    if (!rec) { mlog("weapons:   %s empty (item %p)", tag, item); return; }
    if (IsBadReadPtr(rec, 0x20)) { mlog("weapons:   %s rec %p UNREADABLE item %p", tag, rec, item); return; }
    int idx = obj_idx_d(item);
    int ty = *(int *)rec;
    mlog("weapons:   %s rec %p type %d ammo %d/%d free charges %d item %p idx %d uid %08lx f70 %02x f71 %02x", tag, rec, ty, W_AMMO(rec), W_MAX_AMMO(rec), ty == TYPE_MINE || ty == TYPE_EXPLOSIVE ? mines_free_charges(rec) : -1,
         item, idx, idx >= 0 ? *(DWORD *)item : 0, idx >= 0 ? ((BYTE *)item)[0x70] : 0, idx >= 0 ? ((BYTE *)item)[0x71] : 0);
}
void weapons_debug_dump(const char *why)
{
    BYTE *u = SARGE;
    int s, m; unsigned r = gameplay_run(&s, &m);
    mlog("weapons: DUMP %s: run %u scen %d mission %d sarge %p hp %d veh %08x cur %d owner %p objs %p/%d", why, r, s, m, u, u ? SARGE_HEALTH(u) : 0,
         u ? SARGE_VEHICLE(u) : 0, u ? CUR_SLOT(u) : 0, g_owner, OBJECTS_D, OBJECT_COUNT_D);
    if (!u) return;
    for (int c = 0; c < SLOTS; c++) { char t[24]; wsprintfA(t, "slot%d(box %d)", c, g_slot_box[c]); dump_w(t, SLOT_TYPE(u, c), SLOT_ITEM(u, c)); }
    for (int b = 0; b < BOXES; b++) if (g_store[b].type) { char t[16]; wsprintfA(t, "store%d", b); dump_w(t, g_store[b].type, g_store[b].item); }
}
/* Ctrl+W (cheat_keys): pick up the nearest weapon item lying on the map whose type is not carried yet (none: any
 * type), through the game's own pickup (0x4495c0, wrapped by weapons_touch) - a test aid for the six boxes */
void weapons_debug_pickup(int charges_only)
{
    BYTE *u = SARGE, *best = NULL;
    if (!u || !OBJECTS_D) return;
    static int census;
    if (!census++) {
        BYTE *o = OBJECTS_D;
        for (int i = 0; i < OBJECT_COUNT_D; i++, o += OBJECT_SIZE_D) {
            BYTE *w = *(BYTE **)(o + 0x20);
            if (*(short *)(o + 0x68) != 0x19 || !w || IsBadReadPtr(w, 0x20)) continue;
            mlog("cheat: map item idx %d type %d ammo %d/%d at %d,%d flags %02x", i, *(int *)w, W_AMMO(w), W_MAX_AMMO(w), *(short *)(o + 4), *(short *)(o + 6), o[0x70]);
        }
    }
    for (int pass = 0; pass < 2 && !best; pass++) {
        long bd = 0x7fffffff;
        BYTE *o = OBJECTS_D;
        for (int i = 0; i < OBJECT_COUNT_D; i++, o += OBJECT_SIZE_D) {
            if (*(short *)(o + 0x68) != 0x19 || (o[0x70] & 0x24)) continue;
            BYTE *w = *(BYTE **)(o + 0x20);
            if (!w || IsBadReadPtr(w, 0x20)) continue;
            int t = *(int *)w;
            if (t < 1 || t > 0x14 || (t >= 0xe && t <= 0x13) || (pass == 0 && weapons_carried(t, NULL))) continue;
            if (charges_only && t != TYPE_MINE && t != TYPE_EXPLOSIVE) continue;
            if (charges_only && pass == 0) continue;
            long dx = *(short *)(o + 4) - *(short *)(u + 4), dy = *(short *)(o + 6) - *(short *)(u + 6), d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = o; }
        }
    }
    if (!best) { mlog("cheat: no weapon item on the map"); return; }
    BYTE *w = *(BYTE **)(best + 0x20);
    mlog("cheat: pick up item idx %d type %d ammo %d at %d,%d", obj_idx_d(best), *(int *)w, W_AMMO(w), *(short *)(best + 4), *(short *)(best + 6));
    ((TouchFn)0x4495c0)(u, best, 0);
    weapons_debug_dump("after cheat pickup");
}
/* Ctrl+U (cheat_keys): Sarge dies (invulnerability off, a lethal bullet through ItemDamage 0x431f60) - Play Again tests */
void weapons_debug_die(void)
{
    BYTE *u = SARGE;
    if (!u) return;
    *(volatile BYTE *)0x69752c = 0;
    short from[2] = { *(short *)(u + 4), *(short *)(u + 6) };
    mlog("cheat: Sarge dies");
    ((void (__fastcall *)(void *, int, int, short *, DWORD))0x431f60)(u, 1000, 6, from, 0);
}
