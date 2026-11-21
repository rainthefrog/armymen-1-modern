/* ============================================================================================
 * patrol.c - enemy marches become patrols (2026-11-21, [gameplay] patrol_marches=1, F10 "Enemy marches patrol")
 *
 * User (Desert 1: two enemy squads march to Sarge's start point by mission design and stand there): "treat it like
 * a patrol. maybe have them patrol to target position and original position".
 *
 * Game facts (disassembly, 2026-11-21):
 *   UnitOrders FUN_0041b990 (__fastcall ecx = UNIT, edx = order, 7 stack args, ret 0x1c):
 *     arg1 SPOINT goal (x low word, y high word; 0 = none), arg2 target ITEM (or NULL), arg3..arg7 stored at
 *     +0xa0, +0x78, +0x7c, +0xa4, +0xa8 (movement / engage style). Stores +0x74 order, +0x90 target, +0x94 goal
 *     (snapped by FUN_0042ffd0), +0x98 goal cell, +0x9c = 0 "arrived" (1 at once when already on that cell),
 *     +0xec / +0xf0 = game frame. Order 1 = go to the point and hold there (the deploy FUN_0041ae60 gives order 1
 *     to its own deploy point), 2 = the UI's second order (same shape), 3 = FOLLOW the target.
 *   UNIT: +0 UID (army = bits 26..28 - 1, type = bits 22..25, 2 = unit), +4 position SPOINT, +0xc cell, +0x70 mode
 *     (3..5 deployed, 2 in a vehicle), +0xb4 troopers, +0xdc last trooper index.
 *   Callers of FUN_0041b990 (return addresses, for the log and the "who ordered it" test below):
 *     0x41afa1 deploy (FUN_0041ae60), 0x41b2f6 UI / AI orders (FUN_0041b270), 0x41c06a / 0x41c7af / 0x41ca09 the
 *     unit AI dropping a lost follow target, 0x41db50 mission script MOVE / goto (FUN_0041d940), 0x41ea8a mission
 *     script ATTACK (FUN_0041e9c0), 0x41f2a9 FUN_0041f260 (all units hold where they are).
 *
 * What the mod does (switch on):
 *   - every order an ENEMY unit (army not allied with the player) gets is seen in the wrap of FUN_0041b990;
 *   - a MARCH = order 1 with no target to a point at least PATROL_MIN_PX from the unit, given within
 *     DEPLOY_WINDOW_MS of the mission start (Wine, Desert 1: the map's units are deployed at load (0x41afa1, order 1 on
 *     their own spot) and the mission script's MOVE (0x41db50) follows in the same ms - the two squads 0881001f /
 *     08810020 to 2856,296 = Sarge's start). Reinforcements deployed later (Desert 1: at 3320,168, ~19 s in) that the
 *     script walks to their post are NOT marches (patrolling them back to the map edge would empty their posts).
 *     -> patrol: post = where the unit stood when the order came, goal = the point as the game stored it (+0x94: its
 *     legal-spot search may move it, e.g. 2856 -> 2904), the order's own arguments kept;
 *   - per frame (patrol_frame): a patrol unit within ARRIVE_PX of its current leg end (or arrived by the game's
 *     own flag +0x9c on that goal) gets the game's own move order to the other end (FUN_0041b990, order 1, the
 *     march's arguments) - post -> goal -> post ...; if the game puts a leg end more than LEG_MOVED_PX away from the
 *     asked point (no legal spot) the unit holds where it stands and the patrol ends;
 *   - the patrol ends for good when the unit notices Sarge (aware.c: a trooper of it fully aware; with
 *     enemy_awareness off: a trooper has a victim / the unit has a target) - the game's engage logic then runs as
 *     always -, when the unit is gone, or when any other order reaches it (a mission script MOVE / ATTACK, the
 *     game's own AI orders): that order is kept as it is and logged.
 *   Nothing else changes: the walk itself, the engage logic, the scripts. Times are the game clock (gpGame +0x44).
 * ============================================================================================ */
#include "common.h"
#include <math.h>

UnitOrderFn patrol_order_original;                   /* trampoline (patches.c) */

typedef int (__fastcall *AllyFn)(int a, int b);
#define GAME_ALLIED      ((AllyFn)0x41a1e0)
#define ARMY_IN_PLAY(a)  (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))
#define GAME_CLOCK       (*(DWORD *)(*(BYTE **)0x4b3f70 + 0x44))   /* game ms (the mine 6000 ms test uses it) */

#define UNIT_STRIDE      0x2c4
#define U_UID(u)         (*(DWORD *)(u))
#define U_X(u)           (*(short *)((u) + 4))
#define U_Y(u)           (*(short *)((u) + 6))
#define U_MODE(u)        (*(int *)((u) + 0x70))
#define U_ORDER(u)       (*(int *)((u) + 0x74))
#define U_TARGET(u)      (*(BYTE **)((u) + 0x90))
#define U_GOAL(u)        (*(DWORD *)((u) + 0x94))
#define U_ARRIVED(u)     (*(int *)((u) + 0x9c))
#define U_TROOPS(u)      ((BYTE **)((u) + 0xb4))
#define U_LAST(u)        (*(int *)((u) + 0xdc))
#define T_MODE(t)        (*(int *)((t) + 0x68))
#define T_HEALTH(t)      (*(short *)((t) + 0x1c))
#define T_VICTIM(t)      (*(BYTE **)((t) + 0x30))
#define UID_ARMY(v)      ((int)(((v) >> 26) & 7) - 1)
#define UID_TYPE(v)      ((int)(((v) >> 22) & 0xf))
#define TYPE_UNIT        2
#define ORDER_GOTO       1
#define PX(p)            ((short)((p) & 0xffff))
#define PY(p)            ((short)((p) >> 16))

#define CALL_DEPLOY      0x41afa1
#define CALL_SCRIPT_MOVE 0x41db50
#define CALL_SCRIPT_ATK  0x41ea8a

#define PATROL_MIN_PX    200     /* a march: the point is at least this far from the unit */
#define DEPLOY_WINDOW_MS 3000    /* ... and the order came this soon after the mission started (units placed with the map; reinforcements
                                  * that arrive later and walk to their post are not patrols) */
#define ARRIVE_PX        48      /* within this of the leg end: turn round */
#define LEG_MOVED_PX     64      /* the game put the leg end farther than this from the asked point: patrol given up */
#define MAX_PATROLS      16

typedef struct {
    BYTE *u; DWORD uid;
    DWORD post, goal;            /* SPOINTs */
    int a3, a4, a5, a6, a7;      /* the march order's own arguments */
    int leg;                     /* 1 = to the goal, 0 = back to the post */
    int turns; DWORD leg_at, log_at;
    DWORD set_goal;              /* the goal the game stored for the current leg (UNIT +0x94, after its own legal-spot search) */
} Patrol;
static Patrol g_p[MAX_PATROLS];
static int g_issuing;            /* our own order is running */
static DWORD g_last_clock;

/* a new mission (or Play Again: the game clock starts again): everything recorded belongs to the old one */
#define GAME_PTR         (*(BYTE **)0x4b3f70)
#define GAME_SCENARIO    (*(int *)(GAME_PTR + 0x70))
#define GAME_MISSION     (*(int *)(GAME_PTR + 0x74))
static int g_key = -1;
static DWORD g_mission_t0;       /* game clock when this mission was first seen */
static void sync(void)
{
    DWORD now = GAME_CLOCK;
    int key = (GAME_MISSION << 16) ^ GAME_SCENARIO;
    if (key != g_key || now + 1000 < g_last_clock) {
        if (g_cfg.log >= 1) mlog("patrol: mission %d (scenario %d) - patrols reset (game clock %lu, was %lu)", GAME_MISSION, GAME_SCENARIO, now, g_last_clock);
        memset(g_p, 0, sizeof g_p);
        g_key = key; g_mission_t0 = now;
    }
    g_last_clock = now;
}

static int dist_pt(int x0, int y0, DWORD p) { double dx = PX(p) - x0, dy = PY(p) - y0; return (int)sqrt(dx * dx + dy * dy); }

static int enemy_army(int a) { return a >= 1 && a <= 3 && ARMY_IN_PLAY(a) && !GAME_ALLIED(0, a); }

static Patrol *patrol_of(BYTE *u)
{
    for (int i = 0; i < MAX_PATROLS; i++) if (g_p[i].u == u && g_p[i].uid == U_UID(u)) return &g_p[i];
    return NULL;
}
static const char *caller_name(uintptr_t r)
{
    switch (r) {
    case CALL_DEPLOY: return "deploy";
    case 0x41b2f6: return "UI/AI order (FUN_0041b270)";
    case 0x41c06a: case 0x41c7af: case 0x41ca09: return "unit AI (target lost)";
    case CALL_SCRIPT_MOVE: return "mission script MOVE";
    case CALL_SCRIPT_ATK: return "mission script ATTACK";
    case 0x41f2a9: return "all units hold (FUN_0041f260)";
    }
    return "?";
}

static void end_patrol(Patrol *p, const char *why)
{
    mlog("patrol: unit %08lx patrol ENDS after %d turns - %s", p->uid, p->turns, why);
    p->u = NULL;
}

void __fastcall patrol_order(BYTE *u, int order, DWORD goal, BYTE *target, int a3, int a4, int a5, int a6, int a7)
{
    uintptr_t from = (uintptr_t)__builtin_return_address(0);
    Patrol *made = NULL;
    if (!g_issuing && u && !IsBadReadPtr(u, 0x100) && UID_TYPE(U_UID(u)) == TYPE_UNIT && enemy_army(UID_ARMY(U_UID(u)))) {
        sync();
        DWORD now = GAME_CLOCK;
        int d = goal ? dist_pt(U_X(u), U_Y(u), goal) : -1;
        int early = now - g_mission_t0 <= DEPLOY_WINDOW_MS;
        if (g_cfg.log >= 2 || (g_cfg.log >= 1 && from == CALL_SCRIPT_MOVE))
            mlog("patrol: unit %08lx (army %d, mode %d, at %d,%d) order %d goal %d,%d (%d px) target %08lx [%d %d %d %d %d] from %s (0x%x)%s",
                 U_UID(u), UID_ARMY(U_UID(u)), U_MODE(u), U_X(u), U_Y(u), order, PX(goal), PY(goal), d,
                 target && !IsBadReadPtr(target, 4) ? U_UID(target) : 0, a3, a4, a5, a6, a7, caller_name(from), (unsigned)from,
                 early ? " (mission start)" : "");
        Patrol *p = patrol_of(u);
        if (p && g_cfg.patrol_marches) {
            end_patrol(p, from == CALL_SCRIPT_MOVE || from == CALL_SCRIPT_ATK ? "the mission script gives it an order (kept)" : "another order (kept)");
        } else if (!p && g_cfg.patrol_marches && order == ORDER_GOTO && !target && goal && d >= PATROL_MIN_PX && early) {
            for (int i = 0; i < MAX_PATROLS; i++) if (!g_p[i].u) {
                p = &g_p[i];
                memset(p, 0, sizeof *p);
                p->u = u; p->uid = U_UID(u);
                p->post = (DWORD)(WORD)U_X(u) | (DWORD)(WORD)U_Y(u) << 16;
                p->goal = goal; p->a3 = a3; p->a4 = a4; p->a5 = a5; p->a6 = a6; p->a7 = a7;
                p->leg = 1; p->leg_at = now; p->set_goal = 0; made = p;
                mlog("patrol: unit %08lx (army %d) marches from %d,%d to %d,%d (%d px, %s) -> PATROL between the two",
                     p->uid, UID_ARMY(p->uid), PX(p->post), PY(p->post), PX(goal), PY(goal), d, caller_name(from));
                break;
            }
        }
    }
    patrol_order_original(u, order, goal, target, a3, a4, a5, a6, a7);
    if (made && U_GOAL(u)) {                     /* the goal as the game stored it (its legal-spot search may move it a little) */
        made->goal = made->set_goal = U_GOAL(u);
        if (PX(made->goal) != PX(goal) || PY(made->goal) != PY(goal)) mlog("patrol: unit %08lx: the game put the march goal at %d,%d", made->uid, PX(made->goal), PY(made->goal));
    }
}

/* the unit has noticed / engages Sarge (or anyone): the game's fight logic takes over */
static int engaged(BYTE *u)
{
    if (g_cfg.enemy_awareness) return aware_unit_noticed(u);
    if (U_TARGET(u)) return 1;
    for (int i = 0; i <= U_LAST(u) && i < 10; i++) {
        BYTE *t = U_TROOPS(u)[i];
        if (t && T_MODE(t) == 1 && T_HEALTH(t) > 0 && T_VICTIM(t)) return 1;
    }
    return 0;
}

static int alive(BYTE *u)
{
    if (U_MODE(u) < 3 || U_MODE(u) > 5) return 0;
    for (int i = 0; i <= U_LAST(u) && i < 10; i++) {
        BYTE *t = U_TROOPS(u)[i];
        if (t && T_MODE(t) == 1 && T_HEALTH(t) > 0) return 1;
    }
    return 0;
}

void patrol_frame(void)
{
    if (!GAME_PTR) return;
    sync();
    DWORD now = GAME_CLOCK;
    if (!g_cfg.patrol_marches) { memset(g_p, 0, sizeof g_p); return; }
    for (int i = 0; i < MAX_PATROLS; i++) {
        Patrol *p = &g_p[i];
        BYTE *u = p->u;
        if (!u) continue;
        if (U_UID(u) != p->uid || !alive(u)) { end_patrol(p, "unit gone"); continue; }
        if (engaged(u)) { end_patrol(p, "it noticed Sarge - the game's engage logic takes over"); continue; }
        DWORD end = p->leg ? p->goal : p->post;
        int d = dist_pt(U_X(u), U_Y(u), end);
        if (g_cfg.log >= 2 && now - p->log_at >= 1000) {
            p->log_at = now; { mlog("patrol: t %lu unit %08lx at %d,%d, leg to %s %d,%d, %d px, order %d goal %d,%d arrived %d",
                now - g_mission_t0, p->uid, U_X(u), U_Y(u), p->leg ? "goal" : "post", PX(end), PY(end), d, U_ORDER(u), PX(U_GOAL(u)), PY(U_GOAL(u)), U_ARRIVED(u)); }
        }
        if (d > ARRIVE_PX && !(U_ARRIVED(u) && ((p->set_goal && U_GOAL(u) == p->set_goal) || dist_pt(PX(end), PY(end), U_GOAL(u)) <= 16))) continue;
        p->leg = !p->leg; p->turns++;
        DWORD next = p->leg ? p->goal : p->post;
        mlog("patrol: unit %08lx at %d,%d reached its %s after %lu (game clock) - turns to the %s %d,%d (turn %d)",
             p->uid, U_X(u), U_Y(u), p->leg ? "post" : "goal", now - p->leg_at, p->leg ? "goal" : "post", PX(next), PY(next), p->turns);
        p->leg_at = now;
        g_issuing = 1;
        patrol_order_original(u, ORDER_GOTO, next, NULL, p->a3, p->a4, p->a5, p->a6, p->a7);
        g_issuing = 0;
        p->set_goal = U_GOAL(u);
        int moved = dist_pt(PX(next), PY(next), p->set_goal);
        if (moved > 16) mlog("patrol: unit %08lx: the game moved the leg end %d,%d to %d,%d (%d px, its legal-spot search)", p->uid, PX(next), PY(next), PX(p->set_goal), PY(p->set_goal), moved);
        if (moved > LEG_MOVED_PX) {                    /* no spot there: hold where it stands, as the game does at a march end */
            DWORD here = (DWORD)(WORD)U_X(u) | (DWORD)(WORD)U_Y(u) << 16;
            g_issuing = 1;
            patrol_order_original(u, ORDER_GOTO, here, NULL, p->a3, p->a4, p->a5, p->a6, p->a7);
            g_issuing = 0;
            end_patrol(p, "no legal spot at the leg end - holds where it stands");
        }
    }
}
