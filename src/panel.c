/* ============================================================================================
 * panel.c - the mod's own on-screen boxes: F10 settings panel, clicks on the pinned objective
 *
 * F10 opens a settings panel over the game (works in borderless fullscreen, where our menu bar is
 * hidden); the game clock is paused while it is open. Each row is a switch, saved to the ini:
 * modern controls, health regen, CRT filter, edge fog, shoot over, pinned objective, target sector,
 * HUD corners, and the game speed (click cycles 0.5x .. 2x).
 * Left click on the pinned objective box (objectives.c) shows the mission briefing again (the
 * game's Objective command, 0x460e70).
 * overlay_message() runs first in the window hook: F9 (CRT), F10 (panel), left clicks on our boxes.
 * controls.c treats our boxes as interface (no firing / walking there).
 * ============================================================================================ */
#include "common.h"

#define CMD_OBJECTIVE ((VoidFnPtr)0x460e70)

static int g_open, g_was_paused;
static int g_page;                     /* tabs in the title row: 0 settings, 1 controls, 2 modern, 3 binds (bindings.c) */
#define PAGES 4

/* The Controls page: the modern layout (controls.c, keys also in the game's own table) or the game's own keys. */
static const char *const KEYS_MODERN[] = {           /* one-button mouse (controls.c; the only modern scheme) */
    "Left click ground: walk there (finds a way)   Double-click: sprint there",
    "Hold left: walk toward pointer (double-click + hold: run)   Click Sarge: one stance down (lying: stand up)",
    "Double-click Sarge: prone   Press on Sarge + drag up / down: pick stand / kneel / prone",
    "Left on an enemy in range (red reticle): shoot, hold = keep shooting",
    "Left on your empty vehicle: get in (double-click: sprint to it)   Click your vehicle while driving: get out",
    "Shoot anywhere: right button or Shift + left      Right click your soldier: select his unit",
    "Grenade / mortar: click throws at the pointer (circle = blast area, distance shown)",
    "Move: W A S D  Sprint: Shift  Slow: Ctrl  C: tap down, hold prone, double-tap stand  Dive: Space / Mouse 4",
    "Weapons: 1-6 / wheel / middle click, Mouse 5 previous   Hold middle: radial (on Sarge: stance ring)   Q: last   G: special",
    "Vehicle: F      Drop item: hold X (or right-click its HUD box)",
    "Squad: F1 follow  F2 attack  F3 defend  F9 attack now      Next unit: Tab",
    "Air: F5 strike  P paratroopers  R recon (or click HUD)      Map: M   Objectives: J",
    "Game speed: Ctrl - / = / 0      Fullscreen: F11   CRT: F9   Mod menu: F10",
    "Zoom the battlefield: Ctrl + wheel or Ctrl + PageUp / PageDown",
    "Modern: Z roll, hold E peek, hold I stats, hold B radial (pointer on Sarge: stance ring), T ping, hold F12 restart",
    "These are the defaults - every key / extra mouse button: BINDS tab (Tab key switches tabs)",
    "Ctrl + left: squad attack / defend   Ctrl + right: follow   Gamepad works",
};
static const char *const KEYS_CLASSIC[] = {
    "Move: Up Down / Num8 Num2      Turn: Left Right / Num4 Num6",
    "Roll: Num7 Num9      Dive: Num/      Kneel / prone: Num5      Slow: Shift",
    "Fire: Ctrl / Space      Gun left / right: Z X",
    "Weapons: 1 2 3      Enter vehicle: E      Drop item: D / X",
    "Squad: F1 follow  F2 attack  F3 defend  A attack now  F defend now",
    "Next unit: Tab      Air: S strike  P paratroopers  R recon",
    "Map: V      Objective: O      Gunsight: G      Pause: Esc      Mod menu: F10",
};
#define NLINES(a) ((int)(sizeof a / sizeof a[0]))
static RECT g_panel;                   /* game-screen rect of the panel (while open) */

static int g_hd_all;                   /* panel row "Upscaled art" (any of the three on) */
typedef struct { const char *name; int *value; const char *section, *key; } Row;

static void toggle_modern(void)
{
    g_cfg.controls_modern = !g_cfg.controls_modern;
    patches_set_modern_controls(g_cfg.controls_modern);
    controls_reset();
    WritePrivateProfileStringA("controls", "mode", g_cfg.controls_modern ? "modern" : "classic", mod_ini_path());
}

/* The row ORDER is an interface: modmenu.c names rows by index. Add new rows at the END (2026-11-19: a row put in
 * the middle made Mod Settings' "HUD in corners" switch the target highlight instead). */
static Row g_rows[] = {
    { "Modern controls",          &g_cfg.controls_modern, NULL, NULL },
    { "Upscaled art",             &g_hd_all, "hd", NULL },
    { "Health regeneration",      &g_cfg.health_regen, "gameplay", "health_regen" },
    { "Barricade rules",          &g_cfg.shoot_over, "gameplay", "shoot_over" },
    { "CRT filter (F9)",          &g_cfg.crt, "display", "crt" },
    { "Fog at the map edge",      &g_cfg.map_edge_fog, "display", "map_edge_fog" },
    { "See Sarge through roofs",  &g_cfg.see_through, "display", "see_through" },
    { "Pinned objective",         &g_cfg.pin_objective, "hud", "pin_objective" },
    { "Target sector on minimap", &g_cfg.target_sector, "hud", "target_sector" },
    { "HUD in the corners",       &g_cfg.hud_corners, "hud", "corners" },
    { "Target outline on ground", &g_cfg.target_area, "hud", "target_area" },                 /* 10 */
    { "Enemies use barricades",   &g_cfg.cover_enemies, "gameplay", "cover_enemies" },
    { "Corner shots",             &g_cfg.corner_shots, "gameplay", "corner_shots" },
    { "Leg hits count",           &g_cfg.leg_hits, "gameplay", "leg_hits" },
    { "Height rules",             &g_cfg.height_rules, "gameplay", "height_rules" },   /* 14: cycles stance / am2 / original (was Downhill hits) */
    { "Enemies seen in buildings", &g_cfg.see_enemies, "display", "see_enemies" },            /* 15 */
    { "Menu buttons press in",    &g_cfg.button_press, "display", "button_press" },
    { "Longer weapon range",      &g_cfg.longer_range, "gameplay", "longer_range" },            /* 17 */
    { "Building interior view",   &g_cfg.see_interior, "display", "see_interior" },
    { "Sharp menu text",          &g_cfg.menu_text, "display", "menu_text" },                  /* 19 */
    { "No corner squash",         &g_cfg.fix_squash, "gameplay", "fix_squash" },               /* 20 */
    { "Radio messages as overlay", &g_cfg.radio_overlay, "display", "radio_overlay" },        /* 21 */
    { "Mission path on ground",   &g_cfg.mission_path, "hud", "mission_path" },             /* 22 */
    { "Drive empty enemy vehicles", &g_cfg.drive_enemy_vehicles, "gameplay", "drive_enemy_vehicles" },   /* 23 */
    { "Minimap: bigger blips",    &g_cfg.mm_blips, "hud", "minimap_blips" },                   /* minimap.c (modmenu.c finds these by value pointer, any index) */
    { "Minimap: Sarge marked",    &g_cfg.mm_sarge, "hud", "minimap_sarge" },
    { "Minimap: contrast",        &g_cfg.mm_contrast, "hud", "minimap_contrast" },
    { "Minimap: hover details",   &g_cfg.mm_hover, "hud", "minimap_hover" },
    { "Vehicles: reverse to fit", &g_cfg.vehicle_reverse, "controls", "vehicle_reverse" },    /* vehicles.c (2026-11-20) */
    { "Vehicles: steady turret",  &g_cfg.turret_smooth, "controls", "turret_smooth" },
    { "Vehicles: get unstuck",    &g_cfg.vehicle_unstick, "controls", "vehicle_unstick" },
    { "Sarge: get unstuck",       &g_cfg.sarge_unstick, "controls", "sarge_unstick" },      /* barricades.c (2026-11-20) */
    { "Vehicles: bail out alive", &g_cfg.vehicle_bailout, "gameplay", "vehicle_bailout" },     /* capture.c (2026-11-20) */
    { "Sarge breaks fences",      &g_cfg.break_fences, "gameplay", "break_fences" },         /* fences.c (2026-11-20) */
    { "Enemy awareness",          &g_cfg.enemy_awareness, "modern", "enemy_awareness" },   /* aware.c (2026-11-20) */
    { "Squad sprints with Sarge", &g_cfg.squad_sprint, "gameplay", "squad_sprint" },        /* squad.c (2026-11-20) */
    { "Army Men II sounds",       &g_cfg.am2_sounds, "audio", "am2_sounds" },              /* audio.c (2026-11-21) */
    { "Army Men II music",        &g_cfg.am2_music, "audio", "am2_music" },                /* music.c (2026-11-21) */
    { "Enemy marches patrol",     &g_cfg.patrol_marches, "gameplay", "patrol_marches" },  /* patrol.c (2026-11-21) */
    { "Squad: health regeneration", &g_cfg.ally_regen, "gameplay", "ally_regen" },        /* allies.c (2026-11-21) */
    { "Squad: medkits heal them", &g_cfg.ally_heal, "gameplay", "ally_heal" },
    { "Squad: health bars",       &g_cfg.ally_health_bars, "hud", "ally_health_bars" },
};
#define ROWS ((int)(sizeof g_rows / sizeof g_rows[0]))
#define SPEED_ROW ROWS                 /* game speed */
#define ZOOM_ROW  (ROWS + 1)           /* the last row: battlefield zoom (zoom.c) */
static const double SPEEDS[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };

#define PANEL_W   260
#define ROW_H     14
#define PAD       8

int panel_is_open(void) { return g_open; }

static void set_open(int on)
{
    if (on == g_open) return;
    g_open = on;
    if (on) { g_was_paused = clock_is_paused(); clock_set_paused(1); }
    else clock_set_paused(g_was_paused);
    mlog("panel: settings %s", on ? "open" : "closed");
}

/* Is a game-screen point on one of our boxes? */
int overlay_at(int x, int y)
{
    POINT p = { x, y };
    if (g_open && PtInRect(&g_panel, p)) return 1;
    RECT r;
    return objectives_box(&r) && PtInRect(&r, p);
}

static void click_row(int row)
{
    if (row == ZOOM_ROW) { zoom_cycle(); return; }
    if (row == SPEED_ROW) {
        double s = clock_speed_now();
        int i = 0, n = (int)(sizeof SPEEDS / sizeof SPEEDS[0]);
        while (i < n && SPEEDS[i] <= s + 0.01) i++;
        clock_set_speed(SPEEDS[i < n ? i : 0]);
        return;
    }
    if (row < 0 || row >= ROWS) return;
    Row *r = &g_rows[row];
    if (!r->section) { toggle_modern(); return; }
    if (!r->key) { menu_toggle_hd(); return; }
    if (r->value == &g_cfg.height_rules) { height_rules_next(); return; }   /* three values (modded: ini only) */
    *r->value = !*r->value;
    WritePrivateProfileStringA(r->section, r->key, *r->value ? "1" : "0", mod_ini_path());
    mlog("panel: %s %d", r->name, *r->value);
}

static int overlay_click(int x, int y)
{
    POINT p = { x, y };
    if (g_open) {
        if (!PtInRect(&g_panel, p)) return 0;
        if (y < g_panel.top + PAD + ROW_H) {                 /* title row: the tabs */
            g_page = (x - g_panel.left) * PAGES / (g_panel.right - g_panel.left);
            if (g_page < 0) g_page = 0;
            if (g_page >= PAGES) g_page = PAGES - 1;
            if (bindings_panel_capturing()) bindings_panel_key(VK_ESCAPE);   /* a tab click ends a capture */
            return 1;
        }
        int row = (y - g_panel.top - PAD - ROW_H - 4) / ROW_H;
        if (y - g_panel.top - PAD - ROW_H - 4 < 0) return 1;
        if (g_page == 0) click_row(row);
        else if (g_page == 2 && row < modern_count()) modern_toggle(row);          /* MODERN: each feature */
        else if (g_page == 3) bindings_panel_click(x, y);                          /* BINDS (bindings.c) */
        return 1;
    }
    if (layout_resize_click(x, y)) { controls_block_left(); return 1; }   /* a HUD group's resize icon (layout.c) */
    RECT r;
    /* 2026-11-20: off by default - clicks into the top-left corner during play replayed the briefing radio and its
     * voice again and again (user). [hud] objective_click=1 brings it back. */
    static int oc = -1;
    if (oc < 0) oc = GetPrivateProfileIntA("hud", "objective_click", 0, mod_ini_path());
    if (oc && objectives_box(&r) && PtInRect(&r, p)) {
        mlog("panel: objective box clicked - briefing");
        CMD_OBJECTIVE();
        return 1;
    }
    return 0;
}

/* Window hook, before anything else. Returns 1 when the message was ours. */
int overlay_message(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    int key = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    /* BINDS page (bindings.c): waiting for a new key takes every key first (it refuses F9 F10 F11 itself) */
    if (g_open && g_page == 3 && key && bindings_panel_capturing()) { bindings_panel_key((int)wp); return 1; }
    if (g_open && key && wp == VK_TAB) {                                 /* Tab / Shift+Tab: the next / previous tab */
        g_page = (g_page + ((GetKeyState(VK_SHIFT) & 0x8000) ? PAGES - 1 : 1)) % PAGES;
        return 1;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F9) { menu_toggle_crt(); return 1; }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F10) {   /* F10 would open the menu bar */
        /* F10 = Mod Settings in the game's menus (modmenu.c); Shift + F10 = this overlay (CONTROLS / KEYS pages) */
        if (!g_open && !(GetKeyState(VK_SHIFT) & 0x8000) && modmenu_f10()) return 1;
        if (layout_view_is_gameplay() || g_open) set_open(!g_open);
        return 1;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && g_open) { set_open(0); return 1; }
    if (g_open && g_page == 3 && key && bindings_panel_key((int)wp)) return 1;   /* arrows, Enter, Delete, Space */
    if (msg == WM_LBUTTONDOWN && layout_view_is_gameplay()) {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        view_client_to_game(&p);
        if (overlay_click(p.x, p.y)) return 1;
    }
    return 0;
}

/* Finished frame: the panel, last (above everything). */
void panel_draw(Surf *b)
{
    g_hd_all = g_cfg.hd_world || g_cfg.hd_sprites || g_cfg.hd_menus;
    if (!g_open) return;
    if (!layout_view_is_gameplay()) { set_open(0); return; }
    int font, colour;
    if (!objectives_font(&font, &colour) || !hud_draw_string_original || !b || !b->bits) return;
    const char *const *keys = !g_cfg.controls_modern ? KEYS_CLASSIC : KEYS_MODERN;
    int nkeys = !g_cfg.controls_modern ? NLINES(KEYS_CLASSIC) : NLINES(KEYS_MODERN);
    int panel_w = 420;
    int hud_top = layout_hud_top();                                   /* BINDS: kept above the HUD (the upscaled HUD covers what is under it) */
    if (hud_top < 200 || hud_top > b->h) hud_top = b->h;
    int rows = g_page == 1 ? nkeys : g_page == 2 ? modern_count() : g_page == 3 ? bindings_panel_rows((hud_top - 40 - PAD * 2 - ROW_H - 6) / ROW_H) : ROWS + 2;
    int h = PAD * 2 + ROW_H + 4 + rows * ROW_H;
    int x0 = (g_cfg.width - panel_w) / 2, y0 = 40;
    if (x0 < 0 || x0 + panel_w > b->w || y0 + h > b->h) return;
    g_panel = (RECT){ x0, y0, x0 + panel_w, y0 + h };
    BYTE *shade = *(BYTE **)0x4e2448;           /* x0.40 shade table */
    for (int y = y0; y < y0 + h; y++) {
        BYTE *row = b->bits + (size_t)y * b->pitch;
        for (int x = x0; x < x0 + panel_w; x++) {
            int edge = y == y0 || y == y0 + h - 1 || x == x0 || x == x0 + panel_w - 1;
            row[x] = edge ? (BYTE)colour : shade ? shade[shade[row[x]]] : row[x];
        }
    }
    int clip[2] = { panel_w - 2 * PAD, ROW_H };
    static const char *const TABS[PAGES] = { "SETTINGS", "CONTROLS", "MODERN", "BINDS" };
    for (int t = 0; t < PAGES; t++) {
        char tab[24];
        wsprintfA(tab, t == g_page ? ">%s<" : " %s", TABS[t]);
        int tclip[2] = { panel_w / PAGES, ROW_H };
        hud_draw_string_original(b, x0 + PAD + t * (panel_w - 2 * PAD) / PAGES, y0 + PAD, tab, font, tclip, colour);
    }
    char buf[96];
    if (g_page == 1) {
        for (int i = 0; i < nkeys; i++)
            hud_draw_string_original(b, x0 + PAD, y0 + PAD + ROW_H + 4 + i * ROW_H, keys[i], font, clip, colour);
        return;
    }
    if (g_page == 2) {                          /* MODERN: one switch per feature (S = small, M = medium) */
        for (int i = 0; i < modern_count(); i++) {
            wsprintfA(buf, "[%s] %c  %s", modern_on(i) ? "X" : " ", modern_size(i), modern_name(i));
            hud_draw_string_original(b, x0 + PAD, y0 + PAD + ROW_H + 4 + i * ROW_H, buf, font, clip, colour);
        }
        return;
    }
    if (g_page == 3) {                          /* BINDS: every action's keyboard / mouse binding (bindings.c) */
        bindings_panel_draw(b, x0 + PAD, y0 + PAD + ROW_H + 4, panel_w - 2 * PAD, font, colour);
        return;
    }
    for (int i = 0; i < rows; i++) {
        int y = y0 + PAD + ROW_H + 4 + i * ROW_H;
        if (i < ROWS && g_rows[i].value == &g_cfg.height_rules)
            wsprintfA(buf, "%s: %s", g_rows[i].name, HEIGHT_RULE_TEXT[g_cfg.height_rules]);
        else if (i < ROWS) wsprintfA(buf, "[%s] %s", *g_rows[i].value ? "X" : " ", g_rows[i].name);
        else if (i == SPEED_ROW) {
            int s100 = (int)(clock_speed_now() * 100 + 0.5);
            wsprintfA(buf, "Game speed: %d.%02dx  (click to change)", s100 / 100, s100 % 100);
        } else {
            int z100 = (int)(zoom_target() * 100 + 0.5);
            wsprintfA(buf, "Zoom: %d.%02dx  (click, or Ctrl + wheel)", z100 / 100, z100 % 100);
        }
        hud_draw_string_original(b, x0 + PAD, y, buf, font, clip, colour);
    }
}

int panel_rect(RECT *r) { if (!g_open) return 0; *r = g_panel; return 1; }

/* ---------------------------------------------------------------- shared with modmenu.c
 * The same switches for the game-menu settings screen (modmenu.c): value / set by row, and the
 * cycling rows (game speed, zoom). */
int panel_row_count(void) { return ROWS; }
int panel_row_value(int i)
{
    g_hd_all = g_cfg.hd_world || g_cfg.hd_sprites || g_cfg.hd_menus;
    return i >= 0 && i < ROWS ? *g_rows[i].value != 0 : 0;
}
void panel_row_set(int i, int on) { if (i >= 0 && i < ROWS && panel_row_value(i) != (on != 0)) click_row(i); }
void panel_speed_cycle(void) { click_row(SPEED_ROW); }
void panel_zoom_cycle(void) { click_row(ZOOM_ROW); }

/* modmenu.c: the row that switches this value (-1 = none) - rows found by what they switch, not by position */
int panel_row_of(const int *value)
{
    for (int i = 0; i < ROWS; i++) if (g_rows[i].value == value) return i;
    return -1;
}

/* controls.c (WM_MOUSEWHEEL): the wheel while the panel is open scrolls the BINDS page, never changes weapons */
int panel_wheel(int delta)
{
    if (!g_open) return 0;
    if (g_page == 3) bindings_panel_wheel(delta);
    return 1;
}
