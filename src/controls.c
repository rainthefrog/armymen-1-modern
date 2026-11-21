/* ============================================================================================
 * controls.c - modern mouse + keyboard controls for Sarge ([controls] mode=modern)
 *
 *   mouse        a free cursor (Sarge's aiming reticle follows the pointer)
 *   left button  turn to the pointer and fire; held = fire again every fire_interval_ms
 *                (hold-to-aim weapons such as grenades: hold = the game's own range aiming, release = throw)
 *   right hold   walk toward the pointer
 *   right tap    walk to the clicked spot, steering around obstacles (any other movement cancels)
 *   W A S D      walk up / left / down / right on screen (diagonals too); while firing, Sarge keeps
 *                aiming at the pointer and walks forward or backward, whichever is closer
 *   right tap on one of your empty vehicles: walk to it and climb in (E still enters / leaves)
 *   vehicle      the turret follows the pointer, left button fires (held = keeps firing),
 *                W A S D / right hold / right tap steer the hull toward that screen direction / spot
 *   HUD          left click on the air support box: air strike / paratroopers / recon (= F5 F6 F7)
 * The original keys (numpad, arrows, space, ...) keep working.
 *
 * How it works. Every frame the game's keyboard dispatcher (FUN_0041fca0) clears the player input
 * word and lets each pressed key's handler set bits in it (tables at 0x4d0710 / 0x4d0380):
 *     INPUT_FIRE 0x01 (held), INPUT_FIRE_PRESSED 0x10000 (this frame), INPUT_FORWARD 0x02,
 *     INPUT_BACK 0x04, INPUT_TURN_LEFT 0x10, INPUT_TURN_RIGHT 0x20, INPUT_ROLL 0x200
 * Sarge's update then reads that word. We wrap the dispatcher: after it runs we add our bits and
 * set Sarge's facing directly (the original turned by INPUT_TURN_* at a fixed rate).
 * The game binds A, S and D to squad / air-strike commands, so in modern mode W A S D are removed
 * from the keyboard state DirectInput hands the game (filter on the device's GetDeviceState).
 *
 * Sarge (unit record, DAT_00697520):
 *   +0x04 short x, +0x06 short y     position in world pixels
 *   +0x2c6 short / +0x88 / +0x74     facing 0..255 (also DAT_0069753c = facing * 256 + 8, 16 bit)
 *   +0x2d0                           vehicle he is in (0 = on foot)
 * Screen position = world - camera (DAT_004e2de8 / DAT_004e2dea, shorts; FUN_00404ca0).
 * Facing direction tables (doubles, 256 entries): x at 0x4ea778, y at 0x4e9f78 (FUN_0043eff0).
 * ============================================================================================ */
#include "common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define INPUT               (*(volatile DWORD *)0x6a547c)
#define INPUT_FIRE          0x00001
#define INPUT_FORWARD       0x00002
#define INPUT_BACK          0x00004
#define INPUT_TURN_LEFT     0x00010
#define INPUT_TURN_RIGHT    0x00020
#define INPUT_ROLL          0x00200
#define INPUT_FIRE_PRESSED  0x10000

#define SARGE               (*(BYTE **)0x697520)
#define SARGE_X(u)          (*(short *)((u) + 0x04))
#define SARGE_Y(u)          (*(short *)((u) + 0x06))
#define SARGE_FACING16      (*(volatile WORD *)0x69753c)
#define SARGE_VEHICLE(u)    (*(int *)((u) + 0x2d0))
#define CAMERA_X            (*(short *)0x4e2de8)
#define CAMERA_Y            (*(short *)0x4e2dea)
#define FACING_DX           ((const double *)0x4ea778)
#define FACING_DY           ((const double *)0x4e9f78)
#define FACINGS             256
#define MENU_CURSOR_SHOWN   (*(int *)0x4fe448)   /* the game shows its mouse cursor only in menus */
#define DI_KEYBOARD         (*(void ***)0x4fe47c) /* IDirectInputDevice* for the keyboard */

#define ARRIVE_DISTANCE     6     /* world pixels: "walk to spot" stops this close */
#define GOTO_TIMEOUT_MS     8000

/* ---------------------------------------------------------------- facing */

/* The facing whose direction is closest in ANGLE to (vx, vy). The game's direction tables are
 * filled at run time and need not be a unit circle (vectors of different length, e.g. squashed for
 * the view), so each is normalised; comparing raw dot products favoured the longer (diagonal)
 * vectors and made shots drift on diagonals. */
static double g_unit_dx[FACINGS], g_unit_dy[FACINGS];
static int g_units_ready;

static void prepare_facings(void)
{
    double min_len = 1e30, max_len = 0;
    for (int f = 0; f < FACINGS; f++) {
        double len = sqrt(FACING_DX[f] * FACING_DX[f] + FACING_DY[f] * FACING_DY[f]);
        if (len <= 0) return;                         /* tables not filled yet */
        g_unit_dx[f] = FACING_DX[f] / len;
        g_unit_dy[f] = FACING_DY[f] / len;
        if (len < min_len) min_len = len;
        if (len > max_len) max_len = len;
    }
    g_units_ready = 1;
    mlog("controls: facing tables: f0 (%.3f,%.3f) f32 (%.3f,%.3f) f64 (%.3f,%.3f) f192 (%.3f,%.3f); length %.3f..%.3f",
         FACING_DX[0], FACING_DY[0], FACING_DX[32], FACING_DY[32], FACING_DX[64], FACING_DY[64],
         FACING_DX[192], FACING_DY[192], min_len, max_len);
}

static int facing_toward(double vx, double vy)
{
    if (!g_units_ready) prepare_facings();
    const double *ux = g_units_ready ? g_unit_dx : FACING_DX, *uy = g_units_ready ? g_unit_dy : FACING_DY;
    int best = 0;
    double best_dot = -1e30;
    for (int f = 0; f < FACINGS; f++) {
        double d = ux[f] * vx + uy[f] * vy;
        if (d > best_dot) { best_dot = d; best = f; }
    }
    return best;
}

static int facing_of(BYTE *u) { return *(short *)(u + 0x2c6) & (FACINGS - 1); }

static void set_facing(BYTE *u, int f)
{
    f &= FACINGS - 1;
    *(short *)(u + 0x2c6) = (short)f;
    *(int *)(u + 0x88) = f;
    *(int *)(u + 0x74) = f;
    SARGE_FACING16 = (WORD)(f * 256 + 8);
}

/* ---------------------------------------------------------------- pointer */

/* Pointer position in game pixels, or 0 if it is not over the battlefield. */
/* The pointer in game coordinates anywhere inside the window (also over the HUD). */
static int pointer_in_window(int *x, int *y)
{
    HWND h = g_dd ? g_dd->hwnd : NULL;
    if (!h || GetForegroundWindow() != h) return 0;
    POINT p;
    RECT c;
    GetCursorPos(&p);
    ScreenToClient(h, &p);
    if (!GetClientRect(h, &c) || !PtInRect(&c, p)) return 0;
    view_client_to_game(&p);
    *x = p.x;
    *y = p.y;
    return 1;
}

/* Interface under a game-screen point (HUD panel, spare-weapon column, the mod's boxes). */
static int ui_at(int x, int y)
{
    return layout_on_hud_panel(x, y) || weapons_ui_at(x, y) || overlay_at(x, y) || panel_is_open() || layout_resize_icon_at(x, y) >= 0;
}
/* The same with HYSTERESIS (2026-11-17, user: moving between the interface and the battlefield was
 * jarring): the HUD's top edge is uneven (see-through panel art), so moving along it flipped the cursor
 * between the battlefield's and the interface's every few pixels. The state now only flips once the
 * pointer is UI_EDGE_BAND game px across the edge (all four neighbours agree); in between it stays. */
#define UI_EDGE_BAND 3
static int ui_at_steady(int x, int y)
{
    static int state;
    if (layout_resize_repeat_at(x, y)) { state = 1; return 1; }       /* just resized there: still interface */
    if (layout_resize_icon_at(x, y) >= 0) { state = 1; return 1; }   /* a resize icon is small: no hysteresis there */
    int here = ui_at(x, y);
    if (here == state) return state;
    int b = UI_EDGE_BAND;
    if (ui_at(x, y - b) == here && ui_at(x, y + b) == here && ui_at(x - b, y) == here && ui_at(x + b, y) == here) state = here;
    return state;
}

static int pointer_on_battlefield(int *x, int *y)
{
    HWND h = g_dd ? g_dd->hwnd : NULL;
    if (!h || GetForegroundWindow() != h) return 0;
    POINT p;
    RECT c;
    GetCursorPos(&p);
    ScreenToClient(h, &p);
    if (!GetClientRect(h, &c) || !PtInRect(&c, p)) return 0;
    view_client_to_game(&p);
    if (ui_at_steady(p.x, p.y)) return 0;                   /* HUD panel, spare weapons, the mod's boxes */
    *x = p.x;
    *y = p.y + layout_field_shift();                        /* battlefield moved up at the map's bottom */
    return 1;
}

/* bullets.c: the world point under the pointer (game picture + camera), 0 when it is not on the battlefield */
int controls_pointer_world(int *x, int *y)
{
    int px, py;
    if (!pointer_on_battlefield(&px, &py)) return 0;
    *x = px + CAMERA_X; *y = py + CAMERA_Y;
    return 1;
}

static int key_down(int vk) { return GetAsyncKeyState(vk) < 0; }

/* ---------------------------------------------------------------- keyboard filter */

/* Hide W A S D from the game: replace GetDeviceState (vtable slot 9) of the DirectInput device
 * IN PLACE. (Giving the object a copied vtable crashed on Windows: its DirectInput relies on the
 * object pointing at its own vtable.) The vtable may be shared with other devices, so only calls
 * for the game's keyboard are filtered. */
typedef HRESULT (WINAPI *GetDeviceStateFn)(void *dev, DWORD size, void *data);
static GetDeviceStateFn g_real_get_state;
static void **g_kb_vtable_patched;

/* Modern key layout, passed to the game as its OWN bindings (DirectInput scan codes):
 *   W A S D  hidden (our movement)        Shift  hidden (our sprint, controls_frame)
 *   Ctrl     -> Shift   = Slow            C      -> Num5    = Kneel/Prone
 *   Space    -> Num/    = Dive            F      -> E       = Enter/leave vehicle
 *   G        -> 3       = Special item    M      -> V       = View map
 *   J        -> O       = Objective       Q, X   hidden (quick swap, hold-X drop: controls_frame)
 * The original Fire keys (Ctrl, Space) are mouse buttons now; Defend Now (F), Change Gunsight (G),
 * Mouse Button 2 (M) and Gun Left/Right (Z/X) stay on their other keys (F10 etc.). pulse() presses a key
 * for a single read (hold-X drop -> F8 = Drop Item); g_force holds a key down while set (a held binding the
 * game must see, e.g. Slow on a shared key).
 * 2026-11-20: the keys are BINDINGS now (bindings.c, F10 panel BINDS page, ini [bindings]): every key above
 * is the default of an action and can be changed; kb_get_state hides the bound keys (bindings_hide). */
static volatile BYTE g_pulse[256];
static BYTE g_force[256];
static void pulse(int dik) { if (dik > 0 && dik < 256) g_pulse[dik] = 1; }

/* ---------------------------------------------------------------- the game's own key table, modern layout
 * The modern keys used to be remapped in kb_get_state (C -> Num5 etc.), which assumed the game's DEFAULT keys:
 * with the user's custom.cfg (Kneel = RShift, Fire = Space) C did nothing, and the pause menu's Controls screen
 * still listed the old keys (user 2026-11-17: "the pause menu needs to be updated with keys"). Now the modern
 * keys are written into the game's key table itself: Sarge 0x4d0710 (17 rows) and game 0x4d0380 (15 rows),
 * 24 bytes per row, keys at +0x14: [0] custom, [1] fixed, [2] default1, [3] default2; the dispatcher uses
 * custom + fixed; the Controls screen (FUN_00465ae0) shows them; custom.cfg (69 bytes, FUN_004611f0 load /
 * FUN_00461480 save) stores custom + fixed per row. On every return to gameplay our 7 keys are put on their rows
 * (the Controls screen's Typical / Alternative / Custom choice reloads the table) and rows whose custom key
 * collides with a modern key lose it (their fixed F-key stays); all other rows keep what the player set.
 * Classic mode puts the first-seen keys back on the rows we changed. */
#define SARGE_KEY_TABLE 0x4d0710
#define GAME_KEY_TABLE  0x4d0380
enum { SK_FORWARD, SK_LEFT, SK_RIGHT, SK_BACK, SK_ROLL_L, SK_ROLL_R, SK_DIVE, SK_KNEEL, SK_SLOW, SK_FIRE, SK_GUN_L,
       SK_GUN_R, SK_MAIN, SK_HEAVY, SK_SPECIAL, SK_ENTER, SK_DROP, SK_ROWS };
enum { GK_FOLLOW, GK_ATTACK, GK_DEFEND, GK_ATTACK_NOW, GK_DEFEND_NOW, GK_NEXT_UNIT, GK_AIR_STRIKE, GK_PARATROOP, GK_RECON,
       GK_SCROLL, GK_CHAT, GK_OBJECTIVE, GK_VIEW_MAP, GK_GUNSIGHT, GK_MOUSE_BUTTON, GK_ROWS };
static BYTE *key_row(int game, int row) { return (BYTE *)(intptr_t)((game ? GAME_KEY_TABLE : SARGE_KEY_TABLE) + row * 24 + 0x14); }

/* The game rows the modern layout puts a key on: the key is the action's keyboard binding (bindings.c; defaults
 * C Space Ctrl F G M J), worked out in controls_game_keys (g_row_dik; a spare key when the action has none - then
 * only its mouse binding / the stance code presses it). The stance row is always pressed by us (tap / hold /
 * double on C); the others are read by the game directly unless the binding needs us (bindings_direct). */
static const struct { BYTE game, row, act; } MODERN_GAME_KEYS[] = {
    { 0, SK_KNEEL,   ACT_KNEEL },     /* C      kneel / prone */
    { 0, SK_DIVE,    ACT_DIVE },      /* Space  dive */
    { 0, SK_SLOW,    ACT_SLOW },      /* Ctrl   slow (walk) */
    { 0, SK_ENTER,   ACT_ENTER },     /* F      enter / leave vehicle */
    { 0, SK_SPECIAL, ACT_SPECIAL },   /* G      special item */
    { 1, GK_VIEW_MAP,  ACT_MAP },     /* M      map */
    { 1, GK_OBJECTIVE, ACT_OBJECTIVE },   /* J      objective */
};
#define N_MODERN_GAME_KEYS ((int)(sizeof MODERN_GAME_KEYS / sizeof *MODERN_GAME_KEYS))
static BYTE g_row_dik[N_MODERN_GAME_KEYS] = { 0x2e, 0x39, 0x1d, 0x21, 0x22, 0x32, 0x24 };
static BYTE g_key_first[2][SK_ROWS][2], g_key_wrote[2][SK_ROWS][2];
static int g_keys_seen, g_keys_modern;
#define KEY_NAMES 0x4cf940          /* char *[256] DIK -> name (FUN_004609a0); NULL / "" = "undefined" */

/* keys the mod itself uses in modern mode: every active keyboard binding (bindings.c: W A S D, 1-6, Q, X, Shift,
 * the feature keys ...) and the keys on the game rows above */
static int modern_reserved_key(int dik)
{
    if (bindings_reserved_dik(dik)) return 1;
    for (int i = 0; i < N_MODERN_GAME_KEYS; i++) if (g_row_dik[i] == dik) return 1;
    return 0;
}

static int key_in_use(int dik)
{
    int rows[2] = { SK_ROWS, GK_ROWS };
    for (int t = 0; t < 2; t++) for (int r = 0; r < rows[t]; r++) { BYTE *k = key_row(t, r); if (k[0] == dik || k[1] == dik) return 1; }
    return 0;
}

/* A key nobody presses for a row that lost its key: NEVER 0 - the Controls screen then draws the 9-letter
 * "undefined" into the Sarge column's text field, which corrupted the heap (crash in Wine 2026-11-17). */
static int spare_key(void)
{
    static const BYTE SPARE[] = { 0xd2, 0xc7, 0xcf, 0xc9, 0xd1, 0xd3, 0xb8, 0x9c };   /* Insert Home End PgUp PgDn Delete RAlt NumEnter */
    for (size_t i = 0; i < sizeof SPARE; i++) {
        const char *name = *(const char **)(intptr_t)(KEY_NAMES + SPARE[i] * 4);
        if (name && *name && !key_in_use(SPARE[i]) && !modern_reserved_key(SPARE[i])) return SPARE[i];
    }
    return 0xcf;
}

/* The keyboard dispatcher (FUN_0041fca0) does NOT read the key table: it walks binding entries (Sarge
 * 0x4d0500, game 0x4d0110; 24 bytes: +0 key, +4 handler, ...; a handler can have several entries: the table's
 * key and its FIXED key). SelectKeyBindings (FUN_00461a20, ecx = config 0 Typical / 1 Alternative / 2 Custom;
 * current one at 0x6a5478) copies the chosen slot of each table row into the first entry with that handler
 * (0 when it equals the fixed key) - and also writes the game's registry value "Config", so it is not called:
 * dispatch_from_table does the same copy. Entries of OTHER commands that hold one of our keys (Fire's fixed
 * Space!) are cleared and remembered for classic mode. (Found 2026-11-17: the table alone changed only the
 * Controls screen - a C press did not kneel in Wine.) */
#define SARGE_DISPATCH 0x4d0500
#define GAME_DISPATCH  0x4d0110
#define KEY_CONFIG     (*(int *)0x6a5478)
static BYTE *g_cleared[64]; static BYTE g_cleared_key[64]; static int g_ncleared;

static void dispatch_from_table(int slot)
{
    int rows[2] = { SK_ROWS, GK_ROWS };
    for (int t = 0; t < 2; t++) {
        BYTE *list = (BYTE *)(intptr_t)(t ? GAME_DISPATCH : SARGE_DISPATCH);
        for (int r = 0; r < rows[t]; r++) {
            BYTE *k = key_row(t, r);
            DWORD handler = *(DWORD *)(k - 0x14);
            for (BYTE *e = list; *(DWORD *)(e + 4); e += 24)
                if (*(DWORD *)(e + 4) == handler) { e[0] = k[slot] == k[1] ? 0 : k[slot]; break; }
        }
    }
}

static void dispatch_modern(int modern)
{
    for (int i = 0; i < g_ncleared; i++) if (g_cleared[i][0] == 0) g_cleared[i][0] = g_cleared_key[i];   /* put back first */
    g_ncleared = 0;
    if (!modern) {
        int cfg = KEY_CONFIG;
        dispatch_from_table(cfg == 0 ? 2 : cfg == 1 ? 3 : 0);
        return;
    }
    dispatch_from_table(0);                                      /* the table's custom slot = the modern layout */
    for (int t = 0; t < 2; t++) {
        BYTE *list = (BYTE *)(intptr_t)(t ? GAME_DISPATCH : SARGE_DISPATCH);
        for (BYTE *e = list; *(DWORD *)(e + 4); e += 24) {
            if (!e[0] || !modern_reserved_key(e[0])) continue;
            int ours = 0;                                        /* the entry of the command that owns this key */
            for (int i = 0; i < N_MODERN_GAME_KEYS; i++) {
                BYTE *k = key_row(MODERN_GAME_KEYS[i].game, MODERN_GAME_KEYS[i].row);
                if (g_row_dik[i] == e[0] && MODERN_GAME_KEYS[i].game == t && *(DWORD *)(k - 0x14) == *(DWORD *)(e + 4)) ours = 1;
            }
            if (ours || g_ncleared >= 64) continue;
            g_cleared[g_ncleared] = e; g_cleared_key[g_ncleared++] = e[0];
            e[0] = 0;
        }
    }
    mlog("controls: dispatcher keys rebuilt (modern), %d colliding bindings cleared", g_ncleared);
}

void controls_game_keys(int modern)
{
    int rows[2] = { SK_ROWS, GK_ROWS };
    if (!g_keys_seen) {
        for (int t = 0; t < 2; t++) for (int r = 0; r < rows[t]; r++) for (int s = 0; s < 2; s++)
            g_key_first[t][r][s] = g_key_wrote[t][r][s] = key_row(t, r)[s];
        g_keys_seen = 1;
    }
    int changed = 0;
    if (modern) {                                                  /* the rows' keys from the bindings (spare if none) */
        for (int i = 0; i < N_MODERN_GAME_KEYS; i++) { BYTE d[2]; g_row_dik[i] = bindings_key_diks(MODERN_GAME_KEYS[i].act, d) ? d[0] : 0; }
        for (int i = 0; i < N_MODERN_GAME_KEYS; i++) if (!g_row_dik[i]) g_row_dik[i] = (BYTE)spare_key();
    }
    for (int t = 0; t < 2; t++)
        for (int r = 0; r < rows[t]; r++) {
            BYTE *k = key_row(t, r);
            int ours = -1;
            for (int i = 0; i < N_MODERN_GAME_KEYS; i++)
                if (MODERN_GAME_KEYS[i].game == t && MODERN_GAME_KEYS[i].row == r) ours = g_row_dik[i];
            int want[2] = { k[0], k[1] };
            if (modern) {
                if (want[1] && modern_reserved_key(want[1]) && want[1] != ours) want[1] = 0;   /* fixed slot: blank is safe */
                if (ours >= 0) want[0] = ours;                                 /* our 7 keys: always */
                else if (!want[0] || modern_reserved_key(want[0]))            /* collides: its fixed key, else a spare */
                    want[0] = want[1] ? want[1] : spare_key();
            } else {
                for (int s = 0; s < 2; s++) if (k[s] == g_key_wrote[t][r][s]) want[s] = g_key_first[t][r][s];   /* ours: back */
            }
            for (int s = 0; s < 2; s++) {
                if (k[s] != want[s]) { k[s] = (BYTE)want[s]; changed++; }
                g_key_wrote[t][r][s] = (BYTE)want[s];
            }
        }
    dispatch_modern(modern);                                       /* what the game actually reacts to */
    if (changed || modern != g_keys_modern) mlog("controls: game key table %s (%d keys changed)", modern ? "set to the modern layout" : "back to the player's keys", changed);
    g_keys_modern = modern;
}

/* the key the game itself reacts to for a command (custom, else fixed): for pulsed commands (roll, gamepad) */
static BYTE game_command_key(int game, int row)
{
    BYTE *k = key_row(game, row);
    return k[0] ? k[0] : k[1] ? k[1] : k[2];
}

/* bullets.c stance hints: the keys that change Sarge's stance now - one step down (the game's Kneel/Prone command:
 * modern C, classic the player's key) and one step up (forward input while kneeling / prone, FUN_00447290: modern W,
 * classic the player's forward key). Upper case, for modern.c's small font. */
static void dik_name(BYTE dik, char *s, int n)
{
    LONG lp = (LONG)(dik & 0x7f) << 16 | (dik & 0x80 ? 1L << 24 : 0);
    if (!dik || !GetKeyNameTextA(lp, s, n)) wsprintfA(s, "KEY %d", dik);
    CharUpperA(s);
}
void controls_stance_keys(char *down, char *up, int n)
{
    if (g_cfg.controls_modern) { lstrcpynA(down, "C", n); lstrcpynA(up, "W", n); return; }
    dik_name(game_command_key(0, SK_KNEEL), down, n);
    dik_name(game_command_key(0, SK_FORWARD), up, n);
}

static HRESULT WINAPI kb_get_state(void *dev, DWORD size, void *data)
{
    HRESULT hr = g_real_get_state(dev, size, data);
    if (hr == S_OK && size >= 256 && dev == (void *)DI_KEYBOARD) {
        BYTE *keys = data;
        /* a key used on the F10 panel stays away from the game until it is let go: the Esc that closes the panel opened
         * the game's pause menu one frame later (Wine 2026-11-20) */
        static BYTE swallow[256];
        int open = panel_is_open();
        static int was_open; static DWORD closed_at;
        if (was_open && !open) closed_at = GetTickCount();
        was_open = open;
        int just_closed = !open && closed_at && GetTickCount() - closed_at < 300;   /* the closing key went to the panel as a message */
        for (int i = 0; i < 256; i++) {
            if ((open || just_closed) && (keys[i] & 0x80)) swallow[i] = 1;
            else if (swallow[i] && !(keys[i] & 0x80)) swallow[i] = 0;
            if (swallow[i]) keys[i] = 0;
        }
        if (open) {                                              /* the F10 panel takes the keys (BINDS page: arrows, Enter, capture) */
            memset(keys, 0, 256);
            for (int i = 0; i < 256; i++) g_pulse[i] = 0;
        } else if (g_cfg.controls_modern) {
            /* the bound keys (bindings.c: W A S D, 1-6, Q, X, Shift, C, the feature keys ...) are ours; game rows the
             * game reads itself (Space dive, Ctrl slow, F G M J by default) stay (controls_game_keys put them in its table) */
            bindings_hide(keys);
            for (int i = 0; i < 256; i++) {
                if (g_force[i]) keys[i] = 0x80;
                if (g_pulse[i]) { keys[i] = 0x80; g_pulse[i] = 0; }
            }
        }
    }
    return hr;
}

static void filter_keyboard(void)
{
    void **dev = DI_KEYBOARD;
    if (!dev) return;
    void **vtable = *(void ***)dev;
    if (vtable == g_kb_vtable_patched || vtable[9] == (void *)kb_get_state) return;
    DWORD old;
    if (!VirtualProtect(&vtable[9], sizeof(void *), PAGE_READWRITE, &old)) {
        mlog("controls: could not hide W A S D (VirtualProtect failed)");
        g_kb_vtable_patched = vtable;
        return;
    }
    g_real_get_state = (GetDeviceStateFn)vtable[9];
    vtable[9] = (void *)kb_get_state;
    VirtualProtect(&vtable[9], sizeof(void *), old, &old);
    g_kb_vtable_patched = vtable;
    mlog("controls: W A S D hidden from the game's keyboard bindings");
}

static void confine_pointer(int want);

/* ---------------------------------------------------------------- per frame */

static struct {
    int active;                     /* gameplay controls were running last frame */
    int left_blocked, right_blocked;/* held when gameplay started: ignored until released */
    int right_was_down, left_was_down;
    DWORD right_down_at, right_up_at;
    int running;                    /* double-click right and hold: Sarge runs (controls_move) */
    int left_capture, right_capture;/* button pressed over the battlefield, still held */
    int firing, pressing;           /* left button down; fire input currently held */
    DWORD shot_at;                  /* start of the current press */
    DWORD released_at;              /* when the last press was released (= the shot) */
    int saw_shot;                   /* the shot animation started after the last release */
    int charging;                   /* holding fire with a hold-to-aim weapon */
    DWORD throw_started_at, throw_released_at;   /* throw at the pointer: first release, last release (retries) */
    int route_run;                  /* the walk-to-spot was double-clicked: sprint along it */
    int cursor_x, cursor_y, have_cursor;  /* last pointer position on the battlefield (reticle) */
    int going;                      /* walking (or driving) to target_x/y */
    BYTE *enter;                    /* ... to climb into this vehicle (right click on it) */
    int target_x, target_y;
    DWORD going_since;
    int last_x, last_y, stuck, detour;  /* obstacle steering for walk-to-spot */
    DWORD check_at, detour_until;
    POINT wp[32]; int nwp, wpi;         /* route of the walk-to-spot (pathfind.c); nwp 0 = straight */
    /* one-button mouse ([controls] mouse = am2) */
    int lmode;                          /* what the current left press does: LM_* */
    DWORD lpress_at, lup_at;            /* left press / last release (double-click + hold = run) */
    int lpress_x, lpress_y;             /* pointer (game screen) at the press */
    DWORD fire_end_at;                  /* last time a fire press ended: no walking for 300 ms after */
} S;
enum { LM_NONE, LM_PENDING, LM_WALK, LM_FIRE, LM_ENTER, LM_TURN, LM_PICK };   /* LM_PICK: the drag stance picker */
#define WALK_LOCK_AFTER_SHOT_MS  300    /* Army Men II: no walking right after a shot (mis-clicks) */
static DWORD g_fire_cancel_at, g_fire_lock_until, g_click_hint_at;
/* modern.c: the rifle fires once per click - the button is held after its shot (2026-11-20, user: "the regular rifle
 * needs to indicate you need to keep clicking to shoot") */
int controls_click_hint(void) { return g_click_hint_at && GetTickCount() - g_click_hint_at < 120; }
/* modern.c: the pointer indicator - fire lock left (ms, 0 = none), just cancelled */
void controls_fire_state(int *lock_ms_left, int *cancelled)
{
    DWORD now = GetTickCount();
    *lock_ms_left = g_fire_lock_until && g_fire_lock_until > now ? (int)(g_fire_lock_until - now) : 0;
    *cancelled = g_fire_cancel_at && now - g_fire_cancel_at < 700;
}
static int combat_lock_ms(void)
{
    static int v = -1;
    if (v < 0) { v = GetPrivateProfileIntA("controls", "combat_lock_ms", 0, mod_ini_path());   /* 0 = off (2026-11-20: replaced by the firing zone, user) */ if (v <= 0) v = WALK_LOCK_AFTER_SHOT_MS; }
    return v;
}
#define TURN_ONLY_RADIUS         33     /* a hold this close to Sarge, in its first 500 ms: turn only */
#define TURN_ONLY_MS             500

/* Firing. Army Men 1 fires when the fire input is RELEASED (held = raise the rifle, Sarge state 4;
 * release = shoot, state 7). For "shoot on click, keep shooting while held" the fire input is pulsed:
 * held for FIRE_PRESS_MS, then released (= the shot). While the button stays down it presses again as
 * soon as the shot animation (state 7) is over - as fast as rapid clicking - plus an optional extra
 * delay, [controls] fire_interval_ms. The first shot comes FIRE_PRESS_MS after the click. */
#define FIRE_PRESS_MS 40

/* Weapon kinds. Types 2 and 5 are hold-to-aim weapons (grenades and the like): while fire is held
 * the game swings a charge 0..100% (FUN_0043e6e0) that sets the throw distance between the weapon's
 * min and max range (FUN_0043eef0, table at 0x50504c), shows it with the reticle, and throws on
 * release. For those the left button is passed straight through and the reticle is left alone, so
 * the game's own range display works. All other weapons fire straight ahead and use the pulse. */
#define SARGE_WEAPON_TYPE(u) (*(int **)((u) + 0x20) ? **(int **)((u) + 0x20) : -1)

static int g_throw_pending, g_throw_wx, g_throw_wy;   /* throw at the pointer: the clicked world spot */
static DWORD g_throw_cancel_until;                    /* ... and after both are up, held until then (or the aim ended) */
static int g_throw_cancel;                            /* a charge / throw was cancelled (both buttons): no throw until both are up */
static int weapon_is_hold_to_aim(BYTE *u)
{
    int t = SARGE_WEAPON_TYPE(u);
    return t == 2 || t == 5;
}

#define SARGE_STATE(u)    (*(int *)((u) + 0x68))
#define STATE_SHOOTING    7     /* the shot animation; a new press only counts after it */
#define SHOT_TIMEOUT_MS   400   /* press again anyway if no shot animation was seen (e.g. no ammo) */
#define WEAPON_BAZOOKA_TYPE 4   /* ARMY_BAZOOKA (game table) */
#define WEAPON_RIFLE_TYPE   9   /* ARMY_RIFLE (the auto rifle is 10) */
static int rifle_semi_auto(void)
{
    static int v = -1;
    if (v < 0) v = GetPrivateProfileIntA("controls", "rifle_semi_auto", 1, mod_ini_path());
    return v;
}

static void fire_pulse(BYTE *u, int left, DWORD now)
{
    if (!left) { S.firing = 0; S.pressing = 0; return; }
    if (!S.firing) {                                                   /* click down: press now */
        S.firing = 1;
        S.shot_at = now;
        S.saw_shot = 0;
    } else if (!S.pressing && (SARGE_WEAPON_TYPE(u) == WEAPON_BAZOOKA_TYPE || (SARGE_WEAPON_TYPE(u) == WEAPON_RIFLE_TYPE && rifle_semi_auto()))) {
        if (SARGE_WEAPON_TYPE(u) == WEAPON_RIFLE_TYPE && now - S.released_at > 200) g_click_hint_at = now;   /* held: "click" hint */
        /* the bazooka: one rocket per click (a click held a moment fired a second one - user 2026-11-17).
         * The RIFLE too (2026-11-20, user: "the rifle and auto rifle seem the same with the mouse controls. want the
         * regular rifle to behave more like the original"): one shot per click, as the original's press-and-release;
         * the auto rifle (and machine guns) keep firing while held. [controls] rifle_semi_auto=1 */
    } else if (!S.pressing) {
        /* held: press again once the shot animation has started and finished */
        if (SARGE_STATE(u) == STATE_SHOOTING) S.saw_shot = 1;
        int shot_done = (S.saw_shot && SARGE_STATE(u) != STATE_SHOOTING) ||
                        now - S.released_at >= SHOT_TIMEOUT_MS;
        if (shot_done && now - S.released_at >= (DWORD)g_cfg.controls_fire_interval_ms) {
            S.shot_at = now;
            S.saw_shot = 0;
        }
    }
    if (now - S.shot_at < FIRE_PRESS_MS) {
        INPUT |= INPUT_FIRE;
        if (!S.pressing) INPUT |= INPUT_FIRE_PRESSED;
        S.pressing = 1;
    } else if (S.pressing) {
        if (g_cfg.log >= 2) mlog("controls: fire released (shot) at +%lu ms", now - S.shot_at);
        S.pressing = 0;                                                /* released: the shot */
        S.released_at = now;
    }
}

static int sarge_blocked_at(int x, int y);

/* Smoother walking: if the way ahead (8 and 14 px) is blocked, turn to the nearest free direction
 * (up to 90 degrees either side, smaller turns first), so Sarge glides along walls and around
 * corners instead of stopping dead. Inside a spot the game already counts as blocked (building
 * interiors) nothing is changed - the game lets him move freely there. */
static int free_facing(BYTE *u, int f)
{
    if (!g_units_ready) prepare_facings();
    int x = SARGE_X(u), y = SARGE_Y(u);
    if (sarge_blocked_at(x, y)) return f;
    for (int k = 0; k <= 64; k += 8)
        for (int side = 1; side >= -1; side -= 2) {
            int g = (f + side * k) & (FACINGS - 1);
            double dx = g_units_ready ? g_unit_dx[g] : 0, dy = g_units_ready ? g_unit_dy[g] : 0;
            /* every px up to 14 (2026-11-20: two samples at 8 / 14 px passed through holes and ragged edges of
             * sandbag / fence masks; the game refused the first frame's step and Sarge stood wedged) */
            int i = 1;
            while (i <= 14 && !sarge_blocked_at(x + (int)(dx * i), y + (int)(dy * i))) i++;
            if (i > 14) return g;
            if (!k) break;
        }
    return f;                                    /* boxed in: let the game stop him */
}

/* Walking turns are interpolated: the facing moves toward the wanted one at TURN_RATE facing units
 * per millisecond (256 = full circle; a half turn takes ~150 ms), so Sarge turns through the
 * in-between sprite directions and his path curves instead of kinking. Firing still snaps (aim). */
#define TURN_RATE 0.85
static void turn_toward(BYTE *u, int target)
{
    static double pos;                          /* facing with its fraction */
    static LARGE_INTEGER last, freq;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    double dt = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart : 0;
    last = now;
    if (dt > 60) dt = 60;
    int cur = facing_of(u);
    if (fabs(pos - cur) > 1.5) pos = cur;       /* turned by something else (aiming, the game) */
    double diff = (double)((((target - cur) & (FACINGS - 1)) + FACINGS / 2) & (FACINGS - 1)) - FACINGS / 2;
    double step = TURN_RATE * dt;
    if (fabs(diff) <= step || dt <= 0) pos = target;
    else pos += diff > 0 ? step : -step;
    if (pos < 0) pos += FACINGS;
    if (pos >= FACINGS) pos -= FACINGS;
    set_facing(u, (int)floor(pos + 0.5));
    pos = fmod(pos, FACINGS);
}

static void walk_toward(BYTE *u, double vx, double vy)
{
    fences_walk(u, facing_toward(vx, vy), 0);       /* [gameplay] break_fences: pushing against a wooden fence breaks it (fences.c) */
    turn_toward(u, free_facing(u, facing_toward(vx, vy)));
    INPUT = (INPUT & ~(INPUT_BACK | INPUT_TURN_LEFT | INPUT_TURN_RIGHT)) | INPUT_FORWARD;
}

/* Walk to the clicked spot, steering around obstacles in a simple way: every STUCK_CHECK_MS check
 * whether Sarge actually moved; if not, walk at an angle for DETOUR_MS (alternating sides, wider
 * each time: 45, 90, 135 degrees), then aim at the target again. Gives up after MAX_DETOURS. */
#define STUCK_CHECK_MS  300
#define DETOUR_MS       1000   /* Army Men II keeps a detour for 1 s: no back-and-forth along fences */
#define MAX_DETOURS     8

static void walk_around_obstacles(BYTE *u, int vx, int vy, DWORD now)
{
    int x = SARGE_X(u), y = SARGE_Y(u);
    if (now - S.check_at >= STUCK_CHECK_MS) {
        int moved = abs(x - S.last_x) + abs(y - S.last_y);
        if (moved < 3 && S.check_at) {
            S.stuck++;
            S.detour_until = now + DETOUR_MS;
            S.detour = (S.stuck & 1 ? 1 : -1) * 32 * ((S.stuck + 1) / 2 > 3 ? 3 : (S.stuck + 1) / 2);
            if (S.stuck > MAX_DETOURS) { S.going = 0; return; }
            if (g_cfg.log >= 2) mlog("controls: walk-to-spot blocked at %d,%d, detour %+d (try %d)", x, y, S.detour, S.stuck);
        } else if ((LONG)(now - S.detour_until) > 0) {
            S.stuck = 0;
        }
        S.last_x = x; S.last_y = y; S.check_at = now;
    }
    fences_walk(u, facing_toward(vx, vy), 1);       /* break_fences: a fence on the click route breaks as he reaches it (fences.c) */
    int f = facing_toward(vx, vy);
    if ((LONG)(now - S.detour_until) < 0) f += S.detour;
    turn_toward(u, free_facing(u, f));
    INPUT = (INPUT & ~(INPUT_BACK | INPUT_TURN_LEFT | INPUT_TURN_RIGHT)) | INPUT_FORWARD;
}

/* ---------------------------------------------------------------- vehicles
 * Vehicle record (Sarge +0x2d0; the player's vehicles are listed in the army record FUN_0041a160(0):
 * +0x50 count (byte), +0x54 pointers):
 *   +0x04 / +0x06 short x / y (world)     +0x08 short hull direction 0..255 (set on deployment)
 *   +0x1c short health                     +0x68 1 = usable    +0xe8 driver (0 = empty)
 *   +0x9c mover object (+0x1c..+0x28 int bounding box x1 y1 x2 y2; +0xe4 the turret sprite)
 *   +0xc2 short turret direction           +0xc9 byte: has a turret
 * Type = ((*v >> 16) & 0x3f); type 4 can only aim its gun 0x20 either side of the hull.
 * The game drives a vehicle from the same input word (FUN_004182e0): FORWARD / BACK, TURN_LEFT /
 * TURN_RIGHT (hull), 0x40 / 0x80 (turret, 2 steps per 60 ms), FIRE held = keep firing.
 * Entering: FUN_00418910 (the Enter Vehicle key) picks the nearest usable vehicle within 6 tiles and
 * calls FUN_00448ae0 (__fastcall ecx unit, edx vehicle); not while DAT_004b8a88 (command cursor) is set. */
#define VEH_X(v)            (*(short *)((v) + 0x04))
#define VEH_Y(v)            (*(short *)((v) + 0x06))
#define VEH_HULL(v)         (*(short *)((v) + 0x08) & (FACINGS - 1))
#define VEH_USABLE(v)       (*(int *)((v) + 0x68) == 1)
#define VEH_DRIVER(v)       (*(int *)((v) + 0xe8))
#define VEH_MOVER(v)        (*(BYTE **)((v) + 0x9c))
#define VEH_TURRET(v)       (*(short *)((v) + 0xc2))
#define VEH_HAS_TURRET(v)   (*(BYTE *)((v) + 0xc9))
#define VEH_TYPE(v)         ((*(int *)(v) >> 16) & 0x3f)
#define VEH_TYPE_FIXED_GUN  4
#define INPUT_TURRET_LEFT   0x40
#define INPUT_TURRET_RIGHT  0x80
#define COMMAND_CURSOR      (*(volatile char *)0x4b8a88)   /* 0 = Sarge, 1 = command cursor (air strike ...) */
#define COMMAND_CLICK       0x441000      /* the command cursor's battlefield click (__fastcall ecx 1 = confirm) */
#define COMMAND_TOGGLE      0x43f6c0      /* command cursor on / off (__fastcall ecx 0) */
#define ENTER_DISTANCE      40      /* world pixels: close enough to climb in */
#define ENTER_MARGIN        20      /* world pixels: or this close to the vehicle's box (mover rect) */
#define TURRET_STEP         8       /* turret: at most this many direction steps ... */
#define TURRET_STEP_MS      60      /* ... per this many ms (the original mouse turret speed) */
#define STEER_DEAD          6       /* hull: no turning within this many steps of the heading (vehicles turn 8 per
                                       frame: 3 made a diagonal heading overshoot left / right - halftrack, user) */
#define STEER_DRIVE         40      /* hull: drive only while the heading is within this many steps */
#define STEER_KEEP          72      /* hull: once driving, keep driving while within this many steps */

typedef BYTE *(__fastcall *ArmyFn)(int army);
typedef void (__fastcall *EnterVehicleFn)(BYTE *unit, BYTE *vehicle);
typedef void (__fastcall *TurretSpriteFn)(BYTE *sprite, int direction);
#define ARMY_RECORD         ((ArmyFn)0x41a160)
#define ENTER_VEHICLE       ((EnterVehicleFn)0x448ae0)
#define SET_TURRET_SPRITE   ((TurretSpriteFn)0x4146c0)

static int wrap_dir(int d) { d &= FACINGS - 1; return d >= FACINGS / 2 ? d - FACINGS : d; }   /* -128..127 */

/* The player's usable, empty vehicle under the world point (wx, wy), or NULL. With [gameplay]
 * drive_enemy_vehicles also another army's empty vehicle near Sarge (capture.c). */
static BYTE *vehicle_at(int wx, int wy)
{
  for (int a = 0; a < 4; a++) {
    if (a && !capture_allowed()) break;
    BYTE *army = ARMY_RECORD(a);
    if (!army) continue;
    int n = *(signed char *)(army + 0x50);
    for (int i = 0; i < n && i < 50; i++) {
        BYTE *v = *(BYTE **)(army + 0x54 + 4 * i);
        if (!v || (a ? !capture_hover_ok(v) : (!VEH_USABLE(v) || VEH_DRIVER(v)))) continue;
        RECT r = { VEH_X(v) - 16, VEH_Y(v) - 16, VEH_X(v) + 16, VEH_Y(v) + 16 };
        BYTE *m = VEH_MOVER(v);
        if (m && *(int *)(m + 0x24) > *(int *)(m + 0x1c) && *(int *)(m + 0x28) > *(int *)(m + 0x20))
            r = (RECT){ *(int *)(m + 0x1c), *(int *)(m + 0x20), *(int *)(m + 0x24), *(int *)(m + 0x28) };
        InflateRect(&r, 4, 4);
        POINT p = { wx, wy };
        if (PtInRect(&r, p)) return v;
    }
  }
    return NULL;
}

static int try_enter_vehicle(BYTE *u, BYTE *v)
{
    if (COMMAND_CURSOR || SARGE_VEHICLE(u) || !capture_can_enter(v)) return 0;   /* own: usable, not AI driven */
    int dx = VEH_X(v) - SARGE_X(u), dy = VEH_Y(v) - SARGE_Y(u);
    /* 2026-11-20 (user: "i can't enter the vehicle now via mouse click"): the walk now ends at the free point on the
     * near side, just outside the vehicle's outline - for a tank / halftrack that is further than ENTER_DISTANCE from
     * its centre. Close enough = within ENTER_DISTANCE of the centre OR within ENTER_MARGIN of its box (VEH_MOVER). */
    int close_enough = dx * dx + dy * dy <= ENTER_DISTANCE * ENTER_DISTANCE;
    BYTE *m = VEH_MOVER(v);
    if (!close_enough && m && *(int *)(m + 0x24) > *(int *)(m + 0x1c) && *(int *)(m + 0x28) > *(int *)(m + 0x20)) {
        int sx = SARGE_X(u), sy = SARGE_Y(u);
        int ox = sx < *(int *)(m + 0x1c) ? *(int *)(m + 0x1c) - sx : sx > *(int *)(m + 0x24) ? sx - *(int *)(m + 0x24) : 0;
        int oy = sy < *(int *)(m + 0x20) ? *(int *)(m + 0x20) - sy : sy > *(int *)(m + 0x28) ? sy - *(int *)(m + 0x28) : 0;
        close_enough = ox <= ENTER_MARGIN && oy <= ENTER_MARGIN;
    }
    if (!close_enough) return 0;
    ENTER_VEHICLE(u, v);
    if (g_cfg.log >= 2) mlog("controls: entered vehicle %p (type %d) by right click", v, VEH_TYPE(v));
    return 1;
}

/* Turret toward the pointer, at the original mouse-turret speed. */
static void aim_turret(BYTE *v, int want, DWORD now)
{
    static DWORD last;
    static double budget;
    if (!VEH_HAS_TURRET(v) || !VEH_MOVER(v)) return;
    budget += (double)(now - last) * TURRET_STEP / TURRET_STEP_MS;
    last = now;
    if (budget > TURRET_STEP) budget = TURRET_STEP;
    if (VEH_TYPE(v) == VEH_TYPE_FIXED_GUN) {              /* gun limited to 0x20 either side of the hull */
        int rel = wrap_dir(want - VEH_HULL(v));
        if (rel > 0x20) rel = 0x20;
        if (rel < -0x20) rel = -0x20;
        want = VEH_HULL(v) + rel;
    }
    int cur = VEH_TURRET(v) & (FACINGS - 1), d = wrap_dir(want - cur);
    if (!d) return;
    int step = (int)budget;
    if (step <= 0) return;
    if (d > step) d = step;
    if (d < -step) d = -step;
    budget -= abs(d);
    int dir = (cur + d) & (FACINGS - 1);
    VEH_TURRET(v) = (short)dir;
    SET_TURRET_SPRITE(VEH_MOVER(v) + 0xe4, dir);
}

/* Hull toward a heading: turn (and drive forward once roughly facing it). The direction the turn
 * bits move the hull is learned from the first turn (assumed: TURN_LEFT = higher direction). */
static void steer_vehicle(BYTE *v, int heading, int drive)   /* drive: 1 forward, -1 backward (rear toward heading) */
{
    if (drive < 0) heading = (heading + FACINGS / 2) & (FACINGS - 1);
    static int sign = 1, last_hull = -1, last_turn, learned;
    int hull = VEH_HULL(v);
    if (last_turn && last_hull >= 0 && hull != last_hull && !learned) {
        int moved = wrap_dir(hull - last_hull);
        if ((moved > 0) != (last_turn * sign > 0)) sign = -sign;
        learned = 1;
        mlog("controls: vehicle hull turns %s with TURN_LEFT", sign > 0 ? "up (counter-clockwise)" : "down (clockwise)");
    }
    int d = wrap_dir(heading - hull), turn = 0;
    if (d > STEER_DEAD) turn = sign;
    else if (d < -STEER_DEAD) turn = -sign;
    INPUT &= ~(INPUT_FORWARD | INPUT_BACK | INPUT_TURN_LEFT | INPUT_TURN_RIGHT);
    if (turn > 0) INPUT |= INPUT_TURN_LEFT;
    if (turn < 0) INPUT |= INPUT_TURN_RIGHT;
    /* drive with hysteresis: start within STEER_DRIVE, keep going up to STEER_KEEP (the throttle flickered on / off
     * at the limit: "vehicles moving forward back and forth" - user 2026-11-17) */
    static int driving;
    int lim = driving ? STEER_KEEP : STEER_DRIVE;
    driving = drive != 0 && abs(d) <= lim;
    if (drive > 0 && driving) INPUT |= INPUT_FORWARD;
    if (drive < 0 && driving) INPUT |= INPUT_BACK;
    last_hull = hull;
    last_turn = turn;
}

#define REVERSE_FROM  96         /* pointer more than ~135 deg from the heading: reverse */
#define VEH_CORNER_REACHED 28    /* px: a route corner counts as reached this close (vehicles turn wide) */
#define VEH_ROUTE_TIMEOUT_MS 30000 /* a planned vehicle route may take this long */
#define VEH_ARRIVE 36              /* px: a vehicle has arrived this close to the clicked spot */
#define VEH_ARRIVE_BEHIND 80       /* ... or this close with the spot already behind it */
#define VEH_EXIT_RADIUS 24         /* px around the vehicle's centre: a click there gets out */
#define REVERSE_UNTIL 64         /* ... until it is within ~90 deg again */
#define REVERSE_FLIP_MS 1200     /* backing up longer than this: turn round when there is room (vehicles_turn_room) */
#define REVERSE_ROOM_EVERY_MS 200  /* ... room tested this often while it keeps backing up */
#define VEH_UTURN_FROM 64        /* facings: a jeep turning round forward steers this far to its side (as vehicles.c) */
/* The pointer (screen) on the vehicle's middle: the get-out click / pointer. The game's position is the box's
 * hotspot (a jeep's is near its rear: front 57, back 7 - vehiclesizes 0x4b2b20, index VEH_TYPE - 1), so the
 * middle lies (front - back) / 2 along the hull. */
static int on_own_vehicle(BYTE *v, int px, int py)
{
    int t = VEH_TYPE(v) - 1;
    const short *sz = (const short *)(intptr_t)(0x4b2b20 + 8 * (t >= 0 && t < 6 ? t : 0));
    int h = VEH_HULL(v);
    double cx = VEH_X(v) + FACING_DX[h] * (sz[0] - sz[1]) / 2.0, cy = VEH_Y(v) + FACING_DY[h] * (sz[0] - sz[1]) / 2.0;
    double dx = px + CAMERA_X - cx, dy = py + CAMERA_Y - cy;
    return dx * dx + dy * dy <= VEH_EXIT_RADIUS * VEH_EXIT_RADIUS;
}

static void vehicle_controls(BYTE *u, BYTE *v, int pointer, int px, int py, int left, int right_hold,
                             int right_tap, int kx, int ky, DWORD now)
{
    int vx = VEH_X(v) - CAMERA_X, vy = VEH_Y(v) - CAMERA_Y;    /* vehicle on screen */
    if (pointer && (px != vx || py != vy) && g_cfg.turret_smooth)   /* vehicles.c: world aim, eased, no fight with the hull */
        vehicles_aim_turret(v, px + CAMERA_X, py + CAMERA_Y, now, !left || VEH_TYPE(v) == VEH_TYPE_FIXED_GUN ? 0 : !S.left_was_down ? 1 : 2);
    else if (pointer && (px != vx || py != vy)) {
        int ox = vx, oy = vy, mx, my;                        /* the shot starts at the muzzle (vehicles_muzzle) */
        if (vehicles_muzzle(v, &mx, &my)) { ox = mx - CAMERA_X; oy = my - CAMERA_Y; }
        if (left && !S.left_was_down && VEH_HAS_TURRET(v) && VEH_MOVER(v) && VEH_TYPE(v) != VEH_TYPE_FIXED_GUN) {
            int dir = facing_toward(px - ox, py - oy);       /* firing: the turret points exactly at the pointer */
            VEH_TURRET(v) = (short)dir;
            SET_TURRET_SPRITE(VEH_MOVER(v) + 0xe4, dir);
        } else aim_turret(v, facing_toward(px - ox, py - oy), now);
    }
    /* right held with the pointer BEHIND the vehicle: back up toward it (until it is in front again) */
    /* 2026-11-20 (user: "backing up a vehicle will continue backing up - make it flip around if there is enough
     * space after backing up for too long"): the camera follows the vehicle, so a pointer held behind it stays
     * behind and it backed up for ever. After REVERSE_FLIP_MS of backing up, with room to turn the hull round
     * (vehicles.c vehicles_turn_room: footprint at every heading of the turn), it turns round and drives forward;
     * it does not back up again until the pointer has been in front (or the button is let go). A short hold
     * behind the vehicle stays a reverse. */
    static int reversing, flipped;
    static DWORD reverse_since, room_checked;
    if (right_hold && pointer) {
        int want = facing_toward(px - vx, py - vy), rel = abs(wrap_dir(want - VEH_HULL(v)));
        if (flipped) { if (rel < REVERSE_UNTIL) flipped = 0; }
        else if (!reversing && rel > REVERSE_FROM) { reversing = 1; reverse_since = now; if (g_cfg.log >= 2) mlog("controls: vehicle backs up toward the pointer (hull %d, pointer at %d)", VEH_HULL(v), want); }
        else if (reversing && rel < REVERSE_UNTIL) reversing = 0;
        if (reversing && now - reverse_since >= REVERSE_FLIP_MS && now - room_checked >= REVERSE_ROOM_EVERY_MS) {
            room_checked = now;
            int room = vehicles_turn_room(v, want);
            if (room) {
                reversing = 0; flipped = room;   /* 1 as vehicles_steer turns, +-2 forward U-turn to that side */
                mlog("controls: vehicle backed up %lu ms - turning round (hull %d -> %d, %s)", now - reverse_since, VEH_HULL(v), want, room == 1 ? "on the spot / K-turn" : "forward U-turn");
            } else if (g_cfg.log >= 2) mlog("controls: vehicle backing up %lu ms - no room to turn round at %d,%d", now - reverse_since, VEH_X(v), VEH_Y(v));
        }
    } else reversing = flipped = 0;
    if (right_tap && pointer && VEH_MOVER(v)) {      /* click on the vehicle itself: get out (mouse only) */
        if (on_own_vehicle(v, px, py)) { pulse(game_command_key(0, SK_ENTER)); right_tap = 0; mlog("controls: click on own vehicle: get out"); }
    }
    if (right_tap && pointer) {
        S.going = 1; S.target_x = px + CAMERA_X; S.target_y = py + CAMERA_Y; S.going_since = now; S.enter = NULL;
        /* a route by the vehicle's own footprint (pathfind.c path_find_vehicle); vehicle_reverse: with reversing (vehicles.c) */
        if (g_cfg.vehicle_reverse) { vehicles_drive_start(v, S.target_x, S.target_y, now); S.nwp = 0; }
        else S.nwp = path_find_vehicle(VEH_TYPE(v) - 1, VEH_X(v), VEH_Y(v), S.target_x, S.target_y, S.wp, (int)(sizeof S.wp / sizeof *S.wp));
        S.wpi = 0;
    }
    if (kx || ky || right_hold) S.going = 0;
    if (S.going && !g_cfg.vehicle_reverse && now - S.going_since > (S.nwp ? VEH_ROUTE_TIMEOUT_MS : GOTO_TIMEOUT_MS)) S.going = 0;
    if ((kx || ky) && g_cfg.vehicle_reverse) vehicles_steer(v, facing_toward(kx, ky), 1, 0);   /* vehicles.c: jeeps turn only while moving */
    else if (kx || ky) steer_vehicle(v, facing_toward(kx, ky), 1);
    else if (right_hold && pointer && reversing && g_cfg.vehicle_reverse)   /* the game inverts turns while backing up */
        vehicles_steer(v, (facing_toward(px - vx, py - vy) + FACINGS / 2) & (FACINGS - 1), -1, 0);
    else if (right_hold && pointer && g_cfg.vehicle_reverse) {
        int want = facing_toward(px - vx, py - vy);
        if ((flipped == 2 || flipped == -2) && abs(wrap_dir(want - VEH_HULL(v))) > VEH_UTURN_FROM)   /* forward U-turn: keep turning to its side */
            want = (VEH_HULL(v) + flipped / 2 * VEH_UTURN_FROM) & (FACINGS - 1);
        vehicles_steer(v, want, 1, 0);
    }
    else if (right_hold && pointer) steer_vehicle(v, facing_toward(px - vx, py - vy), reversing ? -1 : 1);
    else if (S.going && g_cfg.vehicle_reverse) { if (!vehicles_drive(v, now)) S.going = 0; }
    else if (S.going) {
        int gx = S.target_x, gy = S.target_y;
        while (S.nwp && S.wpi < S.nwp) {                      /* along the route: next corner when close */
            int cx = S.wp[S.wpi].x - VEH_X(v), cy = S.wp[S.wpi].y - VEH_Y(v);
            if (S.wpi + 1 < S.nwp && cx * cx + cy * cy <= VEH_CORNER_REACHED * VEH_CORNER_REACHED) { S.wpi++; continue; }
            gx = S.wp[S.wpi].x; gy = S.wp[S.wpi].y;
            break;
        }
        int dx = gx - VEH_X(v), dy = gy - VEH_Y(v);
        int fx = S.target_x - VEH_X(v), fy = S.target_y - VEH_Y(v);
        /* arrived: close, or the target already behind the hull nearby (a vehicle carries on a little and used to
         * turn round for the last pixels - the back and forth) */
        int behind = abs(wrap_dir(facing_toward(fx, fy) - VEH_HULL(v))) > FACINGS / 4;
        long fd = (long)fx * fx + (long)fy * fy;
        if (fd <= VEH_ARRIVE * VEH_ARRIVE || (behind && fd <= VEH_ARRIVE_BEHIND * VEH_ARRIVE_BEHIND)) S.going = 0;
        else steer_vehicle(v, facing_toward(dx, dy), 1);
    }
    if (left) vehicles_aim_report(v, px + CAMERA_X, py + CAMERA_Y, left, pointer ? "" : "(pointer off)");
    if (left) {
        INPUT |= INPUT_FIRE;
        if (!S.left_was_down) INPUT |= INPUT_FIRE_PRESSED;
    }
    if (g_cfg.log >= 2) {
        static DWORD last;
        if (now - last >= (g_cfg.log >= 3 ? 30 : 250)) {
            last = now;
            mlog("controls: vehicle %p type %d at %d,%d hull %d turret %d | pointer %s%d,%d | keys x%+d y%+d L%d going %d | input %#lx",
                 v, VEH_TYPE(v), vx, vy, VEH_HULL(v), VEH_TURRET(v), pointer ? "" : "(off) ", px, py, kx, ky, left, S.going, INPUT);
        }
    }
}

/* ---------------------------------------------------------------- HUD buttons
 * The air support box of the HUD panel (design coordinates; drawn as part of the panel picture):
 * three rows - air strike (F5), paratroopers (F6), recon (F7). A left click runs the same handler as
 * the key. Air strike and paratroopers switch to the game's target cursor (pick the spot with the
 * next click, as after F5/F6); recon reveals the spot the pointer was last over on the battlefield. */
#define AIR_X0  437
#define AIR_X1  504
#define AIR_Y0  404
#define AIR_ROW 23
#define CURSOR_X (*(int *)0x4fe3e8)
#define CURSOR_Y (*(int *)0x4fe3ec)
static const uintptr_t AIR_HANDLER[3] = { 0x440dc0, 0x440d30, 0x440e50 };   /* strike, paratroop, recon */
static const char *const AIR_NAME[3] = { "air strike", "paratroopers", "recon" };

static int air_support_row(int gx, int gy)
{
    if (gx < AIR_X0 || gx >= AIR_X1 || gy < AIR_Y0 || gy >= AIR_Y0 + 3 * AIR_ROW) return -1;
    return (gy - AIR_Y0) / AIR_ROW;
}

/* HUD squad box: FOLLOW / ATTACK / DEFEND rows (F1-F3) and the unit box (Next Unit, Tab) - design
 * coordinates measured on the panel; a left click runs the same handler as the key. */
#define SQUAD_X0 372
#define SQUAD_X1 430
#define SQUAD_Y0 435
#define SQUAD_ROW 13
#define UNIT_X0 322
#define UNIT_X1 373
#define UNIT_Y0 438
#define UNIT_Y1 474
static const uintptr_t SQUAD_HANDLER[3] = { 0x460dc0, 0x460dd0, 0x460e00 };   /* follow, attack, defend */
#define NEXT_UNIT_HANDLER 0x460ee0

static int squad_click(int x, int y)
{
    int gx, gy;
    if (!layout_on_hud_panel(x, y) && layout_active()) return 0;
    layout_pointer_to_game(x, y, &gx, &gy);
    if (gx >= SQUAD_X0 && gx < SQUAD_X1 && gy >= SQUAD_Y0 && gy < SQUAD_Y0 + 3 * SQUAD_ROW) {
        int row = (gy - SQUAD_Y0) / SQUAD_ROW;
        ((VoidFnPtr)SQUAD_HANDLER[row])();
        if (g_cfg.log >= 1) mlog("controls: HUD squad command %d clicked", row);
        return 1;
    }
    if (gx >= UNIT_X0 && gx < UNIT_X1 && gy >= UNIT_Y0 && gy < UNIT_Y1) {
        ((VoidFnPtr)NEXT_UNIT_HANDLER)();
        if (g_cfg.log >= 1) mlog("controls: HUD unit box clicked - next unit");
        return 1;
    }
    return 0;
}

static int g_air_pending = -1;     /* strike / paratroop HUD box pressed: switch on at the release */
static int g_air_was_on, g_air_was_mode;   /* target mode on / its mode (5 strike, 6 paratroop) at that press */
static int g_cmd_prev_on, g_cmd_prev_mode;   /* the same at the end of the last frame (the game's own HUD click may
                                                have toggled it already in the frame of the press) */
#define COMMAND_MODE (*(volatile int *)0x696ec8)
static int air_support_click(int x, int y)
{
    int gx, gy;
    if (!layout_on_hud_panel(x, y) && layout_active()) return 0;
    layout_pointer_to_game(x, y, &gx, &gy);
    int row = air_support_row(gx, gy);
    if (row < 0) return 0;
    if (row != 2) {
        /* strike / paratroop: switched on when the button is let go (air_support_pending) - switched on at the
         * press, the game's own click handler for that same press (0x440880 -> FUN_00441000) already saw the
         * target mode and launched at once (user 2026-11-17: "clicking is immediately firing them") */
        g_air_pending = row;
        g_air_was_on = g_cmd_prev_on; g_air_was_mode = g_cmd_prev_mode;
        if (g_cfg.log >= 2) mlog("controls: HUD click: %s (on release)", AIR_NAME[row]);
        return 1;
    }
    int cx = CURSOR_X, cy = CURSOR_Y;
    if (row == 2 && S.have_cursor) { CURSOR_X = S.cursor_x; CURSOR_Y = S.cursor_y; }   /* recon: last battlefield spot */
    ((VoidFnPtr)AIR_HANDLER[row])();
    if (row == 2) { CURSOR_X = cx; CURSOR_Y = cy; }
    if (g_cfg.log >= 2) mlog("controls: HUD click: %s", AIR_NAME[row]);
    return 1;
}

typedef BYTE *(__fastcall *ObjectsAtFn)(short *point);
#define OBJECTS_AT ((ObjectsAtFn)0x42fca0)
/* ---------------------------------------------------------------- what is under the pointer
 * One-button mouse ([controls] mouse = am2, from Army Men II): the left button does what the thing
 * under the pointer suggests - an enemy in range: shoot; one of your empty vehicles: walk there and
 * climb in; anything else: walk (click = walk there along a route, hold = walk toward the pointer).
 * The test is the game's own: GetObjectsIntersectPoint (0x42fca0, the sprite-mask test bullets use)
 * at the pointer's world point, items filtered by their uid:
 *     army = ((uid >> 26) & 7) - 1 (0 = the player), type = ((uid >> 22) & 0xf) - 1
 *     (2 vehicle, 4 trooper, 5 sarge). Alive: health (+0x1c short) > 0; troopers also mode (+0x68) 1.
 * Enemy = another army that is in play (legion 0x4f7820 + army * 0x19e8, +0x1990 status) and not
 * allied with the player (ArmyHaveAllianceWith 0x41a1e0, __fastcall ecx mine, edx other).
 * In range = within 1.2 x the current weapon's maximum range (FUN_00445c60, __fastcall ecx = weapon
 * type -> world px), as in Army Men II; further away the pointer stays a walk pointer. */
enum { HOVER_GROUND, HOVER_TARGET, HOVER_ENTER, HOVER_FAR_ENEMY };
#define ITEM_TYPE_VEHICLE  2
#define ITEM_TYPE_TROOPER  4
#define ITEM_TYPE_SARGE    5
#define LEGION_STATUS(a)   (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))
typedef int (__fastcall *AllianceFn)(int mine, int other);
typedef int (__fastcall *RangeFn)(int weapon_type);
#define ALLIED             ((AllianceFn)0x41a1e0)
#define WEAPON_MAX_RANGE   ((RangeFn)0x445c60)
#define RANGE_SLACK        1.2

/* Objects worth shooting (binary OBJECTSTRUCT after the ITEM: +0x68 short category, +0x6a short
 * number = the art's file number, +0x74 char owner army): the Boot Camp shooting-range boards (numbers
 * 351-353 standing / kneeling / prone, 367-368 backs), and anything an enemy army owns (gun turrets,
 * AA guns, pillboxes ...). Raw item types 7 asset, 8 structure, 9 obstacle; alive = health > 0. */
#define OBJ_NUMBER(it)  (*(short *)((it) + 0x6a))
#define OBJ_OWNER(it)   (*(signed char *)((it) + 0x74))
/* Object categories (binary OBJECT_TYPES) a shot can damage and that are worth aiming at:
 * explosives, gun turrets, AA guns, bases, buildings, pill boxes, power plants, gates, radar, and
 * MISCELLANEOUS (the Boot Camp cut-out targets - soldiers, tank, jeep, building facade). Trees and
 * bushes only with the flamethrower (it burns them). Rocks, fences, walls, bridges: never (a click
 * there should walk). Must be destructible (max strength +0x1e > 0) and not destroyed (strength > 0). */
#define OBJ_CATEGORY(it)  (*(short *)((it) + 0x68))
enum { CAT_BUSH = 1, CAT_TREE = 2, CAT_BIG_TREE = 3, CAT_EXPLOSIVES = 9, CAT_GUN_TURRET = 10, CAT_AA_GUN = 11,
       CAT_BASE = 12, CAT_AUX_BASE = 13, CAT_BUILDING = 17, CAT_PILL_BOX = 18, CAT_POWER_PLANT = 19,
       CAT_GATE = 20, CAT_WALL_GATE = 21, CAT_RADAR = 22, CAT_MISC = 23 };
#define WEAPON_FLAME_THROWER 3
static int is_target_object(BYTE *it)
{
    DWORD uid = *(DWORD *)it;
    int raw = (int)((uid >> 22) & 0xf);
    if (raw < 7 || raw > 9 || *(short *)(it + 0x1c) <= 0) return 0;
    int num = OBJ_NUMBER(it), own = OBJ_OWNER(it);
    if ((num >= 351 && num <= 353) || num == 367 || num == 368) return 1;             /* range boards */
    if (own >= 1 && own <= 3 && LEGION_STATUS(own) && !ALLIED(0, own)) return 1;       /* enemy-owned */
    if (own == 0) return 0;                                                             /* our own */
    if (*(short *)(it + 0x1e) <= 0) return 0;                                           /* indestructible */
    switch (OBJ_CATEGORY(it)) {
    case CAT_EXPLOSIVES: case CAT_GUN_TURRET: case CAT_AA_GUN: case CAT_BASE: case CAT_AUX_BASE:
    case CAT_BUILDING: case CAT_PILL_BOX: case CAT_POWER_PLANT: case CAT_GATE: case CAT_WALL_GATE:
    case CAT_RADAR: case CAT_MISC:
        return 1;
    case CAT_BUSH: case CAT_TREE: case CAT_BIG_TREE: {
        BYTE *s = SARGE;
        BYTE *v = s ? (BYTE *)(intptr_t)SARGE_VEHICLE(s) : NULL;
        int *w = s ? *(int **)((v ? v : s) + 0x20) : NULL;
        return w && *w == WEAPON_FLAME_THROWER;
    }
    default:
        return 0;
    }
}

static int is_enemy_at(BYTE *it)
{
    DWORD uid = *(DWORD *)it;
    int army = (int)((uid >> 26) & 7) - 1, type = (int)((uid >> 22) & 0xf) - 1;
    if (army <= 0 || army > 3) return 0;
    if (type != ITEM_TYPE_VEHICLE && type != ITEM_TYPE_TROOPER && type != ITEM_TYPE_SARGE) return 0;
    if (*(short *)(it + 0x1c) <= 0) return 0;
    if (type == ITEM_TYPE_TROOPER && *(int *)(it + 0x68) != 1) return 0;
    if (!LEGION_STATUS(army) || ALLIED(0, army)) return 0;
    return 1;
}

/* Soldiers and vehicles are not in the object layers GetObjectsIntersectPoint searches, so the
 * armies are walked: legion (ARMY_RECORD) +4 / +8 = first / end of the UNIT array (stride 0x2c4);
 * UNIT +0x70 mode (3..5 on the map), +0xb4 pTroops[10], +0xdc nLastTroop (i <= n); TROOPER +0x68
 * mode 1 = alive, +0x1c health, +0x26c avatar -> +0x1c ANIM (ANIM +0x1c world RECT); the hit is the
 * game's own sprite-mask test ObjectIntersectPoint 0x42faf0 (__fastcall ecx ANIM*, edx SPOINT*).
 * Vehicles: legion +0x50 count, +0x54 pointers, health +0x1c, mover box as in vehicle_at. */
#define UNIT_STRIDE 0x2c4
typedef int (__fastcall *AnimHitFn)(BYTE *anim, short *point);
#define ANIM_HIT ((AnimHitFn)0x42faf0)
/* FIRING ZONE (2026-11-20, user: "firing and trying to move is still super clunky - lets instead have the area around
 * the enemy be a firing zone too. that way the cursor needs to be further away to walk"): a point within
 * [controls] fire_zone_px (default 24) around an enemy soldier's / vehicle's hitbox counts as that enemy (click =
 * fire, the reticle shows); the nearest one wins. 0 = only the hitbox. */
static int fire_zone_px(void)
{
    static int v = -1;
    if (v < 0) { v = GetPrivateProfileIntA("controls", "fire_zone_px", 24, mod_ini_path()); if (v < 0) v = 0; }
    return v;
}
static void zone_try(const RECT *rc, POINT pt, BYTE *it, BYTE **best, int *bestd)
{
    int z = fire_zone_px();
    if (!z) return;
    int dx = pt.x < rc->left ? rc->left - pt.x : pt.x >= rc->right ? pt.x - rc->right + 1 : 0;
    int dy = pt.y < rc->top ? rc->top - pt.y : pt.y >= rc->bottom ? pt.y - rc->bottom + 1 : 0;
    if (dx > z || dy > z) return;
    int d = dx * dx + dy * dy;
    if (d <= z * z && d < *bestd) { *bestd = d; *best = it; }
}
static BYTE *enemy_unit_at(int wx, int wy)
{
    short p[2] = { (short)wx, (short)wy };
    POINT pt = { wx, wy };
    static int dumped;
    int dump = g_cfg.log >= 3 && GetAsyncKeyState(VK_LBUTTON) < 0 && dumped < 3;
    if (dump) {
        dumped++;
        for (int dy = -12; dy <= 12; dy += 4)               /* objects around the pointer */
            for (int dx = -12; dx <= 12; dx += 4) {
                short q[2] = { (short)(wx + dx), (short)(wy + dy) };
                if (q[0] < 0 || q[1] < 0 || q[0] > 4095 || q[1] > 4095) continue;
                for (BYTE *it = OBJECTS_AT(q); it; it = *(BYTE **)(it + 0x60)) {
                    DWORD uid = *(DWORD *)it;
                    mlog("controls:   item at %+d,%+d: %p uid %08lx army %d type %d pos %d,%d health %d cat %d", dx, dy, it, uid,
                         (int)((uid >> 26) & 7) - 1, (int)((uid >> 22) & 0xf) - 1, *(short *)(it + 4), *(short *)(it + 6),
                         *(short *)(it + 0x1c), *(BYTE *)(it + 0x68));
                }
            }
        BYTE *sg = SARGE;
        if (sg) mlog("controls:   sarge at %d,%d, pointer world %d,%d", SARGE_X(sg), SARGE_Y(sg), wx, wy);
        roofs_debug_near(wx, wy);
        BYTE *cl[8]; int nc = roofs_cells_at(wx, wy, cl, 8);
        for (int i = 0; i < nc; i++) {
            BYTE *ow = *(BYTE **)(cl[i] + 0x0c); DWORD uid = ow ? *(DWORD *)ow : 0;
            mlog("controls:   cell %p flags %08lx owner %p uid %08lx army %d type %d pos %d,%d health %d +68 %d", cl[i], *(DWORD *)cl[i], ow, uid,
                 (int)((uid >> 26) & 7) - 1, (int)((uid >> 22) & 0xf) - 1, ow ? *(short *)(ow + 4) : 0, ow ? *(short *)(ow + 6) : 0,
                 ow ? *(short *)(ow + 0x1c) : 0, ow ? *(int *)(ow + 0x68) : 0);
        }
    }
    BYTE *zbest = NULL; int zd = 1 << 30;
    for (int a = 1; a <= 3; a++) {
        BYTE *leg = ARMY_RECORD(a);
        if (dump && leg) {
            mlog("controls: army %d status %d allied %d legion %p units %p..%p vehicles %d", a, LEGION_STATUS(a), ALLIED(0, a), leg,
                 *(BYTE **)(leg + 4), *(BYTE **)(leg + 8), *(signed char *)(leg + 0x50));
            for (int ty = 0; ty < 8; ty++) {
                BYTE *a0 = *(BYTE **)(leg + 0xc + 4 * ty), *a1 = *(BYTE **)(leg + 0x2c + 4 * ty);
                mlog("controls:   pUnits[%d] %p..%p", ty, a0, a1);
                for (int g = 0; a0 && a0 < a1 && g < 32; a0 += UNIT_STRIDE, g++) {
                    int last = *(int *)(a0 + 0xdc);
                    BYTE *t0 = last >= 0 && last < 10 ? ((BYTE **)(a0 + 0xb4))[0] : NULL;
                    mlog("controls:     unit %p mode %d last %d first trooper %p at %d,%d mode %d", a0, *(int *)(a0 + 0x70), last, t0,
                         t0 ? *(short *)(t0 + 4) : 0, t0 ? *(short *)(t0 + 6) : 0, t0 ? *(int *)(t0 + 0x68) : 0);
                }
            }
            BYTE *u2 = *(BYTE **)(leg + 4), *e2 = *(BYTE **)(leg + 8);
            for (int g = 0; u2 && u2 < e2 && g < 64; u2 += UNIT_STRIDE, g++) {
                if (*(int *)(u2 + 0x70) <= 1) continue;     /* not on the map */
                int last = *(int *)(u2 + 0xdc);
                mlog("controls:   unit %p mode %d last %d", u2, *(int *)(u2 + 0x70), last);
                for (int i = 0; i <= last && i < 10; i++) {
                    BYTE *t = ((BYTE **)(u2 + 0xb4))[i];
                    if (!t) continue;
                    BYTE *av = *(BYTE **)(t + 0x26c), *an = av ? *(BYTE **)(av + 0x1c) : NULL;
                    const int *r = an ? (const int *)(an + 0x1c) : NULL;
                    mlog("controls:     trooper %p at %d,%d mode %d health %d rect %d,%d,%d,%d", t, *(short *)(t + 4), *(short *)(t + 6),
                         *(int *)(t + 0x68), *(short *)(t + 0x1c), r ? r[0] : 0, r ? r[1] : 0, r ? r[2] : 0, r ? r[3] : 0);
                }
            }
        }
        if (!LEGION_STATUS(a) || ALLIED(0, a)) continue;
        if (!leg) continue;
        BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
        for (int guard = 0; un && un < end && guard < 64; un += UNIT_STRIDE, guard++) {
            int mode = *(int *)(un + 0x70);
            if (mode < 3 || mode > 5) continue;
            int last = *(int *)(un + 0xdc);
            if (last < 0 || last > 9) last = 9;
            for (int i = 0; i <= last; i++) {
                BYTE *t = ((BYTE **)(un + 0xb4))[i];
                if (!t || *(int *)(t + 0x68) != 1 || *(short *)(t + 0x1c) <= 0) continue;
                BYTE *av = *(BYTE **)(t + 0x26c);
                BYTE *anim = av ? *(BYTE **)(av + 0x1c) : NULL;
                if (!anim) continue;
                const int *r = (const int *)(anim + 0x1c);
                RECT rc = { r[0], r[1], r[2], r[3] };
                if (PtInRect(&rc, pt)) return t;            /* inside the hitbox: easier than the exact pixels */
                zone_try(&rc, pt, t, &zbest, &zd);
            }
        }
        int nv = *(signed char *)(leg + 0x50);
        for (int i = 0; i < nv && i < 50; i++) {
            BYTE *v = *(BYTE **)(leg + 0x54 + 4 * i);
            if (!v || *(short *)(v + 0x1c) <= 0 || v == capture_vehicle()) continue;   /* Sarge's captured one */
            BYTE *m = VEH_MOVER(v);
            if (!m) continue;
            RECT rc = { *(int *)(m + 0x1c), *(int *)(m + 0x20), *(int *)(m + 0x24), *(int *)(m + 0x28) };
            if (PtInRect(&rc, pt)) return v;
            zone_try(&rc, pt, v, &zbest, &zd);
        }
    }
    return zbest;                                           /* the nearest enemy whose firing zone holds the point */
}

/* ---------------------------------------------------------------- mouse-only extras (user 2026-11-17:
 * "would like to be able to fully play this with mouse only as an option")
 * - right click on one of your squad's soldiers = select that unit for squad orders (F1-F3, radial, Ctrl+click):
 *   the game's selected unit is 0x69195c, set by FUN_00441b10 (__fastcall ecx = UNIT*; Next Unit 0x460ee0 ->
 *   0x440f10 -> 0x41b880 picks units with +0x70 > 1 and +0x88 == 0); the HUD unit box shows its name.
 * - click ON Sarge = stance: standing -> kneel -> prone (the game's Kneel/Prone command, FUN_004473a0) -> stand
 *   (forward input while kneeling / prone raises one step without moving, FUN_00447290; one frame each).
 *   Sarge +0x6c = target pose (0xb kneel, 0xc stand, 0xd prone), +0x70 = pose change in progress. */
typedef void (__fastcall *SelectUnitFn)(BYTE *unit);
#define SELECT_UNIT     ((SelectUnitFn)0x441b10)
#define SELECTED_UNIT   (*(BYTE **)0x69195c)
#define SARGE_POSE(u)   (*(int *)((u) + 0x6c))
#define SARGE_POSING(u) (*(int *)((u) + 0x70))
#define POSE_KNEEL 0xb
#define POSE_STAND 0xc
#define POSE_PRONE 0xd

static BYTE *own_unit_at(int wx, int wy)
{
    POINT pt = { wx, wy };
    BYTE *leg = ARMY_RECORD(0);
    if (!leg) return NULL;
    BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
    for (int guard = 0; un && un < end && guard < 64; un += UNIT_STRIDE, guard++) {
        if (*(int *)(un + 0x70) <= 1 || *(int *)(un + 0x88) != 0) continue;      /* as Next Unit picks them */
        int last = *(int *)(un + 0xdc);
        if (last < 0 || last > 9) last = 9;
        for (int i = 0; i <= last; i++) {
            BYTE *t = ((BYTE **)(un + 0xb4))[i];
            if (!t || *(short *)(t + 0x1c) <= 0) continue;
            BYTE *av = *(BYTE **)(t + 0x26c);
            BYTE *anim = av ? *(BYTE **)(av + 0x1c) : NULL;
            if (!anim) continue;
            const int *r = (const int *)(anim + 0x1c);
            RECT rc = { r[0], r[1], r[2], r[3] };
            if (PtInRect(&rc, pt)) return un;
        }
    }
    return NULL;
}

/* the pointer (screen) is on Sarge's body: his feet are at (sx, sy), the figure is ~24 px tall (lying: wider, lower) */
static int on_sarge(int px, int py, int sx, int sy)
{
    BYTE *u = SARGE;
    if (u && SARGE_POSE(u) == POSE_PRONE) return abs(px - sx) <= 14 && py - sy >= -16 && py - sy <= 4;
    return abs(px - sx) <= 9 && py - sy >= -26 && py - sy <= 3;
}

/* ---------------------------------------------------------------- stance (2026-11-20, user design)
 * The game has no "go to pose X": the Kneel/Prone command (FUN_004473a0, the dispatcher key handler of row
 * SK_KNEEL) steps stand -> kneel -> prone (sets the target pose +0x6c and the pose-change flag +0x70); the move
 * input (FUN_00447290) raises one step while kneeling / prone without walking (prone -> kneel -> stand). So a
 * wanted pose is reached by single steps over frames: press the row's key for one frame (pulse) to go down,
 * INPUT_FORWARD for one frame to go up, wait for the pose change to finish (+0x70 back to 0), again. The game's
 * own rules stay: with a grenade / flamer / bazooka the animation kneels instead of lying (FUN_00439c50), the
 * mortar with fire held does not stand. Given up after STANCE_TIMEOUT_MS or when a step changes nothing.
 * Inputs: tap C / click Sarge = one down (prone: stand up), hold C / double-click Sarge = prone, double-tap C /
 * click prone Sarge = stand, the drag picker and the stance radial = any. log=2: "controls: stance ... reached". */
#define STANCE_TIMEOUT_MS 2500
#define STANCE_STEP_MS    120      /* at least this long between two steps (the pose flag shows up a frame later) */
#define SARGE_DBL_MS      350      /* a second click on Sarge within this: double-click (prone) */
static int g_mid_radial;             /* (kept for the radial's state; the middle button itself is a binding now) */
static int g_stance_want, g_stance_tries, g_stance_last;
static DWORD g_stance_until, g_stance_next;
static const char *g_stance_why = "";
static int pose_rank(int p) { return p == POSE_STAND ? 2 : p == POSE_KNEEL ? 1 : p == POSE_PRONE ? 0 : -1; }
static const char *pose_name(int p) { return p == POSE_STAND ? "stand" : p == POSE_KNEEL ? "kneel" : p == POSE_PRONE ? "prone" : "?"; }
static void stance_goto(BYTE *u, int pose, DWORD now, const char *why)
{
    if (!u || SARGE_VEHICLE(u)) return;
    g_stance_want = pose; g_stance_until = now + STANCE_TIMEOUT_MS; g_stance_next = 0; g_stance_tries = 0; g_stance_last = -1;
    g_stance_why = why;
    if (g_cfg.log >= 2) mlog("controls: stance %s -> %s (%s)", pose_name(SARGE_POSE(u)), pose_name(pose), why);
}
/* one step down; lying: stand up (the click on Sarge since 2026-11-17, now also tap C) */
static void stance_cycle(BYTE *u, DWORD now, const char *why)
{
    int pose = SARGE_POSE(u);
    stance_goto(u, pose == POSE_STAND ? POSE_KNEEL : pose == POSE_KNEEL ? POSE_PRONE : POSE_STAND, now, why);
}
static void stance_update(BYTE *u, DWORD now)
{
    if (!g_stance_want) return;
    int pose = SARGE_POSE(u);
    if (pose == g_stance_want || SARGE_VEHICLE(u) || now > g_stance_until) {
        if (g_cfg.log >= 2) mlog("controls: stance %s %s (%s, state %d)", pose_name(g_stance_want), pose == g_stance_want ? "reached" : "not reached (timeout)",
                                 g_stance_why, SARGE_STATE(u));
        g_stance_want = 0;
        return;
    }
    if (SARGE_POSING(u) || now < g_stance_next) return;
    int r = pose_rank(pose), w = pose_rank(g_stance_want);
    if (r < 0) return;
    if (pose == g_stance_last && ++g_stance_tries >= 4) {            /* the game does not take the step (its rule) */
        if (g_cfg.log >= 2) mlog("controls: stance %s refused by the game at %s (%s)", pose_name(g_stance_want), pose_name(pose), g_stance_why);
        g_stance_want = 0;
        return;
    }
    if (pose != g_stance_last) g_stance_tries = 0;
    g_stance_last = pose;
    if (r > w) pulse(game_command_key(0, SK_KNEEL));                 /* one step down */
    else INPUT |= INPUT_FORWARD;                                     /* one step up (kneeling / prone: no walk) */
    g_stance_next = now + STANCE_STEP_MS;
}
/* modern.c stance radial: the six moves */
void controls_stance_command(int cmd)
{
    BYTE *u = SARGE;
    if (!u || SARGE_VEHICLE(u)) return;
    DWORD now = GetTickCount();
    switch (cmd) {
    case STANCE_CMD_STAND: stance_goto(u, POSE_STAND, now, "stance radial"); break;
    case STANCE_CMD_KNEEL: stance_goto(u, POSE_KNEEL, now, "stance radial"); break;
    case STANCE_CMD_PRONE: stance_goto(u, POSE_PRONE, now, "stance radial"); break;
    case STANCE_CMD_DIVE: pulse(game_command_key(0, SK_DIVE)); if (g_cfg.log >= 2) mlog("controls: dive (stance radial)"); break;
    case STANCE_CMD_ROLL_L: case STANCE_CMD_ROLL_R:
        pulse(game_command_key(0, cmd == STANCE_CMD_ROLL_R ? SK_ROLL_R : SK_ROLL_L));
        if (g_cfg.log >= 2) mlog("controls: roll %s (stance radial)", cmd == STANCE_CMD_ROLL_R ? "right" : "left");
        break;
    }
}
/* the drag picker (press on Sarge + drag >= STANCE_PICKER_DRAG px up / down; modern.c draws it) */
static int g_picker, g_picker_sel;
static int picker_select(BYTE *u, int pointer_wy)
{
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < STANCE_PICKER_BOXES; i++) {
        int cy = SARGE_Y(u) - STANCE_PICKER_BASE - (STANCE_PICKER_BOXES - 1 - i) * STANCE_PICKER_STEP, d = abs(pointer_wy - cy);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
int controls_stance_picker(int *wx, int *wy, int *sel, int *cur)
{
    BYTE *u = SARGE;
    if (!g_picker || !u) return 0;
    *wx = SARGE_X(u); *wy = SARGE_Y(u); *sel = g_picker_sel;
    int p = SARGE_POSE(u);
    *cur = p == POSE_STAND ? 0 : p == POSE_KNEEL ? 1 : p == POSE_PRONE ? 2 : -1;
    return 1;
}
static const int PICK_POSE[STANCE_PICKER_BOXES] = { POSE_STAND, POSE_KNEEL, POSE_PRONE, 0 };   /* 0: DIVE (the game's dive key) */
/* the radial being opened: on Sarge (stance ring) - set by modern_controls just before modern_radial opens it */
static int g_radial_sarge;
static int g_mpress_sarge[3];         /* Middle / Mouse4 / Mouse5: the last press started on Sarge */
int controls_radial_on_sarge(void) { return g_radial_sarge; }

static int hover_at(BYTE *u, BYTE *veh, int wx, int wy, BYTE **thing);

/* log=2 self-test (every 2 s): the hover test at the middle of each enemy soldier on screen. */
static void hover_selftest(BYTE *u, DWORD now)
{
    static DWORD last;
    if (g_cfg.log < 3 || now - last < 2000) return;
    last = now;
    int vw = g_cfg.width, vh = g_cfg.height, n = 0;
    {                                                   /* target objects (shooting-range boards, enemy-owned) */
        static BYTE *cells[1024];
        int nc = roofs_cells_list(cells, 1024), m = 0;
        for (int i = 0; i < nc && m < 12; i++) {
            BYTE *ow = *(BYTE **)(cells[i] + 0x0c);
            if (!ow || !is_target_object(ow)) continue;
            const int *r = (const int *)(cells[i] + 0x1c);
            int x = (r[0] + r[2]) / 2, y = (r[1] + r[3]) / 2, sx = x - CAMERA_X, sy = y - CAMERA_Y;
            if (sx < 0 || sy < 0 || sx >= vw || sy >= vh) continue;
            int hits = 0, tried = 0;
            for (int yy = r[1]; yy < r[3]; yy += 3) for (int xx = r[0]; xx < r[2]; xx += 3) {
                short q[2] = { (short)xx, (short)yy }; tried++;
                if (ANIM_HIT(cells[i], q)) { if (!hits) { x = xx; y = yy; } hits++; }
            }
            BYTE *thing;
            int h = hover_at(u, NULL, x, y, &thing);
            mlog("controls: selftest target object %p number %d category %d owner %d at %d,%d screen %d,%d: hover %d (sprite test: %d of %d points, cell flags %08lx)",
                 ow, OBJ_NUMBER(ow), OBJ_CATEGORY(ow), OBJ_OWNER(ow), x, y, sx, sy, h, hits, tried, *(DWORD *)cells[i]);
            m++;
        }
    }
    for (int a = 1; a <= 3 && n < 4; a++) {
        if (!LEGION_STATUS(a) || ALLIED(0, a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        BYTE *un = leg ? *(BYTE **)(leg + 4) : NULL, *end = leg ? *(BYTE **)(leg + 8) : NULL;
        for (int g = 0; un && un < end && g < 64 && n < 4; un += UNIT_STRIDE, g++) {
            int mode = *(int *)(un + 0x70);
            if (mode < 3 || mode > 5) continue;
            for (int i = 0; i <= *(int *)(un + 0xdc) && i < 10 && n < 4; i++) {
                BYTE *t = ((BYTE **)(un + 0xb4))[i];
                if (!t || *(int *)(t + 0x68) != 1) continue;
                int x = *(short *)(t + 4), y = *(short *)(t + 6);
                BYTE *av = *(BYTE **)(t + 0x26c), *an = av ? *(BYTE **)(av + 0x1c) : NULL;
                if (an) { const int *r = (const int *)(an + 0x1c); x = (r[0] + r[2]) / 2; y = (r[1] + r[3]) / 2; }
                int sx = x - CAMERA_X, sy = y - CAMERA_Y;
                if (sx < 0 || sy < 0 || sx >= vw || sy >= vh) continue;
                BYTE *thing;
                int h = hover_at(u, NULL, x, y, &thing);
                mlog("controls: selftest enemy %p (army %d) at %d,%d screen %d,%d: hover %d thing %p", t, a, x, y, sx, sy, h, thing);
                n++;
            }
        }
    }
}

/* What the pointer (world wx, wy) is over, for unit u (on foot or in vehicle veh). */
static int hover_at(BYTE *u, BYTE *veh, int wx, int wy, BYTE **thing)
{
    *thing = NULL;
    if (wx < 0 || wy < 0 || wx > 4095 || wy > 4095) return HOVER_GROUND;
    if (!veh) {
        BYTE *v = vehicle_at(wx, wy);
        if (v) { *thing = v; return HOVER_ENTER; }
    }
    short p[2] = { (short)wx, (short)wy };
    BYTE *enemy = NULL;
    static int log_next;                    /* log=2: what is under the pointer, on the next press */
    if (g_cfg.log >= 3 && GetAsyncKeyState(VK_LBUTTON) < 0) { if (!log_next) log_next = 1; } else log_next = 0;
    for (BYTE *it = OBJECTS_AT(p); it; it = *(BYTE **)(it + 0x60)) {
        if (log_next == 1) {
            DWORD uid = *(DWORD *)it;
            int army = (int)((uid >> 26) & 7) - 1;
            mlog("controls: under pointer %d,%d: item %p uid %08lx army %d type %d health %d mode %d status %d allied %d number %d owner %d",
                 wx, wy, it, uid, army, (int)((uid >> 22) & 0xf) - 1, *(short *)(it + 0x1c), *(int *)(it + 0x68),
                 army > 0 && army <= 3 ? LEGION_STATUS(army) : -1, army > 0 && army <= 3 ? ALLIED(0, army) : -1, OBJ_NUMBER(it), OBJ_OWNER(it));
        }
        if (is_enemy_at(it) || is_target_object(it)) { enemy = it; break; }
    }
    if (log_next == 1) log_next = 2;
    if (!enemy) enemy = enemy_unit_at(wx, wy);
    if (!enemy) {                    /* drawn objects outside the point-test layers (shooting-range boards) */
        BYTE *cl[8];
        int nc = roofs_cells_at(wx, wy, cl, 8);
        for (int i = 0; i < nc && !enemy; i++) {
            BYTE *ow = *(BYTE **)(cl[i] + 0x0c);
            if (ow && (is_target_object(ow) || is_enemy_at(ow))) enemy = ow;   /* inside its hitbox (the cell rect): objects, enemy vehicles and soldiers */
        }
    }
    if (log_next == 2 && g_cfg.log >= 3) { log_next = 3; mlog("controls: enemy under pointer %d,%d: %p", wx, wy, enemy); }
    if (!enemy) return HOVER_GROUND;
    *thing = enemy;
    BYTE *shooter = veh ? veh : u;
    int *w = *(int **)(shooter + 0x20);
    int range = w ? WEAPON_MAX_RANGE(*w) : 0;
    int dx = *(short *)(enemy + 4) - *(short *)(shooter + 4), dy = *(short *)(enemy + 6) - *(short *)(shooter + 6);
    if (range > 0 && (double)dx * dx + (double)dy * dy > RANGE_SLACK * RANGE_SLACK * range * range) return HOVER_FAR_ENEMY;
    return HOVER_TARGET;
}

/* ---------------------------------------------------------------- pointer shapes (one-button mouse)
 * Over the battlefield: the game's red reticle when a left click would shoot (enemy in range, or
 * force fire); a walk marker (green ring) otherwise; an "enter" arrow over your empty vehicles.
 * The game's reticle is hidden while the walk / enter pointer shows (its two draw calls, 0x43eeb6
 * in a vehicle and 0x43eee4 on foot, call FUN_004130e0 with ecx = the gunsight picture: redirected
 * to controls_draw_gunsight). */
static HCURSOR g_cur_walk, g_cur_enter, g_cur_nowalk, g_cur_exit, g_cur_weapon[4];
#define CUR_NOWALK 10                           /* pointer kind: ground Sarge cannot reach (pathfind.c) */
#define CUR_EXIT   11                           /* pointer kind: on your own vehicle while driving = get out */
#define CUR_GRENADE 12                          /* aiming pointers per weapon: grenade, mortar round, rocket */
#define CUR_SHELL   13
#define CUR_ROCKET  14
#define CUR_TANKROUND 15
static int g_cur_size;
static volatile int g_hide_reticle;             /* this frame: walk / enter pointer, no reticle */
static volatile int g_cursor_kind = -1;         /* HOVER_* shown, -1 = the game's (no Windows pointer) */

/* Weapon pointers (user 2026-11-17: "the cursor could turn into a grenade for a grenade and a mortar round for
 * mortar ... bazooka round"): shapes in pointer units (32 px pointer, centre 0,0, y down); 1 = inside. */
static int weapon_icon(int kind, double x, double y, uint32_t *rgb)
{
    double ax = fabs(x);
    if (kind == CUR_GRENADE) {
        if (ax <= 2.2 && y >= -11 && y <= -6.5) { *rgb = 0x9a9a92; return 1; }                  /* fuse head */
        if (x >= 1.5 && x <= 7 && y >= -10 + (x - 1.5) * 1.1 - 1.2 && y <= -10 + (x - 1.5) * 1.1 + 1.2) { *rgb = 0x9a9a92; return 1; }   /* lever */
        double ex = x / 6.8, ey = (y - 1.5) / 8.2;
        if (ex * ex + ey * ey <= 1) {                                                              /* pineapple body */
            int seg = ((int)floor((y + 20) / 3.2) + (int)floor((x + 20) / 3.2)) & 1;
            *rgb = seg ? 0x4f6b2c : 0x3c5321; return 1;
        }
        return 0;
    }
    if (kind == CUR_SHELL) {                                                                       /* mortar round */
        if (y >= -12 && y <= -3) { double r = 5.0 * (y + 12) / 9.0; if (ax <= r) { *rgb = y < -9 ? 0xc8b45a : 0x56663a; return 1; } return 0; }
        if (y > -3 && y <= 5) { if (ax <= 5) { *rgb = y < -1 ? 0xc8b45a : 0x56663a; return 1; } return 0; }
        if (y > 5 && y <= 9) { if (ax <= 2) { *rgb = 0x6a6a62; return 1; } return 0; }
        if (y > 9 && y <= 13) { if (ax <= 6.5 && (ax <= 2 || fabs(ax - 5) <= 1.4)) { *rgb = 0x6a6a62; return 1; } return 0; }
        return 0;
    }
    if (kind == CUR_TANKROUND) {                                                                   /* tank round: grey tip, brass case */
        if (y >= -13 && y <= -5) { double r = 3.8 * (y + 13) / 8.0; if (ax <= r) { *rgb = 0x8a8c86; return 1; } return 0; }
        if (y > -5 && y <= 10) { if (ax <= 3.8) { *rgb = y < -3 ? 0x5a5c56 : 0xc8a040; return 1; } return 0; }
        if (y > 10 && y <= 12.5) { if (ax <= 4.8) { *rgb = 0xa88830; return 1; } return 0; }
        return 0;
    }
    if (kind == CUR_ROCKET) {                                                                      /* bazooka rocket */
        if (y >= -13 && y <= -7) { double r = 3.6 * (y + 13) / 6.0; if (ax <= r) { *rgb = 0xc83a2a; return 1; } return 0; }
        if (y > -7 && y <= 7) { if (ax <= 3.6) { *rgb = 0x4c5a3a; return 1; } return 0; }
        if (y > 7 && y <= 13) { double fin = 3.6 + (y - 7) * 0.6; if (ax <= fin && (ax <= 2 || ax >= fin - 1.6)) { *rgb = 0x6a6a62; return 1; } return 0; }
        return 0;
    }
    return 0;
}

static HCURSOR make_cursor(int kind, int size)
{
    BITMAPV5HEADER bh = { 0 };
    bh.bV5Size = sizeof bh; bh.bV5Width = size; bh.bV5Height = -size; bh.bV5Planes = 1; bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00ff0000; bh.bV5GreenMask = 0x0000ff00; bh.bV5BlueMask = 0x000000ff; bh.bV5AlphaMask = 0xff000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP colour = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!colour || !bits) return NULL;
    uint32_t *px = bits;
    double c = (size - 1) / 2.0, s = size / 32.0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            double dx = x - c, dy = y - c, d = sqrt(dx * dx + dy * dy);
            double a = 0; uint32_t rgb = 0;
            if (kind >= CUR_GRENADE && kind <= CUR_TANKROUND) {   /* weapon icon, dark outline, red aim dot */
                double ux = dx / s, uy = dy / s;
                uint32_t c2;
                if (weapon_icon(kind, ux, uy, &c2)) { a = 1; rgb = c2; }
                else {
                    for (int k = 0; k < 8 && a == 0; k++) {
                        static const double OX[8] = { 1.4, -1.4, 0, 0, 1, 1, -1, -1 }, OY[8] = { 0, 0, 1.4, -1.4, 1, -1, 1, -1 };
                        if (weapon_icon(kind, ux + OX[k], uy + OY[k], &c2)) { a = 1; rgb = 0x14180c; }
                    }
                }
                if (d <= 1.3 * s) { a = 1; rgb = 0xff3020; }
            } else if (kind == HOVER_GROUND) {                /* green ring + centre dot, dark outline */
                double ring = fabs(d - 9 * s), dot = d;
                double in = 1.0 - (ring - 1.2 * s), out = 1.0 - (ring - 2.4 * s);
                if (in > 0) { a = in > 1 ? 1 : in; rgb = 0x7cf07c; }
                else if (out > 0) { a = out > 1 ? 1 : out; rgb = 0x102010; }
                if (dot < 1.8 * s) { a = 1; rgb = 0x7cf07c; } else if (dot < 2.8 * s && a < 1) { a = 1; rgb = 0x102010; }
            } else if (kind == CUR_NOWALK) {                  /* red ring with a slash: cannot walk there */
                double ring = fabs(d - 9 * s);
                double slash = fabs(dx - dy) / 1.41421356;     /* distance to the "\" diagonal */
                double line = d <= 9 * s ? (ring < slash ? ring : slash) : ring;
                double in = 1.0 - (line - 1.2 * s), out = 1.0 - (line - 2.4 * s);
                if (in > 0) { a = in > 1 ? 1 : in; rgb = 0xff4a3a; }
                else if (out > 0) { a = out > 1 ? 1 : out; rgb = 0x200808; }
            } else {                                          /* enter: yellow down arrow into a bar; exit: green, mirrored (up, out) */
                if (kind == CUR_EXIT) dy = -dy;
                double ax = fabs(dx), ty = dy + 8 * s;        /* arrow: shaft x<2.5s for y -8..2, head y 2..9 */
                int shaft = ax <= 2.5 * s && dy >= -11 * s && dy <= 1 * s;
                int head = dy >= 1 * s && dy <= 9 * s && ax <= (9 * s - dy) ;
                int bar = dy >= 11 * s && dy <= 13.5 * s && ax <= 9 * s;
                int fill = shaft || head || bar;
                int edge = 0;
                if (!fill)
                    for (int oy = -2; oy <= 2 && !edge; oy++)
                        for (int ox = -2; ox <= 2 && !edge; ox++) {
                            double ex = fabs(dx + ox), ey = dy + oy;
                            if ((ex <= 2.5 * s && ey >= -11 * s && ey <= 1 * s) || (ey >= 1 * s && ey <= 9 * s && ex <= 9 * s - ey) ||
                                (ey >= 11 * s && ey <= 13.5 * s && ex <= 9 * s)) edge = 1;
                        }
                if (fill) { a = 1; rgb = kind == CUR_EXIT ? 0x7cf07c : 0xffd84a; } else if (edge) { a = 1; rgb = 0x201800; }
                (void)ty;
            }
            uint32_t A = (uint32_t)(a * 255 + 0.5);
            uint32_t r = (rgb >> 16 & 255) * A / 255, g = (rgb >> 8 & 255) * A / 255, b = (rgb & 255) * A / 255;
            px[y * size + x] = A << 24 | r << 16 | g << 8 | b;
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii = { FALSE, (DWORD)(size / 2), (DWORD)(size / 2), mask, colour };
    HCURSOR h = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(colour); DeleteObject(mask);
    return h;
}

static void ensure_cursors(void)
{
    int scale = view_pixel_scale();
    int size = scale >= 3 ? 48 : 32;
    if (g_cur_walk && size == g_cur_size) return;
    if (g_cur_walk) DestroyCursor(g_cur_walk);
    if (g_cur_enter) DestroyCursor(g_cur_enter);
    if (g_cur_nowalk) DestroyCursor(g_cur_nowalk);
    if (g_cur_exit) DestroyCursor(g_cur_exit);
    for (int i = 0; i < 4; i++) if (g_cur_weapon[i]) DestroyCursor(g_cur_weapon[i]);
    g_cur_walk = make_cursor(HOVER_GROUND, size);
    g_cur_enter = make_cursor(HOVER_ENTER, size);
    g_cur_nowalk = make_cursor(CUR_NOWALK, size);
    g_cur_exit = make_cursor(CUR_EXIT, size);
    for (int i = 0; i < 4; i++) g_cur_weapon[i] = make_cursor(CUR_GRENADE + i, size);
    g_cur_size = size;
}

/* The target under the pointer this frame (one-button mouse): 1 = in weapon range, 0 = beyond it,
 * -1 = no target. For the reticle's range display (a target out of range still gets the reticle and a
 * click still shoots - the shot just falls short). */
static volatile int g_hover_state = -1;
int controls_target_in_range(void) { return g_hover_state; }
static int is_target_object(BYTE *it); static int is_enemy_at(BYTE *it);
int controls_is_target(BYTE *it) { return it && (is_target_object(it) || is_enemy_at(it)); }   /* debugview.c */

/* WM_SETCURSOR (mouse.c) and every frame: the Windows pointer for the battlefield, NULL = none. */
HCURSOR controls_cursor(void)
{
    if (COMMAND_CURSOR) return NULL;          /* air strike / paratroop target: the game draws its own cursor */
    int k = g_cursor_kind;
    if (k == HOVER_GROUND) return g_cur_walk;
    if (k == HOVER_ENTER) return g_cur_enter;
    if (k == CUR_NOWALK) return g_cur_nowalk;
    if (k == CUR_EXIT) return g_cur_exit;
    if (k >= CUR_GRENADE && k <= CUR_TANKROUND) return g_cur_weapon[k - CUR_GRENADE];
    return NULL;
}

/* Is the ground under the pointer closed off from Sarge (pathfind.c path_reachable)? Asked again when
 * the pointer moves to another 8 px cell (at most every 80 ms, 6 ms search budget) or Sarge has moved
 * on (every 500 ms); undecided counts as reachable. */
static int path_blocked(int x, int y, int dx, int dy);
static int pointer_unreachable(BYTE *u, int wx, int wy, DWORD now)
{
    static int key = -1, skey = -1, res = 1;
    static DWORD at;
    int k = (wy >> 3) * 512 + (wx >> 3), sk = (SARGE_Y(u) >> 3) * 512 + (SARGE_X(u) >> 3);
    if ((k != key && now - at >= 80) || (sk != skey && now - at >= 500)) {
        int sx = SARGE_X(u), sy = SARGE_Y(u);
        res = path_reachable(sx, sy, wx, wy, 6);
        /* 2026-11-20 (user: "sarge walks on a barricade or fence and the cursor shows he can't walk even though he
         * can"): the planner's grid is strict (fences, barricades blocked) - standing ON one, his start cell is
         * blocked and every target looked unreachable. Also reachable: a straight walk the game's own test allows,
         * or a path from a free spot right beside him. */
        if (!res && !path_blocked(sx, sy, wx - sx, wy - sy)) res = 1;
        static const int OX[4] = { 12, -12, 0, 0 }, OY[4] = { 0, 0, 12, -12 };
        for (int i = 0; i < 4 && !res; i++)
            if (!controls_blocked_at(sx + OX[i], sy + OY[i]) && path_reachable(sx + OX[i], sy + OY[i], wx, wy, 6)) res = 1;
        key = k; skey = sk; at = now;
    }
    return res == 0;
}

typedef void (__fastcall *PictureFn)(void *pic, int x, int y);
#define DRAW_PICTURE_TO_SCREEN ((PictureFn)0x4130e0)
void __fastcall controls_draw_gunsight(void *pic, int x, int y)
{
    if (g_hide_reticle) return;
    DRAW_PICTURE_TO_SCREEN(pic, x, y);
}

/* ---------------------------------------------------------------- modernization (modern.c switches)
 * The control features of the modernization plan, each behind its own switch (modern_on):
 *   gamepad twin-stick, dodge roll key, mouse side buttons = weapons, sprint toggle, sprint stamina,
 *   edge peek, Ctrl + click squad orders, radial command menu, ping, aim assist (aim_shot_at_cursor)
 *   and grenades / mortar landing at the pointer (modern_on_foot). Keys pressed for the game go out
 *   as single "pulsed" scan codes through kb_get_state (g_pulse_key), the same way as hold-X drop. */
#define DIK_F1 0x3b
#define DIK_F2 0x3c
#define DIK_F3 0x3d
#define DIK_TAB    0x0f
#define CMD_FOLLOW     ((VoidFnPtr)0x460dc0)
#define CMD_ATTACK     ((VoidFnPtr)0x460dd0)
#define CMD_DEFEND     ((VoidFnPtr)0x460e00)
#define CMD_ATTACK_NOW ((VoidFnPtr)0x460e30)
#define XB_DPAD_UP 0x0001
#define XB_DPAD_DOWN 0x0002
#define XB_DPAD_LEFT 0x0004
#define XB_DPAD_RIGHT 0x0008
#define XB_START 0x0010
#define XB_BACK 0x0020
#define XB_LTHUMB 0x0040
#define XB_RTHUMB 0x0080
#define XB_LB 0x0100
#define XB_RB 0x0200
#define XB_A 0x1000
#define XB_B 0x2000
#define XB_X 0x4000
#define XB_Y 0x8000
#define PAD_AIM_RADIUS 150  /* game px: the right stick puts the reticle this far from Sarge (full tilt) */

static int g_sprint_latched;                  /* sprint toggle (Shift / left stick click) */
static float g_stamina = 1.0f; static int g_exhausted;
static DWORD g_pad_aim_at;                    /* the right stick aimed recently: the reticle ignores the mouse */
static int g_hover_kind = -1, g_hover_range; static BYTE *g_hover_thing;
static int g_block_left, g_block_right;       /* a press used for a command: no fire / walk until released */
void controls_block_left(void) { g_block_left = 1; }        /* panel.c: a click on our own UI (resize icon) */

static int sprint_wanted(void)
{
    if (modern_on(MOD_SPRINT_TOGGLE)) return g_sprint_latched;
    return bindings_held(ACT_SPRINT) || g_sprint_latched;
}

/* Sprint stamina: ~4 s of running from full, back in ~6 s; at empty no running until a third is back. */
static void apply_stamina(DWORD now, int moving)
{
    static DWORD last;
    DWORD dt = last ? now - last : 0;
    last = now;
    if (dt > 200) dt = 200;
    if (!modern_on(MOD_STAMINA)) { g_stamina = 1.0f; g_exhausted = 0; return; }
    if (S.running && moving && !g_exhausted) g_stamina -= dt / 4000.0f;
    else g_stamina += dt / 6000.0f;
    if (g_stamina <= 0) { g_stamina = 0; g_exhausted = 1; }
    if (g_stamina > 1) g_stamina = 1;
    if (g_exhausted && g_stamina >= 0.35f) g_exhausted = 0;
    if (g_exhausted) S.running = 0;
}
float controls_stamina(void) { return g_stamina; }

static void note_hover(int kind, BYTE *thing, BYTE *u, BYTE *veh)
{
    g_hover_kind = kind; g_hover_thing = thing; g_hover_range = 0;
    if (kind == HOVER_TARGET || kind == HOVER_FAR_ENEMY) {
        BYTE *shooter = veh ? veh : u;
        int *w = *(int **)(shooter + 0x20);
        g_hover_range = w ? WEAPON_MAX_RANGE(*w) : 0;
    }
}
int controls_hover_range(int *range)
{
    if (g_hover_range <= 0 || (g_hover_kind != HOVER_TARGET && g_hover_kind != HOVER_FAR_ENEMY)) return 0;
    *range = g_hover_range;
    return 1;
}
BYTE *controls_hover_target(void)    /* modern.c: the target under the pointer (item with health at +0x1c / +0x1e), else NULL */
{
    return g_hover_kind == HOVER_TARGET || g_hover_kind == HOVER_FAR_ENEMY ? g_hover_thing : NULL;
}
int controls_reticle(int *x, int *y) { if (!S.have_cursor) return 0; *x = S.cursor_x; *y = S.cursor_y; return 1; }
int controls_route(POINT *pts, int max)
{
    BYTE *u = SARGE;
    if (!u || !S.going || max < 2) return 0;
    if (SARGE_VEHICLE(u) && g_cfg.vehicle_reverse) return vehicles_route(pts, max);   /* vehicles.c: planned poses */
    int n = 0;
    pts[n++] = (POINT){ SARGE_X(u), SARGE_Y(u) };
    for (int i = S.wpi; i < S.nwp && n < max; i++) pts[n++] = S.wp[i];
    if (!S.nwp && n < max) pts[n++] = (POINT){ S.target_x, S.target_y };
    return n;
}

/* Roll toward the side the player is moving (W A S D relative to the way Sarge faces), else left. */
static void dodge_roll(BYTE *u, int kx, int ky)
{
    int f = facing_of(u);
    double fx = FACING_DX[f], fy = FACING_DY[f];
    double cross = fx * ky - fy * kx;              /* > 0: the movement is to Sarge's right on screen */
    int right = (kx || ky) && cross > 0;
    pulse(game_command_key(0, right ? SK_ROLL_R : SK_ROLL_L));    /* the key the game has for it now */
    if (g_cfg.log >= 2) mlog("controls: dodge roll %s (key %02x)", right ? "right" : "left", game_command_key(0, right ? SK_ROLL_R : SK_ROLL_L));
}

static void modern_controls(BYTE *u, int sx, int sy, int *px, int *py, int *pointer, int *field,
                            int *left, int *right, int *kx, int *ky, DWORD now)
{
    BYTE *veh = (BYTE *)(intptr_t)SARGE_VEHICLE(u);

    /* gamepad: left stick = W A S D, right stick = aim (reticle around Sarge), RT = fire, buttons = keys */
    const ModPad *pad = modern_pad();
    if (pad->connected) {
        if (pad->lx * pad->lx + pad->ly * pad->ly > 0.12f) {
            double a = atan2(pad->ly, pad->lx), oct = floor(a / (3.14159265 / 4) + 0.5);
            *kx = (int)lround(cos(oct * 3.14159265 / 4)); *ky = (int)lround(sin(oct * 3.14159265 / 4));
        }
        float m = sqrtf(pad->rx * pad->rx + pad->ry * pad->ry);
        if (m > 0.25f) {
            float r = PAD_AIM_RADIUS * (m < 0.35f ? 0.35f : m > 1 ? 1 : m) / m;
            *px = sx + (int)(pad->rx * r); *py = sy + (int)(pad->ry * r);
            *pointer = *field = 1;
            S.cursor_x = *px; S.cursor_y = *py; S.have_cursor = 1;
            g_pad_aim_at = now;
        }
        if (pad->rt > 0.3f) *right = 1;   /* fire */
        WORD p = pad->pressed;
        if (p & XB_LB) weapons_wheel(1);
        if (p & XB_RB) weapons_wheel(-1);
        if (p & XB_A) pulse(game_command_key(0, SK_DIVE));
        if (p & XB_B) pulse(game_command_key(0, SK_KNEEL));
        if (p & XB_X) pulse(game_command_key(0, SK_ENTER));
        if (p & XB_Y) weapons_quick_swap();
        if (p & XB_BACK) pulse(game_command_key(1, GK_VIEW_MAP));
        if (p & XB_DPAD_UP) pulse(DIK_F1);
        if (p & XB_DPAD_LEFT) pulse(DIK_F2);
        if (p & XB_DPAD_RIGHT) pulse(DIK_F3);
        if (p & XB_DPAD_DOWN) pulse(DIK_TAB);
        if (p & XB_LTHUMB) g_sprint_latched = !g_sprint_latched;
        if (p & XB_RTHUMB) dodge_roll(u, *kx, *ky);
        if ((p & XB_START) && g_dd && g_dd->hwnd) { PostMessageA(g_dd->hwnd, WM_KEYDOWN, VK_ESCAPE, 0x00010001); PostMessageA(g_dd->hwnd, WM_KEYUP, VK_ESCAPE, 0xC0010001); }
        if (!(pad->held & XB_LTHUMB) && !*kx && !*ky && !modern_on(MOD_SPRINT_TOGGLE)) g_sprint_latched = 0;
    }
    /* sprint toggle: the sprint key switches it on and off (it also ends when Sarge stops) */
    {
        static int shift_was;
        int sh = bindings_held(ACT_SPRINT);
        if (modern_on(MOD_SPRINT_TOGGLE) && sh && !shift_was) g_sprint_latched = !g_sprint_latched;
        if (modern_on(MOD_SPRINT_TOGGLE) && !*kx && !*ky && !*right && !S.going && !*left) g_sprint_latched = 0;
        shift_was = sh;
    }
    /* one-shot actions (bindings.c; the defaults: Space / Mouse4 dive, Z roll, T ping, Mouse5 previous weapon,
     * middle click next weapon, F G M J the game's rows when not read by the game itself). X1 / X2 since
     * 2026-11-20 (user design): Mouse4 = dive, Mouse5 = previous weapon - no longer next / previous both. */
    if (bindings_pressed(ACT_DIVE) && !veh) { pulse(game_command_key(0, SK_DIVE)); if (g_cfg.log >= 2) mlog("controls: dive (%s)", bindings_from_mouse(ACT_DIVE) ? "mouse" : "key"); }
    if (bindings_pressed(ACT_ROLL) && !veh) dodge_roll(u, *kx, *ky);
    if (bindings_pressed(ACT_ENTER)) pulse(game_command_key(0, SK_ENTER));
    if (bindings_pressed(ACT_SPECIAL)) pulse(game_command_key(0, SK_SPECIAL));
    if (bindings_pressed(ACT_MAP)) pulse(game_command_key(1, GK_VIEW_MAP));
    if (bindings_pressed(ACT_OBJECTIVE)) pulse(game_command_key(1, GK_OBJECTIVE));
    if (bindings_pressed(ACT_NEXT_WEAPON)) weapons_wheel(-1);
    if (bindings_pressed(ACT_PREV_WEAPON)) weapons_wheel(1);
    /* ping: a marker at the pointer and the squad's Attack Now */
    if (bindings_pressed(ACT_PING) && *pointer) { modern_ping(*px + CAMERA_X, *py + CAMERA_Y); CMD_ATTACK_NOW(); }
    /* radial menu: hold its key / button, move the pointer onto a command, release. Opened ON Sarge (the button
     * pressed on him / the key held with the pointer on him): the stance ring (modern.c STANCE_RADIAL). */
    {
        int d = bindings_held(ACT_RADIAL) && *pointer;
        if (d && !modern_radial_open()) {
            int vk = bindings_source_vk(ACT_RADIAL), mi = vk == VK_MBUTTON ? 0 : vk == VK_XBUTTON1 ? 1 : vk == VK_XBUTTON2 ? 2 : -1;
            g_radial_sarge = !veh && (bindings_from_mouse(ACT_RADIAL) && mi >= 0 ? g_mpress_sarge[mi] : *field && on_sarge(*px, *py, sx, sy));
        }
        if (d || modern_radial_open()) modern_radial(d, *px, *py);
        if (d) { *left = *right = 0; g_block_left = g_block_right = 1; }
    }
    /* Ctrl + click: squad orders (left on an enemy = Attack, left elsewhere = Defend, right = Follow) */
    if (modern_on(MOD_SQUAD_MOUSE) && key_down(VK_CONTROL) && *field) {
        static int lw, rw;
        if (*left && !lw && !g_block_left) {
            BYTE *thing; int h = hover_at(u, veh, *px + CAMERA_X, *py + CAMERA_Y, &thing);
            if (h == HOVER_TARGET || h == HOVER_FAR_ENEMY) CMD_ATTACK(); else CMD_DEFEND();
            g_block_left = 1;
            if (g_cfg.log >= 2) mlog("controls: Ctrl + click -> squad %s", h == HOVER_TARGET || h == HOVER_FAR_ENEMY ? "attack" : "defend");
        }
        if (*right && !rw && !g_block_right) { CMD_FOLLOW(); g_block_right = 1; }
        lw = *left; rw = *right;
    }
    if (g_block_left) { if (!*left) g_block_left = 0; *left = 0; }
    if (g_block_right) { if (!*right) g_block_right = 0; *right = 0; }
}

/* controls_camera: edge peek (hold its key) looks four times further toward the pointer */
static float peek_scale(void) { return bindings_held(ACT_PEEK) ? 4.0f : 1.0f; }   /* feature switch: bindings.c */

static void apply_wheel(BYTE *u);

/* Sarge on foot, both mouse schemes: fire = shoot at the pointer (held = keep shooting),
 * fire_was = fire was held last frame, walk_hold = walk toward the pointer, walk_tap = walk to the
 * pointer's spot (along a route from pathfind.c; onto a vehicle: climb in), kx/ky = W A S D. */
#define ROUTE_POINT_REACHED  10     /* world px: a route corner counts as reached this close */
#define ROUTE_TIMEOUT_MS     30000

/* Where a lob goes: the pointer's world point (for a short while it was the hovered target's position - off
 * to the side on multi-part objects; the user wants the pointer). */
static int g_throw_aiming;           /* hold-to-aim throw in progress (button held, circle shown) */
static void throw_target(int px, int py, int *wx, int *wy)
{
    /* always the pointer (user 2026-11-17: "the center should be ... where the mouse is"); the target's own
     * position was off to the side for multi-part objects (Boot Camp boards) */
    *wx = px + CAMERA_X; *wy = py + CAMERA_Y;
}

/* [debug] fence_route = tx,ty[,delay_ms[,sx,sy]] (cheat_keys; testing break_fences in the container, where right-click
 * taps on a world spot are hard to aim): once, delay_ms (default 4000) into the mission - Sarge first placed at sx,sy
 * (the game's placement 0x448a90, invulnerable) and half a second later - a walk to tx,ty as if right-tapped there
 * (on a vehicle: walk to it and climb in). Testing squad_sprint (squad.c): walk_ms = time from the placement to the
 * walk (default 500), run = 1 sprints along the route (as a double-click tap), follow = 1 gives the squads the Follow order
 * then (squad.c). Format tx,ty,delay,sx,sy,walk_ms,run,follow. */
static int g_debug_route_run;
static int debug_fence_route(BYTE *u, int *x, int *y, DWORD now)
{
    static int state, tx, ty, delay, sx, sy, walk_after, follow, n; static int prepped; static DWORD t0;
    if (state == 3 || !g_cfg.cheat_keys) return 0;
    if (state == 0) {
        char s[64] = "";
        GetPrivateProfileStringA("debug", "fence_route", "", s, sizeof s, mod_ini_path());
        n = sscanf(s, "%d,%d,%d,%d,%d,%d,%d,%d", &tx, &ty, &delay, &sx, &sy, &walk_after, &g_debug_route_run, &follow);
        if (n < 6 || walk_after < 0) walk_after = 500;
        if (n < 2) { state = 3; return 0; }
        if (n < 3) delay = 4000;
        state = 1; t0 = now; return 0;
    }
    if (state == 2 && n >= 8 && follow && !prepped && now - t0 + 300 >= (DWORD)delay) {
        /* squad test: in the container the right button read as held from the squad's rendezvous on (Wine) and Sarge
         * walked toward the pointer: ignore it until a real release, start again from sx,sy (a few frames before the walk) */
        typedef void (__fastcall *PlaceFn)(BYTE *unit, int x, int y);
        ((PlaceFn)0x448a90)(u, sx, sy);
        S.right_blocked = 1; prepped = 1;
        mlog("controls: [debug] fence_route: Sarge placed again at %d,%d, right button ignored", SARGE_X(u), SARGE_Y(u));
    }
    if (now - t0 < (DWORD)delay) return 0;
    if (state == 1 && n >= 5) {
        typedef void (__fastcall *PlaceFn)(BYTE *unit, int x, int y);
        *(volatile BYTE *)0x69752c = 1;                                  /* invulnerable */
        ((PlaceFn)0x448a90)(u, sx, sy);
        mlog("controls: [debug] fence_route: Sarge placed at %d,%d -> %d,%d", sx, sy, SARGE_X(u), SARGE_Y(u));
        state = 2; t0 = now; delay = walk_after; return 0;
    }
    state = 3; *x = tx; *y = ty;
    mlog("controls: [debug] fence_route: %s to %d,%d", g_debug_route_run ? "sprint" : "walk", tx, ty);
    if (n >= 8 && follow) squad_debug_follow();
    return 1;
}

static void modern_on_foot(BYTE *u, int sx, int sy, int px, int py, int pointer, int left, int fire_was,
                           int right_hold, int right_tap, int kx, int ky, DWORD now)
{
    int dbg_x, dbg_y, dbg_tap = debug_fence_route(u, &dbg_x, &dbg_y, now);
    if ((right_tap && pointer) || dbg_tap) {
        S.stuck = 0; S.check_at = 0; S.detour_until = now;
        S.going = 1;
        S.target_x = dbg_tap ? dbg_x : px + CAMERA_X;
        S.target_y = dbg_tap ? dbg_y : py + CAMERA_Y;
        S.going_since = now;
        if (dbg_tap) S.route_run = g_debug_route_run;
        /* on a vehicle: walk to it and climb in */
        S.enter = vehicle_at(S.target_x, S.target_y);
        if (S.enter) {
            S.target_x = VEH_X(S.enter);
            S.target_y = VEH_Y(S.enter);
            {   /* the side NEAREST Sarge (2026-11-20, user: "entering the vehicle is picking the opposite side"): the vehicle's
                 * centre is blocked, the route then ended at any free cell round it - often behind. Walk out from the
                 * centre toward Sarge to the first free point (+4 px), up to 64 px. */
                double vx = SARGE_X(u) - S.target_x, vy = SARGE_Y(u) - S.target_y, len = sqrt(vx * vx + vy * vy);
                if (len > 1) {
                    vx /= len; vy /= len;
                    for (int d = 4; d <= 64; d += 2) {
                        int x = S.target_x + (int)(vx * d), y = S.target_y + (int)(vy * d);
                        if (!controls_blocked_at(x, y)) { S.target_x = S.target_x + (int)(vx * (d + 4)); S.target_y = S.target_y + (int)(vy * (d + 4)); break; }
                    }
                }
            }
            if (try_enter_vehicle(u, S.enter)) { S.going = 0; S.enter = NULL; return; }
            if (g_cfg.log >= 2) mlog("controls: walking to vehicle %p at %d,%d", S.enter, S.target_x, S.target_y);
        }
        /* closed off (across water, inside a fence): no walk at all (user: it used to walk to the nearest point) */
        if (!S.enter && path_reachable(SARGE_X(u), SARGE_Y(u), S.target_x, S.target_y, 30) == 0) {
            S.going = 0; S.nwp = 0;
            if (g_cfg.log >= 2) mlog("controls: walk to %d,%d: unreachable, not walking", S.target_x, S.target_y);
        } else {
            /* a route around walls and fences (Army Men II walks a clicked spot with path finding) */
            S.nwp = path_find(SARGE_X(u), SARGE_Y(u), S.target_x, S.target_y, S.wp, (int)(sizeof S.wp / sizeof *S.wp));
            S.wpi = 0;
        }
    }

    if (kx || ky || right_hold) S.going = 0;
    if (S.going && now - S.going_since > (S.nwp ? ROUTE_TIMEOUT_MS : GOTO_TIMEOUT_MS)) S.going = 0;

    /* aim: the pointer, whenever it is over the battlefield */
    int aim = -1;
    if (pointer && (px != sx || py != sy)) aim = facing_toward(px - sx, py - sy);

    if (kx || ky) {
        if (left && aim >= 0) {
            /* run and gun: keep aiming, walk forward or backward along the aim */
            set_facing(u, aim);
            double fwd = FACING_DX[aim] * kx + FACING_DY[aim] * ky;
            INPUT = (INPUT & ~(INPUT_TURN_LEFT | INPUT_TURN_RIGHT | INPUT_FORWARD | INPUT_BACK)) |
                    (fwd >= 0 ? INPUT_FORWARD : INPUT_BACK);
        } else {
            walk_toward(u, kx, ky);
        }
    } else if (right_hold && aim >= 0) {
        walk_toward(u, px - sx, py - sy);
    } else if (S.going && !left) {
        /* the next corner of the route (or the spot itself without a route) */
        int ex = S.nwp ? S.wp[S.nwp - 1].x : S.target_x, ey = S.nwp ? S.wp[S.nwp - 1].y : S.target_y;
        int tx = ex, ty = ey;
        if (S.nwp) {
            while (S.wpi < S.nwp - 1) {
                int dx = S.wp[S.wpi].x - SARGE_X(u), dy = S.wp[S.wpi].y - SARGE_Y(u);
                if (dx * dx + dy * dy > ROUTE_POINT_REACHED * ROUTE_POINT_REACHED) break;
                S.wpi++;
                S.stuck = 0;
            }
            tx = S.wp[S.wpi].x; ty = S.wp[S.wpi].y;
        }
        int vx = tx - SARGE_X(u), vy = ty - SARGE_Y(u);
        int fx = ex - SARGE_X(u), fy = ey - SARGE_Y(u);
        if (S.enter && try_enter_vehicle(u, S.enter)) { S.going = 0; S.enter = NULL; }
        else if (fx * fx + fy * fy <= ARRIVE_DISTANCE * ARRIVE_DISTANCE) S.going = 0;
        else walk_around_obstacles(u, vx, vy, now);
    } else if (left && aim >= 0 && !(INPUT & (INPUT_TURN_LEFT | INPUT_TURN_RIGHT | INPUT_ROLL))) {
        set_facing(u, aim);                    /* standing and firing: turn to the pointer */
    }
    /* Otherwise Sarge does not turn: the pointer is a free cursor (controls_aim_point). */

    if (weapon_is_hold_to_aim(u) && modern_on(MOD_THROW_POINTER)) {
        /* Throw at the pointer (Army Men II): one click throws. The game's charge (FUN_0043e6e0) is a
         * 2000 ms triangle from its start time 0x6969e0 on the game's animation clock (*(0x4b3f70)+0x44),
         * 0 -> 100 % in the first second; the range display puts the shell at min + (max - min) x %. So:
         * frame 0 presses fire (the game starts the charge), frame 1 moves the start back by % x 10 ms,
         * frame 2 releases: the throw goes to the pointer's distance, clamped to the weapon's range. */
        static int phase; static int pct;
        S.firing = S.pressing = 0;
        if (g_throw_cancel) {
            /* cancelled (both buttons, 2026-11-21 user: "right click cancel is not working for grenades"): the throw machine used
             * to see the left button as let go and moved on to the release = the throw. Now it stops here: no release, fire
             * stays held (the game throws on the RELEASE of fire) until both buttons are up and the weapon is selected again
             * (cancel_hold, apply_modern_controls) - the charge is dropped without a throw. */
            if (phase && g_cfg.log >= 1) mlog("controls: throw cancelled (phase %d) - no throw", phase);
            phase = 0; g_throw_pending = 0; g_throw_aiming = 0;
            INPUT |= INPUT_FIRE;
            S.charging = 1;
            return;
        }
        S.charging = phase != 0;
        if (phase == 0 && left && !fire_was && aim >= 0) {
            int *w = *(int **)(u + 0x20);
            int lo = w ? ((RangeFn)0x445c50)(*w) : 0, hi = w ? WEAPON_MAX_RANGE(*w) : 0;
            int dx = px - sx, dy = py - sy;
            double d = sqrt((double)dx * dx + (double)dy * dy);
            pct = hi > lo ? (int)((d - lo) * 100 / (hi - lo)) : 100;
            if (pct < 0) pct = 0;
            if (pct > 90) pct = 90;   /* the charge bar never at its peak (the real distance is set at the shot) */
            set_facing(u, aim);
            INPUT |= INPUT_FIRE | INPUT_FIRE_PRESSED;
            S.throw_started_at = 0;
            phase = 1;
            g_throw_pending = 1; throw_target(px, py, &g_throw_wx, &g_throw_wy);   /* exact: controls_fire_shot */
            g_throw_aiming = modern_on(MOD_THROW_HOLD);
            if (g_cfg.log >= 2) mlog("controls: throw at pointer: %d px (range %d..%d) -> %d %%", (int)d, lo, hi, pct);
        } else if (phase == 1) {
            /* keep fire held until Sarge is in his aiming stance (state 4): a grenade gets there at once,
             * the mortar first sets up - released earlier, its throw never came (Wine 2026-11-17). The
             * distance itself is set exactly when the shot fires (throw_at_pointer_fire). */
            static DWORD since; static int frames;
            if (frames++ == 0) since = now;
            BYTE *game = *(BYTE **)0x4b3f70;
            if (game) *(DWORD *)0x6969e0 = *(DWORD *)(game + 0x44) - (DWORD)pct * 10;
            if (aim >= 0) set_facing(u, aim);
            INPUT |= INPUT_FIRE;
            int ready = SARGE_STATE(u) == 4;
            if (g_cfg.log >= 3) mlog("controls: throw hold f%d state %d", frames, SARGE_STATE(u));
            if (g_throw_aiming) {
                /* hold-to-aim mode ([modern] throw_hold): fire stays held while the button is, the target follows
                 * the pointer (the circle shows it), the release throws (user 2026-11-17 "alternative mode") */
                if (left) throw_target(px, py, &g_throw_wx, &g_throw_wy);
                else { phase = 2; frames = 0; g_throw_aiming = 0; }
            } else if ((ready && frames >= 2) || now - since > 1500) { phase = 2; frames = 0; }
        } else if (phase == 2) {
            phase = 3;                                       /* released: the throw */
            S.throw_released_at = now;
            if (!S.throw_started_at) S.throw_started_at = now;
        } else if (phase == 3 && g_throw_pending && SARGE_WEAPON_TYPE(u) == 5 && SARGE_STATE(u) != 4 && SARGE_STATE(u) != STATE_SHOOTING &&
                   now - S.throw_released_at > 250 && now - S.throw_started_at < 3000) {
            /* mortar only (a full-power grenade throw fires late in its animation: a re-press there threw
             * twice - user 2026-11-17), and only while Sarge is not aiming / throwing. No shot came: a mortar press while standing only makes Sarge kneel (binary: DO_FIRE ->
             * AVATAR_KNEEL for ARMY_MORTAR); the next press + release fires. Press again. */
            if (aim >= 0) set_facing(u, aim);
            INPUT |= INPUT_FIRE | INPUT_FIRE_PRESSED;
            phase = 1;
            if (g_cfg.log >= 2) mlog("controls: throw: no shot yet (state %d) - pressing again", SARGE_STATE(u));
        } else if (phase == 3 && !left && (!g_throw_pending || now - S.throw_started_at >= 3000)) {
            phase = 0;                                       /* next throw after the button is let go */
            g_throw_pending = 0; S.throw_started_at = 0;
        }
    } else if (weapon_is_hold_to_aim(u)) {
        /* hold = charge (the game shows the range), release = throw */
        S.firing = S.pressing = 0;
        if (g_throw_cancel) { INPUT |= INPUT_FIRE; S.charging = 1; return; }   /* cancelled: no release = no throw */
        S.charging = left;
        if (left) {
            INPUT |= INPUT_FIRE;
            if (!fire_was) INPUT |= INPUT_FIRE_PRESSED;
        }
    } else {
        S.charging = 0;
        fire_pulse(u, left, now);
    }
    if (g_cfg.log >= 2) {                            /* debugging: what the controls decided */
        static DWORD last;
        if (now - last >= (g_cfg.log >= 3 ? 30 : 250)) {
            last = now;
            mlog("controls: sarge %d,%d facing %d state %d weapon %d | pointer %s%d,%d aim %d | keys x%+d y%+d F%d W%d going %d (route %d/%d) | input %#lx",
                 sx, sy, facing_of(u), *(int *)(u + 0x68), SARGE_WEAPON_TYPE(u), pointer ? "" : "(off) ", px, py, aim, kx, ky,
                 left, right_hold, S.going, S.wpi, S.nwp, INPUT);
        }
    }
    static int first_frame_logged;
    if (!first_frame_logged++) {
        mlog("controls: first gameplay frame done (sarge %d,%d facing %d)", sx, sy, facing_of(u));
        clock_trace_start(GetPrivateProfileIntA("debug", "trace_seconds", 0, mod_ini_path()));   /* [debug] trace_seconds */
    }
}

static void apply_modern_controls(void)
{
    BYTE *u = SARGE;
    if (!u || MENU_CURSOR_SHOWN || !layout_view_is_gameplay()) {
        confine_pointer(0);
        S.going = 0;
        S.right_was_down = S.left_was_down = 0;
        S.active = 0;
        S.charging = 0;
        g_hide_reticle = 0; g_cursor_kind = -1; S.lmode = LM_NONE; g_picker = 0; g_stance_want = 0;
        return;
    }
    if (!S.active) {
        /* Gameplay just started (or a menu closed): the click that got us here must not fire or
         * walk. Clear the "pressed since last check" flags and ignore buttons until released. */
        GetAsyncKeyState(VK_LBUTTON);
        GetAsyncKeyState(VK_RBUTTON);
        S.left_blocked = key_down(VK_LBUTTON);
        S.right_blocked = key_down(VK_RBUTTON);
        controls_game_keys(1);                    /* the game's key table: modern layout (pause menu shows it) */
        bindings_reset_state();                   /* keys / buttons held now: ignored until released */
        S.left_was_down = S.right_was_down = 0;
        S.active = 1;
        return;
    }
    confine_pointer(1);
    apply_wheel(u);
    path_regions_step();                         /* pathfind.c: whole-map walkable areas, a slice per frame */                              /* mouse wheel: weapons */
    int sx = SARGE_X(u) - CAMERA_X, sy = SARGE_Y(u) - CAMERA_Y;   /* Sarge on screen */
    int px = 0, py = 0;
    int field = pointer_on_battlefield(&px, &py);

    /* Mouse buttons. A press that STARTS over the battlefield belongs to the game ("captured")
     * and keeps working while it is held, even if the pointer drifts over the HUD (walking,
     * firing). A press that starts over the HUD is a click on the interface: it never fires or
     * walks. GetAsyncKeyState low bit = pressed since the last check, so a click that starts and
     * ends between two frames still counts. */
    SHORT lstate = GetAsyncKeyState(VK_LBUTTON), rstate = GetAsyncKeyState(VK_RBUTTON);
    int lheld = lstate < 0 || (lstate & 1), rheld = rstate < 0;
    static int lheld_before;
    if (lheld && !lheld_before && !field) {       /* a click on the interface: air support, a weapon box? */
        int ux, uy;
        if (pointer_in_window(&ux, &uy) && !air_support_click(ux, uy) && !squad_click(ux, uy)) weapons_click_at(ux, uy);
    }
    lheld_before = lheld;
    if (g_air_pending >= 0 && !lheld) {          /* the strike / paratroop HUD box was let go: target mode on now */
        /* 2026-11-20 (user: "air strike UI does not enable the air strike state. and it needs to be cancellable ...
         * cancel via clicking again on the UI"): the handler TOGGLES (FUN_0043f6c0) - when the game's own HUD click
         * had already switched it on, our call switched it off again. Now the release sets the WANTED state:
         * off -> on; on in this box's mode -> off (click again = cancel); on in the other mode -> that mode. */
        int want_mode = g_air_pending == 0 ? 5 : 6;
        int cancel = g_air_was_on && g_air_was_mode == want_mode;
        if (cancel) {
            if (COMMAND_CURSOR == 1) ((UnitFn)COMMAND_TOGGLE)(NULL);
        } else {
            if (COMMAND_CURSOR == 1 && COMMAND_MODE != want_mode) ((UnitFn)COMMAND_TOGGLE)(NULL);
            if (COMMAND_CURSOR != 1) ((VoidFnPtr)AIR_HANDLER[g_air_pending])();
        }
        mlog("controls: HUD %s %s (command cursor %d, mode %d)", AIR_NAME[g_air_pending], cancel ? "cancelled" : "- pick the target",
             COMMAND_CURSOR, COMMAND_MODE);
        g_air_pending = -1;
    }
    if (g_air_pending < 0) { g_cmd_prev_on = COMMAND_CURSOR == 1; g_cmd_prev_mode = COMMAND_MODE; }
    static int rheld_before;
    int rpressed = rheld || (rstate & 1);
    if (rpressed && !rheld_before && !field) {       /* right click on the interface: drop that weapon */
        int ux, uy;
        if (pointer_in_window(&ux, &uy)) weapons_drop_at(ux, uy);
    }
    rheld_before = rpressed;
    if (!lheld) S.left_capture = 0;
    else if (!S.left_capture && !S.left_was_down && field) S.left_capture = 1;
    if (!rheld) S.right_capture = 0;
    else if (!S.right_capture && !S.right_was_down && field) S.right_capture = 1;
    int pointer = field;
    if (!field && (S.left_capture || S.right_capture)) pointer = pointer_in_window(&px, &py);
    int left = S.left_capture && lheld;
    int right = S.right_capture && rheld;
    int right_quick_click = field && !right && (rstate & 1) && !S.right_was_down;
    if (S.left_blocked) { if (lstate >= 0) S.left_blocked = 0; left = 0; }
    if (S.right_blocked) { if (rstate >= 0) S.right_blocked = 0; right = right_quick_click = 0; }
    DWORD now = GetTickCount();
    if (right && !S.right_was_down) {
        /* a second press soon after the last one (double-click) that stays held = run */
        S.running = S.right_up_at && now - S.right_up_at <= GetDoubleClickTime();
        S.right_down_at = now;
    }
    if (right_quick_click) S.right_up_at = now;
    if (!right && S.right_was_down) { S.right_up_at = now; S.running = 0; }
    if (!right) S.running = 0;
    int right_tap = right_quick_click ||
                    (!right && S.right_was_down && now - S.right_down_at < (DWORD)g_cfg.controls_tap_ms);
    int right_hold = right && (S.running || now - S.right_down_at >= (DWORD)g_cfg.controls_tap_ms);
    /* keyboard / extra mouse buttons: the bindings (bindings.c). Mouse buttons count only when pressed over the
     * battlefield (the middle click / hold always did); the F10 panel open = nothing counts. */
    bindings_poll(now, field, panel_is_open());
    if ((lstate & 1) || (rstate & 1)) bindings_note_device(0);    /* a left / right click: the mouse is in use */
    {                                                            /* where the extra buttons were pressed (stance radial) */
        static const int MVK[3] = { VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        static int was[3];
        for (int i = 0; i < 3; i++) {
            int d = key_down(MVK[i]);
            if (d && !was[i]) g_mpress_sarge[i] = field && !SARGE_VEHICLE(u) && on_sarge(px, py, sx, sy);
            was[i] = d;
        }
    }
    memset(g_force, 0, sizeof g_force);
    if (bindings_held(ACT_SLOW) && !bindings_direct(ACT_SLOW)) g_force[game_command_key(0, SK_SLOW)] = 1;   /* Slow on a shared key */
    /* walking: a screen direction (default W A S D) */
    int kx = bindings_held(ACT_RIGHT) - bindings_held(ACT_LEFT);
    int ky = bindings_held(ACT_DOWN) - bindings_held(ACT_UP);
    /* the sprint key held while moving = sprint (same as double-click and hold): after modern_controls below */
    /* quick swap to the previous weapon (Q); hold to drop the current item (X 400 ms -> F8 = Drop Item) */
    if (bindings_pressed(ACT_QUICK_SWAP)) weapons_quick_swap();
    if (bindings_pressed(ACT_DROP)) pulse(0x42);
    /* the six weapon boxes (A1 A2 A3 B1 B2 B3; default 1-6) */
    for (int k = 0; k < 6; k++) if (bindings_pressed(ACT_WEAPON1 + k)) weapons_select_box(k);
    /* stance keys (default C: tap = one down, hold = prone, double-tap = stand up) */
    if (!SARGE_VEHICLE(u)) {
        if (bindings_pressed(ACT_KNEEL)) stance_cycle(u, now, "stance-down key");
        if (bindings_pressed(ACT_PRONE)) stance_goto(u, POSE_PRONE, now, bindings_from_mouse(ACT_PRONE) ? "prone button" : "prone key (hold)");
        if (bindings_pressed(ACT_STAND)) stance_goto(u, POSE_STAND, now, bindings_from_mouse(ACT_STAND) ? "stand button" : "stand key (double-tap)");
    }
    g_mid_radial = 0;
    modern_controls(u, sx, sy, &px, &py, &pointer, &field, &left, &right, &kx, &ky, now);
    {                                                             /* sprint: Shift (or its toggle) */
        if (sprint_wanted() && (kx || ky || right)) S.running = 1;
    }

    {
        /* (The only mouse scheme since 2026-11-17: the split left-fire / right-walk scheme was removed - user:
         * "nuke the old mouse scheme".) One-button mouse (Army Men II): the LEFT button does what is under the pointer when it is
         * pressed - an enemy in range: shoot (held = keep shooting); your empty vehicle: walk there and
         * climb in; the ground: click = walk there along a route, hold = walk toward the pointer
         * (double-click + hold = run; a hold right next to Sarge only turns him for the first 500 ms).
         * A press keeps its meaning while held, so a walk never turns into shooting when the pointer
         * crosses an enemy. Shift + left, a movement key held + left, or the RIGHT button = shoot at
         * the pointer anywhere (force fire). No walking for 300 ms after a shot. */
        BYTE *veh = (BYTE *)(intptr_t)SARGE_VEHICLE(u);
        /* Air strike / paratroopers picking a target (the game's command cursor, 0x4b8a88 = 1, switched on by the
         * HUD box or F5 / F6 - 0x440dc0 / 0x440d30 -> FUN_0043f6c0, mode 0x696ec8 = 5 strike / 6 paratroop):
         * a left click on the battlefield = there - the game's command click FUN_00441000 (__fastcall ecx 1)
         * launches at the game cursor (0x4fe3e8 / ec); a right click cancels (FUN_0043f6c0 toggles it off).
         * Our walk / fire stays out of it (user 2026-11-17: "toggle on ... with a left click so I can choose
         * where"; the click used to walk Sarge and the request never came). */
        if (COMMAND_CURSOR == 1) {
            /* the game's own click handlers (0x440880 left -> FUN_00441000 ecx 1, 0x4411c0 right -> ecx 0) still run
             * and act on the game cursor: keep it on the pointer, and click ourselves only if the game has not
             * acted a few frames after a press (else it launched twice) */
            static int cl_was, cr_was, wait_l, wait_r;
            static DWORD seen_at;
            if (now - seen_at > 200) { wait_l = wait_r = 0; cl_was = left; cr_was = right; }   /* mode just started */
            seen_at = now;
            int cl = left && !cl_was, cr = right && !cr_was;
            cl_was = left; cr_was = right;
            if (pointer && field) {             /* the game cursor AND ours (the camera pans with it: the gunsight
                                                   routine that normally keeps S.cursor current does not run here) */
                CURSOR_X = px; CURSOR_Y = py;
                S.cursor_x = px; S.cursor_y = py; S.have_cursor = 1;
            }
            if (cl && pointer && field) { wait_l = 3; mlog("controls: air support target at %d,%d (mode %d)", px + CAMERA_X, py + CAMERA_Y, *(int *)0x696ec8); }
            else if (cr) wait_r = 3;
            if (wait_l && --wait_l == 0 && COMMAND_CURSOR == 1) { ((UnitFn)COMMAND_CLICK)((BYTE *)1); mlog("controls: air support: the game did not take the click - sent"); }
            if (wait_r && --wait_r == 0 && COMMAND_CURSOR == 1) { ((UnitFn)COMMAND_TOGGLE)(NULL); mlog("controls: air support cancelled"); }
            g_hide_reticle = 0; g_cursor_kind = -1;
            S.right_was_down = right; S.left_was_down = 0; S.lmode = LM_NONE;
            /* W A S D still move Sarge (the camera pans toward the pointer meanwhile: controls_camera) - picking a
             * target used to lock everything (user 2026-11-17) */
            if (!veh) modern_on_foot(u, sx, sy, px, py, pointer, 0, 0, 0, 0, kx, ky, now);
            return;
        }
        BYTE *thing = NULL;
        int hover = pointer && field ? hover_at(u, veh, px + CAMERA_X, py + CAMERA_Y, &thing) : HOVER_GROUND;
        hover_selftest(u, now);
        note_hover(hover, thing, u, veh);
        static int raw_was;                     /* the left button itself last frame */
        int lpress = left && !raw_was, lrelease = !left && raw_was;
        raw_was = left;
        int lmode_before = S.lmode;
        static int lrun;
        static int press_on_sarge;                 /* this press started on Sarge himself (stance click) */
        if (lpress) {
            S.lpress_at = now; S.lpress_x = px; S.lpress_y = py;
            int dbl = S.lup_at && now - S.lup_at <= GetDoubleClickTime();
            if (layout_any_panel_open()) S.lmode = LM_NONE;          /* closing a pop-up (sign, radio) */
            /* a radio drawn by the mod (radio.c) is no panel: the click that closes the briefing / mission-over radio
             * walked Sarge there (2026-11-20, user) - bDisplayRecall 0x505d00, SCREEN state 16 */
            else if (*(volatile BYTE *)0x505d00 || (*(BYTE **)0x6a5944 && *(int *)(*(BYTE **)0x6a5944 + 4) == 16)) S.lmode = LM_NONE;
            else if (bindings_held(ACT_SPRINT) || kx || ky || hover == HOVER_TARGET || hover == HOVER_FAR_ENEMY) S.lmode = LM_FIRE;   /* sprint key + left = force fire */
            else if (hover == HOVER_ENTER) S.lmode = LM_ENTER;
            /* in a fire fight (user: "if I move the mouse off the enemy while clicking, sarge will start running over"):
             * a click within combat_lock_ms of the last shot FIRES at the pointer instead of walking ([controls]
             * combat_lock_ms, default 1500; 0 = the old 300 ms "no walk" pause) */
            else if (now - S.fire_end_at < (DWORD)combat_lock_ms()) S.lmode = combat_lock_ms() > WALK_LOCK_AFTER_SHOT_MS ? LM_FIRE : LM_NONE;
            else S.lmode = LM_PENDING;
            lrun = (S.lmode == LM_PENDING || S.lmode == LM_ENTER) && dbl;   /* double-click on a vehicle too: sprint to it (2026-11-20, user) */
            press_on_sarge = pointer && field && !veh && hover == HOVER_GROUND && on_sarge(px, py, sx, sy);
            if (g_cfg.log >= 2) mlog("controls: left press -> %s (hover %d)", S.lmode == LM_FIRE ? "fire" : S.lmode == LM_ENTER ? "enter" :
                                     S.lmode == LM_PENDING ? (lrun ? "run" : "walk") : "none", hover);
        }
        /* the drag stance picker: a press on Sarge dragged up / down STANCE_PICKER_DRAG px opens three pose boxes
         * above him (modern.c draws them); the release picks the box nearest the pointer's height; no walk, no turn */
        if (left && press_on_sarge && !g_picker && (S.lmode == LM_PENDING || S.lmode == LM_TURN || S.lmode == LM_WALK) && pointer && abs(py - S.lpress_y) >= STANCE_PICKER_DRAG) {
            g_picker = 1; S.lmode = LM_PICK;
            if (g_cfg.log >= 2) mlog("controls: stance picker open");
        }
        if (g_picker && left && pointer) g_picker_sel = picker_select(u, py + CAMERA_Y);
        if (g_picker && lrelease) {
            g_picker = 0;
            if (PICK_POSE[g_picker_sel]) stance_goto(u, PICK_POSE[g_picker_sel], now, "drag picker");
            else { pulse(game_command_key(0, SK_DIVE)); if (g_cfg.log >= 2) mlog("controls: dive (drag picker)"); }
        }
        if (g_picker && !left) g_picker = 0;
        if (left && (S.lmode == LM_PENDING || S.lmode == LM_TURN)) {
            int dxs = px - sx, dys = py - sy;
            int close_by = dxs * dxs + dys * dys <= TURN_ONLY_RADIUS * TURN_ONLY_RADIUS && now - S.lpress_at < TURN_ONLY_MS;
            /* the second press of a double-click also waits tap_ms: released quickly it is a TAP = sprint along the
             * route (it used to switch to hold-walk at once, and its release stopped the route - user 2026-11-17);
             * held longer it is the double-click + hold run */
            if (now - S.lpress_at >= (DWORD)g_cfg.controls_tap_ms) S.lmode = close_by && !lrun ? LM_TURN : LM_WALK;
            if (S.lmode == LM_TURN && pointer && (dxs || dys)) turn_toward(u, facing_toward(dxs, dys));
        }
        int tap = lrelease && (lmode_before == LM_PENDING || lmode_before == LM_ENTER);
        int tap_run = tap && lrun;                  /* double-click: sprint to the spot */
        /* click on Sarge = stance (2026-11-20 user design): click = one down (lying: stand up), double-click = prone.
         * Standing / kneeling the first click acts at once (the second only goes further down); lying, the click
         * waits SARGE_DBL_MS for a second one (else a double-click would stand him up and lie him down again). */
        static DWORD sarge_tap_at; static int sarge_pending;
        if (tap && press_on_sarge && lmode_before == LM_PENDING) {
            tap = tap_run = 0;
            if (sarge_tap_at && now - sarge_tap_at <= SARGE_DBL_MS) { sarge_tap_at = 0; sarge_pending = 0; stance_goto(u, POSE_PRONE, now, "double-click on Sarge"); }
            else { sarge_tap_at = now; if (SARGE_POSE(u) == POSE_PRONE) sarge_pending = 1; else stance_cycle(u, now, "click on Sarge"); }
        }
        if (sarge_pending && now - sarge_tap_at > SARGE_DBL_MS) { sarge_pending = 0; stance_cycle(u, now, "click on prone Sarge"); }
        if (lrelease) press_on_sarge = 0;
        /* right click on one of your soldiers: select his unit (no shot at him) */
        static int rsel;
        if (right && !S.right_was_down && pointer && field && !veh) {
            BYTE *un = own_unit_at(px + CAMERA_X, py + CAMERA_Y);
            if (un) { SELECT_UNIT(un); rsel = 1; mlog("controls: right click selected unit %p", un); }
        }
        if (!right) rsel = 0;
        if (lrelease) { S.lup_at = now; if (lmode_before == LM_FIRE) S.fire_end_at = now; S.lmode = LM_NONE; lrun = 0; }
        if (right && !S.right_was_down) S.fire_end_at = now;
        if (!right && S.right_was_down) S.fire_end_at = now;
        /* CANCEL (2026-11-20, user: "i need a way to cancel shooting - also if I'm holding down the button. for all
         * weapons"): pressing the OTHER button while one is held (both down) stops firing at once, ends the fire lock
         * (the next click walks again) and ignores both buttons until both are up. A charged throw that is cancelled
         * is held (no release = no throw) until both buttons are up, then the weapon is selected again. */
        static int cancel_hold;
        /* a right CLICK while left is held counts too: pressed and let go between two frames it is never "held" (2026-11-21) */
        int right_flash = left && !right && (rstate & 1) && !S.right_was_down;
        int both_new = (left && right && (!S.right_was_down || lpress)) || right_flash;
        if (both_new && !cancel_hold) {
            if (S.charging) g_throw_cancel = 1;   /* a charged throw: never released */
            cancel_hold = 1; S.lmode = LM_NONE; S.fire_end_at = 0; g_fire_cancel_at = now; lrun = 0; g_picker = 0;
            S.firing = S.pressing = 0;
            if (g_cfg.log >= 1) mlog("controls: fire cancelled (both buttons)");
        }
        if (cancel_hold && !left && !right) {
            cancel_hold = 0;
            if (S.charging && *(signed char *)(u + 0x2c4) >= 0) { int c = *(signed char *)(u + 0x2c4); *(signed char *)(u + 0x2c4) = -1; ((int (__fastcall *)(BYTE *, int))0x448df0)(u, c); }   /* drop the charge */
            if (g_throw_cancel) g_throw_cancel_until = now + 600;   /* fire held a little longer: the reselect ends the aim */
        }
        if (g_throw_cancel && !cancel_hold && g_throw_cancel_until && (now > g_throw_cancel_until || SARGE_STATE(u) != 4)) {
            g_throw_cancel = 0; g_throw_cancel_until = 0;
            if (g_cfg.log >= 2) mlog("controls: throw cancel done (state %d)", SARGE_STATE(u));
        }
        g_fire_lock_until = S.fire_end_at && combat_lock_ms() > WALK_LOCK_AFTER_SHOT_MS ? S.fire_end_at + combat_lock_ms() : 0;
        /* what the rest of this function sees: fire = "left", walk hold / walk tap = the old right button */
        static int fire_was;
        int fire = (left && S.lmode == LM_FIRE) || (right && !rsel);
        /* firing stops a walk in progress (2026-11-20, user: "shooting should also immediately stop any path traversal") */
        if (fire && !fire_was && (S.going || S.nwp)) { S.going = 0; S.nwp = 0; S.route_run = 0; S.running = 0; if (g_cfg.log >= 2) mlog("controls: fire - walk stopped"); }
        if (cancel_hold) { fire = 0; tap = tap_run = 0; if (S.charging) INPUT |= INPUT_FIRE; }
        right_hold = left && S.lmode == LM_WALK;
        right_tap = tap;
        S.running = right_hold && lrun;
        if (tap_run) S.route_run = 1; else if (tap) S.route_run = 0;
        else if (!S.going) S.route_run = 0;
        if (!right_hold && S.route_run && S.going) S.running = 1;     /* double-clicked route: sprint */
        if (sprint_wanted() && (kx || ky)) S.running = 1;
        apply_stamina(now, kx || ky || right_hold);
        left = fire;
        /* pointer: the reticle where a click shoots, the walk ring / enter arrow elsewhere */
        int kind;
        if (!pointer || !field) kind = -1;
        else if (fire) kind = -1;
        else if (right_hold || S.lmode == LM_TURN) kind = HOVER_GROUND;
        else kind = hover == HOVER_TARGET || hover == HOVER_FAR_ENEMY ? -1 : hover;   /* any target: the reticle (range shown on it) */
        g_hover_state = hover == HOVER_TARGET ? 1 : hover == HOVER_FAR_ENEMY ? 0 : -1;
        if (kind != -1 && veh && kind == HOVER_ENTER) kind = HOVER_GROUND;
        if (veh && pointer && field && !fire && on_own_vehicle(veh, px, py)) kind = CUR_EXIT;   /* click = get out */
        if (kind == -1 && !veh && pointer && field) {     /* aiming (on a target / firing): the weapon's own pointer */
            int wt = SARGE_WEAPON_TYPE(u);
            kind = wt == 2 ? CUR_GRENADE : wt == 5 ? CUR_SHELL : wt == WEAPON_BAZOOKA_TYPE ? CUR_ROCKET : -1;
        }
        if (kind == -1 && veh && pointer && field) {      /* a tank aiming: its shell */
            BYTE *vw = *(BYTE **)(veh + 0x20);
            if (vw && *(int *)vw == 6) kind = CUR_TANKROUND;   /* ARMY_LARGE_CANNON */
        }
        if (kind == HOVER_GROUND && !veh && !right_hold && S.lmode != LM_TURN &&
            pointer_unreachable(u, px + CAMERA_X, py + CAMERA_Y, now)) kind = CUR_NOWALK;   /* cannot walk there */
        /* one cursor (user): over the interface the Windows arrow shows - no reticle left on the battlefield */
        g_hide_reticle = kind != -1 || controls_pointer_on_ui();
        if (kind != g_cursor_kind) {
            g_cursor_kind = kind;
            ensure_cursors();
            if (field) SetCursor(controls_cursor());
        }
        S.right_was_down = right;
        BYTE *vehicle = veh;
        if (vehicle) {
            vehicle_controls(u, vehicle, pointer, px, py, left, right_hold, right_tap, kx, ky, now);
            S.firing = S.pressing = S.charging = 0;
            S.left_was_down = fire_was = left;
            return;
        }
        modern_on_foot(u, sx, sy, px, py, pointer, left, fire_was, right_hold, right_tap, kx, ky, now);
        stance_update(u, now);                  /* click on Sarge: standing up step by step */
        S.left_was_down = fire_was = left;
    }
}

/* ---------------------------------------------------------------- hook */

/* Wrap of FUN_0041fca0, the game's per-frame keyboard dispatcher (no arguments). */
VoidFnPtr controls_dispatch_original;

/* Testing aids ([debug] cheat_keys): the game's own cheat routines (the typed cheats "Telekinetic"
 * and "Invulnerable", table at 0x4c5f10), on keys. Teleport = to the game cursor (the pointer). */
#define CHEAT_TELEPORT      ((VoidFnPtr)0x46b970)   /* FUN_00448a90 Sarge -> cursor, if the ground is passable */
#define CHEAT_INVULNERABLE  ((VoidFnPtr)0x46b8f0)   /* toggles DAT_0069752c */
#define INVULNERABLE        (*(volatile BYTE *)0x69752c)

typedef void (__fastcall *PlaceUnitFn)(BYTE *unit, int x, int y);
#define PLACE_UNIT ((PlaceUnitFn)0x448a90)   /* what "Telekinetic" uses to move Sarge */
typedef int (__fastcall *TileFn)(int tile);
#define TILE_BLOCKED ((TileFn)0x42a230)      /* Telekinetic: non-zero = Sarge can't stand there */
#define MAP_TILES_W  (*(unsigned short *)0x4e368c)

/* Ctrl+B: Sarge next to the nearest of the player's usable, empty vehicles (testing). */
static void teleport_to_vehicle(BYTE *u)
{
    BYTE *army = ARMY_RECORD(0), *best = NULL;
    long best_d = 0x7fffffff;
    int n = army ? *(signed char *)(army + 0x50) : 0;
    for (int k = 0; k < n; k++) {
        BYTE *v = *(BYTE **)(army + 0x54 + 4 * k);
        if (!v || !VEH_USABLE(v) || VEH_DRIVER(v)) continue;
        long dx = VEH_X(v) - SARGE_X(u), dy = VEH_Y(v) - SARGE_Y(u), d = dx * dx + dy * dy;
        if (d > 0 && d < best_d) { best_d = d; best = v; }
    }
    if (!best) { mlog("cheat: no usable vehicle"); return; }
    static const int off[][2] = { { 0, 40 }, { 40, 0 }, { -40, 0 }, { 0, -40 }, { 0, 56 }, { 56, 0 }, { -56, 0 }, { 0, -56 } };
    for (size_t k = 0; k < sizeof off / sizeof off[0]; k++) {
        int x = SARGE_X(u), y = SARGE_Y(u), tx = VEH_X(best) + off[k][0], ty = VEH_Y(best) + off[k][1];
        if (tx < 0 || ty < 0 || TILE_BLOCKED((ty >> 4) * MAP_TILES_W + (tx >> 4))) continue;
        PLACE_UNIT(u, tx, ty);
        if (SARGE_X(u) != x || SARGE_Y(u) != y) {
            mlog("cheat: Sarge to %d,%d next to vehicle %p (type %d) at %d,%d", SARGE_X(u), SARGE_Y(u), best, VEH_TYPE(best), VEH_X(best), VEH_Y(best));
            return;
        }
    }
    mlog("cheat: no free ground next to vehicle %p", best);
}

/* Ctrl+K (testing, 2026-11-19 - user: "need to see tan soldiers death. add a cheat to auto kill them"): the nearest
 * enemy soldier on the map takes a lethal bullet through the game's own ItemDamage (0x431f60, __fastcall ecx = item,
 * edx = damage, stack: damage type, SPOINT *from, UID from; binary) - his real death animation plays
 * (DMGTYPE_BULLET 6 from Sarge's position: the shot death, as from a rifle). */
typedef void (__fastcall *ItemDamageFn)(void *item, int damage, int type, short *from, DWORD from_uid);
#define ITEM_DAMAGE     ((ItemDamageFn)0x431f60)
#define DMGTYPE_BULLET  6
static void kill_nearest_enemy(BYTE *u)
{
    BYTE *best = NULL; long best_d = 0x7fffffff;
    int ox = SARGE_X(u), oy = SARGE_Y(u), px, py;
    if (pointer_on_battlefield(&px, &py)) { ox = px + CAMERA_X; oy = py + CAMERA_Y; }   /* nearest the pointer (game picture -> world) */
    for (int a = 1; a <= 3; a++) {
        if (!LEGION_STATUS(a) || ALLIED(0, a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg) continue;
        BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
        for (int g = 0; un && un < end && g < 64; un += UNIT_STRIDE, g++) {
            int mode = *(int *)(un + 0x70);
            if (mode < 3 || mode > 5) continue;
            int last = *(int *)(un + 0xdc);
            for (int i = 0; i <= last && i < 10; i++) {
                BYTE *t = ((BYTE **)(un + 0xb4))[i];
                if (!t || *(int *)(t + 0x68) != 1 || *(short *)(t + 0x1c) <= 0) continue;
                long dx = *(short *)(t + 4) - ox, dy = *(short *)(t + 6) - oy, d = dx * dx + dy * dy;
                if (d < best_d) { best_d = d; best = t; }
            }
        }
    }
    if (!best) { mlog("cheat: no enemy soldier on the map"); return; }
    short from[2] = { (short)SARGE_X(u), (short)SARGE_Y(u) };
    mlog("cheat: kill enemy soldier %p at %d,%d (health %d; Sarge %d,%d, picked near %d,%d)", best, *(short *)(best + 4), *(short *)(best + 6), *(short *)(best + 0x1c), SARGE_X(u), SARGE_Y(u), ox, oy);
    ITEM_DAMAGE(best, 1000, DMGTYPE_BULLET, from, *(DWORD *)u);
}

static void cheat_keys(void)
{
    static int t_was, i_was;
    HWND h = g_dd ? g_dd->hwnd : NULL;
    int ctrl = key_down(VK_CONTROL), fg = h && GetForegroundWindow() == h;
    int t = fg && ctrl && key_down('T'), i = fg && ctrl && key_down('I');
    BYTE *u = SARGE;
    if (t && !t_was && u && !SARGE_VEHICLE(u) && layout_view_is_gameplay() && !MENU_CURSOR_SHOWN) {
        int x = SARGE_X(u), y = SARGE_Y(u), px, py, cx = CURSOR_X, cy = CURSOR_Y;
        int on_field = pointer_on_battlefield(&px, &py);
        if (on_field) { CURSOR_X = px; CURSOR_Y = py; }       /* the game cursor is not the pointer in gameplay */
        CHEAT_TELEPORT();
        if (on_field) { CURSOR_X = cx; CURSOR_Y = cy; }
        mlog("cheat: teleport %d,%d -> %d,%d%s", x, y, SARGE_X(u), SARGE_Y(u),
             x == SARGE_X(u) && y == SARGE_Y(u) ? " (refused: ground not passable)" : "");
    }
    if (i && !i_was && u && layout_view_is_gameplay()) {
        CHEAT_INVULNERABLE();
        mlog("cheat: invulnerable %s", INVULNERABLE ? "ON" : "OFF");
    }
    /* Ctrl+G (testing): the current weapon becomes a grenade, again = mortar, again = bazooka, with the game's own
     * table values (AI_WTBL at 0x505044, 24 bytes per type: +0x10 damage, +0x14 / +0x16 velocities) */
    static int g_was;
    int gk = fg && ctrl && key_down('G');
    if (gk && !g_was && u && !SARGE_VEHICLE(u) && layout_view_is_gameplay()) {
        BYTE *w = *(BYTE **)(u + 0x20);
        if (w) {
            int type = *(int *)w == 2 ? 5 : *(int *)w == 5 ? 4 : *(int *)w == 4 ? 3 : 2;   /* grenade -> mortar -> bazooka -> flamethrower -> grenade */
            const BYTE *t = (const BYTE *)(intptr_t)(0x505044 + type * 24);
            *(int *)w = type;
            *(short *)(w + 0x0c) = *(short *)(w + 0x0e) = 50;
            *(short *)(w + 0x10) = *(const short *)(t + 0x10);
            *(short *)(w + 0x14) = *(const short *)(t + 0x14);
            *(short *)(w + 0x16) = *(const short *)(t + 0x16);
            *(short *)(w + 0x12) = *(const short *)(t + 0);       /* reload ms (rateOfFire.nFrameDelay, as MakeWeapon) */
            *(int *)(w + 8) = *(short *)(w + 0x12);                /* ready */
            mlog("cheat: weapon -> type %d (velocity %d, z %d, reload %d ms, range %d..%d)", type, *(short *)(w + 0x14), *(short *)(w + 0x16), *(short *)(w + 0x12),
                 ((RangeFn)0x445c50)(type), WEAPON_MAX_RANGE(type));
        }
    }
    g_was = gk;
    static int a_was;                             /* Ctrl+A: air support refilled (the game's "Aeroballistics" cheat) */
    int ak = fg && ctrl && key_down('A');
    if (ak && !a_was && u && layout_view_is_gameplay()) { ((VoidFnPtr)0x46b8e0)(); mlog("cheat: air support refilled"); }
    a_was = ak;
    static int k_was;
    int kk = fg && ctrl && key_down('K');
    if (kk && !k_was && u && layout_view_is_gameplay()) kill_nearest_enemy(u);
    k_was = kk;
    static int v_was;
    int vk = fg && ctrl && key_down('B');
    if (vk && !v_was && u && !SARGE_VEHICLE(u) && layout_view_is_gameplay() && !MENU_CURSOR_SHOWN) teleport_to_vehicle(u);
    /* Ctrl+Y (testing): Sarge to [debug] teleport_to = x,y (world px) - to stage a spot the rig cannot walk to */
    static int y_was;
    int yk = fg && ctrl && key_down('Y');
    if (yk && !y_was && u && !SARGE_VEHICLE(u)) {
        char s[32] = ""; static int presses;
        GetPrivateProfileStringA("debug", "teleport_to", "", s, sizeof s, mod_ini_path());
        const char *comma = strchr(s, ',');
        if (comma) {
            int tx = atoi(s), ty = atoi(comma + 1) - 24 * presses++;   /* each further press: 24 px up */
            PLACE_UNIT(u, tx, ty);
            mlog("cheat: Sarge to %d,%d -> now %d,%d", tx, ty, SARGE_X(u), SARGE_Y(u));
        }
    }
    y_was = yk;
    t_was = t; i_was = i; v_was = vk;
}

static void throw_track_frame(void);   /* below: measuring throws */
static void controls_dispatch_body(void)
{
    if (g_cfg.controls_modern) filter_keyboard();
    controls_dispatch_original();
    if (g_cfg.cheat_keys) cheat_keys();
    throw_track_frame();
    if (g_cfg.controls_modern) apply_modern_controls();
    gameplay_frame();                                /* gameplay.c: health regeneration */
    modern_frame();                                  /* modern.c: mission stats, half damage */
    resize_poll();                                   /* resize.c: a fullscreen switch made in a menu */
    weapons_frame();                                 /* weapons.c: spare weapons after a drop */
}

/* ---------------------------------------------------------------- weapons on the mouse wheel
 * Modern controls: the wheel steps through Sarge's weapons (weapons.c: the three slots and their
 * spares; not in a vehicle, not while the fire input is held). Right-clicking no longer
 * changes weapons (patches.c, MODERN table). Left-clicking the weapon buttons on the HUD still
 * works (the game's own buttons); clicks on the HUD panel never fire. */
static volatile LONG g_wheel;                    /* wheel notches since the last frame (+ = up) */

int controls_wheel(int delta)
{
    if (panel_wheel(delta)) return 1;                /* the F10 panel open: it scrolls (BINDS page) */
    if (!g_cfg.controls_modern || !layout_view_is_gameplay() || MENU_CURSOR_SHOWN) return 0;
    InterlockedExchangeAdd(&g_wheel, delta / WHEEL_DELTA ? delta / WHEEL_DELTA : (delta > 0 ? 1 : -1));
    return 1;
}

static void apply_wheel(BYTE *u)
{
    LONG notches = InterlockedExchange(&g_wheel, 0);
    if (notches) weapons_wheel((int)notches);     /* weapons.c: slots and their spares */
}

/* Wrap of FUN_0043eff0, which puts Sarge's aiming reticle (the cursor in gameplay; screen
 * position at 0x6968d4 as two shorts) at a fixed distance in front of him. In modern mode the
 * reticle is a free cursor at the mouse pointer instead. */
VoidFnPtr controls_aim_point_original;

void __cdecl controls_aim_point(void)
{
    controls_aim_point_original();
    if (!g_cfg.controls_modern) return;
    BYTE *u = SARGE;
    if (!u) return;
    /* charging a throw by holding (throw at the pointer OFF): the game shows its range; with throw at the
     * pointer the reticle stays at the pointer (user: holding the click showed the original range reticle) */
    if (!SARGE_VEHICLE(u) && S.charging && !modern_on(MOD_THROW_POINTER)) return;
    int px, py;
    if (GetTickCount() - g_pad_aim_at > 1500 && pointer_on_battlefield(&px, &py)) { S.cursor_x = px; S.cursor_y = py; S.have_cursor = 1; }
    if (!S.have_cursor) return;
    /* Always the last pointer position on the battlefield - never the game's fixed distance. */
    *(short *)0x6968d4 = (short)S.cursor_x;
    *(short *)0x6968d6 = (short)S.cursor_y;
}


/* Wrap of FUN_00447d80, which sets the direction a shot will travel (unit +0x25, 0..255) just before
 * firing. It uses Sarge's facing, EXCEPT when the game has picked a target for him (+0x94, the
 * built-in aim assist / gunsight): then the shot goes toward that target (FUN_00445fe0 ->
 * FUN_00414160), which is why shots went somewhere else than a far-away cursor. In modern mode the
 * shot goes exactly toward the cursor. Hold-to-aim weapons (grenades) keep the game's own aiming.
 * __fastcall: ecx = unit (ret). */
UnitFn controls_shot_direction_original;

/* FUN_00447a80: where the shot leaves the gun = unit position + the muzzle point of the current
 * animation frame (frame record +8/+10). __fastcall: ecx = unit, edx = short[2] out (world). */
typedef void (__fastcall *MuzzleFn)(BYTE *unit, short *out);
#define GAME_MUZZLE_POSITION ((MuzzleFn)0x447a80)

/* Points the shot (unit +0x25) at the cursor, measured FROM THE MUZZLE: the bullet starts there,
 * not at Sarge's centre, so aiming from the centre sent it parallel to the right line, offset by
 * the gun's position - most visible on diagonals. The muzzle point changes with the animation
 * frame, so this runs again when the shot is fired. when = log label (NULL: no log).
 * Returns 0 when modern aiming does not apply. */
static int aim_shot_at_cursor(BYTE *unit, const char *when)
{
    if (!g_cfg.controls_modern || unit != SARGE || SARGE_VEHICLE(unit) ||
        weapon_is_hold_to_aim(unit) || !S.have_cursor)
        return 0;
    short muzzle[2];
    GAME_MUZZLE_POSITION(unit, muzzle);
    int vx = S.cursor_x + CAMERA_X - muzzle[0], vy = S.cursor_y + CAMERA_Y - muzzle[1];
    if (modern_on(MOD_AIM_ASSIST) && g_hover_kind == HOVER_TARGET && g_hover_thing) {   /* aim assist: the target's body */
        int raw = (int)((*(DWORD *)g_hover_thing >> 22) & 0xf);
        vx = *(short *)(g_hover_thing + 4) - muzzle[0];
        vy = *(short *)(g_hover_thing + 6) - (raw == 5 || raw == 6 ? 8 : 0) - muzzle[1];
    }
    /* the target at or behind the muzzle (pointer right next to Sarge; the bazooka's muzzle is far out): the
     * muzzle -> target vector points BACKWARDS (user: "double shot of bazooka in opposite direction") - aim from
     * Sarge himself then */
    {
        int tx = muzzle[0] + vx, ty = muzzle[1] + vy, ax = tx - SARGE_X(unit), ay = ty - SARGE_Y(unit);
        int mx = muzzle[0] - SARGE_X(unit), my = muzzle[1] - SARGE_Y(unit);
        if ((long)vx * ax + (long)vy * ay <= 0 || (long)ax * ax + (long)ay * ay <= (long)mx * mx + (long)my * my) { vx = ax; vy = ay; }
    }
    if (vx == 0 && vy == 0) return 0;
    BYTE game_dir = *(BYTE *)(unit + 0x25);
    *(BYTE *)(unit + 0x25) = (BYTE)facing_toward(vx, vy);
    if (g_cfg.log >= 2 && when)
        mlog("controls: shot direction %s %d (game chose %d, target %p), muzzle %+d,%+d from Sarge, cursor %d,%d away",
             when, *(BYTE *)(unit + 0x25), game_dir, *(void **)(unit + 0x94),
             muzzle[0] - SARGE_X(unit), muzzle[1] - SARGE_Y(unit), vx, vy);
    return 1;
}

void __fastcall controls_shot_direction(BYTE *unit)
{
    controls_shot_direction_original(unit);
    aim_shot_at_cursor(unit, NULL);              /* aiming stance; logged only when fired */
}

/* Wrap of FUN_00447c30, which FIRES the shot from the muzzle in direction +0x25 (-> FUN_00445c70).
 * Called from Sarge's update (FUN_004474c0) and 0x43a1b0. The direction is set again here, with
 * the muzzle point of the frame the shot is actually fired from. __fastcall: ecx = unit (ret). */
UnitFn controls_fire_shot_original;

/* Throw at the pointer: the shell lands where the player clicked. The game does not aim a lob at a
 * point - the throw "distance" d (Sarge +0x28, 0..max range) only scales the launch speeds
 * (binary ProjectileSet = exe 0x435250, checked in the disassembly): v = minv + (V - minv) d/max
 * across, vz = minz + (Z - minz) d/max up (V, Z = WEAPON +0x14 / +0x16); the shell bursts when it reaches
 * the ground (it does not slide on): lands L = v (vz + sqrt(vz^2 + 2 g h)) / g past its start point.
 * Measured in Wine 2026-11-17 (shell traced per frame, controls.c throw_track_*):
 *   - start height h ~14 (grenade and mortar), start point = Sarge + a hand/muzzle offset (north: 6,-33);
 *   - gravity: each game frame vz -= (int)(400 * dt / 1000) (0x4b48b0 = 400, dt = 0x6918f4 ms): the
 *     truncation makes it ~360..400 depending on the frame time -> g_eff from the game's dt;
 *   - first model (h 10, g 400, from Sarge) landed 25-45 % short of the real bursts.
 * Exe constants: mortar minz 225 / minv 100 (0x4b48b4/b8), grenade minz 130 / minv 100 (0x4b48bc/c0). */
#define GRAVITY_PER_S   (*(int *)0x4b48b0)
#define GAME_FRAME_DT   (*(volatile int *)0x6918f4)   /* ms of the last projectile update */
#define LOB_LAUNCH_Z    14.0
static double g_lob_dt = 16;                          /* smoothed game frame time (throw_track_frame) */
static double g_lob_scale = 1.0;                    /* measured flight / model, learned from each throw (throw_track_frame) */

static double lob_gravity(void)
{
    if (framerate_gravity()) return framerate_gravity();   /* framerate.c: exact gravity, same at any frame rate */
    double g = GRAVITY_PER_S;
    if (g < 50 || g > 5000) g = 400;
    /* the game frame time read at 0x6918f4 was 16..17 ms, but gravity 6 per 16 ms = 375 fitted every
     * measured throw (17 ms -> 353 overshot the mortar by 5 %): the projectile clock steps 16 ms */
    double dt = 16;
    double step = floor(g * dt / 1000);               /* what the game takes off vz per frame */
    return step > 0 ? step * 1000 / dt : g;
}

#define LOB_F_MIN -0.75                       /* shortest throw: 75 % below the minimum speeds */
static int throw_distance_for(BYTE *w, double want, double *landed, double *shortest, double *longest)
{
    int type = *(int *)w, max = WEAPON_MAX_RANGE(type);
    double V = *(short *)(w + 0x14), Z = *(short *)(w + 0x16), g = lob_gravity();
    double minz = type == 5 ? *(int *)0x4b48b4 : *(int *)0x4b48bc, minv = type == 5 ? *(int *)0x4b48b8 : *(int *)0x4b48c0;
    if (minz <= 0 || minz > 2000) minz = type == 5 ? 225 : 130;
    if (minv <= 0 || minv > 2000) minv = 100;
    if (max <= 0) return 0;
    want /= g_lob_scale;                            /* learned correction (this PC's frame time) */
    #define LAND(f) ((minv + (V - minv) * (f)) * ((minz + (Z - minz) * (f)) + \
                     sqrt((minz + (Z - minz) * (f)) * (minz + (Z - minz) * (f)) + 2 * g * LOB_LAUNCH_Z)) / g)
    /* f below 0 (a negative distance) slows the shell under the weapon's minimum speeds: ProjectileSet takes it
     * as is, so throws land closer than the normal minimum (user 2026-11-17: "let the mouse throw be this
     * close"); LOB_F_MIN keeps both speeds well above 0 (grenade at -0.75: v 25, vz 77 -> ~15 px) */
    double lo = LOB_F_MIN, hi = 1;
    if (want <= LAND(LOB_F_MIN)) hi = LOB_F_MIN;
    else if (want >= LAND(1.0)) lo = 1;
    else for (int i = 0; i < 30; i++) { double m = (lo + hi) / 2; if (LAND(m) < want) lo = m; else hi = m; }
    double f = (lo + hi) / 2;
    if (landed) *landed = LAND(f) * g_lob_scale;
    if (shortest) *shortest = LAND(LOB_F_MIN) * g_lob_scale;
    if (longest) *longest = LAND(1.0) * g_lob_scale;
    #undef LAND
    return (int)lround(f * max);
}

/* A throw from start (sx, sy) toward the pointer (tx, ty): always along Sarge's aim (Sarge -> pointer) - the
 * hand can be past a pointer right next to Sarge, then the old start -> pointer vector pointed BACKWARDS (the
 * preview said 14 ft behind him). want = how far past the start the pointer lies along that line. */
static void lob_aim(BYTE *u, int tx, int ty, double sx, double sy, double *ux, double *uy, double *want)
{
    double ax = tx - SARGE_X(u), ay = ty - SARGE_Y(u), l = sqrt(ax * ax + ay * ay);
    if (l < 1) { ax = 0; ay = -1; l = 1; }
    *ux = ax / l; *uy = ay / l;
    *want = (tx - sx) * *ux + (ty - sy) * *uy;
    if (*want < 1) *want = 1;
}

/* Where the shell starts for a throw toward (tx, ty). At the shot (exact): the game's muzzle point
 * (0x447a80, the throwing hand of this animation frame) + 12 px down - so in all 12 measured throws
 * (Wine 2026-11-17). Before it (preview, Sarge may still face elsewhere): measured starts fitted as
 * Sarge + base + r x direction: grenade (1,-6) + (30,28) x dir, mortar (2,-2) + (25,18) x dir (within ~6 px). */
#define LOB_START_BELOW_MUZZLE 12
static void lob_start(BYTE *u, int tx, int ty, double *sx, double *sy, int exact)
{
    if (exact) {
        short m[2]; GAME_MUZZLE_POSITION(u, m);
        *sx = m[0]; *sy = m[1] + LOB_START_BELOW_MUZZLE;
        return;
    }
    double dx = tx - SARGE_X(u), dy = ty - SARGE_Y(u), l = sqrt(dx * dx + dy * dy);
    if (l < 1) l = 1;
    int mortar = SARGE_WEAPON_TYPE(u) == 5;
    *sx = SARGE_X(u) + (mortar ? 2 : 1) + (mortar ? 25 : 30) * dx / l;
    *sy = SARGE_Y(u) + (mortar ? -2 : -6) + (mortar ? 18 : 28) * dy / l;
}

static int g_track_mx, g_track_my; static double g_track_model;     /* the game's muzzle point at the throw (compared with the shell's start) */

/* the throw fires now: direction and distance for the clicked spot (set before the game uses them) */
static void throw_at_pointer_fire(BYTE *unit)
{
    if (!g_throw_pending || unit != SARGE || SARGE_VEHICLE(unit) || !weapon_is_hold_to_aim(unit)) return;
    g_throw_pending = 0;
    BYTE *w = *(BYTE **)(unit + 0x20);
    if (!w) return;
    double sx, sy;
    lob_start(unit, g_throw_wx, g_throw_wy, &sx, &sy, 1);
    double dx, dy, want, landed = 0;
    lob_aim(unit, g_throw_wx, g_throw_wy, sx, sy, &dx, &dy, &want);
    int d = throw_distance_for(w, want, &landed, NULL, NULL);
    *(BYTE *)(unit + 0x25) = (BYTE)facing_toward(dx, dy);
    *(short *)(unit + 0x28) = (short)d;
    g_track_model = landed;
    short m[2]; GAME_MUZZLE_POSITION(unit, m); g_track_mx = m[0]; g_track_my = m[1];
    if (g_cfg.log >= 2) mlog("controls: throw (weapon %d) at %d px from %d,%d -> distance %d of %d, lands ~%d px (g %.0f, dt %.1f, muzzle %+d,%+d)",
                             *(int *)w, (int)want, (int)sx, (int)sy, d, WEAPON_MAX_RANGE(*(int *)w), (int)landed, lob_gravity(), g_lob_dt,
                             m[0] - SARGE_X(unit), m[1] - SARGE_Y(unit));
}

/* modern.c: where a throw at the pointer would land now (world), and the blast radius. Grenade and mortar
 * both burst with LargeExplosion (binary GrenadeExplode / MortarExplode; exe records 0x4bd040 /
 * 0x4bd050 both point to the 96 x 96 pattern 0x4b4888): items inside the 96 x 96 square around the burst
 * are damaged by how much of them is inside (game binary ProcessExplosionLayer); the mortar's 4 extra puffs
 * within 31 px do no damage. Shown as a circle of 48. The landing point is the pointer, pulled in / out
 * to the throw's shortest / longest landing distance. */
#define BLAST_RADIUS 48
#define BAZOOKA_BLAST_RADIUS 32         /* BazookaExplode: MediumExplosion 64 x 64 (exe record 0x4bd030 -> 0x4b4878) */
#define WEAPON_BAZOOKA 4
#define WEAPON_LARGE_CANNON 6           /* tank gun (game table ARMY_LARGE_CANNON; damage type missile = BazookaExplode) */
int controls_throw_preview(int *wx, int *wy, int *radius, int *type, int *clamped, int *dist)
{
    BYTE *u = SARGE;
    if (!g_cfg.controls_modern || !u || !S.have_cursor || controls_pointer_on_ui()) return 0;
    if (SARGE_VEHICLE(u)) {                                      /* a tank's cannon: like the bazooka (user 2026-11-17) */
        BYTE *v = (BYTE *)(intptr_t)SARGE_VEHICLE(u), *vw = *(BYTE **)(v + 0x20);
        int on = (g_hover_kind == HOVER_TARGET || g_hover_kind == HOVER_FAR_ENEMY) && g_hover_thing;
        if (!vw || *(int *)vw != WEAPON_LARGE_CANNON || !on) return 0;   /* LARGE_CANNON: DMGTYPE_MISSILE -> 64 x 64 */
        *wx = S.cursor_x + CAMERA_X; *wy = S.cursor_y + CAMERA_Y;
        *radius = BAZOOKA_BLAST_RADIUS; *type = WEAPON_LARGE_CANNON; *clamped = 0;
        double ex = *wx - VEH_X(v), ey = *wy - VEH_Y(v); *dist = (int)lround(sqrt(ex * ex + ey * ey));
        return 1;
    }
    BYTE *w = *(BYTE **)(u + 0x20);
    if (!w || *(short *)(w + 0xc) == 0) return 0;              /* no ammo */
    int wt = *(int *)w;
    int on_target = (g_hover_kind == HOVER_TARGET || g_hover_kind == HOVER_FAR_ENEMY) && g_hover_thing;
    /* when it shows (user 2026-11-17): on an enemy / target only - or while a hold-to-aim throw is held */
    if (wt == WEAPON_BAZOOKA) {                                  /* the rocket bursts on the target: 64 x 64 pattern */
        if (!on_target) return 0;
        *wx = S.cursor_x + CAMERA_X; *wy = S.cursor_y + CAMERA_Y;   /* at the pointer (the rocket flies there) */
        *radius = BAZOOKA_BLAST_RADIUS; *type = wt; *clamped = 0;
        double ex = *wx - SARGE_X(u), ey = *wy - SARGE_Y(u); *dist = (int)lround(sqrt(ex * ex + ey * ey));
        return 1;
    }
    if (!weapon_is_hold_to_aim(u) || !modern_on(MOD_THROW_POINTER) || (!on_target && !g_throw_aiming)) return 0;
    int tx, ty;
    if (g_throw_aiming) { tx = g_throw_wx; ty = g_throw_wy; } else throw_target(S.cursor_x, S.cursor_y, &tx, &ty);
    double sx, sy;
    lob_start(u, tx, ty, &sx, &sy, 0);
    double dx, dy, want, landed = 0, shortest = 0, longest = 0;
    lob_aim(u, tx, ty, sx, sy, &dx, &dy, &want);
    throw_distance_for(w, want, &landed, &shortest, &longest);
    *wx = (int)lround(sx + dx * landed);
    *wy = (int)lround(sy + dy * landed);
    *radius = BLAST_RADIUS;
    *type = wt;
    /* pointer out of reach: the circle sits at the limit - 1 = beyond the longest throw, -1 = closer than the shortest */
    *clamped = want > longest + 6 ? 1 : want < shortest - 6 ? -1 : 0;
    { double ex = *wx - SARGE_X(u), ey = *wy - SARGE_Y(u); *dist = (int)lround(sqrt(ex * ex + ey * ey)); }
    return 1;
}

/* Measuring throws (log >= 1, one line per throw; log >= 2 every frame): the shell is a FLOAT_OBJ in the
 * army's pool (ProjectileSet 0x435250: 0x4f7820 + army * 0x19e8 + slot * 0x88 + 0x434, 20 slots, next slot
 * index at 0x4f7c50 + army * 0x19e8; its shadow 0xaa0 further). Object +4/+6 world x/y (short), +0x68 ANIM*;
 * shadow +0x6c distance travelled; ANIM +0x18 height, +0xb2 z speed, +0xac ground speed; ANIM +0 bit 0 active. */
#define FLOAT_POOL(army)      (0x4f7820 + (army) * 0x19e8)
#define FLOAT_NEXT(army)      (*(int *)(0x4f7c50 + (army) * 0x19e8))
#define UID_ARMY_OF(u)        ((int)((*(DWORD *)(u) >> 26) & 7) - 1)
static BYTE *g_track;                  /* the thrown shell, NULL = none */
static int g_track_fx, g_track_fy, g_track_px, g_track_py, g_track_lx, g_track_ly, g_track_d, g_track_w, g_track_frames;
static DWORD g_track_t0;

static void throw_track_start(BYTE *unit, int army, int slot_before)
{
    if (FLOAT_NEXT(army) == slot_before) return;           /* nothing was fired */
    g_track = (BYTE *)(intptr_t)(FLOAT_POOL(army) + slot_before * 0x88 + 0x434);
    g_track_fx = SARGE_X(unit); g_track_fy = SARGE_Y(unit);
    g_track_lx = *(short *)(g_track + 4); g_track_ly = *(short *)(g_track + 6);
    g_track_d = *(short *)(unit + 0x28);
    BYTE *w = *(BYTE **)(unit + 0x20); g_track_w = w ? *(int *)w : 0;
    g_track_frames = 0; g_track_t0 = GetTickCount();
}

static int g_track_sx = -1, g_track_sy, g_track_st; static int g_track_lz = 99;   /* the shell's first seen position */
static void throw_track_frame(void)
{
    int dt = GAME_FRAME_DT;                      /* the game's frame time: lob gravity (controls_throw_preview) */
    if (dt >= 10 && dt <= 50) g_lob_dt += (dt - g_lob_dt) * 0.02;   /* hiccups (Wine: 66 ms) left out */
    if (!g_track) return;
    BYTE *anim = *(BYTE **)(g_track + 0x68), *shadow = g_track + 0xaa0, *sanim = *(BYTE **)(shadow + 0x68);
    int active = anim && (*(DWORD *)anim & 1);
    int x = *(short *)(g_track + 4), y = *(short *)(g_track + 6);
    if (g_cfg.log >= 2 && active)
        mlog("throw: f%d t%lu pos %d,%d z %d vz %d v %d travelled %d", g_track_frames, GetTickCount() - g_track_t0, x, y,
             *(short *)(anim + 0x18), *(short *)(anim + 0xb2), sanim ? *(short *)(sanim + 0xac) : -1, *(int *)(shadow + 0x6c));
    if (active && g_track_sx < 0) { g_track_sx = x; g_track_sy = y; g_track_st = *(int *)(shadow + 0x6c); }
    if (active) { g_track_lx = x; g_track_ly = y; g_track_lz = anim ? *(short *)(anim + 0x18) : 99; }
    if (!active || ++g_track_frames > 600) {
        int dx = g_track_lx - g_track_fx, dy = g_track_ly - g_track_fy;
        int px = g_track_px - g_track_fx, py = g_track_py - g_track_fy;
        mlog("throw: weapon %d distance %d of %d: burst at %d px (%d,%d) after %lu ms; aimed %d px (%d,%d), off by %d px",
             g_track_w, g_track_d, WEAPON_MAX_RANGE(g_track_w), (int)sqrt((double)dx * dx + dy * dy), g_track_lx, g_track_ly,
             GetTickCount() - g_track_t0, (int)sqrt((double)px * px + py * py), g_track_px, g_track_py,
             (int)sqrt((double)(g_track_lx - g_track_px) * (g_track_lx - g_track_px) + (double)(g_track_ly - g_track_py) * (g_track_ly - g_track_py)));
        { double fx = g_track_lx - g_track_sx, fy = g_track_ly - g_track_sy;
          /* learn: a throw that ended on the ground (not on a tree / wall) corrects later ones (Windows 16.6 ms
           * frames flew 4 % further than the Wine-fitted model) */
          double flew = sqrt(fx * fx + fy * fy) + g_track_st;
          if (g_track_model > 40 && g_track_lz <= 12) {
              double ratio = flew / g_track_model;
              if (ratio > 0.85 && ratio < 1.15) { g_lob_scale *= sqrt(ratio); mlog("throw: model scale now %.3f", g_lob_scale); }
          }
          g_track_lz = 99;
          mlog("throw: shell first seen at %+d,%+d from Sarge (after %d px), game muzzle %+d,%+d; flew %d px, model %d px",
               g_track_sx - g_track_fx - (int)(g_track_st * (fx / (sqrt(fx * fx + fy * fy) + 1e-9))), g_track_sy - g_track_fy - (int)(g_track_st * (fy / (sqrt(fx * fx + fy * fy) + 1e-9))),
               g_track_st, g_track_mx - g_track_fx, g_track_my - g_track_fy, (int)(sqrt(fx * fx + fy * fy) + g_track_st), (int)g_track_model); }
        g_track_sx = -1;
        g_track = NULL;
    }
}

void __fastcall controls_fire_shot(BYTE *unit)
{
    int tracking = g_throw_pending && unit == SARGE && weapon_is_hold_to_aim(unit);
    int army = tracking ? UID_ARMY_OF(unit) : 0, slot = tracking ? FLOAT_NEXT(army) : 0;
    if (tracking) { g_track_px = g_throw_wx; g_track_py = g_throw_wy; }
    throw_at_pointer_fire(unit);
    aim_shot_at_cursor(unit, "(fire)");
    if (unit == SARGE) modern_note_shot();          /* modern.c: mission stats */
    if (unit == SARGE && g_cfg.log >= 2) mlog("controls: SHOT weapon %d direction %d facing %d (pointer at %d) state %d distance %d at %lu", SARGE_WEAPON_TYPE(unit),
                                             *(BYTE *)(unit + 0x25), facing_of(unit), S.have_cursor ? facing_toward(S.cursor_x + CAMERA_X - SARGE_X(unit), S.cursor_y + CAMERA_Y - SARGE_Y(unit)) : -1,
                                             SARGE_STATE(unit), *(short *)(unit + 0x28), GetTickCount());
    /* The flamethrower fires along Sarge's facing +0x2c6 (FUN_00447c30, weapon 3), measured from his middle, while
     * the flame leaves the nozzle off to his side: the stream ran beside the pointer (user 2026-11-17: "not
     * accurately track the cursor ... on trees"). For the shot only, +0x2c6 = nozzle -> pointer. */
    int flame = unit == SARGE && !SARGE_VEHICLE(unit) && SARGE_WEAPON_TYPE(unit) == WEAPON_FLAME_THROWER && S.have_cursor && g_cfg.controls_modern;
    short body_facing = *(short *)(unit + 0x2c6);
    if (flame) {
        short m[2]; GAME_MUZZLE_POSITION(unit, m);
        int vx = S.cursor_x + CAMERA_X - m[0], vy = S.cursor_y + CAMERA_Y - m[1];
        if (vx || vy) *(short *)(unit + 0x2c6) = (short)facing_toward(vx, vy);
    }
    controls_fire_shot_original(unit);
    if (flame) *(short *)(unit + 0x2c6) = body_facing;
    if (tracking && army >= 0 && army < 4) throw_track_start(unit, army, slot);
}

/* ---------------------------------------------------------------- keeping the pointer in the window
 * While playing in a window the pointer is kept inside the window (ClipCursor), so the game keeps
 * focus and keeps tracking it. Released when a menu is up, and when the window is deactivated
 * (Alt-Tab etc.; ddraw.c calls controls_release_pointer from the window hook). */
static int g_pointer_confined;

void controls_release_pointer(void)
{
    if (g_pointer_confined) { ClipCursor(NULL); mlog("controls: pointer released"); }
    g_pointer_confined = 0;
}

/* While the user drags the window's border or title bar (ddraw.c window hook), the pointer must be
 * free to leave the window. */
static int g_sizing;

void controls_sizing(int on)
{
    g_sizing = on;
    if (on) controls_release_pointer();
}

/* The pointer is kept inside the WHOLE window (frame, title and menu bar included), so the window
 * can still be resized and the menu bar reached while playing. */
static void confine_pointer(int want)
{
    HWND h = g_dd ? g_dd->hwnd : NULL;
    if (!want || g_sizing || !h || GetForegroundWindow() != h || !g_cfg.controls_confine) { controls_release_pointer(); return; }
    RECT r;
    if (!GetWindowRect(h, &r)) return;
    if (!ClipCursor(&r)) return;
    if (!g_pointer_confined) mlog("controls: pointer kept inside the window (%ld,%ld - %ld,%ld)", r.left, r.top, r.right, r.bottom);
    g_pointer_confined = 1;
}

/* ---------------------------------------------------------------- running
 * Wrap of FUN_00414ad0, which moves a game object one step: distance = its speed (short at +0xac)
 * x the elapsed time, along the facing tables. Army Men 1 has no run (only "slow"), so while the
 * player runs (double-click right and hold) Sarge's moving object gets [controls] run_speed x its
 * speed for the step. Sarge's object is recognised by sitting on his position (+0x10/+0x12 = his
 * world x/y), which also takes along anything moving attached to him.
 * __fastcall: ecx = object, edx = flag, one stack argument (ret 4). */
#define OBJ_X(o)     (*(short *)((o) + 0x10))
#define OBJ_Y(o)     (*(short *)((o) + 0x12))
#define OBJ_SPEED(o) (*(short *)((o) + 0xac))
typedef int (__fastcall *TileFn)(int tile);
#define UNTRAFFICABLE  ((TileFn)0x42a230)               /* UntrafficableUnit: tile blocks walking */
#define MAP_TILES_W    (*(unsigned short *)0x4e368c)   /* map width in tiles */

MoveFn controls_move_original;

/* The game's walking test for Sarge (binary isSargeUntrafficable): the tile, plus the
 * movement values of every asset / structure / obstacle whose shape covers the point (fences,
 * barricades, walls are objects, not tiles), plus vehicles and wrecks; 15 or more blocks.
 * GetObjectsIntersectPoint = 0x42fca0 (__fastcall ecx SPOINT*, returns ITEM* list via +0x60). */
typedef BYTE *(__fastcall *ObjectsAtFn)(short *point);
#define OBJECTS_AT ((ObjectsAtFn)0x42fca0)
/* strict = the ground tile by the NPC standard (UntrafficableUnit 0x42a230, also tiles next to walls /
 * buildings): pathfind.c keeps that margin. Not strict = exactly the exe's isSargeUntrafficable
 * (FUN_00446ad0, player Sarge: npc byte +0x26c == 0): tile trigger bit TILE_UNTRAFFICABLE counts 16, then
 * the objects' movement values. 2026-11-19 (user: "sprinting is still walking over these fences", Desert low
 * stone walls WALL #796/#797): the run check used the strict tile test, so Sarge standing legally next to
 * a wall counted as "already blocked" and controls_move skipped the whole check -> ran through the wall. */
#define TILE_TRIGGERS       (*(BYTE **)0x4e3560)               /* the_map.triggers, one byte per tile */
#define TILE_UNTRAFFICABLE_BIT 0x40
#define MOVEMENT_BLOCKS     15                                 /* movement >= 15 = Sarge can't stand there */
static int blocked_test(int x, int y, int strict)
{
    const int *sb = (const int *)0x4e3014;                     /* the_map.sargeboundrect: Sarge never leaves it */
    if (sb[2] > sb[0] && sb[3] > sb[1] && (x < sb[0] || y < sb[1] || x >= sb[2] || y >= sb[3])) return 1;
    int tile = (y >> 4) * MAP_TILES_W + (x >> 4), movement = 0;
    if (strict) { if (UNTRAFFICABLE(tile)) return 1; }
    else if (TILE_TRIGGERS && (TILE_TRIGGERS[tile] & TILE_UNTRAFFICABLE_BIT)) movement = 16;
    short p[2] = { (short)x, (short)y };
    for (BYTE *it = OBJECTS_AT(p); it; it = *(BYTE **)(it + 0x60)) {
        int t = (int)((*(DWORD *)it >> 22) & 0xf);            /* ITEM_TYPE + 1 */
        if (t == 7 || t == 8 || t == 9) movement += *(signed char *)(it + 0x6f);
        else if (t == 3 || t == 10) movement += 16;
    }
    return movement >= MOVEMENT_BLOCKS;
}
static int sarge_blocked_at(int x, int y) { return blocked_test(x, y, 0); }       /* the game's own test */
int controls_blocked_at(int x, int y) { return blocked_test(x, y, 1); }          /* pathfind.c (with margin) */
int controls_sarge_blocked_at(int x, int y) { return sarge_blocked_at(x, y); }    /* roofs.c: building floor */

/* A step of (dx, dy) from (x, y), plus a few px for Sarge's size, crosses a blocked point. */
static int path_blocked(int x, int y, int dx, int dy)
{
    int len = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (!len) return 0;
    for (int i = 1; i <= len + 6; i++)
        if (sarge_blocked_at(x + dx * i / len, y + dy * i / len)) return 1;
    return 0;
}

/* A squad trooper's sprint step (squad.c decides who): same speed-up as Sarge's; the longer step is checked
 * pixel by pixel with the NPC rule (controls_blocked_at, the tile test the game's squash uses) and cut back to
 * the normal walking step the game planned if anything blocks it, or if he already stands on a blocked spot. */
int controls_sarge_running(void) { return S.running && g_cfg.controls_modern; }
static void squad_move(BYTE *obj, int flag, int arg, float mul)
{
    short speed = OBJ_SPEED(obj), fast = (short)(speed * mul);
    short ox = OBJ_X(obj), oy = OBJ_Y(obj);
    OBJ_SPEED(obj) = fast;
    controls_move_original(obj, flag, arg);
    if (OBJ_SPEED(obj) == fast) OBJ_SPEED(obj) = speed;     /* unless the game set a new speed meanwhile */
    int dx = OBJ_X(obj) - ox, dy = OBJ_Y(obj) - oy;
    if (!dx && !dy) return;
    int len = abs(dx) > abs(dy) ? abs(dx) : abs(dy), blocked = controls_blocked_at(ox, oy);
    for (int i = 1; i <= len && !blocked; i++) blocked = controls_blocked_at(ox + dx * i / len, oy + dy * i / len);
    if (blocked) {
        int wx = (int)(dx / mul), wy = (int)(dy / mul);
        OBJ_X(obj) = (short)(ox + wx);
        OBJ_Y(obj) = (short)(oy + wy);
        *(short *)(obj + 0xae) = (short)wx;
        *(short *)(obj + 0xb0) = (short)wy;
    }
    if (g_cfg.log >= 2) {
        static DWORD last;
        if (GetTickCount() - last > 500) {
            last = GetTickCount();
            mlog("squad: trooper anim %p sprint step %d,%d (speed %d -> %d)%s", obj, dx, dy, speed, fast, blocked ? " blocked -> walking step" : "");
        }
    }
}

void __fastcall controls_move(BYTE *obj, int flag, int arg)
{
    BYTE *u = SARGE;
    float mul = obj && OBJ_SPEED(obj) > 0 ? squad_sprint_speed(obj) : 0;
    if (mul > 1.0f) { squad_move(obj, flag, arg, mul); return; }
    if (S.running && u && obj && g_cfg.log >= 3) {
        static int n;
        if (n++ < 40) mlog("controls: move obj %p at %d,%d speed %d flag %d | sarge %d,%d +0x268 %p",
                           obj, OBJ_X(obj), OBJ_Y(obj), OBJ_SPEED(obj), flag, SARGE_X(u), SARGE_Y(u), *(void **)(u + 0x268));
    }
    if (!S.running || !g_cfg.controls_modern || !u || SARGE_VEHICLE(u) || !obj ||
        OBJ_X(obj) != SARGE_X(u) || OBJ_Y(obj) != SARGE_Y(u) || !OBJ_SPEED(obj)) {
        controls_move_original(obj, flag, arg);
        return;
    }
    short speed = OBJ_SPEED(obj), fast = (short)(speed * g_cfg.controls_run_speed);
    OBJ_SPEED(obj) = fast;
    short ox = OBJ_X(obj), oy = OBJ_Y(obj);
    controls_move_original(obj, flag, arg);
    if (OBJ_SPEED(obj) == fast) OBJ_SPEED(obj) = speed;     /* unless the game set a new speed meanwhile */
    /* AnimMove only sets this step (x/y_velocity); the caller checks the DESTINATION and moves. A
     * longer running step can land beyond a thin fence or a wall edge, so every tile along the step
     * is checked with the game's own walk test (UntrafficableUnit); if one is blocked the step is
     * cut back to a normal walking step and the game's own collision handles it. */
    /* In the exe this routine MOVES the object itself (position += step; step also in +0xae/+0xb0).
     * Check the whole path from the old position (a few px further for Sarge's size); if anything
     * blocks it, put him back and take only the normal walking step - the one the game checked -
     * or no step at all if even that is blocked. */
    int dx = OBJ_X(obj) - ox, dy = OBJ_Y(obj) - oy;
    if (sarge_blocked_at(ox, oy)) return;      /* inside a building etc.: the game moves freely there */
    if (path_blocked(ox, oy, dx, dy)) {
        /* slide along the wall: the step's x or y part alone, else a walking step, else none */
        int wx = (int)(dx / g_cfg.controls_run_speed), wy = (int)(dy / g_cfg.controls_run_speed);
        int tx = 0, ty = 0;
        if (!path_blocked(ox, oy, dx, 0) && abs(dx) >= abs(dy) / 2) tx = dx;
        else if (!path_blocked(ox, oy, 0, dy) && abs(dy) >= abs(dx) / 2) ty = dy;
        else if (!path_blocked(ox, oy, wx, wy)) { tx = wx; ty = wy; }
        OBJ_X(obj) = (short)(ox + tx);
        OBJ_Y(obj) = (short)(oy + ty);
        *(short *)(obj + 0xae) = (short)tx;
        *(short *)(obj + 0xb0) = (short)ty;
        if (g_cfg.log >= 2) mlog("controls: run step blocked at %d,%d -> walking step", ox + dx, oy + dy);
    }
    if (g_cfg.log >= 2) {
        static DWORD last;
        if (GetTickCount() - last > 500) { last = GetTickCount(); mlog("controls: running (object %p speed %d -> %d)", obj, speed, fast); }
    }
}

/* Modern controls switched on or off in the Gameplay menu. */
void controls_reset(void)
{
    if (g_keys_seen) controls_game_keys(g_cfg.controls_modern);   /* first snapshot at gameplay start (after custom.cfg) */
    controls_release_pointer();
    memset(&S, 0, sizeof S);
}

/* ---------------------------------------------------------------- smooth camera
 * The original camera (FUN_0043f430 -> FUN_0040e4e0 -> FUN_0040e330 / FUN_0040e100) aims at a point
 * a FIXED distance ahead of Sarge in his facing direction while he aims, and moves toward it by up
 * to 32 pixels per frame with no easing. With the mouse turning Sarge, every turn or shot threw the
 * view around. In modern mode the camera instead:
 *   - follows Sarge himself, plus a small lead toward the pointer ([controls] camera_lead x the
 *     pointer's offset from the middle of the view - from its SCREEN position, so the camera
 *     moving never moves the lead),
 *   - eases toward that point ([controls] camera_smooth_ms = time to cover ~63 % of the way).
 * Wrap of FUN_0040dcd0, which turns the camera centre DAT_004e3050 into the view position; we set
 * the centre just before it runs. Vehicles use it too (locked to the vehicle). Menus and classic
 * controls keep the original camera. */
#define CAM_CENTRE_X   (*(short *)0x4e3050)
#define CAM_CENTRE_Y   (*(short *)0x4e3052)
#define VIEW_W         (*(short *)0x4e2fb8)    /* battlefield view size */
#define VIEW_H         (*(short *)0x4e2fba)
#define BOUND_L        (*(int *)0x4e2ff4)      /* the_map.boundrect: the visible part of the map (14 tiles */
#define BOUND_T        (*(int *)0x4e2ff8)      /* in from every edge); the original camera never shows */
#define MAP_W          (*(int *)0x4e2ffc)      /* anything outside it (clamp in FUN_0040e100) */
#define MAP_H          (*(int *)0x4e3000)
#define CAM_SNAP_DIST  600                     /* farther than this (new map, respawn): jump, no easing */
#define HUD_COVER_H    94                      /* rows of the battlefield the HUD panel covers */
#define LOB_CAMERA_LEAD 0.5                    /* camera lead toward the pointer with a grenade / mortar: at least */
#define LOB_CAMERA_LEAD_MAX 4.0                /* ... at most */
#define LOB_EDGE_MARGIN 64                     /* px: the longest throw this far inside the view edge / HUD top */
#define AIR_TARGET_REACH 700                   /* px: picking an air support target, the window edge reaches this far */
#define LOB_CAMERA_REACH_MIN 200               /* world px the window edge reaches at least */
#define LOB_START_AHEAD 25                     /* the shell starts ~25 px ahead of Sarge (lob_start) */

VoidFnPtr controls_camera_original;

/* Sub-pixel camera offset of the frame being drawn, and of the frame on screen (taken at Flip). */
static double g_subpx[2];
static int g_subpx_on;
static volatile float g_shown_subpx[2];
static volatile int g_shown_subpx_on;
static int g_trace_u[2];
static double g_trace_c[2];
void controls_trace_info(char *buf, int n)
{
    char tmp[160];
    int sx = (int)(g_shown_subpx[0] * 1000), sy = (int)(g_shown_subpx[1] * 1000);
    wsprintfA(tmp, "unit %d,%d cam %d,%d sub %d,%d on %d", g_trace_u[0], g_trace_u[1], (int)(g_trace_c[0] * 10),
              (int)(g_trace_c[1] * 10), sx, sy, g_shown_subpx_on);
    lstrcpynA(buf, tmp, n);
}
void controls_frame_shown(void)
{
    g_shown_subpx[0] = (float)g_subpx[0]; g_shown_subpx[1] = (float)g_subpx[1];
    g_shown_subpx_on = g_subpx_on && g_cfg.smooth_scroll;
}
int controls_shown_subpx(float *x, float *y)
{
    *x = g_shown_subpx[0]; *y = g_shown_subpx[1];
    return g_shown_subpx_on;
}

static double g_cam_want_y;                    /* modern camera: wanted centre y (unclamped) */
static unsigned g_cam_want_frame;              /* ... and the frame it was set in */

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Camera centre kept so the view stays inside [lo, hi) (boundrect), as the original does; a view
 * wider than that range is centred on it (fog.c covers what lies outside). */
static double clamp_centre(double c, int lo, int hi, int view)
{
    if (hi - lo <= view) return (lo + hi) / 2.0;
    return clampd(c, lo + view / 2.0, hi - view / 2.0 - 1);
}

static void controls_camera_body(void)
{
    /* The camera is locked to Sarge (as the original is): only the LEAD toward the pointer is eased.
     * Easing Sarge's position itself made him jitter against the background - he moves in uneven
     * whole-pixel steps per frame, which a smoothly gliding camera shows instead of hiding. The easing
     * runs on the high-resolution timer (GetTickCount only ticks every ~15.6 ms). */
    static double lx, ly;                      /* eased lead, in pixels from Sarge */
    static int have;
    static double fx, fy, fvx, fvy;            /* the camera itself: position and velocity (spring) */
    static int fhave;
    static LARGE_INTEGER last, freq;
    BYTE *u = SARGE;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    if (!g_cfg.controls_modern || !u || MENU_CURSOR_SHOWN || !layout_view_is_gameplay() ||
        VIEW_W <= 0 || VIEW_H <= 0) {
        have = 0; fhave = 0;
        g_subpx_on = 0;
        controls_camera_original();
        return;
    }
    double tlx = 0, tly = 0;                   /* wanted lead */
    int lob_snap = 0;                          /* the mortar camera just ended: jump to the normal lead */
    if (S.have_cursor && g_cfg.controls_camera_lead > 0) {
        tlx = (S.cursor_x - VIEW_W / 2.0) * g_cfg.controls_camera_lead * peek_scale();   /* modern.c edge peek */
        tly = (S.cursor_y - VIEW_H / 2.0) * g_cfg.controls_camera_lead * peek_scale();
        /* grenade / mortar on foot (user: "when trying to throw grenades below Sarge, the UI gets in the
         * way"): a long lead toward the pointer, and aiming down the camera also slides by up to the HUD
         * panel's height, so the whole throw range shows above the panel (below Sarge only ~H/2 - 94 px
         * were visible, a grenade reaches ~260 px) */
        /* the mortar only while it is set up (Sarge kneeling, not walking): once he moves the camera snaps back
         * to the normal lead (user 2026-11-17); the grenade always */
        int lob = !SARGE_VEHICLE(u) && weapon_is_hold_to_aim(u) && modern_on(MOD_THROW_POINTER) &&
                  (SARGE_WEAPON_TYPE(u) != 5 || (SARGE_POSE(u) == POSE_KNEEL && !S.going && !(INPUT & (INPUT_FORWARD | INPUT_BACK))));
        static int was_lob;
        if (was_lob && !lob) lob_snap = 1;
        was_lob = lob;
        if (COMMAND_CURSOR == 1) {
            /* picking an air strike / paratroop target: the view follows the pointer far (to the window edge =
             * AIR_TARGET_REACH px away) so any spot nearby can be picked - the screen was locked (user) */
            double lx2 = AIR_TARGET_REACH / (VIEW_W / 2.0) - 1, ly2 = AIR_TARGET_REACH / (VIEW_H / 2.0) - 1;
            tlx = (S.cursor_x - VIEW_W / 2.0) * clampd(lx2, 0.5, LOB_CAMERA_LEAD_MAX);
            tly = (S.cursor_y - VIEW_H / 2.0) * clampd(ly2, 0.5, LOB_CAMERA_LEAD_MAX);
        } else if (lob) {
            /* the lead is sized so the window edge (the HUD panel top when aiming down) is the weapon's
             * longest throw (user: "need mortar updated to go further": a fixed 0.5 reached ~260 px, the
             * mortar flies ~500) */
            double reach = LOB_CAMERA_REACH_MIN, landed = 0;
            BYTE *w = *(BYTE **)(u + 0x20);
            if (w) { throw_distance_for(w, 1e9, &landed, NULL, NULL); if (landed + LOB_START_AHEAD > reach) reach = landed + LOB_START_AHEAD; }
            /* the longest throw sits LOB_EDGE_MARGIN inside the view edge / the HUD panel top, so its blast circle
             * stays clear of the panel (user 2026-11-17: the mortar's range end was right on the panel) */
            double half_w = VIEW_W / 2.0 - LOB_EDGE_MARGIN, half_h = VIEW_H / 2.0 - LOB_EDGE_MARGIN;
            double below = VIEW_H / 2.0 - HUD_COVER_H - LOB_EDGE_MARGIN;
            if (half_w < 32) half_w = 32;
            if (half_h < 32) half_h = 32;
            if (below < 32) below = 32;
            double lead_x = clampd(reach / half_w - 1, LOB_CAMERA_LEAD, LOB_CAMERA_LEAD_MAX);
            double lead_up = clampd(reach / half_h - 1, LOB_CAMERA_LEAD, LOB_CAMERA_LEAD_MAX);
            double lead_down = clampd((reach - HUD_COVER_H) / below - 1, LOB_CAMERA_LEAD, LOB_CAMERA_LEAD_MAX);
            double ox = S.cursor_x - VIEW_W / 2.0, oy = S.cursor_y - VIEW_H / 2.0;
            tlx = ox * lead_x * peek_scale();
            tly = oy * (oy > 0 ? lead_down : lead_up) * peek_scale();
            if (oy > 0) tly += HUD_COVER_H * clampd(oy / below, 0, 1);   /* past the HUD panel */
        }
    }
    double dt = have ? (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart : 0;
    if (!have || lob_snap) {
        lx = tlx; ly = tly; have = 1;
    } else if (dt > 0) {
        double k = g_cfg.controls_camera_smooth_ms > 0 ? 1.0 - exp(-dt / g_cfg.controls_camera_smooth_ms) : 1.0;
        lx += (tlx - lx) * k;
        ly += (tly - ly) * k;
    }
    last = now;
    BYTE *veh = (BYTE *)(uintptr_t)SARGE_VEHICLE(u);     /* in a vehicle: follow the vehicle the same way */
    double ux = veh ? VEH_X(veh) : SARGE_X(u), uy = veh ? VEH_Y(veh) : SARGE_Y(u);
    /* Smooth estimate of where the unit really is between its whole-pixel steps (alpha-beta filter on
     * the high-resolution clock), kept within 0.9 px of the stored position. The camera follows the
     * estimate; the leftover fraction of the integer camera is shown as a sub-pixel shift of the
     * battlefield when the picture is scaled up (g_subpx -> present.c / gpu.c), so the ground glides
     * evenly instead of moving 1, 1, 1, 2 pixels per frame. */
    static double ex, ey, vx, vy;
    static int est;
    if (!est || fabs(ux - ex) > 24 || fabs(uy - ey) > 24 || dt > 250) { ex = ux; ey = uy; vx = vy = 0; est = 1; }
    else if (dt > 0.5) {
        ex += vx * dt; ey += vy * dt;
        double rx = ux - ex, ry = uy - ey;
        ex += 0.3 * rx; ey += 0.3 * ry;
        vx += 0.03 * rx / dt; vy += 0.03 * ry / dt;
        ex = clampd(ex, ux - 0.9, ux + 0.9); ey = clampd(ey, uy - 0.9, uy + 0.9);
    }
    /* The camera is NOT locked to the unit: it follows the smooth estimate plus the pointer lead with a
     * critically damped spring ([controls] camera_smooth_ms = its time constant): it trails a little
     * when the unit starts moving, eases in when it stops, and never overshoots. The camera position
     * stays fractional, so the battlefield still scrolls by sub-pixels (g_subpx). */
    double gx = ex + tlx, gy = ey + tly;
    double T = g_cfg.controls_camera_smooth_ms;
    if (!fhave || fabs(gx - fx) + fabs(gy - fy) > CAM_SNAP_DIST || dt > 250 || T <= 0) {
        fx = gx; fy = gy; fvx = fvy = 0; fhave = 1;
    } else if (dt > 0) {
        double w = 2.0 / T, x = w * dt, e = 1.0 / (1.0 + x + 0.48 * x * x + 0.235 * x * x * x);
        double ch = fx - gx, tmp = (fvx + w * ch) * dt;
        fvx = (fvx - w * tmp) * e; fx = gx + (ch + tmp) * e;
        ch = fy - gy; tmp = (fvy + w * ch) * dt;
        fvy = (fvy - w * tmp) * e; fy = gy + (ch + tmp) * e;
    }
    (void)lx; (void)ly;
    double cx = fx, cy = fy;
    if (g_cfg.log >= 4) mlog("frame %.1f ms: unit %d,%d centre %.1f,%.1f", (double)now.QuadPart * 1000.0 / (double)freq.QuadPart, veh ? VEH_X(veh) : SARGE_X(u), veh ? VEH_Y(veh) : SARGE_Y(u), cx, cy);
    /* the battlefield runs under the HUD panel on wide screens (layout.c): keep Sarge in the middle
     * of the part above the panel, not of the whole picture */
    if (g_cfg.width > LAYOUT_DESIGN_W) cy += HUD_COVER_H / 2;
    double tx = cx, ty = cy;                   /* for the log line below */
    /* keep the view on the map, as the original does; the eased point itself stays unclamped, so
     * how far the camera WANTS to go past the bottom is known (controls_field_shift_target) */
    g_cam_want_y = cy;
    g_cam_want_frame = view_frame_count();
    double ccx = clamp_centre(cx, BOUND_L, MAP_W, VIEW_W), ccy = clamp_centre(cy, BOUND_T, MAP_H, VIEW_H);
    CAM_CENTRE_X = (short)floor(ccx + 0.5);
    CAM_CENTRE_Y = (short)floor(ccy + 0.5);
    g_subpx[0] = ccx - CAM_CENTRE_X;            /* in [-0.5, 0.5]: where the view should really be */
    g_subpx[1] = ccy - CAM_CENTRE_Y;
    g_subpx_on = 1;
    g_trace_u[0] = (int)ux; g_trace_u[1] = (int)uy; g_trace_c[0] = ccx; g_trace_c[1] = ccy;
    if (g_cfg.log >= 3) {
        static DWORD logged;
        DWORD ms = GetTickCount();
        if (ms - logged >= 250) { logged = ms; mlog("camera: sarge %d,%d lead %.0f,%.0f centre %.0f,%.0f", SARGE_X(u), SARGE_Y(u), lx, ly, tx, ty); }
    }
    controls_camera_original();
}

/* How many rows the battlefield picture should move UP so the bottom of the map is not hidden
 * under the HUD panel (layout.c layout_draw_battlefield): how far the wanted camera centre lies
 * past the lowest centre the map allows, at most HUD_COVER_H. Wanted centre = the modern camera's
 * point, or (classic controls) Sarge as the middle of the area above the panel. */
int controls_field_shift_target(void)
{
    BYTE *u = SARGE;
    if (g_cfg.width <= LAYOUT_DESIGN_W || VIEW_H <= 0 || MAP_H <= 0 || !u) return 0;
    BYTE *v = (BYTE *)(intptr_t)SARGE_VEHICLE(u);          /* driving: the vehicle (the original camera follows it) */
    double want = !v && g_cam_want_frame == view_frame_count() ? g_cam_want_y : (v ? VEH_Y(v) : SARGE_Y(u)) + HUD_COVER_H / 2.0;
    double over = want - (MAP_H - VIEW_H / 2.0 - 1);
    return (int)(clampd(over, 0, HUD_COVER_H) + 0.5);
}

/* ---------------------------------------------------------------- sprint indicator
 * While Sarge runs (double-click right and hold), a small "SPRINT" label is drawn in the HUD's
 * Sarge-health box (design position SPRINT_X, SPRINT_Y, right of the health pips), into the
 * finished game picture (surface.c calls this at the end of a frame, with the weapon markers). */
#define SPRINT_X 396
#define SPRINT_Y 400

static const BYTE FONT5x7[][7] = {       /* S P R I N T, 5 bits per row, top row first */
    { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e },  /* S */
    { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 },  /* P */
    { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 },  /* R */
    { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e },  /* I */
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },  /* N */
    { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },  /* T */
};

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

void controls_draw_sprint(Surf *back)
{
    if (!back || !S.running || !g_cfg.controls_modern || !layout_view_is_gameplay()) return;
    int x = SPRINT_X, y = SPRINT_Y;
    layout_place(&x, &y);
    if (x < 0 || y < 0 || x + 6 * 6 > back->w || y + 7 > back->h) return;
    BYTE ink = (BYTE)nearest_colour(255, 220, 40);
    for (int ch = 0; ch < 6; ch++)
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (FONT5x7[ch][r] & (0x10 >> c)) back->bits[(size_t)(y + r) * back->pitch + x + ch * 6 + c] = ink;
}

/* Is the pointer over the interface during gameplay (the HUD panel or the spare-weapon column)?
 * There the Windows arrow is shown (mouse.c), since the game shows no cursor in gameplay. */
int controls_pointer_on_ui(void)
{
    int x, y;
    if (!layout_view_is_gameplay() || MENU_CURSOR_SHOWN || !pointer_in_window(&x, &y)) return 0;
    return ui_at_steady(x, y);
}

/* The pointer in game-screen coordinates (anywhere in the window) - for tips.c. */
int controls_pointer(int *x, int *y) { return pointer_in_window(x, y); }

/* modern.c: the game's reticle is shown now (not the walk ring / enter arrow of the one-button mouse) */
int controls_reticle_visible(void) { return !g_hide_reticle; }

/* modern.c: the current weapon (Sarge's, or his vehicle's) is reloading and the wait is long enough to show
 * (user 2026-11-17: "a reload indicator to the cursor for weapons that have a reload timer that is
 * substantive"). The game counts WEAPON +8 fire_count up by the frame time every frame (WeaponFireUpdate,
 * Sarge / vehicle update) and fires when it reaches +0x12 nFireRate (ms; WeaponFireTime). */
#define RELOAD_MIN_MS 400
int controls_reload(double *frac)
{
    BYTE *u = SARGE;
    if (!g_cfg.controls_modern || !u) return 0;
    BYTE *sh = SARGE_VEHICLE(u) ? (BYTE *)(intptr_t)SARGE_VEHICLE(u) : u;
    BYTE *w = *(BYTE **)(sh + 0x20);
    if (!w || IsBadReadPtr(w, 0x14)) return 0;
    int rate = *(short *)(w + 0x12), count = *(int *)(w + 8);
    if (g_cfg.log >= 3) { static DWORD t; if (GetTickCount() - t > 250) { t = GetTickCount(); mlog("controls: reload weapon %d rate %d count %d", *(int *)w, rate, count); } }
    if (rate < RELOAD_MIN_MS || count < 0 || count >= rate) return 0;
    *frac = (double)count / rate;
    return 1;
}

void __cdecl controls_dispatch(void) { PROF_BEGIN(t); controls_dispatch_body(); PROF_END(t, PR_CONTROLS); }   /* timed (trace) */

void __cdecl controls_camera(void) { PROF_BEGIN(t); controls_camera_body(); PROF_END(t, PR_CAMERA); }   /* timed (trace) */

/* modern.c: unit vector (screen x, y) of a game facing 0..255 */
void controls_facing_vector(int f, double *dx, double *dy) { if (!g_units_ready) prepare_facings(); f &= FACINGS - 1; *dx = g_units_ready ? g_unit_dx[f] : 0; *dy = g_units_ready ? g_unit_dy[f] : 0; }

/* bindings.c: a binding was changed on the BINDS page - the game's key table / dispatcher follow at once (in a
 * mission; otherwise at the next return to gameplay, as always) */
void controls_bindings_changed(void)
{
    if (g_cfg.controls_modern && S.active) controls_game_keys(1);
    bindings_reset_state();
}
