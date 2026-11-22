/* ============================================================================================
 * capture.c - Sarge drives empty enemy vehicles ([gameplay] drive_enemy_vehicles=1, COMBAT page)
 *
 * User 2026-11-20: "would also be really cool to have a toggle to enable sarge driving enemy vehicles so long
 * as they are empty. prevent anyone from entering the vehicle if sarge is inside it".
 *
 * Game facts (checked against the disassembly):
 *   VEHICLE (game table): +0x00 UID, +0x0c tile, +0x1c health, +0x2c TAD (targeting entry), +0x68 state
 *   (1 = deployed), +0xb8 full_velocity, +0xe0 AI controller, +0xe4 aiGoal (SPOINT), +0xe8 npc (AI drives it;
 *   every non-player vehicle in one-player games, VehicleInitialDeploy 0x417010), +0xec humanAI,
 *   +0x14c pDriver (Sarge). In Army Men 1 enemy vehicles have no crew: the AI drives them (npc); soldiers only
 *   ride in them as passengers (UNIT +0x70 mode 2 = UNIT_IN_VEHICLE, +0xac pVehicle).
 *   UID bits (game table): army+1 << 26 (3 bits), item type+1 << 22 (vehicle = 2), class+1 << 16,
 *   legion index+1 (16 bits). The army comes ONLY from the UID: targeting (game binary ITEM_ARMY of the
 *   target), friend / foe of shots (projectiles take the firing vehicle's army, game binary),
 *   VehicleDie's "bail Sarge out" (SargePointer(ITEM_ARMY)), invulnerable cheat, visibility. VehicleFind
 *   (0x4188c0) = legion[army].slot[index] with the same UID (no NULL check).
 *
 * What changes while Sarge drives a vehicle of another army (single-player games only):
 *   - the vehicle becomes the player's: its UID army is the player's and its index points at a spare slot
 *     (CAPTURE_SLOT, beyond the count, so no player loop runs it twice) of the player's legion, which holds
 *     the vehicle - VehicleFind of the new UID finds it. It stays in its own army's list (that army's update
 *     loop still moves it, as before). Its TAD is moved to the player's army (deleted BEFORE the UID change:
 *     TadDeleteTad finds the list by the item's army). Result: enemies target and shoot it, the player's
 *     soldiers do not; its shots hurt enemies, not the player's soldiers; it dies like a player vehicle
 *     (Sarge bails out).
 *   - its AI is off: npc / aiGoal / humanAI cleared, and RunAI's HandleVehicle (0x4296f0, call 0x4296b6) skips
 *     it (Control would otherwise drive and fire it).
 *   - no unit may board it (UnitEnterVehicle 0x41d800 refuses it).
 *   - visibility: no game loop checks it for the enemy (they only look at foe legions' counted vehicles), so
 *     each frame every enemy seer (units on the map, vehicles, Sarge) that has line of sight
 *     (CanSeeFromItem 0x44af10, the test ItemUpdateSeen uses) marks it seen (See 0x449a90) - no "item seen"
 *     events fired.
 *   - its colours are the player's (user 2026-11-20: "some green in the driver's seat to look like it's being
 *     driven by sarge"): the body and turret ANIMs' trans_table (game binary AnimTransTable(pAnimation /
 *     pAnimation[TURRETANIM], UnitColorTable(army)), done once at creation, 0x416d40) point at the player's
 *     unit colour table. AnimCellDisplay copies it into the sprite every draw, so the 8-bit picture turns green
 *     at once, and hdworld.c's army_of (the table the remap blitter gets) picks the green ramp for the upscaled
 *     art - one source for both. VEHICLE +0x9c pAnimation, ANIM size 0xe4 (turret = pAnimation + 0xe4),
 *     ANIM +0x08 trans_table; unit_color_tables 0x4f75e8 (as modern.c colour-blind).
 *   On leaving (SargeOutVehicle, also from VehicleDie / VehicleUndeploy): UID, legion slot, TAD and colours go
 *   back to its army; the AI stays off - an abandoned vehicle stays parked (Sarge can come back to it).
 *
 * Rewrites / wraps:
 *   FUN_00418910 Enter Vehicle key (VehicleSargeFlip, game binary), key-table handler, __cdecl no args.
 *     Kept: in a vehicle -> get out when it stands still (sound 0x4187b0 edx 6, SargeOutVehicle); command
 *     cursor (0x4b8a88) blocks; nearest within 6 tiles (ItemCenterPos 0x449d10 -> tile, TileDistance
 *     0x42b910), own vehicles deployed and not npc. Added: other armies' vehicles that capture_can_enter.
 *   FUN_00448ae0 SargeInVehicle (__fastcall ecx Sarge, edx vehicle): wrapped, then capture.
 *   FUN_00448b50 SargeOutVehicle (__fastcall ecx Sarge): wrapped, then release; from a wreck (VehicleDie) also bail_out
 *   ([gameplay] vehicle_bailout, below).
 *   The spare slot 49 is never left empty after a capture (blank stand-in, see slot_clear: VehicleFind crash fix).
 *   FUN_0041d800 UnitEnterVehicle (__fastcall ecx unit, edx vehicle): wrapped, refused for the captured one.
 *   call 0x4296b6 -> HandleVehicle 0x4296f0 (__fastcall ecx vehicle): skipped for the captured one.
 *   FUN_0041a3e0 army file writer (__fastcall ecx FILE *): wrapped - the captured vehicle is written as its own army's
 *   vehicle (own UID) and Sarge on foot (Play Again crash fix 2026-11-21, see capture_army_save).
 * ============================================================================================ */
#include "common.h"

/* ---- game memory */
#define PLAYER_SARGE      (*(BYTE **)0x697520)
#define GAME              (*(BYTE **)0x4b3f70)          /* gpGame */
#define GAME_TYPE(g)      (*(int *)((g) + 0x68))         /* 0 = one player */
#define COMMAND_CURSOR    (*(volatile char *)0x4b8a88)   /* air strike etc. target cursor on */
#define MAP_TILES_W       (*(unsigned short *)0x4e368c)
#define LEGION_STATUS(a)  (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))   /* army in play */
#define ARMIES            4
#define PLAYER_ARMY       0

#define UID(it)           (*(DWORD *)(it))
#define UID_ARMY(u)       ((int)(((u) >> 26) & 7) - 1)
#define UID_TYPE(u)       ((int)(((u) >> 22) & 0xf) - 1)
#define UID_ARMY_MASK     0x1c000000u
#define UID_INDEX_MASK    0x0000ffffu
#define ITEM_VEHICLE      2
#define ITEM_TILE(it)     (*(int *)((it) + 0x0c))
#define ITEM_HEALTH(it)   (*(short *)((it) + 0x1c))
#define ITEM_TAD(it)      (*(void **)((it) + 0x2c))

#define VEH_STATE(v)      (*(int *)((v) + 0x68))
#define VEH_DEPLOYED      1
#define VEH_SPEED(v)      (*(short *)((v) + 0xb8))       /* full_velocity */
#define VEH_AI_GOAL(v)    (*(int *)((v) + 0xe4))         /* SPOINT aiGoal (x = 0: none) */
#define VEH_NPC(v)        (*(int *)((v) + 0xe8))
#define VEH_HUMAN_AI(v)   (*(int *)((v) + 0xec))
#define VEH_DRIVER(v)     (*(BYTE **)((v) + 0x14c))

#define VEH_ANIM(v)       (*(BYTE **)((v) + 0x9c))       /* pAnimation: body ANIM, turret ANIM right after it */
#define ANIM_SIZE         0xe4
#define ANIM_TRANS(a)     (*(BYTE **)((a) + 0x08))       /* trans_table (army colours) */
#define VEH_ANIMS         2                              /* body, TURRETANIM */
#define UNIT_COLOR_TABLES ((BYTE **)0x4f75e8)            /* game binary unit_color_tables[army] */

#define SARGE_VEHICLE(s)  (*(BYTE **)((s) + 0x2d0))

#define LEG_UNITS(l)      (*(BYTE **)((l) + 4))
#define LEG_UNITS_END(l)  (*(BYTE **)((l) + 8))
#define LEG_VEH_COUNT(l)  (*(signed char *)((l) + 0x50))
#define LEG_VEH(l, i)     (*(BYTE **)((l) + 0x54 + 4 * (i)))
#define LEG_VEH_MAX       50                            /* ARMY_MAX_VEHICLES */
#define CAPTURE_SLOT      (LEG_VEH_MAX - 1)
#define UNIT_STRIDE       0x2c4
#define UNIT_MODE(u)      (*(int *)((u) + 0x70))
#define UNIT_IN_VEHICLE   2
#define UNIT_VEHICLE(u)   (*(BYTE **)((u) + 0xac))
#define UNIT_ON_MAP(u)    (UNIT_MODE(u) >= 3 && UNIT_MODE(u) <= 5)

#define ENTER_TILES       6          /* the Enter Vehicle key's reach (tiles) */
#define HOVER_REACH       96         /* px: an enemy vehicle shows the enter pointer this close to Sarge */
#define VSOUND_DELETE     6

typedef BYTE *(__fastcall *ArmyFn)(int army);
typedef CaptureTwoFn TwoFn;
typedef CaptureOneFn OneFn;
typedef void (__fastcall *SoundFn)(BYTE *v, int sound);
typedef DWORD (__fastcall *CentreFn)(BYTE *item);
typedef int (__fastcall *TileDistFn)(int a, int b);
typedef int (__fastcall *TadCreateFn)(int army, int type, BYTE *item, DWORD flags);
typedef void (__fastcall *TadDeleteFn)(void *tad);
typedef int (__fastcall *CanSeeFn)(BYTE *from, int tile);
typedef void (__fastcall *SeeFn)(int army, BYTE *item);
#define SARGE_OF          ((ArmyFn)0x4490d0)            /* SargePointer */
#define ARMY_RECORD       ((ArmyFn)0x41a160)            /* ArmyGetLegion */
#define VEHICLE_SOUND     ((SoundFn)0x4187b0)
#define ITEM_CENTRE       ((CentreFn)0x449d10)          /* ItemCenterPos: SPOINT packed x | y << 16 */
#define TILE_DISTANCE     ((TileDistFn)0x42b910)
#define TAD_CREATE        ((TadCreateFn)0x423500)       /* TadCreateObject, callee pops 8 */
#define TAD_DELETE        ((TadDeleteFn)0x423540)       /* TadDeleteObject */
#define CAN_SEE           ((CanSeeFn)0x44af10)          /* CanSeeFromItem (range box + line of sight) */
#define SEE               ((SeeFn)0x449a90)             /* See: item->seen[army] = now */
#define SARGE_IN_VEHICLE  ((TwoFn)0x448ae0)             /* hooked: runs capture_sarge_in */
#define SARGE_OUT_VEHICLE ((OneFn)0x448b50)             /* hooked: runs capture_sarge_out */
#define HANDLE_VEHICLE    ((OneFn)0x4296f0)

TwoFn capture_sarge_in_original, capture_unit_enter_original;
OneFn capture_sarge_out_original;

static struct {
    BYTE *v;            /* the vehicle Sarge took from another army, or NULL */
    BYTE *legion;       /* the player legion whose spare slot holds it */
    DWORD uid;          /* its own UID (restored on leaving) */
    DWORD uid_now;      /* the player's UID it carries now */
    BYTE *trans[VEH_ANIMS];   /* its own colour tables (restored on leaving) */
    int recoloured;
} g_cap;

/* The vehicle's body / turret ANIM, NULL when its animation block does not look valid. */
static BYTE *veh_anim(BYTE *v, int i)
{
    BYTE *a = VEH_ANIM(v);
    if (!a || IsBadWritePtr(a, ANIM_SIZE * VEH_ANIMS)) return NULL;
    return a + ANIM_SIZE * i;
}

/* Captured: draw it with the player's colours (8-bit table; the upscaled art follows the table). */
static void recolour_to_player(BYTE *v)
{
    /* 2026-11-20: off (user: "i don't want the vehicle to fully turn colors - just drawing the driver in there");
     * modern.c draws a green driver in the seat instead (capture_driver_seat). Kept for [gameplay]
     * captured_green=1. */
    static int on = -1;
    if (on < 0) on = GetPrivateProfileIntA("gameplay", "captured_green", 0, mod_ini_path());
    if (!on) return;
    BYTE *t = UNIT_COLOR_TABLES[PLAYER_ARMY];
    if (!t) return;
    for (int i = 0; i < VEH_ANIMS; i++) {
        BYTE *a = veh_anim(v, i);
        if (!a) continue;
        if (!g_cap.recoloured) g_cap.trans[i] = ANIM_TRANS(a);
        ANIM_TRANS(a) = t;
    }
    g_cap.recoloured = 1;
}

static void recolour_back(BYTE *v)
{
    if (!g_cap.recoloured) return;
    for (int i = 0; i < VEH_ANIMS; i++) {
        BYTE *a = veh_anim(v, i);
        if (a && ANIM_TRANS(a) == UNIT_COLOR_TABLES[PLAYER_ARMY]) ANIM_TRANS(a) = g_cap.trans[i];
    }
    g_cap.recoloured = 0;
}

int capture_allowed(void)
{
    BYTE *g = GAME;
    return g_cfg.drive_enemy_vehicles && g && GAME_TYPE(g) == 0;
}

BYTE *capture_vehicle(void) { return g_cap.v; }

static int has_passengers(BYTE *v)
{
    for (int a = 0; a < ARMIES; a++) {
        if (!LEGION_STATUS(a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg) continue;
        BYTE *u = LEG_UNITS(leg), *end = LEG_UNITS_END(leg);
        for (int g = 0; u && u < end && g < 256; u += UNIT_STRIDE, g++)
            if (UNIT_VEHICLE(u) == v && UNIT_MODE(u) == UNIT_IN_VEHICLE) return 1;
    }
    return 0;
}

/* A squad has this vehicle assigned (UNIT +0xac) and is alive on the map (mode 3..5) - its crew climbs in when the
 * AI decides to use it (UnitEnterVehicle 0x41d800). 2026-11-21, user: "sometimes there is a tank that I guess spawns
 * an enemy in it? I want those to register as full": it looked empty, Sarge took it, then its crew appeared. */
static int has_crew_assigned(BYTE *v)
{
    for (int a = 0; a < ARMIES; a++) {
        if (!LEGION_STATUS(a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg) continue;
        BYTE *u = LEG_UNITS(leg), *end = LEG_UNITS_END(leg);
        for (int g = 0; u && u < end && g < 256; u += UNIT_STRIDE, g++)
            if (UNIT_VEHICLE(u) == v && UNIT_MODE(u) >= 2 && UNIT_MODE(u) <= 5) return 1;
    }
    return 0;
}

static BYTE *player_legion(void) { return ARMY_RECORD(PLAYER_ARMY); }

/* 2026-11-20 CRASH FIX (user log, build 21:28:48: EXCEPTION at 0x4188f5 in VehicleFind, uid 04c20032, slot 49 empty):
 * the captured vehicle's player UID outlives the capture. A trooper caches its target's UID (TROOPER +0x2b8, copied
 * from the target item +0x30 each update at 0x43cc30) and its fire step 0x43c150 looks it up again with ItemFind
 * 0x427b80 -> VehicleFind 0x4188c0 = legion[army].slot[index] dereferenced WITHOUT a NULL check. The enemy soldier
 * that shot the captured jeep to death still held 04c20032 when VehicleDie bailed Sarge out and release() emptied
 * slot 49 -> NULL read. Rule now: once a capture used the spare slot it is never left empty - it holds this blank
 * stand-in (UID 0, matches no UID), so VehicleFind of any stale captured UID returns NULL like any other gone
 * item (its callers test for NULL). The game's own loops never look past the vehicle count, so nothing else reads
 * the slot (ArmyDelete / VehicleFree run over the count only: the stand-in is never freed). */
static DWORD g_slot_blank[0x200 / 4];
#define SLOT_BLANK ((BYTE *)g_slot_blank)

static void slot_clear(BYTE *leg, BYTE *v)
{
    if (leg && (LEG_VEH(leg, CAPTURE_SLOT) == v || !LEG_VEH(leg, CAPTURE_SLOT))) LEG_VEH(leg, CAPTURE_SLOT) = SLOT_BLANK;
}

static int slot_free(void)
{
    BYTE *leg = player_legion();
    if (!leg || LEG_VEH_COUNT(leg) >= CAPTURE_SLOT) return 0;
    BYTE *in = LEG_VEH(leg, CAPTURE_SLOT);
    return !in || in == SLOT_BLANK || in == g_cap.v;
}

/* May Sarge climb into v? Player vehicles: the game's rule (deployed, not AI driven). Other armies'
 * (option on, one player): deployed, alive, standing, no driver, no passengers, nothing captured. */
int capture_can_enter(BYTE *v)
{
    if (!v || VEH_STATE(v) != VEH_DEPLOYED || v == g_cap.v) return 0;   /* (the captured one carries the player's UID) */
    if (UID_ARMY(UID(v)) == PLAYER_ARMY) return VEH_NPC(v) == 0;
    if (!capture_allowed() || g_cap.v) return 0;
    if (UID_TYPE(UID(v)) != ITEM_VEHICLE || ITEM_HEALTH(v) <= 0 || VEH_DRIVER(v) || VEH_SPEED(v) != 0) return 0;
    return !has_passengers(v) && !has_crew_assigned(v) && slot_free();
}

/* The one-button mouse's enter pointer over another army's vehicle: only near Sarge (further away it stays a
 * target to shoot at). */
int capture_hover_ok(BYTE *v)
{
    BYTE *s = PLAYER_SARGE;
    /* 2026-11-20 (user: "can't enter the enemy vehicle unless I'm close to it. if its empty go straight to entering
     * it"): an EMPTY standing enemy vehicle is no threat - from any distance the click walks to it and climbs in
     * (controls.c S.enter, as for own vehicles); occupied / moving ones stay targets */
    return s && capture_can_enter(v);
}

static void capture(BYTE *s, BYTE *v)
{
    BYTE *leg = player_legion();
    if (!leg || !slot_free()) { mlog("capture: no spare slot in the player's legion - vehicle %p stays its army's", v); return; }
    DWORD uid = UID(v);
    int had_tad = ITEM_TAD(v) != NULL;
    if (had_tad) { TAD_DELETE(ITEM_TAD(v)); ITEM_TAD(v) = NULL; }           /* while the UID still names its army */
    g_cap.v = v;
    g_cap.legion = leg;
    g_cap.uid = uid;
    g_cap.uid_now = (uid & ~(UID_ARMY_MASK | UID_INDEX_MASK)) | ((DWORD)(PLAYER_ARMY + 1) << 26) | (CAPTURE_SLOT + 1);
    LEG_VEH(leg, CAPTURE_SLOT) = v;
    UID(v) = g_cap.uid_now;
    VEH_NPC(v) = 0;
    VEH_AI_GOAL(v) = 0;
    VEH_HUMAN_AI(v) = 0;
    if (had_tad) TAD_CREATE(PLAYER_ARMY, ITEM_VEHICLE, v, 0);
    recolour_to_player(v);
    mlog("capture: vehicle %p colours %p,%p -> player table %p", v, g_cap.trans[0], g_cap.trans[1], UNIT_COLOR_TABLES[PLAYER_ARMY]);
    mlog("capture: Sarge took army %d vehicle %p (class %d) uid %08lx -> %08lx (player slot %d)%s",
         UID_ARMY(uid), v, (int)((uid >> 16) & 0x3f) - 1, uid, g_cap.uid_now, CAPTURE_SLOT, had_tad ? "" : ", no TAD");
}

static void release(void)
{
    BYTE *v = g_cap.v;
    if (!v) return;
    if (UID(v) == g_cap.uid_now) {
        int had_tad = ITEM_TAD(v) != NULL;
        if (had_tad) { TAD_DELETE(ITEM_TAD(v)); ITEM_TAD(v) = NULL; }
        UID(v) = g_cap.uid;
        recolour_back(v);
        if (had_tad) TAD_CREATE(UID_ARMY(g_cap.uid), ITEM_VEHICLE, v, 0);
        mlog("capture: Sarge left vehicle %p - back to army %d (uid %08lx, health %d, parked)", v, UID_ARMY(g_cap.uid), g_cap.uid,
             ITEM_HEALTH(v));
    }
    slot_clear(g_cap.legion, v);                         /* never empty: stale UIDs of it must find no vehicle, not crash */
    memset(&g_cap, 0, sizeof g_cap);
}

void __fastcall capture_sarge_in(BYTE *s, BYTE *v)
{
    /* 2026-11-20 (user: "if the vehicle is occupied, sarge can get into it with repeated clicking (happens when
     * shooting)"): every way in (click, F key, the game's own enter) ends here - an occupied vehicle (another
     * driver, troopers inside) or one moving under AI control is refused */
    if (s && v && !IsBadReadPtr(v, 0x150) && SARGE_VEHICLE(s) != v &&
        ((VEH_DRIVER(v) && VEH_DRIVER(v) != s) || has_passengers(v) || (VEH_NPC(v) && VEH_SPEED(v) != 0))) {
        static DWORD said;
        if (GetTickCount() - said > 1000) { said = GetTickCount(); mlog("capture: Sarge kept out of occupied vehicle %p (driver %p)", v, VEH_DRIVER(v)); }
        return;
    }
    capture_sarge_in_original(s, v);
    if (s && v && SARGE_VEHICLE(s) == v && UID_ARMY(UID(v)) != UID_ARMY(UID(s)) && !g_cap.v && capture_allowed())
        capture(s, v);
}

/* ---- vehicle_bailout (2026-11-20, user: "if the vehicle gets destroyed - kick sarge out. dont kill him")
 * The game already bails him out: VehicleDie 0x4185f0 (game binary, called from VehicleDamage 0x418460 at
 * 0x418540) sets state VEH_DYING + health 0, moves the vehicle off the map and calls SargeOutVehicle (0x418707)
 * for the Sarge of the vehicle's (UID) army - true for the captured one too, its UID is the player's then.
 * SargeOutVehicle 0x448b50 puts him on the nearest tile the walk rule allows (ItemFanOut 0x44a5e0, 8 tiles,
 * OutOfVehicle 0x448c80) and gives him his TAD back - but with no protection: he appears in the middle of the
 * wreck's own fireballs: ~0.2 s after VehicleDie six DMGTYPE_OTHER_EXPLOSION (0x45) hits of 20 land around the
 * wreck (Wine 2026-11-20, Desert 1 Tan jeep: 1356..1435, 314..396 round the jeep at 1416,392; Sarge put at 1392,368)
 * = 120 damage against his 60 - he dies right after the bail-out unless the mission-start invulnerability is still
 * running (one unprotected Wine run: health 53 -> 0 at +0.5 s).
 * Now ([gameplay] vehicle_bailout=1): after the game's bail-out from a wreck he loses BAILOUT_HURT_PCT of his
 * full health (never below 1 - "maybe hurt a little") and is invulnerable for BAILOUT_SAFE_MS through the
 * game's own invulnerableTimer (SARGE +0x2dc, ms; SargeDamage 0x447fc0 ignores all damage while it runs, the
 * Sarge update 0x446c95 counts it down by the frame time) - time to run from the wreck. Off: the game's way. */
#define VEH_DYING          2                             /* VehicleDie: state 2 (VEHICLE_DYING), health 0, then SargeOutVehicle */
#define SARGE_STATE(s)     (*(int *)((s) + 0x68))
#define SARGE_MAX_HEALTH(s) (*(short *)((s) + 0x1e))
#define SARGE_INVULN_MS(s) (*(int *)((s) + 0x2dc))       /* invulnerableTimer */
#define BAILOUT_HURT_PCT   10
#define BAILOUT_SAFE_MS    2500

static struct { BYTE *s; DWORD until; int hp, x, y, st; } g_watch;   /* log >= 2: Sarge for a few seconds after a bail-out */

static void watch_frame(void)
{
    BYTE *s = g_watch.s;
    if (!s) return;
    if (s != PLAYER_SARGE || GetTickCount() > g_watch.until) { g_watch.s = NULL; return; }
    int hp = ITEM_HEALTH(s), x = *(short *)(s + 4), y = *(short *)(s + 6), st = SARGE_STATE(s);
    if (hp != g_watch.hp || st != g_watch.st || x != g_watch.x || y != g_watch.y)
        mlog("bailout: Sarge health %d state %d at %d,%d (invulnerable %d ms)", hp, st, x, y, SARGE_INVULN_MS(s));
    g_watch.hp = hp; g_watch.x = x; g_watch.y = y; g_watch.st = st;
}

static void bail_out(BYTE *s, BYTE *v)
{
    int hp = ITEM_HEALTH(s), max = SARGE_MAX_HEALTH(s);
    if (g_cfg.vehicle_bailout && hp > 0) {
        int hurt = max * BAILOUT_HURT_PCT / 100;
        if (hurt < 1) hurt = 1;
        ITEM_HEALTH(s) = (short)(hp - hurt < 1 ? 1 : hp - hurt);
        if (SARGE_INVULN_MS(s) < BAILOUT_SAFE_MS) SARGE_INVULN_MS(s) = BAILOUT_SAFE_MS;
    }
    mlog("bailout: vehicle %p destroyed with Sarge inside - out at %d,%d health %d -> %d/%d%s", v, *(short *)(s + 4), *(short *)(s + 6),
         hp, ITEM_HEALTH(s), max, g_cfg.vehicle_bailout ? ", invulnerable for a moment" : " (vehicle_bailout off)");
    if (g_cfg.log >= 2) { g_watch.s = s; g_watch.until = GetTickCount() + 6000; g_watch.hp = -1; }
}

void __fastcall capture_sarge_out(BYTE *s)
{
    BYTE *v = s ? SARGE_VEHICLE(s) : NULL;
    int wrecked = v && VEH_STATE(v) == VEH_DYING;
    capture_sarge_out_original(s);
    if (v && v == g_cap.v && SARGE_VEHICLE(s) != v) release();
    if (wrecked && SARGE_VEHICLE(s) != v) bail_out(s, v);
}

void __fastcall capture_unit_enter(BYTE *unit, BYTE *v)
{
    if (v && v == g_cap.v) {
        static BYTE *last;
        if (unit != last) { last = unit; mlog("capture: unit %p not let into Sarge's vehicle %p", unit, v); }
        return;
    }
    capture_unit_enter_original(unit, v);
}

void __fastcall capture_handle_vehicle(BYTE *v)
{
    if (v && v == g_cap.v) return;                       /* no AI control while Sarge drives it */
    HANDLE_VEHICLE(v);
}

/* FUN_00418910: the Enter Vehicle key. */
void __cdecl capture_enter_key(void)
{
    BYTE *s = SARGE_OF(PLAYER_ARMY);
    if (!s || COMMAND_CURSOR) return;
    BYTE *in = SARGE_VEHICLE(s);
    if (in) {
        if (VEH_SPEED(in) == 0) { VEHICLE_SOUND(in, VSOUND_DELETE); SARGE_OUT_VEHICLE(s); }
        return;
    }
    BYTE *best = NULL;
    int best_d = 10000;
    for (int a = 0; a < ARMIES; a++) {
        if (a != PLAYER_ARMY && (!capture_allowed() || !LEGION_STATUS(a))) continue;
        BYTE *leg = ARMY_RECORD(a);
        int n = leg ? LEG_VEH_COUNT(leg) : 0;
        for (int i = 0; i < n && i < LEG_VEH_MAX; i++) {
            BYTE *v = LEG_VEH(leg, i);
            if (!capture_can_enter(v)) continue;
            DWORD c = ITEM_CENTRE(v);
            int tile = ((short)(c >> 16) >> 4) * MAP_TILES_W + ((short)c >> 4);
            int d = TILE_DISTANCE(tile, ITEM_TILE(s));
            if (d > ENTER_TILES || d >= best_d) continue;
            best_d = d;
            best = v;
        }
    }
    if (best) SARGE_IN_VEHICLE(s, best);
}

/* Enemy seers that have the captured vehicle in sight mark it seen (their targeting needs it). */
static void update_seen(BYTE *v)
{
    int tile = ITEM_TILE(v);
    if (!tile) return;
    for (int a = 0; a < ARMIES; a++) {
        if (a == PLAYER_ARMY || !LEGION_STATUS(a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg) continue;
        int seen = 0;
        BYTE *sg = SARGE_OF(a);
        if (sg && ITEM_TILE(sg) && ITEM_HEALTH(sg) > 0 && !SARGE_VEHICLE(sg) && CAN_SEE(sg, tile)) seen = 1;
        BYTE *u = LEG_UNITS(leg), *end = LEG_UNITS_END(leg);
        for (int g = 0; !seen && u && u < end && g < 256; u += UNIT_STRIDE, g++)
            if (UNIT_ON_MAP(u) && !UNIT_VEHICLE(u) && ITEM_TILE(u) && ITEM_HEALTH(u) > 0 && CAN_SEE(u, tile)) seen = 1;
        int n = LEG_VEH_COUNT(leg);
        for (int i = 0; !seen && i < n && i < LEG_VEH_MAX; i++) {
            BYTE *w = LEG_VEH(leg, i);
            if (w && w != v && VEH_STATE(w) == VEH_DEPLOYED && ITEM_TILE(w) && ITEM_HEALTH(w) > 0 && CAN_SEE(w, tile)) seen = 1;
        }
        if (seen) SEE(a, v);
    }
}

/* Once per gameplay frame (gameplay.c). */

void capture_frame(void)
{
    watch_frame();
    if (g_cfg.log >= 2 && capture_allowed()) {           /* every 10 s: other armies' vehicles (staging tests) */
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last > 10000) {
            last = now;
            for (int a = 1; a < ARMIES; a++) {
                if (!LEGION_STATUS(a)) continue;
                BYTE *leg = ARMY_RECORD(a);
                int n = leg ? LEG_VEH_COUNT(leg) : 0;
                for (int i = 0; i < n && i < LEG_VEH_MAX; i++) {
                    BYTE *v = LEG_VEH(leg, i);
                    if (!v || VEH_STATE(v) != VEH_DEPLOYED) continue;
                    mlog("capture: army %d vehicle %p class %d at %d,%d health %d npc %d speed %d driver %p passengers %d -> %s",
                         a, v, (int)((UID(v) >> 16) & 0x3f) - 1, *(short *)(v + 4), *(short *)(v + 6), ITEM_HEALTH(v), VEH_NPC(v),
                         VEH_SPEED(v), VEH_DRIVER(v), has_passengers(v), capture_can_enter(v) ? "enterable" : "not enterable");
                }
            }
        }
    }
    if (!g_cap.v) return;
    BYTE *s = PLAYER_SARGE;
    if (!s || SARGE_VEHICLE(s) != g_cap.v || UID(g_cap.v) != g_cap.uid_now) {
        /* left without SargeOutVehicle (mission end / restart: the vehicle may be freed) - forget it */
        mlog("capture: captured vehicle %p gone (new mission?) - forgotten", g_cap.v);
        slot_clear(g_cap.legion, g_cap.v);
        memset(&g_cap, 0, sizeof g_cap);
        return;
    }
    BYTE *v = g_cap.v;
    VEH_NPC(v) = 0;                                      /* a script order may have switched its AI on */
    VEH_AI_GOAL(v) = 0;
    recolour_to_player(v);                               /* (kept: nothing in the game sets it again, cheap) */
    update_seen(v);
}

/* modern.c (2026-11-20, user: "show if vehicle is not empty on cursor"): another army's vehicle under the world point
 * (wx, wy) within HOVER_REACH of Sarge that he cannot take - why not ("Occupied", "Moving", ...), its position in
 * *vx, *vy; NULL = none there or it can be taken. */
const char *capture_hover_note(int wx, int wy, int *vx, int *vy)
{
    BYTE *s = PLAYER_SARGE;
    if (!s || !capture_allowed() || g_cap.v) return NULL;
    for (int a = 1; a < ARMIES; a++) {
        if (!LEGION_STATUS(a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg) continue;
        int n = *(signed char *)(leg + 0x50);
        for (int i = 0; i < n && i < 50; i++) {
            BYTE *v = *(BYTE **)(leg + 0x54 + 4 * i);
            if (!v || IsBadReadPtr(v, 0x150)) continue;
            int x = *(short *)(v + 4), y = *(short *)(v + 6);
            if (abs(wx - x) > 22 || abs(wy - y) > 22) continue;
            int dx = x - *(short *)(s + 4), dy = y - *(short *)(s + 6);
            if (dx * dx + dy * dy > HOVER_REACH * HOVER_REACH * 4) continue;
            *vx = x; *vy = y;
            if (ITEM_HEALTH(v) <= 0) return "Wrecked";
            if (VEH_STATE(v) != VEH_DEPLOYED) return NULL;
            if (VEH_DRIVER(v) || has_passengers(v)) return "Occupied";
            if (has_crew_assigned(v)) return "Occupied - crew nearby";
            if (VEH_NPC(v) && VEH_SPEED(v) != 0) return "Occupied - moving";
            if (VEH_SPEED(v) != 0) return "Moving";
            if (!slot_free()) return "Cannot take";
            if (dx * dx + dy * dy > HOVER_REACH * HOVER_REACH) return "Get closer";
            return NULL;
        }
    }
    return NULL;
}

/* modern.c: the captured vehicle Sarge drives - its world position and hull facing (0..255), for the driver figure */
int capture_driver_seat(int *wx, int *wy, int *facing)
{
    BYTE *v = g_cap.v;
    if (!v || IsBadReadPtr(v, 0x150)) return 0;
    *wx = *(short *)(v + 4); *wy = *(short *)(v + 6); *facing = *(short *)(v + 8) & 255;
    return 1;
}

/* ---- VehicleFind rewrite (2026-11-21 CRASH, user log build 05:38:04: EXCEPTION 0xc0000005 at 0x4188f5, ecx 0x31,
 * edx 0 - a stale captured UID 04c30032 looked up ~9 s after the captured jeep was destroyed with Sarge inside; slot
 * 49 was NULL, not the blank stand-in: the game's own clean-up of the wreck emptied the slot it was in at death).
 * FUN_004188c0 VehicleFind, __fastcall ecx = UID, returns the VEHICLE* or NULL; callers: ItemFind 0x427b80 and others.
 * Original: army = (uid >> 26 & 7) - 1; if the army is in play (0x4f91b0 + army * 0x19e8 = legion +0x1990) the slot
 * legion +0x54 + (index - 1) * 4 is read and its UID compared - WITHOUT checking the slot for NULL.
 * Kept: the same lookup and result. Changed: an empty / unreadable slot, an index outside 1..50 or an army outside
 * 0..3 return NULL (= "no such vehicle", which every caller handles) instead of crashing. Redirected with hook_jmp. */
#define VEHICLE_SLOTS 50
BYTE *__fastcall capture_vehicle_find(DWORD uid)
{
    int army = (int)((uid >> 26) & 7) - 1, idx = (int)(uid & 0xffff) - 1;
    if (army < 0 || army >= ARMIES || idx < 0 || idx >= VEHICLE_SLOTS || !LEGION_STATUS(army)) return NULL;
    BYTE *leg = ARMY_RECORD(army);
    if (!leg) return NULL;
    BYTE *v = LEG_VEH(leg, idx);
    if (!v || IsBadReadPtr(v, 4) || UID(v) != uid) return NULL;
    return v;
}

/* ---- army file (2026-11-21 CRASH, user: "mission 6 crashes after I steal a tank"; EXCEPTION at 0x417337 in
 * FUN_004196e0 on Play Again). Reproduced in Wine: Sarge drove a captured Tan tank (player UID 04c30032, slot 49)
 * when mission 6 was won -> the game wrote the army file (<player>.asf, FUN_0046a970 -> FUN_0041a3e0) with every
 * legion's vehicles BY UID - army 1's list held the tank under the borrowed player UID. Mission 7 continues on the
 * same map; its Play Again reloads the map and the army file (FUN_0041a970, mission index > 0) and looks each record
 * up with ItemFind: 04c30032 = player slot 49, empty after the reload -> NULL -> written through (0x417337).
 * FUN_0041a3e0 (army file writer, __fastcall ecx = FILE *, caller FUN_0046a970 at 0x46aa3d): wrapped. While it writes,
 * the captured vehicle carries its own UID again (the file names it as its army's vehicle) and Sarge is written on
 * foot: with his vehicle link the load puts him back in and capture.c takes the vehicle again BEFORE its army's records
 * are placed (Wine: its record then not found, crash at 0x416c26). Afterwards both are put back (Sarge still drives).
 * Reload result (Wine): the tank back as army 1's at its saved spot, Sarge next to it on foot. Kept: everything else. */
ArmySaveFn capture_army_save_original;
void __fastcall capture_army_save(void *file)
{
    BYTE *v = g_cap.v;
    int swap = v && UID(v) == g_cap.uid_now;
    BYTE *s = swap ? SARGE_OF(PLAYER_ARMY) : NULL;
    if (s && SARGE_VEHICLE(s) != v) s = NULL;
    if (swap) {
        UID(v) = g_cap.uid;
        if (s) SARGE_VEHICLE(s) = NULL;
        mlog("capture: army file written while Sarge drives vehicle %p - saved as its army's (uid %08lx), Sarge on foot", v, g_cap.uid);
    }
    capture_army_save_original(file);
    if (swap) { UID(v) = g_cap.uid_now; if (s) SARGE_VEHICLE(s) = v; }
}

/* savestate.c: the captured vehicle (its legion slot, colours) is part of the mission state */
void capture_savestate(void) { SAVESTATE(g_cap); SAVESTATE(g_slot_blank); SAVESTATE(g_watch); }
