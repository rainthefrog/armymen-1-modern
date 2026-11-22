/* ============================================================================================
 * allies.c - the player's soldiers (2026-11-21, user: "allies should heal over time. and allow me to heal them -
 * similar to how am2 does. this would have a health cursor when the health is selected and I can click on soldiers
 * to heal them. I also need health bars to show for them. and they probably need more health than they have")
 *
 * "The player's soldiers" = the troopers of the player's army (army 0: the squads Sarge gets, reinforcements).
 * Allied armies (ALLIED(0, a)) and enemies are unchanged. Trooper record (game): +0x1c health, +0x1e max
 * (short), +0x68 mode (1 = on the map), +0x94 his UNIT, +0x26c avatar. The UNIT keeps the SUM of its troopers'
 * health / max at +0x1c / +0x1e (FUN_0043b5f0 adds them, the script HEAL FUN_0041e7e0 takes the old value off and
 * adds the new one): every change here does the same, so the unit totals (minimap "hp", squad HUD) stay right.
 *
 * 1. More health ([gameplay] ally_health_x = 200 percent; 100 = the original).
 *    FUN_0043b480 (trooper creation; __fastcall ecx = UNIT, edx = TROOPER, stack = trooper kind; ret 4; its only
 *    caller is the unit creation 0x41beab) sets health = max = the kind's table value (0x50434c + kind * 0x40) and
 *    then calls FUN_0043b5f0 (__fastcall ecx = TROOPER, edx = UNIT, no stack args) to add him to the unit. That call
 *    (0x43b4e5) is redirected to allies_trooper_join: for a unit of army 0 the trooper's max and health are scaled
 *    first, then the original adds them to the unit. Kept: everything else of the creation, enemies, allied armies.
 *    The mission script HEAL (FUN_0041e7e0: health += pct x max / 100, capped at max) works on the scaled max, so
 *    "HEAL 100" still means full.
 *
 * 2. Regeneration ([gameplay] ally_regen = 1): like Sarge's (gameplay.c): after regen_delay_ms without losing health,
 *    max / regen_full_s per second, game clock (clock_game_ms: speed and pause applied); each hit restarts the wait.
 *    Per trooper state in a small table, cleared on a new mission.
 *
 * 3. Medkits used on soldiers ([gameplay] ally_heal = 1; modern controls, on foot). Army Men 1 has no carried medkit:
 *    the health item (OBJECTSTRUCT category 0x19, WEAPON type 0x16) heals Sarge when he walks over it - FUN_00439700
 *    (pickup test, __fastcall ecx = item, edx = Sarge; 1 = pick up) refuses it while he is at full health, so it stays
 *    on the ground; FUN_00449330 case 0x16 adds WEAPON +0x10 percent of his max (capped) and uses the item up.
 *    Kept: all of that - a hurt Sarge still heals on the spot. Added: the pickup test call 0x439855 goes through
 *    allies_pickup_test - when the game said no ONLY because Sarge is at full health (the item's 5 s re-test timer
 *    +0xcc was just restarted, i.e. the test really ran) and he carries fewer than MEDKIT_MAX, the answer becomes 1:
 *    the game's own pickup takes the item (health stays capped at max, its sounds, the item used up) and the mod
 *    counts one carried kit with that item's percentage.
 *    Using a kit (AM2: select the MedKit, click a friendly unit):
 *      - the heal key (bindings ACT_HEAL, default H) selects / deselects the medkit (only with kits carried);
 *        choosing a weapon (wheel, keys, HUD), getting into a vehicle or running out of kits deselects it;
 *      - while selected the pointer is a red cross over the battlefield; grey over a soldier at full health, with no
 *        kits, or where Sarge cannot walk; a label beside it says what a click does (modern.c, allies_heal_info);
 *        a target under the pointer still shows the reticle (a click shoots as always);
 *      - left click on one of the player's soldiers: Sarge walks the normal click route to a point HEAL_SHORT px
 *        short of him and, within HEAL_REACH, uses ONE kit on him: + the kit's percentage of HIS max (as the game
 *        heals Sarge), capped; the game's health pickup sound (FUN_00420ff0 ecx 0x1394, edx 100, as 0x4493d1);
 *      - left click on Sarge himself: one kit on Sarge (when hurt; at full health the click is the stance click);
 *      - the HUD shows the carried kits as a small medkit + "xN" right of the VEHICLE HEALTH pips (weapons.c, allies_kits).
 *    Kits carry over to the next part of the same scenario and are restored on a restart (see "Carried kits" below).
 *
 * 4. The medical tent heals them too ([gameplay] tent_heals_allies = 1; 2026-11-22, user: "healing tent should also
 *    heal allies"). See "Medical tent" below for how the game heals Sarge there (mission script only).
 * ============================================================================================ */
#include "common.h"
#include <math.h>

#define SARGE              (*(BYTE **)0x697520)
#define SARGE_X(u)         (*(short *)((u) + 0x04))
#define SARGE_Y(u)         (*(short *)((u) + 0x06))
#define SARGE_WEAPON(u)    (*(BYTE **)((u) + 0x20))
#define SARGE_VEHICLE(u)   (*(int *)((u) + 0x2d0))
#define HEALTH(it)         (*(short *)((it) + 0x1c))      /* troopers, Sarge, units (sum) */
#define MAX_HEALTH(it)     (*(short *)((it) + 0x1e))
#define UID_ARMY(it)       ((int)((*(DWORD *)(it) >> 26) & 7) - 1)
#define ARMY_PLAYER        0
typedef BYTE *(__fastcall *ArmyFn)(int army);
#define ARMY_RECORD        ((ArmyFn)0x41a160)             /* ArmyGetLegion */
#define LEGION_STATUS(a)   (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))   /* army in play */
#define UNIT_STRIDE        0x2c4
#define UNIT_MODE(un)      (*(int *)((un) + 0x70))        /* 3..5 = deployed */
#define UNIT_TROOPS(un)    ((BYTE **)((un) + 0xb4))
#define UNIT_LAST(un)      (*(int *)((un) + 0xdc))
#define TROOPER_MODE(t)    (*(int *)((t) + 0x68))         /* 1 = on the map */
#define TROOPER_UNIT(t)    (*(BYTE **)((t) + 0x94))       /* FUN_0043b5f0: trooper +0x94 = his UNIT */
#define TROOPER_AVATAR(t)  (*(BYTE **)((t) + 0x26c))
#define AVATAR_ANIM(a)     (*(BYTE **)((a) + 0x1c))
#define ANIM_RECT(an)      ((const int *)((an) + 0x1c))   /* world RECT of the drawn figure */
#define GAME               (*(BYTE **)0x4b3f70)
#define GAME_TIME(g)       (*(int *)((g) + 0x44))         /* the game clock FUN_00439700 times its re-tests with */

/* items (OBJECTSTRUCT) */
#define OBJ_CATEGORY(o)    (*(short *)((o) + 0x68))
#define OBJ_WEAPON(o)      (*(BYTE **)((o) + 0x20))
#define OBJ_RETEST_AT(o)   (*(int *)((o) + 0xcc))         /* FUN_00439700: last pickup test (5 s apart) */
#define CAT_SPECIAL_ITEM   0x19
#define TYPE_MEDKIT        0x16
#define W_TYPE(w)          (*(int *)(w))
#define W_HEAL_PCT(w)      (*(short *)((w) + 0x10))       /* FUN_00449330 case 0x16: percent of max */

typedef void (__fastcall *JoinFn)(BYTE *trooper, BYTE *unit);
#define TROOPER_ADD_TO_UNIT ((JoinFn)0x43b5f0)
typedef int (__fastcall *PickupTestFn)(BYTE *item, BYTE *sarge);
#define PICKUP_TEST        ((PickupTestFn)0x439700)
typedef int (__fastcall *SoundFn)(int id, int volume);
#define PLAY_VOICE         ((SoundFn)0x420ff0)            /* as 0x4493d1 (medkit pickup) */
#define SND_MEDKIT         0x1394

#define MEDKIT_MAX         3          /* kits carried at most */
#define KIT_PCT_FALLBACK   50         /* a kit whose record gives no percentage */
#define HEAL_REACH         32         /* within this of the soldier Sarge uses the kit */
/* 2026-11-21, user: "don't need to walk to heal. as long as within a certain range, just heal with click" - a click on a
 * hurt soldier within heal_range of Sarge heals at once; farther: nothing ("TOO FAR"), no walk */
static int heal_range(void) { static int r = -1; if (r < 0) { r = GetPrivateProfileIntA("gameplay", "heal_range", 160, mod_ini_path()); if (r < HEAL_REACH) r = HEAL_REACH; } return r; }
static int in_heal_range(BYTE *t)
{
    BYTE *u = SARGE;
    if (!u || !t) return 0;
    int dx = SARGE_X(t) - SARGE_X(u), dy = SARGE_Y(t) - SARGE_Y(u), r = heal_range();
    return dx * dx + dy * dy <= r * r;
}
#define HEAL_SHORT         20         /* the walk ends this far short of the soldier */
#define HEAL_TIMEOUT_MS    20000
#define HEAL_IDLE_MS       600        /* the walk ended this long ago out of reach: given up */
#define HIT_SLACK          4          /* px round the soldier's figure that still count as "on him" */
#define REGEN_SLOTS        256
#define MAX_PCT            1000

/* ---------------------------------------------------------------- 1. more health */

static void __fastcall allies_trooper_join_px(BYTE *trooper, BYTE *unit) {
    int pct = g_cfg.ally_health_x;
    if (trooper && unit && UID_ARMY(unit) == ARMY_PLAYER && pct > 0 && pct != 100) {
        if (pct > MAX_PCT) pct = MAX_PCT;
        int was = MAX_HEALTH(trooper), mx = was * pct / 100;
        if (mx < 1) mx = 1;
        if (mx > 30000) mx = 30000;
        MAX_HEALTH(trooper) = (short)mx;
        HEALTH(trooper) = (short)mx;
        if (g_cfg.log >= 2) mlog("allies: trooper %p of unit %p: max health %d -> %d (ally_health_x %d)", trooper, unit, was, mx, pct);
    }
    PX_GAME(TROOPER_ADD_TO_UNIT(trooper, unit));
}
void __fastcall allies_trooper_join(BYTE *trooper, BYTE *unit) { PX_BEGIN(px); allies_trooper_join_px(trooper, unit); PX_END(px, PR_ALLIES); }   /* timed: stage PR_ALLIES (exclusive, common.h) */

/* ---------------------------------------------------------------- the player's soldiers */

static int alive(BYTE *t) { return t && !IsBadReadPtr(t, 0x270) && TROOPER_MODE(t) == 1 && HEALTH(t) > 0 && MAX_HEALTH(t) > 0; }

void allies_each(SoldierFn fn)
{
    if (!LEGION_STATUS(ARMY_PLAYER)) return;
    BYTE *leg = ARMY_RECORD(ARMY_PLAYER);
    if (!leg) return;
    BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
    for (int g = 0; un && un < end && g < 64; un += UNIT_STRIDE, g++) {
        if (UNIT_MODE(un) < 3 || UNIT_MODE(un) > 5) continue;
        int last = UNIT_LAST(un);
        for (int i = 0; i <= last && i < 10; i++) {
            BYTE *t = UNIT_TROOPS(un)[i];
            if (!alive(t)) continue;
            int cx = SARGE_X(t), cy = SARGE_Y(t), top = cy - 24;
            BYTE *av = TROOPER_AVATAR(t), *an = av ? AVATAR_ANIM(av) : NULL;
            if (an) { const int *r = ANIM_RECT(an); cx = (r[0] + r[2]) / 2; cy = (r[1] + r[3]) / 2; top = r[1]; }
            fn(t, cx, cy, top, 0);
        }
    }
}

/* health of a trooper changes: his unit's sum too (as FUN_0041e7e0) */
static void set_health(BYTE *t, int hp)
{
    int was = HEALTH(t);
    BYTE *un = TROOPER_UNIT(t);
    if (un && !IsBadReadPtr(un, 0x20)) HEALTH(un) = (short)(HEALTH(un) + hp - was);
    HEALTH(t) = (short)hp;
}

/* the soldier whose figure holds the world point (nearest centre if several) */
static BYTE *g_pick; static int g_pick_d;
static int g_pick_x, g_pick_y;
static void pick_one(BYTE *t, int cx, int cy, int top, int veh)
{
    (void)top; (void)veh;
    BYTE *av = TROOPER_AVATAR(t), *an = av ? AVATAR_ANIM(av) : NULL;
    int x0, y0, x1, y1;
    if (an) { const int *r = ANIM_RECT(an); x0 = r[0]; y0 = r[1]; x1 = r[2]; y1 = r[3]; }
    else { x0 = SARGE_X(t) - 8; y0 = SARGE_Y(t) - 24; x1 = SARGE_X(t) + 8; y1 = SARGE_Y(t) + 2; }
    if (g_pick_x < x0 - HIT_SLACK || g_pick_x > x1 + HIT_SLACK || g_pick_y < y0 - HIT_SLACK || g_pick_y > y1 + HIT_SLACK) return;
    int d = (g_pick_x - cx) * (g_pick_x - cx) + (g_pick_y - cy) * (g_pick_y - cy);
    if (!g_pick || d < g_pick_d) { g_pick = t; g_pick_d = d; }
}
static BYTE *soldier_at(int wx, int wy)
{
    g_pick = NULL; g_pick_x = wx; g_pick_y = wy;
    allies_each(pick_one);
    return g_pick;
}
static int g_member_found; static BYTE *g_member;
static void member_one(BYTE *t, int cx, int cy, int top, int veh) { (void)cx; (void)cy; (void)top; (void)veh; if (t == g_member) g_member_found = 1; }
static int still_ours(BYTE *t) { g_member = t; g_member_found = 0; allies_each(member_one); return g_member_found; }

/* ---------------------------------------------------------------- 2. regeneration */

static struct { BYTE *t; short last_hp; int since_ms; double owed; } g_reg[REGEN_SLOTS];
static DWORD g_reg_ms;
static int g_reg_dt;

static void regen_one(BYTE *t, int cx, int cy, int top, int veh)
{
    (void)cx; (void)cy; (void)top; (void)veh;
    unsigned h = (unsigned)((uintptr_t)t >> 4) * 2654435761u % REGEN_SLOTS;
    for (int k = 0; k < REGEN_SLOTS && g_reg[h].t && g_reg[h].t != t; k++) h = (h + 1) % REGEN_SLOTS;
    if (g_reg[h].t != t) {
        if (g_reg[h].t) return;                          /* table full */
        g_reg[h].t = t; g_reg[h].last_hp = HEALTH(t); g_reg[h].since_ms = 0; g_reg[h].owed = 0;
        return;
    }
    int hp = HEALTH(t), mx = MAX_HEALTH(t);
    if (hp < g_reg[h].last_hp) { g_reg[h].since_ms = 0; g_reg[h].owed = 0; }   /* hurt: wait again */
    else g_reg[h].since_ms += g_reg_dt;
    if (g_reg[h].since_ms >= g_cfg.regen_delay_ms && hp < mx && g_cfg.regen_full_s > 0) {
        g_reg[h].owed += (double)mx * g_reg_dt / (g_cfg.regen_full_s * 1000.0);
        int add = (int)g_reg[h].owed;
        if (add > 0) {
            g_reg[h].owed -= add;
            hp = hp + add > mx ? mx : hp + add;
            set_health(t, hp);
            if (g_cfg.log >= 2) mlog("allies: trooper %p regenerates to %d / %d", t, hp, mx);
        }
    }
    g_reg[h].last_hp = (short)hp;
}

/* ---------------------------------------------------------------- 3. medkits */

/* Carried kits across missions (2026-11-21, user: "medkits need to carry over between missions in the same area to
 * match the other level persistence"): like Sarge's weapons, which the game carries between the parts of a scenario
 * in the army file (FUN_0041a3e0 writes it when a part is won, FUN_0041a970 reads it when a part with index > 0
 * starts). Kept in memory per mission RUN (gameplay_run: any (re)start):
 *   - the next MissionID of the SAME scenario (the last part was won): the kits carried at the end of the last run;
 *   - the SAME scenario and mission again (Play Again, a restart, entered again from the menus after a loss): the
 *     kits that mission started with (the game restores the weapons the same way);
 *   - anything else (a new scenario, Boot Camp, the first start): none.
 * The percentages of the kits go with them. */
static struct { unsigned run; int scen, mission, n, start_n; short pct[MEDKIT_MAX], start_pct[MEDKIT_MAX]; } K = { 0, -1, -1, 0, 0, { 0 }, { 0 } };
static int g_heal;                     /* the medkit is selected */
static BYTE *g_heal_weapon;            /* Sarge's weapon when it was selected (another one = deselected) */
static struct { BYTE *t; DWORD at, idle_since; } H;   /* walking to a soldier to heal him */

static int kits_valid(void)
{
    BYTE *u = SARGE;
    int s, m;
    unsigned r = gameplay_run(&s, &m);
    if (!u) return 0;
    if (r != K.run) {
        const char *how;
        if (K.run && s == K.scen && s >= 0 && m == K.mission + 1) how = "carried over from the last part";        /* K.n, pct as they are */
        else if (K.run && s == K.scen && s >= 0 && m == K.mission) { K.n = K.start_n; memcpy(K.pct, K.start_pct, sizeof K.pct); how = "restored to the mission start"; }
        else { K.n = 0; how = "new scenario"; }
        mlog("allies: mission run %u (scenario %d mission %d): %d medkit(s) - %s", r, s, m, K.n, how);
        K.run = r; K.scen = s; K.mission = m;
        K.start_n = K.n; memcpy(K.start_pct, K.pct, sizeof K.pct);
        g_heal = 0; H.t = NULL;
    }
    return 1;
}
int allies_kits(void) { return g_cfg.ally_heal && kits_valid() ? K.n : 0; }
static int usable(BYTE *u) { return g_cfg.ally_heal && g_cfg.controls_modern && u && HEALTH(u) > 0 && !SARGE_VEHICLE(u); }
int allies_heal_mode(void) { return g_heal && usable(SARGE) && allies_kits() > 0; }

static void set_mode(int on, const char *why)
{
    if (on == g_heal) return;
    g_heal = on;
    g_heal_weapon = on && SARGE ? SARGE_WEAPON(SARGE) : NULL;
    mlog("allies: medkit %s (%s), %d kit(s)", on ? "selected" : "put away", why, K.n);
}

/* Ways out (2026-11-21, user: "med kits don't cancel"): H again, right click (controls.c, no shot), Esc (window hook
 * below, the key does not reach the game so no pause menu), choosing a weapon (weapons.c activate / quick swap; a
 * changed current weapon too), a vehicle, no kits left. After a heal the medkit stays selected only while kits remain AND
 * someone (a soldier on the map or Sarge) is still hurt - otherwise it is put away. */
int allies_cancel(const char *why)
{
    if (!g_heal) return 0;
    set_mode(0, why);
    return 1;
}
/* Esc: the window message (allies_message) and the DirectInput keyboard state the game reads (controls.c kb_get_state ->
 * allies_take_esc) - whichever sees the press first puts the medkit away; the key is then taken out of the game's
 * keyboard state until released, so the pause menu does not open (Wine 2026-11-21: the message alone was not enough). */
static int g_esc_eaten;
int allies_message(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)h; (void)lp;
    if (msg != WM_KEYDOWN || wp != VK_ESCAPE) return 0;
    if (g_esc_eaten) return 1;                                          /* the same press (auto-repeat) */
    if (!allies_heal_mode()) return 0;
    allies_cancel("Esc");
    g_esc_eaten = 1;
    return 1;
}
int allies_take_esc(int down)
{
    if (!down) { g_esc_eaten = 0; return 0; }
    if (!g_esc_eaten && allies_heal_mode()) { allies_cancel("Esc"); g_esc_eaten = 1; }
    return g_esc_eaten;
}

void allies_toggle_heal(void)
{
    BYTE *u = SARGE;
    if (!usable(u)) return;
    if (!g_heal && allies_kits() <= 0) { mlog("allies: heal key - no medkits carried"); return; }
    set_mode(!g_heal, "heal key");
}

static int __fastcall allies_pickup_test_px(BYTE *item, BYTE *sarge) {
    int r = PX_CALL(PICKUP_TEST(item, sarge));
    if (r != 0 || !g_cfg.ally_heal || !item || !sarge || sarge != SARGE || UID_ARMY(sarge) != ARMY_PLAYER) return r;
    if (OBJ_CATEGORY(item) != CAT_SPECIAL_ITEM || !OBJ_WEAPON(item) || W_TYPE(OBJ_WEAPON(item)) != TYPE_MEDKIT) return r;
    BYTE *g = GAME;
    if (!g || OBJ_RETEST_AT(item) != GAME_TIME(g)) return r;          /* the test did not run (5 s re-test wait) */
    if (HEALTH(sarge) < MAX_HEALTH(sarge)) return r;                  /* not the full-health refusal */
    kits_valid();
    if (K.n >= MEDKIT_MAX) { mlog("allies: medkit at %d,%d left on the ground - %d carried (the most)", SARGE_X(item), SARGE_Y(item), K.n); return r; }
    int pct = W_HEAL_PCT(OBJ_WEAPON(item));
    K.pct[K.n++] = (short)(pct > 0 ? pct : KIT_PCT_FALLBACK);
    mlog("allies: medkit picked up at full health (%d %% of max) - %d carried", pct, K.n);
    return 1;                                                           /* the game's pickup takes it */
}
int __fastcall allies_pickup_test(BYTE *item, BYTE *sarge) { PX_BEGIN(px); int r_ = allies_pickup_test_px(item, sarge); PX_END(px, PR_ALLIES); return r_; }   /* timed: stage PR_ALLIES (exclusive, common.h) */

static int kit_pct(void) { return K.n > 0 ? K.pct[K.n - 1] : 0; }
static int g_any_hurt;
static void hurt_one(BYTE *t, int cx, int cy, int top, int veh) { (void)cx; (void)cy; (void)top; (void)veh; if (HEALTH(t) < MAX_HEALTH(t)) g_any_hurt = 1; }
static void use_kit(void)
{
    if (K.n > 0) K.n--;
    PLAY_VOICE(SND_MEDKIT, 100);
    if (K.n == 0) { set_mode(0, "no kits left"); return; }
    g_any_hurt = SARGE && HEALTH(SARGE) < MAX_HEALTH(SARGE);
    allies_each(hurt_one);
    if (!g_any_hurt) set_mode(0, "nobody else hurt");
}
static int heal_amount(BYTE *it)
{
    int add = MAX_HEALTH(it) * kit_pct() / 100;
    return add < 1 ? 1 : add;
}

static void heal_soldier(BYTE *t)
{
    int hp = HEALTH(t), mx = MAX_HEALTH(t), to = hp + heal_amount(t);
    if (to > mx) to = mx;
    set_health(t, to);
    mlog("allies: medkit used on trooper %p at %d,%d: health %d -> %d / %d (+%d %%), %d kit(s) left", t, SARGE_X(t), SARGE_Y(t), hp, to, mx, kit_pct(), K.n - 1);
    use_kit();
}

int allies_hover(int wx, int wy, int unreachable)
{
    if (!allies_heal_mode()) return HEAL_NOBODY;
    BYTE *t = soldier_at(wx, wy);
    if (!t) return HEAL_NOBODY;
    if (HEALTH(t) >= MAX_HEALTH(t)) return HEAL_FULL;
    (void)unreachable;
    if (!in_heal_range(t)) return HEAL_NO_WAY;                        /* too far: no walk any more */
    return HEAL_OK;
}

int allies_click(int wx, int wy, DWORD now, int *walk_x, int *walk_y)
{
    H.t = NULL;
    BYTE *u = SARGE;
    if (!allies_heal_mode()) return 0;
    BYTE *t = soldier_at(wx, wy);
    if (!t) return 0;                                                  /* not on a soldier: the normal click */
    if (HEALTH(t) >= MAX_HEALTH(t)) { mlog("allies: heal click on trooper %p - at full health (%d)", t, HEALTH(t)); return 1; }
    (void)u; (void)now; (void)walk_x; (void)walk_y;
    if (in_heal_range(t)) { heal_soldier(t); return 1; }
    mlog("allies: heal click on trooper %p - too far (range %d px)", t, heal_range());
    return 1;                                                          /* taken: no walk */
}

int allies_heal_self(void)
{
    BYTE *u = SARGE;
    if (!allies_heal_mode() || HEALTH(u) >= MAX_HEALTH(u)) return 0;
    int hp = HEALTH(u), to = hp + heal_amount(u);
    if (to > MAX_HEALTH(u)) to = MAX_HEALTH(u);
    HEALTH(u) = (short)to;
    mlog("allies: medkit used on Sarge: health %d -> %d / %d (+%d %%), %d kit(s) left", hp, to, MAX_HEALTH(u), kit_pct(), K.n - 1);
    use_kit();
    return 1;
}

int allies_walk_frame(DWORD now, int walking)
{
    BYTE *u = SARGE;
    if (!H.t) return 0;
    if (!allies_heal_mode() || !still_ours(H.t)) { mlog("allies: heal walk dropped (medkit put away / soldier gone)"); H.t = NULL; return 0; }
    BYTE *t = H.t;
    int dx = SARGE_X(t) - SARGE_X(u), dy = SARGE_Y(t) - SARGE_Y(u);
    if (dx * dx + dy * dy <= HEAL_REACH * HEAL_REACH) {
        H.t = NULL;
        if (HEALTH(t) >= MAX_HEALTH(t)) { mlog("allies: reached trooper %p - already at full health", t); return 1; }
        heal_soldier(t);
        return 1;
    }
    if (now - H.at > HEAL_TIMEOUT_MS) { mlog("allies: heal walk timed out"); H.t = NULL; return 0; }
    if (walking) H.idle_since = now;
    else if (now - H.idle_since > HEAL_IDLE_MS) {
        mlog("allies: the walk ended %d px from trooper %p - no heal", (int)sqrt((double)(dx * dx + dy * dy)), t);
        H.t = NULL;
    }
    return 0;
}

int allies_heal_info(HealInfo *hi)
{
    memset(hi, 0, sizeof *hi);
    if (!allies_heal_mode()) return 0;
    int px, py;
    hi->on = 1; hi->kits = K.n; hi->pct = kit_pct(); hi->state = HEAL_NOBODY;
    if (!controls_pointer_world(&px, &py)) return 1;
    hi->x = px; hi->y = py;
    BYTE *t = soldier_at(px, py);
    if (t) { hi->hp = HEALTH(t); hi->max = MAX_HEALTH(t); hi->state = HEALTH(t) >= MAX_HEALTH(t) ? HEAL_FULL : !in_heal_range(t) ? HEAL_NO_WAY : HEAL_OK; }
    return 1;
}

/* ---------------------------------------------------------------- 4. medical tent
 * How the game heals Sarge at the tent (2026-11-22, read from the shipped Maps\alp*.txt + the exe): there is NO tent
 * code in the exe - the tent ("greenmash", a plain object) only gives the place, the mission SCRIPT does the healing.
 * Every single-player Alpine script (alp1_1 .. alp4_2) has the same block:
 *     PAD medicaltentpad <n> GREEN SARGE > 0              (n = 23 in alp1/alp2, 9 in alp3, 30 in alp4)
 *     IF on_medicaltentpad AFTER off_medicaltentpad AND gsmedtime AFTER NULL AND startup AFTER killed_greenmash THEN
 *         HEAL ME 10, TRIGGERDELAY 1.5 gsmedtime
 * = while Sarge stands on pad n and the tent is not destroyed: +10 % of his max at once, then again every 1.5 s.
 * HEAL = action 5 of the script parser FUN_00454a70, run by FUN_00452480 case 5 -> FUN_0041e7e0 (__fastcall ecx = UID,
 * edx = percent): Sarge (UID type 6) health += pct x max / 100, capped; a unit (type 2): the same per trooper +
 * the unit sum. The pad's own trigger only counts Sarge ("GREEN SARGE"), so soldiers were never healed there.
 * ("on_medicaltentpad1 ... GREEN UNIT -> HEAL ME 100" also heals SARGE, once, when a green unit enters.)
 * Mod: the player's soldiers (allies_each) standing on the same pad heal by the same rule: TENT_PCT of THEIR max
 * (the doubled one) on entering, then every TENT_EVERY_MS of game time, only while the tent is not destroyed.
 * The pad: the script's pad trigger table (FUN_0045b210 fills it while the script loads; names char *[512] at
 * 0x6a45d0, count 0x6a4dd0, records 0x24 bytes at 0x69dbd0: +0 short index, +2 short PHYSICAL pad number - as
 * FUN_0045bb80 reads it). "Standing on it": the trooper's tile in the pad layer (as missionpath.c on_pad).
 * The tent: named item TENT_NAME (objectives_item_ground -1 = destroyed; not named = taken as standing).
 * Sarge's own tent healing is the script's, untouched. */
#define PADTRIG_COUNT      (*(int *)0x6a4dd0)
#define PADTRIG_NAME(i)    (((const char **)0x6a45d0)[i])
#define PADTRIG_PAD(i)     (*(short *)(0x69dbd2 + (i) * 0x24))
#define PAD_BITS_LAYER     (*(BYTE **)0x4e354c)          /* pads 0..7: bits */
#define PAD_BYTE_LAYER     (*(BYTE **)0x4e3550)          /* pads 8..255: byte value */
#define MAP_TILE_COUNT     (*(unsigned *)0x6083c8)
#define MAP_TILES_W        (*(unsigned short *)0x4e368c)
#define TENT_PAD_NAME      "medicaltentpad"
#define TENT_NAME          "greenmash"
#define TENT_PCT           10                             /* HEAL ME 10 */
#define TENT_EVERY_MS      1500                           /* TRIGGERDELAY 1.5 gsmedtime */

static int tent_on(void) { static int v = -1; if (v < 0) v = GetPrivateProfileIntA("gameplay", "tent_heals_allies", 1, mod_ini_path()) != 0; return v; }

static struct { BYTE *t; int ms; } g_tent[REGEN_SLOTS];   /* per soldier on the pad: game ms since his last tent heal (-1 = off it) */
static int g_tent_pad = -2;                                /* -2 = not looked up this run, -1 = no tent pad in this mission */

static int tent_pad(void)
{
    if (g_tent_pad != -2) return g_tent_pad;
    int n = PADTRIG_COUNT;
    if (n <= 0) return -1;                                   /* script not loaded yet (or no pads): ask again */
    g_tent_pad = -1;
    if (n <= 512)
        for (int i = 0; i < n; i++) {
            const char *nm = PADTRIG_NAME(i);
            if (!nm || IsBadReadPtr(nm, sizeof TENT_PAD_NAME) || lstrcmpiA(nm, TENT_PAD_NAME)) continue;
            int p = PADTRIG_PAD(i);
            if (p >= 0 && p < 256) g_tent_pad = p;
            break;
        }
    if (g_tent_pad >= 0) {
        BYTE *layer = g_tent_pad < 8 ? PAD_BITS_LAYER : PAD_BYTE_LAYER;   /* where it is (log only, once per mission) */
        unsigned nt = MAP_TILE_COUNT, w = MAP_TILES_W, k = 0; long long sx = 0, sy = 0;
        if (layer && w && nt && nt <= 1024 * 1024 && !IsBadReadPtr(layer, nt))
            for (unsigned t = 0; t < nt; t++)
                if (g_tent_pad < 8 ? (layer[t] & (1 << g_tent_pad)) != 0 : layer[t] == g_tent_pad) { sx += (t % w) * 16 + 8; sy += (t / w) * 16 + 8; k++; }
        mlog("allies: medical tent pad %d (script pad '%s', %u tiles, centre %d,%d) - soldiers on it heal %d %% every %d ms",
             g_tent_pad, TENT_PAD_NAME, k, k ? (int)(sx / k) : 0, k ? (int)(sy / k) : 0, TENT_PCT, TENT_EVERY_MS);
    }
    return g_tent_pad;
}
static int on_pad(int pad, int x, int y)
{
    BYTE *layer = pad < 8 ? PAD_BITS_LAYER : PAD_BYTE_LAYER;
    unsigned n = MAP_TILE_COUNT, w = MAP_TILES_W;
    if (!layer || !w || x < 0 || y < 0) return 0;
    unsigned t = (unsigned)(y / 16) * w + (unsigned)(x / 16);
    if (t >= n || IsBadReadPtr(layer + t, 1)) return 0;
    return pad < 8 ? (layer[t] & (1 << pad)) != 0 : layer[t] == pad;
}
static int tent_standing(void)
{
    int x, y, raw;
    return objectives_item_ground(TENT_NAME, &x, &y, &raw, NULL) != -1;
}

static void tent_one(BYTE *t, int cx, int cy, int top, int veh)
{
    (void)cx; (void)cy; (void)top; (void)veh;
    unsigned h = (unsigned)((uintptr_t)t >> 4) * 2654435761u % REGEN_SLOTS;
    for (int k = 0; k < REGEN_SLOTS && g_tent[h].t && g_tent[h].t != t; k++) h = (h + 1) % REGEN_SLOTS;
    if (g_tent[h].t != t) { if (g_tent[h].t) return; g_tent[h].t = t; g_tent[h].ms = -1; }
    if (!on_pad(g_tent_pad, SARGE_X(t), SARGE_Y(t))) { g_tent[h].ms = -1; return; }
    if (g_tent[h].ms >= 0) { g_tent[h].ms += g_reg_dt; if (g_tent[h].ms < TENT_EVERY_MS) return; }
    g_tent[h].ms = 0;                                        /* stepped on (heal at once, as on_pad fires) or 1.5 s gone */
    int hp = HEALTH(t), mx = MAX_HEALTH(t);
    if (hp >= mx) return;
    int add = mx * TENT_PCT / 100, to;
    if (add < 1) add = 1;
    to = hp + add > mx ? mx : hp + add;
    set_health(t, to);
    if (g_cfg.log >= 1) mlog("allies: trooper %p at the medical tent (pad %d, %d,%d): health %d -> %d / %d", t, g_tent_pad, SARGE_X(t), SARGE_Y(t), hp, to, mx);
}

/* ---------------------------------------------------------------- per gameplay frame */

void allies_frame(void)
{
    static unsigned run;
    unsigned r = gameplay_run(NULL, NULL);
    if (r != run) { run = r; memset(g_reg, 0, sizeof g_reg); g_reg_ms = 0; memset(g_tent, 0, sizeof g_tent); g_tent_pad = -2; }   /* troopers of the last run: forgotten */
    DWORD now = clock_game_ms();
    g_reg_dt = g_reg_ms ? (int)(now - g_reg_ms) : 0;
    g_reg_ms = now;
    if (g_reg_dt < 0 || g_reg_dt > 250) g_reg_dt = 0;      /* a gap (menu, loading): not healing time */
    if (g_cfg.ally_regen) allies_each(regen_one);
    if (tent_on() && tent_pad() >= 0 && tent_standing()) allies_each(tent_one);
    kits_valid();
    BYTE *u = SARGE;
    if (g_heal) {
        if (!usable(u)) set_mode(0, "Sarge in a vehicle / down / off");
        else if (K.n <= 0) set_mode(0, "no kits");
        else if (SARGE_WEAPON(u) != g_heal_weapon) set_mode(0, "weapon changed");
    }
}


/* savestate.c: medkits, regeneration and a heal walk are part of the mission state */
void allies_savestate(void) { SAVESTATE(K); SAVESTATE(g_reg); SAVESTATE(g_reg_ms); SAVESTATE(g_reg_dt); SAVESTATE(g_heal); SAVESTATE(g_heal_weapon); SAVESTATE(H); SAVESTATE(g_tent); }
