/* ============================================================================================
 * deploy.c - placing an army's records on the map (mission start, the army file of a follow-on mission, Play Again)
 *
 * 2026-11-21 CRASH (user, Windows, build 06:30:47; user: "mission 6 crashes after I steal a tank"): EXCEPTION
 * 0xc0000005 at 0x417337 (FUN_00417330, ecx = vehicle = NULL) from 0x419842 in FUN_004196e0, on Play Again; the
 * caller's ebx = FILE *, ebp = 1: the army file load FUN_0041a970, army 1. Reproduced in Wine (mission 6 won while
 * Sarge drove a captured Tan tank, mission 7 on the same map, Sarge dies, Play Again): the army file held the tank
 * under the borrowed player UID 04c30032, which nothing has after the reload -> ItemFind NULL -> crash here, and
 * once this is skipped a second one at 0x41ab65 (FUN_0041ab30, same record). Cause and fix: capture.c
 * capture_army_save (the file names the captured vehicle by its own UID). These two rewrites keep the loader from
 * crashing on any record whose item is gone (army files written by older builds still hold 04c30032).
 *
 * FUN_004196e0 (range 0x4196e0..0x41990c). Callers: FUN_004194f0 (call 0x419580: one army's force of the mission,
 * full = 0, records set up by the mission load FUN_00419910) and FUN_0041a970 (call 0x41aae1: one army's records
 * from the army file <player>.asf, full = 1, items by UID). __fastcall: ecx = records, edx = count, stack: full
 * (callee pops 4). Returns 1 when a Sarge record was placed.
 * RECORD (0x6c bytes = 0x1b ints; writer FUN_0041a3e0): +0x00 type (0x8000 = Sarge / vehicle; type & 0x7fff: 5 =
 * Sarge, otherwise the vehicle class; without 0x8000: a unit), +0x04 / +0x06 x, y (world px), +0x08 item, +0x0c UID
 * (when +0x08 is 0: the item is looked up with ItemFind 0x427b80), +0x10 facing (byte), +0x11 deploy now (byte),
 * +0x12 unit order (byte), +0x54 vehicle weapon value (signed byte), +0x58 health (short), +0x5c UID of a linked
 * item (Sarge: his vehicle), +0x64 value for an item not on the map, +0x68 "not on the map" (byte: dead / dying).
 * Kept: everything - the order, position rule (an item given by pointer is put on its tile centre, one found by UID at
 * the exact spot), every callee and argument, "Found multiple Sarges", the name strings (0x44a030 + sprintf 0x471090,
 * built and not used, as the original). Changed: a record whose item is NULL (ItemFind found no item with that UID)
 * is skipped and logged; the original wrote through the NULL pointer (0x417337, 0x41979c, 0x419876, ...).
 * Redirected with hook_jmp (patches.c).
 * ============================================================================================ */
#include "common.h"

#define REC_INTS          0x1b
#define REC_TYPE(r)       ((r)[0])
#define REC_X(r)          (*(short *)((BYTE *)(r) + 0x04))
#define REC_Y(r)          (*(short *)((BYTE *)(r) + 0x06))
#define REC_ITEM(r)       (*(BYTE **)((BYTE *)(r) + 0x08))
#define REC_UID(r)        (*(DWORD *)((BYTE *)(r) + 0x0c))
#define REC_FACING(r)     (*((BYTE *)(r) + 0x10))
#define REC_DEPLOY(r)     (*((BYTE *)(r) + 0x11))
#define REC_ORDER(r)      (*((BYTE *)(r) + 0x12))
#define REC_HEALTH(r)     (*(short *)((BYTE *)(r) + 0x58))
#define REC_LINK_UID(r)   (*(DWORD *)((BYTE *)(r) + 0x5c))
#define REC_PARKED_VAL(r) (*(DWORD *)((BYTE *)(r) + 0x64))
#define REC_PARKED(r)     (*((BYTE *)(r) + 0x68))
#define REC_BIG           0x8000      /* Sarge or vehicle */
#define REC_KIND_MASK     0x7fff
#define REC_SARGE         5

#define ITEM_HEALTH(it)   (*(short *)((it) + 0x1c))

typedef struct { short x, y; } SPoint;
typedef BYTE *(__fastcall *FindFn)(DWORD uid);
typedef const char *(__fastcall *NameFn)(BYTE *item);
typedef void (__fastcall *PosFn)(BYTE *item, SPoint *pos, int facing);
typedef void (__fastcall *SargeDeployFn)(BYTE *item, SPoint *pos, int facing, BYTE *link);
typedef void (__fastcall *SargeHealthFn)(BYTE *item, int on);
typedef void (__fastcall *OrderFn)(BYTE *item, int order);
typedef void (__fastcall *UnitDeployFn)(BYTE *item, int facing, DWORD pos, int how, BYTE *link);   /* pos: SPOINT x | y << 16 */
typedef void (__cdecl *TraceFn)(const char *msg);
typedef int (__cdecl *SprintfFn)(char *out, const char *fmt, ...);

#define ITEM_FIND          ((FindFn)0x427b80)
#define ITEM_NAME          ((NameFn)0x44a030)
#define GAME_SPRINTF       ((SprintfFn)0x471090)
#define NAME_FMT           ((const char *)0x4b2840)          /* "%s" */
#define TRACE              ((TraceFn)0x4236d0)
#define MULTI_SARGE_MSG    ((const char *)0x4b2d54)          /* "Found multiple Sarges" */
#define SARGE_SET_POS      ((PosFn)0x448a70)                 /* SARGE +0x2c8 pos, +0x2cc facing */
#define SARGE_DEPLOY       ((SargeDeployFn)0x4487e0)
#define SARGE_HEALTH_SET   ((SargeHealthFn)0x448720)
#define VEHICLE_SET_POS    ((PosFn)0x417330)                 /* VEHICLE +0x6c pos, +0x70 facing */
#define VEHICLE_DEPLOY     ((PosFn)0x417010)                 /* VehicleInitialDeploy */
#define UNIT_SET_ORDER     ((OrderFn)0x41ace0)
#define UNIT_SET_POS       ((PosFn)0x41ad90)                 /* UNIT +0x8a pos, +0x8e facing */
#define UNIT_DEPLOY        ((UnitDeployFn)0x41ae60)
#define UNIT_DEPLOY_HOW    2

#define SARGE_PARKED_VAL(s)   (*(DWORD *)((s) + 0x300))
#define VEH_STATE(v)          (*(int *)((v) + 0x68))
#define VEH_PARKED_STATE      3
#define VEH_PARKED_VAL(v)     (*(DWORD *)((v) + 0x158))
#define UNIT_MODE(u)          (*(int *)((u) + 0x70))
#define UNIT_PARKED_VAL(u)    (*(DWORD *)((u) + 0x2b6))

int __fastcall deploy_force_records(DWORD *rec, int count, int full)
{
    int sarge_placed = 0;
    if (g_cfg.log >= 2) mlog("deploy: %d records at %p%s", count, rec, full ? " (army file)" : "");
    for (; count > 0; count--, rec += REC_INTS) {
        BYTE *link = NULL;
        SPoint pos;
        if (REC_ITEM(rec)) {
            pos.x = (short)((REC_X(rec) & 0xfff0) + 8);
            pos.y = (short)((REC_Y(rec) & 0xfff0) + 8);
        } else {
            if (!REC_UID(rec)) continue;
            REC_ITEM(rec) = ITEM_FIND(REC_UID(rec));
            pos.x = REC_X(rec);
            pos.y = REC_Y(rec);
        }
        BYTE *it = REC_ITEM(rec);
        int facing = REC_FACING(rec);
        if (g_cfg.log >= 2)
            mlog("deploy: record type %04lx uid %08lx item %p at %d,%d%s%s", REC_TYPE(rec) & 0xffff, REC_UID(rec), it, pos.x, pos.y,
                 REC_DEPLOY(rec) ? " deploy" : "", REC_PARKED(rec) ? " parked" : "");
        if (!it) {
            /* the original wrote through NULL here; the item with this UID is gone - leave the record out */
            mlog("deploy: record type %04lx uid %08lx at %d,%d: no item with that UID - skipped (the game would crash here)",
                 REC_TYPE(rec) & 0xffff, REC_UID(rec), pos.x, pos.y);
            continue;
        }
        char name[100];
        if (REC_TYPE(rec) & REC_BIG) {
            if ((REC_TYPE(rec) & REC_KIND_MASK) == REC_SARGE) {
                if (sarge_placed) { TRACE(MULTI_SARGE_MSG); continue; }
                SARGE_SET_POS(it, &pos, facing);
                if (REC_PARKED(rec)) { SARGE_PARKED_VAL(it) = REC_PARKED_VAL(rec); continue; }
                if (full && REC_LINK_UID(rec)) {
                    link = ITEM_FIND(REC_LINK_UID(rec));
                    GAME_SPRINTF(name, NAME_FMT, ITEM_NAME(it));
                }
                if (REC_DEPLOY(rec)) SARGE_DEPLOY(it, &pos, facing, link);
                if (full) { ITEM_HEALTH(it) = REC_HEALTH(rec); SARGE_HEALTH_SET(it, 1); }
                sarge_placed = 1;
            } else {
                if (REC_PARKED(rec)) { VEH_STATE(it) = VEH_PARKED_STATE; VEH_PARKED_VAL(it) = REC_PARKED_VAL(rec); continue; }
                VEHICLE_SET_POS(it, &pos, facing);
                if (REC_DEPLOY(rec)) VEHICLE_DEPLOY(it, &pos, facing);
                if (full) ITEM_HEALTH(it) = REC_HEALTH(rec);
            }
        } else {
            if (REC_PARKED(rec)) { UNIT_MODE(it) = 0; UNIT_PARKED_VAL(it) = REC_PARKED_VAL(rec); continue; }
            UNIT_SET_ORDER(it, REC_ORDER(rec));
            UNIT_SET_POS(it, &pos, facing);
            if (full && REC_LINK_UID(rec)) {
                link = ITEM_FIND(REC_LINK_UID(rec));
                GAME_SPRINTF(name, NAME_FMT, ITEM_NAME(it));
            }
            if (REC_DEPLOY(rec)) UNIT_DEPLOY(it, facing, (DWORD)(unsigned short)pos.x | (DWORD)(unsigned short)pos.y << 16, UNIT_DEPLOY_HOW, link);
        }
    }
    return sarge_placed;
}

/* FUN_0041ab30 (range 0x41ab30..0x41ab76), caller FUN_0041a970 (the army file load, right after FUN_004196e0(1)):
 * __fastcall ecx = records, edx = count, one stack argument (not used; callee pops 4). For every record: the item is
 * looked up by UID when +0x08 is 0, and a vehicle's weapon gets the saved value (VEHICLE +0x20 weapon, WEAPON +0x0c
 * short = (signed char) record +0x54).
 * Kept: the lookup and the write. Changed: a vehicle record whose item is NULL is skipped (the original read
 * NULL +0x20 at 0x41ab65 - the second crash after FUN_004196e0 skipped the same record). Redirected with hook_jmp. */
#define REC_WEAPON_VAL(r) (*(signed char *)((BYTE *)(r) + 0x54))
#define VEH_WEAPON(v)     (*(BYTE **)((v) + 0x20))
#define WEAPON_VAL(w)     (*(short *)((w) + 0x0c))
void __fastcall deploy_vehicle_weapons(DWORD *rec, int count, void *unused)
{
    for (; count > 0; count--, rec += REC_INTS) {
        if (!REC_ITEM(rec)) REC_ITEM(rec) = ITEM_FIND(REC_UID(rec));
        if (!(REC_TYPE(rec) & REC_BIG) || (REC_TYPE(rec) & REC_KIND_MASK) == REC_SARGE) continue;
        BYTE *v = REC_ITEM(rec);
        if (!v || !VEH_WEAPON(v)) continue;          /* gone (logged by deploy_force_records) */
        WEAPON_VAL(VEH_WEAPON(v)) = REC_WEAPON_VAL(rec);
    }
}
