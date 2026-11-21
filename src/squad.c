/* ============================================================================================
 * squad.c - the squad sprints with Sarge (2026-11-20, [gameplay] squad_sprint=1, F10 "Squad sprints with Sarge")
 *
 * User: "the other character AI should be able to use sprinting. especially when the squad is following sarge
 * and he is sprinting". Army Men 1 has no run: Sarge's sprint is the mod's (controls.c controls_move scales the
 * speed of his moving ANIM in AnimMove FUN_00414ad0 by [controls] run_speed). His squad walked at the normal
 * speed and fell behind.
 *
 * No game function rewritten; this file only decides WHICH moving objects sprint. The speed-up itself is the
 * same wrap of FUN_00414ad0 (controls.c controls_move; __fastcall ecx = ANIM, edx = flag, one stack argument):
 * a trooper's walk step is made there through his avatar: trooper update FUN_0043c810 (ecx = TROOPER)
 * -> FUN_00439c50 (avatar action, third argument = TROOPER +0x26c avatar) -> FUN_00414ad0 with ecx = the
 * avatar's ANIM (+0x1c). Distance per step = ANIM +0xac speed x frame time, so a bigger speed = a longer step.
 *
 * Who sprints (squad_frame, once per gameplay frame, collects their ANIMs):
 *   - troopers of the PLAYER's deployed units (UNIT +0x70 mode 3..5) whose order is FOLLOW Sarge: UNIT +0x74 = 3
 *     (UNIT_FOLLOW, set by FUN_0041b990 from the Follow command 0x460dc0 -> 0x440b90 -> FUN_0041b1a0 edx = 3)
 *     and UNIT +0x90 = Sarge's ITEM (the target the follow order stores);
 *   - while Sarge sprints (controls_sarge_running: Shift / double-click right / its toggle, stamina left), or
 *     while the trooper is more than CATCH_UP_PX from Sarge (catch up; stops at CATCH_UP_PX), Sarge on foot.
 *   speed: [controls] run_speed x the step (as Sarge); while Sarge sprints and the trooper is more than FAR_BEHIND_PX
 *     away, FAR_BOOST x that, so a squad that started late closes up (Wine: without it the gap only stayed).
 * Enemy and allied AI unchanged; other orders (attack, defend, scripted moves) unchanged.
 *
 * Walls: a longer step must not jump a thin wall or fence, and the game squashes a soldier whose tile turns
 * untrafficable under him (0x43c840 / 0x43c9ff), so controls_move checks every pixel of the longer step with the
 * NPC tile rule (controls_blocked_at = UntrafficableUnit 0x42a230 + object movement values); if anything blocks
 * it, the step is cut back to the normal walking step the game planned.
 * ============================================================================================ */
#include "common.h"

#define SARGE              (*(BYTE **)0x697520)       /* the player's Sarge (ITEM*) */
#define SARGE_VEHICLE(u)   (*(int *)((u) + 0x2d0))
#define SARGE_HEALTH(u)    (*(short *)((u) + 0x1c))
#define ITEM_X(it)         (*(short *)((it) + 0x04))  /* world position (Sarge and troopers) */
#define ITEM_Y(it)         (*(short *)((it) + 0x06))
typedef BYTE *(__fastcall *ArmyFn)(int army);
#define ARMY_RECORD        ((ArmyFn)0x41a160)         /* ArmyGetLegion */
#define ARMY_PLAYER        0
#define LEGION_STATUS(a)   (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))   /* army in play */
#define UNIT_STRIDE        0x2c4
#define UNIT_MODE(un)      (*(int *)((un) + 0x70))    /* 3..5 = deployed */
#define UNIT_ORDER(un)     (*(int *)((un) + 0x74))    /* FUN_0041b990: the unit's command */
#define UNIT_FOLLOW        3
#define UNIT_TARGET(un)    (*(BYTE **)((un) + 0x90))  /* FUN_0041b990: target ITEM (follow: Sarge) */
#define UNIT_TROOPS(un)    ((BYTE **)((un) + 0xb4))
#define UNIT_LAST(un)      (*(int *)((un) + 0xdc))    /* last index used in UNIT_TROOPS */
#define TROOPER_MODE(t)    (*(int *)((t) + 0x68))     /* 1 = in play */
#define TROOPER_HEALTH(t)  (*(short *)((t) + 0x1c))
#define TROOPER_AVATAR(t)  (*(BYTE **)((t) + 0x26c))
#define AVATAR_ANIM(a)     (*(BYTE **)((a) + 0x1c))   /* the ANIM AnimMove moves */
#define CATCH_UP_PX        120                        /* farther from Sarge than this: sprint to catch up */
#define MAX_SPRINTERS      40
#define SARGE_STILL_MS     300                        /* Sarge not moved for this long: not sprinting */
#define FAR_BEHIND_PX      240                        /* while Sarge sprints, farther than this: a little faster than him */
#define FAR_BOOST          1.25f                      /* ... x run_speed, so the squad closes up instead of only keeping the gap */

static BYTE *g_anim[MAX_SPRINTERS];
static float g_mul[MAX_SPRINTERS];                /* speed factor for that ANIM this frame */
static int g_n;

void squad_frame(void)
{
    g_n = 0;
    BYTE *s = SARGE;
    if (!s || SARGE_VEHICLE(s) || SARGE_HEALTH(s) <= 0 || !LEGION_STATUS(ARMY_PLAYER)) return;
    BYTE *leg = ARMY_RECORD(ARMY_PLAYER);
    if (!leg) return;
    /* Sarge sprinting = the sprint input AND he really moves (running in place against a wall does not count) */
    static short last_x, last_y; static DWORD moved_at;
    DWORD now = clock_game_ms();
    if (ITEM_X(s) != last_x || ITEM_Y(s) != last_y) { last_x = ITEM_X(s); last_y = ITEM_Y(s); moved_at = now; }
    int running = controls_sarge_running() && now - moved_at < SARGE_STILL_MS;
    BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
    if (g_cfg.log >= 3) {                                  /* testing: the player's units, orders, trooper positions */
        static DWORD last;
        if (GetTickCount() - last >= 1000) {
            last = GetTickCount();
            char b[700]; int o = wsprintfA(b, "squad: t %lu Sarge %d,%d%s |", (unsigned long)clock_game_ms(), ITEM_X(s), ITEM_Y(s), running ? " RUN" : "");
            for (BYTE *v = un; v && v < end && o < 600; v += UNIT_STRIDE) {
                if (UNIT_MODE(v) < 3 || UNIT_MODE(v) > 5) continue;
                o += wsprintfA(b + o, " unit mode %d order %d%s:", UNIT_MODE(v), UNIT_ORDER(v), UNIT_TARGET(v) == s ? " ->Sarge" : "");
                for (int i = 0; i <= UNIT_LAST(v) && i < 10 && o < 640; i++) {
                    BYTE *t = UNIT_TROOPS(v)[i];
                    if (!t || TROOPER_MODE(t) != 1 || !TROOPER_AVATAR(t)) continue;
                    BYTE *a = AVATAR_ANIM(TROOPER_AVATAR(t));
                    o += wsprintfA(b + o, " %d,%d(v%d)", ITEM_X(t), ITEM_Y(t), a ? *(short *)(a + 0xac) : -1);
                }
            }
            mlog("%s", b);
        }
    }
    for (int g = 0; un && un < end && g < 64; un += UNIT_STRIDE, g++) {
        if (UNIT_MODE(un) < 3 || UNIT_MODE(un) > 5 || UNIT_ORDER(un) != UNIT_FOLLOW || UNIT_TARGET(un) != s) continue;
        for (int i = 0; i <= UNIT_LAST(un) && i < 10 && g_n < MAX_SPRINTERS; i++) {
            BYTE *t = UNIT_TROOPS(un)[i];
            if (!t || TROOPER_MODE(t) != 1 || TROOPER_HEALTH(t) <= 0 || !TROOPER_AVATAR(t)) continue;
            int dx = ITEM_X(t) - ITEM_X(s), dy = ITEM_Y(t) - ITEM_Y(s);
            if (!running && dx * dx + dy * dy <= CATCH_UP_PX * CATCH_UP_PX) continue;
            BYTE *a = AVATAR_ANIM(TROOPER_AVATAR(t));
            if (!a) continue;
            g_mul[g_n] = g_cfg.controls_run_speed * (running && dx * dx + dy * dy > FAR_BEHIND_PX * FAR_BEHIND_PX ? FAR_BOOST : 1.0f);
            g_anim[g_n++] = a;
        }
    }
    if (!g_cfg.squad_sprint) g_n = 0;                      /* switched off: nobody (the log above still runs, for comparison) */
    if (g_cfg.log >= 2) {
        static DWORD last; static int last_n = -1;
        if (g_n != last_n || GetTickCount() - last > 2000) {
            last = GetTickCount(); last_n = g_n;
            mlog("squad: %d following trooper(s) sprinting (Sarge %s)", g_n, running ? "sprinting" : "walking");
        }
    }
}

/* controls.c controls_move: the speed factor of this moving ANIM this frame (a sprinting squad trooper), 0 = none */
float squad_sprint_speed(BYTE *anim)
{
    for (int i = 0; i < g_n; i++) if (g_anim[i] == anim) return g_mul[i];
    return 0;
}

/* Testing aid ([debug] fence_route 8th value = 1, controls.c): give every deployed player unit the FOLLOW order, as the
 * Follow key (F1) does for the selected unit: selected unit 0x69195c set, then the Follow handler FUN_00440b90
 * (no arguments; FUN_0041b1a0 ecx = unit, edx = 3). Key presses in the container rig left the right mouse button
 * reading as held (Wine), so the order is given from here instead. */
#define SELECTED_UNIT      (*(BYTE **)0x69195c)
#define FOLLOW_HANDLER     ((void (*)(void))0x440b90)
void squad_debug_follow(void)
{
    BYTE *leg = LEGION_STATUS(ARMY_PLAYER) ? ARMY_RECORD(ARMY_PLAYER) : NULL;
    if (!leg) return;
    BYTE *keep = SELECTED_UNIT;
    for (BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8); un && un < end; un += UNIT_STRIDE) {
        if (UNIT_MODE(un) < 3 || UNIT_MODE(un) > 5) continue;
        SELECTED_UNIT = un;
        FOLLOW_HANDLER();
        mlog("squad: [debug] follow order to unit %p -> order %d%s", un, UNIT_ORDER(un), UNIT_TARGET(un) == SARGE ? " (Sarge)" : "");
    }
    SELECTED_UNIT = keep;
}
