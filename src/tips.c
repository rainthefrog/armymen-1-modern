/* ============================================================================================
 * tips.c - tooltips on the HUD: name and key of the box under the pointer
 *
 * While the pointer rests (TIP_DELAY_MS) on a part of the HUD panel during gameplay, a small box
 * with what it is and its keys is drawn next to the pointer, into the finished frame (after the
 * corner zones are enlarged, so the tip itself stays 1x). Zones are in the HUD's 640x480 design
 * coordinates (the same ones controls.c / weapons.c use for clicks); layout_pointer_to_game maps the
 * pointer there, including the split panel and the enlarged corners.
 * Texts follow the modern key layout (controls.c kb_get_state); classic mode shows the game's keys.
 * ============================================================================================ */
#include "common.h"

#define TIP_DELAY_MS 350

static RECT g_tip;                     /* last tooltip drawn, and when (the scroller keeps it still) */
static DWORD g_tip_at;

typedef struct { short x0, y0, x1, y1; const char *modern, *classic; } Zone;

static const Zone ZONES[] = {
    /* weapon column A (rows of 23 from y 399; the B column is handled with weapons_ui_at) */
    { 243, 397, 317, 420, "Main weapon  [1]", "Main weapon [1]" },
    { 243, 420, 317, 441, "Heavy weapon  [2]", "Heavy weapon [2]" },
    { 243, 441, 317, 465, "Special item  [3]", "Special item [3]" },
    /* health box */
    { 322, 386, 432, 412, "Sarge health", "Sarge health" },
    { 322, 412, 432, 434, "Vehicle health", "Vehicle health" },
    /* squad box: unit box, then FOLLOW / ATTACK / DEFEND rows (13 high from y 435) */
    { 322, 438, 373, 474, "Next unit  [Tab]", "Next unit  [Tab]" },
    { 372, 435, 430, 448, "Follow  [F1]", "Follow  [F1]" },
    { 372, 448, 430, 461, "Attack  [F2]", "Attack  [F2]" },
    { 372, 461, 430, 474, "Defend  [F3]", "Defend  [F3]" },
    /* air support rows (23 high from y 404) */
    { 437, 404, 504, 427, "Air strike  [F5]", "Air strike  [F5]" },
    { 437, 427, 504, 450, "Paratroopers  [F6]", "Paratroopers  [F6]" },
    { 437, 450, 504, 473, "Recon  [F7]", "Recon  [F7]" },
    /* minimap and the item box */
    { 523, 389, 609, 475, "Map  [M]", "Map  [V]" },
    { 186, 386, 242, 440, "Mission items", "Mission items" },
};
#define NZONES ((int)(sizeof ZONES / sizeof ZONES[0]))

static const char *tip_at(int sx, int sy)
{
    RECT r;
    POINT p = { sx, sy };
    if (objectives_box(&r) && PtInRect(&r, p))
        return g_cfg.controls_modern ? "Briefing  [J]" : "Briefing  [O]";
    if (layout_resize_icon_at(sx, sy) >= 0) return "Resize";
    if (weapons_ui_at(sx, sy))
        return "Spare weapon";
    if (!layout_on_hud_panel(sx, sy)) return NULL;
    int gx, gy;
    layout_pointer_to_game(sx, sy, &gx, &gy);
    for (int i = 0; i < NZONES; i++)
        if (gx >= ZONES[i].x0 && gx < ZONES[i].x1 && gy >= ZONES[i].y0 && gy < ZONES[i].y1)
            return g_cfg.controls_modern ? ZONES[i].modern : ZONES[i].classic;
    return NULL;
}

void tips_draw(Surf *b)
{
    static const char *last;
    static DWORD since;
    int sx, sy, font, colour;
    if (!b || !b->bits || !layout_view_is_gameplay() || panel_is_open() || !controls_pointer(&sx, &sy)) { last = NULL; return; }
    if (minimap_hovering()) { last = NULL; return; }          /* minimap.c shows that blip's details instead */
    const char *t = tip_at(sx, sy);
    DWORD now = GetTickCount();
    if (t != last) { last = t; since = now; }
    if (!t || now - since < TIP_DELAY_MS) return;
    if (!objectives_font(&font, &colour) || !hud_draw_string_original) return;
    int w = (int)strlen(t) * 8 + 16, h = 15;          /* the font is ~8 px a character (6 cut texts off) */
    int x0 = sx + 12, y0 = sy - h - 6;                     /* above-right of the pointer, kept on screen */
    if (x0 + w > b->w) x0 = b->w - w;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = sy + 18;
    if (y0 + h > b->h) y0 = b->h - h;
    BYTE *shade = *(BYTE **)0x4e2448;                      /* x0.40 shade table */
    for (int y = y0; y < y0 + h; y++) {
        BYTE *row = b->bits + (size_t)y * b->pitch;
        for (int x = x0; x < x0 + w && x < b->w; x++) {
            int edge = y == y0 || y == y0 + h - 1 || x == x0 || x == x0 + w - 1;
            row[x] = edge ? (BYTE)colour : shade ? shade[shade[row[x]]] : row[x];
        }
    }
    g_tip = (RECT){ x0, y0, x0 + w, y0 + h }; g_tip_at = now;
    int clip[2] = { w - 8, h - 2 };
    hud_draw_string_original(b, x0 + 5, y0 + 2, t, font, clip, colour);
}

int tips_rect(RECT *r) { if (GetTickCount() - g_tip_at > 150) return 0; *r = g_tip; return 1; }
