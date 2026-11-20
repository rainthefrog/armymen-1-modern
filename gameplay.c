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

void gameplay_frame(void)
{
    weapon_range();
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

/* ---------------------------------------------------------------- no squash at fence corners (2026-11-20)
 * User: "just had an armymen hit the corner of a fence and was 'smushed'". The game's trooper update (0x43c810,
 * binary TrooperUpdate) kills a soldier with DMGTYPE_RUNOVER (= the squash death) whenever the tile
 * he is on is untrafficable (call 0x43c840 -> UntrafficableUnit 0x42a230: movement layer >= 15 or a parked
 * vehicle) - "died when terrain became untrafficable". Meant for a building collapsing on him or a vehicle parking
 * on him; but a soldier whose path clips the 16 px corner tile of a fence (its footprint marks the tile) dies too.
 * The call is redirected here (squash_stub passes the trooper, ESI there, and the tile, ECX): "blocked" only when
 * the SAME tile was free the last time this soldier was checked - it became blocked under him. A soldier who
 * walked onto a blocked tile is left alone (the game moves him on). The second kill site (0x43c9ff, destination
 * tile) is unchanged. [gameplay] fix_squash=1 (COMBAT "No corner squash"). */
#define UNTRAFFICABLE ((int (__fastcall *)(int))0x42a230)
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
    if (was_free_here) return 1;                         /* became blocked under him: the game's squash */
    static DWORD logged;
    if (GetTickCount() - logged > 2000) { logged = GetTickCount(); mlog("gameplay: trooper %p on blocked tile %d (walked onto it) - not squashed", trooper, tile); }
    return 0;
}
__asm__(".globl _squash_stub\n"
        "_squash_stub:\n"
        "    push %ecx\n"                                /* tile */
        "    push %esi\n"                                /* the trooper (0x43c83d: mov ecx, [esi+0xc]) */
        "    call _squash_check\n"
        "    add $8, %esp\n"
        "    ret\n");
