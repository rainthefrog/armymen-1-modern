/* ============================================================================================
 * fog.c - darkness over the edge of the map
 *
 * Only the_map.boundrect (the map inset by 14 tiles, 0x4e2ff4) is meant to be seen: the original
 * camera never shows anything outside it, and map makers parked objects and pickup boxes in the
 * border. With wide screens the border can come into view at the left and right, so
 * the finished battlefield is darkened from FADE_IN pixels inside boundrect to fully dark FADE_OUT
 * pixels outside it.
 * Bottom edge on wide screens: the camera may show up to the HUD panel's 94 rows past boundrect
 * (controls_map_bottom_room, layout.c) - rows seen only through the HUD's gaps, meant to cover the
 * window down to its bottom (user 2026-11-21: no dark / black strip there). The bottom fade is moved
 * below the lowest row the camera can show, so it never darkens them.
 *
 * Darkening uses the exe's own shade tables (index -> index remaps on the game palette, built by
 * FUN_00403b30; pointers at 0x4e2448..0x4e2454 = x0.40 / x0.50 / x0.60 / x0.85), composed into
 * deeper levels, with an ordered dither between levels so the fade is smooth.
 * Battlefield back-buffer pixel (x, y) is world (x + screenrect.left, y + screenrect.top)
 * (screenrect 0x4e2fd4 includes screen shake).
 * [display] map_edge_fog = 1
 * ============================================================================================ */
#include "common.h"

#define SCREEN_L   (*(int *)0x4e2fd4)          /* the_map.screenrect: world rect of the battlefield */
#define SCREEN_T   (*(int *)0x4e2fd8)
#define SCREEN_R   (*(int *)0x4e2fdc)
#define SCREEN_B   (*(int *)0x4e2fe0)
#define BOUND_L    (*(int *)0x4e2ff4)          /* the_map.boundrect */
#define BOUND_T    (*(int *)0x4e2ff8)
#define BOUND_R    (*(int *)0x4e2ffc)
#define BOUND_B    (*(int *)0x4e3000)
#define SHADE(i)   (*(BYTE **)(0x4e2448 + 4 * (i)))   /* x0.40, x0.50, x0.60, x0.85 */

#define FADE_IN    24                          /* fog starts this far inside the visible area */
#define FADE_OUT   72                          /* and is fully dark this far outside it */
#define LEVELS     7                           /* 0 = clear .. 6 = darkest */

static BYTE g_level[LEVELS][256];
static BYTE *g_built_from;

static const double FACTOR[LEVELS] = { 1.0, 0.82, 0.64, 0.48, 0.34, 0.2, 0.08 };
static DWORD g_pal_sum;

/* Fog levels straight from the palette: each colour darkened by the level's factor, then the
 * nearest palette entry (by brightness-weighted distance). Applying the game's shade tables two or
 * three times drifted dark colours toward greens (user: the fade looked green). */
static int build_levels(void)
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    if (!p) return 0;
    DWORD sum = 0;
    for (int i = 0; i < 256; i++) sum = sum * 31 + (p->e[i].peRed << 16 | p->e[i].peGreen << 8 | p->e[i].peBlue);
    if (g_built_from && sum == g_pal_sum) return 1;
    for (int lv = 0; lv < LEVELS; lv++)
        for (int i = 0; i < 256; i++) {
            if (!lv) { g_level[0][i] = (BYTE)i; continue; }
            int r = (int)(p->e[i].peRed * FACTOR[lv]), g = (int)(p->e[i].peGreen * FACTOR[lv]), b = (int)(p->e[i].peBlue * FACTOR[lv]);
            int best = i, best_d = 1 << 30;
            for (int j = 0; j < 256; j++) {
                int dr = p->e[j].peRed - r, dg = p->e[j].peGreen - g, db = p->e[j].peBlue - b;
                int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
                if (d < best_d) { best_d = d; best = j; }
            }
            g_level[lv][i] = (BYTE)best;
        }
    g_pal_sum = sum;
    g_built_from = (BYTE *)1;
    return 1;
}

static const BYTE BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* Fog level (0..LEVELS-1, dithered) for a signed distance d outside the visible area. */
static int level_at(int d, int x, int y)
{
    if (d <= -FADE_IN) return 0;
    if (d >= FADE_OUT) return LEVELS - 1;
    int f = (d + FADE_IN) * (LEVELS - 1) * 16 / (FADE_IN + FADE_OUT);   /* 16ths of a level */
    int lv = f >> 4;
    if ((f & 15) > BAYER[y & 3][x & 3]) lv++;
    return lv < LEVELS ? lv : LEVELS - 1;
}

void fog_draw(Surf *b)
{
    if (!g_cfg.map_edge_fog || !b || !b->bits || !layout_view_is_gameplay()) return;
    int bl = BOUND_L, bt = BOUND_T, br = BOUND_R, bb = BOUND_B;
    if (br - bl < 64 || bb - bt < 64) return;
    int room = controls_map_bottom_room();      /* see the header: bottom on wide screens */
    if (room) bb += room + FADE_IN;
    if (!build_levels()) return;
    int w = SCREEN_R - SCREEN_L, h = SCREEN_B - SCREEN_T;
    if (w > b->w) w = b->w;
    if (h > b->h) h = b->h;
    int ox = SCREEN_L, oy = SCREEN_T;
    /* nothing to do when the whole view is well inside */
    if (ox >= bl + FADE_IN && oy >= bt + FADE_IN && ox + w <= br - FADE_IN && oy + h <= bb - FADE_IN) return;
    for (int y = 0; y < h; y++) {
        int wy = y + oy;
        int dy = bt - wy > wy - (bb - 1) ? bt - wy : wy - (bb - 1);
        BYTE *row = b->bits + (size_t)y * b->pitch;
        int x0 = 0, x1 = w;
        if (dy <= -FADE_IN) {                    /* row inside: only the left and right bands */
            int left_end = bl + FADE_IN - ox, right_start = br - FADE_IN - ox;
            for (int x = 0; x < w && x < left_end; x++) {
                int d = bl - (x + ox);
                int lv = level_at(d, x, y);
                if (lv) row[x] = g_level[lv][row[x]];
            }
            for (int x = right_start > 0 ? right_start : 0; x < w; x++) {
                int d = (x + ox) - (br - 1);
                int lv = level_at(d, x, y);
                if (lv) row[x] = g_level[lv][row[x]];
            }
            continue;
        }
        for (int x = x0; x < x1; x++) {
            int wx = x + ox;
            int dx = bl - wx > wx - (br - 1) ? bl - wx : wx - (br - 1);
            int lv = level_at(dx > dy ? dx : dy, x, y);
            if (lv) row[x] = g_level[lv][row[x]];
        }
    }
}
