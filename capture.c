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
 *   On leaving (SargeOutVehicle, also from VehicleDie / VehicleUndeploy): UID, legion slot and TAD go back to
 *   its army; the AI stays off - an abandoned vehicle stays parked (Sarge can come back to it).
 *
 * Rewrites / wraps:
 *   FUN_00418910 Enter Vehicle key (VehicleSargeFlip, game binary), key-table handler, __cdecl no args.
 *     Kept: in a vehicle -> get out when it stands still (sound 0x4187b0 edx 6, SargeOutVehicle); command
 *     cursor (0x4b8a88) blocks; nearest within 6 tiles (ItemCenterPos 0x449d10 -> tile, TileDistance
 *     0x42b910), own vehicles deployed and not npc. Added: other armies' vehicles that capture_can_enter.
 *   FUN_00448ae0 SargeInVehicle (__fastcall ecx Sarge, edx vehicle): wrapped, then capture.
 *   FUN_00448b50 SargeOutVehicle (__fastcall ecx Sarge): wrapped, then release.
 *   FUN_0041d800 UnitEnterVehicle (__fastcall ecx unit, edx vehicle): wrapped, refused for the captured one.
 *   call 0x4296b6 -> HandleVehicle 0x4296f0 (__fastcall ecx vehicle): skipped for the captured one.
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
} g_cap;

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

static BYTE *player_legion(void) { return ARMY_RECORD(PLAYER_ARMY); }

static int slot_free(void)
{
    BYTE *leg = player_legion();
    return leg && LEG_VEH_COUNT(leg) < CAPTURE_SLOT && (!LEG_VEH(leg, CAPTURE_SLOT) || LEG_VEH(leg, CAPTURE_SLOT) == g_cap.v);
}

/* May Sarge climb into v? Player vehicles: the game's rule (deployed, not AI driven). Other armies'
 * (option on, one player): deployed, alive, standing, no driver, no passengers, nothing captured. */
int capture_can_enter(BYTE *v)
{
    if (!v || VEH_STATE(v) != VEH_DEPLOYED || v == g_cap.v) return 0;   /* (the captured one carries the player's UID) */
    if (UID_ARMY(UID(v)) == PLAYER_ARMY) return VEH_NPC(v) == 0;
    if (!capture_allowed() || g_cap.v) return 0;
    if (UID_TYPE(UID(v)) != ITEM_VEHICLE || ITEM_HEALTH(v) <= 0 || VEH_DRIVER(v) || VEH_SPEED(v) != 0) return 0;
    return !has_passengers(v) && slot_free();
}

/* The one-button mouse's enter pointer over another army's vehicle: only near Sarge (further away it stays a
 * target to shoot at). */
int capture_hover_ok(BYTE *v)
{
    BYTE *s = PLAYER_SARGE;
    if (!s || !capture_can_enter(v)) return 0;
    int dx = *(short *)(v + 4) - *(short *)(s + 4), dy = *(short *)(v + 6) - *(short *)(s + 6);
    return dx * dx + dy * dy <= HOVER_REACH * HOVER_REACH;
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
        if (had_tad) TAD_CREATE(UID_ARMY(g_cap.uid), ITEM_VEHICLE, v, 0);
        mlog("capture: Sarge left vehicle %p - back to army %d (uid %08lx, health %d, parked)", v, UID_ARMY(g_cap.uid), g_cap.uid,
             ITEM_HEALTH(v));
    }
    if (g_cap.legion && LEG_VEH(g_cap.legion, CAPTURE_SLOT) == v) LEG_VEH(g_cap.legion, CAPTURE_SLOT) = NULL;
    memset(&g_cap, 0, sizeof g_cap);
}

void __fastcall capture_sarge_in(BYTE *s, BYTE *v)
{
    capture_sarge_in_original(s, v);
    if (s && v && SARGE_VEHICLE(s) == v && UID_ARMY(UID(v)) != UID_ARMY(UID(s)) && !g_cap.v && capture_allowed())
        capture(s, v);
}

void __fastcall capture_sarge_out(BYTE *s)
{
    BYTE *v = s ? SARGE_VEHICLE(s) : NULL;
    capture_sarge_out_original(s);
    if (v && v == g_cap.v && SARGE_VEHICLE(s) != v) release();
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
        if (g_cap.legion && LEG_VEH(g_cap.legion, CAPTURE_SLOT) == g_cap.v) LEG_VEH(g_cap.legion, CAPTURE_SLOT) = NULL;
        memset(&g_cap, 0, sizeof g_cap);
        return;
    }
    BYTE *v = g_cap.v;
    VEH_NPC(v) = 0;                                      /* a script order may have switched its AI on */
    VEH_AI_GOAL(v) = 0;
    update_seen(v);
}
