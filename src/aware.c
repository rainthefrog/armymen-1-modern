/* ============================================================================================
 * aware.c - enemy awareness of Sarge (2026-11-20, [modern] enemy_awareness, F10 row "Enemy awareness")
 *
 * One switch for four things (drawn by modern.c alerts_draw from the state kept here):
 *   1. "could see you": an enemy trooper that has Sarge (or the vehicle he drives) in range and in sight but is not
 *      targeting him -> a dim yellow eye over him;
 *   2. exposure meter in the Sarge-health box: targeting enemies count a full segment, could-see ones half;
 *   3. delayed spotting (a real rule change): an enemy must NOTICE Sarge before he may open fire on him;
 *   4. the old alerts ("!" spotted, red wedge while targeting, "?" lost) - "!" now means "noticed and targeting".
 * The old key [modern] enemy_alerts is read only when enemy_awareness is missing (ddraw.c).
 *
 * Game facts (disassembly, 2026-11-20):
 *   Trooper AI tick FUN_0043cba0 (ecx trooper). Victim +0x30 (object pointer), its UID copied to +0x2b8 every tick.
 *   Sight test FUN_0043b2e0, __fastcall ecx = WEAPON* (trooper +0x20, *WEAPON = weapon type), edx = SHOOTER, stack =
 *   target cell (target +0xc), ret 4: -1 target beyond max range (FUN_00427b50 shooter, px), -2 inside min range
 *   (FUN_00445c50 type), 1 grenade (2) / mortar (5) at once, else the cell line FUN_0042a650 (ecx from, edx to, stack
 *   int buf[]: buf[0] = n, buf[1..n] = cells, ret 4) is walked twice (shooter->target, then back); a cell between
 *   the ends blocks when cover byte [0x4e3564][cell] >= class 0x696840 = FUN_0040bd90(SHOOTER) (1 prone, 2 kneel,
 *   3 stand, 4 vehicle); blocked only if both walks are blocked -> 0, else 1. NOTE: the class is the SHOOTER's own
 *   stance (edx), not the target's - the game's own test ignores Sarge's stance. Writes global 0x696840.
 *   After the test (0x43ccc3) the tick: 0 / -1 -> +0x278 = 150 ticks pause and FUN_0043d310 (blocked target kept at
 *   +0x2ec, timer +0x2e8 = frame + 1500); -2 -> moves away; 1 -> state +0x6c = 3, +0xa8 = 1 and the fire step
 *   FUN_0043d0d0 (call at 0x43d05b, __fastcall ecx trooper, edx victim, 4 stack ints, ret 0x10: picks the firing
 *   stance, burst +0x2a8, starts the shot).
 *
 * Hooks (patches.c; both do nothing while the switch is off):
 *   0x43ccc3 call FUN_0043b2e0 -> aware_sight_tick: the original result is returned unchanged; when the victim is
 *     Sarge / his vehicle the result (plus our stance-aware line, below) is recorded as "sees Sarge now".
 *   0x43d05b call FUN_0043d0d0 -> aware_fire_step: victim Sarge / his vehicle and awareness < 1 -> no fire step;
 *     the trooper does what the tick does with no victim (state 0, FUN_0043e110 default action, FUN_00437c80 hold
 *     position). Returning 0 from the sight test instead was rejected: that sets the 150-tick pause and the 1500-frame
 *     blocked-target timer, so the delay would be ~2.5 s + re-acquisition instead of the intended fill time.
 *
 * Visibility used here ("sees"): the game's test == 1 (globals 0x696830..0x696858 saved / restored around our own
 * calls) AND our line with class min(shooter class, target class): Sarge prone / kneeling behind cover lower than
 * the shooter's eye is hidden (the game's test alone cannot do that). For grenadiers / mortars (no line in the game's
 * test) the same line decides. Troopers not targeting Sarge are tested round-robin, AW_PER_FRAME a frame, only
 * when within their max range (cheap distance first).
 *
 * Awareness per trooper (0..1, game clock): fills while he sees Sarge in AW_FILL_MS x stance (kneel 1.6, prone 2.5)
 * x (1 + distance / max range); full at once when Sarge fired within AW_SHOT_MS and the trooper sees him, or when
 * Sarge's projectile hit that trooper or anyone of his unit (bullets.c note_for_modern -> modern_note_hit; a health-drop
 * guess was dropped: other damage made far troopers aware); stays full while the trooper keeps targeting Sarge; otherwise
 * drains in AW_DRAIN_MS.
 * ============================================================================================ */
#include "common.h"

#define SARGE_OBJ        (*(BYTE **)0x697520)           /* the player's Sarge object */
#define SARGE_VEH_OBJ(s) (*(BYTE **)((s) + 0x2d0))      /* the vehicle he drives, or NULL */
#define OBJ_UID(o)       (*(DWORD *)(o))
#define OBJ_CELL(o)      (*(int *)((o) + 0xc))
#define OBJ_HEALTH(o)    (*(short *)((o) + 0x1c))
#define T_WEAPON(t)      (*(int **)((t) + 0x20))       /* WEAPON*, first int = weapon type */
#define T_VICTIM_UID(t)  (*(DWORD *)((t) + 0x2b8))
#define T_STATE(t)       (*(int *)((t) + 0x6c))
#define T_UNIT(t)        (*(BYTE **)((t) + 0x94))
#define COVER_MAP        (*(BYTE **)0x4e3564)          /* per-cell cover height */
#define ARMY_IN_PLAY(a)  (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))
#define AI_GLOBALS       0x696830                       /* scratch globals of the AI's target search / sight test */
#define AI_GLOBALS_N     11                             /* 0x696830 .. 0x696858 */

typedef int (__fastcall *SightFn)(int *weapon, BYTE *shooter, int cell);
typedef int (__fastcall *WalkFn)(int from, int to, int *buf);
typedef int (__fastcall *ObjFn)(BYTE *o);
typedef int (__fastcall *TypeFn)(int type);
typedef int (__fastcall *DistFn)(int a, int b);
typedef void (__fastcall *FireStepFn)(BYTE *t, BYTE *victim, int a, int b, int c, int d);
typedef void (__fastcall *ActionFn)(BYTE *t, int action, int facing);
typedef void (__fastcall *HoldFn)(BYTE *goal, int pos_edx, int pos);
typedef BYTE *(__fastcall *ArmyFn)(int army);
typedef int (__fastcall *AllyFn)(int a, int b);
#define GAME_SIGHT     ((SightFn)0x43b2e0)
#define GAME_WALK      ((WalkFn)0x42a650)
#define GAME_CLASS     ((ObjFn)0x40bd90)               /* stance class: 1 prone, 2 kneel, 3 stand, 4 vehicle */
#define GAME_MAXRANGE  ((ObjFn)0x427b50)               /* px */
#define GAME_CELLDIST  ((DistFn)0x42b910)              /* cells (octile-ish) */
#define GAME_FIRESTEP  ((FireStepFn)0x43d0d0)
#define GAME_ACTION    ((ActionFn)0x43e110)
#define GAME_HOLD      ((HoldFn)0x437c80)
#define GAME_ARMY      ((ArmyFn)0x41a160)
#define GAME_ALLIED    ((AllyFn)0x41a1e0)

#define AW_MAX        96
#define AW_PER_FRAME  4          /* round-robin sight tests a frame (troopers not targeting Sarge) */
#define AW_FILL_MS    800.0f
#define AW_KNEEL      1.6f
#define AW_PRONE      2.5f
#define AW_DRAIN_MS   2000.0f
#define AW_SHOT_MS    1500u
#define AW_SEEN_MS    1000u      /* a "sees" result is trusted this long */

typedef struct {
    BYTE *t; DWORD uid;
    float level;                 /* 0..1 */
    int sees, targeting, range_ok;
    DWORD seen_at, last_ms, fill_from, target_from;
    float dist_frac;             /* distance / max range at the last test */
    int logged_full, held, fired;
} Aw;
static Aw g_aw[AW_MAX];
static int g_n;
static DWORD g_shot_at, g_me, g_veh;
static BYTE *g_last_sarge;
static int g_rr;
static struct { BYTE *unit; DWORD at; } g_hit_units[8];
static int g_could, g_targ;      /* this frame */
static int g_walk[50002];        /* FUN_0042a650 writes up to 49999 cells */

static int on(void) { return g_cfg.enemy_awareness; }
static int is_sarge_uid(DWORD v) { return v && (v == g_me || (g_veh && v == g_veh)); }

static Aw *find(BYTE *t)
{
    for (int i = 0; i < AW_MAX; i++) if (g_aw[i].t == t && g_aw[i].uid == OBJ_UID(t)) return &g_aw[i];
    return NULL;
}

static int clamp_class(int c) { return c >= 1 && c <= 4 ? c : 3; }

/* one direction of the game's line test (FUN_0043b2e0's loop): cells buf[n-1] .. buf[2] */
static int walk_blocked(int from, int to, int cls)
{
    BYTE *cover = COVER_MAP;
    if (!cover) return 0;
    int n = GAME_WALK(from, to, g_walk);
    for (int i = n - 1; i > 1; i--) if (cover[g_walk[i]] >= cls) return 1;
    return 0;
}

/* Does shooter t see target (in range, game's test, our stance-aware line)? Fills *frac with distance / max range.
 * Returns the game's result in *game (or 2 when not called). */
static int sees(BYTE *t, BYTE *target, float *frac, int *game)
{
    int *w = T_WEAPON(t);
    *game = 2;
    if (!w || !target) return 0;
    int maxr = GAME_MAXRANGE(t), d = GAME_CELLDIST(OBJ_CELL(t), OBJ_CELL(target)) * 16;
    *frac = maxr > 0 ? (float)d / (float)maxr : 1.0f;
    if (d > maxr) { *game = -1; return 0; }
    DWORD keep[AI_GLOBALS_N];
    memcpy(keep, (void *)AI_GLOBALS, sizeof keep);
    int r = GAME_SIGHT(w, t, OBJ_CELL(target));
    memcpy((void *)AI_GLOBALS, keep, sizeof keep);
    *game = r;
    return r == 1;
}

/* our extra line: the lower of the two stances (a prone Sarge behind a sandbag is hidden); grenade / mortar: their
 * own class (the game skips the line for them) */
static int line_ok(BYTE *t, BYTE *target)
{
    int sc = clamp_class(GAME_CLASS(t)), tc = clamp_class(GAME_CLASS(target)), wt = T_WEAPON(t) ? *T_WEAPON(t) : 0;
    int cls = tc < sc ? tc : sc;
    if (cls == sc && wt != 2 && wt != 5) return 1;          /* identical to the line the game just tested */
    if (!walk_blocked(OBJ_CELL(t), OBJ_CELL(target), cls)) return 1;
    return !walk_blocked(OBJ_CELL(target), OBJ_CELL(t), cls);
}

static BYTE *sarge_target(void)
{
    BYTE *s = SARGE_OBJ;
    if (!s) return NULL;
    BYTE *v = SARGE_VEH_OBJ(s);
    return v ? v : s;
}

/* ---------------------------------------------------------------- hooks */

int __fastcall aware_sight_tick(int *weapon, BYTE *shooter, int cell)
{
    int r = GAME_SIGHT(weapon, shooter, cell);
    if (!on()) return r;
    BYTE *s = SARGE_OBJ, *victim = *(BYTE **)(shooter + 0x30);
    if (!s || !victim || !is_sarge_uid(OBJ_UID(victim))) return r;
    Aw *a = find(shooter);
    if (!a) return r;
    int maxr = GAME_MAXRANGE(shooter), d = GAME_CELLDIST(OBJ_CELL(shooter), cell) * 16;
    a->dist_frac = maxr > 0 ? (float)d / (float)maxr : 1.0f;
    DWORD keep = *(DWORD *)0x696840;
    a->sees = r == 1 && line_ok(shooter, victim);
    *(DWORD *)0x696840 = keep;
    a->range_ok = r != -1;
    a->seen_at = clock_game_ms();
    if (!a->targeting) { a->targeting = 1; a->target_from = a->seen_at; a->held = 0; a->fired = 0; }
    return r;
}

void __fastcall aware_fire_step(BYTE *t, BYTE *victim, int p1, int p2, int p3, int p4)
{
    Aw *a = on() && victim && is_sarge_uid(OBJ_UID(victim)) ? find(t) : NULL;
    if (a && a->level < 1.0f) {
        if (!a->held && g_cfg.log >= 2) mlog("aware: enemy %08lx holds fire at Sarge (awareness %.2f)", OBJ_UID(t), a->level);
        a->held = 1;
        /* what the tick does without a victim (0x43cc46..0x43cc69) */
        int act = *(int *)(t + 0x84), ut = T_UNIT(t) ? *(int *)(T_UNIT(t) + 0x7c) : 0;
        if (ut == 4 || ut == 6) act = 0xb; else if (ut == 5) act = 0xd; else if (ut == 7) act = 0xc;
        T_STATE(t) = 0;
        GAME_ACTION(t, act, *(short *)(t + 8));
        GAME_HOLD(t + 0xa4, *(int *)(t + 4), *(int *)(t + 4));
        return;
    }
    if (a && !a->fired) {
        a->fired = 1;
        if (g_cfg.log >= 2) mlog("aware: enemy %08lx FIRST SHOT at Sarge %lu ms after targeting him (stance %d, dist %.2f of range)",
                                 OBJ_UID(t), clock_game_ms() - a->target_from, SARGE_OBJ ? clamp_class(GAME_CLASS(SARGE_OBJ)) : 0, a->dist_frac);
    }
    GAME_FIRESTEP(t, victim, p1, p2, p3, p4);
}

/* ---------------------------------------------------------------- events */

void aware_note_shot(void) { g_shot_at = clock_game_ms(); if (!g_shot_at) g_shot_at = 1; }

static void unit_hit(BYTE *unit)
{
    if (!unit) return;
    int k = 0;
    for (int i = 0; i < 8; i++) { if (g_hit_units[i].unit == unit) { k = i; break; } if (g_hit_units[i].at < g_hit_units[k].at) k = i; }
    g_hit_units[k].unit = unit; g_hit_units[k].at = clock_game_ms();
}
void aware_note_hit(BYTE *item)
{
    if (!on() || !item) return;
    Aw *a = find(item);
    if (a) unit_hit(T_UNIT(item));
}

/* ---------------------------------------------------------------- per frame (game thread) */

static void update(Aw *a, DWORD now, int stance)
{
    BYTE *t = a->t;
    int targ = is_sarge_uid(T_VICTIM_UID(t));
    if (targ && !a->targeting) { a->target_from = now; a->held = 0; a->fired = 0; }
    a->targeting = targ;
    int fresh = a->sees && now - a->seen_at <= AW_SEEN_MS;
    DWORD dt = now - a->last_ms;
    if (dt > 200) dt = 200;
    a->last_ms = now;
    const char *why = NULL;
    for (int i = 0; i < 8; i++)
        if (g_hit_units[i].unit && g_hit_units[i].unit == T_UNIT(t) && now - g_hit_units[i].at <= 500) why = "hit";
    if (!why && fresh && g_shot_at && now - g_shot_at <= AW_SHOT_MS) why = "Sarge fired";
    float before = a->level;
    if (why) a->level = 1.0f;
    else if (fresh) {
        float need = AW_FILL_MS * (stance == 1 ? AW_PRONE : stance == 2 ? AW_KNEEL : 1.0f) * (1.0f + (a->dist_frac < 2 ? a->dist_frac : 2));
        if (before <= 0) { a->fill_from = now; if (g_cfg.log >= 2) mlog("aware: enemy %08lx starts noticing Sarge (stance %d, dist %.2f of range, needs %d ms)", OBJ_UID(t), stance, a->dist_frac, (int)need); }
        a->level += dt / need;
    } else if (!(targ && a->level >= 1.0f)) a->level -= dt / AW_DRAIN_MS;
    if (a->level > 1) a->level = 1;
    if (a->level < 0) a->level = 0;
    if (a->level >= 1 && before < 1 && !a->logged_full) {
        a->logged_full = 1;
        if (g_cfg.log >= 2) mlog("aware: enemy %08lx noticed Sarge: %s%s (fill %lu ms, stance %d, dist %.2f)", OBJ_UID(t), why ? "at once - " : "filled",
                                 why ? why : "", before > 0 && !why ? now - a->fill_from : 0, stance, a->dist_frac);
    }
    if (a->level <= 0) a->logged_full = 0;
    if (targ && a->level >= 1) g_targ++;
    else if (fresh) g_could++;
}

void aware_frame(void)
{
    BYTE *s = SARGE_OBJ;
    g_could = g_targ = 0;
    if (!on() || !s || OBJ_HEALTH(s) <= 0) { memset(g_aw, 0, sizeof g_aw); g_n = 0; return; }
    if (s != g_last_sarge || OBJ_UID(s) != g_me) { memset(g_aw, 0, sizeof g_aw); memset(g_hit_units, 0, sizeof g_hit_units); g_last_sarge = s; g_shot_at = 0; }
    g_me = OBJ_UID(s);
    g_veh = SARGE_VEH_OBJ(s) ? OBJ_UID(SARGE_VEH_OBJ(s)) : 0;
    BYTE *target = sarge_target();
    int stance = clamp_class(GAME_CLASS(target));
    DWORD now = clock_game_ms();
    static int seen[AW_MAX];
    static BYTE *list[AW_MAX];
    int nl = 0;
    memset(seen, 0, sizeof seen);
    for (int army = 1; army <= 3; army++) {
        if (!ARMY_IN_PLAY(army) || GAME_ALLIED(0, army)) continue;
        BYTE *leg = GAME_ARMY(army);
        if (!leg) continue;
        BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
        for (int g = 0; un && un < end && g < 64; un += 0x2c4, g++) {
            int mode = *(int *)(un + 0x70);
            if (mode < 3 || mode > 5) continue;
            int last = *(int *)(un + 0xdc);
            for (int i = 0; i <= last && i < 10; i++) {
                BYTE *t = ((BYTE **)(un + 0xb4))[i];
                if (!t || *(int *)(t + 0x68) != 1 || OBJ_HEALTH(t) <= 0) continue;
                Aw *a = find(t);
                if (!a) {
                    for (int j = 0; j < AW_MAX; j++) if (!g_aw[j].t) { a = &g_aw[j]; break; }
                    if (!a) continue;
                    memset(a, 0, sizeof *a); a->t = t; a->uid = OBJ_UID(t); a->last_ms = now;
                }
                seen[a - g_aw] = 1;
                if (nl < AW_MAX) list[nl++] = t;
            }
        }
    }
    for (int j = 0; j < AW_MAX; j++) if (g_aw[j].t && !seen[j]) g_aw[j].t = NULL;   /* dead / gone */
    /* round-robin sight tests for the ones not targeting Sarge (the tick hook covers the others) */
    int k = 0;
    for (int done = 0; k < nl && done < AW_PER_FRAME; k++) {
        BYTE *t = list[(g_rr + k) % nl];
        Aw *a = find(t);
        if (!a || is_sarge_uid(T_VICTIM_UID(t))) continue;
        float frac = 1; int game;
        int v = sees(t, target, &frac, &game);
        if (game != -1) done++;                             /* out of range costs nothing: not counted */
        DWORD keep = *(DWORD *)0x696840;
        a->sees = v && line_ok(t, target);
        *(DWORD *)0x696840 = keep;
        a->dist_frac = frac; a->range_ok = game != -1; a->seen_at = now;
    }
    if (nl) g_rr = (g_rr + k) % nl;
    for (int j = 0; j < AW_MAX; j++) if (g_aw[j].t) update(&g_aw[j], now, stance);
    g_n = nl;
    static DWORD log_at;
    if (g_cfg.log >= 2 && now - log_at >= 2000) {
        log_at = now;
        mlog("aware: enemies %d, could see Sarge %d, targeting (noticed) %d, exposure %d/10, Sarge stance %d", nl, g_could, g_targ, aware_exposure(NULL, NULL), stance);
    }
}

/* ---------------------------------------------------------------- for modern.c */

int aware_info(BYTE *t, float *level)
{
    Aw *a = on() ? find(t) : NULL;
    if (!a) { *level = 1.0f; return -1; }
    *level = a->level;
    DWORD now = clock_game_ms();
    return (a->sees && now - a->seen_at <= AW_SEEN_MS ? AW_SEES : 0) | (a->targeting ? AW_TARGETING : 0);
}

int aware_exposure(int *targeting, int *could)
{
    if (targeting) *targeting = g_targ;
    if (could) *could = g_could;
    int v = g_targ * 2 + g_could;
    return v > 10 ? 10 : v;
}
