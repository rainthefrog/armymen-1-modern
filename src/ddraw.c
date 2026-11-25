/* DLL entry, config, logging, IDirectDraw/IDirectDraw2, palette and clipper objects. */
#include "common.h"
#include <stdio.h>
#include <stdarg.h>

#include <stdlib.h>

struct Config g_cfg = { 1024, 540, 1, 1 };

/* The one-file release deliberately ships without the optional HD pack. Keep every HD path available for a
 * later armymen-mod.ini, but do not advertise missing assets as enabled when the DLL is used on its own. */
#ifdef ARMYMEN_PORTABLE
#define DEFAULT_HD_ART 0
#define DEFAULT_RECORD_RUNS 0
#else
#define DEFAULT_HD_ART 1
#define DEFAULT_RECORD_RUNS 1
#endif
HMODULE g_self;
static FILE *g_log;

/* ---------------------------------------------------------------- config and log */

static void path_next_to_dll(char *out, size_t n, const char *name)
{
    GetModuleFileNameA(g_self, out, (DWORD)n);
    char *slash = strrchr(out, '\\');
    if (slash) slash[1] = 0; else out[0] = 0;
    strncat(out, name, n - strlen(out) - 1);
}

/* armymen-mod.ini next to the DLL (menu.c also writes the Gameplay menu choice there). */
const char *mod_ini_path(void)
{
    static char ini[MAX_PATH];
    if (!ini[0]) path_next_to_dll(ini, sizeof ini, "armymen-mod.ini");
    return ini;
}

void config_load(void)
{
    const char *ini = mod_ini_path();
    g_cfg.width  = GetPrivateProfileIntA("display", "width",  1024, ini);
    g_cfg.height = GetPrivateProfileIntA("display", "height", 540, ini);
    g_cfg.vsync  = GetPrivateProfileIntA("display", "vsync",  1, ini);
    g_cfg.fps_limit = GetPrivateProfileIntA("display", "fps_limit", 0, ini);
    if (g_cfg.fps_limit < 0 || g_cfg.fps_limit > 1000) g_cfg.fps_limit = 0;
    g_cfg.log    = GetPrivateProfileIntA("debug",   "log",    1, ini);
    g_cfg.mouse_fix       = GetPrivateProfileIntA("mouse", "fix", 1, ini);
    g_cfg.mouse_winspeed  = GetPrivateProfileIntA("mouse", "use_windows_speed", 1, ini);
    g_cfg.mouse_pointer   = GetPrivateProfileIntA("mouse", "follow_windows_pointer", 1, ini);
    char buf[32];
    GetPrivateProfileStringA("mouse", "sensitivity", "1.0", buf, sizeof buf, ini);
    g_cfg.mouse_sens = (float)atof(buf);
    if (g_cfg.mouse_sens <= 0.0f || g_cfg.mouse_sens > 20.0f) g_cfg.mouse_sens = 1.0f;
    g_cfg.scale    = GetPrivateProfileIntA("display", "scale", 2, ini);
    g_cfg.windowed = GetPrivateProfileIntA("display", "windowed", 1, ini);
    if (g_cfg.scale < 1 || g_cfg.scale > 8) g_cfg.scale = 1;
    GetPrivateProfileStringA("clock", "speed", "1.0", buf, sizeof buf, ini);
    g_cfg.clock_speed = (float)atof(buf);
    if (g_cfg.clock_speed < 0.0f || g_cfg.clock_speed > 8.0f) g_cfg.clock_speed = 1.0f;
    g_cfg.clock_keys = GetPrivateProfileIntA("clock", "hotkeys", 1, ini);
    g_cfg.video_fit = GetPrivateProfileIntA("display", "video_fit", 1, ini);
    g_cfg.borderless_fs = GetPrivateProfileIntA("display", "borderless_fullscreen", 0, ini);
    g_cfg.fs_extends_view = GetPrivateProfileIntA("display", "fullscreen_extends_view", 1, ini);
    { char zb[16]; GetPrivateProfileStringA("display", "zoom", "1.0", zb, sizeof zb, ini); g_cfg.zoom = (float)atof(zb);
      if (!(g_cfg.zoom >= 1.0f && g_cfg.zoom <= 4.0f)) g_cfg.zoom = 1.0f; }
    char mode[16];
    GetPrivateProfileStringA("controls", "mode", "modern", mode, sizeof mode, ini);
    g_cfg.controls_modern = !_stricmp(mode, "modern");
    g_cfg.controls_tap_ms = GetPrivateProfileIntA("controls", "tap_ms", 250, ini);
    g_cfg.controls_fire_interval_ms = GetPrivateProfileIntA("controls", "fire_interval_ms", 0, ini);
    g_cfg.controls_confine = GetPrivateProfileIntA("controls", "keep_pointer_in_window", 0, ini);   /* off since 2026-11-17 (user) */
    GetPrivateProfileStringA("controls", "run_speed", "1.6", buf, sizeof buf, ini);
    g_cfg.controls_run_speed = (float)atof(buf);
    if (g_cfg.controls_run_speed < 1.0f || g_cfg.controls_run_speed > 4.0f) g_cfg.controls_run_speed = 1.6f;
    g_cfg.six_weapons = GetPrivateProfileIntA("controls", "six_weapons", 1, ini);
    g_cfg.cheat_keys = GetPrivateProfileIntA("debug", "cheat_keys", 0, ini);   /* test aids (Ctrl+T/I/B/G): off unless set */
    g_cfg.record_runs = GetPrivateProfileIntA("debug", "record_runs", DEFAULT_RECORD_RUNS, ini) != 0;
    g_cfg.update_check = GetPrivateProfileIntA("update", "check", 1, ini) != 0;
    g_cfg.health_regen = GetPrivateProfileIntA("gameplay", "health_regen", 1, ini);
    g_cfg.shoot_over = GetPrivateProfileIntA("gameplay", "shoot_over", 1, ini);
    g_cfg.cover_enemies = GetPrivateProfileIntA("gameplay", "cover_enemies", 1, ini);
    g_cfg.corner_shots = GetPrivateProfileIntA("gameplay", "corner_shots", 1, ini);
    g_cfg.leg_hits = GetPrivateProfileIntA("gameplay", "leg_hits", 1, ini);
    {   /* height_rules = stance (default) | am2 | original | modded (bullets.c). No key: the older downhill_hits=0 -> original */
        char hr[16];
        GetPrivateProfileStringA("gameplay", "height_rules", "", hr, sizeof hr, ini);
        if (!hr[0]) g_cfg.height_rules = GetPrivateProfileIntA("gameplay", "downhill_hits", 1, ini) ? HEIGHTS_STANCE : HEIGHTS_ORIGINAL;
        else g_cfg.height_rules = height_rules_parse(hr);
    }
    g_cfg.longer_range = GetPrivateProfileIntA("gameplay", "longer_range", 1, ini);
    g_cfg.fix_squash = GetPrivateProfileIntA("gameplay", "fix_squash", 1, ini);
    g_cfg.drive_enemy_vehicles = GetPrivateProfileIntA("gameplay", "drive_enemy_vehicles", 1, ini);   /* on by default (2026-11-20, user expected it) */
    g_cfg.vehicle_bailout = GetPrivateProfileIntA("gameplay", "vehicle_bailout", 1, ini);
    g_cfg.vehicle_reverse = GetPrivateProfileIntA("controls", "vehicle_reverse", 1, ini);   /* vehicles.c (2026-11-20) */
    g_cfg.turret_smooth = GetPrivateProfileIntA("controls", "turret_smooth", 1, ini);
    g_cfg.vehicle_unstick = GetPrivateProfileIntA("controls", "vehicle_unstick", 1, ini);
    g_cfg.sarge_unstick = GetPrivateProfileIntA("controls", "sarge_unstick", 1, ini);   /* barricades.c (2026-11-20) */
    g_cfg.break_fences = GetPrivateProfileIntA("gameplay", "break_fences", 0, ini);   /* fences.c (2026-11-20); default off 2026-11-21 (user: "Sarge cannot run through fences - only vehicles") */
    /* aware.c / modern.c (2026-11-20): enemy_awareness groups the enemy alerts; the older key enemy_alerts is the fallback */
    g_cfg.enemy_awareness = GetPrivateProfileIntA("modern", "enemy_awareness", GetPrivateProfileIntA("modern", "enemy_alerts", 1, ini), ini);
    g_cfg.squad_sprint = GetPrivateProfileIntA("gameplay", "squad_sprint", 1, ini);   /* squad.c (2026-11-20) */
    g_cfg.patrol_marches = GetPrivateProfileIntA("gameplay", "patrol_marches", 1, ini);   /* patrol.c (2026-11-21) */
    g_cfg.ally_regen = GetPrivateProfileIntA("gameplay", "ally_regen", 1, ini);           /* allies.c (2026-11-21) */
    g_cfg.ally_heal = GetPrivateProfileIntA("gameplay", "ally_heal", 1, ini);
    g_cfg.ally_health_x = GetPrivateProfileIntA("gameplay", "ally_health_x", 200, ini);
    g_cfg.ally_health_bars = GetPrivateProfileIntA("hud", "ally_health_bars", 1, ini);
    g_cfg.am2_sounds = GetPrivateProfileIntA("audio", "am2_sounds", 0, ini);   /* audio.c (2026-11-21) */
    g_cfg.am2_music = GetPrivateProfileIntA("audio", "am2_music", 0, ini);     /* music.c (2026-11-21) */
    g_cfg.see_interior = GetPrivateProfileIntA("display", "see_interior", 1, ini);
    g_cfg.fronts_order = GetPrivateProfileIntA("display", "fronts_order", 1, ini);
    g_cfg.air_layer = GetPrivateProfileIntA("display", "air_layer", 1, ini);
    g_cfg.menu_text = GetPrivateProfileIntA("display", "menu_text", 1, ini);
    g_cfg.radio_overlay = GetPrivateProfileIntA("display", "radio_overlay", 1, ini);
    g_cfg.weapon_range_pct = GetPrivateProfileIntA("gameplay", "weapon_range_pct", 120, ini);
    g_cfg.map_edge_fog = GetPrivateProfileIntA("display", "map_edge_fog", 1, ini);
    g_cfg.hud_corners = GetPrivateProfileIntA("hud", "corners", 1, ini);
    { char cs[16]; GetPrivateProfileStringA("hud", "corner_scale", "1.5", cs, sizeof cs, ini); g_cfg.hud_corner_scale = (float)atof(cs); }
    { char cs[16]; GetPrivateProfileStringA("hud", "minimap_scale", "2", cs, sizeof cs, ini); g_cfg.hud_minimap_scale = (float)atof(cs); }
    { char cs[16]; GetPrivateProfileStringA("hud", "centre_scale", "1.5", cs, sizeof cs, ini); g_cfg.hud_centre_scale = (float)atof(cs); }
    { char cs[16]; GetPrivateProfileStringA("hud", "air_scale", "1.5", cs, sizeof cs, ini); g_cfg.hud_air_scale = (float)atof(cs); }
    g_cfg.pin_objective = GetPrivateProfileIntA("hud", "pin_objective", 1, ini);
    g_cfg.target_sector = GetPrivateProfileIntA("hud", "target_sector", 1, ini);
    g_cfg.target_area = GetPrivateProfileIntA("hud", "target_area", 1, ini);
    g_cfg.mission_path = GetPrivateProfileIntA("hud", "mission_path", 1, ini);
    g_cfg.mission_path_variation = GetPrivateProfileIntA("hud", "mission_path_variation", 1, ini);
    g_cfg.mission_path_all = GetPrivateProfileIntA("hud", "mission_path_all", 0, ini);
    g_cfg.mm_blips = GetPrivateProfileIntA("hud", "minimap_blips", 1, ini);          /* minimap.c */
    g_cfg.mm_sarge = GetPrivateProfileIntA("hud", "minimap_sarge", 1, ini);
    g_cfg.mm_contrast = GetPrivateProfileIntA("hud", "minimap_contrast", 1, ini);
    g_cfg.mm_hover = GetPrivateProfileIntA("hud", "minimap_hover", 1, ini);
    g_cfg.crt = GetPrivateProfileIntA("display", "crt", 0, ini);
    g_cfg.see_through = GetPrivateProfileIntA("display", "see_through", 1, ini);
    g_cfg.see_enemies = GetPrivateProfileIntA("display", "see_enemies", 1, ini);
    g_cfg.button_press = GetPrivateProfileIntA("display", "button_press", 1, ini);
    g_cfg.smooth_scroll = GetPrivateProfileIntA("display", "smooth_scroll", 1, ini);
    g_cfg.clock_lock = GetPrivateProfileIntA("clock", "lock_to_display", 1, ini);
    g_cfg.hd_menus = GetPrivateProfileIntA("display", "hd_menus", DEFAULT_HD_ART, ini);
    g_cfg.hd_sprites = GetPrivateProfileIntA("display", "hd_sprites", DEFAULT_HD_ART, ini);
    g_cfg.hd_world = GetPrivateProfileIntA("display", "hd_world", DEFAULT_HD_ART, ini);
    g_cfg.hd_videos = GetPrivateProfileIntA("display", "hd_videos", DEFAULT_HD_ART, ini);
    g_cfg.regen_delay_ms = GetPrivateProfileIntA("gameplay", "regen_delay_ms", 6000, ini);
    GetPrivateProfileStringA("gameplay", "regen_full_s", "90", buf, sizeof buf, ini);
    g_cfg.regen_full_s = (float)atof(buf);
    GetPrivateProfileStringA("controls", "camera_lead", "0.15", buf, sizeof buf, ini);
    g_cfg.controls_camera_lead = (float)atof(buf);
    if (g_cfg.controls_camera_lead < 0.0f || g_cfg.controls_camera_lead > 1.0f) g_cfg.controls_camera_lead = 0.15f;
    GetPrivateProfileStringA("controls", "camera_smooth_ms", "120", buf, sizeof buf, ini);
    g_cfg.controls_camera_smooth_ms = (float)atof(buf);
    if (g_cfg.controls_camera_smooth_ms < 0.0f || g_cfg.controls_camera_smooth_ms > 2000.0f) g_cfg.controls_camera_smooth_ms = 120.0f;
    if (g_cfg.controls_fire_interval_ms < 0) g_cfg.controls_fire_interval_ms = 0;
    g_cfg.classic_gameplay = GetPrivateProfileIntA("preset", "classic_gameplay", 0, ini) != 0;
    g_cfg.modern_hud_layout = GetPrivateProfileIntA("hud", "modern_layout", 1, ini) != 0;
    if (!g_cfg.modern_hud_layout) {
        g_cfg.hud_corners = 0;
        g_cfg.hud_corner_scale = g_cfg.hud_minimap_scale = g_cfg.hud_centre_scale = g_cfg.hud_air_scale = 1.0f;
    }
    if (g_cfg.classic_gameplay) {
        g_cfg.controls_modern = g_cfg.six_weapons = g_cfg.health_regen = 0;
        g_cfg.shoot_over = g_cfg.cover_enemies = g_cfg.corner_shots = g_cfg.leg_hits = 0;
        g_cfg.height_rules = HEIGHTS_ORIGINAL; g_cfg.longer_range = g_cfg.fix_squash = 0;
        g_cfg.drive_enemy_vehicles = g_cfg.vehicle_bailout = g_cfg.vehicle_reverse = g_cfg.turret_smooth = 0;
        g_cfg.vehicle_unstick = g_cfg.sarge_unstick = g_cfg.break_fences = g_cfg.enemy_awareness = 0;
        g_cfg.squad_sprint = g_cfg.patrol_marches = g_cfg.ally_regen = g_cfg.ally_heal = g_cfg.ally_health_bars = 0;
        g_cfg.ally_health_x = 100; g_cfg.am2_sounds = g_cfg.am2_music = 0;
        g_cfg.menu_text = g_cfg.radio_overlay = g_cfg.see_interior = g_cfg.fronts_order = g_cfg.air_layer = 0;
        g_cfg.map_edge_fog = g_cfg.pin_objective = g_cfg.target_sector = g_cfg.target_area = g_cfg.mission_path = 0;
        g_cfg.mm_blips = g_cfg.mm_sarge = g_cfg.mm_contrast = g_cfg.mm_hover = 0;
        g_cfg.crt = g_cfg.see_through = g_cfg.see_enemies = g_cfg.button_press = 0;
        g_cfg.hd_menus = g_cfg.hd_sprites = g_cfg.hd_world = g_cfg.hd_videos = 0;
    }
    /* [display] resolution = a standard size (layout.c presets) or "custom" for width/height/scale */
    char res[32];
    GetPrivateProfileStringA("display", "resolution", "1080p", res, sizeof res, ini);
    if (layout_resolve_preset(res)) {
        /* preset chose width, height and scale. Starting in fullscreen (borderless remembered, or
         * windowed=0): the game picture takes the whole screen at the SAME scale, so the HUD keeps
         * its size and the battlefield view grows (user: extend the view, do not enlarge it). The
         * game's surfaces are made once, so switching while running applies from the next start. */
        if ((g_cfg.borderless_fs || !g_cfg.windowed) && g_cfg.fs_extends_view) {
            int dw, dh;
            desktop_size(&dw, &dh);
            if (dw / g_cfg.scale >= g_cfg.width && dh / g_cfg.scale >= g_cfg.height) {
                g_cfg.width = (dw / g_cfg.scale) & ~1;
                g_cfg.height = dh / g_cfg.scale;
            }
        }
    } else if (g_cfg.width == 0 || g_cfg.height == 0) {
        /* 0 = fill the desktop (in game pixels, i.e. divided by scale) */
        desktop_size(&g_cfg.width, &g_cfg.height);
        g_cfg.width /= g_cfg.scale;
        g_cfg.height /= g_cfg.scale;
    }
    if (g_cfg.width < 640) g_cfg.width = 640;     /* the HUD and menus need at least 640x480 */
    if (g_cfg.height < 480) g_cfg.height = 480;
    if (g_cfg.log) {
        char lp[MAX_PATH];
        path_next_to_dll(lp, sizeof lp, "armymen-mod.log");
        /* the previous run's log is kept as armymen-mod.prev.log (a new start used to wipe a trace the user had
         * just recorded - 2026-11-17) */
        char pp[MAX_PATH];
        path_next_to_dll(pp, sizeof pp, "armymen-mod.prev.log");
        MoveFileExA(lp, pp, MOVEFILE_REPLACE_EXISTING);
        g_log = fopen(lp, "w");
    }
    mlog("armymen-mod ddraw.dll v%s built " __DATE__ " " __TIME__, update_version());
    mlog("armymen-mod ddraw.dll loaded; resolution %s: game size %dx%d x%d (%dx%d on screen), %s, vsync %d",
         res, g_cfg.width, g_cfg.height, g_cfg.scale, g_cfg.width * g_cfg.scale, g_cfg.height * g_cfg.scale,
         g_cfg.windowed ? "windowed" : "fullscreen", g_cfg.vsync);
    mlog("clock: speed %.2f hotkeys %d", g_cfg.clock_speed, g_cfg.clock_keys);
    mlog("display: videos fill the window %d, borderless fullscreen %d", g_cfg.video_fit, g_cfg.borderless_fs);
    mlog("controls: %s", g_cfg.controls_modern ? "modern (mouse aim, WASD)" : "classic");
    mlog("preset: classic gameplay %d, modern HUD layout %d", g_cfg.classic_gameplay, g_cfg.modern_hud_layout);
    mlog("mouse: fix %d follow_windows_pointer %d sensitivity %.2f use_windows_speed %d", g_cfg.mouse_fix,
         g_cfg.mouse_pointer, g_cfg.mouse_sens, g_cfg.mouse_winspeed);
}

/* Without this, Windows display scaling (125-200%, normal on 4K screens) makes Windows lie to the
 * game about the screen size and stretch its window: blurry, and the cursor no longer lines up. */
static const char *make_dpi_aware(void)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    BOOL (WINAPI *ctx)(HANDLE) = (void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (ctx) {
        if (ctx((HANDLE)-4)) return "per-monitor v2";            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        /* refused because it is already set: by Armymen.exe.manifest (expected) or a Windows
         * compatibility setting (reported by report_compat_layers) */
        if (GetLastError() == ERROR_ACCESS_DENIED) return "already set before load (Armymen.exe.manifest)";
    }
    BOOL (WINAPI *old)(void) = (void *)GetProcAddress(u, "SetProcessDPIAware");
    if (old && old()) return "system";
    return "unchanged";
}

void mlog(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(g_log, "[%8lu] ", GetTickCount());
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(ap);
}

void desktop_size(int *w, int *h)
{
    /* the user's desktop mode (not one we switched to); real pixels since we are DPI aware */
    DEVMODEA dm = { .dmSize = sizeof dm };
    if (EnumDisplaySettingsA(NULL, ENUM_REGISTRY_SETTINGS, &dm) && dm.dmPelsWidth) {
        *w = dm.dmPelsWidth; *h = dm.dmPelsHeight;
    } else {
        *w = GetSystemMetrics(SM_CXSCREEN); *h = GetSystemMetrics(SM_CYSCREEN);
    }
}

/* Windows adds compatibility settings to old games on its own (Program Compatibility Assistant).
 * "DWM8And16BitMitigation" (reduced colour mode) in particular turns off display-scaling awareness
 * and shrinks or moves the game window, so the window no longer fits our size calculations.
 * Windows tells the process which layers are active in the __COMPAT_LAYER environment variable. */
static void report_compat_layers(void)
{
    char layers[512];
    DWORD n = GetEnvironmentVariableA("__COMPAT_LAYER", layers, sizeof layers);
    if (n == 0 || n >= sizeof layers) { mlog("Windows compatibility settings: none"); return; }
    mlog("Windows compatibility settings: %s", layers);
    if (strstr(layers, "DWM8And16BitMitigation") || strstr(layers, "256Color") || strstr(layers, "640X480"))
        mlog("  WARNING: these change the screen mode and window size. Turn them off: right-click "
             "Armymen.exe > Properties > Compatibility > \"Change settings for all users\" and untick "
             "\"Reduced color mode\" / \"Run in 640 x 480\" on both tabs.");
}

/* ---------------------------------------------------------------- guarded calls into game code
 * 2026-11-19, user after two crashes in the game's blitter reached through the mod's ghost redraw: "make sure to
 * guard against these crashes". A mod feature that calls a game routine with the mod's own arguments, or walks
 * game structures it does not own, runs inside a guard: if it faults, the fault is logged ("GUARDED ..."), the
 * call is abandoned and the game goes on (the feature can switch itself off). One guard at a time, on the
 * thread that entered it. Use:
 *     Guard g;
 *     if (GUARD_TRY(g, "what")) { ...risky...; guard_leave(&g); } else { ...it faulted: undo, disable... }
 * Locals changed between GUARD_TRY and the fault must be volatile if read afterwards. Not for the game's own
 * normal paths (a half-run game routine leaves game state behind) - only for the mod's extra calls / reads.
 * How: crash_report (vectored, first chance) sees the fault on the guarding thread, points Eip at guard_land,
 * which restores the SEH chain head saved at entry and jumps back (__builtin_longjmp). */
static Guard *volatile g_guard;
static volatile DWORD g_guard_tid;

void guard_enter(Guard *g, const char *what)
{
    g->what = what;
    g->seh = (void *)__readfsdword(0);
    g->prev = g_guard_tid == GetCurrentThreadId() ? g_guard : NULL;
    g_guard = g;
    g_guard_tid = GetCurrentThreadId();
}

void guard_leave(Guard *g)
{
    if (g_guard == g) { g_guard = g->prev; if (!g->prev) g_guard_tid = 0; }
}

static void guard_land(void)
{
    Guard *g = g_guard;
    __writefsdword(0, (DWORD)g->seh);
    g_guard = g->prev;
    if (!g->prev) g_guard_tid = 0;
    __builtin_longjmp(g->jb, 1);
}

/* Crash reporter: logs the first few serious exceptions with the module and offset they happened
 * in (e.g. "Armymen.exe+0x47480" or "ddraw.dll+0x1234"), so a crash on the user's PC can be found.
 * These are first-chance exceptions: the game may handle some of them itself. */
static LONG CALLBACK crash_report(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    void *at = e->ExceptionRecord->ExceptionAddress;
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)at, &m))
        GetModuleFileNameA(m, path, sizeof path);
    const char *slash = strrchr(path, '\\');
    const char *name = slash ? slash + 1 : path;
    /* IsBadReadPtr probes fault inside KERNEL32 / KERNELBASE and are caught there: they used up the 5 reports
     * before a real crash on the user's PC (2026-11-19, the crash after Sarge died went unlogged) */
    if (!lstrcmpiA(name, "KERNEL32.DLL") || !lstrcmpiA(name, "KERNELBASE.dll")) return EXCEPTION_CONTINUE_SEARCH;
    int guarded = g_guard && g_guard_tid == GetCurrentThreadId() && code != EXCEPTION_STACK_OVERFLOW;
    static int reported, greported;
    if (guarded ? greported++ >= 20 : reported++ >= 8) {
        if (!guarded) return EXCEPTION_CONTINUE_SEARCH;
        e->ContextRecord->Eip = (DWORD)(uintptr_t)guard_land;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    CONTEXT *c = e->ContextRecord;
    DWORD *sp = (DWORD *)c->Esp;
    mlog("EXCEPTION %#lx at %p (%s+%#lx), data address %p | eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx "
         "edi %08lx esp %08lx | stack %08lx %08lx %08lx %08lx", code, at, name, (DWORD)((BYTE *)at - (BYTE *)m),
         e->ExceptionRecord->NumberParameters > 1 ? (void *)e->ExceptionRecord->ExceptionInformation[1] : NULL,
         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp,
         IsBadReadPtr(sp, 16) ? 0 : sp[0], IsBadReadPtr(sp, 16) ? 0 : sp[1],
         IsBadReadPtr(sp, 16) ? 0 : sp[2], IsBadReadPtr(sp, 16) ? 0 : sp[3]);
    /* 2026-11-22: the stack words that point into code (exe .text 0x401000..0x4a0000 or our dll) - the likely return
     * addresses, so the mod caller of a crashing game routine can be found (ddraw+offset: nm of the built dll) */
    {
        char line[600]; int len = 0, found = 0;
        BYTE *self = (BYTE *)g_self;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(self + ((IMAGE_DOS_HEADER *)self)->e_lfanew);
        DWORD self_n = nt->OptionalHeader.SizeOfImage;
        for (int i = 0; i < 256 && found < 16 && !IsBadReadPtr(sp + i, 4); i++) {
            DWORD v = sp[i];
            if (v >= 0x401000 && v < 0x4a0000)
                len += snprintf(line + len, sizeof line - len, " [%d]exe:%06lx", i, v), found++;
            else if (v > (DWORD)(uintptr_t)self && v < (DWORD)(uintptr_t)self + self_n)
                len += snprintf(line + len, sizeof line - len, " [%d]ddraw+%lx", i, v - (DWORD)(uintptr_t)self), found++;
            if (len > (int)sizeof line - 40) break;
        }
        mlog("EXCEPTION stack code words:%s", found ? line : " none");
    }
    if (!guarded) record_note_exception(code, at, name, (DWORD)((BYTE *)at - (BYTE *)m));
    if (guarded) {
        mlog("GUARDED: the fault above was inside \"%s\" - call abandoned, the game goes on", g_guard->what);
        c->Eip = (DWORD)(uintptr_t)guard_land;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* [debug] guard_test = 1: fault on purpose inside a guard, twice (a write through a bad pointer three calls deep,
 * and a call into game code with a bad argument), and log that both came back. */
static __attribute__((noinline)) void guard_test_poke(volatile int *p, int depth) { if (depth) guard_test_poke(p, depth - 1); else *p = 1; }
static void guard_self_test(const char *ini)
{
    if (!GetPrivateProfileIntA("debug", "guard_test", 0, ini)) return;
    Guard g;
    volatile int stage = 0;
    if (GUARD_TRY(g, "guard self-test 1 (bad write)")) { stage = 1; guard_test_poke((volatile int *)16, 3); stage = 2; guard_leave(&g); }
    mlog("guard self-test 1: %s (stage %d, guard %s)", stage == 1 ? "recovered" : "NOT as expected", stage, g_guard ? "still set - WRONG" : "cleared");
    stage = 0;
    if (GUARD_TRY(g, "guard self-test 2 (game routine, bad cell)")) { stage = 1; ((void (__fastcall *)(void *, void *))0x42f120)((void *)16, (void *)16); stage = 2; guard_leave(&g); }
    mlog("guard self-test 2: %s (stage %d, guard %s)", stage == 1 ? "recovered" : "NOT as expected", stage, g_guard ? "still set - WRONG" : "cleared");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        present_init();
        AddVectoredExceptionHandler(1, crash_report);
        const char *dpi = make_dpi_aware();
        config_load();
        mlog("DPI awareness: %s", dpi);
        report_compat_layers();
        patches_apply();
        update_cleanup();
        record_init();
        guard_self_test(mod_ini_path());
        hdsprites_start();                          /* upscaled pictures: read the catalogue on its own thread */
        hdworld_start();                            /* upscaled battlefield: open hdworld.pak */
        update_check_async(0);                      /* background GitHub Releases check (if [update] check=1) */
    } else if (reason == DLL_PROCESS_DETACH) {
        record_shutdown();
        if (g_log) fclose(g_log);
    }
    return TRUE;
}

/* ---------------------------------------------------------------- GUIDs (no dxguid dependency) */

static const GUID G_IUnknown  = {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID G_DD        = {0x6C14DB80,0xA733,0x11CE,{0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60}};
static const GUID G_DD2       = {0xB3A6F3E0,0x2B43,0x11CF,{0xA2,0xDE,0x00,0xAA,0x00,0xB9,0x33,0x56}};
static const GUID G_PAL       = {0x6C14DB84,0xA733,0x11CE,{0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60}};
static const GUID G_CLIP      = {0x6C14DB85,0xA733,0x11CE,{0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60}};

static void log_guid(const char *what, REFIID g)
{
    mlog("%s: unsupported interface {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", what,
         g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
         g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* ---------------------------------------------------------------- presenting */

static void make_fullscreen_window(DDraw *dd)
{
    if (!dd->hwnd || dd->restyled) return;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    dd->saved_style = GetWindowLongA(dd->hwnd, GWL_STYLE);
    dd->saved_exstyle = GetWindowLongA(dd->hwnd, GWL_EXSTYLE);
    SetWindowLongA(dd->hwnd, GWL_STYLE, (dd->saved_style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                   WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP | WS_VISIBLE);
    SetWindowLongA(dd->hwnd, GWL_EXSTYLE, dd->saved_exstyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
    SetWindowPos(dd->hwnd, HWND_TOP, 0, 0, sw, sh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    dd->restyled = 1;
    mlog("fullscreen window %dx%d", sw, sh);
}

DDraw *g_dd;

void dd_present(DDraw *dd, int frame_end)
{
    missionpath_screen_note();                      /* missionpath.c: restart vs fresh entry (menus seen) */
    gameplay_run(NULL, NULL);                       /* gameplay.c: notes every mission (re)start (also seen during loading) */
    if (!dd->primary || !dd->hwnd) return;
    if (dd->coop & DDSCL_FULLSCREEN) make_fullscreen_window(dd);
    if (frame_end) music_frame();                   /* music.c: [audio] am2_music switched -> restart the playing music */
    present_request(dd, frame_end);
}

/* Display thread, under present_lock. */
int dd_frame_source(DDraw *dd, FrameSource *s)
{
    Surf *p = dd->primary;
    if (!p || !dd->hwnd || !IsWindowVisible(dd->hwnd) || IsIconic(dd->hwnd)) return 0;
    static uint32_t grey[256];
    if (!grey[255]) for (int i = 0; i < 256; i++) grey[i] = 0xFF000000u | i * 0x010101u;

    s->hwnd = dd->hwnd;
    s->bits = p->bits;
    s->pitch = p->pitch;
    s->lut = dd->pal ? dd->pal->bgra : grey;
    return 1;
}

int dd_is_windowed(void) { return g_dd && !(g_dd->coop & DDSCL_FULLSCREEN); }


/* ---------------------------------------------------------------- windowed size */

/* Window size itself comes from our rewrite of FUN_00407ae0 (window.c). */
static WNDPROC g_game_wndproc;

/* Music during a modal loop (user: "if I use one of the menus on the title it loops the intro song until I
 * click off of it"). While a menu-bar menu is open or the window is dragged, Windows runs its own modal message
 * loop and the game loop stops: no new screen, no stream bookkeeping. Each music stream keeps playing its
 * looping DirectSound buffer (its 50 ms timeSetEvent timer 0x4697b0 only flags a refill, 0x80; the refill
 * 0x46a4f0 runs from the game loop), so the buffer replayed its last stretch. Servicing the streams from a
 * window timer (first fix) kept them fed, but the song still looped: the game moves on to the next screen /
 * track from its loop. Now the streams are PAUSED for the modal loop and resumed after it: stream records at
 * *0x6a5ba8 (count 0x6a5b8c, 0x4c bytes each), +0x18 = IDirectSoundBuffer* (FUN_00469820 GetCurrentPosition /
 * Lock on it); playing ones are Stop()ped and Play(0, 0, DSBPLAY_LOOPING)ed again - the play position is kept,
 * the game's refill bookkeeping (+0x3c last position) continues where it was.
 * 2026-11-21 (music.c work): those records are the RADIO VOICE streams (play flag 4 = SND_VOICE_FLAG, FUN_00468b50);
 * the MUSIC streams (flag 8 = SND_MUSIC_FLAG, FUN_00410d50) are a second table of the same design: *0x4e3b0c, count
 * 0x4e3b1c, 0x48 bytes, +0xc handle, +0x14 IDirectSoundBuffer*. Both tables are paused now (before, the music kept
 * looping its buffer - only voices were paused). */
#define STREAMS        (*(BYTE **)0x6a5ba8)        /* radio voice streams */
#define STREAM_COUNT   (*(unsigned *)0x6a5b8c)
#define STREAM_SIZE    0x4c
#define MUSIC_STREAMS  (*(BYTE **)0x4e3b0c)        /* music streams */
#define MUSIC_COUNT    (*(unsigned *)0x4e3b1c)
#define MUSIC_SIZE     0x48
#define MAX_PAUSED     16
#define DSB_OK         0             /* dsound.h DS_OK, DSBSTATUS_PLAYING, DSBPLAY_LOOPING; vtable: 9 GetStatus, */
#define DSB_PLAYING    1             /* 12 Play, 18 Stop (IDirectSoundBuffer) */
#define DSB_LOOPING    1
typedef HRESULT (WINAPI *DsbStatusFn)(void *, DWORD *);
typedef HRESULT (WINAPI *DsbStopFn)(void *);
typedef HRESULT (WINAPI *DsbPlayFn)(void *, DWORD, DWORD, DWORD);
static int g_modal_loops;
static DWORD g_modal_at;
static void *g_paused[MAX_PAUSED];
static int g_npaused;

/* Stop the playing buffers of one stream table (handle_at: the record's handle, -1 = unused; buffer_at: its
 * IDirectSoundBuffer*); returns how many were stopped. */
static int pause_streams(const char *what, BYTE *s, unsigned count, unsigned size, unsigned handle_at, unsigned buffer_at)
{
    int n = 0;
    if (g_cfg.log >= 2) mlog("modal loop: %s streams %p count %u", what, (void *)s, count);
    if (!s || s == (BYTE *)-1) return 0;
    for (unsigned i = 0; i < count && g_npaused < MAX_PAUSED; i++) {
        BYTE *r = s + i * size;
        void **b = *(void ***)(r + buffer_at);
        DWORD st = 0;
        if (g_cfg.log >= 2 && b) mlog("modal loop: %s stream %u: handle %d buffer %p", what, i, *(int *)(r + handle_at), (void *)b);
        /* unused records (handle -1) and released buffers (vtable NULL - crashed when the Graphics menu opened,
         * user 2026-11-17) are skipped */
        if (*(int *)(r + handle_at) == -1 || !b || IsBadReadPtr(b, 4) || !*(void **)b || IsBadReadPtr(*(void **)b, 0x50)) continue;
        if (((DsbStatusFn)(*(void ***)b)[9])(b, &st) != DSB_OK || !(st & DSB_PLAYING)) continue;
        ((DsbStopFn)(*(void ***)b)[18])(b);
        g_paused[g_npaused++] = b;
        n++;
    }
    return n;
}

static void modal_loop(HWND h, int enter)
{
    if (enter) {
        if (g_modal_loops++) return;
        g_modal_at = GetTickCount();
        g_npaused = 0;
        int m = pause_streams("music", MUSIC_STREAMS, MUSIC_COUNT, MUSIC_SIZE, 0xc, 0x14);
        int v = pause_streams("voice", STREAMS, STREAM_COUNT, STREAM_SIZE, 4, 0x18);
        mlog("modal loop: start, %d sound stream(s) paused (%d music, %d voice)", g_npaused, m, v);
    } else if (g_modal_loops > 0 && --g_modal_loops == 0) {
        for (int i = 0; i < g_npaused; i++)
            if (!IsBadReadPtr(g_paused[i], 4) && *(void **)g_paused[i]) ((DsbPlayFn)(*(void ***)g_paused[i])[12])(g_paused[i], 0, 0, DSB_LOOPING);
        mlog("modal loop: %lu ms, %d sound stream(s) resumed", GetTickCount() - g_modal_at, g_npaused);
        g_npaused = 0;
    }
}

static LRESULT CALLBACK wndproc_hook(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT handled;
    /* Alt+F4 closes the game (user 2026-11-17: it did not; GOG's IgnoreAltTab shim swallowed it - passed on by
     * system_keys_proc - and the game's own window procedure does not close on it) */
    /* the game PAUSES while its window is inactive (WM_ACTIVATEAPP 0 -> gpGame flags 1, the frame loop stops: no
     * picture, no sound bookkeeping) - logged, so a frozen stretch in a log can be matched (2026-11-21, win-pause hunt) */
    if (msg == WM_ACTIVATEAPP) mlog("window: %s", wp ? "game active again" : "game INACTIVE (another program in front) - the game pauses itself");
    if (update_message(h, msg, wp, lp)) return 0;
    record_message(h, msg, wp, lp);
    if (screenbtn_message(h, msg, wp)) return 0;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_MOUSEMOVE) modmenu_slider_mouse(msg, lp);                       /* screenbtn.c: a pressed button acts (release / timer) */
    if (msg == WM_SYSKEYDOWN && wp == VK_F4) { mlog("window: Alt+F4 - closing"); PostMessageA(h, WM_CLOSE, 0, 0); return 0; }
    if (msg == WM_ENTERMENULOOP || msg == WM_ENTERSIZEMOVE) modal_loop(h, 1);
    if (msg == WM_EXITMENULOOP || msg == WM_EXITSIZEMOVE) modal_loop(h, 0);
    if (resize_message(h, msg)) return 0;                              /* resize.c: new render size (between frames) */
    if (modern_message(h, msg, wp, lp)) return 0;                      /* modern.c: any key / click skips a video */
    if (zoom_message(msg, wp)) return 0;                               /* zoom.c: Ctrl+wheel, Ctrl+PgUp/PgDn */
    if (msg == WM_MOUSEWHEEL && controls_wheel(GET_WHEEL_DELTA_WPARAM(wp))) return 0;   /* controls.c: weapons */
    if (savestate_message(h, msg, wp, lp)) return 0;                   /* savestate.c: quicksave / quickload keys */
    if (overlay_message(h, msg, wp, lp)) return 0;                     /* panel.c: F9, F10, clicks on mod boxes */
    if (orders_message(h, msg, wp, lp)) return 0;                      /* orders.c: Esc cancels picking a command target */
    if (allies_message(h, msg, wp, lp)) return 0;                      /* allies.c: Esc puts the medkit away (no pause menu) */
    if (mouse_filter_message(h, msg, &wp, lp, &handled)) return handled;   /* mouse.c pointer mode */
    if (window_fullscreen_message(h, msg, wp, lp)) return 0;           /* window.c: maximise, Alt+Enter, F11 */
    if (msg == WM_ACTIVATE && g_cfg.borderless_fs && dd_is_windowed()) {   /* remembered fullscreen: once */
        static int applied;
        if (!applied++) window_set_fullscreen(h, 1);
    }
    if (msg == WM_COMMAND && menu_command(h, LOWORD(wp))) return 0;    /* menu.c: our Gameplay menu */
    if (msg == WM_ENTERSIZEMOVE) controls_sizing(1);                    /* dragging the frame: pointer free */
    if (msg == WM_SIZING) { window_keep_shape(h, wp, (RECT *)lp); return TRUE; }   /* window.c */
    if (msg == WM_EXITSIZEMOVE) controls_sizing(0);
    if (msg == WM_LBUTTONDOWN) screenbtn_lbuttondown(h, 1);           /* screenbtn.c: clicks on screen buttons */
    LRESULT r = CallWindowProcA(g_game_wndproc, h, msg, wp, lp);
    if (msg == WM_LBUTTONDOWN) screenbtn_lbuttondown(h, 0);
    if (msg == WM_INITMENUPOPUP) menu_popup((HMENU)wp);
    if (msg == WM_GETMINMAXINFO) window_minmax(h, (MINMAXINFO *)lp);    /* after the game: any size */
    if ((msg == WM_ACTIVATEAPP && !wp) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE))
        controls_release_pointer();                    /* Alt-Tab etc.: let the pointer go */
    if (msg == WM_ACTIVATE || msg == WM_SIZE || msg == WM_WINDOWPOSCHANGED) {
        menu_install(h);
        if (dd_is_windowed()) window_allow_resize(h);  /* window.c: drag to resize, maximise */
    }
    if (msg == WM_WINDOWPOSCHANGED && dd_is_windowed()) {
        window_fit_client(h);
        window_sync_game_rects(h);
        if (g_dd) dd_present(g_dd, 0);                   /* resized: show the picture at the new size */
    }
    return r;
}

/* ---------------------------------------------------------------- Windows key / Alt+Tab back
 * GOG installs goggame_win7.sdb, a shim database for Armymen.exe with IgnoreAltTab (+ ForceSimpleWindow,
 * EmulateSlowCPU, SingleProcAffinity). IgnoreAltTab puts a system-wide low-level keyboard hook into the game's
 * process that swallows the Windows key, Alt+Tab, Alt+Esc and Ctrl+Esc while the game runs - also when
 * another program is in front (user 2026-11-17: "windows key doesn't work at all when the game is running").
 * Low-level hooks are called newest first: ours, installed after the game window exists, hands these keys
 * straight to Windows (returns 0 without CallNextHookEx, so the shim's hook never sees them). Own thread with
 * its own message loop: a slow game frame cannot stall keyboard input. [window] system_keys = 1 (default). */
static HHOOK g_kb_hook;

static LRESULT CALLBACK system_keys_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        int alt = (k->flags & LLKHF_ALTDOWN) != 0, ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
        if (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN || (alt && (k->vkCode == VK_TAB || k->vkCode == VK_ESCAPE || k->vkCode == VK_F4)) ||
            (ctrl && k->vkCode == VK_ESCAPE))
            return 0;                                   /* to Windows, past the shim's hook */
    }
    return CallNextHookEx(g_kb_hook, code, wp, lp);
}

static DWORD WINAPI system_keys_thread(LPVOID unused)
{
    g_kb_hook = SetWindowsHookExA(WH_KEYBOARD_LL, system_keys_proc, g_self, 0);
    mlog("system keys: %s", g_kb_hook ? "Windows key / Alt+Tab handed to Windows (IgnoreAltTab shim bypassed)" : "hook failed");
    MSG m;
    while (g_kb_hook && GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
}

static void system_keys_start(void)
{
    static int started;
    if (started++ || !GetPrivateProfileIntA("window", "system_keys", 1, mod_ini_path())) return;
    const char *shims[] = { "AcLayers.dll", "AcGenral.dll", "AcXtrnal.dll", "AcSpecfc.dll" };
    for (int i = 0; i < 4; i++) if (GetModuleHandleA(shims[i])) mlog("compatibility shim DLL loaded: %s", shims[i]);
    HANDLE t = CreateThread(NULL, 0, system_keys_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void hook_window(DDraw *dd, HWND h)
{
    system_keys_start();
    if (g_game_wndproc || !h) return;
    g_game_wndproc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)wndproc_hook);
    menu_install(h);
    mlog("window %p hooked", h);
}

static void restore_mode(DDraw *dd)
{
    if (dd->changed_mode) {
        ChangeDisplaySettingsA(NULL, 0);
        dd->changed_mode = 0;
        mlog("display mode restored");
    }
    if (dd->restyled && dd->hwnd && IsWindow(dd->hwnd)) {
        SetWindowLongA(dd->hwnd, GWL_STYLE, dd->saved_style);
        SetWindowLongA(dd->hwnd, GWL_EXSTYLE, dd->saved_exstyle);
        SetWindowPos(dd->hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    dd->restyled = 0;
}

/* ---------------------------------------------------------------- IDirectDraw / IDirectDraw2 */

#define DD1(p) ((DDraw *)(p))
#define DD2(p) ((DDraw *)((BYTE *)(p) - offsetof(DDraw, vtbl2)))

static HRESULT dd_qi(DDraw *dd, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &G_IUnknown) || IsEqualGUID(riid, &G_DD)) *out = &dd->vtbl1;
    else if (IsEqualGUID(riid, &G_DD2)) *out = &dd->vtbl2;
    else { *out = NULL; log_guid("IDirectDraw::QueryInterface", riid); return E_NOINTERFACE; }
    dd->ref++;
    return S_OK;
}
static ULONG dd_addref(DDraw *dd) { return ++dd->ref; }
static ULONG dd_release(DDraw *dd)
{
    ULONG r = --dd->ref;
    if (r == 0) {
        mlog("IDirectDraw released");
        present_shutdown();
        restore_mode(dd);
        if (g_dd == dd) g_dd = NULL;
        HeapFree(GetProcessHeap(), 0, dd);
    }
    return r;
}

static HRESULT dd_create_clipper(DDraw *dd, DWORD f, void **out, IUnknown *o)
{ return clip_create((Clip **)out); }

static HRESULT dd_create_palette(DDraw *dd, DWORD f, LPPALETTEENTRY e, void **out, IUnknown *o)
{ return pal_create(dd, f, e, (Pal **)out); }

static HRESULT dd_create_surface(DDraw *dd, LPDDSURFACEDESC d, void **out, IUnknown *o)
{ return surf_create(dd, d, (Surf **)out); }

static HRESULT dd_enum_modes(DDraw *dd, DWORD f, LPDDSURFACEDESC d, LPVOID ctx, LPDDENUMMODESCALLBACK cb)
{
    static const int modes[][2] = { {640,480}, {800,600}, {1024,768}, {1280,720}, {1280,1024}, {1600,900}, {1920,1080} };
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        DDSURFACEDESC m = { .dwSize = sizeof m };
        m.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
        m.dwWidth = modes[i][0]; m.dwHeight = modes[i][1]; m.lPitch = modes[i][0];
        m.ddpfPixelFormat.dwSize = sizeof m.ddpfPixelFormat;
        m.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        m.ddpfPixelFormat.dwRGBBitCount = 8;
        if (cb(&m, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}

static HRESULT dd_get_caps(DDraw *dd, LPDDCAPS hw, LPDDCAPS hel)
{
    LPDDCAPS c[2] = { hw, hel };
    for (int i = 0; i < 2; i++) if (c[i]) {
        DWORD sz = c[i]->dwSize ? c[i]->dwSize : sizeof(DDCAPS);
        memset(c[i], 0, sz);
        c[i]->dwSize = sz;
        c[i]->dwCaps = DDCAPS_BLT | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_PALETTE | DDCAPS_BLTCOLORFILL;
        c[i]->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        c[i]->dwPalCaps = DDPCAPS_8BIT;
        c[i]->dwVidMemTotal = c[i]->dwVidMemFree = 256u << 20;
        c[i]->ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_BACKBUFFER |
                               DDSCAPS_FLIP | DDSCAPS_PALETTE | DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY;
    }
    return DD_OK;
}

static HRESULT dd_get_display_mode(DDraw *dd, LPDDSURFACEDESC d)
{
    int w = dd->mode_w, h = dd->mode_h;
    if (!w) desktop_size(&w, &h);
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = w; d->dwHeight = h; d->lPitch = w;
    d->ddpfPixelFormat.dwSize = sizeof d->ddpfPixelFormat;
    d->ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    d->ddpfPixelFormat.dwRGBBitCount = 8;
    return DD_OK;
}

static HRESULT dd_set_coop(DDraw *dd, HWND hwnd, DWORD flags)
{
    mlog("SetCooperativeLevel(hwnd %p, flags %#lx)", hwnd, flags);
    DWORD was = dd->coop;
    dd->coop = flags;
    if ((was & DDSCL_FULLSCREEN) && !(flags & DDSCL_FULLSCREEN)) restore_mode(dd);
    if (hwnd) { dd->hwnd = hwnd; hook_window(dd, hwnd); }
    if (dd->hwnd && !(flags & DDSCL_FULLSCREEN)) window_sync_game_rects(dd->hwnd);
    return DD_OK;
}

static HRESULT dd_set_mode(DDraw *dd, DWORD w, DWORD h, DWORD bpp)
{
    int dw, dh;
    if (dd->changed_mode) { ChangeDisplaySettingsA(NULL, 0); dd->changed_mode = 0; }
    desktop_size(&dw, &dh);
    mlog("SetDisplayMode(%lu, %lu, %lu bpp); desktop %dx%d", w, h, bpp, dw, dh);
    dd->mode_w = w; dd->mode_h = h;
    /* with scale > 1 the frame is enlarged on the desktop, so the screen mode never changes */
    if (g_cfg.scale == 1 && ((int)w != dw || (int)h != dh)) {
        /* real mode change, but always 32-bit: we convert the game's 8-bit pixels ourselves */
        DEVMODEA dm = { .dmSize = sizeof dm };
        dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmBitsPerPel = 32;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        LONG r = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
        if (r == DISP_CHANGE_SUCCESSFUL) dd->changed_mode = 1;
        else mlog("  32-bit %lux%lu mode not available (%ld); showing centred on the desktop", w, h, r);
    }
    if (dd->restyled) {   /* keep the window covering the (possibly new) screen */
        dd->restyled = 0;
        SetWindowLongA(dd->hwnd, GWL_STYLE, dd->saved_style);
        SetWindowLongA(dd->hwnd, GWL_EXSTYLE, dd->saved_exstyle);
    }
    return DD_OK;
}

static HRESULT dd_restore_mode(DDraw *dd) { restore_mode(dd); dd->mode_w = dd->mode_h = 0; return DD_OK; }

static HRESULT dd_vidmem(DDraw *dd, LPDDSCAPS caps, LPDWORD total, LPDWORD freem)
{
    if (total) *total = 256u << 20;
    if (freem) *freem = 256u << 20;
    return DD_OK;
}

/* v1 thunks */
static HRESULT WINAPI d1_qi(void *p, REFIID r, void **o) { return dd_qi(DD1(p), r, o); }
static ULONG   WINAPI d1_addref(void *p) { return dd_addref(DD1(p)); }
static ULONG   WINAPI d1_release(void *p) { return dd_release(DD1(p)); }
static HRESULT WINAPI d1_compact(void *p) { return DD_OK; }
static HRESULT WINAPI d1_create_clipper(void *p, DWORD f, void **o, IUnknown *u) { return dd_create_clipper(DD1(p), f, o, u); }
static HRESULT WINAPI d1_create_palette(void *p, DWORD f, LPPALETTEENTRY e, void **o, IUnknown *u) { return dd_create_palette(DD1(p), f, e, o, u); }
static HRESULT WINAPI d1_create_surface(void *p, LPDDSURFACEDESC d, void **o, IUnknown *u) { return dd_create_surface(DD1(p), d, o, u); }
static HRESULT WINAPI d1_duplicate(void *p, void *s, void **o) { return DDERR_UNSUPPORTED; }
static HRESULT WINAPI d1_enum_modes(void *p, DWORD f, LPDDSURFACEDESC d, LPVOID c, LPDDENUMMODESCALLBACK cb) { return dd_enum_modes(DD1(p), f, d, c, cb); }
static HRESULT WINAPI d1_enum_surfaces(void *p, DWORD f, LPDDSURFACEDESC d, LPVOID c, LPDDENUMSURFACESCALLBACK cb) { return DD_OK; }
static HRESULT WINAPI d1_flip_to_gdi(void *p) { return DD_OK; }
static HRESULT WINAPI d1_get_caps(void *p, LPDDCAPS a, LPDDCAPS b) { return dd_get_caps(DD1(p), a, b); }
static HRESULT WINAPI d1_get_display_mode(void *p, LPDDSURFACEDESC d) { return dd_get_display_mode(DD1(p), d); }
static HRESULT WINAPI d1_get_fourcc(void *p, LPDWORD n, LPDWORD c) { if (n) *n = 0; return DD_OK; }
static HRESULT WINAPI d1_get_gdi_surface(void *p, void **o) { *o = NULL; return DDERR_NOTFOUND; }
static HRESULT WINAPI d1_get_freq(void *p, LPDWORD f) { *f = 60; return DD_OK; }
static HRESULT WINAPI d1_get_scanline(void *p, LPDWORD l) { *l = 0; return DD_OK; }
static HRESULT WINAPI d1_get_vblank(void *p, LPBOOL b) { *b = TRUE; return DD_OK; }
static HRESULT WINAPI d1_initialize(void *p, GUID *g) { return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI d1_restore_mode(void *p) { return dd_restore_mode(DD1(p)); }
static HRESULT WINAPI d1_set_coop(void *p, HWND h, DWORD f) { return dd_set_coop(DD1(p), h, f); }
static HRESULT WINAPI d1_set_mode(void *p, DWORD w, DWORD h, DWORD b) { return dd_set_mode(DD1(p), w, h, b); }
static HRESULT WINAPI d1_wait_vblank(void *p, DWORD f, HANDLE e) { return DD_OK; }

/* v2 thunks */
static HRESULT WINAPI d2_qi(void *p, REFIID r, void **o) { return dd_qi(DD2(p), r, o); }
static ULONG   WINAPI d2_addref(void *p) { return dd_addref(DD2(p)); }
static ULONG   WINAPI d2_release(void *p) { return dd_release(DD2(p)); }
static HRESULT WINAPI d2_create_clipper(void *p, DWORD f, void **o, IUnknown *u) { return dd_create_clipper(DD2(p), f, o, u); }
static HRESULT WINAPI d2_create_palette(void *p, DWORD f, LPPALETTEENTRY e, void **o, IUnknown *u) { return dd_create_palette(DD2(p), f, e, o, u); }
static HRESULT WINAPI d2_create_surface(void *p, LPDDSURFACEDESC d, void **o, IUnknown *u) { return dd_create_surface(DD2(p), d, o, u); }
static HRESULT WINAPI d2_enum_modes(void *p, DWORD f, LPDDSURFACEDESC d, LPVOID c, LPDDENUMMODESCALLBACK cb) { return dd_enum_modes(DD2(p), f, d, c, cb); }
static HRESULT WINAPI d2_get_caps(void *p, LPDDCAPS a, LPDDCAPS b) { return dd_get_caps(DD2(p), a, b); }
static HRESULT WINAPI d2_get_display_mode(void *p, LPDDSURFACEDESC d) { return dd_get_display_mode(DD2(p), d); }
static HRESULT WINAPI d2_restore_mode(void *p) { return dd_restore_mode(DD2(p)); }
static HRESULT WINAPI d2_set_coop(void *p, HWND h, DWORD f) { return dd_set_coop(DD2(p), h, f); }
static HRESULT WINAPI d2_set_mode(void *p, DWORD w, DWORD h, DWORD b, DWORD rate, DWORD f) { return dd_set_mode(DD2(p), w, h, b); }
static HRESULT WINAPI d2_vidmem(void *p, LPDDSCAPS c, LPDWORD t, LPDWORD f) { return dd_vidmem(DD2(p), c, t, f); }

static const void *const vt_dd1[] = {
    d1_qi, d1_addref, d1_release, d1_compact, d1_create_clipper, d1_create_palette, d1_create_surface,
    d1_duplicate, d1_enum_modes, d1_enum_surfaces, d1_flip_to_gdi, d1_get_caps, d1_get_display_mode,
    d1_get_fourcc, d1_get_gdi_surface, d1_get_freq, d1_get_scanline, d1_get_vblank, d1_initialize,
    d1_restore_mode, d1_set_coop, d1_set_mode, d1_wait_vblank,
};
/* Methods that ignore `this` can be shared between the two vtables. */
static const void *const vt_dd2[] = {
    d2_qi, d2_addref, d2_release, d1_compact, d2_create_clipper, d2_create_palette, d2_create_surface,
    d1_duplicate, d2_enum_modes, d1_enum_surfaces, d1_flip_to_gdi, d2_get_caps, d2_get_display_mode,
    d1_get_fourcc, d1_get_gdi_surface, d1_get_freq, d1_get_scanline, d1_get_vblank, d1_initialize,
    d2_restore_mode, d2_set_coop, d2_set_mode, d1_wait_vblank, d2_vidmem,
};

HRESULT WINAPI DirectDrawCreate(GUID *guid, LPDIRECTDRAW *out, IUnknown *outer)
{
    DDraw *dd = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *dd);
    if (!dd) return DDERR_OUTOFMEMORY;
    dd->vtbl1 = vt_dd1;
    dd->vtbl2 = vt_dd2;
    dd->ref = 1;
    g_dd = dd;
    *out = (LPDIRECTDRAW)&dd->vtbl1;
    mlog("DirectDrawCreate -> %p", dd);
    return DD_OK;
}

HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx)
{
    cb(NULL, "Primary Display Driver", "display", ctx);
    return DD_OK;
}

/* ---------------------------------------------------------------- IDirectDrawPalette */

void pal_lut(Pal *p)
{
    for (int i = 0; i < 256; i++)
        p->bgra[i] = 0xFF000000u | (p->e[i].peRed << 16) | (p->e[i].peGreen << 8) | p->e[i].peBlue;
    /* index 0 is the game's colour key (cyan in Army Men's palettes) - never a real colour. Surfaces cleared
     * to 0 and shown between screens flashed cyan (user 2026-11-17): it is shown as black. The game's own
     * entries (GetEntries, p->e) are untouched. */
    p->bgra[0] = 0xFF000000u;
}

static HRESULT WINAPI pal_qi(void *p, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown) || IsEqualGUID(r, &G_PAL)) { ((Pal *)p)->ref++; *o = p; return S_OK; }
    *o = NULL; log_guid("IDirectDrawPalette::QueryInterface", r); return E_NOINTERFACE;
}
static ULONG WINAPI pal_addref(void *p) { return ++((Pal *)p)->ref; }
static ULONG WINAPI pal_release(void *p)
{
    Pal *pl = p;
    ULONG r = --pl->ref;
    if (!r) {
        if (pl->dd && pl->dd->pal == pl) pl->dd->pal = NULL;
        HeapFree(GetProcessHeap(), 0, pl);
    }
    return r;
}
static HRESULT WINAPI pal_get_caps(void *p, LPDWORD c) { *c = DDPCAPS_8BIT | DDPCAPS_ALLOW256; return DD_OK; }
static HRESULT WINAPI pal_get_entries(void *p, DWORD f, DWORD start, DWORD n, LPPALETTEENTRY e)
{
    if (start > 256 || n > 256 - start) return DDERR_INVALIDPARAMS;
    memcpy(e, ((Pal *)p)->e + start, n * sizeof *e);
    return DD_OK;
}
static HRESULT WINAPI pal_init(void *p, void *dd, DWORD f, LPPALETTEENTRY e) { return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI pal_set_entries(void *p, DWORD f, DWORD start, DWORD n, LPPALETTEENTRY e)
{
    Pal *pl = p;
    if (start > 256 || n > 256 - start) return DDERR_INVALIDPARAMS;
    present_lock();
    memcpy(pl->e + start, e, n * sizeof *e);
    modern_palette(pl, start, n);                  /* modern.c: colour-blind enemy colour */
    pal_lut(pl);
    present_unlock();
    /* On real hardware a palette change on the screen shows at once (fades rely on this). */
    if (pl->dd && pl->dd->pal == pl) dd_present(pl->dd, 0);
    return DD_OK;
}
static const void *const vt_pal[] = {
    pal_qi, pal_addref, pal_release, pal_get_caps, pal_get_entries, pal_init, pal_set_entries,
};

HRESULT pal_create(DDraw *dd, DWORD flags, const PALETTEENTRY *e, Pal **out)
{
    Pal *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *p);
    if (!p) return DDERR_OUTOFMEMORY;
    p->vtbl = vt_pal;
    p->ref = 1;
    p->dd = dd;
    if (e) memcpy(p->e, e, sizeof p->e);
    pal_lut(p);
    *out = p;
    return DD_OK;
}

/* ---------------------------------------------------------------- IDirectDrawClipper */

static HRESULT WINAPI clip_qi(void *p, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown) || IsEqualGUID(r, &G_CLIP)) { ((Clip *)p)->ref++; *o = p; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI clip_addref(void *p) { return ++((Clip *)p)->ref; }
static ULONG WINAPI clip_release(void *p)
{
    ULONG r = --((Clip *)p)->ref;
    if (!r) HeapFree(GetProcessHeap(), 0, p);
    return r;
}
static HRESULT WINAPI clip_get_list(void *p, LPRECT r, LPRGNDATA d, LPDWORD n) { return DDERR_NOCLIPLIST; }
static HRESULT WINAPI clip_get_hwnd(void *p, HWND *h) { *h = ((Clip *)p)->hwnd; return DD_OK; }
static HRESULT WINAPI clip_init(void *p, void *dd, DWORD f) { return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI clip_changed(void *p, BOOL *b) { *b = FALSE; return DD_OK; }
static HRESULT WINAPI clip_set_list(void *p, LPRGNDATA d, DWORD f) { return DD_OK; }
static HRESULT WINAPI clip_set_hwnd(void *p, DWORD f, HWND h) { ((Clip *)p)->hwnd = h; return DD_OK; }
static const void *const vt_clip[] = {
    clip_qi, clip_addref, clip_release, clip_get_list, clip_get_hwnd, clip_init, clip_changed,
    clip_set_list, clip_set_hwnd,
};

HRESULT clip_create(Clip **out)
{
    Clip *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *c);
    if (!c) return DDERR_OUTOFMEMORY;
    c->vtbl = vt_clip;
    c->ref = 1;
    *out = c;
    return DD_OK;
}
