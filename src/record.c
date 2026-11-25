/* ============================================================================================
 * record.c - automatic manual-test session recorder & semantic replayer
 *
 * Recording ([debug] record_runs):
 *   - Dev builds:     default ON  (1) when omitted from armymen-mod.ini
 *   - Release builds: default OFF (0) when built with -DARMYMEN_PORTABLE=1
 *   - Always overridable via armymen-mod.ini: [debug] record_runs=1 (or 0)
 *
 * Writes one newline-delimited JSON (.jsonl) file per launch under <game>\<record_dir>\
 * (default "recordings\run_YYYYMMDD_HHMMSS.jsonl"), keeping the newest [debug] record_keep
 * files (default 30). Captures everything needed to rerun or convert a manual session into
 * an automated test case (scripts/rec2test.py):
 *   - session header (build info + active mod config) & mid-run setting changes
 *   - mission starts/runs, quicksaves/quickloads, cheats, bookmarks (Ctrl+Shift+B), crashes
 *   - Sarge movement breadcrumbs (world px, facing, stance, sprint, HP, vehicle) & walk targets
 *   - weapon pickups/ammo merges, weapon switches/drops, weapon firing & mine laying
 *   - enemy kills (both soldiers and vehicles: UID, army, class/name, world pos, combat vs cheat)
 *   - vehicle enter/exit/capture/bailout, squad/air orders, medkit pickups/uses
 *   - UI/menu/HUD clicks (game pixels g:x,y + context) and non-movement key presses
 *
 * Semantic Replayer ([debug] replay=<path.jsonl>):
 *   - Always OFF unless [debug] replay is explicitly set in armymen-mod.ini.
 *   - Replays a recorded mission segment using the mod's real walk/aim/fire hooks, weapon
 *     selection, optional enemy auto-kill ([debug] replay_autokill=1), and optional
 *     invulnerability ([debug] replay_invuln=1). Logs "replay: PASS (...)" or "replay: FAIL (...)"
 *     for automated verification under test/run.sh.
 * ============================================================================================ */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

/* ---- game memory */
#define GAME              (*(BYTE **)0x4b3f70)
#define SARGE             (*(BYTE **)0x697520)
#define SARGE_X(u)        (*(short *)((u) + 0x04))
#define SARGE_Y(u)        (*(short *)((u) + 0x06))
#define SARGE_HEALTH(u)   (*(short *)((u) + 0x1c))
#define SARGE_MAX(u)      (*(short *)((u) + 0x1e))
#define SARGE_WEAPON(u)   (*(BYTE **)((u) + 0x20))
#define SARGE_STATE(u)    (*(int *)((u) + 0x68))
#define SARGE_FACING(u)   (*(short *)((u) + 0x2c6) & 255)
#define SARGE_FACING16    (*(volatile WORD *)0x69753c)
#define SARGE_VEHICLE(u)  (*(BYTE **)((u) + 0x2d0))
#define CAMERA_X          (*(short *)0x4e2de8)
#define CAMERA_Y          (*(short *)0x4e2dea)
#define MENU_CURSOR_SHOWN (*(int *)0x4fe448)
#define INVULNERABLE      (*(volatile BYTE *)0x69752c)

#define LEGION_STATUS(a)  (*(int *)(0x4f7820 + (a) * 0x19e8 + 0x1990))
#define ALLIED(a, b)      (*(int *)(0x507d60 + ((a) * 4 + (b)) * 4))
#define UNIT_STRIDE       0x2c4
#define LEG_VEH_COUNT(l)  (*(signed char *)((l) + 0x50))
#define LEG_VEH(l, i)     (*(BYTE **)((l) + 0x54 + 4 * (i)))
#define LEG_VEH_MAX       50

#define UID_OF(p)         (*(DWORD *)(p))
#define UID_ARMY(u)       ((int)(((u) >> 26) & 7) - 1)
#define UID_TYPE(u)       ((int)(((u) >> 22) & 0xf) - 1)
#define UID_CLASS(u)      ((int)(((u) >> 16) & 0x3f) - 1)

#define T_MODE(t)         (*(int *)((t) + 0x68))
#define T_HEALTH(t)       (*(short *)((t) + 0x1c))
#define T_X(t)            (*(short *)((t) + 0x04))
#define T_Y(t)            (*(short *)((t) + 0x06))
#define T_WEAPON(t)       (*(BYTE **)((t) + 0x20))
#define W_TYPE(w)         (*(int *)(w))
#define W_AMMO(w)         (*(short *)((w) + 0x0c))

#define VEH_STATE(v)      (*(int *)((v) + 0x68))
#define VEH_HEALTH(v)     (*(short *)((v) + 0x1c))
#define VEH_X(v)          (*(short *)((v) + 0x04))
#define VEH_Y(v)          (*(short *)((v) + 0x06))

#define OBJECTS           (*(BYTE **)0x4e30ec)
#define OBJECT_COUNT      (*(int *)0x4e30e8)
#define OBJECT_SIZE       0xd0
#define OBJ_CAT(o)        (*(short *)((o) + 0x68))
#define OBJ_NUM(o)        (*(short *)((o) + 0x6a))
#define OBJ_FLAGS(o)      (*(BYTE *)((o) + 0x70))

typedef BYTE *(__fastcall *ArmyRecFn)(int army);
typedef void (__fastcall *ItemDamageFn)(void *item, int damage, int type, short *from, DWORD from_uid);
typedef void (__fastcall *PlaceUnitFn)(BYTE *unit, int x, int y);
typedef void (__fastcall *SargeInVehFn)(BYTE *sarge, BYTE *veh);
typedef void (__fastcall *SargeOutVehFn)(BYTE *sarge);
#define ARMY_RECORD       ((ArmyRecFn)0x41a160)
#define ITEM_DAMAGE       ((ItemDamageFn)0x431f60)
#define PLACE_UNIT        ((PlaceUnitFn)0x448a90)
#define SARGE_IN_VEH      ((SargeInVehFn)0x448ae0)
#define SARGE_OUT_VEH     ((SargeOutVehFn)0x448b50)
#define DMGTYPE_BULLET    6

/* ---- names */
static const char *weapon_name(int wt)
{
    switch (wt) {
        case 1:  return "trooper_rifle";
        case 2:  return "grenade";
        case 3:  return "flamethrower";
        case 4:  return "bazooka";
        case 5:  return "mortar";
        case 7:  return "mmg";
        case 8:  return "hmg";
        case 9:  return "rifle";
        case 10: return "auto_rifle";
        case 11: return "mine";
        case 12: return "explosive";
        case 14: return "cannon";
        case 20: return "sweeper";
        case 22: return "medkit";
        default: return "weapon";
    }
}

static const char *trooper_class_name(int cls)
{
    static const char *const N[] = {
        "rifleman", "grenadier", "flameman", "bazookaman",
        "sweeper", "mortarman", "minerman", "special"
    };
    return cls >= 0 && cls < 8 ? N[cls] : "soldier";
}

static const char *vk_to_name(int vk, char *buf, size_t n)
{
    if (vk >= 'A' && vk <= 'Z') { snprintf(buf, n, "%c", vk + 32); return buf; }
    if (vk >= '0' && vk <= '9') { snprintf(buf, n, "%c", vk); return buf; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(buf, n, "F%d", vk - VK_F1 + 1); return buf; }
    switch (vk) {
        case VK_RETURN:   return "Return";
        case VK_ESCAPE:   return "Escape";
        case VK_SPACE:    return "space";
        case VK_TAB:      return "Tab";
        case VK_BACK:     return "BackSpace";
        case VK_INSERT:   return "Insert";
        case VK_DELETE:   return "Delete";
        case VK_HOME:     return "Home";
        case VK_END:      return "End";
        case VK_PRIOR:    return "Prior";
        case VK_NEXT:     return "Next";
        case VK_PAUSE:    return "Pause";
        case VK_UP:       return "Up";
        case VK_DOWN:     return "Down";
        case VK_LEFT:     return "Left";
        case VK_RIGHT:    return "Right";
        case VK_OEM_MINUS:return "minus";
        case VK_OEM_PLUS: return "equal";
        default:          snprintf(buf, n, "vk_%d", vk); return buf;
    }
}

static void json_escape(const char *s, char *out, size_t n)
{
    size_t j = 0;
    if (!s) s = "";
    for (size_t i = 0; s[i] && j + 2 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = (char)c; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else if (c >= 0x20) out[j++] = (char)c;
    }
    out[j] = 0;
}

/* ============================================================================================
 * PART 1: SESSION RECORDER
 * ============================================================================================ */

static CRITICAL_SECTION g_cs;
static int g_cs_init;
static FILE *g_fp;
static char g_rec_path[MAX_PATH];
static DWORD g_t0;
static unsigned g_events;
static int g_bookmarks;

/* Enemy tracking (independent of weapon_drops so kills are ALWAYS recorded) */
#define MAX_ENEMIES 768
#define MAX_ENVEH   128
typedef struct { BYTE *p; DWORD uid; short x, y; BYTE army, cls, wt; } TrackEnt;
static TrackEnt g_tr_alive[MAX_ENEMIES], g_tr_now[MAX_ENEMIES];
static int g_ntr_alive;
static TrackEnt g_vh_alive[MAX_ENVEH], g_vh_now[MAX_ENVEH];
static int g_nvh_alive;

/* Cheat-kill tag ring so Ctrl+K kills are tagged via:"cheat" */
#define CHEAT_KILL_RING 16
static struct { DWORD uid; DWORD at; } g_cheat_kills[CHEAT_KILL_RING];
static int g_cheat_kill_idx;

/* Per-mission Sarge & lifecycle tracking */
static unsigned g_rec_run;
static int g_mission_open, g_mission_idx;
static unsigned g_cur_run;
static int g_cur_sc, g_cur_mi, g_cur_won;
static DWORD g_cur_t0;
static int g_cur_moves, g_cur_pickups, g_cur_fires, g_cur_kills;
static int g_last_sx, g_last_sy, g_last_hp, g_last_pose, g_last_sprint, g_last_wtype, g_last_wammo;
static DWORD g_last_veh_uid, g_last_move_ms;
static int g_was_moving, g_have_sarge;
static int g_stop_idx, g_is_stopped;
static DWORD g_stop_t0;
static int g_stop_x, g_stop_y;

static void set_sarge_facing(BYTE *u, int f)
{
    if (!u || IsBadReadPtr(u, 0x2d4)) return;
    f &= 255;
    *(short *)(u + 0x2c6) = (short)f;
    *(int *)(u + 0x88) = f;
    *(int *)(u + 0x74) = f;
    SARGE_FACING16 = (WORD)(f * 256 + 8);
}

/* Mouse button down timestamps for hold duration */
static DWORD g_btn_down_ms[5];
static int g_btn_down_gx[5], g_btn_down_gy[5];

static void rec_write_raw(const char *body, int flush_now)
{
    if (!g_fp) return;
    DWORD now = GetTickCount() - g_t0;
    DWORD gt = (g_dd && layout_view_is_gameplay()) ? clock_game_ms() : 0;
    EnterCriticalSection(&g_cs);
    if (g_fp) {
        fprintf(g_fp, "{\"t\":%lu,\"gt\":%lu,%s}\n", now, gt, body);
        g_events++;
        if (flush_now || (g_events & 15) == 0) fflush(g_fp);
    }
    LeaveCriticalSection(&g_cs);
}

static void rec_emit(int flush_now, const char *fmt, ...)
{
    if (!g_fp) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rec_write_raw(buf, flush_now);
}

static void dll_dir(char *out, size_t n)
{
    GetModuleFileNameA(g_self, out, (DWORD)n);
    char *slash = strrchr(out, '\\');
    if (slash) slash[1] = 0; else out[0] = 0;
}

static void prune_old_recordings(const char *dir, int keep)
{
    if (keep <= 0) return;
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\run_*.jsonl", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    char names[256][64];
    int count = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && count < 256) {
            snprintf(names[count], sizeof names[0], "%s", fd.cFileName);
            count++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (count <= keep) return;
    /* lexicographical sort works directly on run_YYYYMMDD_HHMMSS.jsonl */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char tmp[64];
                memcpy(tmp, names[i], sizeof tmp);
                memcpy(names[i], names[j], sizeof tmp);
                memcpy(names[j], tmp, sizeof tmp);
            }
    for (int i = 0; i < count - keep; i++) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s", dir, names[i]);
        DeleteFileA(p);
    }
}

static void replay_init(void);

void record_init(void)
{
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = 1; }
    g_t0 = GetTickCount();
    replay_init();
    if (!g_cfg.record_runs) return;

    const char *ini = mod_ini_path();
    char dir_rel[MAX_PATH] = "recordings";
    GetPrivateProfileStringA("debug", "record_dir", "recordings", dir_rel, sizeof dir_rel, ini);
    int keep = GetPrivateProfileIntA("debug", "record_keep", 30, ini);

    char dir[MAX_PATH];
    if (dir_rel[0] && ((dir_rel[1] == ':') || (dir_rel[0] == '\\' && dir_rel[1] == '\\'))) {
        strncpy(dir, dir_rel, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    } else {
        dll_dir(dir, sizeof dir);
        strncat(dir, dir_rel[0] ? dir_rel : "recordings", sizeof dir - strlen(dir) - 1);
    }
    CreateDirectoryA(dir, NULL);
    prune_old_recordings(dir, keep > 1 ? keep - 1 : 1);

    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(g_rec_path, sizeof g_rec_path, "%s\\run_%04u%02u%02u_%02u%02u%02u.jsonl",
             dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_fp = fopen(g_rec_path, "w");
    if (!g_fp) {
        mlog("record: could not open %s", g_rec_path);
        return;
    }

    int portable = 0;
#ifdef ARMYMEN_PORTABLE
    portable = 1;
#endif
    char hr[32];
    json_escape(g_cfg.height_rules >= 0 && g_cfg.height_rules < HEIGHTS_COUNT ? HEIGHT_RULE_NAMES[g_cfg.height_rules] : "stance", hr, sizeof hr);
    rec_emit(1,
        "\"e\":\"session_start\",\"build\":\"%s %s\",\"portable\":%d,"
        "\"cfg\":{\"w\":%d,\"h\":%d,\"scale\":%d,\"windowed\":%d,\"modern\":%d,"
        "\"classic_gameplay\":%d,\"modern_hud\":%d,\"six_weapons\":%d,\"health_regen\":%d,"
        "\"drive_enemy_vehicles\":%d,\"vehicle_bailout\":%d,\"break_fences\":%d,"
        "\"enemy_awareness\":%d,\"height_rules\":\"%s\",\"weapon_range_pct\":%d,"
        "\"clock_speed\":%.2f,\"cheat_keys\":%d}",
        __DATE__, __TIME__, portable,
        g_cfg.width, g_cfg.height, g_cfg.scale, g_cfg.windowed, g_cfg.controls_modern,
        g_cfg.classic_gameplay, g_cfg.modern_hud_layout, g_cfg.six_weapons, g_cfg.health_regen,
        g_cfg.drive_enemy_vehicles, g_cfg.vehicle_bailout, g_cfg.break_fences,
        g_cfg.enemy_awareness, hr, g_cfg.weapon_range_pct,
        (double)g_cfg.clock_speed, g_cfg.cheat_keys);
    mlog("record: session recording active -> %s (keep=%d)", g_rec_path, keep);
}

static void close_mission(const char *reason)
{
    if (!g_fp || !g_mission_open) return;
    DWORD dur = GetTickCount() - g_cur_t0;
    BYTE *s = SARGE;
    int sx = (s && !IsBadReadPtr(s, 8)) ? SARGE_X(s) : g_last_sx;
    int sy = (s && !IsBadReadPtr(s, 8)) ? SARGE_Y(s) : g_last_sy;
    int hp = (s && !IsBadReadPtr(s, 0x20)) ? SARGE_HEALTH(s) : g_last_hp;
    rec_emit(1,
        "\"e\":\"mission_end\",\"idx\":%d,\"run\":%u,\"scenario\":%d,\"mission\":%d,\"reason\":\"%s\","
        "\"won\":%d,\"duration_ms\":%lu,\"wx\":%d,\"wy\":%d,\"hp\":%d,"
        "\"moves\":%d,\"pickups\":%d,\"fires\":%d,\"kills\":%d",
        g_mission_idx, g_cur_run, g_cur_sc, g_cur_mi, reason ? reason : "end",
        g_cur_won ? 1 : 0, dur, sx, sy, hp,
        g_cur_moves, g_cur_pickups, g_cur_fires, g_cur_kills);
    mlog("record: mission #%d ended (mission %d, reason=%s, won=%d, %lu ms, moves=%d pickups=%d fires=%d kills=%d)",
         g_mission_idx, g_cur_mi, reason ? reason : "end", g_cur_won, dur,
         g_cur_moves, g_cur_pickups, g_cur_fires, g_cur_kills);
    g_mission_open = 0;
    g_cur_won = 0;
}

static void open_mission(unsigned run, int sc, int mi, int x, int y, int hp, int max_hp, int wt, int wa)
{
    g_mission_idx++;
    g_mission_open = 1;
    g_cur_run = run; g_cur_sc = sc; g_cur_mi = mi;
    g_cur_won = 0;
    g_cur_t0 = GetTickCount();
    g_cur_moves = g_cur_pickups = g_cur_fires = g_cur_kills = 0;
    g_stop_idx = 1;
    g_is_stopped = 1;
    g_was_moving = 0;
    g_stop_t0 = g_cur_t0;
    g_stop_x = x; g_stop_y = y;
    rec_emit(1, "\"e\":\"mission_start\",\"idx\":%d,\"run\":%u,\"scenario\":%d,\"mission\":%d,\"wx\":%d,\"wy\":%d,\"hp\":%d,\"max_hp\":%d,\"weapon\":%d,\"ammo\":%d",
             g_mission_idx, run, sc, mi, x, y, hp, max_hp, wt, wa);
    rec_emit(1, "\"e\":\"sarge_stop\",\"stop_idx\":%d,\"wx\":%d,\"wy\":%d,\"facing\":0,\"pose\":0,\"hp\":%d",
             g_stop_idx, x, y, hp);
    mlog("record: mission #%d started (run %u, scenario %d, mission %d at %d,%d, stop #1)", g_mission_idx, run, sc, mi, x, y);
}

void record_note_mission_won(void)
{
    if (!g_fp || !g_mission_open) return;
    g_cur_won = 1;
    rec_emit(1, "\"e\":\"mission_won\",\"idx\":%d,\"run\":%u,\"scenario\":%d,\"mission\":%d",
             g_mission_idx, g_cur_run, g_cur_sc, g_cur_mi);
}

void record_screen_note(void)
{
    if (!g_fp || !g_mission_open) return;
    if (!layout_view_is_gameplay()) {
        const char *reason = g_cur_won ? "won" : (g_last_hp <= 0 ? "failed" : "menu");
        close_mission(reason);
        g_have_sarge = 0;
        g_ntr_alive = g_nvh_alive = 0;
    }
}

void record_shutdown(void)
{
    if (!g_fp) return;
    if (g_mission_open) close_mission(g_cur_won ? "won" : "exit");
    rec_emit(1, "\"e\":\"session_end\",\"events\":%u,\"duration_ms\":%lu", g_events, GetTickCount() - g_t0);
    EnterCriticalSection(&g_cs);
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    LeaveCriticalSection(&g_cs);
}

int record_active(void) { return g_fp != NULL; }

/* ---- explicit event hooks called from other modules */

void record_note_walk_target(int tx, int ty, int sprint, DWORD veh_uid)
{
    if (!g_fp) return;
    BYTE *s = SARGE;
    int sx = s && !IsBadReadPtr(s, 8) ? SARGE_X(s) : 0;
    int sy = s && !IsBadReadPtr(s, 8) ? SARGE_Y(s) : 0;
    if (veh_uid)
        rec_emit(0, "\"e\":\"walk_target\",\"tx\":%d,\"ty\":%d,\"sprint\":%d,\"veh\":\"%08lx\",\"sx\":%d,\"sy\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"modern_walk_target\"",
                 tx, ty, sprint ? 1 : 0, veh_uid, sx, sy, g_stop_idx, g_is_stopped ? 1 : 0);
    else
        rec_emit(0, "\"e\":\"walk_target\",\"tx\":%d,\"ty\":%d,\"sprint\":%d,\"sx\":%d,\"sy\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"modern_walk_target\"",
                 tx, ty, sprint ? 1 : 0, sx, sy, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_weapon_pickup(int type, int ammo_before, int ammo_after, int box, int wx, int wy, DWORD uid, int merge)
{
    if (!g_fp) return;
    if (g_mission_open) g_cur_pickups++;
    int mod_only = (box >= 3);
    if (mod_only)
        rec_emit(1, "\"e\":\"weapon_pickup\",\"type\":%d,\"name\":\"%s\",\"ammo_before\":%d,\"ammo_after\":%d,\"box\":%d,\"wx\":%d,\"wy\":%d,\"uid\":\"%08lx\",\"merge\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"six_weapons\"",
                 type, weapon_name(type), ammo_before, ammo_after, box, wx, wy, uid, merge ? 1 : 0, g_stop_idx, g_is_stopped ? 1 : 0);
    else
        rec_emit(1, "\"e\":\"weapon_pickup\",\"type\":%d,\"name\":\"%s\",\"ammo_before\":%d,\"ammo_after\":%d,\"box\":%d,\"wx\":%d,\"wy\":%d,\"uid\":\"%08lx\",\"merge\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                 type, weapon_name(type), ammo_before, ammo_after, box, wx, wy, uid, merge ? 1 : 0, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_weapon_select(int type, int ammo, int slot, int box)
{
    if (!g_fp) return;
    int mod_only = (slot >= 3 || box >= 3);
    if (mod_only)
        rec_emit(0, "\"e\":\"weapon_select\",\"type\":%d,\"name\":\"%s\",\"ammo\":%d,\"slot\":%d,\"box\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"six_weapons\"",
                 type, weapon_name(type), ammo, slot, box, g_stop_idx, g_is_stopped ? 1 : 0);
    else
        rec_emit(0, "\"e\":\"weapon_select\",\"type\":%d,\"name\":\"%s\",\"ammo\":%d,\"slot\":%d,\"box\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                 type, weapon_name(type), ammo, slot, box, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_weapon_drop(int type, int wx, int wy)
{
    if (!g_fp) return;
    rec_emit(1, "\"e\":\"weapon_drop\",\"type\":%d,\"name\":\"%s\",\"wx\":%d,\"wy\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
             type, weapon_name(type), wx, wy, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_weapon_fire(int type, int ammo, int wx, int wy, int facing, int tx, int ty, DWORD target_uid)
{
    if (!g_fp) return;
    if (g_mission_open) g_cur_fires++;
    if (target_uid)
        rec_emit(0, "\"e\":\"weapon_fire\",\"type\":%d,\"name\":\"%s\",\"ammo\":%d,\"wx\":%d,\"wy\":%d,\"facing\":%d,\"tx\":%d,\"ty\":%d,\"target_uid\":\"%08lx\",\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                 type, weapon_name(type), ammo, wx, wy, facing, tx, ty, target_uid, g_stop_idx, g_is_stopped ? 1 : 0);
    else
        rec_emit(0, "\"e\":\"weapon_fire\",\"type\":%d,\"name\":\"%s\",\"ammo\":%d,\"wx\":%d,\"wy\":%d,\"facing\":%d,\"tx\":%d,\"ty\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                 type, weapon_name(type), ammo, wx, wy, facing, tx, ty, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_mine_lay(int type, int wx, int wy, int ammo)
{
    if (!g_fp) return;
    rec_emit(1, "\"e\":\"mine_lay\",\"type\":%d,\"name\":\"%s\",\"wx\":%d,\"wy\":%d,\"ammo\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
             type, weapon_name(type), wx, wy, ammo, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_cheat_kill(DWORD uid)
{
    if (!g_fp || !uid) return;
    g_cheat_kills[g_cheat_kill_idx].uid = uid;
    g_cheat_kills[g_cheat_kill_idx].at = GetTickCount();
    g_cheat_kill_idx = (g_cheat_kill_idx + 1) % CHEAT_KILL_RING;
}

static int was_cheat_kill(DWORD uid)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < CHEAT_KILL_RING; i++) {
        if (g_cheat_kills[i].uid == uid && now - g_cheat_kills[i].at < 3000) {
            g_cheat_kills[i].uid = 0;
            return 1;
        }
    }
    return 0;
}

void record_note_vehicle(const char *action, DWORD uid, int army, int cls, int wx, int wy, int hp)
{
    if (!g_fp) return;
    int mod_only = 0;
    const char *mod_feat = NULL;
    if (!strcmp(action, "bailout")) {
        mod_only = 1; mod_feat = "vehicle_bailout";
    } else if ((!strcmp(action, "enter") || !strcmp(action, "capture")) && army != 0) {
        mod_only = 1; mod_feat = "drive_enemy_vehicles";
    }
    if (mod_only)
        rec_emit(1, "\"e\":\"vehicle_%s\",\"uid\":\"%08lx\",\"army\":%d,\"class\":%d,\"wx\":%d,\"wy\":%d,\"hp\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"%s\"",
                 action, uid, army, cls, wx, wy, hp, g_stop_idx, g_is_stopped ? 1 : 0, mod_feat);
    else
        rec_emit(1, "\"e\":\"vehicle_%s\",\"uid\":\"%08lx\",\"army\":%d,\"class\":%d,\"wx\":%d,\"wy\":%d,\"hp\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                 action, uid, army, cls, wx, wy, hp, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_order(const char *mode, int wx, int wy, DWORD unit_uid, const char *via)
{
    if (!g_fp) return;
    char m[32], v[32];
    json_escape(mode, m, sizeof m);
    json_escape(via, v, sizeof v);
    int mod_only = !strcmp(via, "hud");
    if (mod_only)
        rec_emit(1, "\"e\":\"order\",\"mode\":\"%s\",\"wx\":%d,\"wy\":%d,\"unit_uid\":\"%08lx\",\"via\":\"%s\",\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"modern_hud\"",
                 m, wx, wy, unit_uid, v, g_stop_idx, g_is_stopped ? 1 : 0);
    else
        rec_emit(1, "\"e\":\"order\",\"mode\":\"%s\",\"wx\":%d,\"wy\":%d,\"unit_uid\":\"%08lx\",\"via\":\"%s\",\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                 m, wx, wy, unit_uid, v, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_medkit(const char *action, int wx, int wy, int kits, int hp_before, int hp_after)
{
    if (!g_fp) return;
    char a[32];
    json_escape(action, a, sizeof a);
    int mod_only = !strcmp(action, "use") || !strcmp(action, "use_ally");
    if (mod_only)
        rec_emit(1, "\"e\":\"medkit\",\"action\":\"%s\",\"wx\":%d,\"wy\":%d,\"kits\":%d,\"hp_before\":%d,\"hp_after\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"medkit_use\"",
                 a, wx, wy, kits, hp_before, hp_after, g_stop_idx, g_is_stopped ? 1 : 0);
    else
        rec_emit(1, "\"e\":\"medkit\",\"action\":\"%s\",\"wx\":%d,\"wy\":%d,\"kits\":%d,\"hp_before\":%d,\"hp_after\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                 a, wx, wy, kits, hp_before, hp_after, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_setting(const char *section, const char *key, const char *val)
{
    if (!g_fp) return;
    char s[48], k[64], v[64];
    json_escape(section, s, sizeof s);
    json_escape(key, k, sizeof k);
    json_escape(val, v, sizeof v);
    rec_emit(1, "\"e\":\"setting\",\"section\":\"%s\",\"key\":\"%s\",\"val\":\"%s\"", s, k, v);
}

void record_note_cheat(const char *name, int wx, int wy, const char *detail)
{
    if (!g_fp) return;
    char n[48], d[128];
    json_escape(name, n, sizeof n);
    json_escape(detail ? detail : "", d, sizeof d);
    rec_emit(1, "\"e\":\"cheat\",\"name\":\"%s\",\"wx\":%d,\"wy\":%d,\"detail\":\"%s\",\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"cheat\"",
             n, wx, wy, d, g_stop_idx, g_is_stopped ? 1 : 0);
}

void record_note_savestate(const char *action, int slot, int ok)
{
    if (!g_fp) return;
    char a[24];
    json_escape(action, a, sizeof a);
    rec_emit(1, "\"e\":\"savestate\",\"action\":\"%s\",\"slot\":%d,\"ok\":%d,\"mod\":1,\"mod_feature\":\"savestate\"", a, slot, ok ? 1 : 0);
}

void record_note_exception(DWORD code, void *at, const char *module, DWORD offset)
{
    if (!g_fp) return;
    char mod[64];
    json_escape(module, mod, sizeof mod);
    BYTE *s = SARGE;
    int sx = s && !IsBadReadPtr(s, 8) ? SARGE_X(s) : 0;
    int sy = s && !IsBadReadPtr(s, 8) ? SARGE_Y(s) : 0;
    int sc = -1, mi = -1;
    gameplay_run(&sc, &mi);
    rec_emit(1, "\"e\":\"exception\",\"code\":\"0x%08lx\",\"at\":\"%p\",\"mod\":\"%s\",\"off\":\"0x%lx\",\"scenario\":%d,\"mission\":%d,\"wx\":%d,\"wy\":%d",
             code, at, mod, offset, sc, mi, sx, sy);
}

static const char *screen_state_name(int st)
{
    switch (st) {
        case 0: return "title";
        case 1: return "main_menu";
        case 2: return "campaign_select";
        case 3: return "callsign";
        case 4: return "mission_select";
        case 5: return "gameplay";
        default:
            if (layout_view_is_gameplay()) return game_menu_up() ? "pause_menu" : "briefing";
            return "menu";
    }
}

void record_note_button_click(void *bptr)
{
    if (!g_fp || !bptr) return;
    BYTE *btn = (BYTE *)bptr;
    if (IsBadReadPtr(btn, 0x310)) return;

    DWORD data = *(DWORD *)(btn + 0x38);
    void *cb = *(void **)(btn + 0x30);
    const short *pb = (const short *)(btn + 0x2fc); /* SBOX: x, y, cx, cy */
    int bx = pb[0], by = pb[1], bw = pb[2], bh = pb[3];

    int st = missionpath_screen_state();
    const char *sname = screen_state_name(st);

    char label[64] = "";
    const char *txt = (const char *)(btn + 0x44);
    if (!IsBadReadPtr(txt, 4) && (unsigned char)txt[0] >= 0x20 && (unsigned char)txt[0] <= 0x7e) {
        size_t j = 0;
        for (size_t i = 0; txt[i] && j + 1 < sizeof label; i++) {
            if ((unsigned char)txt[i] >= 0x20 && (unsigned char)txt[i] <= 0x7e)
                label[j++] = txt[i];
        }
        label[j] = 0;
    }

    char item[64] = "";
    int is_mod = 0;
    const char *mod_feat = NULL;

    /* Check if this is a mod menu button */
    if (st < 0 || layout_view_is_gameplay()) {
        if (!strcmp(label, "Mod Settings") || !strcmp(label, "In-game Mod Settings")) {
            is_mod = 1; mod_feat = "mod_menu";
            snprintf(item, sizeof item, "MOD_SETTINGS");
        }
    }

    if (!item[0] && label[0]) {
        size_t j = 0;
        for (size_t i = 0; label[i] && j + 1 < sizeof item; i++) {
            char c = label[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) item[j++] = c;
            else if (j > 0 && item[j - 1] != '_') item[j++] = '_';
        }
        if (j > 0 && item[j - 1] == '_') j--;
        item[j] = 0;
    }

    if (!item[0]) {
        if (cb == (void *)0x463e80) {
            if (data == 1 || (bx >= 40 && bx <= 200)) snprintf(item, sizeof item, "CAMPAIGN_DESERT");
            else if (data == 2 || (bx >= 220 && bx <= 380)) snprintf(item, sizeof item, "CAMPAIGN_ALPINE");
            else if (data == 3 || (bx >= 400 && bx <= 560)) snprintf(item, sizeof item, "CAMPAIGN_BAYOU");
            else snprintf(item, sizeof item, "CAMPAIGN_BTN_%lu", data);
        } else if (cb == (void *)0x4640a0) {
            snprintf(item, sizeof item, "SCENARIO_SELECT_%lu", data);
        } else {
            snprintf(item, sizeof item, "BTN_%s_%d_%d", sname, bx, by);
        }
    }

    char item_esc[80], lab_esc[80];
    json_escape(item, item_esc, sizeof item_esc);
    json_escape(label, lab_esc, sizeof lab_esc);

    if (is_mod) {
        rec_emit(1, "\"e\":\"menu_click\",\"item\":\"%s\",\"label\":\"%s\",\"screen\":%d,\"screen_name\":\"%s\",\"box\":[%d,%d,%d,%d],\"data\":%lu,\"mod\":1,\"mod_feature\":\"%s\"",
                 item_esc, lab_esc, st, sname, bx, by, bw, bh, data, mod_feat ? mod_feat : "mod_menu");
    } else {
        rec_emit(1, "\"e\":\"menu_click\",\"item\":\"%s\",\"label\":\"%s\",\"screen\":%d,\"screen_name\":\"%s\",\"box\":[%d,%d,%d,%d],\"data\":%lu,\"mod\":0",
                 item_esc, lab_esc, st, sname, bx, by, bw, bh, data);
    }
    mlog("record: menu click on \"%s\" (label=\"%s\", screen %d [%s], box=[%d,%d,%d,%d], mod=%d)",
         item, label, st, sname, bx, by, bw, bh, is_mod);
}

/* ---- window message hook for UI clicks, hotkeys, and Ctrl+Shift+B bookmark */

static int is_wasd_key(int vk) { return vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D'; }

static const char *click_context(int *screen_out)
{
    int st = missionpath_screen_state();
    if (screen_out) *screen_out = st;
    if (!layout_view_is_gameplay()) return "menu";
    if (MENU_CURSOR_SHOWN) return "menu";
    if (controls_pointer_on_ui()) return "hud";
    return "field";
}

void record_message(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!g_fp) return;
    (void)h;

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (lp & (1 << 30)) return;   /* skip key auto-repeat */
        int ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        int shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        int alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
        /* Ctrl+Shift+B: manual test bookmark / checkpoint */
        if (ctrl && shift && wp == 'B') {
            int sc = -1, mi = -1;
            gameplay_run(&sc, &mi);
            BYTE *s = SARGE;
            int sx = s && !IsBadReadPtr(s, 8) ? SARGE_X(s) : 0;
            int sy = s && !IsBadReadPtr(s, 8) ? SARGE_Y(s) : 0;
            int hp = s && !IsBadReadPtr(s, 0x20) ? SARGE_HEALTH(s) : 0;
            g_bookmarks++;
            rec_emit(1, "\"e\":\"bookmark\",\"n\":%d,\"scenario\":%d,\"mission\":%d,\"wx\":%d,\"wy\":%d,\"hp\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"bookmark\"",
                     g_bookmarks, sc, mi, sx, sy, hp, g_stop_idx, g_is_stopped ? 1 : 0);
            mlog("record: bookmark #%d at mission %d (%d,%d, hp %d)", g_bookmarks, mi, sx, sy, hp);
            return;
        }
        /* Ignore modifier-only keys and gameplay WASD taps (captured via movement breadcrumbs) */
        if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU || wp == VK_LSHIFT || wp == VK_RSHIFT ||
            wp == VK_LCONTROL || wp == VK_RCONTROL) return;
        int in_play = layout_view_is_gameplay() && !MENU_CURSOR_SHOWN;
        if (in_play && !ctrl && !alt && is_wasd_key((int)wp)) return;
        char kbuf[24];
        const char *kn = vk_to_name((int)wp, kbuf, sizeof kbuf);
        int mods = (ctrl ? 1 : 0) | (shift ? 2 : 0) | (alt ? 4 : 0);
        rec_emit(0, "\"e\":\"key\",\"vk\":%u,\"name\":\"%s\",\"mods\":%d,\"ctx\":\"%s\",\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                 (unsigned)wp, kn, mods, in_play ? "play" : "menu", g_stop_idx, g_is_stopped ? 1 : 0);
        return;
    }

    int btn = -1, down = 0;
    const char *bname = NULL;
    /* In pointer mode, mouse_PostMessageA marks the game's synthetic re-posted mouse messages
     * with 0x80000000; skip them so each real user click is recorded once in exact game pixels. */
    if (g_cfg.mouse_pointer && (msg >= WM_MOUSEMOVE && msg <= WM_MOUSEWHEEL) && (wp & 0x80000000u)) return;
    switch (msg) {
        case WM_LBUTTONDOWN: btn = 0; down = 1; bname = "L"; break;
        case WM_LBUTTONUP:   btn = 0; down = 0; bname = "L"; break;
        case WM_RBUTTONDOWN: btn = 1; down = 1; bname = "R"; break;
        case WM_RBUTTONUP:   btn = 1; down = 0; bname = "R"; break;
        case WM_MBUTTONDOWN: btn = 2; down = 1; bname = "M"; break;
        case WM_MBUTTONUP:   btn = 2; down = 0; bname = "M"; break;
        case WM_XBUTTONDOWN: btn = HIWORD(wp) == XBUTTON1 ? 3 : 4; down = 1; bname = btn == 3 ? "X1" : "X2"; break;
        case WM_XBUTTONUP:   btn = HIWORD(wp) == XBUTTON1 ? 3 : 4; down = 0; bname = btn == 3 ? "X1" : "X2"; break;
        case WM_MOUSEWHEEL: {
            int gx = 0, gy = 0, scr = -1;
            mouse_real_cursor(&gx, &gy);
            const char *ctx = click_context(&scr);
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            rec_emit(0, "\"e\":\"ui_click\",\"btn\":\"wheel\",\"delta\":%d,\"gx\":%d,\"gy\":%d,\"ctx\":\"%s\",\"screen\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"wheel\"",
                     delta > 0 ? 1 : -1, gx, gy, ctx, scr, g_stop_idx, g_is_stopped ? 1 : 0);
            return;
        }
        default: return;
    }

    int gx = 0, gy = 0;
    if (g_cfg.mouse_pointer) {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        view_client_to_game(&p);
        gx = p.x; gy = p.y;
    } else {
        mouse_real_cursor(&gx, &gy);
    }

    if (down) {
        g_btn_down_ms[btn] = GetTickCount();
        g_btn_down_gx[btn] = gx;
        g_btn_down_gy[btn] = gy;
        int scr = -1;
        const char *ctx = click_context(&scr);
        if (!strcmp(ctx, "field")) {
            int wx = gx + CAMERA_X, wy = gy + CAMERA_Y;
            rec_emit(0, "\"e\":\"ui_click\",\"btn\":\"%s\",\"gx\":%d,\"gy\":%d,\"ctx\":\"%s\",\"wx\":%d,\"wy\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"battlefield_click\"",
                     bname, gx, gy, ctx, wx, wy, g_stop_idx, g_is_stopped ? 1 : 0);
        } else if (!strcmp(ctx, "hud")) {
            rec_emit(1, "\"e\":\"ui_click\",\"btn\":\"%s\",\"gx\":%d,\"gy\":%d,\"ctx\":\"%s\",\"screen\":%d,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"modern_hud\"",
                     bname, gx, gy, ctx, scr, g_stop_idx, g_is_stopped ? 1 : 0);
        } else {
            rec_emit(1, "\"e\":\"ui_click\",\"btn\":\"%s\",\"gx\":%d,\"gy\":%d,\"ctx\":\"%s\",\"screen\":%d,\"mod\":0",
                     bname, gx, gy, ctx, scr);
        }
    } else if (g_btn_down_ms[btn]) {
        DWORD hold = GetTickCount() - g_btn_down_ms[btn];
        int dx = gx - g_btn_down_gx[btn], dy = gy - g_btn_down_gy[btn];
        g_btn_down_ms[btn] = 0;
        if (hold >= 250 || dx * dx + dy * dy >= 144) {
            int scr = -1;
            const char *ctx = click_context(&scr);
            rec_emit(0, "\"e\":\"ui_release\",\"btn\":\"%s\",\"gx\":%d,\"gy\":%d,\"hold_ms\":%lu,\"ctx\":\"%s\"",
                     bname, gx, gy, hold, ctx);
        }
    }
}

/* ---- per-frame passive tracking (mission starts, movement breadcrumbs, weapon state, enemy kills) */

static int is_enemy_army(int a) { return a >= 1 && a <= 3 && LEGION_STATUS(a) && !ALLIED(0, a); }

static void track_enemies(void)
{
    int nt = 0, nv = 0;
    for (int a = 1; a <= 3; a++) {
        if (!is_enemy_army(a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg || IsBadReadPtr(leg, 0x120)) continue;

        /* Enemy soldiers */
        BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
        for (int g = 0; un && un < end && g < 64 && !IsBadReadPtr(un, 0xe0); un += UNIT_STRIDE, g++) {
            int mode = *(int *)(un + 0x70);
            if (mode < 3 || mode > 5) continue;
            int last = *(int *)(un + 0xdc);
            if (last < 0 || last > 9) last = 9;
            for (int i = 0; i <= last && nt < MAX_ENEMIES; i++) {
                BYTE *t = ((BYTE **)(un + 0xb4))[i];
                if (!t || IsBadReadPtr(t, 0x70) || T_MODE(t) != 1 || T_HEALTH(t) <= 0) continue;
                DWORD uid = UID_OF(t);
                BYTE *w = T_WEAPON(t);
                int wt = w && !IsBadReadPtr(w, 4) ? W_TYPE(w) : 0;
                g_tr_now[nt++] = (TrackEnt){ t, uid, T_X(t), T_Y(t), (BYTE)a, (BYTE)UID_CLASS(uid), (BYTE)wt };
            }
        }

        /* Enemy vehicles */
        int nveh = LEG_VEH_COUNT(leg);
        for (int i = 0; i < nveh && i < LEG_VEH_MAX && nv < MAX_ENVEH; i++) {
            BYTE *v = LEG_VEH(leg, i);
            if (!v || IsBadReadPtr(v, 0x70) || VEH_STATE(v) != 1 || VEH_HEALTH(v) <= 0) continue;
            DWORD uid = UID_OF(v);
            g_vh_now[nv++] = (TrackEnt){ v, uid, VEH_X(v), VEH_Y(v), (BYTE)a, (BYTE)UID_CLASS(uid), 0 };
        }
    }

    BYTE *s = SARGE;
    int sx = s && !IsBadReadPtr(s, 8) ? SARGE_X(s) : 0;
    int sy = s && !IsBadReadPtr(s, 8) ? SARGE_Y(s) : 0;

    /* Newly killed enemy soldiers */
    for (int i = 0; i < g_ntr_alive; i++) {
        int still = 0;
        for (int j = 0; j < nt && !still; j++) still = g_tr_now[j].uid == g_tr_alive[i].uid;
        if (still) continue;
        BYTE *t = g_tr_alive[i].p;
        if (IsBadReadPtr(t, 0x70) || UID_OF(t) != g_tr_alive[i].uid || T_HEALTH(t) > 0) continue;
        int x = T_X(t), y = T_Y(t);
        if (x <= 0 || y <= 0 || x > 4095 || y > 4095) { x = g_tr_alive[i].x; y = g_tr_alive[i].y; }
        int cheat = was_cheat_kill(g_tr_alive[i].uid);
        if (g_mission_open) g_cur_kills++;
        if (cheat)
            rec_emit(1, "\"e\":\"enemy_kill\",\"kind\":\"soldier\",\"uid\":\"%08lx\",\"army\":%d,\"class\":%d,\"name\":\"%s\",\"weapon\":%d,\"wx\":%d,\"wy\":%d,\"sx\":%d,\"sy\":%d,\"via\":\"cheat\",\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"cheat_kill\"",
                     g_tr_alive[i].uid, g_tr_alive[i].army, g_tr_alive[i].cls, trooper_class_name(g_tr_alive[i].cls),
                     g_tr_alive[i].wt, x, y, sx, sy, g_stop_idx, g_is_stopped ? 1 : 0);
        else
            rec_emit(1, "\"e\":\"enemy_kill\",\"kind\":\"soldier\",\"uid\":\"%08lx\",\"army\":%d,\"class\":%d,\"name\":\"%s\",\"weapon\":%d,\"wx\":%d,\"wy\":%d,\"sx\":%d,\"sy\":%d,\"via\":\"combat\",\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                     g_tr_alive[i].uid, g_tr_alive[i].army, g_tr_alive[i].cls, trooper_class_name(g_tr_alive[i].cls),
                     g_tr_alive[i].wt, x, y, sx, sy, g_stop_idx, g_is_stopped ? 1 : 0);
    }

    /* Newly destroyed enemy vehicles */
    for (int i = 0; i < g_nvh_alive; i++) {
        int still = 0;
        for (int j = 0; j < nv && !still; j++) still = g_vh_now[j].uid == g_vh_alive[i].uid;
        if (still) continue;
        BYTE *v = g_vh_alive[i].p;
        if (IsBadReadPtr(v, 0x70) || UID_OF(v) != g_vh_alive[i].uid || (VEH_HEALTH(v) > 0 && VEH_STATE(v) == 1)) continue;
        int x = VEH_X(v), y = VEH_Y(v);
        if (x <= 0 || y <= 0 || x > 4095 || y > 4095) { x = g_vh_alive[i].x; y = g_vh_alive[i].y; }
        if (g_mission_open) g_cur_kills++;
        rec_emit(1, "\"e\":\"enemy_kill\",\"kind\":\"vehicle\",\"uid\":\"%08lx\",\"army\":%d,\"class\":%d,\"name\":\"vehicle_%d\",\"wx\":%d,\"wy\":%d,\"sx\":%d,\"sy\":%d,\"via\":\"combat\",\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                 g_vh_alive[i].uid, g_vh_alive[i].army, g_vh_alive[i].cls, g_vh_alive[i].cls, x, y, sx, sy, g_stop_idx, g_is_stopped ? 1 : 0);
    }

    memcpy(g_tr_alive, g_tr_now, sizeof g_tr_now[0] * (size_t)nt);
    g_ntr_alive = nt;
    memcpy(g_vh_alive, g_vh_now, sizeof g_vh_now[0] * (size_t)nv);
    g_nvh_alive = nv;
}

static void record_gameplay_frame(void)
{
    if (!g_fp) return;
    int sc = -1, mi = -1;
    unsigned run = gameplay_run(&sc, &mi);
    BYTE *s = SARGE;
    int s_ok = s && !IsBadReadPtr(s, 0x2d4);

    if (run != g_rec_run) {
        if (g_mission_open) {
            const char *reason = g_cur_won ? "won" :
                                 (sc == g_cur_sc && mi == g_cur_mi) ? "restart" :
                                 (sc == g_cur_sc && mi > g_cur_mi)  ? "next_part" : "next_mission";
            close_mission(reason);
        }
        g_rec_run = run;
        g_ntr_alive = g_nvh_alive = 0;
        g_have_sarge = 0;
        g_was_moving = 0;
        g_is_stopped = 0;
        if (s_ok && SARGE_X(s) > 0 && SARGE_Y(s) > 0) {
            BYTE *w = SARGE_WEAPON(s);
            int wt = w && !IsBadReadPtr(w, 0x10) ? W_TYPE(w) : -1;
            int wa = w && !IsBadReadPtr(w, 0x10) ? W_AMMO(w) : 0;
            open_mission(run, sc, mi, SARGE_X(s), SARGE_Y(s), SARGE_HEALTH(s), SARGE_MAX(s), wt, wa);
            g_last_sx = SARGE_X(s); g_last_sy = SARGE_Y(s);
            g_last_hp = SARGE_HEALTH(s);
            g_last_pose = SARGE_STATE(s);
            g_last_sprint = controls_sarge_running();
            g_last_wtype = wt; g_last_wammo = wa;
            g_last_veh_uid = SARGE_VEHICLE(s) && !IsBadReadPtr(SARGE_VEHICLE(s), 4) ? UID_OF(SARGE_VEHICLE(s)) : 0;
            g_last_move_ms = clock_game_ms();
            g_have_sarge = 1;
        }
    }

    if (!s_ok) return;
    track_enemies();

    int x = SARGE_X(s), y = SARGE_Y(s);
    if (x <= 0 && y <= 0) return;
    BYTE *veh = SARGE_VEHICLE(s);
    DWORD vuid = veh && !IsBadReadPtr(veh, 4) ? UID_OF(veh) : 0;
    if (vuid && !IsBadReadPtr(veh, 8)) { x = VEH_X(veh); y = VEH_Y(veh); }
    int hp = SARGE_HEALTH(s), max_hp = SARGE_MAX(s);
    int pose = SARGE_STATE(s);
    int sprint = controls_sarge_running();
    int facing = SARGE_FACING(s);
    DWORD now = clock_game_ms();

    if (!g_have_sarge) {
        BYTE *w = SARGE_WEAPON(s);
        int wt = w && !IsBadReadPtr(w, 0x10) ? W_TYPE(w) : -1;
        int wa = w && !IsBadReadPtr(w, 0x10) ? W_AMMO(w) : 0;
        open_mission(run, sc, mi, x, y, hp, max_hp, wt, wa);
        g_last_sx = x; g_last_sy = y; g_last_hp = hp; g_last_pose = pose;
        g_last_sprint = sprint; g_last_veh_uid = vuid; g_last_wtype = wt; g_last_wammo = wa;
        g_last_move_ms = now; g_have_sarge = 1;
        return;
    }

    /* Passive weapon-change fallback (covers classic mode / non-six-weapons swaps) */
    if (!vuid) {
        BYTE *w = SARGE_WEAPON(s);
        int wt = w && !IsBadReadPtr(w, 0x10) ? W_TYPE(w) : -1;
        int wa = w && !IsBadReadPtr(w, 0x10) ? W_AMMO(w) : 0;
        if (wt >= 0 && wt != g_last_wtype) {
            int slot = (int)(*(signed char *)(s + 0x2c4));
            int mod_only = (slot >= 3);
            if (mod_only)
                rec_emit(0, "\"e\":\"weapon_select\",\"type\":%d,\"name\":\"%s\",\"ammo\":%d,\"slot\":%d,\"box\":-1,\"stop_idx\":%d,\"stopped\":%d,\"mod\":1,\"mod_feature\":\"six_weapons\"",
                         wt, weapon_name(wt), wa, slot, g_stop_idx, g_is_stopped ? 1 : 0);
            else
                rec_emit(0, "\"e\":\"weapon_select\",\"type\":%d,\"name\":\"%s\",\"ammo\":%d,\"slot\":%d,\"box\":-1,\"stop_idx\":%d,\"stopped\":%d,\"mod\":0",
                         wt, weapon_name(wt), wa, slot, g_stop_idx, g_is_stopped ? 1 : 0);
            g_last_wtype = wt; g_last_wammo = wa;
        } else {
            g_last_wammo = wa;
        }
    }

    /* Health deltas (damage / death / major heal) */
    if (hp != g_last_hp) {
        if (hp <= 0 || g_last_hp <= 0 || abs(hp - g_last_hp) >= 5) {
            rec_emit(hp <= 0, "\"e\":\"sarge_hp\",\"hp\":%d,\"max_hp\":%d,\"delta\":%d,\"wx\":%d,\"wy\":%d",
                     hp, max_hp, hp - g_last_hp, x, y);
        }
        g_last_hp = hp;
    }

    /* Movement breadcrumb and stop sampling */
    int dx = x - g_last_sx, dy = y - g_last_sy;
    int dist2 = dx * dx + dy * dy;
    DWORD dt = now - g_last_move_ms;

    if (g_is_stopped && dist2 >= 16 * 16) {
        g_is_stopped = 0;
        g_was_moving = 1;
        g_last_sx = x; g_last_sy = y;
        g_last_move_ms = now;
    }

    int moved_far = dist2 >= 24 * 24 && dt >= 120;
    int jumped    = dist2 >= 64 * 64;
    int state_chg = (sprint != g_last_sprint) || (vuid != g_last_veh_uid);
    int stopped   = g_was_moving && (dist2 < 16 * 16) && dt >= 350;

    if (stopped) {
        g_stop_idx++;
        g_is_stopped = 1;
        g_was_moving = 0;
        g_stop_x = x; g_stop_y = y;
        g_stop_t0 = now;
        if (vuid)
            rec_emit(1, "\"e\":\"sarge_stop\",\"stop_idx\":%d,\"wx\":%d,\"wy\":%d,\"facing\":%d,\"pose\":%d,\"hp\":%d,\"veh\":\"%08lx\"",
                     g_stop_idx, x, y, facing, pose, hp, vuid);
        else
            rec_emit(1, "\"e\":\"sarge_stop\",\"stop_idx\":%d,\"wx\":%d,\"wy\":%d,\"facing\":%d,\"pose\":%d,\"hp\":%d",
                     g_stop_idx, x, y, facing, pose, hp);
        g_last_sx = x; g_last_sy = y;
        g_last_pose = pose; g_last_sprint = sprint; g_last_veh_uid = vuid;
        g_last_move_ms = now;
    } else if (moved_far || jumped || state_chg) {
        if (g_mission_open) g_cur_moves++;
        if (vuid)
            rec_emit(0, "\"e\":\"move\",\"wx\":%d,\"wy\":%d,\"facing\":%d,\"pose\":%d,\"sprint\":%d,\"hp\":%d,\"veh\":\"%08lx\"",
                     x, y, facing, pose, sprint, hp, vuid);
        else
            rec_emit(0, "\"e\":\"move\",\"wx\":%d,\"wy\":%d,\"facing\":%d,\"pose\":%d,\"sprint\":%d,\"hp\":%d",
                     x, y, facing, pose, sprint, hp);
        g_last_sx = x; g_last_sy = y;
        g_last_pose = pose; g_last_sprint = sprint; g_last_veh_uid = vuid;
        g_last_move_ms = now;
        g_was_moving = 1;
        g_is_stopped = 0;
    }
}

/* ============================================================================================
 * PART 2: SEMANTIC REPLAYER ([debug] replay=<path.jsonl>)
 * ============================================================================================ */

enum {
    RS_NONE = 0,
    RS_MOVE,
    RS_WALK_TARGET,
    RS_STOP,
    RS_WEAPON_PICKUP,
    RS_WEAPON_SELECT,
    RS_WEAPON_FIRE,
    RS_ENEMY_KILL,
    RS_VEH_ENTER,
    RS_VEH_EXIT,
    RS_BOOKMARK
};

typedef struct {
    int kind;
    DWORD gt;
    int wx, wy, tx, ty;
    int wtype, sprint, army, cls;
    int facing;
    DWORD uid;
} ReplayStep;

#define MAX_REPLAY_STEPS 2048
static struct {
    int on, done, started;
    int autokill, teleport, invuln, any_mission;
    int target_mission;
    int nsteps, idx;
    DWORD step_started_ms, start_gt, first_gt;
    int walked_issued;
    int ok_moves, ok_pickups, total_pickups, ok_fires, ok_kills, fails;
    ReplayStep steps[MAX_REPLAY_STEPS];
} g_rep;

/* Minimal JSON field extractors (our own .jsonl lines are flat & well-formed) */
static int json_str(const char *line, const char *key, char *out, size_t n)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t j = 0;
    while (*p && *p != '"' && j + 1 < n) {
        if (*p == '\\' && p[1]) p++;
        out[j++] = *p++;
    }
    out[j] = 0;
    return 1;
}

static int json_int(const char *line, const char *key, int *out)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    if (*p == '"') return 0;
    *out = atoi(p);
    return 1;
}

static int json_hex(const char *line, const char *key, DWORD *out)
{
    char buf[32];
    if (!json_str(line, key, buf, sizeof buf)) return 0;
    *out = (DWORD)strtoul(buf, NULL, 16);
    return 1;
}

static void replay_init(void)
{
    const char *ini = mod_ini_path();
    char path[MAX_PATH] = "";
    GetPrivateProfileStringA("debug", "replay", "", path, sizeof path, ini);
    if (!path[0]) return;

    FILE *f = fopen(path, "r");
    if (!f) { mlog("replay: FAIL could not open %s", path); return; }

    g_rep.autokill = GetPrivateProfileIntA("debug", "replay_autokill", 1, ini);
    g_rep.teleport = GetPrivateProfileIntA("debug", "replay_teleport", 0, ini);
    g_rep.invuln   = GetPrivateProfileIntA("debug", "replay_invuln", 1, ini);
    g_rep.any_mission = GetPrivateProfileIntA("debug", "replay_any_mission", 0, ini);
    g_rep.target_mission = -1;

    char line[1024];
    while (fgets(line, sizeof line, f) && g_rep.nsteps < MAX_REPLAY_STEPS) {
        char ev[40] = "";
        if (!json_str(line, "e", ev, sizeof ev)) continue;
        int gt = 0; json_int(line, "gt", &gt);
        if (!strcmp(ev, "mission_start")) {
            int mi = -1;
            if (g_rep.target_mission < 0 && json_int(line, "mission", &mi)) g_rep.target_mission = mi;
            continue;
        }
        ReplayStep st = { 0 };
        st.gt = (DWORD)(gt > 0 ? gt : 0);
        st.facing = -1;
        if (!strcmp(ev, "sarge_stop") || !strcmp(ev, "stop")) {
            st.kind = RS_STOP;
            json_int(line, "wx", &st.wx); json_int(line, "wy", &st.wy);
            json_int(line, "facing", &st.facing);
            json_int(line, "stop_idx", &st.cls);
        } else if (!strcmp(ev, "move")) {
            st.kind = RS_MOVE;
            json_int(line, "wx", &st.wx); json_int(line, "wy", &st.wy);
            json_int(line, "sprint", &st.sprint);
        } else if (!strcmp(ev, "walk_target")) {
            st.kind = RS_WALK_TARGET;
            json_int(line, "tx", &st.wx); json_int(line, "ty", &st.wy);
            json_int(line, "sprint", &st.sprint);
            json_hex(line, "veh", &st.uid);
        } else if (!strcmp(ev, "weapon_pickup")) {
            st.kind = RS_WEAPON_PICKUP;
            json_int(line, "type", &st.wtype);
            json_int(line, "wx", &st.wx); json_int(line, "wy", &st.wy);
            json_hex(line, "uid", &st.uid);
            g_rep.total_pickups++;
        } else if (!strcmp(ev, "weapon_select")) {
            st.kind = RS_WEAPON_SELECT;
            json_int(line, "type", &st.wtype);
        } else if (!strcmp(ev, "weapon_fire")) {
            st.kind = RS_WEAPON_FIRE;
            json_int(line, "type", &st.wtype);
            json_int(line, "wx", &st.wx); json_int(line, "wy", &st.wy);
            json_int(line, "tx", &st.tx); json_int(line, "ty", &st.ty);
            json_hex(line, "target_uid", &st.uid);
        } else if (!strcmp(ev, "enemy_kill")) {
            st.kind = RS_ENEMY_KILL;
            json_hex(line, "uid", &st.uid);
            json_int(line, "army", &st.army);
            json_int(line, "class", &st.cls);
            json_int(line, "wx", &st.wx); json_int(line, "wy", &st.wy);
        } else if (!strcmp(ev, "vehicle_enter") || !strcmp(ev, "vehicle_capture")) {
            st.kind = RS_VEH_ENTER;
            json_hex(line, "uid", &st.uid);
            json_int(line, "wx", &st.wx); json_int(line, "wy", &st.wy);
        } else if (!strcmp(ev, "vehicle_exit")) {
            st.kind = RS_VEH_EXIT;
        } else if (!strcmp(ev, "bookmark")) {
            st.kind = RS_BOOKMARK;
            json_int(line, "n", &st.cls);
            json_int(line, "wx", &st.wx); json_int(line, "wy", &st.wy);
        } else {
            continue;
        }
        g_rep.steps[g_rep.nsteps++] = st;
    }
    fclose(f);
    if (g_rep.nsteps > 0) {
        g_rep.on = 1;
        g_rep.first_gt = g_rep.steps[0].gt;
        mlog("replay: loaded %d steps from %s (mission %d, autokill=%d, teleport=%d, invuln=%d)",
             g_rep.nsteps, path, g_rep.target_mission, g_rep.autokill, g_rep.teleport, g_rep.invuln);
    } else {
        mlog("replay: FAIL no replayable steps in %s", path);
    }
}

static BYTE *find_enemy_by_uid_or_pos(DWORD uid, int army, int cls, int wx, int wy)
{
    BYTE *best = NULL;
    long best_d = 160 * 160;
    for (int a = 1; a <= 3; a++) {
        if (!is_enemy_army(a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg || IsBadReadPtr(leg, 0x120)) continue;

        /* Soldiers */
        BYTE *un = *(BYTE **)(leg + 4), *end = *(BYTE **)(leg + 8);
        for (int g = 0; un && un < end && g < 64 && !IsBadReadPtr(un, 0xe0); un += UNIT_STRIDE, g++) {
            int mode = *(int *)(un + 0x70);
            if (mode < 3 || mode > 5) continue;
            int last = *(int *)(un + 0xdc);
            if (last < 0 || last > 9) last = 9;
            for (int i = 0; i <= last; i++) {
                BYTE *t = ((BYTE **)(un + 0xb4))[i];
                if (!t || IsBadReadPtr(t, 0x70) || T_MODE(t) != 1 || T_HEALTH(t) <= 0) continue;
                if (uid && UID_OF(t) == uid) return t;
                if (army > 0 && a != army) continue;
                if (cls >= 0 && UID_CLASS(UID_OF(t)) != cls) continue;
                long dx = T_X(t) - wx, dy = T_Y(t) - wy, d = dx * dx + dy * dy;
                if (d < best_d) { best_d = d; best = t; }
            }
        }
        /* Vehicles */
        int nv = LEG_VEH_COUNT(leg);
        for (int i = 0; i < nv && i < LEG_VEH_MAX; i++) {
            BYTE *v = LEG_VEH(leg, i);
            if (!v || IsBadReadPtr(v, 0x70) || VEH_STATE(v) != 1 || VEH_HEALTH(v) <= 0) continue;
            if (uid && UID_OF(v) == uid) return v;
        }
    }
    return best;
}

static BYTE *find_vehicle_by_uid_or_pos(DWORD uid, int wx, int wy)
{
    BYTE *best = NULL;
    long best_d = 140 * 140;
    for (int a = 0; a < 4; a++) {
        if (!LEGION_STATUS(a)) continue;
        BYTE *leg = ARMY_RECORD(a);
        if (!leg || IsBadReadPtr(leg, 0x120)) continue;
        int nv = LEG_VEH_COUNT(leg);
        for (int i = 0; i < nv && i < LEG_VEH_MAX; i++) {
            BYTE *v = LEG_VEH(leg, i);
            if (!v || IsBadReadPtr(v, 0x70) || VEH_STATE(v) != 1 || VEH_HEALTH(v) <= 0) continue;
            if (uid && UID_OF(v) == uid) return v;
            long dx = VEH_X(v) - wx, dy = VEH_Y(v) - wy, d = dx * dx + dy * dy;
            if (d < best_d) { best_d = d; best = v; }
        }
    }
    return best;
}

static void replay_advance(DWORD now)
{
    g_rep.idx++;
    g_rep.step_started_ms = now;
    g_rep.walked_issued = 0;
}

static void replay_frame(void)
{
    if (!g_rep.on || g_rep.done) return;
    if (!layout_view_is_gameplay() || MENU_CURSOR_SHOWN) return;
    BYTE *u = SARGE;
    if (!u || IsBadReadPtr(u, 0x2d4) || (SARGE_X(u) <= 0 && SARGE_Y(u) <= 0)) return;

    int sc = -1, mi = -1;
    gameplay_run(&sc, &mi);
    if (!g_rep.any_mission && g_rep.target_mission >= 0 && mi != g_rep.target_mission) return;

    DWORD now = clock_game_ms();
    if (!g_rep.started) {
        g_rep.started = 1;
        g_rep.start_gt = now;
        g_rep.step_started_ms = now;
        if (g_rep.invuln) INVULNERABLE = 1;
        mlog("replay: started on mission %d (%d steps, Sarge at %d,%d)", mi, g_rep.nsteps, SARGE_X(u), SARGE_Y(u));
    }

    if (SARGE_HEALTH(u) <= 0) {
        g_rep.done = 1;
        mlog("replay: FAIL Sarge died at step %d/%d (%d,%d)", g_rep.idx, g_rep.nsteps, SARGE_X(u), SARGE_Y(u));
        return;
    }

    if (g_rep.idx >= g_rep.nsteps) {
        g_rep.done = 1;
        mlog("replay: PASS (steps=%d, moves=%d, pickups=%d/%d, fires=%d, kills=%d, fails=%d, final_pos=%d,%d, hp=%d/%d)",
             g_rep.nsteps, g_rep.ok_moves, g_rep.ok_pickups, g_rep.total_pickups, g_rep.ok_fires, g_rep.ok_kills,
             g_rep.fails, SARGE_X(u), SARGE_Y(u), SARGE_HEALTH(u), SARGE_MAX(u));
        return;
    }

    ReplayStep *st = &g_rep.steps[g_rep.idx];
    DWORD elapsed = now - g_rep.step_started_ms;
    int sx = SARGE_X(u), sy = SARGE_Y(u);
    BYTE *veh = SARGE_VEHICLE(u);
    if (veh && !IsBadReadPtr(veh, 8)) { sx = VEH_X(veh); sy = VEH_Y(veh); }

    switch (st->kind) {
        case RS_STOP: {
            if (!veh) {
                PLACE_UNIT(u, st->wx, st->wy);
                if (st->facing >= 0) set_sarge_facing(u, st->facing);
            } else {
                PLACE_UNIT(veh, st->wx, st->wy);
            }
            g_rep.ok_moves++;
            mlog("replay: step %d teleported Sarge to stop at %d,%d (facing %d)", g_rep.idx, st->wx, st->wy, st->facing);
            replay_advance(now);
            break;
        }
        case RS_MOVE:
        case RS_WALK_TARGET: {
            int dx = st->wx - sx, dy = st->wy - sy;
            int dist2 = dx * dx + dy * dy;
            if (dist2 <= 28 * 28) {
                g_rep.ok_moves++;
                replay_advance(now);
                break;
            }
            if (g_rep.teleport) {
                if (!veh) {
                    PLACE_UNIT(u, st->wx, st->wy);
                    if (st->facing >= 0) set_sarge_facing(u, st->facing);
                } else {
                    PLACE_UNIT(veh, st->wx, st->wy);
                }
                g_rep.ok_moves++;
                replay_advance(now);
                break;
            }
            if (!g_rep.walked_issued) {
                controls_replay_walk(st->wx, st->wy, st->sprint);
                g_rep.walked_issued = 1;
            }
            if (elapsed > 7000) {
                if (!veh) PLACE_UNIT(u, st->wx, st->wy);
                mlog("replay: step %d walk to %d,%d timed out (at %d,%d) - nudged", g_rep.idx, st->wx, st->wy, sx, sy);
                g_rep.ok_moves++;
                replay_advance(now);
            }
            break;
        }
        case RS_WEAPON_PICKUP: {
            int dx = st->wx - sx, dy = st->wy - sy;
            if (dx * dx + dy * dy > 24 * 24) {
                if (g_rep.teleport && !veh) PLACE_UNIT(u, st->wx, st->wy);
                else if (!g_rep.walked_issued) { controls_replay_walk(st->wx, st->wy, 0); g_rep.walked_issued = 1; }
                if (elapsed <= 6000) break;
                if (!veh) PLACE_UNIT(u, st->wx, st->wy);
            }
            if (elapsed < 300) break;
            int carried = weapons_carried(st->wtype, NULL) != NULL ||
                          (SARGE_WEAPON(u) && !IsBadReadPtr(SARGE_WEAPON(u), 4) && W_TYPE(SARGE_WEAPON(u)) == st->wtype);
            if (carried) g_rep.ok_pickups++;
            mlog("replay: step %d weapon_pickup type %d (%s) at %d,%d -> %s",
                 g_rep.idx, st->wtype, weapon_name(st->wtype), st->wx, st->wy, carried ? "carried" : "not_carried");
            replay_advance(now);
            break;
        }
        case RS_WEAPON_SELECT: {
            if (!veh) weapons_select_type(st->wtype);
            replay_advance(now);
            break;
        }
        case RS_WEAPON_FIRE: {
            if (!g_rep.walked_issued) {
                if (!veh && st->wtype > 0) weapons_select_type(st->wtype);
                controls_replay_fire(st->tx, st->ty);
                g_rep.walked_issued = 1;
                g_rep.ok_fires++;
            }
            if (elapsed >= 220) replay_advance(now);
            break;
        }
        case RS_ENEMY_KILL: {
            if (g_rep.autokill) {
                BYTE *e = find_enemy_by_uid_or_pos(st->uid, st->army, st->cls, st->wx, st->wy);
                if (e) {
                    short from[2] = { (short)sx, (short)sy };
                    ITEM_DAMAGE(e, 1000, DMGTYPE_BULLET, from, UID_OF(u));
                    g_rep.ok_kills++;
                    mlog("replay: step %d auto-killed enemy %08lx at %d,%d", g_rep.idx, UID_OF(e), T_X(e), T_Y(e));
                } else {
                    g_rep.ok_kills++;
                    mlog("replay: step %d enemy %08lx already dead", g_rep.idx, st->uid);
                }
            }
            replay_advance(now);
            break;
        }
        case RS_VEH_ENTER: {
            if (!veh) {
                BYTE *v = find_vehicle_by_uid_or_pos(st->uid, st->wx, st->wy);
                if (v) {
                    int dx = VEH_X(v) - sx, dy = VEH_Y(v) - sy;
                    if (dx * dx + dy * dy > 48 * 48) {
                        if (g_rep.teleport) PLACE_UNIT(u, VEH_X(v), VEH_Y(v));
                        else if (!g_rep.walked_issued) { controls_replay_walk(VEH_X(v), VEH_Y(v), 0); g_rep.walked_issued = 1; }
                        if (elapsed <= 6000) break;
                    }
                    SARGE_IN_VEH(u, v);
                    mlog("replay: step %d enter vehicle %08lx", g_rep.idx, UID_OF(v));
                }
            }
            replay_advance(now);
            break;
        }
        case RS_VEH_EXIT: {
            if (veh) SARGE_OUT_VEH(u);
            replay_advance(now);
            break;
        }
        case RS_BOOKMARK: {
            mlog("replay: reached bookmark #%d (at %d,%d, hp %d/%d)", st->cls, sx, sy, SARGE_HEALTH(u), SARGE_MAX(u));
            replay_advance(now);
            break;
        }
        default:
            replay_advance(now);
            break;
    }
}

void record_frame(void)
{
    record_gameplay_frame();
    replay_frame();
}
