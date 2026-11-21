/* ============================================================================================
 * window.c - REWRITE of the game's window-rectangle function
 *
 * Replaces: FUN_00407ae0  (hooked in patches.c)
 * Called by: FUN_00407cc0 (window setup, runs before DirectDraw is created)
 * Convention: thiscall. ecx = the main window object (the same object that DAT_004d80fc points
 *             to, see GameWindow below). One stack argument: RECT *out. Returns out.
 *
 * What the caller does with the result: moves the window to *out, then reads the client rect into
 * GAME_CLIENT_RECT and (in screen coordinates) GAME_CLIENT_SCREEN_RECT. The windowed game blits
 * every frame to GAME_CLIENT_SCREEN_RECT.
 *
 * Original behaviour (kept):
 *   - render size = GameWindow.render_w/h (patched from [display] width/height in patches.c)
 *   - frame source rect and full-screen rect = 0,0,w,h
 *   - fullscreen: window rect = 0,0,w,h
 *   - windowed:  caption + menu window (style 0xce0000), centred in the work area; the mouse is
 *                put in the middle of the window
 *   - GameWindow.window_w/h = outer window size. The main window procedure (around 0x408c39)
 *     answers WM_GETMINMAXINFO with these: max size = window_w/h, min size = half.
 * Changed:
 *   - windowed client area = render size x [display] scale (the game's own frame blit then
 *     enlarges each frame by exactly that whole number)
 *   - no extra 4-pixel border around the client area (the original slightly stretched every
 *     windowed frame to fit it)
 * ============================================================================================ */
#include "common.h"

/* Fields of the main window object used here (offsets from the object start). */
typedef struct GameWindow {
    BYTE  _pad0[0xc0];
    int   render_w;            /* +0xc0  game picture size (640x480 originally) */
    int   render_h;            /* +0xc4 */
    BYTE  _pad1[0x100 - 0xc8];
    int   window_w;            /* +0x100 outer window size (windowed) / screen size (fullscreen) */
    int   window_h;            /* +0x104 */
} GameWindow;   /* struct GameWindow in common.h */

/* Game globals this function writes. */
#define GAME_FRAME_SRC_RECT   (*(RECT *)0x4e2e18)   /* part of the back buffer shown each frame */
#define GAME_FULL_RECT        (*(RECT *)0x4e2e00)
#define GAME_CURSOR_X         (*(int *)0x4fe3e8)    /* game cursor, game pixels */
#define GAME_CURSOR_Y         (*(int *)0x4fe3ec)
#define GAME_IS_FULLSCREEN    (**(int **)0x4b41e4)  /* 0 = windowed */

#define WINDOW_STYLE          0xce0000              /* caption, system menu, min/max boxes */

/* Outer window rectangle for a client area of cw x ch, centred in the desktop work area. */
static RECT centred_window(int cw, int ch)
{
    RECT outer = { 0, 0, cw, ch };
    AdjustWindowRectEx(&outer, WINDOW_STYLE, TRUE /* has a menu bar */, 0);
    int ow = outer.right - outer.left, oh = outer.bottom - outer.top;

    RECT work;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) work = (RECT){ 0, 0, ow, oh };
    int x = work.left + ((work.right - work.left) - ow) / 2;
    int y = work.top + ((work.bottom - work.top) - oh) / 2;
    if (x < work.left) x = work.left;     /* bigger than the screen: keep the title bar visible */
    if (y < work.top) y = work.top;
    return (RECT){ x, y, x + ow, y + oh };
}

RECT *__attribute__((thiscall)) game_window_rect(GameWindow *win, RECT *out)
{
    int w = win->render_w, h = win->render_h;
    GAME_FRAME_SRC_RECT = (RECT){ 0, 0, w, h };
    GAME_FULL_RECT = (RECT){ 0, 0, w, h };

    RECT r = { 0, 0, w, h };
    win->window_w = w;
    win->window_h = h;

    if (!GAME_IS_FULLSCREEN && resize_in_progress()) {
        /* resize.c runs the game's display set-up again for a new size: the window stays where it is */
        HWND hw = *(HWND *)((BYTE *)win + 0x1c);
        RECT cur;
        if (hw && GetWindowRect(hw, &cur)) {
            win->window_w = cur.right - cur.left;
            win->window_h = cur.bottom - cur.top;
            *out = cur;
            return out;
        }
    }
    if (!GAME_IS_FULLSCREEN && window_is_fullscreen()) {
        /* borderless fullscreen (below): the game re-places its window during set-up - keep it covering the monitor */
        MONITORINFO mi = { sizeof mi };
        HWND hw = *(HWND *)((BYTE *)win + 0x1c);
        if (hw && GetMonitorInfoA(MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST), &mi)) {
            r = mi.rcMonitor;
            win->window_w = r.right - r.left;
            win->window_h = r.bottom - r.top;
            *out = r;
            return out;
        }
    }
    if (!GAME_IS_FULLSCREEN) {
        int scale = g_cfg.scale < 1 ? 1 : g_cfg.scale;
        r = centred_window(w * scale, h * scale);
        win->window_w = r.right - r.left;
        win->window_h = r.bottom - r.top;

        GAME_CURSOR_X = w / 2;
        GAME_CURSOR_Y = h / 2;
        SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        mlog("window: %dx%d game pixels x%d = %dx%d client, window %ld,%ld %dx%d",
             w, h, scale, w * scale, h * scale, r.left, r.top, win->window_w, win->window_h);
    }
    *out = r;
    return out;
}

/* Called from the window hook (ddraw.c) after every move/resize in windowed mode.
 * AdjustWindowRectEx's border sizes can differ from what the window really gets (themes, Wine),
 * which would leave the client area a few pixels off the exact game size x scale. Measure the real
 * client area and grow/shrink the window once to make it exact. Leaves maximised/minimised
 * windows and user resizes after the first correction alone. */
void window_fit_client(HWND h)
{
    static int corrections;
    static DWORD first;
    if (!first) first = GetTickCount();
    /* only while the window is being set up: afterwards the size is the user's (drag to resize) */
    if (corrections >= 2 || GetTickCount() - first > 3000 || IsIconic(h) || IsZoomed(h)) return;
    int scale = g_cfg.scale < 1 ? 1 : g_cfg.scale;
    int want_w = g_cfg.width * scale, want_h = g_cfg.height * scale;
    RECT client, outer;
    if (!GetClientRect(h, &client) || !GetWindowRect(h, &outer)) return;
    int dw = want_w - client.right, dh = want_h - client.bottom;
    if (dw == 0 && dh == 0) return;
    corrections++;
    int ow = (outer.right - outer.left) + dw, oh = (outer.bottom - outer.top) + dh;
    GameWindow *win = *(GameWindow **)0x4d80fc;      /* DAT_004d80fc: the main window object */
    if (win) { win->window_w = ow; win->window_h = oh; }   /* its WM_GETMINMAXINFO limit */
    mlog("window: client was %ldx%ld, resizing window to %dx%d for an exact %dx%d client",
         client.right, client.bottom, ow, oh, want_w, want_h);
    SetWindowPos(h, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Keep the game's idea of its client area current and GAME-SIZED (windowed mode).
 *
 * The game reads the client rectangle once (FUN_00407cc0, right after our game_window_rect) and
 * uses it for: the windowed frame blit, clearing the screen, and centring videos
 * (FUN_0046c130). Two problems with the values it gets from Windows:
 *   - they go stale when the window moves (the game never re-reads them);
 *   - with [display] scale > 1 they are in enlarged screen pixels, but the screen surface is in
 *     game pixels (surface.c screen_to_game), so videos were centred in the wrong place.
 * Called when DirectDraw is set up and after every window move/resize (ddraw.c window hook). */
#define GAME_CLIENT_RECT        (*(RECT *)0x4e2df0)   /* 0,0,w,h */
#define GAME_CLIENT_SCREEN_RECT (*(RECT *)0x4e2dd8)   /* client origin on the desktop + w,h */

void window_sync_game_rects(HWND h)
{
    POINT origin = { 0, 0 };
    if (!ClientToScreen(h, &origin)) return;
    present_lock();
    GAME_CLIENT_RECT = (RECT){ 0, 0, g_cfg.width, g_cfg.height };
    GAME_CLIENT_SCREEN_RECT = (RECT){ origin.x, origin.y, origin.x + g_cfg.width, origin.y + g_cfg.height };
    present_unlock();
}

/* Replaces GetClientRect for Armymen.exe only (import table hook, patches.c).
 *
 * Windowed with [display] scale > 1 the real client area is game size x scale, but everything the
 * game does with its client size is in game pixels: the frame blit, screen clears, and centring
 * videos (FUN_0046c130 centred a 640x480 video inside 2048x1080, so most of it fell off the
 * 1024x540 screen surface and only the top-left quarter showed). So for the game's own window in
 * windowed mode we answer with the game size. Our DLL's own GetClientRect calls are not affected. */
BOOL WINAPI game_GetClientRect(HWND h, RECT *r)
{
    GameWindow *win = *(GameWindow **)0x4d80fc;
    HWND game = win ? *(HWND *)((BYTE *)win + 0x1c) : NULL;   /* main window object +0x1c = HWND */
    if (h && h == game && !GAME_IS_FULLSCREEN && r) {
        *r = (RECT){ 0, 0, g_cfg.width, g_cfg.height };
        return TRUE;
    }
    return GetClientRect(h, r);
}

/* ---------------------------------------------------------------- resizing (windowed)
 * The game answers WM_GETMINMAXINFO with max = its starting window size and min = half of it, so
 * the window could not be resized. Any size is fine now: the picture is fitted to the window
 * (view.c / present.c) and the game keeps working in its own size. */
#define MIN_CLIENT_W 320
#define MIN_CLIENT_H 180

void window_minmax(HWND h, MINMAXINFO *mm)
{
    if (GAME_IS_FULLSCREEN) return;
    RECT r = { 0, 0, MIN_CLIENT_W, MIN_CLIENT_H };
    AdjustWindowRectEx(&r, GetWindowLongA(h, GWL_STYLE), GetMenu(h) != NULL, GetWindowLongA(h, GWL_EXSTYLE));
    mm->ptMinTrackSize.x = r.right - r.left;
    mm->ptMinTrackSize.y = r.bottom - r.top;
    mm->ptMaxTrackSize.x = GetSystemMetrics(SM_CXMAXTRACK);
    mm->ptMaxTrackSize.y = GetSystemMetrics(SM_CYMAXTRACK);
    MONITORINFO mi = { sizeof mi };
    if (GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {   /* maximise = work area */
        mm->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
        mm->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
        mm->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
        mm->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
    }
}

/* A sizing border and a maximise button on the windowed game window. */
void window_allow_resize(HWND h)
{
    if (GAME_IS_FULLSCREEN) return;
    LONG style = GetWindowLongA(h, GWL_STYLE);
    if (!(style & WS_CAPTION) || (style & (WS_THICKFRAME | WS_MAXIMIZEBOX)) == (WS_THICKFRAME | WS_MAXIMIZEBOX)) return;
    SetWindowLongA(h, GWL_STYLE, style | WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    mlog("window: resizable");
}

/* ---------------------------------------------------------------- borderless fullscreen
 * The maximise button, Alt+Enter and F11 switch the game window to a borderless window covering
 * the whole monitor (and back). The game picture already has the screen's shape (layout.c), so it
 * fills the screen exactly, with no black bars. The menu bar is hidden meanwhile and restored
 * afterwards. The choice is remembered ([display] borderless_fullscreen). The game's own windowed
 * mode keeps running underneath, so nothing in the game notices. */
static struct { int on; LONG style, exstyle; RECT rect; HMENU menu; } g_fs;
/* (switching also changes the render size: resize.c, so the view grows instead of the picture) */

int window_is_fullscreen(void) { return g_fs.on; }

void window_set_fullscreen(HWND h, int on)
{
    if (GAME_IS_FULLSCREEN || !h || on == g_fs.on) return;
    if (on) {
        g_fs.style = GetWindowLongA(h, GWL_STYLE);
        g_fs.exstyle = GetWindowLongA(h, GWL_EXSTYLE);
        GetWindowRect(h, &g_fs.rect);
        g_fs.menu = GetMenu(h);
        MONITORINFO mi = { sizeof mi };
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
        g_fs.on = 1;
        SetMenu(h, NULL);
        SetWindowLongA(h, GWL_STYLE, (g_fs.style & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZE)) | WS_POPUP);
        SetWindowLongA(h, GWL_EXSTYLE, g_fs.exstyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
        RECT m = mi.rcMonitor;
        SetWindowPos(h, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        mlog("window: borderless fullscreen %ldx%ld", m.right - m.left, m.bottom - m.top);
    } else {
        g_fs.on = 0;
        SetWindowLongA(h, GWL_STYLE, g_fs.style & ~WS_MAXIMIZE);
        SetWindowLongA(h, GWL_EXSTYLE, g_fs.exstyle);
        if (g_fs.menu) SetMenu(h, g_fs.menu);
        RECT r = g_fs.rect;
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) &&
            (r.right - r.left) * 10 >= (mi.rcMonitor.right - mi.rcMonitor.left) * 9) {
            /* started in fullscreen: the remembered window is as big as the screen - use 3/4 of it */
            int s = g_cfg.scale < 1 ? 1 : g_cfg.scale;
            int cw = (mi.rcMonitor.right - mi.rcMonitor.left) * 3 / 4 / s * s;
            int ch = (mi.rcMonitor.bottom - mi.rcMonitor.top) * 3 / 4 / s * s;
            r = centred_window(cw, ch);
        }
        SetWindowPos(h, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        mlog("window: back to a window");
    }
    WritePrivateProfileStringA("display", "borderless_fullscreen", on ? "1" : "0", mod_ini_path());
    resize_request(h);                 /* resize.c: same HUD size, more (or less) battlefield */
}

/* After resize.c ran the game's display set-up again: the game's window set-up puts its menu bar
 * (and windowed style) back - take them off again while borderless fullscreen is on. */
void window_reapply_fullscreen(HWND h)
{
    if (!g_fs.on || !h) return;
    MONITORINFO mi = { sizeof mi };
    if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
    HMENU m = GetMenu(h);
    if (m) { g_fs.menu = m; SetMenu(h, NULL); }
    SetWindowLongA(h, GWL_STYLE, (GetWindowLongA(h, GWL_STYLE) & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZE)) | WS_POPUP);
    RECT r = mi.rcMonitor;
    SetWindowPos(h, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* Window hook: maximise button, Alt+Enter, F11. Returns 1 if the message was handled. */
int window_fullscreen_message(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (GAME_IS_FULLSCREEN) return 0;
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MAXIMIZE) { window_set_fullscreen(h, 1); return 1; }
    if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) { window_set_fullscreen(h, !g_fs.on); return 1; }
    if (msg == WM_KEYDOWN && wp == VK_F11) { window_set_fullscreen(h, !g_fs.on); return 1; }
    return 0;
}

/* WM_SIZING: while the frame is dragged, keep the client area in the game picture's shape, so the
 * picture always fills the window (no bars; user: "fit the image into the window"). */
void window_keep_shape(HWND h, WPARAM edge, RECT *r)
{
    if (!g_cfg.width || !g_cfg.height || !r) return;
    RECT wr, cr;
    if (!GetWindowRect(h, &wr) || !GetClientRect(h, &cr)) return;
    int fw = (wr.right - wr.left) - cr.right, fh = (wr.bottom - wr.top) - cr.bottom;   /* frame + menu */
    int cw = (r->right - r->left) - fw, ch = (r->bottom - r->top) - fh;
    if (cw < 64 || ch < 64) return;
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) cw = (int)((long long)ch * g_cfg.width / g_cfg.height);
    else ch = (int)((long long)cw * g_cfg.height / g_cfg.width);
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) r->left = r->right - cw - fw;
    else r->right = r->left + cw + fw;
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) r->top = r->bottom - ch - fh;
    else r->bottom = r->top + ch + fh;
}
