/* ============================================================================================
 * resize.c - change the game's render size while it runs (fullscreen shows MORE battlefield)
 *
 * User: going fullscreen enlarged the picture; it should show more of the battlefield with the
 * interface at the same size. The game's surfaces are made for one size, so on a size change the
 * game's own display set-up is run again, exactly like its windowed <-> fullscreen switch
 * (FUN_00408840) does it, minus the mode/style change:
 *
 *   FUN_004083e0  thiscall(ecx = main window object)  release screen, back buffer, map surface,
 *                 palette, DirectDraw2 (NOT the DirectDraw object: sprites, tiles and HUD pictures,
 *                 all made from it, stay valid)
 *   FUN_0044be60  free the GDI-built fonts
 *   FUN_00407ed0  thiscall(ecx = main window object)  window rect (our game_window_rect), screen,
 *                 back buffer, map surface (height + 14, layout.c), palette, clipper
 *   FUN_0044bed0  build the fonts again (TextOut into the new back buffer)
 *   FUN_0040c5c0  battlefield view size from the new screen size + "redraw the whole map" flag
 * plus what we set at start-up for a size: the window object's render size (+0xc0/+0xc4), the
 * immediates in FUN_00407820 and the default clip rect (patches.c), the 640x480-based limits
 * (layout_patch_limits). The palette in use (a mission's terrain palette) is put back afterwards.
 *
 * When: never inside a frame. window.c asks for a resize (resize_request) after switching to or
 * from borderless fullscreen; a private message posted to the game window runs it from the game's
 * own message loop, between frames. The display thread is held off (present_lock) meanwhile.
 * New size = client area / [display] scale (whole game pixels per screen pixel), so the HUD keeps
 * its size on screen and the battlefield grows or shrinks.
 * ============================================================================================ */
#include "common.h"
#include <string.h>
#include <stdlib.h>

#define WM_MOD_RESIZE      (WM_APP + 0x41)
#define MAIN_WINDOW_OBJ    (*(BYTE **)0x4d80fc)      /* DAT_004d80fc */
#define OBJ_RENDER_W(o)    (*(int *)((o) + 0xc0))
#define OBJ_RENDER_H(o)    (*(int *)((o) + 0xc4))
#define OBJ_HWND(o)        (*(HWND *)((o) + 0x1c))
#define GAME_IS_FULLSCREEN (**(int **)0x4b41e4)      /* the game's own exclusive mode (not ours) */
#define IMM_RENDER_W       0x407849                  /* FUN_00407820: mov [ecx+0xc0], imm32 */
#define IMM_RENDER_H       0x407853
#define DEFAULT_CLIP       ((int *)0x4abac0)         /* left, top, right, bottom */

typedef void (__fastcall *ThisFn)(void *ecx, void *edx);
typedef int  (__fastcall *ThisIntFn)(void *ecx, void *edx);
#define DD_TEARDOWN   ((ThisFn)0x4083e0)
#define DD_INIT       ((ThisIntFn)0x407ed0)
#define FONTS_FREE    ((VoidFnPtr)0x44be60)
#define FONTS_BUILD   ((VoidFnPtr)0x44bed0)
#define MAP_VIEW_SIZE ((VoidFnPtr)0x40c5c0)
#define CAMERA_APPLY  ((VoidFnPtr)0x40dcd0)          /* view position + screenrect from the camera centre (our wrap, controls.c) */
#define MAP_REDRAW_ALL (*(int *)0x4e30e4)           /* 1 = draw every map tile again */
#define MAP_UNLOCK_KEPT ((VoidFnPtr)0x42ec80)        /* unlock the map surface if the game keeps it locked */
#define MAP_KEEP_LOCKED (*(int *)0x4e2dc4)            /* game keeps the map surface locked between frames */
#define MAP_IS_LOCKED   (*(int *)0x4e2dc0)            /* ... and it is locked now (pixels at 0x4e2444) */

static int g_busy;

/* Colour state the display set-up rebuilds from the START-UP palette (FUN_00420e40 + FUN_00403b30): the
 * game palette copies, the shade tables (x0.40 .. x0.85 at 0x4e2998, pointers 0x4e2448..), the shadow
 * table (*0x4e221c) and the green highlight (*0x4e2da8). Saved before and put back after, so a mission
 * keeps its own colours (the first test showed brown streaks on rocks and dotted fences). */
typedef struct { BYTE *at; size_t n; BYTE *copy; } Keep;
static void keep_save(Keep *k, void *at, size_t n) { k->at = at; k->n = n; k->copy = at ? malloc(n) : NULL; if (k->copy) memcpy(k->copy, at, n); }
static void keep_restore(Keep *k) { if (k->copy && k->at) memcpy(k->at, k->copy, k->n); free(k->copy); k->copy = NULL; }

static void dump(const char *when)
{
    if (g_cfg.log < 2) return;                         /* [debug] log=2: the game globals around a resize */
    Surf *m = *(Surf **)0x4e2e5c, *b = *(Surf **)0x4e2e4c;
    int *r = (int *)0x4e2fd4, *fr = (int *)0x4e2fc4, *br = (int *)0x4e2ff4;
    short *pb = (short *)0x4e2fb4;
    mlog("resize %s: shade %p %p %p %p shadow %p green %p", when, ((void **)0x4e2448)[0], ((void **)0x4e2448)[1], ((void **)0x4e2448)[2], ((void **)0x4e2448)[3], *(void **)0x4e221c, *(void **)0x4e2da8);
    mlog("resize %s: map %dx%d back %dx%d | locked %p pitch %d keep %d is %d | view %d,%d %dx%d tiles %d,%d | screenrect %d,%d,%d,%d full %d,%d,%d,%d bound %d,%d,%d,%d | 2e18 %d,%d,%d,%d 3690 %d,%d,%d,%d",
         when, m ? m->w : 0, m ? m->h : 0, b ? b->w : 0, b ? b->h : 0, *(void **)0x4e2444, *(int *)0x4e2880,
         *(int *)0x4e2dc4, *(int *)0x4e2dc0, pb[0], pb[1], pb[2], pb[3], *(short *)0x4e2fb0, *(short *)0x4e2fb2,
         r[0], r[1], r[2], r[3], fr[0], fr[1], fr[2], fr[3], br[0], br[1], br[2], br[3],
         ((int *)0x4e2e18)[0], ((int *)0x4e2e18)[1], ((int *)0x4e2e18)[2], ((int *)0x4e2e18)[3],
         ((int *)0x4e3690)[0], ((int *)0x4e3690)[1], ((int *)0x4e3690)[2], ((int *)0x4e3690)[3]);
}
int resize_in_progress(void) { return g_busy; }
static HWND g_pending;                 /* a resize waiting for gameplay */
#define MENU_CURSOR_SHOWN (*(int *)0x4fe448) /* the game shows its mouse cursor only in menus (incl. pause) */
static int resize_can_rebuild(void) { return layout_view_is_gameplay() && !MENU_CURSOR_SHOWN && !layout_any_panel_open(); }

static void write_code(uintptr_t va, DWORD v)
{
    DWORD old;
    if (!VirtualProtect((void *)va, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy((void *)va, &v, 4);
    VirtualProtect((void *)va, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)va, 4);
}

/* The render size for a client area of cw x ch: whole game pixels at the display scale. */
static void size_for_client(int cw, int ch, int *w, int *h)
{
    int s = g_cfg.scale < 1 ? 1 : g_cfg.scale;
    *w = (cw / s) & ~1;
    *h = ch / s;
    if (*w < LAYOUT_DESIGN_W) *w = LAYOUT_DESIGN_W;      /* the HUD and menus need 640x480 */
    if (*h < LAYOUT_DESIGN_H) *h = LAYOUT_DESIGN_H;
}

void resize_request(HWND h)
{
    if (!g_cfg.fs_extends_view || !h || GAME_IS_FULLSCREEN) return;
    PostMessageA(h, WM_MOD_RESIZE, 0, 0);
}

static void rebuild(HWND hwnd, int w, int h)
{
    BYTE *win = MAIN_WINDOW_OBJ;
    if (!win || !g_dd) return;
    mlog("resize: %dx%d -> %dx%d game pixels", g_cfg.width, g_cfg.height, w, h);
    g_busy = 1;
    present_lock();                                    /* the display thread must not read the old screen */
    dump("before");
    PALETTEENTRY pal[256]; int have_pal = 0;
    BYTE *shadow = *(BYTE **)0x4e221c, *green = *(BYTE **)0x4e2da8;
    Keep keep[5];
    keep_save(&keep[0], (void *)0x4ab2b4, 0x400);
    keep_save(&keep[1], (void *)0x4ab6bc, 0x400);
    keep_save(&keep[2], (void *)0x4e2998, 0x400);
    keep_save(&keep[3], shadow, 256);
    keep_save(&keep[4], green, 256);
    void *ptrs[6]; memcpy(ptrs, (void *)0x4e2448, 4 * sizeof(void *)); ptrs[4] = shadow; ptrs[5] = green;
    if (g_dd->pal) { memcpy(pal, g_dd->pal->e, sizeof pal); have_pal = 1; }
    /* the GDI palette (HPALETTE 0x4e2db0) too: the set-up rebuilds it from the start-up palette, and
     * pictures loaded after the rebuild (the re-shown welcome sign) were colour-matched against it */
    PALETTEENTRY gdi_pal[256]; HPALETTE hp = *(HPALETTE *)0x4e2db0;
    int have_gdi = hp && GetPaletteEntries(hp, 0, 256, gdi_pal) == 256;

    if (MAP_KEEP_LOCKED && MAP_IS_LOCKED) MAP_UNLOCK_KEPT();   /* else the next frame draws into the freed surface */
    DD_TEARDOWN(win, NULL);
    FONTS_FREE();
    g_cfg.width = w; g_cfg.height = h;
    OBJ_RENDER_W(win) = w; OBJ_RENDER_H(win) = h;
    write_code(IMM_RENDER_W, (DWORD)w);
    write_code(IMM_RENDER_H, (DWORD)h);
    DEFAULT_CLIP[0] = 0; DEFAULT_CLIP[1] = 0; DEFAULT_CLIP[2] = w; DEFAULT_CLIP[3] = h;
    layout_patch_limits();                             /* battlefield height, map surface height, aim limits */
    int ok = DD_INIT(win, NULL);
    memcpy((void *)0x4e2448, ptrs, 4 * sizeof(void *));
    *(BYTE **)0x4e221c = ptrs[4]; *(BYTE **)0x4e2da8 = ptrs[5];
    for (int i = 0; i < 5; i++) keep_restore(&keep[i]);
    if (have_pal && g_dd->pal) {                       /* the mission's palette, not the start-up one */
        memcpy(g_dd->pal->e, pal, sizeof pal);
        pal_lut(g_dd->pal);
    }
    HPALETTE hp2 = *(HPALETTE *)0x4e2db0;
    if (have_gdi && hp2) SetPaletteEntries(hp2, 0, 256, gdi_pal);
    FONTS_BUILD();
    MAP_VIEW_SIZE();
    CAMERA_APPLY();                                    /* else the first frame clips sprites to the OLD view size */
    MAP_REDRAW_ALL = 1;
    dump("after");
    present_unlock();
    g_busy = 0;
    window_reapply_fullscreen(hwnd);                   /* the game put its menu bar back */
    window_sync_game_rects(hwnd);
    controls_reset();                                  /* camera / sub-pixel state for the new view */
    mlog("resize: done (display set-up returned %d), screen %dx%d", ok,
         g_dd->primary ? g_dd->primary->w : 0, g_dd->primary ? g_dd->primary->h : 0);
    dd_present(g_dd, 0);
}

/* Window hook. Returns 1 when the message was ours. */
int resize_message(HWND hwnd, UINT msg)
{
    if (msg != WM_MOD_RESIZE) return 0;
    if (g_busy || GAME_IS_FULLSCREEN || !g_dd || !g_dd->primary) return 1;
    RECT c;
    if (!GetClientRect(hwnd, &c) || c.right <= 0 || c.bottom <= 0 || IsIconic(hwnd)) return 1;
    int w, h;
    size_for_client(c.right, c.bottom, &w, &h);
    if (w == g_cfg.width && h == g_cfg.height) { g_pending = NULL; return 1; }
    if (!resize_can_rebuild()) {
        /* A menu (title, briefing, pause menu) is drawn once and never again: after a rebuild the new
         * screen stayed blank (cyan). Menus are a fixed 640x480 picture scaled to the window anyway, so
         * the new render size waits for gameplay (resize_poll). */
        g_pending = hwnd;
        return 1;
    }
    g_pending = NULL;
    rebuild(hwnd, w, h);
    return 1;
}

/* controls.c, every gameplay frame: a resize asked for while a menu was up is done now (posted, so it
 * runs between frames). */
void resize_poll(void)
{
    if (g_pending && resize_can_rebuild()) { HWND h = g_pending; g_pending = NULL; PostMessageA(h, WM_MOD_RESIZE, 0, 0); }
}
