/* ============================================================================================
 * modmenu.c - the mod's settings as a screen of the GAME'S OWN menus (2026-11-17, user: "the settings
 * UI is drawn in a new window - incorporate it into the game menus, main menu and pause menu")
 *
 * The game builds its menu screens from static definition tables (SCREENDEF -> SCREENOBJDEF ->
 * SCREENBTNDEF, see tooling/docs/game-menus.md) and runs them through a 21-entry state table. This file:
 *   - adds a "MOD SETTINGS" text button to the PAUSE menu (Game Options, object 0x4c6f70, free slot 3 at
 *     y 180) and the MAIN menu (object 0x4c6bb0, free slot 4 at y 188): our button definitions, copied
 *     from the game's own buttons next to them, written into the free slots (data patch only);
 *   - takes over screen state 14 (SCRN_PLAY_ABOUT_STATE, unreachable in the shipped exe): its init is
 *     ours (mm_init), its terminate the game's ScrnTerminateScreen; ScrnOptionsVisible (0x468510,
 *     rewritten here) also answers TRUE for it, so the game draws / updates it during play;
 *   - builds the screen like the game's Audio Options: the large panel (panel.bmp 480x340) with the
 *     title, rows of label + ON / OFF buttons (copies of the SFX ON / OFF buttons), value buttons for
 *     game speed / zoom / minimap size, page tabs (GENERAL / MODERN 1 / MODERN 2) and BACK.
 * All definitions are copied at run time from the game's own (fonts, colours, icons stay the game's)
 * and only position / text / callback / data are changed. Callbacks: LONG __fastcall cb(button); 1 =
 * the screen changed (the game stops its button loop). The switches themselves are panel.c's
 * (panel_row_*) and modern.c's (modern_on / modern_toggle), saved to the ini as before.
 * ============================================================================================ */
#include "common.h"
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- the game's menu code (verified) */
#define SCRN_FULL          (*(BYTE **)0x6a5944)        /* SCRNfullScreen: the current screen */
#define STATE_TABLE        0x4d3558                    /* 21 x { state, init, display, update, terminate } */
#define MODSET_STATE       14                          /* SCRN_PLAY_ABOUT_STATE, unused by the exe */
#define STATE_PAUSE        11                          /* SCRN_PLAY_OPTIONS_STATE: Game Options */
#define STATE_MAIN         1                           /* SCRN_GAME_START_STATE: the main menu */
#define TERMINATE_SCREEN   0x464740                    /* ScrnTerminateScreen */
#define GAME_PLAY_STATE    5
typedef BYTE *(__fastcall *ScrnCreateFn)(const void *pos, int attrs);
typedef void (__fastcall *ScrnLoadDefFn)(BYTE *screen, const void *def);
typedef void (__fastcall *ScrnSetStateFn)(int state);
typedef int (__cdecl *ScrnGetStateFn)(void);
typedef BOOL (__fastcall *SBtnSetStateFn)(BYTE *btn, int state);
typedef BOOL (__fastcall *SBtnSetTextFn)(BYTE *btn, const char *text);
typedef void (__fastcall *SetPathFn)(const void *path);
typedef void (__fastcall *ShowCursorFn)(int show);
typedef void (__cdecl *EraseFn)(void);
typedef void (__fastcall *SetUIOverrideFn)(void *frame, int edx, void *ui);   /* thiscall, 1 stack arg */
typedef int (__fastcall *GameStateFn)(void *game, int edx);                  /* thiscall */
#define SCRN_CREATE    ((ScrnCreateFn)0x4626f0)
#define SCRN_LOAD_DEF  ((ScrnLoadDefFn)0x462e70)
#define SCRN_SET_STATE ((ScrnSetStateFn)0x462680)
#define SCRN_GET_STATE ((ScrnGetStateFn)0x4626e0)
#define SBTN_SET_STATE ((SBtnSetStateFn)0x45e370)
#define SBTN_SET_TEXT  ((SBtnSetTextFn)0x45e6f0)
#define SET_PATH       ((SetPathFn)0x420e40)
#define SCREEN_PATH    ((const void *)0x4abbd0)
#define SHOW_CURSOR    ((ShowCursorFn)0x420610)
#define ERASE_SURFACES ((EraseFn)0x406250)
#define SET_UI_OVERRIDE ((SetUIOverrideFn)0x4089d0)
#define MAIN_FRAME     (*(void **)0x4d80fc)
#define GAME_GET_STATE ((GameStateFn)0x423d00)
#define GAME           (*(void **)0x4b3f70)

/* runtime structs (offsets verified in the disassembly) */
#define SCREEN_UPDATE(s)   (*(int *)((s) + 0x10))
#define SCREEN_OBJ(s, i)   (((BYTE **)((s) + 0x70))[i])
#define OBJ_UPDATE(o)      (*(int *)((o) + 0x9c))
#define OBJ_BTN(o, i)      (((BYTE **)((o) + 0xe8))[i])
#define BTN_ATTRS(b)       (*(DWORD *)((b) + 0x00))
#define BTN_DATA(b)        (*(DWORD *)((b) + 0x38))
#define BTN_UPDATE(b)      (*(int *)((b) + 0x3c))
#define BUTTON_NULL 0
#define BUTTON_FOCUSED 1
#define BUTTON_CLICKED 2
#define SBTN_CLICK_BYPASS 0x800

/* definition sizes / offsets (exe data) */
#define BTNDEF_SIZE  0x140   /* +0x20 position, +0x28 text, +0x138 callback, +0x13c dwData */
#define OBJDEF_SIZE  0x40    /* +4 position, +8 title, +0xc image, +0x10 area, +0x14 count, +0x18 buttons */
#define TITLE_SIZE   24      /* position, font, bg, fg, resource, text */
#define AREA_SIZE    16      /* box (4 shorts), colour, shade */
#define SCRDEF_HDR   0x5c    /* +0x58 object count, +0x5c objects */
#define MAX_OBJS     24

/* the game's definitions we copy (verified) */
#define DEF_PAUSE_OBJ     0x4c6f70   /* Game Options panel object (3 buttons) */
#define DEF_MAIN_OBJ      0x4c6bb0   /* main menu panel object (4 buttons) */
#define DEF_QUIT_OBJ      0x4c6bf0   /* main menu Quit object (290,310) */
#define DEF_AUDIO_BTN     0x4cb1a0   /* pause menu "Audio" text button */
#define DEF_OPTIONS_BTN   0x4c94e0   /* main menu "Options" text button (highlight style) */
#define DEF_AUDIO_PANEL   0x4c74f0   /* Audio Options panel object (title, smlpanel) */
#define DEF_AUDIO_ROW     0x4c7770   /* SFX row object (label + ON / OFF) */
#define DEF_SFX_ON        0x4cd0e0
#define DEF_SFX_OFF       0x4cd220
#define DEF_AUDIO_BACKOBJ 0x4c72b0   /* in-game Audio BACK object */
#define DEF_PLAY_SCREEN   0x4d27d8   /* in-game Audio screen (transparent, no picture) */
#define DEF_MENU_SCREEN   0x4d1a58   /* Options screen (Title2.bmp) */

/* ---------------------------------------------------------------- layout (640x480 design) */
#define PANEL_X   80
#define PANEL_Y   12                 /* everything above y 360 (layout.c: lower rows belong to the HUD) */
#define PANEL_W   480
#define PANEL_H   340
#define ROW_X0    (PANEL_X + 10)
#define ROW_Y0    (PANEL_Y + 54)
#define COL_W     232
#define ROW_H     26
#define ROWS_PER_COL 9
#define LABEL_X   8
#define ON_X      150
#define OFF_X     190
#define NAV_Y     (PANEL_Y + PANEL_H - 44)
#define LABEL_COLOUR 15                                         /* SBTN_WHITE (the game's CYAN is dark here) */
#define DD_COPY_BACK_TO_FRONT ((void (__cdecl *)(void))0x406310)   /* clears a menu drawn over the paused game */

enum { K_ON = 1, K_OFF, K_CYCLE, K_TAB, K_BACK, K_OPEN };
#define DATA(kind, a, b) ((DWORD)(kind) << 24 | (DWORD)(a) << 12 | (DWORD)(b))
#define D_KIND(d)  ((int)((d) >> 24))
#define D_A(d)     ((int)(((d) >> 12) & 0xfff))
#define D_B(d)     ((int)((d) & 0xfff))

/* rows: source 0 = panel.c row (index = its place in panel.c g_rows), 1 = modern.c feature, 2 = cycle (0 speed,
 * 1 zoom, 2 minimap ...). Pages = logical groups (user 2026-11-19: "consolidated into logical grouping and have
 * toggles in the mod settings"): GENERAL = picture and interface, COMBAT = the shooting / damage rules
 * (bullets.c groups 1-4, health), MODERN 1 / 2 = modern.c features. At most 18 rows a page (2 x ROWS_PER_COL). */
typedef struct { const char *label; int source, index; } Item;
static const Item GENERAL[] = {
    { "Modern controls", 0, 0 }, { "Upscaled art", 0, 1 }, { "CRT filter", 0, 4 }, { "Edge fog", 0, 5 },
    { "See-through roofs", 0, 6 }, { "Enemies in bldgs", 0, 15 }, { "Building interior", 0, 18 }, { "Pinned objective", 0, 7 }, { "Target sector", 0, 8 },
    { "Target outline", 0, 10 }, { "HUD in corners", 0, 9 },
    { "Game speed", 2, 0 }, { "Zoom", 2, 1 }, { "Minimap size", 2, 2 }, { "Message box size", 2, 3 },
    { "Centre HUD size", 2, 4 }, { "Air support size", 2, 5 },
};
static const Item COMBAT[] = {
    { "Barricade rules", 0, 3 }, { "Enemies use them", 0, 11 }, { "Corner shots", 0, 12 }, { "Leg hits", 0, 13 },
    { "Downhill hits", 0, 14 }, { "Longer range", 0, 17 }, { "No corner squash", 0, 20 }, { "Health regen", 0, 2 }, { "Half damage", 1, MOD_HALF_DAMAGE },
    { "Aim assist", 1, MOD_AIM_ASSIST }, { "Hit markers", 1, MOD_HIT_MARKERS }, { "Damage direction", 1, MOD_DAMAGE_DIR },
    { "Health bars", 1, MOD_HEALTH_BARS }, { "Enemy vehicles", 0, 23 },
};
static const Item MENUS[] = {
    { "Sharp menu text", 0, 19 }, { "Buttons press in", 0, 16 }, { "Radio overlay", 0, 21 },
};
static const Item MODERN1[] = {
    { "Dodge roll key", 1, MOD_ROLL }, { "Sprint stamina", 1, MOD_STAMINA },
    { "Side btn weapons", 1, MOD_SIDE_BUTTONS }, { "Sprint toggle", 1, MOD_SPRINT_TOGGLE },
    { "Off-screen arrows", 1, MOD_ARROWS }, { "Ammo at reticle", 1, MOD_AMMO },
    { "Range at reticle", 1, MOD_RANGE_RING }, { "Route marker", 1, MOD_ROUTE }, { "Mission path", 0, 22 }, { "Edge peek key", 1, MOD_PEEK },
    { "No vehicle zoom", 1, MOD_VEHICLE_ZOOM }, { "No screen shake", 1, MOD_NO_SHAKE }, { "Skip videos", 1, MOD_SKIP_VIDEO },
};
static const Item MODERN2[] = {
    { "Stats key", 1, MOD_STATS }, { "Restart key", 1, MOD_RESTART }, { "Gamepad", 1, MOD_GAMEPAD },
    { "Throw at pointer", 1, MOD_THROW_POINTER }, { "Throw on release", 1, MOD_THROW_HOLD }, { "Colour-blind", 1, MOD_COLOURBLIND },
    { "Ctrl+click orders", 1, MOD_SQUAD_MOUSE }, { "Radial menu", 1, MOD_RADIAL }, { "Ping key", 1, MOD_PING },
};
#define N(a) ((int)(sizeof a / sizeof a[0]))
static const struct { const char *tab; const Item *items; int n; } PAGES[] = {
    { "GENERAL", GENERAL, N(GENERAL) }, { "COMBAT", COMBAT, N(COMBAT) }, { "MODERN 1", MODERN1, N(MODERN1) }, { "MODERN 2", MODERN2, N(MODERN2) }, { "MENUS", MENUS, N(MENUS) },
};
#define NPAGES N(PAGES)

static int g_page;                 /* page shown */
static int g_from_play;            /* opened from the pause menu (else from the main menu) */
static int g_ready;                /* templates copied, patches in */

/* ---------------------------------------------------------------- values */

static int item_value(const Item *it)
{
    return it->source == 0 ? panel_row_value(it->index) : it->source == 1 ? modern_on(it->index) : 0;
}
static void item_set(const Item *it, int on)
{
    if (it->source == 0) panel_row_set(it->index, on);
    else if (it->source == 1 && modern_on(it->index) != (on != 0)) modern_toggle(it->index);
}
static char g_vtext[6][16];
static const char *cycle_text(int which)
{
    /* 2..5: HUD group sizes (layout.c groups: minimap 3, message box 0, centre 1, air support 2) */
    static const int GROUP[4] = { 3, 0, 1, 2 };
    double v = which == 0 ? clock_speed_now() : which == 1 ? zoom_target() : layout_group_scale(GROUP[which - 2]);
    int c = (int)(v * 100 + 0.5);
    wsprintfA(g_vtext[which], "%d.%02dx", c / 100, c % 100);
    return g_vtext[which];
}
/* ---------------------------------------------------------------- sliders (2026-11-20)
 * The values that are not on / off (game speed, zoom, HUD group sizes) are sliders: stops = the values the old
 * cycle stepped through; a press on the track (or on the knob) and a drag set the nearest stop. The value is set
 * by the existing cycle functions until it is reached (they also write the ini and keep the HUD sizes that do
 * not fit out). Drawn by menutext.c (track, stops, knob, value) in the label layer. */
#define SL_X0 128
#define SL_X1 190
static const double SL_SPEED[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
static const double SL_ZOOM[] = { 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0 };
static const double SL_HUD[] = { 1.0, 1.25, 1.5, 2.0, 2.5 };
static void cycle(int which);
static double cycle_value(int which)
{
    static const int GROUP[4] = { 3, 0, 1, 2 };
    return which == 0 ? clock_speed_now() : which == 1 ? zoom_target() : layout_group_scale(GROUP[which - 2]);
}
static const double *slider_stops(int which, int *n)
{
    if (which == 0) { *n = 6; return SL_SPEED; }
    if (which == 1) { *n = 7; return SL_ZOOM; }
    *n = 5; return SL_HUD;
}
static int slider_pos(int which)
{
    int n; const double *v = slider_stops(which, &n);
    double cur = cycle_value(which); int best = 0;
    for (int i = 1; i < n; i++) if (fabs(v[i] - cur) < fabs(v[best] - cur)) best = i;
    return best;
}
static void slider_box(int i, RECT *r)
{
    int col = i / ROWS_PER_COL, row = i % ROWS_PER_COL;
    int x = ROW_X0 + col * COL_W, y = ROW_Y0 + row * ROW_H;
    SetRect(r, x + SL_X0, y + 2, x + SL_X1, y + 22);
}
static int g_drag = -1;                      /* page item being dragged */
static void sliders_publish(void)
{
    MtSlider s[MT_SLIDERS]; int n = 0;
    const Item *items = PAGES[g_page].items;
    for (int i = 0; i < PAGES[g_page].n && n < MT_SLIDERS; i++) {
        if (items[i].source != 2) continue;
        memset(&s[n], 0, sizeof s[n]);
        slider_box(i, &s[n].r);
        slider_stops(items[i].index, &s[n].steps);
        s[n].pos = slider_pos(items[i].index);
        s[n].active = g_drag == i;
        lstrcpynA(s[n].value, cycle_text(items[i].index), sizeof s[n].value);
        n++;
    }
    menutext_set_sliders(s, n, MODSET_STATE);
}
static void slider_set(int i, int x)
{
    const Item *it = &PAGES[g_page].items[i];
    RECT r; slider_box(i, &r);
    int n; const double *v = slider_stops(it->index, &n);
    int want = (int)floor((double)(x - r.left) * (n - 1) / (r.right - r.left) + 0.5);
    want = want < 0 ? 0 : want > n - 1 ? n - 1 : want;
    for (int t = 0; t < 12 && fabs(cycle_value(it->index) - v[want]) > 0.01; t++) cycle(it->index);
}
void modmenu_slider_mouse(UINT msg, LPARAM lp)
{
    if (!g_ready || SCRN_GET_STATE() != MODSET_STATE) { g_drag = -1; return; }
    if (msg == WM_MOUSEMOVE && g_drag < 0) return;
    POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
    view_client_to_game(&p);
    int dx = p.x, dy = p.y;
    if (layout_menu_scale()) layout_pointer_to_game(p.x, p.y, &dx, &dy);
    else { RECT v = view_current(); if (v.right > v.left) { dx -= v.left; dy -= v.top; } }
    if (msg == WM_LBUTTONDOWN) {
        const Item *items = PAGES[g_page].items;
        for (int i = 0; i < PAGES[g_page].n; i++) {
            if (items[i].source != 2) continue;
            RECT r; slider_box(i, &r); InflateRect(&r, 8, 4);
            if (dx >= r.left && dx < r.right && dy >= r.top && dy < r.bottom) { g_drag = i; break; }
        }
    }
    if (g_drag >= 0) slider_set(g_drag, dx);
    if (msg == WM_LBUTTONUP) g_drag = -1;
    sliders_publish();
}

static void cycle(int which)
{
    if (which == 0) panel_speed_cycle();
    else if (which == 1) panel_zoom_cycle();
    else { static const int GROUP[4] = { 3, 0, 1, 2 }; layout_group_cycle(GROUP[which - 2]); }
}

/* ---------------------------------------------------------------- definitions (built per page) */

static BYTE g_tpl_on[BTNDEF_SIZE], g_tpl_off[BTNDEF_SIZE], g_tpl_text[BTNDEF_SIZE], g_tpl_back[BTNDEF_SIZE];
static BYTE g_tpl_row[OBJDEF_SIZE], g_tpl_panel[OBJDEF_SIZE], g_tpl_backobj[OBJDEF_SIZE];
static BYTE g_hdr_play[SCRDEF_HDR], g_hdr_menu[SCRDEF_HDR];

static BYTE g_btn[64][BTNDEF_SIZE]; static int g_nbtn;
static BYTE g_obj[MAX_OBJS][OBJDEF_SIZE]; static int g_nobj;
static BYTE g_titles[MAX_OBJS][TITLE_SIZE];
static BYTE g_area_row[AREA_SIZE], g_area_nav[AREA_SIZE];
static BYTE g_image[8 + 68];                                                     /* SOBJIMAGEDEF: box + name */
static BYTE g_screen[SCRDEF_HDR + 4 * MAX_OBJS];

/* our entry buttons in the game's menus */
static BYTE g_entry_pause[BTNDEF_SIZE], g_entry_main[BTNDEF_SIZE];

static void set_pos(BYTE *p, int x, int y) { ((short *)p)[0] = (short)x; ((short *)p)[1] = (short)y; }

static BYTE *new_btn(const BYTE *tpl, int x, int y, const char *text, void *cb, DWORD data)
{
    if (g_nbtn >= 64) return NULL;
    BYTE *b = g_btn[g_nbtn++];
    memcpy(b, tpl, BTNDEF_SIZE);
    set_pos(b + 0x20, x, y);
    if (text) *(const char **)(b + 0x28) = text;
    *(void **)(b + 0x138) = cb;
    *(DWORD *)(b + 0x13c) = data;
    return b;
}
static BYTE *new_obj(const BYTE *tpl, int x, int y, const char *title)
{
    if (g_nobj >= MAX_OBJS) return NULL;
    BYTE *o = g_obj[g_nobj];
    memcpy(o, tpl, OBJDEF_SIZE);
    set_pos(o + 4, x, y);
    if (title) {
        BYTE *t = g_titles[g_nobj];
        memcpy(t, *(BYTE **)(tpl + 8), TITLE_SIZE);
        *(const char **)(t + 20) = title;
        *(BYTE **)(o + 8) = t;
    }
    *(int *)(o + 0x14) = 0;
    memset(o + 0x18, 0, OBJDEF_SIZE - 0x18);
    g_nobj++;
    return o;
}
static void add_btn(BYTE *o, BYTE *b) { int n = *(int *)(o + 0x14); if (o && b && n < 9) { ((BYTE **)(o + 0x18))[n] = b; *(int *)(o + 0x14) = n + 1; } }

static LONG __fastcall mm_button(BYTE *btn);

static void build_page(void)
{
    g_nbtn = 0; g_nobj = 0;
    BYTE *panel = new_obj(g_tpl_panel, PANEL_X, PANEL_Y, "Mod Settings");
    if (panel) *(BYTE **)(panel + 0xc) = g_image;                 /* the large panel picture */
    const Item *items = PAGES[g_page].items;
    int n = PAGES[g_page].n;
    for (int i = 0; i < n; i++) {
        int col = i / ROWS_PER_COL, row = i % ROWS_PER_COL;
        BYTE *o = new_obj(g_tpl_row, ROW_X0 + col * COL_W, ROW_Y0 + row * ROW_H, items[i].label);
        if (!o) break;
        *(BYTE **)(o + 0x10) = g_area_row;
        BYTE *t = *(BYTE **)(o + 8);
        set_pos(t, LABEL_X, 3);
        *(int *)(t + 12) = LABEL_COLOUR;
        int oi = g_nobj - 1;
        if (items[i].source == 2) {
            /* a slider (2026-11-20, user: "change the mod options that are not buttons to be sliders"): no game
             * button; drawn by menutext.c, dragged through modmenu_slider_mouse */
            (void)oi;
        } else {
            add_btn(o, new_btn(g_tpl_on, ON_X, 0, NULL, (void *)mm_button, DATA(K_ON, oi, i)));
            add_btn(o, new_btn(g_tpl_off, OFF_X, 0, NULL, (void *)mm_button, DATA(K_OFF, oi, i)));
        }
    }
    BYTE *nav = new_obj(g_tpl_row, PANEL_X, NAV_Y, NULL);
    if (nav) {
        *(BYTE **)(nav + 8) = *(BYTE **)(g_tpl_backobj + 8);     /* no title */
        *(BYTE **)(nav + 0x10) = g_area_nav;
        for (int p = 0; p < NPAGES; p++)
            add_btn(nav, new_btn(g_tpl_text, 14 + p * 74, 6, PAGES[p].tab, (void *)mm_button, DATA(K_TAB, g_nobj - 1, p)));
        add_btn(nav, new_btn(g_tpl_back, PANEL_W - 64 - 20, 0, NULL, (void *)mm_button, DATA(K_BACK, g_nobj - 1, 0)));
    }
    memcpy(g_screen, g_from_play ? g_hdr_play : g_hdr_menu, SCRDEF_HDR);
    *(int *)(g_screen + 0x58) = g_nobj;
    for (int i = 0; i < g_nobj; i++) ((BYTE **)(g_screen + SCRDEF_HDR))[i] = g_obj[i];
}

/* ---------------------------------------------------------------- the screen (state 14) */

int game_in_play(void) { return GAME && GAME_GET_STATE(GAME, 0) == GAME_PLAY_STATE; }
#define in_play game_in_play

static LONG __fastcall mm_init(BYTE **ph)
{
    menutext_clear();                                  /* a page rebuilt under the same state: the old labels go */
    g_drag = -1;
    g_from_play = in_play();
    if (!g_from_play) ERASE_SURFACES();
    SET_PATH(SCREEN_PATH);
    build_page();
    BYTE *scr = SCRN_CREATE(g_screen + 4, *(int *)g_screen & 0xffff);
    if (!scr) { mlog("modmenu: ScrnCreate failed"); return 0; }
    SCRN_LOAD_DEF(scr, g_screen);
    /* the switches' state: ON or OFF pressed; the tab of this page pressed */
    for (int i = 0; i < g_nobj; i++) {
        BYTE *o = SCREEN_OBJ(scr, i);
        if (!o) continue;
        for (int k = 0; k < 9; k++) {
            BYTE *b = OBJ_BTN(o, k);
            if (!b) break;
            DWORD d = BTN_DATA(b);
            int kind = D_KIND(d);
            if (kind == K_ON || kind == K_OFF) {
                int v = item_value(&PAGES[g_page].items[D_B(d)]);
                if ((kind == K_ON) == (v != 0)) { BTN_ATTRS(b) |= SBTN_CLICK_BYPASS; SBTN_SET_STATE(b, BUTTON_CLICKED); }
            } else if (kind == K_TAB && D_B(d) == g_page) {
                BTN_ATTRS(b) |= SBTN_CLICK_BYPASS; SBTN_SET_STATE(b, BUTTON_CLICKED);
            }
        }
    }
    SCREEN_UPDATE(scr) = 1;
    *ph = scr;
    SHOW_CURSOR(1);
    mlog("modmenu: page %d open (%s)", g_page, g_from_play ? "pause menu" : "main menu");
    sliders_publish();
    return 0;
}

static LONG __fastcall mm_button(BYTE *btn)
{
    DWORD d = BTN_DATA(btn);
    int kind = D_KIND(d), oi = D_A(d), idx = D_B(d);
    BYTE *scr = SCRN_FULL;
    BYTE *obj = scr && oi < MAX_OBJS ? SCREEN_OBJ(scr, oi) : NULL;
    switch (kind) {
    case K_ON: case K_OFF: {
        const Item *it = &PAGES[g_page].items[idx];
        item_set(it, kind == K_ON);
        mlog("modmenu: %s %s", it->label, kind == K_ON ? "on" : "off");
        if (obj) {
            BYTE *other = OBJ_BTN(obj, kind == K_ON ? 1 : 0);
            if (other) { SBTN_SET_STATE(other, BUTTON_NULL); BTN_UPDATE(other) = 1; }
            OBJ_UPDATE(obj) = 1;
        }
        BTN_ATTRS(btn) |= SBTN_CLICK_BYPASS;
        BTN_UPDATE(btn) = 1;
        return 1;
    }
    case K_CYCLE: {
        const Item *it = &PAGES[g_page].items[idx];
        cycle(it->index);
        /* rebuilt (as a page switch): changing the text in place left the old text under the new one (the
         * HD menu layers keep it) */
        SCRN_SET_STATE(MODSET_STATE);
        return 1;
    }
    case K_TAB:
        if (idx == g_page) { BTN_ATTRS(btn) |= SBTN_CLICK_BYPASS; return 0; }
        g_page = idx;
        SCRN_SET_STATE(MODSET_STATE);                    /* rebuilt with the new page */
        return 1;
    case K_BACK:
        g_page = 0;
        if (g_from_play) DD_COPY_BACK_TO_FRONT();         /* the big panel would stay under the small pause menu */
        SCRN_SET_STATE(g_from_play ? STATE_PAUSE : STATE_MAIN);
        return 1;
    case K_OPEN:
        g_page = 0;
        SCRN_SET_STATE(MODSET_STATE);
        if (in_play() && MAIN_FRAME) SET_UI_OVERRIDE(MAIN_FRAME, 0, NULL);   /* as the game's Audio button */
        return 1;
    }
    return 0;
}

/* Rewrite of ScrnOptionsVisible (0x468510): screens that are up during play; ours (14) added. */
static BOOL __cdecl mm_options_visible(void)
{
    switch (SCRN_GET_STATE()) {
    case 7: case 8: case 9: case 11: case 12: case 13: case MODSET_STATE:
    case 16: case 17: case 18: case 19: case 20:
        return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- install (patches.c, at start) */

static int check_count(uintptr_t obj, int want)
{
    int n = *(int *)(obj + 0x14);
    if (n != want || ((void **)(obj + 0x18))[want]) { mlog("modmenu: object %08lx has %d buttons (expected %d) - not added", (unsigned long)obj, n, want); return 0; }
    return 1;
}

void modmenu_install(void)
{
    if (!GetPrivateProfileIntA("display", "mod_menu", 1, mod_ini_path())) { mlog("modmenu: off ([display] mod_menu=0)"); return; }
    /* templates: the game's own definitions */
    memcpy(g_tpl_on, (void *)DEF_SFX_ON, BTNDEF_SIZE);
    memcpy(g_tpl_off, (void *)DEF_SFX_OFF, BTNDEF_SIZE);
    memcpy(g_tpl_text, (void *)DEF_AUDIO_BTN, BTNDEF_SIZE);
    *(DWORD *)g_tpl_text &= ~0x04000000u;                     /* not centred: placed where we say */
    *(int *)(g_tpl_text + 4) = 4;                              /* FONT_CARGO_14 like the row labels */
    memcpy(g_tpl_back, (void *)*(uintptr_t *)(DEF_AUDIO_BACKOBJ + 0x18), BTNDEF_SIZE);
    memcpy(g_tpl_row, (void *)DEF_AUDIO_ROW, OBJDEF_SIZE);
    memcpy(g_tpl_panel, (void *)DEF_AUDIO_PANEL, OBJDEF_SIZE);
    memcpy(g_tpl_backobj, (void *)DEF_AUDIO_BACKOBJ, OBJDEF_SIZE);
    memcpy(g_area_row, *(void **)(DEF_AUDIO_ROW + 0x10), AREA_SIZE);
    ((short *)g_area_row)[2] = COL_W; ((short *)g_area_row)[3] = ROW_H;
    memcpy(g_area_nav, g_area_row, AREA_SIZE);
    ((short *)g_area_nav)[2] = PANEL_W; ((short *)g_area_nav)[3] = 40;
    memset(g_image, 0, sizeof g_image);
    ((short *)g_image)[2] = PANEL_W; ((short *)g_image)[3] = PANEL_H;
    strcpy((char *)g_image + 8, "panel.bmp");
    memcpy(g_hdr_play, (void *)DEF_PLAY_SCREEN, SCRDEF_HDR);
    memcpy(g_hdr_menu, (void *)DEF_MENU_SCREEN, SCRDEF_HDR);

    /* state 14: our init, the game's plain terminate */
    uintptr_t e = STATE_TABLE + MODSET_STATE * 20;
    if (*(DWORD *)e != MODSET_STATE || *(DWORD *)(e + 8) != 0x4629a0 || *(DWORD *)(e + 12) != 0x462a70) { mlog("modmenu: state table entry 14 unexpected - off"); return; }
    DWORD init = (DWORD)(uintptr_t)mm_init, term = TERMINATE_SCREEN;
    if (!write_mem(e + 4, &init, 4) || !write_mem(e + 16, &term, 4)) { mlog("modmenu: state table write failed"); return; }
    static const BYTE vis_o[] = { 0x56, 0x33, 0xf6, 0xe8, 0xc8, 0xa1, 0xff, 0xff };
    hook_jmp("modmenu: ScrnOptionsVisible (0x468510) + state 14", 0x468510, vis_o, sizeof vis_o, (void *)mm_options_visible);

    /* the entry buttons */
    memcpy(g_entry_pause, (void *)DEF_AUDIO_BTN, BTNDEF_SIZE);
    set_pos(g_entry_pause + 0x20, 0, 180);
    *(const char **)(g_entry_pause + 0x28) = "Mod Settings";
    *(void **)(g_entry_pause + 0x138) = (void *)mm_button;
    *(DWORD *)(g_entry_pause + 0x13c) = DATA(K_OPEN, 0, 0);
    memcpy(g_entry_main, (void *)DEF_OPTIONS_BTN, BTNDEF_SIZE);
    set_pos(g_entry_main + 0x20, 0, 190);                       /* 30 px after Options (160), like the others */
    *(const char **)(g_entry_main + 0x28) = "Mod Settings";
    *(void **)(g_entry_main + 0x138) = (void *)mm_button;
    *(DWORD *)(g_entry_main + 0x13c) = DATA(K_OPEN, 0, 0);
    if (check_count(DEF_PAUSE_OBJ, 3)) {
        void *p = g_entry_pause; int n = 4;
        write_mem(DEF_PAUSE_OBJ + 0x18 + 3 * 4, &p, 4); write_mem(DEF_PAUSE_OBJ + 0x14, &n, 4);
        mlog("modmenu: Mod Settings added to the pause menu");
    }
    if (check_count(DEF_MAIN_OBJ, 4)) {
        void *p = g_entry_main; int n = 5;
        write_mem(DEF_MAIN_OBJ + 0x18 + 4 * 4, &p, 4); write_mem(DEF_MAIN_OBJ + 0x14, &n, 4);
        mlog("modmenu: Mod Settings added to the main menu");
        /* Quit (object 0x4c6bf0 at 290,310) sat right under it: down into the panel's empty bottom (panel 100..380) */
        short *qp = (short *)(DEF_QUIT_OBJ + 4);
        if (qp[0] == 290 && qp[1] == 310) { short q[2] = { 290, 328 }; write_mem(DEF_QUIT_OBJ + 4, q, 4); }
    }
    g_ready = 1;
}

/* F10 (panel.c): open the settings screen when a menu screen is up, else 0 (the caller falls back) */
int modmenu_open(void)
{
    if (!g_ready) return 0;
    int st = SCRN_GET_STATE();
    if (st != STATE_PAUSE && st != STATE_MAIN) return 0;
    g_page = 0;
    SCRN_SET_STATE(MODSET_STATE);
    if (in_play() && MAIN_FRAME) SET_UI_OVERRIDE(MAIN_FRAME, 0, NULL);
    return 1;
}

/* layout.c: while the settings screen is up during play, the game-screen rect of its panel (the enlarged
 * HUD corners must not be drawn over it) */
int modmenu_panel_rect(RECT *r)
{
    if (!g_ready || SCRN_GET_STATE() != MODSET_STATE || !in_play()) return 0;
    int k = layout_menu_scale(), x0 = PANEL_X, y0 = PANEL_Y, x1 = PANEL_X + PANEL_W, y1 = PANEL_Y + PANEL_H;
    if (k < 64) k = 64;
    layout_menu_place(k, &x0, &y0); layout_menu_place(k, &x1, &y1);   /* the menu as drawn (layout.c) */
    SetRect(r, x0, y0, x1, y1);
    return 1;
}

/* F10 (panel.c): Mod Settings from anywhere. On the pause or main menu it opens directly; on the settings
 * screen it goes back; during play it opens the pause menu (Esc, as the gamepad's Start) and steps on
 * into Mod Settings once that is up (modmenu_tick). Returns 0 when it does not apply (title, videos). */
static DWORD g_pending;
int modmenu_f10(void)
{
    if (!g_ready) return 0;
    int st = SCRN_GET_STATE();
    if (st == MODSET_STATE) { g_page = 0; if (g_from_play) DD_COPY_BACK_TO_FRONT(); SCRN_SET_STATE(g_from_play ? STATE_PAUSE : STATE_MAIN); return 1; }
    if (st == STATE_PAUSE || st == STATE_MAIN) return modmenu_open();
    if (in_play() && layout_view_is_gameplay() && g_dd && g_dd->hwnd) {
        PostMessageA(g_dd->hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
        PostMessageA(g_dd->hwnd, WM_KEYUP, VK_ESCAPE, 0xC0010001);
        g_pending = GetTickCount();
        return 1;
    }
    return 0;
}
/* each finished frame (panel.c): the pause menu opened by F10 is up -> on into Mod Settings */
void modmenu_tick(void)
{
    if (!g_pending) return;
    if (GetTickCount() - g_pending > 1500) { g_pending = 0; return; }
    if (SCRN_GET_STATE() == STATE_PAUSE) { g_pending = 0; modmenu_open(); }
}

/* layout.c / hud.c: one of the game's option screens is up over a PAUSED mission (the screen state alone can
 * stay at the pause menu after Esc closed it). Not the stats screen (16, a full 640x480 picture). */
int game_menu_up(void)
{
    BYTE *g = GAME;
    if (!g || !in_play() || !(*(DWORD *)(g + 8) & 8)) return 0;     /* game+8: pause flags, 8 = PAUSE_LOCAL */
    int st = SCRN_GET_STATE();
    return (st >= 7 && st <= 14) || (st >= 17 && st <= 20);
}
