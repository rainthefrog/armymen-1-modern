/* The game's own clock, separate from the system clock.
 *
 * All gameplay timing in Armymen.exe comes from GetTickCount (the frame loop FUN_00407460 turns
 * it into a per-frame time step, capped at 66 ms). We replace that import, for the exe only, with
 * a clock that runs at [clock] speed times real time and can be paused. Windows, DirectSound and
 * the video player keep real time, so audio and videos are unaffected; the game's 50 ms
 * multimedia timers (audio/video streaming) also stay on real time.
 *
 * Hotkeys ([clock] hotkeys=1), only while the game window is in front:
 *   Ctrl + '-'   slower (x0.8)        Ctrl + '='   faster (x1.25)
 *   Ctrl + '0'   back to the ini speed  Pause key  pause / resume the game clock
 * Note: because each frame's time step is capped at 66 ms, very high speeds at low frame rates
 * stop speeding the game up further.
 */
#include "common.h"

static CRITICAL_SECTION g_cs;
static LARGE_INTEGER g_freq, g_last;
static double g_ms;          /* game milliseconds since start */
static DWORD g_base;         /* real GetTickCount at start, so values look like normal ticks */
static double g_speed;
static int g_paused;
static DWORD g_next_poll;

static void advance(void)    /* call with g_cs held */
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_paused) g_ms += (double)(now.QuadPart - g_last.QuadPart) * 1000.0 / (double)g_freq.QuadPart * g_speed;
    g_last = now;
}

static void set_speed(double s, int paused)
{
    EnterCriticalSection(&g_cs);
    advance();               /* time so far counts at the old speed */
    if (s < 0.05) s = 0.05;
    if (s > 8.0) s = 8.0;
    g_speed = s;
    g_paused = paused;
    LeaveCriticalSection(&g_cs);
    mlog("clock: speed %.2f%s", s, paused ? " (paused)" : "");
}

static void poll_trace_key(void);
static int pressed(int vk)   /* went down since the last poll */
{
    return (GetAsyncKeyState(vk) & 1) != 0;
}

static void poll_keys(void)
{
    DWORD now = GetTickCount();          /* real time; our hook only covers the exe's import */
    if ((LONG)(now - g_next_poll) < 0) return;
    g_next_poll = now + 15;
    HWND game = g_dd ? g_dd->hwnd : NULL;
    if (!game || GetForegroundWindow() != game) return;
    poll_trace_key();
    int ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
    /* read every key each poll so stale "pressed" bits don't fire later */
    int minus = pressed(VK_OEM_MINUS) | pressed(VK_SUBTRACT);
    int plus = pressed(VK_OEM_PLUS) | pressed(VK_ADD);
    int zero = pressed('0') | pressed(VK_NUMPAD0);
    int pause = pressed(VK_PAUSE);
    if (pause) set_speed(g_speed, !g_paused);
    else if (ctrl && minus) set_speed(g_speed * 0.8, g_paused);
    else if (ctrl && plus) set_speed(g_speed * 1.25, g_paused);
    else if (ctrl && zero) set_speed(g_cfg.clock_speed, g_paused);
}

/* ---------------------------------------------------------------- locked to the display
 * In gameplay the game clock advances in whole display refreshes: after each frame is on screen
 * (present_request -> clock_frame_shown) it moves on by n x the refresh period, n = refreshes that
 * really passed (usually 1). Every shown frame then moves the world by the same amount; with real
 * time, the per-frame step jittered with when the frame happened to be computed, and the motion
 * stuttered. If the locked time drifts more than 3 refreshes from real time it is set back to it,
 * and when no frame is shown for 100 ms (menus, loading) the clock simply runs on real time.
 * [clock] lock_to_display (default 1). */
static double g_lock_ms;     /* locked game time, valid while g_locked */
static int g_locked;
static LARGE_INTEGER g_lock_at;  /* real time of the last frame shown */
static double g_period = 1000.0 / 60.0;

/* The frame period the display paces at (2026-11-17, 120 Hz+ support): the refresh of the monitor the
 * WINDOW is on (was: the primary monitor - a game on a 120 Hz second screen would have stepped 16.7 ms
 * per 8.3 ms frame, double speed), or 1000 / [display] fps_limit when vsync is off. */
double clock_frame_period(void)
{
    if (!g_cfg.vsync && g_cfg.fps_limit > 0) return 1000.0 / g_cfg.fps_limit;
    DEVMODEA dm = { 0 };
    dm.dmSize = sizeof dm;
    MONITORINFOEXA mi = { .cbSize = sizeof mi };
    HMONITOR mon = g_dd && g_dd->hwnd ? MonitorFromWindow(g_dd->hwnd, MONITOR_DEFAULTTONEAREST) : NULL;
    const char *dev = mon && GetMonitorInfoA(mon, (MONITORINFO *)&mi) ? mi.szDevice : NULL;
    if (EnumDisplaySettingsA(dev, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 24 && dm.dmDisplayFrequency <= 500)
        return 1000.0 / dm.dmDisplayFrequency;
    return 1000.0 / 60.0;
}
#define refresh_period clock_frame_period

void clock_frame_shown(int gameplay)
{
    if (!g_cfg.clock_lock) return;
    if (!g_cfg.vsync && g_cfg.fps_limit <= 0) return;   /* uncapped: frames follow no period - real time */
    EnterCriticalSection(&g_cs);
    advance();                                     /* real game time in g_ms */
    LARGE_INTEGER now = g_last;
    double real_dt = g_locked ? (double)(now.QuadPart - g_lock_at.QuadPart) * 1000.0 / (double)g_freq.QuadPart : 0;
    if (!gameplay || !g_locked || real_dt > 100) {
        g_locked = gameplay;
        g_lock_ms = g_ms;
        g_period = refresh_period();
    } else {
        static LARGE_INTEGER checked;                  /* the window may move to another monitor: re-read every 2 s */
        if ((double)(now.QuadPart - checked.QuadPart) > 2.0 * (double)g_freq.QuadPart) {
            checked = now;
            double p = refresh_period();
            if (p != g_period) { mlog("clock: frame period %.2f ms (%.1f Hz)", p, 1000.0 / p); g_period = p; }
        }
        /* whole refreshes, plus a GENTLE pull toward real time (2026-11-17): the old hard reset when more
         * than 3 refreshes off set game time BACK (the period is a whole-Hz guess: 59.94 Hz reads 59), and
         * the game's unsigned frame step then wrapped to a 66 ms jump - a hitch every few seconds on any
         * display that is not exactly 60 Hz. Now 10 % of the drift per frame, at most a quarter refresh,
         * never backwards; far behind (a long stall) catches up at once. */
        /* 2026-11-20 (stutter): n was round(time since the last frame / refresh). The display keeps one frame in
         * hand (present.c: take, then wait for the refresh slot), so a frame that arrives up to ~14 ms late is
         * still shown at the very next refresh - but 8.4 ms late already rounded to n = 2: the world moved two
         * refreshes in a frame shown for one, and the next frame (which came "early") one more: a jump on every
         * slow frame (game work 25-31 ms; the user's spike frames). Now n counts whole refreshes the game clock has
         * really FALLEN BEHIND after one refresh's step - 2 only when a whole refresh is missing (the display did
         * skip one); smaller lateness is taken up by the gentle pull below, spread over the next frames. */
        double one = g_period * g_speed, behind = g_ms - (g_lock_ms + one);
        int n = behind >= one ? 1 + (int)(behind / one) : 1;
        double step = n * one;
        double drift = g_ms - (g_lock_ms + step), lim = 0.25 * g_period * g_speed;
        double corr = drift > 250.0 * g_speed ? drift : drift * 0.1;
        if (drift <= 250.0 * g_speed) corr = corr > lim ? lim : corr < -lim ? -lim : corr;
        step += corr;
        /* never more than one refresh AHEAD of real time (2026-11-21, win-pause hunt): frames that come faster than
         * the refresh (display not pacing - seen in Wine at ~100 frames/s on a "60 Hz" desktop; vsync off without
         * fps_limit is skipped above) each stepped at least 3/4 refresh, so the locked time ran ahead of real time
         * without limit (17 s after 80 s of play in Wine). The first stand-still after that - the game's own 100 ms
         * wait when play resumes, the deadlines around the mission end - unlocks the clock (no frame for 100 ms),
         * and since game time never goes backwards it stood still until real time had caught up: seconds of frozen
         * picture ("stall" in PeekMessage of the run loop 0x4014e0, FUN_00424c70 waiting for gpGame +0x34). */
        if (g_lock_ms + step > g_ms + one) {
            static DWORD said, held;
            step = g_ms + one - g_lock_ms;
            held++;
            if (GetTickCount() - said > 10000) {     /* rare on a paced display: say it, at most every 10 s */
                said = GetTickCount();
                mlog("clock: frames faster than the refresh (%.2f ms) - game time held to real time (%lu frames)", g_period, held);
            }
        }
        if (step < 0) step = 0;
        if (!g_paused) g_lock_ms += step;
    }
    g_lock_at = now;
    LeaveCriticalSection(&g_cs);
}

static int g_trace_frames;
static LARGE_INTEGER g_trace_end;
static DWORD g_last_t; static int g_have_t;  /* the last value handed to the game (never backwards) */

/* savestate.c (2026-11-21), after a quickload: the game clock is put back to the value it had at the save (the
 * restored game state holds time stamps of then). The only place where it goes backwards on purpose. */
void clock_restore(DWORD tick)
{
    EnterCriticalSection(&g_cs);
    advance();
    g_ms = (double)(DWORD)(tick - g_base);
    g_lock_ms = g_ms;
    g_last_t = tick; g_have_t = 1;
    LeaveCriticalSection(&g_cs);
}
int clock_tracing(void)
{
    if (!g_trace_end.QuadPart) return 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart < g_trace_end.QuadPart) return 1;
    g_trace_end.QuadPart = 0;
    mlog("trace: end (%d frames)", g_trace_frames);
    return 0;
}

static DWORD WINAPI game_GetTickCount(void)
{
    if (g_cfg.clock_keys) poll_keys();
    EnterCriticalSection(&g_cs);
    advance();
    double ms = g_ms;
    if (g_locked) {                                /* locked: the time of the frame being made */
        double since = (double)(g_last.QuadPart - g_lock_at.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
        if (since > 100) g_locked = 0;             /* no frame shown for a while: real time again */
        else ms = g_lock_ms;
    }
    DWORD t = g_base + (DWORD)(unsigned long long)ms;
    /* never backwards (locked -> real time, speed changes): the game's frame step is unsigned and a smaller
     * value than last time wraps into a 66 ms step */
    if (g_have_t && (LONG)(t - g_last_t) < 0) t = g_last_t;
    g_last_t = t; g_have_t = 1;
    LeaveCriticalSection(&g_cs);
    return t;
}

/* Ctrl + Shift + T: log 5 s of frame timing (present.c writes a line per shown frame). */
static void poll_trace_key(void)
{
    static int was;
    int on = GetAsyncKeyState(VK_CONTROL) < 0 && GetAsyncKeyState(VK_SHIFT) < 0 && GetAsyncKeyState('T') < 0;
    if (on && !was) {
        QueryPerformanceCounter(&g_trace_end);
        g_trace_end.QuadPart += g_freq.QuadPart * 5;
        g_trace_frames = 0;
        mlog("trace: start - 5 s of frames (refresh %.2f ms, lock_to_display %d, smooth_scroll %d)", refresh_period(), g_cfg.clock_lock, g_cfg.smooth_scroll);
    }
    was = on;
}
/* [debug] trace_seconds (controls.c, first gameplay frame): a trace started without the key (user 2026-11-17) */
void clock_trace_start(int seconds)
{
    if (seconds <= 0) return;
    QueryPerformanceCounter(&g_trace_end);
    g_trace_end.QuadPart += g_freq.QuadPart * seconds;
    g_trace_frames = 0;
    mlog("trace: start - %d s of frames (refresh %.2f ms, lock_to_display %d, smooth_scroll %d)", seconds, refresh_period(), g_cfg.clock_lock, g_cfg.smooth_scroll);
}
void clock_trace_frame(const char *line) { g_trace_frames++; mlog("trace: %s", line); }

void clock_install(void)
{
    InitializeCriticalSection(&g_cs);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);
    g_base = GetTickCount();
    g_speed = g_cfg.clock_speed > 0 ? g_cfg.clock_speed : 1.0;
    void *old = iat_hook("kernel32.dll", "GetTickCount", (void *)game_GetTickCount);
    mlog("clock: GetTickCount %s (speed %.2f)", old ? "now runs on the game clock" : "NOT hooked", g_speed);
}

/* The game's own clock in milliseconds (speed and pause applied), for mod code that must run on
 * game time (gameplay.c). */
DWORD clock_game_ms(void) { return game_GetTickCount(); }

/* For the settings panel (panel.c): pause / resume, read and set the speed. */
void clock_set_paused(int paused) { set_speed(g_speed, paused); }
int clock_is_paused(void) { return g_paused; }
double clock_speed_now(void) { return g_speed; }
void clock_set_speed(double s) { set_speed(s, g_paused); }


/* present.c (freeze report): how far the time last handed to the game is ahead of the game clock (ms; > 0 = the game
 * waits for the clock - it never goes backwards), whether the clock is paused (Pause key) and that last time. */
int clock_ahead_ms(int *paused, DWORD *handed)
{
    EnterCriticalSection(&g_cs);
    advance();
    int ahead = g_have_t ? (int)(LONG)(g_last_t - (g_base + (DWORD)(unsigned long long)g_ms)) : 0;
    if (paused) *paused = g_paused;
    if (handed) *handed = g_last_t;
    LeaveCriticalSection(&g_cs);
    return ahead;
}
