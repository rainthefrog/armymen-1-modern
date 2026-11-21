/* ============================================================================================
 * mouse.c - REWRITES of the game's mouse cursor functions (hooked in patches.c)
 *
 *   FUN_0041f380  mouse_update     move the game cursor by one DirectInput movement event
 *   FUN_0041f330  mouse_release    windowed: give the mouse back to Windows (cursor left the window)
 *   FUN_0041f480  mouse_reacquire  windowed: take the mouse back, starting where the pointer is
 * ============================================================================================
 *
 * mouse_update:
 *
 * The game reads raw mouse movement from DirectInput (exclusive mode) and keeps its own cursor.
 * FUN_0041f4e0 calls FUN_0041f380 once per movement event: ecx = x movement, edx = y movement.
 * The original:
 *   - adds 2x the raw movement (ignores the Windows pointer speed; far too fast on modern mice),
 *   - clamps y to 475 (0x1db, i.e. 480-5) even at higher resolutions, so the cursor cannot reach
 *     the lower part of the screen and "scroll down" triggers in the middle of the screen.
 * This version scales movement by [mouse] sensitivity (times the Windows pointer speed if
 * use_windows_speed=1), keeps the fractions so slow movements are not lost, and clamps to the
 * real screen size. Everything else matches the original.
 */
#include "common.h"

#define I32(va)  (*(volatile int *)(va))
#define U8(va)   (*(volatile BYTE *)(va))

#define CURSOR_X        0x4fe3e8   /* game cursor, client pixels */
#define CURSOR_Y        0x4fe3ec
#define PENDING_DX      0x4fe490   /* movement queued by other code */
#define PENDING_DY      0x4fe494
#define MOUSE_ACTIVE    0x4fe448
#define MOUSE_ACTIVE2   0x4b8a88
#define AT_LEFT         0x4fe44c   /* edge flags used for map scrolling */
#define AT_RIGHT        0x4fe450
#define AT_TOP          0x4fe454
#define AT_BOTTOM       0x4fe458
#define GFX_OBJ         0x4d80fc   /* graphics object; +0x100/+0x104 = client size */
#define WINDOWED_PTR    0x4b41e4   /* -> dword, 0 = windowed */

typedef void (__fastcall *game_fastcall)(int ecx, int edx);
#define RELEASE_MOUSE   ((game_fastcall)0x420610)   /* ecx=0: give the mouse back to Windows */
#define SHOW_CURSOR     ((game_fastcall)0x420660)   /* ecx=1: show the Windows cursor */

static float g_factor, g_frac_x, g_frac_y;

static float windows_pointer_speed(void)
{
    /* Windows' own multipliers for the 20 pointer-speed steps (10 = default = 1.0) */
    static const float t[21] = { 1.0f, 0.03125f, 0.0625f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f,
                                 0.875f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.25f, 2.5f, 2.75f, 3.0f,
                                 3.25f, 3.5f };
    int s = 10;
    if (!SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &s, 0) || s < 1 || s > 20) s = 10;
    return t[s];
}

/* Mouse counts -> game pixels. g_factor is in screen pixels per count; one game pixel covers
 * view_pixel_scale() screen pixels, so divide by it: the cursor then moves at the same speed as the
 * Windows pointer whatever the scale. */
static int take_whole(float *frac, int delta, int pixel_scale)
{
    *frac += delta * g_factor / (float)pixel_scale;
    int whole = (int)*frac;          /* truncates toward zero, keeps the remainder */
    *frac -= whole;
    return whole;
}

/* The real pointer position (game pixels) vs. what the game sees in CURSOR_X/Y.
 * They differ only while the pointer is over the HUD after hud.c moved it to the bottom of a larger
 * screen: the game then sees the matching 640x480 design position (layout.c layout_pointer_to_game). */
static int g_real_x, g_real_y;        /* where the pointer really is */
static int g_seen_x, g_seen_y;        /* what we last put in CURSOR_X/Y */

void mouse_real_cursor(int *x, int *y)
{
    /* if the game moved its cursor itself (e.g. centring it), that is the real position */
    if (I32(CURSOR_X) != g_seen_x || I32(CURSOR_Y) != g_seen_y) { *x = I32(CURSOR_X); *y = I32(CURSOR_Y); return; }
    *x = g_real_x; *y = g_real_y;
}

static void set_cursor(int x, int y)
{
    g_real_x = x; g_real_y = y;
    layout_pointer_to_game(x, y, &g_seen_x, &g_seen_y);
    I32(CURSOR_X) = g_seen_x;
    I32(CURSOR_Y) = g_seen_y;
}

static HWND game_hwnd(void);

void __fastcall mouse_update(int dx, int dy)
{
    if (I32(MOUSE_ACTIVE) == 0 && U8(MOUSE_ACTIVE2) == 0) return;

    if (g_factor == 0.0f) {
        g_factor = g_cfg.mouse_sens * (g_cfg.mouse_winspeed ? windows_pointer_speed() : 1.0f);
        mlog("mouse: %.3f screen pixels per mouse count (divided by the display scale for game pixels)", g_factor);
    }
    dx += I32(PENDING_DX);
    dy += I32(PENDING_DY);
    I32(PENDING_DX) = 0;
    I32(PENDING_DY) = 0;

    int x, y, inside = 1;
    if (g_cfg.mouse_pointer) {
        /* Pointer mode: the game cursor IS the Windows pointer - exactly 1:1, Windows speed and
         * acceleration, and free to leave the window. */
        POINT p;
        RECT client;
        GetCursorPos(&p);
        ScreenToClient(game_hwnd(), &p);
        if (GetClientRect(game_hwnd(), &client)) inside = PtInRect(&client, p);
        view_client_to_game(&p);
        x = p.x;
        y = p.y;
    } else {
        /* Raw mode (the original's way, made smoother): move by DirectInput counts. */
        int pixel_scale = view_pixel_scale();
        mouse_real_cursor(&x, &y);
        x += take_whole(&g_frac_x, dx, pixel_scale);
        y += take_whole(&g_frac_y, dy, pixel_scale);
        set_cursor(x, y);

        /* windowed: moving onto the title bar hands the mouse back to Windows (original behaviour) */
        if (**(int **)WINDOWED_PTR == 0 && y < 5) {
            g_frac_x = g_frac_y = 0;
            RELEASE_MOUSE(0, 0);
            SHOW_CURSOR(1, 0);
            return;
        }
    }

    /* Keep the cursor inside the visible picture (view.c): the whole screen in gameplay, the
     * 640x480 picture in menus. (The game's own limit, graphics object +0x100/+0x104, is the
     * outer window size in windowed mode, not the drawable area.) */
    RECT v = view_current();
    int min_x = v.left + 4, min_y = v.top + 4;
    int max_x = v.right - 5, max_y = v.bottom - 5;
    if (x < min_x) { x = min_x; g_frac_x = 0; } else if (x > max_x) { x = max_x; g_frac_x = 0; }
    if (y < min_y) { y = min_y; g_frac_y = 0; } else if (y > max_y) { y = max_y; g_frac_y = 0; }
    set_cursor(x, y);

    /* edge flags drive map scrolling (pointer mode: only while the pointer is inside the window) */
    U8(AT_LEFT)   = inside && x == min_x;
    U8(AT_RIGHT)  = inside && x == max_x;
    U8(AT_TOP)    = inside && y == min_y;
    U8(AT_BOTTOM) = inside && y == max_y;

    static int logged;
    if (g_cfg.log >= 2 && logged < 200 && (dx || dy)) {
        logged++;
        mlog("mouse: move %d,%d -> cursor %d,%d (limits %d,%d)", dx, dy, x, y, max_x, max_y);
    }
}

/* ---- handing the mouse between Windows and the game (windowed mode)
 * DirectInput mouse device: vtable +0x1c Acquire, +0x20 Unacquire. */
#define DI_MOUSE        0x4fe478
#define DI_ACQUIRED     0x4fe48c
typedef HRESULT (WINAPI *di_call)(void *);

static HWND game_hwnd(void) { return *(HWND *)(I32(GFX_OBJ) + 0x1c); }

/* Replaces FUN_0041f330: give the mouse back to Windows, placing the Windows pointer where the
 * game cursor was. The original did not account for scale. */
void __cdecl mouse_release(void)
{
    if (g_cfg.mouse_pointer) return;                /* pointer mode: the mouse is never taken away from Windows */
    void **dev = *(void ***)DI_MOUSE;
    if (!dev || !U8(DI_ACQUIRED)) return;
    ((di_call)((void **)*dev)[8])(dev);
    U8(DI_ACQUIRED) = 0;
    POINT p;
    mouse_real_cursor((int *)&p.x, (int *)&p.y);
    view_game_to_client(&p);
    ClientToScreen(game_hwnd(), &p);
    SetCursorPos(p.x, p.y);
}

/* Replaces FUN_0041f480: take the mouse back, starting the game cursor where the Windows pointer is. */
void __cdecl mouse_reacquire(void)
{
    void **dev = *(void ***)DI_MOUSE;
    if (!dev) return;
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(game_hwnd(), &p);
    view_client_to_game(&p);
    set_cursor(p.x, p.y);
    g_frac_x = g_frac_y = 0;
    if (**(int **)WINDOWED_PTR != 0 || p.y >= 5) {
        if (((di_call)((void **)*dev)[7])(dev) == 0) U8(DI_ACQUIRED) = 1;
    }
}

/* ============================================================================================
 * Pointer mode plumbing ([mouse] follow_windows_pointer=1)
 *
 * The game opens the DirectInput mouse in EXCLUSIVE mode (0x420283: push 5), which hides the Windows
 * pointer and locks it to the game. patches.c makes it shared (6 = NONEXCLUSIVE | FOREGROUND): the
 * game still gets its button and movement events from DirectInput, the Windows pointer stays free,
 * and mouse_update above reads the pointer position. Two side effects are handled here:
 *   - Windows now ALSO sends its own mouse messages to the game window (in window pixels). The game
 *     posts its own (FUN_0041f4e0, in game pixels). mouse_PostMessageA marks the game's own; the
 *     window hook (ddraw.c) calls mouse_filter_message, which drops the unmarked ones.
 *   - The game hides the Windows pointer with ShowCursor. That would hide it over the title bar
 *     and menu too, so ShowCursor is ignored and the pointer is hidden only over the game picture
 *     (WM_SETCURSOR).
 * ============================================================================================ */
#define GAME_POSTED_MARK 0x80000000u    /* set in wParam of mouse messages the game posts itself */

static int is_mouse_message(UINT msg) { return msg >= WM_MOUSEMOVE && msg <= WM_MOUSEWHEEL; }

BOOL WINAPI mouse_PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_cfg.mouse_pointer && is_mouse_message(msg)) wp |= GAME_POSTED_MARK;
    return PostMessageA(h, msg, wp, lp);
}

int WINAPI mouse_ShowCursor(BOOL show)
{
    return show ? 0 : -1;       /* pretend; visibility is handled by WM_SETCURSOR */
}

/* Returns 1 if the message was handled here (*result set), 0 to pass it on (possibly modified). */
int mouse_filter_message(HWND h, UINT msg, WPARAM *wp, LPARAM lp, LRESULT *result)
{
    if (!g_cfg.mouse_pointer) return 0;
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        /* the game draws its own cursor (menus) or the aiming reticle (battlefield); over the HUD
         * nothing would show, so the Windows arrow is shown there */
        HCURSOR menu = hdart_active() ? hdart_menu_cursor(view_pixel_scale()) : NULL;   /* HD menu: the game cursor, anywhere */
        /* over the interface: the game's own arrow at the picture's scale (2026-11-17; the small Windows arrow
         * next to the scaled battlefield cursors made the change jarring), the Windows arrow if not available */
        HCURSOR ui = NULL;
        if (!menu && controls_pointer_on_ui()) { ui = hdart_menu_cursor(view_pixel_scale()); if (!ui) ui = LoadCursorA(NULL, (LPCSTR)IDC_ARROW); }
        SetCursor(menu ? menu : ui ? ui : controls_cursor());
        *result = TRUE;
        return 1;
    }
    if (is_mouse_message(msg)) {
        if (*wp & GAME_POSTED_MARK) { *wp &= ~GAME_POSTED_MARK; return 0; }
        *result = 0;             /* Windows' own copy: the game already gets this click via DirectInput */
        return 1;
    }
    return 0;
}

/* Pointer mode: refresh the game cursor from the Windows pointer once per frame (called from the
 * cursor drawing, hud.c), so it follows the pointer even when DirectInput reports movement late. */
void mouse_poll_pointer(void)
{
    if (g_cfg.mouse_pointer && g_cfg.mouse_fix) mouse_update(0, 0);
}

/* controls.c (throw camera holding the aimed point): raw mouse mode - move the game's own cursor by dx, dy game px
 * along with the Windows pointer (pointer mode: the game cursor follows the Windows pointer by itself) */
void mouse_nudge_cursor(int dx, int dy)
{
    if (g_cfg.mouse_pointer || (!dx && !dy)) return;
    int x, y;
    mouse_real_cursor(&x, &y);
    set_cursor(x + dx, y + dy);
}
