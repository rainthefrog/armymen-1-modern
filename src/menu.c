/* ============================================================================================
 * menu.c - "Gameplay" and "Graphics" menus in the game window's menu bar (windowed mode)
 *
 * Gameplay:  Modern controls (controls.c, [controls] mode), Health regeneration ([gameplay]
 *            health_regen), Shoot over fences ([gameplay] shoot_over).
 * Graphics:  Upscaled art (battlefield + pictures + widescreen menus; [display] hd_world, hd_sprites,
 *            hd_menus), CRT filter ([display] crt, also F9), fog at the map edge, see Sarge through
 *            roofs, HUD in the corners, pinned objective, target sector on the minimap.
 * Changes take effect at once and are saved to armymen-mod.ini.
 *
 * The game may set its menu bar again (mode switches), so menu_install() is called from the
 * window hook on activation / size changes and adds the menus whenever they are missing. MFC (the
 * game is an MFC program) greys out menu items it has no handler for whenever a menu opens, so
 * menu_popup() enables ours again after the game has handled WM_INITMENUPOPUP.
 * In borderless fullscreen there is no menu bar: the F10 panel has the same switches.
 * ============================================================================================ */
#include "common.h"

#define ID_FIRST    0xA100
#define ID_MODERN   0xA101
#define ID_REGEN    0xA102
#define ID_SHOOT    0xA103
#define ID_HD       0xA110
#define ID_CRT      0xA111
#define ID_FOG      0xA112
#define ID_SEE      0xA113
#define ID_CORNERS  0xA114
#define ID_PIN      0xA115
#define ID_SECTOR   0xA116
#define ID_AREA     0xA117
#define ID_LAST     0xA1FF

typedef struct { int id; const char *label; int *value; const char *section, *key; } Item;

static int g_hd_shown;              /* menu state of "Upscaled art" (any of the three on) */

static Item g_gameplay[] = {
    { ID_MODERN, "&Modern controls (mouse aim, move, run)", &g_cfg.controls_modern, NULL, NULL },
    { ID_REGEN,  "&Health regeneration (slow, stops while under fire)", &g_cfg.health_regen, "gameplay", "health_regen" },
    { ID_SHOOT,  "&Shoot over fences and walls", &g_cfg.shoot_over, "gameplay", "shoot_over" },
};
static Item g_graphics[] = {
    { ID_HD,      "&Upscaled art (sprites, terrain, pictures, wide menus)", &g_hd_shown, NULL, NULL },
    { ID_CRT,     "&CRT filter (scanlines)\tF9", &g_cfg.crt, "display", "crt" },
    { ID_FOG,     "&Fog at the map edge", &g_cfg.map_edge_fog, "display", "map_edge_fog" },
    { ID_SEE,     "&See Sarge through roofs", &g_cfg.see_through, "display", "see_through" },
    { ID_CORNERS, "HUD in the &corners", &g_cfg.hud_corners, "hud", "corners" },
    { ID_PIN,     "&Pinned objective", &g_cfg.pin_objective, "hud", "pin_objective" },
    { ID_SECTOR,  "&Target sector on the minimap", &g_cfg.target_sector, "hud", "target_sector" },
    { ID_AREA,    "Target sector &highlighted on the battlefield", &g_cfg.target_area, "hud", "target_area" },
};
#define N(a) ((int)(sizeof a / sizeof a[0]))

static HMENU g_menu_gameplay, g_menu_graphics;

static void refresh_menu(HMENU m, Item *items, int n)
{
    if (!m) return;
    g_hd_shown = g_cfg.hd_world || g_cfg.hd_sprites || g_cfg.hd_menus;
    for (int i = 0; i < n; i++) {
        EnableMenuItem(m, items[i].id, MF_BYCOMMAND | MF_ENABLED);
        CheckMenuItem(m, items[i].id, MF_BYCOMMAND | (*items[i].value ? MF_CHECKED : MF_UNCHECKED));
    }
}

static void refresh(void)
{
    refresh_menu(g_menu_gameplay, g_gameplay, N(g_gameplay));
    refresh_menu(g_menu_graphics, g_graphics, N(g_graphics));
}

void menu_popup(HMENU m)
{
    if (m && (m == g_menu_gameplay || m == g_menu_graphics)) refresh();
}

static HMENU build(Item *items, int n)
{
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < n; i++) AppendMenuA(m, MF_STRING, items[i].id, items[i].label);
    return m;
}

void menu_install(HWND h)
{
    HMENU bar = GetMenu(h);
    if (!bar || GetMenuState(bar, ID_MODERN, MF_BYCOMMAND) != (UINT)-1) return;   /* none, or already there */
    g_menu_gameplay = build(g_gameplay, N(g_gameplay));
    g_menu_graphics = build(g_graphics, N(g_graphics));
    refresh();
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_menu_gameplay, "Game&play");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)g_menu_graphics, "G&raphics");
    DrawMenuBar(h);
    mlog("menu: Gameplay and Graphics menus added");
}

/* All upscaled art on or off at once (battlefield, pictures, widescreen menus). */
void menu_toggle_hd(void)
{
    int on = !(g_cfg.hd_world || g_cfg.hd_sprites || g_cfg.hd_menus);
    g_cfg.hd_world = g_cfg.hd_sprites = g_cfg.hd_menus = on;
    const char *v = on ? "1" : "0";
    WritePrivateProfileStringA("display", "hd_world", v, mod_ini_path());
    WritePrivateProfileStringA("display", "hd_sprites", v, mod_ini_path());
    WritePrivateProfileStringA("display", "hd_menus", v, mod_ini_path());
    mlog("menu: upscaled art %s", on ? "on" : "off");
    refresh();
}

static void toggle_item(Item *it)
{
    if (it->id == ID_MODERN) {
        g_cfg.controls_modern = !g_cfg.controls_modern;
        patches_set_modern_controls(g_cfg.controls_modern);
        controls_reset();
        WritePrivateProfileStringA("controls", "mode", g_cfg.controls_modern ? "modern" : "classic", mod_ini_path());
        mlog("menu: controls %s", g_cfg.controls_modern ? "modern" : "classic");
    } else if (it->id == ID_HD) {
        menu_toggle_hd();
        return;
    } else {
        *it->value = !*it->value;
        WritePrivateProfileStringA(it->section, it->key, *it->value ? "1" : "0", mod_ini_path());
        mlog("menu: %s = %d", it->key, *it->value);
    }
    refresh();
}

/* WM_COMMAND from the window hook; returns 1 if it was one of ours. */
int menu_command(HWND h, int id)
{
    if (id < ID_FIRST || id > ID_LAST) return 0;
    for (int i = 0; i < N(g_gameplay); i++) if (g_gameplay[i].id == id) toggle_item(&g_gameplay[i]);
    for (int i = 0; i < N(g_graphics); i++) if (g_graphics[i].id == id) toggle_item(&g_graphics[i]);
    return 1;
}

/* CRT filter on/off (F9 or the menu); saved to the ini. */
void menu_toggle_crt(void)
{
    g_cfg.crt = !g_cfg.crt;
    WritePrivateProfileStringA("display", "crt", g_cfg.crt ? "1" : "0", mod_ini_path());
    mlog("menu: CRT filter %d", g_cfg.crt);
    refresh();
}
