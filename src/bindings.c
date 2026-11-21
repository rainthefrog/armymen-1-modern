/* ============================================================================================
 * bindings.c - the modern controls' input bindings: which key / mouse button does what (2026-11-20)
 *
 * No game function is rewritten here; this is the mod's own input layer for [controls] mode=modern
 * (classic mode never reads it). controls.c asks it every gameplay frame:
 *   bindings_poll()      reads the bound keys / buttons (GetAsyncKeyState) and turns them into actions
 *   bindings_pressed(a)  one-shot action given this frame (dive, weapon 3, stance down ...)
 *   bindings_held(a)     continuous action held now (walk up, sprint, peek, radial ...)
 *   bindings_hide(keys)  the bound keys taken out of the keyboard state DirectInput hands the game
 *
 * Every action has TWO slots: a keyboard binding and a mouse binding (Middle / Mouse4 / Mouse5 only - the
 * left / right button and the wheel keep their fixed meaning: left = walk / use / shoot what is under the
 * pointer, right = force fire, both = cancel, wheel = weapons; their stance gestures on Sarge are fixed too).
 * A binding = input + gesture:
 *   tap     one-shot on the press (on the release when the same input also has a hold binding, 250 ms after
 *           the release when it also has a double binding - so a double-tap is never also a tap)
 *   hold    one-shot once the input is held HOLD_MS (300 ms; drop 400 ms)
 *   double  one-shot on the second press within DOUBLE_MS (250 ms)
 * Continuous actions are active while held (at once, or after HOLD_MS when the input also has a tap /
 * double binding: middle tap = next weapon, middle hold = radial).
 *
 * ini [bindings]: <action>=<key>[:tap|:hold|:double] and <action>_mouse=<button>[:...], "none" = unbound,
 * e.g. "prone=C:hold", "dive_mouse=Mouse4". Missing entries = defaults (the mod's old [keys] values are
 * taken over as defaults for roll / peek / stats / radial / ping / restart).
 *
 * Conflicts (one rule): assigning an input that another action already has with the SAME gesture (a
 * continuous action counts as "hold") SWAPS: the other action gets this action's previous binding of that
 * slot (or nothing, if it had none); its row flashes on the BINDS page.
 *
 * The BINDS page of the F10 panel (Shift+F10, panel.c) is drawn and driven from here (bindings_panel_*).
 * Hints: controls_last_device() / controls_hint_input() (common.h) give "double-click Sarge" / "hold C" ...
 * ============================================================================================ */
#include "common.h"
#include <stdio.h>
#include <string.h>

enum { G_TAP, G_HOLD, G_DOUBLE };
enum { AK_PRESS, AK_HELD };                  /* one-shot / continuous */
#define HOLD_MS    300
#define DOUBLE_MS  250
#define NO_MOUSE   0                         /* the action takes no mouse binding */

typedef struct { WORD vk; BYTE g; } Bind;
typedef struct {
    const char *key, *label;
    BYTE kind, mouse_ok;
    WORD min_hold;                           /* hold gesture / continuous: at least this long (ms) */
    Bind def_key, def_mouse;
    const char *fixed_mouse;                 /* the fixed mouse way (not rebindable), shown and used for hints */
    int feature;                             /* MOD_* that has to be on, -1 = always */
    const char *old_key;                     /* [keys] entry of the old KEYS page (default taken over) */
} Action;

#define K(c)    { (WORD)(c), G_TAP }
#define KH(c)   { (WORD)(c), G_HOLD }
#define KD(c)   { (WORD)(c), G_DOUBLE }
#define NONE    { 0, G_TAP }
static const Action ACT[ACT_COUNT] = {
    [ACT_UP]        = { "up",          "Walk up",              AK_HELD,  0, 0,   K('W'), NONE, NULL, -1, NULL },
    [ACT_LEFT]      = { "left",        "Walk left",            AK_HELD,  0, 0,   K('A'), NONE, NULL, -1, NULL },
    [ACT_DOWN]      = { "down",        "Walk down",            AK_HELD,  0, 0,   K('S'), NONE, NULL, -1, NULL },
    [ACT_RIGHT]     = { "right",       "Walk right",           AK_HELD,  0, 0,   K('D'), NONE, NULL, -1, NULL },
    [ACT_SPRINT]    = { "sprint",      "Sprint (+left: force fire)", AK_HELD, 0, 0, K(VK_SHIFT), NONE, "double-click ground / vehicle", -1, NULL },
    [ACT_SLOW]      = { "slow",        "Walk slowly",          AK_HELD,  0, 0,   K(VK_CONTROL), NONE, NULL, -1, NULL },
    [ACT_KNEEL]     = { "stance_down", "Stance down (kneel)",  AK_PRESS, 0, HOLD_MS, K('C'), NONE, "click Sarge", -1, NULL },
    [ACT_PRONE]     = { "prone",       "Go prone",             AK_PRESS, 0, HOLD_MS, KH('C'), NONE, "double-click Sarge", -1, NULL },
    [ACT_STAND]     = { "stand",       "Stand up",             AK_PRESS, 0, HOLD_MS, KD('C'), NONE, "click prone Sarge", -1, NULL },
    [ACT_DIVE]      = { "dive",        "Dive",                 AK_PRESS, 1, HOLD_MS, K(VK_SPACE), K(VK_XBUTTON1), NULL, -1, NULL },
    [ACT_ROLL]      = { "roll",        "Dodge roll",           AK_PRESS, 1, HOLD_MS, K('Z'), NONE, NULL, MOD_ROLL, "dodge_roll" },
    [ACT_WEAPON1]   = { "weapon1",     "Weapon box 1",         AK_PRESS, 1, HOLD_MS, K('1'), NONE, NULL, -1, NULL },
    [ACT_WEAPON2]   = { "weapon2",     "Weapon box 2",         AK_PRESS, 1, HOLD_MS, K('2'), NONE, NULL, -1, NULL },
    [ACT_WEAPON3]   = { "weapon3",     "Weapon box 3",         AK_PRESS, 1, HOLD_MS, K('3'), NONE, NULL, -1, NULL },
    [ACT_WEAPON4]   = { "weapon4",     "Weapon box 4",         AK_PRESS, 1, HOLD_MS, K('4'), NONE, NULL, -1, NULL },
    [ACT_WEAPON5]   = { "weapon5",     "Weapon box 5",         AK_PRESS, 1, HOLD_MS, K('5'), NONE, NULL, -1, NULL },
    [ACT_WEAPON6]   = { "weapon6",     "Weapon box 6",         AK_PRESS, 1, HOLD_MS, K('6'), NONE, NULL, -1, NULL },
    [ACT_NEXT_WEAPON] = { "next_weapon", "Next weapon (+wheel)", AK_PRESS, 1, HOLD_MS, NONE, K(VK_MBUTTON), NULL, -1, NULL },
    [ACT_PREV_WEAPON] = { "prev_weapon", "Previous weapon (+wheel)", AK_PRESS, 1, HOLD_MS, NONE, K(VK_XBUTTON2), NULL, -1, NULL },
    [ACT_QUICK_SWAP] = { "quick_swap", "Last weapon",          AK_PRESS, 1, HOLD_MS, K('Q'), NONE, NULL, -1, NULL },
    [ACT_DROP]      = { "drop",        "Drop item",            AK_PRESS, 1, 400, KH('X'), NONE, "right-click its HUD box", -1, NULL },
    [ACT_SPECIAL]   = { "special",     "Special item",         AK_PRESS, 1, HOLD_MS, K('G'), NONE, NULL, -1, NULL },
    [ACT_ENTER]     = { "vehicle",     "Enter / leave vehicle", AK_PRESS, 0, HOLD_MS, K('F'), NONE, "click the vehicle", -1, NULL },
    [ACT_MAP]       = { "map",         "Map",                  AK_PRESS, 1, HOLD_MS, K('M'), NONE, NULL, -1, NULL },
    [ACT_OBJECTIVE] = { "objective",   "Objective",            AK_PRESS, 1, HOLD_MS, K('J'), NONE, NULL, -1, NULL },
    [ACT_RADIAL]    = { "radial",      "Radial menu (hold)",   AK_HELD,  1, 0,   K('B'), KH(VK_MBUTTON), NULL, MOD_RADIAL, "radial" },
    [ACT_PING]      = { "ping",        "Ping",                 AK_PRESS, 1, HOLD_MS, K('T'), NONE, NULL, MOD_PING, "ping" },
    [ACT_STATS]     = { "stats",       "Mission stats (hold)", AK_HELD,  1, 0,   K('I'), NONE, NULL, MOD_STATS, "stats" },
    [ACT_PEEK]      = { "peek",        "Edge peek (hold)",     AK_HELD,  1, 0,   K('E'), NONE, NULL, MOD_PEEK, "edge_peek" },
    [ACT_RESTART]   = { "restart",     "Restart (hold 1 s)",   AK_HELD,  0, 0,   K(VK_F12), NONE, NULL, MOD_RESTART, "restart" },
};
#undef K
#undef KH
#undef KD
#undef NONE

/* fixed inputs, listed on the BINDS page (not rebindable: the one-button mouse is built on them) */
static const char *const FIXED_INPUTS[][2] = {
    { "Walk / use / shoot (context)", "Left" },
    { "Force fire",                   "Right" },
    { "Cancel firing",                "Left+Right" },
    { "Sprint to a spot / vehicle",   "Dbl-click it" },
    { "Weapons",                      "Wheel" },
    { "Pick a stance",                "Drag on Sarge" },
    { "Stance radial",                "Radial on Sarge" },
};
#define NFIXED ((int)(sizeof FIXED_INPUTS / sizeof FIXED_INPUTS[0]))

static Bind g_bind[ACT_COUNT][2];            /* [action][0 keyboard, 1 mouse] */
static int g_loaded;

/* ---------------------------------------------------------------- key names */

static const struct { WORD vk; const char *name; } VKNAME[] = {
    { VK_SPACE, "Space" }, { VK_SHIFT, "Shift" }, { VK_CONTROL, "Ctrl" }, { VK_MENU, "Alt" }, { VK_TAB, "Tab" },
    { VK_RETURN, "Enter" }, { VK_BACK, "Backspace" }, { VK_CAPITAL, "CapsLock" }, { VK_UP, "Up" }, { VK_DOWN, "Down" },
    { VK_LEFT, "Left" }, { VK_RIGHT, "Right" }, { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" }, { VK_HOME, "Home" },
    { VK_END, "End" }, { VK_PRIOR, "PgUp" }, { VK_NEXT, "PgDn" }, { VK_MULTIPLY, "Num*" }, { VK_ADD, "Num+" },
    { VK_SUBTRACT, "Num-" }, { VK_DECIMAL, "Num." }, { VK_DIVIDE, "Num/" }, { VK_OEM_1, ";" }, { VK_OEM_PLUS, "=" },
    { VK_OEM_COMMA, "," }, { VK_OEM_MINUS, "-" }, { VK_OEM_PERIOD, "." }, { VK_OEM_2, "/" }, { VK_OEM_3, "`" },
    { VK_OEM_4, "[" }, { VK_OEM_5, "\\" }, { VK_OEM_6, "]" }, { VK_OEM_7, "'" }, { VK_OEM_102, "<" },
    { VK_MBUTTON, "Middle" }, { VK_XBUTTON1, "Mouse4" }, { VK_XBUTTON2, "Mouse5" },
};
static const char *const GNAME[3] = { "tap", "hold", "double" };

static int is_mouse_vk(int vk) { return vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2 || vk == VK_LBUTTON || vk == VK_RBUTTON; }

static void vk_name(int vk, char *s, int n)
{
    if (!vk) { lstrcpynA(s, "-", n); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { s[0] = (char)vk; s[1] = 0; return; }
    if (vk >= VK_F1 && vk <= VK_F24) { _snprintf(s, n, "F%d", vk - VK_F1 + 1); s[n - 1] = 0; return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { _snprintf(s, n, "Num%d", vk - VK_NUMPAD0); s[n - 1] = 0; return; }
    for (size_t i = 0; i < sizeof VKNAME / sizeof *VKNAME; i++) if (VKNAME[i].vk == vk) { lstrcpynA(s, VKNAME[i].name, n); return; }
    _snprintf(s, n, "vk%d", vk); s[n - 1] = 0;
}
static int vk_parse(const char *s)
{
    if (!s[0]) return 0;
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    if (!s[1] && s[0] >= 'a' && s[0] <= 'z') return s[0] - 'a' + 'A';
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '0' && s[1] <= '9') { int f = atoi(s + 1); if (f >= 1 && f <= 24) return VK_F1 + f - 1; }
    if (!_strnicmp(s, "Num", 3) && s[3] >= '0' && s[3] <= '9' && !s[4]) return VK_NUMPAD0 + s[3] - '0';
    if (!_strnicmp(s, "vk", 2) && s[2] >= '0' && s[2] <= '9') { int v = atoi(s + 2); return v > 0 && v < 256 ? v : 0; }
    for (size_t i = 0; i < sizeof VKNAME / sizeof *VKNAME; i++) if (!lstrcmpiA(VKNAME[i].name, s)) return VKNAME[i].vk;
    return 0;
}

/* DirectInput scan codes of a virtual key (Shift / Ctrl / Alt: both sides) - what the game's keyboard state uses */
int bindings_vk_diks(int vk, BYTE out[2])
{
    if (!vk || is_mouse_vk(vk)) return 0;
    if (vk == VK_SHIFT) { out[0] = 0x2a; out[1] = 0x36; return 2; }
    if (vk == VK_CONTROL) { out[0] = 0x1d; out[1] = 0x9d; return 2; }
    if (vk == VK_MENU) { out[0] = 0x38; out[1] = 0xb8; return 2; }
    UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (!sc || sc > 0x7f) return 0;
    static const WORD EXT[] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
                                VK_DIVIDE, VK_RCONTROL, VK_RMENU, VK_LWIN, VK_RWIN, VK_APPS };
    for (size_t i = 0; i < sizeof EXT / sizeof *EXT; i++) if (EXT[i] == vk) { sc |= 0x80; break; }   /* DIK_UP 0xc8 ... */
    out[0] = (BYTE)sc;
    return 1;
}

/* ---------------------------------------------------------------- ini */

static void bind_text(Bind b, int kind, char *s, int n)       /* "C:hold", "Mouse4", "none" */
{
    if (!b.vk) { lstrcpynA(s, "none", n); return; }
    char k[24]; vk_name(b.vk, k, sizeof k);
    if (kind == AK_HELD || b.g == G_TAP) lstrcpynA(s, k, n);
    else { _snprintf(s, n, "%s:%s", k, GNAME[b.g]); s[n - 1] = 0; }
}
static Bind bind_parse(const char *s, Bind def, int kind)
{
    Bind b = def;
    char t[40]; lstrcpynA(t, s, sizeof t);
    char *c = strchr(t, ':');
    int g = G_TAP;
    if (c) { *c++ = 0; g = !lstrcmpiA(c, "hold") ? G_HOLD : !lstrcmpiA(c, "double") ? G_DOUBLE : G_TAP; }
    if (!lstrcmpiA(t, "none") || !t[0]) { b.vk = 0; b.g = G_TAP; return b; }
    int vk = vk_parse(t);
    if (!vk) { mlog("bindings: '%s' not understood - default kept", s); return def; }
    b.vk = (WORD)vk; b.g = kind == AK_HELD ? G_HOLD : (BYTE)g;
    return b;
}
static void slot_key(int a, int slot, char *k, int n) { _snprintf(k, n, slot ? "%s_mouse" : "%s", ACT[a].key); k[n - 1] = 0; }

static void load(void)
{
    g_loaded = 1;
    for (int a = 0; a < ACT_COUNT; a++)
        for (int s = 0; s < 2; s++) {
            Bind def = s ? ACT[a].def_mouse : ACT[a].def_key;
            if (ACT[a].kind == AK_HELD && def.vk) def.g = G_HOLD;
            if (!s && ACT[a].old_key) {                    /* the old KEYS page's [keys] value (VK code) */
                int v = GetPrivateProfileIntA("keys", ACT[a].old_key, 0, mod_ini_path());
                if (v > 0 && v < 256) def.vk = (WORD)v;
            }
            char k[40], v[40];
            slot_key(a, s, k, sizeof k);
            GetPrivateProfileStringA("bindings", k, "\x01", v, sizeof v, mod_ini_path());
            g_bind[a][s] = v[0] == '\x01' ? def : bind_parse(v, def, ACT[a].kind);
            if (s && g_bind[a][s].vk && (!ACT[a].mouse_ok || !is_mouse_vk(g_bind[a][s].vk))) g_bind[a][s].vk = 0;
            if (!s && is_mouse_vk(g_bind[a][s].vk)) g_bind[a][s].vk = 0;
        }
}
static void ensure(void) { if (!g_loaded) load(); }
static void save(int a)
{
    for (int s = 0; s < 2; s++) {
        char k[40], v[40];
        slot_key(a, s, k, sizeof k);
        bind_text(g_bind[a][s], ACT[a].kind, v, sizeof v);
        WritePrivateProfileStringA("bindings", k, v, mod_ini_path());
    }
}

/* ---------------------------------------------------------------- what is active */

static int act_on(int a) { return ACT[a].feature < 0 || modern_on(ACT[a].feature); }
static int slot_on(int a, int s)
{
    if (!g_bind[a][s].vk || !act_on(a)) return 0;
    if (s && (g_bind[a][s].vk == VK_XBUTTON1 || g_bind[a][s].vk == VK_XBUTTON2) && !modern_on(MOD_SIDE_BUTTONS)) return 0;
    return 1;
}
static int gesture_of(int a, int s) { return ACT[a].kind == AK_HELD ? G_HOLD : g_bind[a][s].g; }

/* other active bindings on the same input: which gestures */
static void users(int vk, int *tap, int *hold, int *dbl)
{
    *tap = *hold = *dbl = 0;
    for (int a = 0; a < ACT_COUNT; a++)
        for (int s = 0; s < 2; s++) {
            if (!slot_on(a, s) || g_bind[a][s].vk != vk) continue;
            int g = gesture_of(a, s);
            if (g == G_TAP) *tap = 1; else if (g == G_HOLD) *hold = 1; else *dbl = 1;
        }
}

/* Actions the GAME carries out with its own key table rows (controls.c writes the key there). "Direct" =
 * the game reads the real key itself (a plain press, no other binding on that key): exactly as before. Else
 * the key is hidden from the game and controls.c presses the row's key for one frame (bindings_pressed). */
int bindings_direct(int a)
{
    ensure();
    if (a != ACT_DIVE && a != ACT_SLOW && a != ACT_ENTER && a != ACT_SPECIAL && a != ACT_MAP && a != ACT_OBJECTIVE) return 0;
    if (!slot_on(a, 0)) return 0;
    if (ACT[a].kind == AK_PRESS && g_bind[a][0].g != G_TAP) return 0;
    int tap, hold, dbl; users(g_bind[a][0].vk, &tap, &hold, &dbl);
    return tap + hold + dbl == 1;
}

int bindings_vk(int a, int slot) { ensure(); return a >= 0 && a < ACT_COUNT && slot >= 0 && slot < 2 && slot_on(a, slot) ? g_bind[a][slot].vk : 0; }

/* keyboard DIKs of an action (the game row's key), 0 when it has no keyboard binding */
int bindings_key_diks(int a, BYTE out[2]) { ensure(); return a >= 0 && a < ACT_COUNT && g_bind[a][0].vk ? bindings_vk_diks(g_bind[a][0].vk, out) : 0; }

/* a scan code the mod uses in modern mode (any active keyboard binding): the game's own commands lose it */
int bindings_reserved_dik(int dik)
{
    ensure();
    for (int a = 0; a < ACT_COUNT; a++) {
        if (!slot_on(a, 0)) continue;
        BYTE d[2]; int n = bindings_vk_diks(g_bind[a][0].vk, d);
        for (int i = 0; i < n; i++) if (d[i] == dik) return 1;
    }
    return 0;
}

/* kb_get_state (controls.c): bound keys are not the game's (direct game rows excepted) */
void bindings_hide(BYTE *keys)
{
    ensure();
    for (int a = 0; a < ACT_COUNT; a++) {
        if (!slot_on(a, 0) || bindings_direct(a)) continue;
        BYTE d[2]; int n = bindings_vk_diks(g_bind[a][0].vk, d);
        for (int i = 0; i < n; i++) keys[d[i]] = 0;
    }
}

/* ---------------------------------------------------------------- per frame: inputs -> actions */

static struct { BYTE down, ignore, hold_done, dbl_press, pending, cont; DWORD down_at, up_at; } K[256];
static BYTE g_pressed[ACT_COUNT], g_held[ACT_COUNT];
static BYTE g_by_mouse[ACT_COUNT];            /* the last press / hold came from the mouse slot */
static WORD g_src_vk[ACT_COUNT];              /* input that gave the action */
static int g_device = 0;                      /* 0 mouse, 1 keyboard: last used for an action */

int controls_last_device(void) { return g_device; }
void bindings_note_device(int keyboard) { g_device = keyboard ? 1 : 0; }

static void fire(int vk, int gesture)
{
    for (int a = 0; a < ACT_COUNT; a++)
        for (int s = 0; s < 2; s++) {
            if (!slot_on(a, s) || g_bind[a][s].vk != vk || ACT[a].kind != AK_PRESS || g_bind[a][s].g != gesture) continue;
            if (!s && bindings_direct(a)) continue;               /* the game reads that key itself */
            g_pressed[a] = 1; g_by_mouse[a] = (BYTE)s; g_src_vk[a] = (WORD)vk;
            g_device = s ? 0 : 1;
            if (g_cfg.log >= 2) { char n[24]; vk_name(vk, n, sizeof n); mlog("bindings: %s (%s %s)", ACT[a].key, n, GNAME[gesture]); }
        }
}

/* longest hold threshold of the hold bindings on vk (drop needs 400 ms) */
static DWORD hold_ms(int vk, int also_tap)
{
    DWORD ms = 0;
    for (int a = 0; a < ACT_COUNT; a++)
        for (int s = 0; s < 2; s++)
            if (slot_on(a, s) && g_bind[a][s].vk == vk && gesture_of(a, s) == G_HOLD && ACT[a].kind == AK_PRESS && ACT[a].min_hold > ms) ms = ACT[a].min_hold;
    if (also_tap && ms < HOLD_MS) ms = HOLD_MS;
    return ms ? ms : HOLD_MS;
}

void bindings_reset_state(void)
{
    for (int i = 0; i < 256; i++) { K[i].down = GetAsyncKeyState(i) < 0; K[i].ignore = K[i].down; K[i].pending = K[i].cont = 0; }
    memset(g_pressed, 0, sizeof g_pressed); memset(g_held, 0, sizeof g_held);
    if (g_cfg.log >= 2) {                                  /* the hint texts with the current bindings (testing) */
        static const int A[4] = { ACT_KNEEL, ACT_PRONE, ACT_STAND, ACT_DIVE };
        int keep = g_device;
        for (int d = 0; d < 2; d++) {
            g_device = d;
            mlog("bindings: hints (%s): kneel '%s', prone '%s', stand '%s', dive '%s'", d ? "keyboard" : "mouse",
                 controls_hint_input(A[0]), controls_hint_input(A[1]), controls_hint_input(A[2]), controls_hint_input(A[3]));
        }
        g_device = keep;
    }
}

/* mouse_ok: a mouse press counts (it started on the battlefield); paused: nothing counts (panel open) */
void bindings_poll(DWORD now, int mouse_ok, int paused)
{
    ensure();
    memset(g_pressed, 0, sizeof g_pressed);
    memset(g_held, 0, sizeof g_held);
    BYTE seen[256] = { 0 };
    for (int a = 0; a < ACT_COUNT; a++)
        for (int s = 0; s < 2; s++) {
            if (!slot_on(a, s)) continue;
            int vk = g_bind[a][s].vk;
            if (seen[vk]) continue;
            seen[vk] = 1;
            SHORT st = GetAsyncKeyState(vk);
            int down = st < 0;
            int flash = !down && !K[vk].down && (st & 1);           /* pressed and let go between two frames (a quick tap) */
            int tap, hold, dbl; users(vk, &tap, &hold, &dbl);
            if (paused) { if (down) K[vk].ignore = 1; K[vk].pending = 0; }
            if ((down || flash) && !K[vk].down) {                       /* press */
                K[vk].down_at = now; K[vk].hold_done = 0; K[vk].dbl_press = 0; K[vk].cont = 0;
                if (!paused) K[vk].ignore = is_mouse_vk(vk) && !mouse_ok;
                if (!K[vk].ignore) {
                    g_device = is_mouse_vk(vk) ? 0 : 1;
                    if (dbl && K[vk].pending && now - K[vk].up_at <= DOUBLE_MS) { K[vk].pending = 0; K[vk].dbl_press = 1; fire(vk, G_DOUBLE); }
                    else {
                        if (K[vk].pending) { K[vk].pending = 0; fire(vk, G_TAP); }   /* an earlier tap, then this new press */
                        if (tap && !hold && !dbl) fire(vk, G_TAP);
                    }
                }
            }
            if (down && !K[vk].ignore && !K[vk].dbl_press) {           /* held */
                DWORD held = now - K[vk].down_at;
                if (hold && !K[vk].hold_done && held >= hold_ms(vk, tap || dbl)) { K[vk].hold_done = 1; fire(vk, G_HOLD); }
            }
            if (!down && (K[vk].down || flash)) {                       /* release */
                if (!K[vk].ignore && !K[vk].dbl_press && !K[vk].hold_done && !K[vk].cont && tap && (hold || dbl)) {
                    if (dbl) { K[vk].pending = 1; K[vk].up_at = now; }
                    else fire(vk, G_TAP);
                }
                K[vk].ignore = 0;
            }
            if (!down && K[vk].pending && now - K[vk].up_at > DOUBLE_MS) { K[vk].pending = 0; fire(vk, G_TAP); }
            K[vk].down = (BYTE)down;
        }
    /* continuous actions */
    for (int a = 0; a < ACT_COUNT; a++) {
        if (ACT[a].kind != AK_HELD) continue;
        for (int s = 0; s < 2; s++) {
            if (!slot_on(a, s)) continue;
            int vk = g_bind[a][s].vk;
            if (!K[vk].down || K[vk].ignore || K[vk].dbl_press) continue;
            int tap, hold, dbl; users(vk, &tap, &hold, &dbl);
            DWORD need = (tap || dbl) ? HOLD_MS : ACT[a].min_hold;
            if (now - K[vk].down_at < need) continue;
            if (!g_held[a] && !K[vk].cont && (tap || dbl)) g_device = s ? 0 : 1;
            K[vk].cont = 1;
            g_held[a] = 1; g_by_mouse[a] = (BYTE)s; g_src_vk[a] = (WORD)vk;
        }
    }
}

int bindings_pressed(int a) { return a >= 0 && a < ACT_COUNT && g_pressed[a]; }
int bindings_held(int a) { return a >= 0 && a < ACT_COUNT && g_held[a]; }
int bindings_from_mouse(int a) { return a >= 0 && a < ACT_COUNT && g_by_mouse[a]; }
int bindings_source_vk(int a) { return a >= 0 && a < ACT_COUNT ? g_src_vk[a] : 0; }

/* ---------------------------------------------------------------- hints */

static void hint_of(int a, int slot, char *s, int n)
{
    Bind b = g_bind[a][slot];
    char k[24];
    if (slot) {
        lstrcpynA(k, b.vk == VK_MBUTTON ? "middle button" : b.vk == VK_XBUTTON1 ? "mouse button 4" : "mouse button 5", sizeof k);
        const char *how = ACT[a].kind == AK_HELD || b.g == G_HOLD ? "hold" : b.g == G_DOUBLE ? "double-click" : "click";
        _snprintf(s, n, "%s %s", how, k);
    } else {
        vk_name(b.vk, k, sizeof k);
        int tap, hold, dbl; users(b.vk, &tap, &hold, &dbl);
        const char *how = ACT[a].kind == AK_HELD || b.g == G_HOLD ? "hold" : b.g == G_DOUBLE ? "double-tap" : (hold || dbl) ? "tap" : "press";
        _snprintf(s, n, "%s %s", how, k);
    }
    s[n - 1] = 0;
}

/* The text for an action on the device used last (controls_last_device), from the current bindings:
 * ACT_PRONE -> "double-click Sarge" (mouse) / "hold C" (keyboard). Falls back to the other device when the
 * action has no way on this one; "" for an unknown action. Valid until the 8th next call. */
const char *controls_hint_input(int a)
{
    static char ring[8][48]; static int next;
    char *s = ring[next++ & 7];
    s[0] = 0;
    ensure();
    if (a < 0 || a >= ACT_COUNT) return s;
    int mouse_way = slot_on(a, 1) || ACT[a].fixed_mouse, key_way = slot_on(a, 0);
    int dev = g_device;
    if (dev == 0 && !mouse_way) dev = 1;
    if (dev == 1 && !key_way) dev = 0;
    if (dev == 0) { if (slot_on(a, 1)) hint_of(a, 1, s, 48); else if (ACT[a].fixed_mouse) lstrcpynA(s, ACT[a].fixed_mouse, 48); }
    else if (key_way) hint_of(a, 0, s, 48);
    return s;
}

/* modern.c compatibility: the old [keys] list (MK_*) is now part of the bindings */
int bindings_mk_vk(int mk)
{
    static const int MAP[MK_COUNT] = { ACT_ROLL, ACT_PEEK, ACT_STATS, ACT_RADIAL, ACT_PING, ACT_RESTART };
    ensure();
    return mk >= 0 && mk < MK_COUNT ? g_bind[MAP[mk]][0].vk : 0;
}

/* ---------------------------------------------------------------- the BINDS page (F10 panel, panel.c)
 * Rows: every action (keyboard column, mouse column), then the fixed inputs (not selectable), then
 * "RESET TO DEFAULTS". Mouse: click a cell = select it, click it again (or Enter) = wait for the new input.
 * Keyboard: Up / Down / PgUp / PgDn / Home / End, Left / Right = column, Enter = rebind (or reset on the last
 * row), Delete / Backspace = clear, Space = gesture tap -> hold -> double (one-shot actions). While waiting:
 * Esc = cancel, Delete / Backspace = clear; the mouse column takes Middle / Mouse4 / Mouse5 (polled here).
 * Wheel scrolls. */
#define ROW_H    14
#define COL_KEY  168
#define COL_MOUSE 250
#define COL_W    80
#define NITEMS   (ACT_COUNT + NFIXED + 1)
#define RESET_ITEM (ACT_COUNT + NFIXED)
static int g_sel, g_col, g_top, g_capture = -1, g_rows_fit = 10;
static int g_flash = -1; static DWORD g_flash_until; static char g_note[64]; static DWORD g_note_until;
static RECT g_area;                                     /* list area on screen (last draw) */
static int g_mbtn_was[3];
static DWORD g_last_click;

static int selectable(int i) { return i < ACT_COUNT || i == RESET_ITEM; }
static void note(const char *s) { lstrcpynA(g_note, s, sizeof g_note); g_note_until = GetTickCount() + 3000; }

static void changed(void)
{
    controls_bindings_changed();                       /* controls.c: the game's key table / dispatcher again */
}

/* conflict rule: an input another action has with the same gesture -> that action gets our old binding */
static void assign(int a, int s, Bind nb)
{
    Bind old = g_bind[a][s];
    int g = ACT[a].kind == AK_HELD ? G_HOLD : nb.g;
    if (ACT[a].kind == AK_HELD) nb.g = G_HOLD;
    if (nb.vk) {
        for (int o = 0; o < ACT_COUNT; o++) {
            if (o == a || !g_bind[o][s].vk || g_bind[o][s].vk != nb.vk) continue;
            int og = ACT[o].kind == AK_HELD ? G_HOLD : g_bind[o][s].g;
            if (og != g) continue;
            Bind give = old;
            if (give.vk && ACT[o].kind == AK_HELD) give.g = G_HOLD;
            if (give.vk && ACT[o].kind == AK_PRESS && ACT[a].kind == AK_HELD) give.g = g_bind[o][s].g;
            g_bind[o][s] = give;
            save(o);
            g_flash = o; g_flash_until = GetTickCount() + 3000;
            char t[64]; _snprintf(t, sizeof t, "%s: %s", ACT[o].label, give.vk ? "swapped" : "cleared"); t[sizeof t - 1] = 0; note(t);
            mlog("bindings: %s gave its input to %s (%s)", ACT[o].key, ACT[a].key, give.vk ? "swapped" : "cleared");
        }
    }
    g_bind[a][s] = nb;
    save(a);
    char t[40]; bind_text(nb, ACT[a].kind, t, sizeof t);
    mlog("bindings: %s%s = %s", ACT[a].key, s ? "_mouse" : "", t);
    changed();
}

void bindings_reset_defaults(void)
{
    WritePrivateProfileStringA("bindings", NULL, NULL, mod_ini_path());   /* the whole section */
    static const char *const OLD[] = { "dodge_roll", "edge_peek", "stats", "radial", "ping", "restart" };
    for (size_t i = 0; i < sizeof OLD / sizeof *OLD; i++) WritePrivateProfileStringA("keys", OLD[i], NULL, mod_ini_path());
    load();
    note("All bindings back to the defaults");
    mlog("bindings: reset to the defaults");
    changed();
}

int bindings_panel_capturing(void) { return g_capture >= 0; }

static void keep_visible(void)
{
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + g_rows_fit) g_top = g_sel - g_rows_fit + 1;
    if (g_top > NITEMS - g_rows_fit) g_top = NITEMS - g_rows_fit;
    if (g_top < 0) g_top = 0;
}
static void move_sel(int d)
{
    int i = g_sel;
    for (int guard = 0; guard < NITEMS; guard++) {
        i += d > 0 ? 1 : -1;
        if (i < 0 || i >= NITEMS) return;
        if (selectable(i)) { g_sel = i; if (d > 1 || d < -1) { d += d > 0 ? -1 : 1; continue; } break; }
    }
    keep_visible();
}
static void start_capture(void)
{
    if (g_sel == RESET_ITEM) { bindings_reset_defaults(); return; }
    if (g_sel >= ACT_COUNT) return;
    if (g_col == 1 && !ACT[g_sel].mouse_ok) { note(ACT[g_sel].fixed_mouse ? "Mouse: fixed (see the text)" : "No mouse binding for this action"); return; }
    g_capture = g_sel;
    for (int i = 0; i < 3; i++) g_mbtn_was[i] = GetAsyncKeyState(i == 0 ? VK_MBUTTON : i == 1 ? VK_XBUTTON1 : VK_XBUTTON2) < 0;
    note(g_col ? "Press Middle / Mouse4 / Mouse5 (Esc cancels, Del clears)" : "Press a key (Esc cancels, Del clears)");
}
static Bind captured(int a, int s, int vk)
{
    Bind b = { (WORD)vk, g_bind[a][s].vk ? g_bind[a][s].g : (s ? ACT[a].def_mouse.g : ACT[a].def_key.g) };
    return b;
}

/* WM_KEYDOWN / WM_SYSKEYDOWN while the BINDS page is shown. 1 = used. */
int bindings_panel_key(int vk)
{
    ensure();
    if (g_capture >= 0) {
        int a = g_capture, s = g_col;
        if (vk == VK_ESCAPE) { g_capture = -1; note("Cancelled"); return 1; }
        if (vk == VK_BACK || vk == VK_DELETE) { g_capture = -1; assign(a, s, (Bind){ 0, G_TAP }); note("Cleared"); return 1; }
        if (s == 1) return 1;                                  /* the mouse column waits for a button */
        if (vk == VK_F9 || vk == VK_F10 || vk == VK_F11 || vk == VK_PAUSE || vk == VK_LWIN || vk == VK_RWIN) { note("That key is the mod's own (F9 F10 F11 Pause)"); return 1; }
        if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
        if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
        if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
        g_capture = -1;
        assign(a, s, captured(a, s, vk));
        return 1;
    }
    switch (vk) {
    case VK_UP: move_sel(-1); return 1;
    case VK_DOWN: move_sel(1); return 1;
    case VK_PRIOR: move_sel(-8); return 1;
    case VK_NEXT: move_sel(8); return 1;
    case VK_HOME: g_sel = 0; keep_visible(); return 1;
    case VK_END: g_sel = RESET_ITEM; keep_visible(); return 1;
    case VK_LEFT: g_col = 0; return 1;
    case VK_RIGHT: g_col = 1; return 1;
    case VK_RETURN: start_capture(); return 1;
    case VK_BACK: case VK_DELETE:
        if (g_sel < ACT_COUNT && (g_col == 0 || ACT[g_sel].mouse_ok)) { assign(g_sel, g_col, (Bind){ 0, G_TAP }); note("Cleared"); }
        return 1;
    case VK_SPACE:
        if (g_sel < ACT_COUNT && ACT[g_sel].kind == AK_PRESS && g_bind[g_sel][g_col].vk) {
            Bind b = g_bind[g_sel][g_col]; b.g = (BYTE)((b.g + 1) % 3);
            assign(g_sel, g_col, b);
        } else note("Only one-shot actions have tap / hold / double");
        return 1;
    }
    return 0;
}

/* mouse wheel over the open panel */
void bindings_panel_wheel(int delta) { g_top += delta > 0 ? -3 : 3; if (g_top > NITEMS - g_rows_fit) g_top = NITEMS - g_rows_fit; if (g_top < 0) g_top = 0; }

/* a left click on the page (game-screen point). 1 = used */
int bindings_panel_click(int x, int y)
{
    DWORD now = GetTickCount();
    if (now - g_last_click < 60) return 1;                    /* the same click again (two copies of the message) */
    g_last_click = now;
    POINT p = { x, y };
    if (!PtInRect(&g_area, p)) { g_capture = -1; return 1; }
    int i = g_top + (y - g_area.top) / ROW_H;
    if (i < 0 || i >= NITEMS || !selectable(i)) return 1;
    int col = x - g_area.left >= COL_MOUSE ? 1 : 0;
    int on_cell = x - g_area.left >= COL_KEY;
    if (g_capture >= 0) { g_capture = -1; note("Cancelled"); }
    int again = i == g_sel && col == g_col;
    g_sel = i; g_col = col;
    if (i == RESET_ITEM || (on_cell && again) || on_cell) start_capture();
    return 1;
}

/* each frame while the page is shown: the mouse column waits for a button */
static void poll_capture(void)
{
    if (g_capture < 0 || g_col != 1) return;
    static const int VKS[3] = { VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int i = 0; i < 3; i++) {
        int d = GetAsyncKeyState(VKS[i]) < 0;
        if (d && !g_mbtn_was[i]) { int a = g_capture; g_capture = -1; assign(a, 1, captured(a, 1, VKS[i])); g_mbtn_was[i] = d; return; }
        g_mbtn_was[i] = d;
    }
    if (GetAsyncKeyState(VK_RBUTTON) < 0) { g_capture = -1; note("Left / right are fixed (context / force fire)"); }
}

/* rows the page needs (panel.c sizes the panel; max_rows = what fits on screen) */
int bindings_panel_rows(int max_rows)
{
    g_rows_fit = max_rows - 3;                                 /* help line, column titles, note line */
    if (g_rows_fit > NITEMS) g_rows_fit = NITEMS;
    if (g_rows_fit < 4) g_rows_fit = 4;
    keep_visible();
    return g_rows_fit + 3;
}

static void outline(Surf *b, int x0, int y0, int x1, int y1, int c)
{
    for (int x = x0; x <= x1; x++) for (int k = 0; k < 2; k++) { int y = k ? y1 : y0; if (x >= 0 && y >= 0 && x < b->w && y < b->h) b->bits[(size_t)y * b->pitch + x] = (BYTE)c; }
    for (int y = y0; y <= y1; y++) for (int k = 0; k < 2; k++) { int x = k ? x1 : x0; if (x >= 0 && y >= 0 && x < b->w && y < b->h) b->bits[(size_t)y * b->pitch + x] = (BYTE)c; }
}

void bindings_panel_draw(Surf *b, int x, int y, int w, int font, int colour)
{
    ensure();
    poll_capture();
    DWORD now = GetTickCount();
    int clip[2] = { w, ROW_H }, cc[2] = { COL_W - 4, ROW_H }, mc[2] = { w - COL_MOUSE - 2, ROW_H }, lc[2] = { COL_KEY - 6, ROW_H };
    hud_draw_string_original(b, x, y, "Enter/click: change  Del: clear  Space: tap/hold/double", font, clip, colour);
    y += ROW_H;
    hud_draw_string_original(b, x, y, "Action", font, lc, colour);
    hud_draw_string_original(b, x + COL_KEY, y, "Keyboard", font, cc, colour);
    hud_draw_string_original(b, x + COL_MOUSE, y, "Mouse", font, mc, colour);
    y += ROW_H;
    SetRect(&g_area, x, y, x + w, y + g_rows_fit * ROW_H);
    for (int r = 0; r < g_rows_fit && g_top + r < NITEMS; r++) {
        int i = g_top + r, ry = y + r * ROW_H;
        char k[40], m[40], lab[48];
        if (i < ACT_COUNT) {
            int on = act_on(i);
            _snprintf(lab, sizeof lab, "%s%s", ACT[i].label, on ? "" : " (off)"); lab[sizeof lab - 1] = 0;
            bind_text(g_bind[i][0], ACT[i].kind, k, sizeof k);
            if (!g_bind[i][0].vk) lstrcpynA(k, "-", sizeof k);
            if (ACT[i].mouse_ok) { bind_text(g_bind[i][1], ACT[i].kind, m, sizeof m); if (!g_bind[i][1].vk) lstrcpynA(m, ACT[i].fixed_mouse ? ACT[i].fixed_mouse : "-", sizeof m); }
            else lstrcpynA(m, ACT[i].fixed_mouse ? ACT[i].fixed_mouse : "", sizeof m);
            if (ACT[i].fixed_mouse && !ACT[i].mouse_ok) { char t[40]; _snprintf(t, sizeof t, "%s *", m); t[sizeof t - 1] = 0; lstrcpynA(m, t, sizeof m); }
            if (g_capture == i) { if (g_col) lstrcpynA(m, "press button...", sizeof m); else lstrcpynA(k, "press key...", sizeof k); }
        } else if (i < RESET_ITEM) {
            _snprintf(lab, sizeof lab, "%s", FIXED_INPUTS[i - ACT_COUNT][0]); lab[sizeof lab - 1] = 0;
            lstrcpynA(k, "", sizeof k);
            _snprintf(m, sizeof m, "%s *", FIXED_INPUTS[i - ACT_COUNT][1]); m[sizeof m - 1] = 0;
        } else {
            lstrcpynA(lab, ">> RESET TO DEFAULTS <<", sizeof lab); k[0] = m[0] = 0;
        }
        hud_draw_string_original(b, x + 2, ry, lab, font, lc, colour);
        if (k[0]) hud_draw_string_original(b, x + COL_KEY + 2, ry, k, font, cc, colour);
        if (m[0]) hud_draw_string_original(b, x + COL_MOUSE + 2, ry, m, font, mc, colour);
        if (i == g_sel) {
            if (i == RESET_ITEM) outline(b, x, ry - 1, x + COL_KEY - 4, ry + ROW_H - 2, colour);
            else { int cx = x + (g_col ? COL_MOUSE : COL_KEY); outline(b, cx, ry - 1, g_col ? x + w - 2 : cx + COL_W - 2, ry + ROW_H - 2, colour); }
            outline(b, x, ry + ROW_H - 2, x + 3, ry + ROW_H - 2, colour);
        }
        if (i == g_flash && now < g_flash_until && (now / 250) % 2) outline(b, x, ry - 1, x + w - 2, ry + ROW_H - 2, colour);
    }
    int ny = y + g_rows_fit * ROW_H;
    char tail[96];
    if (now < g_note_until) lstrcpynA(tail, g_note, sizeof tail);
    else _snprintf(tail, sizeof tail, "* = fixed    rows %d-%d of %d    saved in armymen-mod.ini [bindings]", g_top + 1, g_top + g_rows_fit < NITEMS ? g_top + g_rows_fit : NITEMS, NITEMS);
    tail[sizeof tail - 1] = 0;
    hud_draw_string_original(b, x, ny, tail, font, clip, colour);
}

/* ---------------------------------------------------------------- the CONTROLS help page (panel.c) */
/* "C" / "C:hold" of an action's keyboard binding ("-" when unbound), for text on the panel */
void bindings_text(int a, char *s, int n)
{
    ensure();
    if (a < 0 || a >= ACT_COUNT || !g_bind[a][0].vk) { lstrcpynA(s, "-", n); return; }
    bind_text(g_bind[a][0], ACT[a].kind, s, n);
}

/* short key / button text for small hints (radial.c): "C", "hold C", "2x C", else the mouse binding, else "" */
void bindings_short(int a, char *s, int n)
{
    ensure();
    s[0] = 0;
    if (a < 0 || a >= ACT_COUNT) return;
    int slot = slot_on(a, 0) ? 0 : slot_on(a, 1) ? 1 : -1;
    if (slot < 0) return;
    Bind b = g_bind[a][slot];
    char k[24]; vk_name(b.vk, k, sizeof k);
    const char *pre = ACT[a].kind == AK_HELD ? "" : b.g == G_HOLD ? "hold " : b.g == G_DOUBLE ? "2x " : "";
    _snprintf(s, n, "%s%s", pre, k); s[n - 1] = 0;
}
