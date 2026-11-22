/* ============================================================================================
 * roofs.c - see Sarge through buildings and other tall objects (x-ray ghost)
 *
 * World sprites are drawn onto the map surface by DrawAnimInRect (FUN_0042eef0, map path; source
 * game binary: __fastcall ecx = ANIMCELL*, edx = clip RECT* in world coordinates, ret), sorted by world
 * y: whatever is drawn AFTER Sarge and overlaps him hides him (a roof drawn above its base, a sign
 * or a wall in front of him). The map surface is only partly redrawn each frame, so it cannot be
 * edited in place; instead:
 *   1. the map-path wrapper notes Sarge's cell and whether a structure / asset / obstacle cell drawn
 *      after it overlaps it;
 *   2. after the battlefield is in the back buffer (layout.c, before the map-bottom shift), Sarge's
 *      cell is drawn ONCE MORE on top with the game's back-buffer sprite routine (FUN_0042f120,
 *      same arguments; it draws into the "locked" surface *(BYTE **)0x4e2444, pitch
 *      *(int *)0x4e2880, at world - screenrect), and every other pixel (checkerboard) of what was
 *      there is put back: Sarge shows as a 50% ghost where he is covered, and is unchanged where
 *      he was visible anyway.
 * ANIMCELL: +0x0c owner ITEM* (uid at +0: raw type (uid >> 22) & 0xf: 6 Sarge, 7 asset,
 * 8 structure, 9 obstacle; army ((uid >> 26) & 7) - 1), +0x1c wrect (4 ints, world).
 * [display] see_through = 1
 * ============================================================================================ */
#include "common.h"
#include <limits.h>

#define LOCKED_BITS   (*(BYTE **)0x4e2444)
#define LOCKED_PITCH  (*(int *)0x4e2880)
#define SCREEN_L      (*(int *)0x4e2fd4)
#define SCREEN_T      (*(int *)0x4e2fd8)
#define SCREEN_R      (*(int *)0x4e2fdc)
#define SCREEN_B      (*(int *)0x4e2fe0)
#define CELL_OWNER(c) (*(BYTE **)((c) + 0x0c))
#define CELL_WRECT(c) ((int *)((c) + 0x1c))
#define RAW_TYPE(uid) ((int)(((uid) >> 22) & 0xf))
#define UID_ARMY(uid) ((int)(((uid) >> 26) & 7) - 1)
#define DRAW_CELL_BACK ((DrawCellFn)0x42f120)    /* DrawAnimInRect into the back buffer */
#define RAW_VEHICLE   3                          /* raw item type of vehicles (Boot Camp tank uid type 3) */
#define TURRET_IN_HULL 0xe4                      /* the turret animation lies at hull animation + 0xe4 (owner NULL) */

#define MAX_W 160
#define MAX_H 160

DrawCellFn roofs_draw_cell_original;

#define MAX_COVER 48
/* Figures drawn again after the map pass (2026-11-19: enemies too - user: "if there are enemies in building
 * shooting - I want them to be visible like sarge is inside building"). Slot 0 = Sarge (also redrawn over low
 * cover), the others = enemy soldiers (raw type 5, army != player) covered by a TALL cell (building, tree,
 * sign): shown as the same 50% ghost there. Only while he fires (and SHOW_AFTER_FIRE_MS after; user: "enemies in
 * buildings should only show *after* they start firing"). A slot lives until its cell is inactive, its owner dead or gone,
 * or the slot is taken by a newer figure (oldest first). */
#define MAX_FIGS 12
#define SHOW_AFTER_FIRE_MS 5000          /* a covered enemy shows while firing and this long after his last shot */
#define ENEMY_FRESH_MS 1500              /* an enemy slot not redrawn for this long is dropped (freed cells) */
#define GHOST_EVERY 4                    /* under tall cover: 1 inside pixel in 4 of the figure kept, outline whole */
/* rect / cover rects are COPIES taken when drawn (2026-11-20 crash: Sarge died, the game freed his cell, and the
 * next cell drawn compared itself with the freed cell's rectangle - DDRAW.dll+0x2c70e in roofs_draw_cell) */
typedef struct { BYTE *cell, *owner; int rect[4]; int covered, ncover, ntall; int cover[MAX_COVER][4]; BYTE cover_low[MAX_COVER]; DWORD seen, shown_at; } Fig;
static Fig g_figs[MAX_FIGS];

static Fig *fig_slot(BYTE *cell, BYTE *owner, int sarge)
{
    if (sarge) return &g_figs[0];
    Fig *old = &g_figs[1];
    for (int i = 1; i < MAX_FIGS; i++) {
        if (g_figs[i].cell == cell || !g_figs[i].cell) return &g_figs[i];
        if (g_figs[i].seen < old->seen) old = &g_figs[i];
    }
    (void)owner;
    return old;
}

/* Low objects: Sarge is drawn fully on top of them (user: "on top of rocks but behind trees").
 * Object category (OBJECTSTRUCT +0x68, binary): ROCK 0, BUSH 1, FALLEN TREE 4, BRIDGE 7, FOOT BRIDGE 8.
 * Bridges (2026-11-18, user: "enemies are going under the bridge"): a long diagonal bridge reaches far down the
 * screen, so in the GPU painter depth (sorted by the bottom row) soldiers walking ON it sorted behind it. Nothing
 * walks under a bridge in this game (OVERPASS 24 stays tall).
 * Pick-up crates (SPECIAL ITEMS 25, 2026-11-19, user: "if the death animation plays overtop of the crate, it has
 * some graphical issues"): the game sorts by the item position, the GPU by the sprite's bottom row - a soldier the
 * game drew OVER a crate could sort behind it, and the crate's image then replaced his pixels up to 4 px from
 * every visible crate pixel. Figures are always on top of a crate now. */
static int is_low_owner(const BYTE *owner)
{
    int type = (int)((*(const DWORD *)owner >> 22) & 0xf);
    if (type < 7 || type > 9) return 0;
    short cat = *(const short *)(owner + 0x68);
    return cat == 0 || cat == 1 || cat == 4 || cat == 7 || cat == 8 || cat == 25;
}

#define CELL_BYTES  0x30                 /* flags +0, owner +0x0c, world rect +0x1c..+0x2c */
#define OWNER_BYTES 0xa0                 /* item fields the mod reads (uid +0, category +0x68, vehicle hull +0x9c) */
static void cells_run_check(void);
/* each battlefield pass: a figure whose cell is gone, inactive or reused is dropped before anything reads it */
void roofs_new_frame(void)
{
    cells_run_check();
    for (int i = 0; i < MAX_FIGS; i++) {
        Fig *f = &g_figs[i];
        if (!f->cell) continue;
        if (IsBadReadPtr(f->cell, CELL_BYTES) || !(*(DWORD *)f->cell & 1) || CELL_OWNER(f->cell) != f->owner
            || (f->owner && IsBadReadPtr(f->owner, OWNER_BYTES))) f->cell = NULL;
    }
}

static int rects_overlap(const int *a, const int *b)
{
    return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}


/* ---------------------------------------------------------------- what is drawn where (controls.c)
 * Every world sprite cell drawn onto the map is remembered (the map is only partly redrawn, so a
 * standing figure may not be drawn again for a long time): controls.c asks which cells' world
 * rectangles hold a point - this finds things that are neither army soldiers nor objects in the
 * game's point-test layers, such as the Boot Camp shooting-range figures. */
#define CELLS 4096
static BYTE *g_cells[CELLS];
static int g_cells_n;

static void remember_cell(BYTE *cell)
{
    unsigned h = (unsigned)((uintptr_t)cell >> 4) * 2654435761u % CELLS;
    for (int i = 0; i < 16; i++) {
        unsigned k = (h + i) % CELLS;
        if (g_cells[k] == cell) return;
        if (!g_cells[k]) { g_cells[k] = cell; g_cells_n++; return; }
    }
    g_cells[h] = cell;                                 /* full around here: replace */
}

void roofs_forget_cells(void) { memset(g_cells, 0, sizeof g_cells); g_cells_n = 0; }

/* A new mission RUN (gameplay_run: new scenario / MissionID, loading -> play, clock back) drops every remembered cell
 * and figure slot: they belong to the previous map. (2026-11-22 crash, mission 5 won -> mission 6: the route line's
 * "under the sprites" test (modern.c under_sprite) handed a cell of the mission-5 map to ObjectIntersectPoint
 * 0x42faf0; its sprite record (+4) had been freed and reused - mask pointer / width garbage - and the mask read at
 * 0x42fad9 faulted. The registry was never cleared, and IsBadReadPtr cannot tell a freed block of the game heap
 * (our savestate arena keeps freed memory readable) from a live one.) */
static unsigned g_cells_run;
static void cells_run_check(void)
{
    unsigned r = gameplay_run(NULL, NULL);
    if (r == g_cells_run) return;
    if (g_cells_run && g_cfg.log >= 1) mlog("roofs: mission run %u - %d remembered cells of the last run forgotten", r, g_cells_n);
    g_cells_run = r;
    roofs_forget_cells();
    memset(g_figs, 0, sizeof g_figs);
}

/* A remembered cell that can still be used: its memory is readable, it is active, and its owner (if any) is
 * readable too. The game frees cells (an object destroyed, a soldier gone) without telling us; a stale entry is
 * dropped here. (2026-11-19: the health bars on targets walk the list every frame and read a freed cell -
 * access violation in roofs_cells_list on the user's PC; before, only hovering read them.) */
static BYTE *live_cell(int k)
{
    BYTE *c = g_cells[k];
    if (!c) return NULL;
    if (IsBadReadPtr(c, CELL_BYTES)) { g_cells[k] = NULL; g_cells_n--; return NULL; }
    if (!(*(DWORD *)c & 1)) return NULL;                 /* not active (any more) */
    BYTE *owner = CELL_OWNER(c);
    if (owner && IsBadReadPtr(owner, OWNER_BYTES)) { g_cells[k] = NULL; g_cells_n--; return NULL; }
    const int *w = CELL_WRECT(c);
    if (w[0] > w[2] || w[1] > w[3]) return NULL;         /* reused memory: not a rectangle */
    return c;
}

/* Up to max active cells whose world rectangle holds (wx, wy); returns how many. */
int roofs_cells_at(int wx, int wy, BYTE **out, int max)
{
    cells_run_check();
    int n = 0;
    for (int k = 0; k < CELLS && n < max; k++) {
        BYTE *c = live_cell(k);
        if (!c) continue;
        const int *w = CELL_WRECT(c);
        if (wx >= w[0] && wx < w[2] && wy >= w[1] && wy < w[3]) out[n++] = c;
    }
    return n;
}

/* ---------------------------------------------------------------- building fronts over figures in front of them
 * 2026-11-21 (user, mission 7 bank: "there are soldiers walking out of the bank - but they appear under the
 * rubble"). The game draws a map region in three passes (FUN_0042e3e0 -> FUN_0042e950 per layer list):
 *   layer 0 (0x4e3568, sorted by the +0x3c part number): building parts drawn UNDER figures (the bank's walls,
 *                                                         doorway and rubble pile: world.pak entry 11657),
 *   layer 1 (0x4e357c, sorted by world_pos.y):          everything that moves or stands - soldiers, vehicles,
 *   layer 2 (0x4e3590, sorted by +0x3c):                 building parts drawn OVER figures (the bank's roof with
 *                                                         the wall tops, the BANK sign block and the broken roof
 *                                                         edge round the hole: entry 17067) - so a roof hides who
 *                                                         is inside.
 * Layer 2 is drawn after every figure in the region, whatever their positions: a soldier standing IN FRONT of
 * the building (south of it) is drawn under the roof part wherever its picture and his overlap - the soldiers
 * walking out of the bank's hole had their heads / rifles under the broken roof edge and the sign block (Wine
 * log 2026-11-21: bank part 1 pos y 2494 layer 0, Sarge y 2705..2810 layer 1, bank part 2 pos y 2580 layer 2,
 * every region redraw in that order; the original 8-bit picture shows the same).
 * Now: after the layer-2 pass, each figure of this pass whose feet (world_pos) lie BELOW the lowest opaque
 * pixel of every layer-2 part it overlaps (checked in the part's own picture, in the columns round his feet) is
 * drawn once more with the game's own routine and the same clip - on top of it, as the y order says. A figure
 * inside the building (a roof pixel below his feet in those columns) stays hidden under it. His shadow is not
 * drawn again (it would darken the roof part). The game's data decides: layer, positions, the part's RLE picture.
 * [display] fronts_order = 1 (0 = the game's order). */
#define LAYER_LIST_0  0x4e3568                   /* FUN_0042e950's edx per pass (FUN_0042e3e0) */
#define LAYER_LIST_1  0x4e357c
#define LAYER_LIST_2  0x4e3590
#define DRAW_LAYER    ((LayerFn)0x42e950)        /* __fastcall ecx = region rect (world), edx = layer list */
#define FRAME_OF(c)   (*(BYTE **)((c) + 4))      /* ANIMCELL +4: the frame drawn (+0 RLE data, +0x3c format) */
#define FEET_COLS     4                          /* columns either side of the feet checked in the front part */
#define MAX_LAYER_FIGS 64
#define MAX_FRONTS    16
typedef void (__fastcall *LayerFn)(void *rect, void *list);
typedef struct { BYTE *cell; int rect[4]; } LayerFig;
typedef struct { BYTE *cell; int rect[4]; const BYTE *data; int dword; } LayerFront;
static int g_layer = -1;                         /* pass being drawn (FUN_0042e3e0), -1 = another path */
static LayerFig g_lfig[MAX_LAYER_FIGS]; static int g_nlfig;
static LayerFront g_lfront[MAX_FRONTS]; static int g_nlfront;
static int g_layer_redraw;                       /* drawing a figure again: not noted */
static void roofs_draw_cell_body(BYTE *cell, void *clip);

/* figures: soldiers (raw 5), Sarge (6), vehicles (3); owner-less cells only as a turret on its hull */
static int is_figure(const BYTE *cell, const BYTE *owner)
{
    if (!owner) {
        const BYTE *hull = cell - TURRET_IN_HULL, *vo;
        if (IsBadReadPtr(hull, 0x10)) return 0;
        vo = *(BYTE *const *)(hull + 0x0c);
        return vo && !IsBadReadPtr(vo, 0xa0) && RAW_TYPE(*(const DWORD *)vo) == RAW_VEHICLE && *(BYTE *const *)(vo + 0x9c) == hull;
    }
    int t = RAW_TYPE(*(const DWORD *)owner);
    return t == 3 || t == 5 || t == 6;
}

static void layer_note(BYTE *cell, void *clip, BYTE *owner)
{
    (void)clip;
    if (g_layer_redraw || g_layer < 1) return;
    if (g_layer == 1) {
        if (g_nlfig < MAX_LAYER_FIGS && is_figure(cell, owner)) {
            g_lfig[g_nlfig].cell = cell; memcpy(g_lfig[g_nlfig].rect, CELL_WRECT(cell), sizeof g_lfig[0].rect); g_nlfig++;
        }
        return;
    }
    /* layer 2: a front part; its picture = the frame the game just drew (formats 0 DRLE = dword rows, 1 / 2 WRLE) */
    if (g_nlfront >= MAX_FRONTS) return;
    BYTE *fr = FRAME_OF(cell);
    if (!fr || IsBadReadPtr(fr, 0x40)) return;
    int fmt = (char)fr[0x3c];
    const BYTE *d = *(const BYTE **)fr;
    if ((fmt != 0 && fmt != 1 && fmt != 2) || !d || IsBadReadPtr(d, 4)) return;
    LayerFront *f = &g_lfront[g_nlfront++];
    f->cell = cell; memcpy(f->rect, CELL_WRECT(cell), sizeof f->rect); f->data = d; f->dword = fmt == 0;
}

/* Does row py of the RLE picture have an opaque pixel in columns [a, b]? Rows: [skip, run, pixels...] until the
 * width (as hdworld.c rle_size). */
static int rle_row_opaque(const BYTE *d, int dword, int py, int a, int b)
{
    int w = *(const uint16_t *)d;
    if (a < 0) a = 0;
    if (b >= w) b = w - 1;
    if (a > b) return 0;
    size_t off = dword ? ((const uint32_t *)(d + 4))[py] : ((const uint16_t *)(d + 4))[py];
    const BYTE *p = d + off;
    for (int x = 0, guard = 0; x < w && guard < 1024; guard++) {
        if (IsBadReadPtr(p, 2)) return 0;
        x += *p++;                                     /* transparent run */
        if (x > b || x >= w) return 0;
        int n = *p++;
        if (n && x + n - 1 >= a) return 1;            /* the run [x, x + n) meets [a, b] (x <= b here) */
        p += n; x += n;
    }
    return 0;
}

/* The lowest opaque row (world y) of the front part in the columns round wx, INT_MIN where it has none there,
 * INT_MAX when its picture is not the one expected (then the game's order is kept). */
static int front_bottom(const LayerFront *f, int wx)
{
    const BYTE *d = f->data;
    int w = *(const uint16_t *)d, h = *(const uint16_t *)(d + 2);
    if (w != f->rect[2] - f->rect[0] || h != f->rect[3] - f->rect[1]) return INT_MAX;
    if (IsBadReadPtr(d, 4 + (size_t)h * (f->dword ? 4 : 2))) return INT_MAX;
    int a = wx - FEET_COLS - f->rect[0], b = wx + FEET_COLS - f->rect[0];
    for (int y = h - 1; y >= 0; y--)
        if (rle_row_opaque(d, f->dword, y, a, b)) return f->rect[1] + y;
    return INT_MIN;
}

/* After the layer-2 pass: figures in front of every front part they overlap are drawn again on top. */
static void layer_fronts_done(void *rect)
{
    if (!g_nlfront || !g_nlfig || !g_cfg.fronts_order) return;
    for (int i = 0; i < g_nlfig; i++) {
        BYTE *c = g_lfig[i].cell;
        if (IsBadReadPtr(c, CELL_BYTES) || !(*(DWORD *)c & 1)) continue;
        int fx = *(short *)(c + 0x10), fy = *(short *)(c + 0x12), over = 0, behind = 0;
        for (int k = 0; k < g_nlfront && !behind; k++) {
            if (!rects_overlap(g_lfig[i].rect, g_lfront[k].rect)) continue;
            int b = front_bottom(&g_lfront[k], fx);
            if (b == INT_MIN) continue;                /* no facade pixel in his columns: nothing of it below his feet */
            if (fy > b) over = 1; else behind = 1;
        }
        if (!over || behind) continue;
        g_layer_redraw = 1;
        hdworld_skip_shadows(1);
        roofs_draw_cell_body(c, rect);
        hdworld_skip_shadows(0);
        g_layer_redraw = 0;
        if (g_cfg.log >= 2) { static int n; if (n++ < 20) mlog("roofs: figure %p at %d,%d drawn again over a building front (layer 2)", c, fx, fy); }
    }
}

void __fastcall roofs_layer_0(void *rect, void *list) { g_layer = 0; DRAW_LAYER(rect, list); g_layer = -1; }
void __fastcall roofs_layer_1(void *rect, void *list) { g_nlfig = 0; g_layer = 1; DRAW_LAYER(rect, list); g_layer = -1; }
void __fastcall roofs_layer_2(void *rect, void *list)
{
    g_nlfront = 0; g_layer = 2; DRAW_LAYER(rect, list); g_layer = -1;
    static int faults;
    if (faults < 3) {
        Guard g;
        if (GUARD_TRY(g, "building fronts (roofs.c)")) { layer_fronts_done(rect); guard_leave(&g); }
        else { g_layer_redraw = 0; hdworld_skip_shadows(0); faults++; }
    }
    g_nlfig = g_nlfront = 0;
}

static void roofs_draw_cell_body(BYTE *cell, void *clip)
{
    BYTE *owner = cell ? CELL_OWNER(cell) : NULL;
    int low = owner && is_low_owner(owner);
    /* the turret: an owner-less cell at hull animation + 0xe4 (vehicle +0x9c = hull; seen in Wine 2026-11-17) -
     * its upscaled image sorts above the hull (user: "tank needs to draw the turret on top of the base") */
    int top = 0;
    if (cell && !owner) {
        BYTE *hull = cell - TURRET_IN_HULL, *vo = *(BYTE **)(hull + 0x0c);
        top = vo && !IsBadReadPtr(vo, 0xa0) && RAW_TYPE(*(DWORD *)vo) == RAW_VEHICLE && *(BYTE **)(vo + 0x9c) == hull;
    }
    hdworld_cell_low(low);                       /* its upscaled images sort behind standing figures */
    hdworld_cell_top(top);
    hdworld_cell_sort(cell ? *(short *)(cell + 0x12) - CELL_WRECT(cell)[1] : 0);   /* the game's sort key (world_pos.y) */
    hdworld_cell_begin(cell);
    { PROF_BEGIN(tb); roofs_draw_cell_original(cell, clip); PROF_END(tb, PR_BLIT); }   /* the game's own sprite draw */
    hdworld_cell_low(0);
    hdworld_cell_top(0);
    hdworld_cell_sort(0);
    hdworld_cell_begin(NULL);
    if (top && g_cfg.log >= 2) { static int n; if (n++ < 6) mlog("roofs: turret cell %p (hull %p)", cell, cell - TURRET_IN_HULL); }
    if (cell) remember_cell(cell);
    if (cell) layer_note(cell, clip, owner);
    if (!g_cfg.see_through || !owner) return;
    DWORD uid = *(DWORD *)owner;
    int type = RAW_TYPE(uid);
    int sarge = type == 6 && UID_ARMY(uid) == 0;
    if (sarge || (type == 5 && UID_ARMY(uid) != 0)) {
        Fig *f = fig_slot(cell, owner, sarge);
        f->cell = cell; f->owner = owner; f->covered = 0; f->ncover = 0; f->ntall = 0; f->seen = GetTickCount();
        memcpy(f->rect, CELL_WRECT(cell), sizeof f->rect);
        return;
    }
    if (type == 7 || type == 8 || type == 9) {
        for (int i = 0; i < MAX_FIGS; i++) {
            Fig *f = &g_figs[i];
            if (!f->cell || !rects_overlap(CELL_WRECT(cell), f->rect)) continue;
            if (i == 0) f->covered = 1;          /* Sarge: low or tall, drawn again on top */
            /* every cell drawn over the figure, in drawing order, low ones too (the LAST MAX_COVER) */
            if (f->ncover == MAX_COVER) {
                f->ntall -= !f->cover_low[0];
                memmove(f->cover, f->cover + 1, sizeof f->cover[0] * (MAX_COVER - 1));
                memmove(f->cover_low, f->cover_low + 1, MAX_COVER - 1);
                f->ncover--;
            }
            memcpy(f->cover[f->ncover], CELL_WRECT(cell), sizeof f->cover[0]); f->cover_low[f->ncover] = (BYTE)low; f->ncover++;
            if (!low) { f->ntall++; f->covered = 1; }
        }
    }
}

/* What covers the figure at this world point is TALL: the cell drawn LAST over the figure whose rectangle holds
 * the point decides (rectangles only). 2026-11-19: before, the game's sprite-mask test (0x42faf0) had to say the
 * cell is solid there - it answers 0 for sprites without a mask, and the list held only the first 16 cells
 * (crates and wall parts filled it before the roof): inside the Desert depot Sarge was "under tall cover" at one
 * spot and not 48 px further (Wine log), so he was drawn solid on the roof. The figure's own pixels that the
 * redraw CHANGED are the covered ones - that part is exact. */
static int under_tall(const Fig *f, int wx, int wy)
{
    for (int i = f->ncover - 1; i >= 0; i--) {
        const int *w = f->cover[i];
        if (wx >= w[0] && wx < w[2] && wy >= w[1] && wy < w[3]) return !f->cover_low[i];
    }
    return 0;
}

/* After the battlefield is in the back buffer: Sarge drawn again on top - fully over low cover (rocks,
 * bushes), as a 50% ghost where a tall object (tree, building, sign) covers him. */
static int g_ghost_faults;              /* a guarded fault in the redraw: no more ghosts this session */
static int cell_room(int x, int y);
static int g_sarge_hidden;              /* % (of a third of his cell box) of Sarge's pixels under tall cover (interior) */
static void ghost_one(Surf *b, Fig *f, int sarge)
{
    if (sarge) g_sarge_hidden = 0;
    if (g_ghost_faults) return;
    if (!f->cell || !f->covered) return;
    if (IsBadReadPtr(f->cell, CELL_BYTES) || !(*(DWORD *)f->cell & 1) || CELL_OWNER(f->cell) != f->owner) { f->cell = NULL; return; }
    if (sarge) {
        BYTE *s = *(BYTE **)0x697520;             /* still the player's Sarge? */
        if (!s || f->owner != s) return;
    } else {                                      /* an enemy soldier: alive (TROOPER +0x68 1, health +0x1c) */
        if (IsBadReadPtr(f->owner, OWNER_BYTES) || RAW_TYPE(*(DWORD *)f->owner) != 5 || *(int *)(f->owner + 0x68) != 1
            || *(short *)(f->owner + 0x1c) <= 0) { f->cell = NULL; return; }
        if (!g_cfg.see_enemies || !f->ntall) return;   /* only behind tall cover (buildings, trees) */
        if (GetTickCount() - f->seen > ENEMY_FRESH_MS) { f->cell = NULL; return; }   /* not drawn lately: stale */
        if (!bullets_fired_within(*(DWORD *)f->owner, SHOW_AFTER_FIRE_MS)) return;   /* only once he shoots */
        f->shown_at = GetTickCount();                 /* roofs_interior_note: a circle round him (user 2026-11-20) */
    }
    int *w = CELL_WRECT(f->cell);
    RECT r = { w[0] - SCREEN_L, w[1] - SCREEN_T, w[2] - SCREEN_L, w[3] - SCREEN_T };
    RECT view = { 0, 0, SCREEN_R - SCREEN_L, SCREEN_B - SCREEN_T };
    if (view.right > b->w) view.right = b->w;
    if (view.bottom > b->h) view.bottom = b->h;
    if (!IntersectRect(&r, &r, &view) || r.right - r.left > MAX_W || r.bottom - r.top > MAX_H) return;
    static BYTE saved[MAX_H][MAX_W];
    int ww = r.right - r.left, hh = r.bottom - r.top;
    for (int y = 0; y < hh; y++) memcpy(saved[y], b->bits + (size_t)(r.top + y) * b->pitch + r.left, ww);
    /* what was drawn ON the battlefield there before the redraw (override mask: HUD, pop-ups, the mission-over
     * radio and its text): the figure must not show through it (user 2026-11-19: "make sure the sarge doesn't
     * bleed through to the end mission overlay") - those pixels are put back as they were */
    BYTE *ov = hdworld_over_mask(b);                 /* hdworld.c override mask (upscaled battlefield) */
    static BYTE savedo[MAX_H][MAX_W];
    if (ov) for (int y = 0; y < hh; y++) memcpy(savedo[y], ov + (size_t)(r.top + y) * b->w + r.left, ww);
    BYTE *old_bits = LOCKED_BITS;
    int old_pitch = LOCKED_PITCH;
    LOCKED_BITS = b->bits;
    LOCKED_PITCH = b->pitch;
    /* Clipped to the BACK BUFFER, and the clip rectangle is INCLUSIVE (2026-11-19, two crashes in the game's
     * blitter 0x40434d, both writing row 540 of the 1024x540 buffer = its first byte past the end): FUN_0042f120
     * intersects the cell with the clip and then adds 1 to a clipped right / bottom edge (the game's screen rect
     * is left..right INCLUSIVE; the map view is 554 rows, the back buffer 540). So right = left + w - 1, bottom =
     * top + h - 1. An enemy cut by the bottom edge hit it; Sarge (mid-screen) never did. Guarded as well: a fault
     * in here is logged, the ghost is switched off for the session, the game goes on. */
    int cw = view.right < b->w ? view.right : b->w - 1, ch = view.bottom < b->h ? view.bottom : b->h - 1;
    int clip[4] = { SCREEN_L, SCREEN_T, SCREEN_L + cw, SCREEN_T + ch };
    Guard g;
    if (GUARD_TRY(g, "ghost redraw (roofs.c)")) {
        DRAW_CELL_BACK(f->cell, clip);
        guard_leave(&g);
    } else {
        g_ghost_faults++;
        f->cell = NULL;
    }
    LOCKED_BITS = old_bits;
    LOCKED_PITCH = old_pitch;
    if (g_ghost_faults) return;
    /* Under TALL cover (2026-11-19, user: "sarge just renders on top of the building - should be better embedded
     * ... maybe a little more ghostly"; later: "the building view is still not doing anything other than
     * rendering sarge on top" - "same for the enemies").
     * Figure pixel covered by something = a pixel the redraw CHANGED (where he was visible anyway it wrote the
     * same byte). Upscaled battlefield (override mask present): the cover's byte is put back and the mask gets 3
     * = "ghost": gpu.c keeps the figure's own upscaled art there and blends it at 50 % over the game picture
     * (the roof). The old way - put the byte back in a pattern and clear the mask - changed NOTHING on screen:
     * the GPU's edge blending drew the figure's whole upscaled image back over the gaps (seen zoomed in Wine,
     * 261 pixels "put back", Sarge solid). Plain 8-bit picture (no mask): outline whole, 1 inside pixel in
     * GHOST_EVERY kept. */
    static BYTE fig[MAX_H][MAX_W];
    for (int y = 0; y < hh; y++) {
        const BYTE *row = b->bits + (size_t)(r.top + y) * b->pitch + r.left;
        for (int x = 0; x < ww; x++) fig[y][x] = row[x] != saved[y][x];
    }
    int n_fig = 0, n_tall = 0;
    for (int y = 0; y < hh; y++) {
        BYTE *row = b->bits + (size_t)(r.top + y) * b->pitch + r.left;
        BYTE *om = ov ? ov + (size_t)(r.top + y) * b->w + r.left : NULL;
        for (int x = 0; x < ww; x++) {
            if (!fig[y][x]) continue;
            if (om && savedo[y][x]) { row[x] = saved[y][x]; om[x] = savedo[y][x]; continue; }   /* interface / pop-up there: not ours */
            n_fig++;
            if (!under_tall(f, r.left + x + SCREEN_L, r.top + y + SCREEN_T)) continue;   /* low cover: the figure stays on top */
            n_tall++;
            if (om) { row[x] = saved[y][x]; om[x] = 3; continue; }
            int edge = x == 0 || y == 0 || x == ww - 1 || y == hh - 1 || !fig[y][x - 1] || !fig[y][x + 1] || !fig[y - 1][x] || !fig[y + 1][x];
            if (edge || ((r.left + x + 2 * (r.top + y)) % GHOST_EVERY) == 0) continue;
            row[x] = saved[y][x];
        }
    }
    if (sarge) g_sarge_hidden = n_tall * 100 / (ww * hh / 3 > 0 ? ww * hh / 3 : 1);
    if (sarge && g_cfg.log >= 2) {
        static DWORD at;
        if (GetTickCount() - at > 2000) { at = GetTickCount(); mlog("ghost: Sarge %dx%d covered px %d (under tall %d), cover cells %d (%d tall), mask %s", ww, hh, n_fig, n_tall, f->ncover, f->ntall, ov ? "yes" : "no"); }
    }
}

static void roofs_draw_ghost_body(Surf *b)
{
    static BYTE *last_sarge;                      /* new mission (or menus): every slot is from the old one */
    BYTE *s = *(BYTE **)0x697520;
    if (s != last_sarge || !layout_view_is_gameplay()) { memset(g_figs, 0, sizeof g_figs); last_sarge = s; }
    if (!g_cfg.see_through || !b || !b->bits || !layout_view_is_gameplay()) return;
    if (game_menu_up()) return;                   /* pause menu & co. over the mission (2026-11-20: ghost over the menu) */
    for (int i = MAX_FIGS - 1; i >= 0; i--) ghost_one(b, &g_figs[i], i == 0);   /* Sarge last: on top */
}

/* log=3: every remembered cell near a world point (debugging what a sprite is) */
void roofs_debug_near(int wx, int wy)
{
    int n = 0;
    for (int k = 0; k < CELLS; k++) {
        BYTE *c = g_cells[k];
        if (!c || IsBadReadPtr(c, CELL_BYTES) || (CELL_OWNER(c) && IsBadReadPtr(CELL_OWNER(c), OWNER_BYTES))) continue;
        const int *w = CELL_WRECT(c);
        int cx = (w[0] + w[2]) / 2, cy = (w[1] + w[3]) / 2;
        if (abs(cx - wx) > 80 || abs(cy - wy) > 80) continue;
        BYTE *ow = CELL_OWNER(c); DWORD uid = ow ? *(DWORD *)ow : 0;
        mlog("roofs: cell %p flags %08lx rect %d,%d,%d,%d pos %d,%d owner %p uid %08lx army %d type %d", c, *(DWORD *)c,
             w[0], w[1], w[2], w[3], *(short *)(c + 0x10), *(short *)(c + 0x12), ow, uid, UID_ARMY(uid), RAW_TYPE(uid) - 1);
        if (++n >= 12) break;
    }
    mlog("roofs: %d cells remembered, %d near %d,%d", g_cells_n, n, wx, wy);
}

/* All remembered active cells (controls.c self-test). */
int roofs_cells_list(BYTE **out, int max)
{
    cells_run_check();
    int n = 0;
    for (int k = 0; k < CELLS && n < max; k++) { BYTE *c = live_cell(k); if (c) out[n++] = c; }
    return n;
}

/* ---------------------------------------------------------------- the game's sprite-mask test on a remembered cell
 * ObjectIntersectPoint 0x42faf0 (__fastcall ecx cell, edx SPOINT* world point; 0 = see-through / outside) reads the
 * cell's sprite record (+4): with a mask (record +4) it tests the point against the cell's world rect (+0x1c) and
 * reads mask byte (y - top) * stride + (x - left) / 8, stride = ((width at record +0x20) + 31) / 8 & ~3 (FUN_0042fa60);
 * without one it reads the picture (record +0) by the record's flags (+0x3a). roofs_cell_hit_ok checks that all of
 * it is readable (once per cell and batch of tests - IsBadReadPtr is slow); roofs_cell_hit runs the call inside a
 * guard, so a cell freed between the check and the call costs a "GUARDED" log line instead of the game.
 * Callers: controls.c (hover / fire zone on target objects, log=3 self-test), modern.c (route line under sprites). */
#define OBJECT_INTERSECT_POINT ((int (__fastcall *)(BYTE *, short *))0x42faf0)
#define SPRITE_BYTES 0x40                /* sprite record fields read: picture +0, mask +4, width +0x20, flags +0x3a */
int roofs_cell_hit_ok(BYTE *c)
{
    if (!c || IsBadReadPtr(c, CELL_BYTES) || !(*(DWORD *)c & 1)) return 0;
    const int *w = CELL_WRECT(c);
    int rw = w[2] - w[0], rh = w[3] - w[1];
    if (rw <= 0 || rh <= 0 || rw > 4096 || rh > 4096) return 0;
    BYTE *sp = *(BYTE **)(c + 4);
    if (!sp) return 1;                                  /* the game answers 0 itself */
    if (IsBadReadPtr(sp, SPRITE_BYTES)) return 0;
    BYTE *mask = *(BYTE **)(sp + 4);
    if (mask) {
        int width = *(int *)(sp + 0x20);
        if (width <= 0 || width > 4096) return 0;
        int stride = ((width + 31) >> 3) & ~3;
        return !IsBadReadPtr(mask, (UINT_PTR)stride * (rh - 1) + ((rw + 7) >> 3));
    }
    BYTE *pic = *(BYTE **)sp;
    return pic && !IsBadReadPtr(pic, 16);
}
int roofs_cell_hit(BYTE *c, int wx, int wy)
{
    short p[2] = { (short)wx, (short)wy };
    volatile int hit = 0;
    Guard g;
    if (GUARD_TRY(g, "sprite-mask test of a remembered cell (roofs.c)")) { hit = OBJECT_INTERSECT_POINT(c, p); guard_leave(&g); }
    return hit;
}

void __fastcall roofs_draw_cell(BYTE *cell, void *clip) { PROF_BEGIN(t); roofs_draw_cell_body(cell, clip); PROF_END(t, PR_CELLS); }   /* timed (trace) */

void roofs_draw_ghost(Surf *b) { PROF_BEGIN(t); roofs_draw_ghost_body(b); PROF_END(t, PR_GHOST); }   /* timed (trace) */

/* ---------------------------------------------------------------- building interior and doors (2026-11-20)
 * User: "I would like to provide an outline of the walkable area in the building. could be cool if the top of the
 * building went transparent and we could see the walls only"; later: "can the edges be smoothed out? might be nice
 * to display the back door of building when near. its also displaying when I'm not in any buildings".
 * The game has no "inside" flag and no picture of the floor under a roof. From the game's DATA instead:
 *   - the building: building cells (game table categories, is_building_cat) whose rectangle, grown by IN_NEAR, holds
 *     Sarge, plus parts touching them; its box (in 8 px cells) bounds everything below;
 *   - free cell = where Sarge may stand by the game's own rule (controls_sarge_blocked_at: the objects' pixel masks
 *     at a point). NOT the movement layer: a building's footprint marks its whole area blocked there (Wine: 47);
 *   - floor cell = free and on the building FOOTPRINT (movement layer >= 15 marks the whole building); door cell =
 *     a floor cell next to a free cell off the footprint (the opening to the outside) - shown when near or inside;
 *   - INSIDE only when the ghost redraw found most of his pixels under tall cover (g_sarge_hidden, roofs.c
 *     ghost_one) and his cell (or one within 2) is floor - before, the building's picture box alone was enough
 *     and the flood ran from the yard into another building (user's screenshot);
 *   - floor plan = the floor cells flood-filled from his cell.
 * gpu.c smooths both masks (bilinear over cell centres: diagonal edges instead of 8 px steps), opens the roof over
 * the floor (a shaded room, light outline), shows the ghost there almost solid, and glows the doors on the
 * building's pixels. Fades over IN_FADE_MS. The free / enclosed tests are cached per building box (IN_REFRESH_MS).
 * [display] see_interior=1. */
#define IN_N          64                 /* window: IN_N x IN_N cells */
#define IN_CELL       8
#define IN_FADE_MS    350
#define IN_REFRESH_MS 2000
#define IN_MAX_CELLS  16
#define IN_NEAR       40                 /* px around a building's picture box: "near" (doors shown) */
#define IN_HIDDEN_PCT 35
#define UNDER_REACH   20                 /* a building-solid cell this many cells below = under the roof */
#define ENEMY_CIRCLE  40                 /* px: the cut-away circle around an enemy firing from under a roof */
#define CIRCLE_CELLS  9                  /* the cut-away circle around Sarge: 9 cells = 72 game px */
#define JOIN          4                  /* walkable inside pieces up to ~2*JOIN cells apart are joined */
#define SEAL          2                  /* blocked cells grown by this to close door gaps (sealed fill) */
#define WALL_HOP      2                  /* inner walls up to this many cells thick are crossed */
#define NEAR_DOOR     6                  /* a door within this many cells (48 px) of Sarge: "near" */
#define EXIT_REACH    3                  /* inside: free cells beyond a door that join the shape */
#define DOOR_GROW     1                  /* cells around a door opened when near (not inside) */                 /* % of Sarge's pixels under tall cover to count as inside */
static int is_building_cat(int cat) { return cat == 12 || cat == 13 || (cat >= 16 && cat <= 19) || cat == 22; }

static int building_cell(BYTE *c)
{
    BYTE *ow = CELL_OWNER(c);
    if (!ow) return 0;
    int raw = RAW_TYPE(*(DWORD *)ow);
    return raw >= 7 && raw <= 9 && is_building_cat(*(short *)(ow + 0x68));
}

typedef struct { int cx0, cy0, inside, circle; unsigned mask[IN_N * 2], door[IN_N * 2]; BYTE *cells[IN_MAX_CELLS]; int ncells; } Floor;
static BYTE g_enc[IN_N][IN_N];           /* 0 unknown, 1 floor, 2 not */
static int g_bx0, g_by0, g_bx1, g_by1, g_wx0, g_wy0;     /* building box and window, in cells */

/* "May Sarge stand in this cell" (the game's own test, an object-list walk per cell), kept per WORLD cell.
 * 2026-11-20 (stutter, user's log: h.obj 1.7-3.7 ms every ~5 frames while walking near a building, 8-21 ms in a
 * base): the answers were kept per building box and ALL thrown away when the box changed (in a base it changes as
 * parts come into / leave reach, every few steps) and every IN_REFRESH_MS - each time every cell of the box was
 * asked again in one frame. Now an answer belongs to its world cell, whatever the box, and lives IN_REFRESH_MS / 2
 * .. IN_REFRESH_MS (per cell, so they do not all run out in the same frame): the same answers, never older than
 * before, asked for a few cells per call instead of all at once. The table is BLK_SPAN cells square and wraps
 * (only cells inside the IN_N window are ever asked: no two of them share an entry); an entry names its cell. */
#define BLK_SPAN 128                     /* power of two, >= IN_N */
typedef struct { short x, y; BYTE v; DWORD until; } BlkCell;     /* v: 0 nothing, 1 free, 2 blocked */
static BlkCell g_blkc[BLK_SPAN][BLK_SPAN];
static DWORD g_in_now;                   /* GetTickCount when this floor_now call began (one time for the whole call) */
static int cell_blocked(int x, int y)    /* absolute cell; outside the box / window = blocked */
{
    if (x < g_bx0 || x > g_bx1 || y < g_by0 || y > g_by1) return 1;
    int i = x - g_wx0, j = y - g_wy0;
    if (i < 0 || j < 0 || i >= IN_N || j >= IN_N) return 1;
    BlkCell *c = &g_blkc[y & (BLK_SPAN - 1)][x & (BLK_SPAN - 1)];
    if (!c->v || c->x != x || c->y != y || (LONG)(g_in_now - c->until) >= 0) {
        c->x = (short)x; c->y = (short)y;
        c->v = controls_sarge_blocked_at(x * IN_CELL + IN_CELL / 2, y * IN_CELL + IN_CELL / 2) ? 2 : 1;
        c->until = g_in_now + IN_REFRESH_MS / 2 + (((unsigned)x * 73856093u ^ (unsigned)y * 19349663u) >> 8) % (IN_REFRESH_MS / 2);
    }
    return c->v == 2;
}
/* the cell lies on a building FOOTPRINT: the movement layer (16 px tiles, 0x4e3554) marks a building's whole
 * area blocked (Wine: 47 under the depot floor), walls and floor alike */
#define MOVE_LAYER  (*(signed char **)0x4e3554)
#define MOVE_W      (*(unsigned short *)0x4e368c)
static int on_footprint(int x, int y)
{
    signed char *mv = MOVE_LAYER;
    int W = MOVE_W, tx = x * IN_CELL / 16, ty = y * IN_CELL / 16;
    if (!mv || W <= 0 || W > 1024 || tx < 0 || ty < 0 || tx >= W || ty >= W) return 0;
    return mv[(size_t)ty * W + tx] >= 15;
}
/* floor: Sarge may stand there AND it is on the footprint (inside the building). (First tried: "blocked in all four
 * directions" - a roof does not block, so free cells under the roof counted as outside and the doors glowed in
 * stripes across the roof, Wine 2026-11-20.) */
static int cell_floor(int x, int y)
{
    int i = x - g_wx0, j = y - g_wy0;
    if (i < 0 || j < 0 || i >= IN_N || j >= IN_N) return 0;
    if (g_enc[j][i]) return g_enc[j][i] == 1;
    int res = !cell_blocked(x, y) && on_footprint(x, y);
    g_enc[j][i] = res ? 1 : 2;
    return res;
}


/* blocked by the building's OWN objects (the game's point list 0x42fca0, movement +0x6f of the parts in g_bowners) */
typedef BYTE *(__fastcall *ObjAtFn)(short *pt);
#define OBJECTS_AT_PT ((ObjAtFn)0x42fca0)
static BYTE *g_bowners[16]; static int g_nbowners;
static BYTE *g_bcells[16]; static int g_nbcells;     /* the building's cells (their drawn images) */
static int floor_tex_entry(int e)
{
    for (int k = 0; k < g_nbcells; k++) if (hdworld_cell_has_entry(g_bcells[k], e)) return 1;
    return 0;
}
static int building_blocked(int x, int y)
{
    short p[2] = { (short)(x * IN_CELL + IN_CELL / 2), (short)(y * IN_CELL + IN_CELL / 2) };
    int mv = 0;
    for (BYTE *it = OBJECTS_AT_PT(p); it; it = *(BYTE **)(it + 0x60))
        for (int i = 0; i < g_nbowners; i++) if (it == g_bowners[i]) mv += *(signed char *)(it + 0x6f);
    return mv >= 15;
}
/* building hitbox answers, kept per building box (reset with the other caches) */
static BYTE g_bb[IN_N][IN_N];            /* 0 unknown, 1 free, 2 blocked - window cells */
static int building_blocked_w(int wx0, int wy0, int x, int y)
{
    if (x < 0 || y < 0 || x >= IN_N || y >= IN_N) return 1;
    if (!g_bb[y][x]) g_bb[y][x] = building_blocked(wx0 + x, wy0 + y) ? 2 : 1;
    return g_bb[y][x] == 2;
}
/* a floor cell (window x, y): inside the building box, free in its own hitbox, its image drawn there */
static int g_floor_noimg;                 /* 1 = floor cells without the building-image test (fallback) */
static int floor_cell(const void *flv, BYTE (*cm)[IN_N], int wx0, int wy0, int bx0, int by0, int bx1, int by1, int sl, int st, int x, int y)
{
    const struct { int cx0, cy0, inside, circle; } *hdr = flv;   /* Floor starts with these */
    (void)hdr;
    if (x < 0 || y < 0 || x >= IN_N || y >= IN_N) return 0;
    if (cm[y][x]) return cm[y][x] == 1;
    int ax = wx0 + x, ay = wy0 + y, ok = 0;
    if (ax >= bx0 && ax <= bx1 && ay >= by0 && ay <= by1 && !building_blocked_w(wx0, wy0, x, y) && !cell_blocked(ax, ay)) {   /* 2026-11-20 (user: "it should respect the bounding"): the game's full Sarge hitbox too */
        int e = hdworld_entry_at(ax * IN_CELL + IN_CELL / 2 - sl, ay * IN_CELL + IN_CELL / 2 - st);
        ok = g_floor_noimg || (e >= 0 && floor_tex_entry(e));
    }
    cm[y][x] = ok ? 1 : 2;
    return ok;
}
/* a ROOM cell: free for Sarge and walled in (blocked) in all four directions inside the box - where he can walk
 * under the roof. (Wine 2026-11-20: the depot's room is NOT marked in the movement layer - only the solid parts of a
 * building are; opening the footprint opened everything but his room.) */
/* 2026-11-20 (stutter): the answer for every cell of the box is worked out ONCE per floor_now call (rooms_build)
 * with four sweeps, instead of four rays of cell_blocked calls every time a cell was asked (the door search asks
 * each cell and its neighbours; Wine, Boot Camp depot, box 51 x 39 cells, all answers known: 0.25 ms for one pass
 * over the cells the old way, 0.025 ms for the table; checked equal cell by cell). Same rule:
 * a ray leaves the box = open; a cell of the box outside the IN_N window counts as blocked (cell_blocked), so a box
 * wider than the window closes the ray at the window's edge. */
static BYTE g_room[IN_N][IN_N];          /* this call: 1 = room cell (window cells inside the box) */
static void rooms_build(void)
{
    static BYTE blk[IN_N][IN_N], hit[IN_N][IN_N];            /* blocked; rays closed so far (bit per direction) */
    memset(g_room, 0, sizeof g_room);
    int i0 = max(g_bx0, g_wx0) - g_wx0, i1 = min(g_bx1, g_wx0 + IN_N - 1) - g_wx0;
    int j0 = max(g_by0, g_wy0) - g_wy0, j1 = min(g_by1, g_wy0 + IN_N - 1) - g_wy0;
    if (i1 < i0 || j1 < j0) return;
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) { blk[j][i] = (BYTE)cell_blocked(g_wx0 + i, g_wy0 + j); hit[j][i] = 0; }
    int beyond_l = g_bx0 < g_wx0, beyond_r = g_bx1 > g_wx0 + IN_N - 1, beyond_u = g_by0 < g_wy0, beyond_d = g_by1 > g_wy0 + IN_N - 1;
    for (int j = j0; j <= j1; j++) {
        int seen = beyond_l;
        for (int i = i0; i <= i1; i++) { if (seen) hit[j][i] |= 1; if (blk[j][i]) seen = 1; }
        seen = beyond_r;
        for (int i = i1; i >= i0; i--) { if (seen) hit[j][i] |= 2; if (blk[j][i]) seen = 1; }
    }
    for (int i = i0; i <= i1; i++) {
        int seen = beyond_u;
        for (int j = j0; j <= j1; j++) { if (seen) hit[j][i] |= 4; if (blk[j][i]) seen = 1; }
        seen = beyond_d;
        for (int j = j1; j >= j0; j--) { if (seen) hit[j][i] |= 8; if (blk[j][i]) seen = 1; }
    }
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) g_room[j][i] = (BYTE)(!blk[j][i] && hit[j][i] == 15);
}
static int cell_room(int x, int y)       /* absolute cell; valid inside floor_now after rooms_build */
{
    int i = x - g_wx0, j = y - g_wy0;
    return i >= 0 && j >= 0 && i < IN_N && j < IN_N && g_room[j][i];
}
/* a BACK door: it opens upward on the screen (free outside ground just above it) - hidden behind the roof. Front
 * and side doors are seen anyway (user 2026-11-20: "don't draw the threshold for front facing building. only the back") */
static int back_door(int x, int y)
{
    for (int k = 1; k <= 2; k++) {
        if (cell_blocked(x, y - k)) return 0;
        if (!cell_room(x, y - k)) return 1;
    }
    return 0;
}
/* a door is an OPENING in a wall: going out in direction d, the cells along the wall line on both sides (within
 * GAP_REACH) are blocked - a plain footprint edge (eaves, porch) is not (Wine: 228 "door" cells before) */
#define GAP_REACH 3
static int wall_gap(int x, int y, int d)
{
    static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
    int px = DY[d], py = DX[d], left = 0, right = 0;
    for (int k = 1; k <= GAP_REACH && !left; k++) left = cell_blocked(x + px * k, y + py * k) || cell_blocked(x + DX[d] + px * k, y + DY[d] + py * k);
    for (int k = 1; k <= GAP_REACH && !right; k++) right = cell_blocked(x - px * k, y - py * k) || cell_blocked(x + DX[d] - px * k, y + DY[d] - py * k);
    return left && right;
}
#define WIN_STEP 16                      /* floor_now: the window on Sarge moves in steps of this many cells */
static int win_floor(int v) { return v >= 0 ? v / WIN_STEP * WIN_STEP : -((-v + WIN_STEP - 1) / WIN_STEP) * WIN_STEP; }
#define BIT_SET(m, x, y) ((m)[((y) - fl->cy0) * 2 + (((x) - fl->cx0) >> 5)] |= 1u << (((x) - fl->cx0) & 31))
#define BIT_GET(m, x, y) ((m)[((y) - fl->cy0) * 2 + (((x) - fl->cx0) >> 5)] & (1u << (((x) - fl->cx0) & 31)))
static int floor_now(Floor *fl)
{
    BYTE *s = *(BYTE **)0x697520;
    if (!s) return 0;
    int sx = *(short *)(s + 4), sy = *(short *)(s + 6);
    RECT box = { 0 };
    fl->ncells = 0; fl->inside = 0; fl->circle = 0;
    for (int pass = 0; pass < 2; pass++)                         /* the building near him, and its touching parts */
        for (int k = 0; k < CELLS && fl->ncells < IN_MAX_CELLS; k++) {
            BYTE *c = live_cell(k);
            if (!c || !building_cell(c)) continue;
            const int *w = CELL_WRECT(c);
            RECT r = { w[0], w[1], w[2], w[3] }, t;
            int take;
            if (pass == 0) take = sx >= w[0] - IN_NEAR && sx < w[2] + IN_NEAR && sy >= w[1] - IN_NEAR && sy < w[3] + IN_NEAR;
            else { RECT g = box; InflateRect(&g, 2, 2); take = IntersectRect(&t, &g, &r); }
            int dup = 0;
            for (int i = 0; i < fl->ncells; i++) dup |= fl->cells[i] == c;
            if (!take || dup) continue;
            fl->cells[fl->ncells++] = c;
            if (IsRectEmpty(&box)) box = r; else UnionRect(&box, &box, &r);
        }
    if (!fl->ncells) return 0;
    int bx0 = box.left / IN_CELL, by0 = box.top / IN_CELL, bx1 = (box.right - 1) / IN_CELL, by1 = (box.bottom - 1) / IN_CELL;
    /* The IN_N window on the building - or, when the building box is wider / taller than the window, on SARGE.
     * 2026-11-21 (user, Desert 1 town: "the shader not drawing all building"; log "no floor cell within 3 (blocked 1,
     * bhit 0)"): the town's wooden buildings touch each other, pass 1 above chains them into one box of 124 x 134
     * cells (Wine: box 29,243..152,376, window 58,277+64) and the window centred on that box left Sarge (cell 52,314)
     * OUTSIDE it - every cell round him counted as blocked (cell_blocked / floor_cell: outside the window). On Sarge
     * the window moves in steps of WIN_STEP cells (the caches below are per window), Sarge 32..47 cells from its left
     * / top edge: the circle (CIRCLE_CELLS + 1) and the door search (NEAR_DOOR) always fit. */
    int wx0 = (bx0 + bx1) / 2 - IN_N / 2, wy0 = (by0 + by1) / 2 - IN_N / 2;
    if (bx1 - bx0 + 1 > IN_N) wx0 = win_floor(sx / IN_CELL - IN_N / 2);
    if (by1 - by0 + 1 > IN_N) wy0 = win_floor(sy / IN_CELL - IN_N / 2);
    static DWORD cached_at; static int cbx0 = -1, cby0, cbx1, cby1, cwx0, cwy0;
    g_in_now = GetTickCount();
    if (bx0 != cbx0 || by0 != cby0 || bx1 != cbx1 || by1 != cby1 || wx0 != cwx0 || wy0 != cwy0 || g_in_now - cached_at > IN_REFRESH_MS) {
        memset(g_enc, 0, sizeof g_enc); memset(g_bb, 0, sizeof g_bb);     /* (cell_blocked keeps its own answers per world cell) */
        cbx0 = bx0; cby0 = by0; cbx1 = bx1; cby1 = by1; cwx0 = wx0; cwy0 = wy0; cached_at = g_in_now;
    }
    g_bx0 = bx0; g_by0 = by0; g_bx1 = bx1; g_by1 = by1; g_wx0 = fl->cx0 = wx0; g_wy0 = fl->cy0 = wy0;
    rooms_build();                                               /* cell_room for this call */
    memset(fl->mask, 0, sizeof fl->mask); memset(fl->door, 0, sizeof fl->door);
    int ndoor = 0;
    for (int y = max(by0, wy0); y <= min(by1, wy0 + IN_N - 1); y++)          /* doors */
        for (int x = max(bx0, wx0); x <= min(bx1, wx0 + IN_N - 1); x++) {
            if (!cell_room(x, y)) continue;                     /* 2026-11-20: room side (user: "don't show the corners") */
            static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
            for (int d = 0; d < 4; d++)
                if (!cell_blocked(x + DX[d], y + DY[d]) && !cell_room(x + DX[d], y + DY[d]) && wall_gap(x, y, d)) { BIT_SET(fl->door, x, y); ndoor++; break; }
        }
    if (g_cfg.log >= 2) {                     /* the masks as text (user: "what do the masks look like?"): once per box */
        static int lbx0 = -1, lby0 = -1;
        if (bx0 != lbx0 || by0 != lby0) {
            lbx0 = bx0; lby0 = by0;
            mlog("interior mask: box cells %d,%d..%d,%d (8 px), '#' blocked for Sarge, '.' free, 'R' room, 'D' door, 'S' Sarge", bx0, by0, bx1, by1);
            for (int y = max(by0, wy0); y <= min(by1, wy0 + IN_N - 1); y++) {
                char line[IN_N + 1]; int n = 0;
                for (int x = max(bx0, wx0); x <= min(bx1, wx0 + IN_N - 1); x++) {
                    int door = (fl->door[(y - fl->cy0) * 2 + ((x - fl->cx0) >> 5)] >> ((x - fl->cx0) & 31)) & 1;
                    line[n++] = (x == sx / IN_CELL && y == sy / IN_CELL) ? 'S' : door ? 'D' : cell_blocked(x, y) ? '#' : cell_room(x, y) ? 'R' : '.';
                }
                line[n] = 0;
                mlog("  %s", line);
            }
        }
    }
    /* 2026-11-20 (user: "the building interior view should be continuous and one solid object"): the opened area
     * is the ROOM he is in (cell_room cells connected to his; NOT the footprint - it holds only the solid parts), holes filled, grown by one cell, so the
     * walls keep their art - not the walkable corridor. Near a building (not inside): a patch over each entrance
     * (door cells grown by DOOR_GROW, on the footprint) - "the back side of building could display this over the
     * entrance when getting near". gpu.c blurs the mask: round corners instead of 8 px steps. */
    static BYTE fp[IN_N][IN_N], comp[IN_N][IN_N];
    for (int j = 0; j < IN_N; j++)
        for (int i = 0; i < IN_N; i++) {
            int x = wx0 + i, y = wy0 + j;
            fp[j][i] = x >= bx0 && x <= bx1 && y >= by0 && y <= by1 && on_footprint(x, y);
        }
    /* circle mode: under the roof is enough (2026-11-20: in a tight spot no free cell was within 2 - no cut-away) */
    int scx = sx / IN_CELL - wx0, scy = sy / IN_CELL - wy0, found = g_sarge_hidden >= IN_HIDDEN_PCT;
    if (!found && g_sarge_hidden > 0 && g_cfg.log >= 1) { static DWORD at2; if (GetTickCount() - at2 > 3000) { at2 = GetTickCount(); mlog("interior: Sarge %d,%d partly under a roof (hidden %d%% < %d) - no circle", sx, sy, g_sarge_hidden, IN_HIDDEN_PCT); } }
    if (0)
        for (int r = 0; r <= 2 && !found; r++)
            for (int dy = -r; dy <= r && !found; dy++) for (int dx = -r; dx <= r && !found; dx++) {
                int x = scx + dx, y = scy + dy;
                if (x >= 0 && y >= 0 && x < IN_N && y < IN_N && !cell_blocked(wx0 + x, wy0 + y)) { scx = x; scy = y; found = 1; }
            }
    memset(comp, 0, sizeof comp);
    static short qx[IN_N * IN_N], qy[IN_N * IN_N];
    if (found) {
        /* 2026-11-20 (user: "a shader that shows a circle around sarge - if its a building and hes under it - it draws
         * where there is no bounding box. does not draw if there is no building texture and if there is no bounding
         * box"): the floor = cells within CIRCLE_CELLS of Sarge that the BUILDING's own hitbox leaves free (its
         * objects' masks, building_blocked); gpu.c draws it only on that building's texture and fades it at the
         * circle's edge (tsg). No sealing / joining needed - the circle keeps it to his surroundings. */
        g_nbowners = 0;
        for (int i = 0; i < fl->ncells && g_nbowners < IN_MAX_CELLS; i++) if (CELL_OWNER(fl->cells[i])) g_bowners[g_nbowners++] = CELL_OWNER(fl->cells[i]);
        /* 2026-11-20 (user: "the shader should be able to fully expand in the texture space to cover the non-bounds
         * space"; "sometimes it draws even if there is bounds" = a front wall face over a free doorway): FLOOR cell =
         *   - free in the building's own hitbox (building_blocked), and
         *   - this building's image is what is drawn at that MAP pixel (hdworld_entry_at / hdworld_cell_has_entry), and
         *   - a solid cell of the building lies within UNDER_REACH below it: under the roof the building's wall is
         *     south of the spot; a front face hangs over ground with no building wall below it. (Ground just north of
         *     the back wall is under the roof too - the back exit comes along.)
         * Flood-filled from Sarge's cell (or the nearest floor cell within 3): one shape, no circle. */
        int c0x = sx / IN_CELL - wx0, c0y = sy / IN_CELL - wy0;
        g_nbcells = 0;
        for (int i = 0; i < fl->ncells && g_nbcells < 16; i++) g_bcells[g_nbcells++] = fl->cells[i];
        /* 2026-11-20 (user: "expand out from sarge around the edges - and once hitting the smaller exit, stop drawing
         * once it wraps back around the outside of the door"): flood from Sarge over FLOOR cells = free in the
         * building's own hitbox AND this building's image drawn at that map pixel. Through the corridor and the door
         * it stops where the ground is no longer under the building's image. (The "wall below" rule of the hour
         * before cut the corridor to the door out - Wine dump.) Cells are tested only as the flood reaches them, the
         * hitbox answers are kept per building (user: "getting stutters now": every 8 px of movement tested all 4096
         * cells x 21 object-list walks). */
        static BYTE cm[IN_N][IN_N];                              /* 0 unknown, 1 floor, 2 not */
        memset(cm, 0, sizeof cm);
        int sl = *(int *)0x4e2fd4, st = *(int *)0x4e2fd8 + layout_field_shift();   /* back buffer origin in the world */
        /* 2026-11-20 (user: "the shader is still messing up - lets go back to the circle around sarge"): the flood
         * only within CIRCLE_CELLS of Sarge (gpu.c fades the circle's edge, tsg) */
        #define IN_CIRCLE(x, y) (((x) - c0x) * ((x) - c0x) + ((y) - c0y) * ((y) - c0y) <= (CIRCLE_CELLS + 1) * (CIRCLE_CELLS + 1))
        #define FLOOR_AT(x, y) (IN_CIRCLE(x, y) && floor_cell(fl, cm, wx0, wy0, bx0, by0, bx1, by1, sl, st, (x), (y)))
        int fx = -1, fy = -1;
        /* 2026-11-21: under the roof = what is drawn over Sarge's body is THIS building's image (map pixel refs, as
         * floor_cell). Wine, Desert 1 depot yard (1331,946, a dead enemy lying across Sarge in front of the building's
         * wall face): "hidden" counts any cover inside a tall cell's RECTANGLE, and the yard round him passes the floor
         * test where the wall face hangs over it - a circle cut into the front wall. Points known to show something
         * else (Sarge himself, ground, another sprite) and none showing the building: not under it. No image
         * reference at all (-1 everywhere): undecided, as before (the fallback below). */
        int feet_known = 0, feet_bld = 0;
        {
            static const int PX[5][2] = { { 0, -2 }, { 0, -8 }, { 0, -14 }, { -4, -8 }, { 4, -8 } };
            for (int k = 0; k < 5; k++) {
                int e = hdworld_entry_at(sx + PX[k][0] - sl, sy + PX[k][1] - st);
                if (e < 0) continue;
                feet_known++;
                if (floor_tex_entry(e)) feet_bld++;
            }
        }
        int not_under = feet_known && !feet_bld;
        if (not_under && g_cfg.log >= 1) { static DWORD at4; if (GetTickCount() - at4 > 3000) { at4 = GetTickCount();
            mlog("interior: Sarge %d,%d hidden %d%% but no building image over him (%d points known) - no circle", sx, sy, g_sarge_hidden, feet_known); } }
        for (int r = 0; r <= 3 && fx < 0 && !not_under; r++)
            for (int dy = -r; dy <= r && fx < 0; dy++) for (int dx = -r; dx <= r && fx < 0; dx++)
                if (FLOOR_AT(c0x + dx, c0y + dy)) { fx = c0x + dx; fy = c0y + dy; }
        if (fx < 0 && !not_under) {
            /* 2026-11-20 (user: "still not drawing under building shader here"): no cell near him passed the "this
             * building's image is drawn at that map pixel" test (a part outside fl->cells, or no image reference) -
             * the circle without that test (hitbox-free cells only; the GPU still draws it on the building's art) */
            g_floor_noimg = 1; memset(cm, 0, sizeof cm);
            for (int r = 0; r <= 3 && fx < 0; r++)
                for (int dy = -r; dy <= r && fx < 0; dy++) for (int dx = -r; dx <= r && fx < 0; dx++)
                    if (FLOOR_AT(c0x + dx, c0y + dy)) { fx = c0x + dx; fy = c0y + dy; }
        }
        if (g_cfg.log >= 1 && fx < 0 && !not_under) { static DWORD at; if (GetTickCount() - at > 3000) { at = GetTickCount();
            mlog("interior: Sarge %d,%d under a roof (hidden %d%%, %d building cells) - no floor cell within 3 (blocked %d, bhit %d) box %d,%d..%d,%d window %d,%d+%d", sx, sy,
                 g_sarge_hidden, fl->ncells, cell_blocked(sx / IN_CELL, sy / IN_CELL), building_blocked(sx / IN_CELL, sy / IN_CELL), bx0, by0, bx1, by1, wx0, wy0, IN_N); } }
        if (fx >= 0) {
            int qn = 0, qi = 0;
            fl->mask[fy * 2 + (fx >> 5)] |= 1u << (fx & 31);
            qx[qn] = (short)fx; qy[qn] = (short)fy; qn++;
            while (qi < qn) {
                int x = qx[qi], y = qy[qi]; qi++;
                static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
                for (int dd = 0; dd < 4; dd++) {
                    int nx = x + DX[dd], ny = y + DY[dd];
                    if (nx < 0 || ny < 0 || nx >= IN_N || ny >= IN_N) continue;
                    if (fl->mask[ny * 2 + (nx >> 5)] >> (nx & 31) & 1) continue;
                    if (!FLOOR_AT(nx, ny)) continue;
                    fl->mask[ny * 2 + (nx >> 5)] |= 1u << (nx & 31);
                    qx[qn] = (short)nx; qy[qn] = (short)ny; qn++;
                }
            }
        }
        #undef FLOOR_AT
        #undef IN_CIRCLE
        g_floor_noimg = 0;
        fl->circle = !not_under;
        if (not_under) { fl->inside = 0; goto not_inside; }
        /* 2026-11-21 (safeguard; the town / depot cases in Wine had their image among the first keys): the GPU opens
         * the roof only on the images of the first 8 keys (cbuffer tik[8], roofs_interior_note takes them in fl->cells
         * order), and fl->cells came in the order of the remembered-cell table (a hash) - in a town or a base 16 parts
         * (Wine: 3..8 keys of 16 parts), so the image over his floor could miss the 8. Now the parts whose image is drawn on most of the floor
         * cells come first, then the nearest to Sarge (rectangle distance). */
        {
            int hits[IN_MAX_CELLS] = { 0 }, dist[IN_MAX_CELLS];
            for (int y = 0; y < IN_N; y++)
                for (int x = 0; x < IN_N; x++) {
                    if (!(fl->mask[y * 2 + (x >> 5)] >> (x & 31) & 1)) continue;
                    int e = hdworld_entry_at((wx0 + x) * IN_CELL + IN_CELL / 2 - sl, (wy0 + y) * IN_CELL + IN_CELL / 2 - st);
                    if (e < 0) continue;
                    for (int i = 0; i < fl->ncells; i++) if (hdworld_cell_has_entry(fl->cells[i], e)) hits[i]++;
                }
            for (int i = 0; i < fl->ncells; i++) {
                const int *w = CELL_WRECT(fl->cells[i]);
                int dx = sx < w[0] ? w[0] - sx : sx >= w[2] ? sx - w[2] + 1 : 0, dy = sy < w[1] ? w[1] - sy : sy >= w[3] ? sy - w[3] + 1 : 0;
                dist[i] = dx + dy;
            }
            for (int i = 0; i < fl->ncells; i++) {               /* selection sort, 16 at most */
                int b = i;
                for (int j = i + 1; j < fl->ncells; j++) if (hits[j] > hits[b] || (hits[j] == hits[b] && dist[j] < dist[b])) b = j;
                if (b == i) continue;
                BYTE *tc = fl->cells[i]; fl->cells[i] = fl->cells[b]; fl->cells[b] = tc;
                int th = hits[i]; hits[i] = hits[b]; hits[b] = th;
                int td = dist[i]; dist[i] = dist[b]; dist[b] = td;
            }
            if (g_cfg.log >= 2) { static DWORD at3; if (GetTickCount() - at3 > 2000) { at3 = GetTickCount();
                mlog("interior: parts by floor cells shown: %d %d %d %d %d %d (of %d)", hits[0], fl->ncells > 1 ? hits[1] : 0, fl->ncells > 2 ? hits[2] : 0,
                     fl->ncells > 3 ? hits[3] : 0, fl->ncells > 4 ? hits[4] : 0, fl->ncells > 5 ? hits[5] : 0, fl->ncells); } }
        }
        goto circle_done;
        /* 2026-11-20 (user: "the room highlight is still wrong - needs to be the entire room"): the walk mask splits a
         * room (free columns that "see" past the walls failed the four-direction test). Now a SEALED fill: every
         * blocked cell grown by SEAL cells (door gaps up to ~2*SEAL cells close), "outside" flooded in from the
         * window's border over the cells that are not sealed; what it cannot reach is INSIDE the building. The room =
         * the inside piece around Sarge (8-connected through inside cells), shrunk back by SEAL (the growth), holes
         * filled below, grown by one cell at the end. (Before: the four-direction flood; the all-cells variant leaked
         * into a crate-closed alcove.) */
        static BYTE blk[IN_N][IN_N], seal[IN_N][IN_N], outside[IN_N][IN_N], ins[IN_N][IN_N];
        /* sealed with the BUILDING's own hitbox only (its objects' pixel masks; 2026-11-20: crates and yard walls
         * sealed the alcove in front, which then counted as inside) */
        g_nbowners = 0;
        for (int i = 0; i < fl->ncells && g_nbowners < IN_MAX_CELLS; i++) if (CELL_OWNER(fl->cells[i])) g_bowners[g_nbowners++] = CELL_OWNER(fl->cells[i]);
        for (int y = 0; y < IN_N; y++)
            for (int x = 0; x < IN_N; x++) {
                int ax = wx0 + x, ay = wy0 + y;
                blk[y][x] = ax >= bx0 && ax <= bx1 && ay >= by0 && ay <= by1 && building_blocked(ax, ay);
            }
        for (int y = 0; y < IN_N; y++)
            for (int x = 0; x < IN_N; x++) {
                int any = 0;
                for (int dy = -SEAL; dy <= SEAL && !any; dy++) for (int dx = -SEAL; dx <= SEAL && !any; dx++) {
                    int u = x + dx, v = y + dy;
                    any = u >= 0 && v >= 0 && u < IN_N && v < IN_N && blk[v][u];
                }
                seal[y][x] = (BYTE)any;
            }
        memset(outside, 0, sizeof outside);
        {
            int qn = 0, qi = 0;
            for (int k = 0; k < IN_N; k++) {
                int px[4] = { k, k, 0, IN_N - 1 }, py[4] = { 0, IN_N - 1, k, k };
                for (int e = 0; e < 4; e++) if (!seal[py[e]][px[e]] && !outside[py[e]][px[e]]) { outside[py[e]][px[e]] = 1; qx[qn] = (short)px[e]; qy[qn] = (short)py[e]; qn++; }
            }
            while (qi < qn) {
                int x = qx[qi], y = qy[qi]; qi++;
                static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
                for (int dd = 0; dd < 4; dd++) {
                    int nx = x + DX[dd], ny = y + DY[dd];
                    if (nx < 0 || ny < 0 || nx >= IN_N || ny >= IN_N || outside[ny][nx] || seal[ny][nx]) continue;
                    outside[ny][nx] = 1; qx[qn] = (short)nx; qy[qn] = (short)ny; qn++;
                }
            }
        }
        /* inside = not outside; shrunk back by SEAL (an inside cell needs inside cells all around within SEAL) */
        for (int y = 0; y < IN_N; y++)
            for (int x = 0; x < IN_N; x++) {
                int all = !outside[y][x];
                for (int dy = -SEAL; dy <= SEAL && all; dy++) for (int dx = -SEAL; dx <= SEAL && all; dx++) {
                    int u = x + dx, v = y + dy;
                    all = u >= 0 && v >= 0 && u < IN_N && v < IN_N && !outside[v][u];
                }
                ins[y][x] = (BYTE)all;
            }
        /* ALL inside pieces of the building, joined (user: "these should all be part of the same highlight - just
         * combine them inside the building"): closed by SEAL (grown, then shrunk) so pieces up to ~2*SEAL cells apart
         * become one shape; holes filled below. Only when Sarge is in one of them (found above). */
        static BYTE grow[IN_N][IN_N];
        for (int y = 0; y < IN_N; y++)
            for (int x = 0; x < IN_N; x++) {
                int any = 0;
                for (int dy = -JOIN; dy <= JOIN && !any; dy++) for (int dx = -JOIN; dx <= JOIN && !any; dx++) {
                    int u = x + dx, v = y + dy;
                    any = u >= 0 && v >= 0 && u < IN_N && v < IN_N && ins[v][u] && !blk[v][u] && !cell_blocked(wx0 + u, wy0 + v);   /* WALKABLE inside cells */
                }
                grow[y][x] = (BYTE)(any && !outside[y][x]);
            }
        for (int y = 0; y < IN_N; y++)
            for (int x = 0; x < IN_N; x++) {
                int all = grow[y][x];
                for (int dy = -JOIN; dy <= JOIN && all; dy++) for (int dx = -JOIN; dx <= JOIN && all; dx++) {
                    int u = x + dx, v = y + dy;
                    all = u >= 0 && v >= 0 && u < IN_N && v < IN_N && grow[v][u];
                }
                /* joined only INSIDE (not into the sealed wall band / front faces) */
                comp[y][x] = (BYTE)((all && ins[y][x]) || (ins[y][x] && !blk[y][x] && !cell_blocked(wx0 + x, wy0 + y)));
            }
        {   /* holes filled: free tiles inside the building (rooms the walk map leaves open) belong to the shape -
             * everything not reachable from the window's border around it */
            static BYTE out[IN_N][IN_N];
            memset(out, 0, sizeof out);
            int qn = 0, qi = 0;
            for (int k = 0; k < IN_N; k++) {
                int px[4] = { k, k, 0, IN_N - 1 }, py[4] = { 0, IN_N - 1, k, k };
                for (int e = 0; e < 4; e++) if (!comp[py[e]][px[e]] && !out[py[e]][px[e]]) { out[py[e]][px[e]] = 1; qx[qn] = (short)px[e]; qy[qn] = (short)py[e]; qn++; }
            }
            while (qi < qn) {
                int x = qx[qi], y = qy[qi]; qi++;
                static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
                for (int dd = 0; dd < 4; dd++) {
                    int nx = x + DX[dd], ny = y + DY[dd];
                    if (nx < 0 || ny < 0 || nx >= IN_N || ny >= IN_N || out[ny][nx] || comp[ny][nx]) continue;
                    out[ny][nx] = 1; qx[qn] = (short)nx; qy[qn] = (short)ny; qn++;
                }
            }
            for (int y = 0; y < IN_N; y++) for (int x = 0; x < IN_N; x++) if (!out[y][x]) comp[y][x] = 1;
        }
        for (int y = 1; y < IN_N - 1; y++)                       /* grown by one cell: one solid shape up to the walls */
            for (int x = 1; x < IN_N - 1; x++) {
                int any = 0;
                for (int dy = -1; dy <= 1 && !any; dy++) for (int dx = -1; dx <= 1 && !any; dx++) any = comp[y + dy][x + dx] != 0;
                if (any) fl->mask[y * 2 + (x >> 5)] |= 1u << (x & 31);
            }
        /* the doors run straight out through the wall (user: "just needs to not round off the edge from the door
         * exit in the back"): door cells and the free outside cells up to EXIT_REACH beyond them join the shape -
         * outside they are ground pixels, which the shading leaves alone, so the opening reads cut through */
        for (int y = 0; y < IN_N; y++)
            for (int x = 0; x < IN_N; x++) {
                if (!(fl->door[y * 2 + (x >> 5)] >> (x & 31) & 1)) continue;
                if (!(fl->mask[y * 2 + (x >> 5)] >> (x & 31) & 1)) continue;   /* only doors of HIS room (a stray blob before) */
                static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
                for (int dd = 0; dd < 4; dd++)
                    for (int k = 1; k <= EXIT_REACH; k++) {
                        int u = x + DX[dd] * k, v = y + DY[dd] * k;
                        if (u < 0 || v < 0 || u >= IN_N || v >= IN_N || cell_blocked(wx0 + u, wy0 + v) || cell_room(wx0 + u, wy0 + v)) break;
                        fl->mask[v * 2 + (u >> 5)] |= 1u << (u & 31);
                    }
            }
    circle_done:
        fl->inside = 1;
    } else if (ndoor) {                                          /* near: a patch over each entrance close to HIM */
        /* 2026-11-20 (user: "weird drawing in weird places ... needs to go off hitbox and not object square"): near =
         * a DOOR cell (the walkable gap in the hitbox) within NEAR_DOOR cells of Sarge, not his distance to the
         * building's picture rectangle (big buildings have big pictures) */
        int scx0 = sx / IN_CELL - wx0, scy0 = sy / IN_CELL - wy0, close = 0;
        for (int y = max(0, scy0 - NEAR_DOOR); y <= min(IN_N - 1, scy0 + NEAR_DOOR); y++)
            for (int x = max(0, scx0 - NEAR_DOOR); x <= min(IN_N - 1, scx0 + NEAR_DOOR); x++)
                if ((fl->door[y * 2 + (x >> 5)] >> (x & 31) & 1) && back_door(wx0 + x, wy0 + y)) close = 1;
        if (!close) ndoor = 0;
        for (int y = 0; y < IN_N && close; y++)
            for (int x = 0; x < IN_N; x++) {
                if (abs(x - scx0) > NEAR_DOOR + DOOR_GROW || abs(y - scy0) > NEAR_DOOR + DOOR_GROW) continue;
                if (!cell_room(wx0 + x, wy0 + y)) continue;   /* the threshold, room side (user: "just the threshold") */
                int near_door = 0;
                for (int dy = -DOOR_GROW; dy <= DOOR_GROW && !near_door; dy++)
                    for (int dx = -DOOR_GROW; dx <= DOOR_GROW && !near_door; dx++) {
                        int u = x + dx, v = y + dy;
                        if (u >= 0 && v >= 0 && u < IN_N && v < IN_N && (fl->door[v * 2 + (u >> 5)] >> (u & 31) & 1) && back_door(wx0 + u, wy0 + v)) near_door = 1;
                    }
                if (near_door) fl->mask[y * 2 + (x >> 5)] |= 1u << (x & 31);
            }
    }
not_inside:
    if (g_cfg.log >= 2) { static DWORD at; if (GetTickCount() - at > 2000) { at = GetTickCount(); { int nm = 0; for (int q = 0; q < IN_N * 2; q++) for (unsigned v = fl->mask[q]; v; v &= v - 1) nm++;
        mlog("interior: Sarge %d,%d hidden %d%%: %d building cell(s), %d door cells, inside %d, floor cells %d", sx, sy, g_sarge_hidden, fl->ncells, ndoor, fl->inside, nm); } } }
    return fl->inside || ndoor > 0;
}
#undef BIT_SET
#undef BIT_GET

void roofs_interior_note(TargetZones *z, int cx, int cy)
{
    static Floor last;                  /* kept while fading out, and between recomputations */
    static int have, last_on, last_scx = -1, last_scy = -1;
    static float s;
    static DWORD t_last, computed_at;
    DWORD now = clock_game_ms();
    float dt = t_last && now >= t_last && now - t_last < 500 ? (float)(now - t_last) / IN_FADE_MS : 0;
    t_last = now;
    z->in_s = 0; z->in_nkey = 0;
    if (!g_cfg.see_interior || !g_cfg.see_through || !layout_view_is_gameplay()) { s = 0; have = 0; last_scx = -1; return; }
    BYTE *sg = *(BYTE **)0x697520;
    int scx = sg ? *(short *)(sg + 4) / IN_CELL : -1, scy = sg ? *(short *)(sg + 6) / IN_CELL : -1;
    int on = last_on;
    if (scx != last_scx || scy != last_scy || GetTickCount() - computed_at > 400) {
        static Floor fl;
        static int faults;
        on = 0;
        if (faults < 3) {
            Guard g;
            volatile int got = 0;
            if (GUARD_TRY(g, "building interior (roofs.c)")) { got = floor_now(&fl); guard_leave(&g); }
            else faults++;
            on = got;
        }
        if (on) { last = fl; have = 1; }
        last_on = on; last_scx = scx; last_scy = scy; computed_at = GetTickCount();
    }
    s += on ? dt : -dt;
    s = s < 0 ? 0 : s > 1 ? 1 : s;
    if (!have || s <= 0) {                                       /* Sarge not inside: only enemy circles */
        memset(z->in_mask, 0, sizeof z->in_mask); memset(z->in_door, 0, sizeof z->in_door);
        z->in_s = 0; z->in_cr = 0; z->in_nkey = 0;
        goto enemies;
    }
    int nk = 0;                          /* keys of the building images (as objectives.c straddlers) */
    for (int i = 0; i < last.ncells && nk < 8; i++) {
        BYTE *c = last.cells[i];
        if (IsBadReadPtr(c, CELL_BYTES)) continue;
        for (int idx = 0; idx < 8 && nk < 8; idx++) {
            int hx, hy, sl, wl, wt;
            int got = hdworld_cell_image(c, idx, &hx, &hy, &sl, &wl, &wt);
            if (got < 0) break;
            if (!got) continue;
            z->in_key[nk][0] = (hx - (wl - cx)) & 0x7ff; z->in_key[nk][1] = (hy - (wt - cy)) & 0x7ff; z->in_key[nk][2] = sl;
            nk++;
        }
    }
    z->in_nkey = nk;                     /* Sarge's building keys first */
enemies:
    {
    int nk = z->in_nkey;
    /* enemies firing from under a roof (user 2026-11-20: "draw one around the enemy once they start shooting"): a
     * circle on their building's texture (keys appended) */
    z->in_nen = 0;
    for (int i = 1; i < MAX_FIGS && z->in_nen < 2; i++) {
        Fig *f = &g_figs[i];
        if (!f->owner || !f->shown_at || GetTickCount() - f->shown_at > 400 || IsBadReadPtr(f->owner, 8)) continue;
        int ex = *(short *)(f->owner + 4), ey = *(short *)(f->owner + 6);
        int added = 0;
        for (int k = 0; k < CELLS && nk < 8; k++) {
            BYTE *c = live_cell(k);
            if (!c || !building_cell(c)) continue;
            const int *w = CELL_WRECT(c);
            if (ex < w[0] || ex >= w[2] || ey < w[1] || ey >= w[3]) continue;
            for (int idx = 0; idx < 8 && nk < 8; idx++) {
                int hx, hy, sl, wl, wt;
                int got = hdworld_cell_image(c, idx, &hx, &hy, &sl, &wl, &wt);
                if (got < 0) break;
                if (!got) continue;
                z->in_key[nk][0] = (hx - (wl - cx)) & 0x7ff; z->in_key[nk][1] = (hy - (wt - cy)) & 0x7ff; z->in_key[nk][2] = sl;
                nk++; added = 1;
            }
        }
        if (!added) continue;
        z->in_en[z->in_nen][0] = ex - cx; z->in_en[z->in_nen][1] = ey - 8 - cy; z->in_en[z->in_nen][2] = ENEMY_CIRCLE;
        z->in_nen++;
    }
    z->in_nkey = nk;
    }
    if (!have || s <= 0) return;
    z->in_s = s;
    z->in_ox = last.cx0 * IN_CELL - cx; z->in_oy = last.cy0 * IN_CELL - cy;
    memcpy(z->in_mask, last.mask, sizeof z->in_mask);
    {   BYTE *sg2 = *(BYTE **)0x697520;
        z->in_cr = last.inside && last.circle && sg2 ? (CIRCLE_CELLS * IN_CELL) : 0;
        z->in_sx = sg2 ? *(short *)(sg2 + 4) - cx : 0; z->in_sy = sg2 ? *(short *)(sg2 + 6) - 8 - cy : 0; }
    memcpy(z->in_door, last.door, sizeof z->in_door);
}
