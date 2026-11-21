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
 *     the game's drop routine (an item dropped with no ammo is destroyed). Not for mines and
 *     explosives: their placed charges are kept in an array sized when the weapon was made.
 *   - mouse wheel (modern controls): steps through the boxes holding a weapon in screen order.
 *   - left click on a box: use that weapon; right click: drop it (not the rifle).
 *   - after a drop, another carried weapon of that kind moves into the slot (it keeps its box).
 * Boxes are forgotten when Sarge dies or a new mission starts (a new Sarge object).
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
static int safe_item(void *it) { return it && !IsBadReadPtr(it, 0x40) && !IsBadReadPtr(*(void **)((BYTE *)it + 0x20) ? *(void **)((BYTE *)it + 0x20) : it, 4); }   /* the item and its weapon record readable */
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
static int valid(BYTE *u)
{
    /* 2026-11-20 crash (user log: EXCEPTION in 0x4492b0 called from here at a mission start): the new mission's
     * Sarge can sit at the SAME address - the boxes of the last mission (freed items) were kept. Also forgotten
     * on a new mission / scenario number now. */
    static int mission = -1;
    BYTE *g = *(BYTE **)0x4b3f70;
    int m = g && !IsBadReadPtr(g, 0x80) ? (*(int *)(g + 0x74) << 16) ^ *(int *)(g + 0x70) : -1;
    if (!u || u != g_owner || SARGE_HEALTH(u) <= 0 || m != mission) { forget(); g_owner = u; mission = m; return 0; }
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
    weapons_touch_original(u, item, arg);
    if (SLOT_ITEM(u, c) != item) {               /* not picked up after all */
        SLOT_TYPE(u, c) = held.type; SLOT_ITEM(u, c) = held.item;
        return;
    }
    Weapon dup = slot_of_box(dup_box) == c ? held : g_store[dup_box];
    BYTE *rec = ITEM_WEAPON(item);
    int total = W_AMMO(dup.type) + W_AMMO(rec);
    if (total > MAX_AMMO) total = MAX_AMMO;
    W_AMMO(dup.type) = (short)total;
    if (W_MAX_AMMO(dup.type) < total) W_MAX_AMMO(dup.type) = (short)total;
    W_AMMO(rec) = 0;
    DROP_SLOT(u, c, *(DWORD *)(u + 4));          /* empty: the game destroys it */
    SLOT_TYPE(u, c) = held.type; SLOT_ITEM(u, c) = held.item;
    if (cur >= 0 && cur < SLOTS && CUR_SLOT(u) != cur) { CUR_SLOT(u) = -1; SELECT_SLOT(u, cur); }
    if (g_cfg.log >= 2) mlog("weapons: type %d ammo added, now %d", ITEM_TYPE(item), total);
    remember(u);
}

void __fastcall weapons_touch(BYTE *unit, void *item, int arg)
{
    BYTE *u = SARGE;
    if (!six() || unit != u || !item || SARGE_VEHICLE(u)) { weapons_touch_original(unit, item, arg); return; }
    valid(u);
    sync(u);
    int c = ITEM_CATEGORY(item), type = ITEM_TYPE(item);
    if (c == 0) { weapons_touch_original(unit, item, arg); remember(u); return; }   /* auto rifle upgrade, ammo, health ... */

    int dup = carried_box(u, type);
    if (dup >= 0) {                              /* the same weapon again: take its ammo */
        BYTE *rec = ITEM_WEAPON(item);
        Weapon d = box_weapon(u, dup);
        if (type == TYPE_MINE || type == TYPE_EXPLOSIVE || W_AMMO(rec) <= 0 || W_AMMO(d.type) <= 0
            || W_AMMO(d.type) >= MAX_AMMO) return;
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
    weapons_touch_original(unit, item, arg);
    if (SLOT_ITEM(u, c) != item) {               /* not picked up: put things back */
        if (stashed) { SLOT_TYPE(u, c) = before.type; SLOT_ITEM(u, c) = before.item; g_store[old_box] = (Weapon){ 0 }; }
        return;
    }
    g_slot_box[c] = b;
    if (stashed && cur == c) activate(u, old_box);   /* keep using the weapon that was in use */
    if (g_cfg.log >= 2) mlog("weapons: picked up type %d (kind %d) into box %c%d", type, c, box_col(b) ? 'B' : 'A', box_row(b) + 1);
    remember(u);
}

/* ---------------------------------------------------------------- per frame */

/* After a drop (the slot's item is gone), another carried weapon of that kind moves into the
 * slot - it keeps its box. */
static void track_current(BYTE *u);
void weapons_frame(void)
{
    BYTE *u = SARGE;
    if (u && !SARGE_VEHICLE(u)) track_current(u);
    if (!six() || !valid(u)) { memset(g_seen, 0, sizeof g_seen); return; }
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
        SELECT_SLOT(u, g_prev);
    }
    track_current(u);
    if (g_cfg.log >= 2) mlog("weapons: quick swap -> %d", g_cur_seen);
}

/* Steps through the boxes holding a weapon in screen order A1 A2 A3 B1 B2 B3; notches > 0 = wheel up. */
void weapons_wheel(int notches)
{
    BYTE *u = SARGE;
    if (!notches || !u || SARGE_VEHICLE(u) || (INPUT & INPUT_FIRE)) return;
    if (!six()) return;
    valid(u);
    sync(u);
    int n = boxes(), pos = current_box(u), step = notches > 0 ? -1 : 1;
    for (int i = 1; i < n; i++) {
        int b = ((pos + step * i) % n + n) % n;
        if (box_weapon(u, b).type && activate(u, b)) {
            if (g_cfg.log >= 2) mlog("weapons: wheel -> box %c%d", box_col(b) ? 'B' : 'A', box_row(b) + 1);
            return;
        }
    }
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
    hud_draw_string_original(back, x, y, buf, t->font, clip, t->colour);
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
    int keep = cur >= 0 && cur < SLOTS && cur_box == cur;      /* the game's highlight is right */
    RECT kr = keep ? box_rect(cur) : (RECT){ 0, 0, 0, 0 };
    static BYTE saved[BOX_H][BOX_W];
    if (keep && (kr.left < 0 || kr.top < 0 || kr.right > back->w || kr.bottom > back->h)) keep = 0;
    if (keep) for (int y = 0; y < BOX_H; y++) memcpy(saved[y], back->bits + (size_t)(kr.top + y) * back->pitch + kr.left, BOX_W);

    if (room) ((BltFastFn)back->vtbl[7])(back, bx, by, art, &s, DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY);
    int repaint[SLOTS] = { 0 };
    for (int k = 1; k < SLOTS; k++)
        if (g_slot_box[k] != k) { repaint[k] = 1; paint_box(back, art, src, k); }
    if (keep) for (int y = 0; y < BOX_H; y++) memcpy(back->bits + (size_t)(kr.top + y) * back->pitch + kr.left, saved[y], BOX_W);

    for (int b = 0; b < boxes(); b++) {
        if (box_col(b) == 0 && !repaint[box_row(b)]) continue;   /* the game drew it right */
        Weapon w = box_weapon(u, b);
        draw_icon(w, b);
        draw_ammo(back, w, b);
    }
    if (!keep && cur_box > 0) frame_box(back, box_rect(cur_box), (BYTE)nearest_colour(40, 240, 40));
}

/* Called with the finished game picture (before it is shown). */
void weapons_draw_markers(Surf *back)
{
    BYTE *u = SARGE;
    draw_vehicle_unlimited(back);                 /* also with classic controls / five-weapon HUD */
    if (!six() || !back || !HUD_WIDGETS || !layout_view_is_gameplay()) return;
    int alive = u && SARGE_HEALTH(u) > 0;
    if (alive) { valid(u); sync(u); }
    /* the column stays on screen while Sarge is dead or respawning (empty: the boxes are cleared) */
    draw_columns(back, alive ? u : NULL);
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
