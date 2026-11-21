/* ============================================================================================
 * orders.c - squad orders ATTACK / DEFEND pick their target; command targets anywhere on the map (2026-11-21)
 *
 * User: "defend/attack should move into those states so that I can select targets" and "take off the limit for the
 * distance of choosing defend/attack/paratroopers/airstrike. should be able to do this anywhere on the map".
 *
 * The original (disassembly 0x460dd0 / 0x460e00 / 0x440be0 / 0x440c70 / 0x441000):
 *   Attack key (F2, key table 0x4d0380 row 1) 0x460dd0: no selected unit (0x69195c) -> nothing. Slow held (input word
 *   0x6a547c bit 0x100) -> FUN_00440be0(ecx 1) -> FUN_00440a30: FUN_0041b1a0(ecx unit, edx 2, the unit's OWN position,
 *   0, 0) = attack at once where the unit stands. Otherwise FUN_00440be0(ecx 0): a voice line (FUN_00420fc0), then
 *   command cursor off -> on (FUN_00447400, FUN_0043f6c0 ecx 1: 0x4b8a88 = 1, the camera target 0x4e3050 on the unit)
 *   and mode 0x696ec8 = 2 (FUN_0043f610 ecx 2; also picks the cursor sprite 0x6968d0 = table 0x696a08[mode]).
 *   Defend key (F3) 0x460e00 -> FUN_00440c70: the same with mode 3 (immediate: FUN_00440a70, order 1).
 *   The command click FUN_00441000 (__fastcall ecx 1 = confirm; from the left-click handler 0x440800 when the click is
 *   above the HUD, from the Fire key 0x460ca0): mode 2 -> FUN_00440ab0 = FUN_0041b1a0(ecx unit, edx 2 ATTACK, point =
 *   game cursor 0x4fe3e8 / ec + camera, 0, shift); mode 3 -> FUN_00440b20 (edx 1 = go there and hold = DEFEND); then
 *   the cursor goes off (FUN_0043f630). Mode 5 / 6 = air strike / paratroopers (FUN_00462410; -2 = refused because
 *   something stands within 480 px of the spot (FUN_00462220) - a game rule, kept).
 *   So the game DOES have a picking state for ATTACK / DEFEND; there is no distance limit in it either - the only limit
 *   is that the spot must be under the cursor, i.e. on screen. In the original a minimap click while picking moved
 *   the view there (0x440800 -> FUN_0040e2d0), which the mod's camera (controls.c) overrides.
 *
 * What this file does:
 *   - orders_attack_key / orders_defend_key: REWRITE of 0x460dd0 / 0x460e00 (hook_jmp, key table, HUD squad box,
 *     radial menu all call these addresses). Kept: Slow held = the original "at once" order; the game's own
 *     FUN_00440be0 / FUN_00440c70 switch the picking state on (voice line, cursor sprite). Changed: no unit selected
 *     (or the selected one not on the map) -> the player's deployed unit nearest to Sarge is selected (FUN_00441b10),
 *     none -> log "no unit" and nothing; pressed again while picking in that mode = cancel (the original only set the
 *     mode again).
 *   - orders_set_pick: the wanted state (HUD box release, controls.c: click the row again = cancel).
 *   - orders_command_click: wrap of FUN_00441000. A call while the pointer is over the INTERFACE (HUD, minimap, mod
 *     boxes) that did not come from here is refused: the game's own click handlers saw a HUD / minimap press as a
 *     battlefield click and launched / ordered at the last battlefield spot (the air box "fired at once", 2026-11-17).
 *     The game's right-click call (ecx 0, 0x4411c0) is skipped with modern controls: in modes 2 / 3 it gave the order
 *     without leaving the mode; a right click cancels instead (controls.c command branch).
 *     Logs every order given: mode, target, distance from Sarge, the unit's order / goal afterwards.
 *   - orders_minimap_click: a left click on the minimap while picking = that spot (anywhere on the map): the game
 *     cursor is set to spot - camera for the call, so FUN_00441000 adds the camera back (FUN_00404c80) and gives the
 *     game's own order / launch there.
 *   - orders_message: Esc while picking = cancel (the key does not reach the game, so no pause menu then).
 *   - orders_pick_label: the text the pointer shows while picking (tips.c draws it).
 * The camera while picking (no lead, edge scroll; the picked point never moves by itself) is in controls.c
 * (controls_camera_body, pick_camera); Sarge's stance while / after picking (no stuck radio pose 0xe) in controls.c
 * command_pose.
 * ============================================================================================ */
#include "common.h"
#include <math.h>

#define COMMAND_CURSOR     (*(volatile char *)0x4b8a88)   /* 0 off, 1 picking a spot, 2 another state */
#define COMMAND_MODE       (*(volatile int *)0x696ec8)    /* 2 attack, 3 defend, 5 air strike, 6 paratroopers */
#define CURSOR_X           (*(int *)0x4fe3e8)             /* the game cursor (game-screen px; spot = cursor + camera) */
#define CURSOR_Y           (*(int *)0x4fe3ec)
#define CAMERA_X           (*(short *)0x4e2de8)
#define CAMERA_Y           (*(short *)0x4e2dea)
#define INPUT              (*(volatile DWORD *)0x6a547c)  /* the player's input word */
#define INPUT_SLOW         0x100                          /* the Slow key: attack / defend NOW (original) */
#define SELECTED_UNIT      (*(BYTE **)0x69195c)
#define SARGE              (*(BYTE **)0x697520)
#define IT_X(it)           (*(short *)((it) + 0x04))
#define IT_Y(it)           (*(short *)((it) + 0x06))
typedef BYTE *(__fastcall *ArmyFn)(int army);
#define ARMY_RECORD        ((ArmyFn)0x41a160)             /* ArmyGetLegion: +4 / +8 first / end of the UNIT array */
#define LEGION_STATUS(a)   (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))   /* army in play */
#define ARMY_PLAYER        0
#define UNIT_STRIDE        0x2c4
#define UNIT_MODE(un)      (*(int *)((un) + 0x70))        /* 3..5 = on the map */
#define UNIT_ORDER(un)     (*(int *)((un) + 0x74))        /* FUN_0041b990: the unit's command */
#define UNIT_GOAL_X(un)    (*(short *)((un) + 0x94))      /* ... and its goal (snapped to a legal spot) */
#define UNIT_GOAL_Y(un)    (*(short *)((un) + 0x96))
#define UNIT_MEN(un)       (*(int *)((un) + 0x68))        /* troops now */
#define UNIT_NO_SELECT(un) (*(BYTE *)((un) + 0x88))       /* the Next Unit chain skips units with this set */
typedef void (__fastcall *SelectUnitFn)(BYTE *unit);
#define SELECT_UNIT        ((SelectUnitFn)0x441b10)
typedef void (__fastcall *OrderKeyFn)(int now);
#define ATTACK_COMMAND     ((OrderKeyFn)0x440be0)         /* ecx 0 = picking state on (mode 2), 1 = at once */
#define DEFEND_COMMAND     ((OrderKeyFn)0x440c70)         /* ecx 0 = picking state on (mode 3), 1 = at once */
typedef void (__fastcall *CmdFn)(int confirm);
#define COMMAND_TOGGLE     ((CmdFn)0x43f6c0)              /* command cursor on / off */
#define MODE_ATTACK        2
#define MODE_DEFEND        3
#define MODE_STRIKE        5
#define MODE_PARA          6

CommandClickFn orders_command_click_original;
static int g_own_call;                 /* the call to FUN_00441000 comes from this file */

static const char *mode_name(int m)
{
    return m == MODE_ATTACK ? "ATTACK" : m == MODE_DEFEND ? "DEFEND" : m == MODE_STRIKE ? "AIR STRIKE" : m == MODE_PARA ? "PARATROOPERS" : "command";
}

int orders_picking(void) { return COMMAND_CURSOR == 1 && layout_view_is_gameplay(); }

/* the player's unit for an order: the selected one when it is on the map, else the deployed one nearest Sarge */
static BYTE *order_unit(void)
{
    BYTE *leg = LEGION_STATUS(ARMY_PLAYER) ? ARMY_RECORD(ARMY_PLAYER) : NULL;
    if (!leg) return NULL;
    BYTE *first = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8), *sel = SELECTED_UNIT, *best = NULL;
    if (sel && sel >= first && sel < end && (sel - first) % UNIT_STRIDE == 0 && UNIT_MODE(sel) >= 3 && UNIT_MODE(sel) <= 5)
        return sel;
    BYTE *s = SARGE;
    double bd = 1e18;
    for (BYTE *un = first; un && un < end && un < first + 64 * UNIT_STRIDE; un += UNIT_STRIDE) {
        if (UNIT_MODE(un) < 3 || UNIT_MODE(un) > 5 || UNIT_NO_SELECT(un) || UNIT_MEN(un) <= 0) continue;
        double dx = s ? IT_X(un) - IT_X(s) : 0, dy = s ? IT_Y(un) - IT_Y(s) : 0, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = un; }
    }
    if (best) {
        SELECT_UNIT(best);
        mlog("orders: no unit selected - the nearest squad %p (%d men, %.0f px from Sarge) selected", best, UNIT_MEN(best), sqrt(bd));
    }
    return best;
}

/* Picking state for mode (MODE_ATTACK / MODE_DEFEND) on or off - the game's own switch, voice line included */
static void pick_on(int mode)
{
    if (mode == MODE_ATTACK) ATTACK_COMMAND(0); else DEFEND_COMMAND(0);
}

void orders_set_pick(int mode, int on, const char *why)
{
    if (!on) {
        if (COMMAND_CURSOR == 1) COMMAND_TOGGLE(0);
        mlog("orders: %s cancelled (%s; command cursor %d)", mode_name(mode), why, COMMAND_CURSOR);
        return;
    }
    if (!order_unit()) { mlog("orders: %s (%s): no unit - no squad of yours on the map", mode_name(mode), why); return; }
    if (COMMAND_CURSOR == 1 && COMMAND_MODE == mode) return;            /* already */
    pick_on(mode);
    mlog("orders: %s - pick the %s (%s; command cursor %d, mode %d, unit %p)", mode_name(mode),
         mode == MODE_ATTACK ? "target" : "spot", why, COMMAND_CURSOR, COMMAND_MODE, SELECTED_UNIT);
}

/* REWRITE of the Attack / Defend key handlers 0x460dd0 / 0x460e00 (key table, HUD box, radial; no arguments) */
static void squad_key(int mode)
{
    if (!order_unit()) { mlog("orders: %s key: no unit - no squad of yours on the map", mode_name(mode)); return; }
    if (INPUT & INPUT_SLOW) {                                           /* original: Slow held = at once, where the unit is */
        if (mode == MODE_ATTACK) ATTACK_COMMAND(1); else DEFEND_COMMAND(1);
        mlog("orders: %s NOW (Slow held) - unit %p order %d", mode_name(mode), SELECTED_UNIT, UNIT_ORDER(SELECTED_UNIT));
        return;
    }
    if (COMMAND_CURSOR == 1 && COMMAND_MODE == mode) { orders_set_pick(mode, 0, "key / box again"); return; }
    orders_set_pick(mode, 1, "key");
}
void __cdecl orders_attack_key(void) { squad_key(MODE_ATTACK); }
void __cdecl orders_defend_key(void) { squad_key(MODE_DEFEND); }

/* wrap of the command click FUN_00441000 (__fastcall ecx confirm) */
void __fastcall orders_command_click(int confirm)
{
    int mode = COMMAND_MODE, on = COMMAND_CURSOR;
    if (!g_own_call && on == 1 && controls_pointer_on_ui()) {
        if (g_cfg.log >= 1) mlog("orders: the game's command click (mode %d) with the pointer on the interface - refused", mode);
        return;
    }
    /* ecx 0 comes only from the right-click handler 0x4411c0 (call 0x441270): in modes 2 / 3 the original then gives the
     * order WITHOUT leaving the mode (and 5 / 6 reach the launch FUN_00462410) - with modern controls a right click
     * cancels (controls.c), so the game's own call is not made */
    if (!g_own_call && on == 1 && !confirm && g_cfg.controls_modern) {
        if (g_cfg.log >= 2) mlog("orders: the game's right-click command call (mode %d) skipped - right click = cancel", mode);
        return;
    }
    int wx = CURSOR_X + CAMERA_X, wy = CURSOR_Y + CAMERA_Y;
    orders_command_click_original(confirm);
    if (on != 1 || !confirm) return;
    BYTE *s = SARGE, *un = SELECTED_UNIT;
    int dist = s ? (int)sqrt((double)(wx - IT_X(s)) * (wx - IT_X(s)) + (double)(wy - IT_Y(s)) * (wy - IT_Y(s))) : -1;
    if ((mode == MODE_ATTACK || mode == MODE_DEFEND) && un)
        mlog("orders: %s at %d,%d (%d px from Sarge) -> unit %p order %d goal %d,%d; command cursor %d", mode_name(mode), wx, wy,
             dist, un, UNIT_ORDER(un), UNIT_GOAL_X(un), UNIT_GOAL_Y(un), COMMAND_CURSOR);
    else
        mlog("orders: %s at %d,%d (%d px from Sarge); command cursor %d mode %d", mode_name(mode), wx, wy, dist, COMMAND_CURSOR, COMMAND_MODE);
}

/* the command click at a world spot (minimap): the game cursor is spot - camera for the call */
static void command_at(int wx, int wy)
{
    int cx = CURSOR_X, cy = CURSOR_Y;
    CURSOR_X = wx - CAMERA_X; CURSOR_Y = wy - CAMERA_Y;
    g_own_call = 1;
    orders_command_click(1);
    g_own_call = 0;
    if (COMMAND_CURSOR != 1) { CURSOR_X = cx; CURSOR_Y = cy; }   /* still picking (refused): the cursor stays on the spot */
}

/* controls.c: a left press on the interface while picking (x, y game-screen px). 1 = it was the minimap: done. */
int orders_minimap_click(int x, int y)
{
    int wx, wy;
    if (COMMAND_CURSOR != 1 || !minimap_world_at(x, y, &wx, &wy)) return 0;
    mlog("orders: minimap click at %d,%d -> world %d,%d (%s)", x, y, wx, wy, mode_name(COMMAND_MODE));
    command_at(wx, wy);
    return 1;
}

/* window hook (ddraw.c): Esc while picking = cancel */
int orders_message(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg != WM_KEYDOWN || wp != VK_ESCAPE || !orders_picking()) return 0;
    if (lp & 0x40000000) return 1;                                      /* auto-repeat of the same press */
    orders_set_pick(COMMAND_MODE, 0, "Esc");
    return 1;
}

/* tips.c: the pointer's label while picking, NULL otherwise */
const char *orders_pick_label(void)
{
    if (!orders_picking()) return NULL;
    switch (COMMAND_MODE) {
    case MODE_ATTACK: return "ATTACK: pick target - right click: cancel";
    case MODE_DEFEND: return "DEFEND: pick spot - right click: cancel";
    case MODE_STRIKE: return "AIR STRIKE: pick target - right click: cancel";
    case MODE_PARA:   return "PARATROOPERS: pick spot - right click: cancel";
    }
    return NULL;
}
