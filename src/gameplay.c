/* ============================================================================================
 * gameplay.c - game rule changes (switchable in the Gameplay menu, menu.c)
 *
 * Health regeneration ([gameplay] health_regen=1):
 *   Sarge's health (short at unit +0x1c, maximum at +0x1e; see the health pickup, type 0x16, in
 *   FUN_00449330) slowly comes back - but only after he has not been hurt for a while. Every hit
 *   restarts that wait, so under steady fire from several enemies nothing regenerates and he can
 *   still die; between fights he recovers.
 *     regen_delay_ms  time without damage before healing starts            (default 6000)
 *     regen_full_s    seconds to heal from empty to full once it has started (default 90)
 *   Time is the game clock (clock.c: speed and Pause applied). Healing is only counted while
 *   gameplay frames run, so menus and the pause screen do not heal.
 * Called once per gameplay frame from controls.c's keyboard-dispatcher wrapper.
 * ============================================================================================ */
#include "common.h"

#define SARGE           (*(BYTE **)0x697520)
#define SARGE_HEALTH(u) (*(short *)((u) + 0x1c))
#define SARGE_MAX(u)    (*(short *)((u) + 0x1e))

/* ---------------------------------------------------------------- longer weapon range (2026-11-20)
 * User: "extend the range of the rifle and similar weapons since the original went a little bit offscreen - with
 * more area visible, we want a longer range. maybe 20% more?". The game's weapon table AIinfo.weaponTables[]
 * (game table AI_WTBL, 24 bytes, range.nMaxRange short at +10) lies at WEAPON_TABLE and is filled at start from
 * AiDefault.aai. WeaponMaxRange (0x445c60) reads it live: the projectile's travel cap (FUN_00435e20,
 * game binary), Sarge's fire distance (SetFireWeapon), and the AI's "in range" tests (CanShoot,
 * AiObjectFiringRange) - so enemies reach as far as Sarge does. Changed: the direct-fire weapons below.
 * Kept: grenade / mortar (their throw speed is mapped through the max, game binary), flame (its reach is a
 * hard-coded 300 px, game binary), tank cannon. The originals are remembered; a value the game writes anew
 * (a reload) becomes the new original. [gameplay] longer_range=1, weapon_range_pct=120. */
#define WEAPON_TABLE      0x505044
#define WEAPON_ROW        24
#define WEAPON_MAX_RANGE  10
static const int RANGED[] = { 1, 4, 7, 8, 9, 10, 21 };   /* LMG, bazooka, MMG, HMG, rifle, auto rifle, tower rifle */
#define NRANGED ((int)(sizeof RANGED / sizeof RANGED[0]))
static void weapon_range(void)
{
    static short orig[NRANGED], applied[NRANGED];
    int pct = g_cfg.weapon_range_pct < 50 ? 50 : g_cfg.weapon_range_pct > 200 ? 200 : g_cfg.weapon_range_pct;
    for (int i = 0; i < NRANGED; i++) {
        short *p = (short *)(WEAPON_TABLE + RANGED[i] * WEAPON_ROW + WEAPON_MAX_RANGE);
        short cur = *p;
        if (cur <= 0 || cur > 4000) continue;            /* not loaded (yet) */
        if (!applied[i] || cur != applied[i]) orig[i] = cur;
        short want = (short)(g_cfg.longer_range ? orig[i] * pct / 100 : orig[i]);
        if (cur != want) {
            *p = want;
            mlog("gameplay: weapon %d max range %d -> %d", RANGED[i], orig[i], want);
        }
        applied[i] = want;
    }
}

/* Vehicle health regeneration (2026-11-20, user: "do vehicles regain health like sarge? if not, they should. just much
 * slower"): the vehicle Sarge drives (SARGE +0x2d0) heals like Sarge - after regen_delay_ms without damage - but
 * VEHICLE_REGEN_SLOWER times slower ([gameplay] vehicle_regen_x, default 4; 0 = off). Health +0x1c / max +0x1e.
 * Only while alive (state 1, health > 0). Game clock. */
#define VEHICLE_REGEN_SLOWER 4
static void vehicle_regen(void)
{
    static int slower = -1;
    if (slower < 0) slower = GetPrivateProfileIntA("gameplay", "vehicle_regen_x", VEHICLE_REGEN_SLOWER, mod_ini_path());
    static BYTE *who; static int last_hp, since_hurt_ms; static double owed; static DWORD last_ms;
    BYTE *u = SARGE;
    BYTE *v = u && !IsBadReadPtr(u, 0x2d4) ? *(BYTE **)(u + 0x2d0) : NULL;
    DWORD now = clock_game_ms();
    int dt = last_ms ? (int)(now - last_ms) : 0;
    last_ms = now;
    if (!g_cfg.health_regen || slower <= 0 || !v || IsBadReadPtr(v, 0x70) || *(int *)(v + 0x68) != 1) { who = NULL; return; }
    int hp = *(short *)(v + 0x1c), max = *(short *)(v + 0x1e);
    if (v != who || hp <= 0 || max <= 0) { who = v; last_hp = hp; since_hurt_ms = 0; owed = 0; return; }
    if (dt < 0 || dt > 250) dt = 0;
    if (hp < last_hp) { since_hurt_ms = 0; owed = 0; }
    else since_hurt_ms += dt;
    if (since_hurt_ms >= g_cfg.regen_delay_ms && hp < max && g_cfg.regen_full_s > 0) {
        owed += (double)max * dt / (g_cfg.regen_full_s * 1000.0 * slower);
        int add = (int)owed;
        if (add > 0) { owed -= add; hp = hp + add > max ? max : hp + add; *(short *)(v + 0x1c) = (short)hp; }
    }
    last_hp = hp;
}

void gameplay_frame(void)
{
    weapon_range();
    capture_frame();                             /* capture.c: Sarge in an enemy vehicle */
    barricades_frame();                          /* barricades.c: Sarge stuck on a barricade */
    fences_frame();                              /* fences.c: Sarge pushing against a wooden fence breaks it */
    squad_frame();                               /* squad.c: troopers following Sarge sprint with him */
    patrol_frame();                              /* patrol.c: enemy marches patrol between post and goal */
    allies_frame();                              /* allies.c: the player's soldiers regenerate, medkit state */
    vehicle_regen();
    static BYTE *who;
    static int last_hp, since_hurt_ms;
    static double owed;                          /* fractions of a health point not yet given */
    static DWORD last_ms;
    BYTE *u = SARGE;
    if (!g_cfg.health_regen || !u) { who = NULL; return; }
    DWORD now = clock_game_ms();
    int dt = last_ms ? (int)(now - last_ms) : 0;
    last_ms = now;
    int hp = SARGE_HEALTH(u), max = SARGE_MAX(u);
    if (u != who || hp <= 0 || max <= 0) {       /* new Sarge, or dead: start over */
        who = u; last_hp = hp; since_hurt_ms = 0; owed = 0;
        mlog("gameplay: Sarge health %d / %d", SARGE_HEALTH(u), max);
        return;
    }
    if (dt < 0 || dt > 250) dt = 0;              /* a gap (menu, loading): not healing time */
    if (hp < last_hp) { since_hurt_ms = 0; owed = 0; }   /* hurt: wait again */
    else since_hurt_ms += dt;
    if (since_hurt_ms >= g_cfg.regen_delay_ms && hp < max && g_cfg.regen_full_s > 0) {
        owed += (double)max * dt / (g_cfg.regen_full_s * 1000.0);
        int add = (int)owed;
        if (add > 0) {
            owed -= add;
            hp = hp + add > max ? max : hp + add;
            SARGE_HEALTH(u) = (short)hp;
            if (g_cfg.log >= 2) mlog("gameplay: health regenerates to %d / %d", hp, max);
        }
    }
    last_hp = hp;
}

/* ---------------------------------------------------------------- no squash at barricades (2026-11-20)
 * User: "just had an armymen hit the corner of a fence and was 'smushed'", then "enemies can't get squished under
 * [barricades]". Three places in the game kill a soldier with the squash death (DMGTYPE_RUNOVER / type 0):
 *  1. TrooperUpdate 0x43c810 (binary), at its start: the soldier's tile (+0xc) is untrafficable
 *     (call 0x43c840 -> UntrafficableUnit 0x42a230: movement layer >= 15 or a parked vehicle) -> TrooperAcceptDamage
 *     0x43c350 ("died when terrain became untrafficable").
 *  2. The same function after the step (AvatarPerform): the new point's tile AND the tile at the start are both
 *     untrafficable (second test: call 0x43c9e3) -> 0x43c350 at 0x43c9ff ("died when he moved onto untrafficable
 *     terrain").
 *  3. AddObjectToMovementLayer 0x40ce70 (binary; callers 0x431701 / 0x458634 pass the object state's crush
 *     damage, state +0x12): every tile where the object's new footprint differs from the old one hits Sarge,
 *     soldiers, vehicles and wrecks at the tile centre with ItemDamage 0x431f60 (call 0x40cff3) - a fence or
 *     sandbag wall changing state after being shot squashed the soldiers lying next to it.
 * Fence / sandbag / wall footprints mark the whole 16 px tile, so a soldier whose path clips a corner (or who lies
 * behind one) is "on" it. Kept (the real cases): a vehicle parking on / running over a soldier (the vehicle layer,
 * and the vehicle's own crush code, untouched), buildings and other non-barricade objects collapsing (crush path
 * from any other category; a tile still blocked without the barricades at it). Barricades = barricades.c
 * barricade_category (ROCK 0 - sandbag rings are ROCK -, FALLEN_TREE 4, FENCE 5, WALL 6, GATE 20, WALL_GATE 21).
 * 1 + 2: squash_stub replaces both UntrafficableUnit calls (ESI = trooper, ECX = tile): "blocked" only when the
 * SAME tile was free the last time this soldier was checked (it became blocked under him) AND the tile is still
 * blocked without the barricade objects at its centre / at the soldier (or a vehicle is parked there). Site 2 then
 * never fires by itself (site 1 ran this frame on the same tile). 3: crush_stub skips the damage for Sarge and
 * soldiers when the object is a barricade. [gameplay] fix_squash=1 (COMBAT "No corner squash"). */
#define UNTRAFFICABLE  ((int (__fastcall *)(int))0x42a230)
#define MOVEMENT_LAYER (*(signed char **)0x4e3554)      /* the_map.movement_layer: per tile, >= 15 untrafficable */
#define VEHICLE_LAYER  (*(BYTE **)0x507f78)             /* parked vehicles per tile (UntrafficableUnit's 2nd test) */
#define MAP_TILES_W    (*(unsigned short *)0x4e368c)
#define TROOPER_X(t)   (*(short *)((t) + 0x04))
#define TROOPER_Y(t)   (*(short *)((t) + 0x06))
typedef BYTE *(__fastcall *GpObjectsAtFn)(short *point);
#define GP_OBJECTS_AT  ((GpObjectsAtFn)0x42fca0)        /* GetObjectsIntersectPoint: ITEM list, next at +0x60 */
#define GP_ITEM_TYPE1(it) ((int)((*(DWORD *)(it) >> 22) & 0xf))   /* ITEM_TYPE + 1: 5 trooper, 6 Sarge, 7/8/9 objects */
#define GP_OBJ_CATEGORY(it) (*(short *)((it) + 0x68))
#define GP_OBJ_MOVEMENT(it) (*(signed char *)((it) + 0x6f))

/* Movement of the barricade objects whose picture holds (x, y); seen[] avoids counting one object twice. */
static int barricade_movement(int x, int y, BYTE **seen, int *nseen)
{
    short p[2] = { (short)x, (short)y };
    int sum = 0;
    for (BYTE *it = GP_OBJECTS_AT(p); it; it = *(BYTE **)(it + 0x60)) {
        int t = GP_ITEM_TYPE1(it);
        if ((t != 7 && t != 8 && t != 9) || !barricade_category(GP_OBJ_CATEGORY(it))) continue;
        int k = 0;
        while (k < *nseen && seen[k] != it) k++;
        if (k < *nseen) continue;
        if (*nseen < 8) seen[(*nseen)++] = it;
        sum += GP_OBJ_MOVEMENT(it);
    }
    return sum;
}

/* The tile is blocked by something that really squashes: a parked vehicle, or the movement layer minus the
 * barricades at the tile centre and at the soldier is still >= 15. */
static int squash_cause(BYTE *trooper, int tile, int *barr_out)
{
    *barr_out = 0;
    if (VEHICLE_LAYER && VEHICLE_LAYER[tile]) return 1;
    if (!MOVEMENT_LAYER || !MAP_TILES_W) return 1;
    BYTE *seen[8]; int nseen = 0;
    int tx = tile % MAP_TILES_W, ty = tile / MAP_TILES_W;
    int barr = barricade_movement(tx * 16 + 8, ty * 16 + 8, seen, &nseen);
    if ((TROOPER_Y(trooper) >> 4) * MAP_TILES_W + (TROOPER_X(trooper) >> 4) == tile)
        barr += barricade_movement(TROOPER_X(trooper), TROOPER_Y(trooper), seen, &nseen);
    *barr_out = barr;
    return MOVEMENT_LAYER[tile] - barr >= 15;
}

#define SQ_SLOTS 128
static struct { BYTE *trooper; int tile; BYTE blocked; } g_sq[SQ_SLOTS];
int __cdecl squash_check(BYTE *trooper, int tile)
{
    int blocked = UNTRAFFICABLE(tile) & 0xff;
    if (!g_cfg.fix_squash || !trooper) return blocked;
    unsigned h = (unsigned)((uintptr_t)trooper >> 4) * 2654435761u % SQ_SLOTS;
    int was_free_here = g_sq[h].trooper == trooper && g_sq[h].tile == tile && !g_sq[h].blocked;
    g_sq[h].trooper = trooper; g_sq[h].tile = tile; g_sq[h].blocked = (BYTE)(blocked != 0);
    if (!blocked) return 0;
    static DWORD logged;
    int barr = 0;
    if (was_free_here) {                                 /* became blocked under him */
        if (squash_cause(trooper, tile, &barr)) {
            mlog("gameplay: trooper %p at %d,%d: tile %d became blocked under him (movement %d, barricades %d) - squashed",
                 trooper, TROOPER_X(trooper), TROOPER_Y(trooper), tile, MOVEMENT_LAYER ? MOVEMENT_LAYER[tile] : -1, barr);
            return 1;
        }
        mlog("gameplay: trooper %p at %d,%d: tile %d became blocked by a barricade (movement %d, barricades %d) - not squashed",
             trooper, TROOPER_X(trooper), TROOPER_Y(trooper), tile, MOVEMENT_LAYER ? MOVEMENT_LAYER[tile] : -1, barr);
        return 0;
    }
    if (GetTickCount() - logged > 2000) {
        logged = GetTickCount();
        mlog("gameplay: trooper %p at %d,%d on blocked tile %d (walked onto it) - not squashed", trooper, TROOPER_X(trooper), TROOPER_Y(trooper), tile);
    }
    return 0;
}
__asm__(".globl _squash_stub\n"
        "_squash_stub:\n"
        "    push %ecx\n"                                /* tile */
        "    push %esi\n"                                /* the trooper (0x43c83d / 0x43c9e0: mov ecx, [esi+0xc]) */
        "    call _squash_check\n"
        "    add $8, %esp\n"
        "    ret\n");

/* 3. The crush damage of AddObjectToMovementLayer. At the call 0x40cff3: ECX = the item hit, EDX = damage, stack
 * (callee pops 12): damage type, SPOINT *at, UID from; the object being added is the caller's local at [esp+0x14]
 * before the three pushes = [esp+0x24] at our entry. Returns 1 = do the game's damage. */
int __cdecl crush_allowed(BYTE *item, int damage, BYTE *obj)
{
    if ((!g_cfg.fix_squash && !g_fences_breaking) || !item || !obj) return 1;   /* fences.c breaking a fence: never a crush */
    int t = GP_ITEM_TYPE1(item);
    if (t != 5 && t != 6) return 1;                                /* vehicles, wrecks: the game's */
    int cat = GP_OBJ_CATEGORY(obj);
    if (!barricade_category(cat)) {
        mlog("gameplay: %s %p at %d,%d crushed by object category %d (damage %d) - the game's", t == 6 ? "Sarge" : "trooper",
             item, TROOPER_X(item), TROOPER_Y(item), cat, damage);
        return 1;
    }
    mlog("gameplay: %s %p at %d,%d next to a barricade (category %d) changing shape - not crushed (damage %d)",
         t == 6 ? "Sarge" : "trooper", item, TROOPER_X(item), TROOPER_Y(item), cat, damage);
    return 0;
}
__asm__(".globl _crush_stub\n"
        "_crush_stub:\n"
        "    push %ecx\n"
        "    push %edx\n"
        "    pushl 0x2c(%esp)\n"                         /* the object: caller [esp+0x14] = our [esp+0x24], +8 pushed */
        "    push %edx\n"
        "    push %ecx\n"
        "    call _crush_allowed\n"
        "    add $12, %esp\n"
        "    pop %edx\n"
        "    pop %ecx\n"
        "    test %eax, %eax\n"
        "    jz 1f\n"
        "    mov $0x431f60, %eax\n"                      /* ItemDamage */
        "    jmp *%eax\n"
        "1:  ret $12\n");

/* ---------------------------------------------------------------- mission runs (2026-11-21)
 * A number that changes whenever a mission is (re)started: a new scenario / MissionID (gpGame +0x70 / +0x74), the game
 * state (thiscall 0x423d00) going from loading (4) to play (5) - polled every frame from dd_present, so a short load is seen -, or the
 * game clock (gpGame +0x44, game ms from the mission start) going backwards - Play Again, a restart, or the same
 * mission entered again from the menus all keep the scenario / mission numbers AND Sarge's address (he is the static
 * record in the legion, 0x4f7944), so neither tells a new run. Per-mission mod state that holds game pointers
 * (weapons.c boxes) or must be restored on a restart (allies.c medkits) keys on it.
 * Crash 2026-11-21 (user: 0x43fdc6 in the HUD weapon-icon widget, FUN_00440210 -> FUN_0043fd40, the icon frame =
 * Sarge's current weapon TYPE (FUN_004402b0: widget +0x4c data +8 = FUN_00448d70) read as "ens/"): mission 7 lost (Sarge
 * alive), entered again from the menus - weapons.c kept its boxes (same Sarge, same mission number) and swapped a
 * weapon record of the previous run, freed and reused for text, into Sarge's slot. */
static unsigned g_run = 1;
#define GP_GAME_STATE ((int (__fastcall *)(void *, int))0x423d00)   /* thiscall: the game's state (4 loading, 5 play) */
unsigned gameplay_run(int *scenario, int *mission)
{
    static int last_s = -999, last_m = -999, last_t = -1, last_gs = -1;
    BYTE *g = *(BYTE **)0x4b3f70;
    if (!g || IsBadReadPtr(g, 0x80)) { if (scenario) *scenario = -1; if (mission) *mission = -1; return g_run; }
    int s = *(int *)(g + 0x70), m = *(int *)(g + 0x74), t = *(int *)(g + 0x44), gs = GP_GAME_STATE(g, 0);
    /* Wine 2026-11-21: Play Again goes game state 5 -> 4 -> 5 with the clock NOT reset; a won part goes on to the next
     * MissionID without leaving state 5 */
    int reload = last_gs == 4 && gs == 5;
    if (s != last_s || m != last_m || reload || (last_t >= 0 && t + 500 < last_t)) {
        if (last_s != -999) { g_run++; mlog("gameplay: mission run %u (scenario %d mission %d%s, game clock %d, was %d / %d at %d)", g_run, s, m, reload ? ", reloaded" : "", t, last_s, last_m, last_t); }
        last_s = s; last_m = m;
    }
    last_t = t; last_gs = gs;
    if (scenario) *scenario = s;
    if (mission) *mission = m;
    return g_run;
}
