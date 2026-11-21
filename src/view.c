/* ============================================================================================
 * view.c - which part of the game's screen is shown, and where it goes in the window
 *
 * The screen surface is game-sized (e.g. 1024x540). Not all of it is always in use:
 *   - gameplay draws the whole surface every frame;
 *   - menus and loading screens are 640x480 pictures (the art and the menu layout are 640x480),
 *     drawn in the top-left corner; the rest of the surface is never painted (it showed as a
 *     blue/cyan border);
 *   - videos are drawn centred, at their own size.
 * The game has no reliable "in menu" flag, so the VIEW follows what was last drawn as a whole
 * picture (surface.c calls view_note_*):
 *   - a frame covering the whole surface (Flip, or the windowed frame blit)  -> whole surface
 *   - a single big picture (>= VIEW_MIN_W x VIEW_MIN_H, e.g. a menu or video) -> that picture
 *   - small blits (the mouse cursor) and colour fills don't change it.
 *
 * Layout: the view is made as big as the window allows, keeping its shape, and centred on black
 * (the window can be resized to any size; present.c scales with [display] smoothing). Videos with
 * [display] video_fit=0 use the largest whole-number size instead.
 * Mouse coordinates are converted with the same layout (view_client_to_game / view_game_to_client).
 * ============================================================================================ */
#include "common.h"

#define VIEW_MIN_W 320
#define VIEW_MIN_H 200

static RECT g_view;             /* game pixels; empty = whole surface. Guarded by present_lock. */
static int g_view_is_video;     /* the view is a video frame: fill the window ([display] video_fit) */
static volatile unsigned g_frames;  /* whole game frames shown so far (layout.c: is a menu still up?) */

unsigned view_frame_count(void) { return g_frames; }

static RECT whole_surface(void)
{
    if (g_dd && g_dd->primary) return (RECT){ 0, 0, g_dd->primary->w, g_dd->primary->h };
    return (RECT){ 0, 0, g_cfg.width, g_cfg.height };
}

RECT view_current(void)
{
    RECT v = g_view;
    RECT all = whole_surface();
    if (IsRectEmpty(&v) || !IntersectRect(&v, &v, &all)) v = all;
    return v;
}

/* surface.c, with present_lock held: something was drawn onto the screen surface. */
void view_note_whole_frame(void)
{
    SetRectEmpty(&g_view);
    g_view_is_video = 0;
    g_frames++;
}

void view_note_picture(const RECT *r)
{
    RECT all = whole_surface();
    if (EqualRect(r, &all)) { SetRectEmpty(&g_view); g_view_is_video = 0; g_frames++; return; }   /* a whole frame too */
    if (r->right - r->left < VIEW_MIN_W || r->bottom - r->top < VIEW_MIN_H) return;
    /* during play a panel smaller than 640x480 (the game's large menu panel, 480x340: Mod Settings, the key
     * setup screens) is drawn OVER the battlefield: the view stays (2026-11-17) */
    if (!g_view_is_video && game_in_play() && (game_menu_up() || r->right - r->left < 640 || r->bottom - r->top < 480)) return;   /* also the 2x menus (layout.c) */
    /* the same large panel (480x340) on a 640x480 menu screen outside play - Mod Settings and the key setup
     * screens opened from the MAIN menu: the view jumped onto the panel (zoomed in, background cut off, HD
     * buttons misplaced - black boxes). The menu screen stays the view (2026-11-18). */
    if (!g_view_is_video && r->right - r->left == 480 && r->bottom - r->top == 340
        && g_view.right - g_view.left == 640 && g_view.bottom - g_view.top == 480
        && r->left >= g_view.left && r->top >= g_view.top && r->right <= g_view.right && r->bottom <= g_view.bottom) return;
    if (!EqualRect(r, &g_view))
        mlog("view: showing %ld,%ld %ldx%ld of the screen", r->left, r->top, r->right - r->left, r->bottom - r->top);
    g_view = *r;
    g_view_is_video = 0;
}

/* A video frame (FUN_0046c130's blit to the screen): shown like a picture, but filling the window. */
void view_note_video(const RECT *r)
{
    view_note_picture(r);
    if (EqualRect(r, &g_view)) g_view_is_video = 1;
}

/* Where the view goes in a client area of cw x ch. */
ViewLayout view_layout(int cw, int ch)
{
    ViewLayout L;
    L.view = view_current();
    int vw = L.view.right - L.view.left, vh = L.view.bottom - L.view.top;
    int fit_w = cw / vw, fit_h = ch / vh;
    L.whole = fit_w < fit_h ? fit_w : fit_h;
    if (L.whole < 1) L.whole = 1;           /* window smaller than the view: crop (present.c) */
    int dw = vw * L.whole, dh = vh * L.whole;
    /* fitted to the window (same shape, any size: the window can be dragged to any size). Only
     * videos with [display] video_fit=0 keep whole-number steps. */
    L.video = g_view_is_video;
    L.fit = !(g_view_is_video && !g_cfg.video_fit);
    if (L.fit) {                            /* as big as the window allows, same shape */
        if ((long long)cw * vh <= (long long)ch * vw) { dw = cw; dh = (int)((long long)cw * vh / vw); }
        else { dh = ch; dw = (int)((long long)ch * vw / vh); }
    }
    if (!g_view_is_video && hdart_active() && vw == 640 && vh == 480) {
        /* menu with a 16:9 HD background (hdart.c): the picture fits the window HEIGHT (nothing cut
         * off top or bottom; a narrower window cuts the wide sides, never the 4:3 centre) */
        double k = (double)ch / vh, k2 = (double)cw / vw;
        if (k2 < k) k = k2;
        dw = (int)(vw * k + 0.5);
        dh = (int)(vh * k + 0.5);
    }
    L.dest.left = (cw - dw) / 2;
    L.dest.top = (ch - dh) / 2;
    L.dest.right = L.dest.left + dw;
    L.dest.bottom = L.dest.top + dh;
    return L;
}

static ViewLayout layout_for_window(void)
{
    RECT c = { 0, 0, g_cfg.width, g_cfg.height };
    if (g_dd && g_dd->hwnd) GetClientRect(g_dd->hwnd, &c);
    return view_layout(c.right, c.bottom);
}

void view_client_to_game(POINT *p)
{
    ViewLayout L = layout_for_window();
    int dw = L.dest.right - L.dest.left, dh = L.dest.bottom - L.dest.top;
    if (dw <= 0 || dh <= 0) return;
    p->x = L.view.left + MulDiv(p->x - L.dest.left, L.view.right - L.view.left, dw);
    p->y = L.view.top + MulDiv(p->y - L.dest.top, L.view.bottom - L.view.top, dh);
    zoom_to_game(p);                         /* zoom.c: battlefield points through the zoom */
}

void view_game_to_client(POINT *p)
{
    zoom_from_game(p);
    ViewLayout L = layout_for_window();
    int vw = L.view.right - L.view.left, vh = L.view.bottom - L.view.top;
    p->x = L.dest.left + MulDiv(p->x - L.view.left, L.dest.right - L.dest.left, vw);
    p->y = L.dest.top + MulDiv(p->y - L.view.top, L.dest.bottom - L.dest.top, vh);
}

/* How many window pixels one game pixel currently covers (the view's whole-number enlargement).
 * mouse.c divides mouse movement by this, so the cursor moves at desktop speed at any scale. */
int view_pixel_scale(void)
{
    return layout_for_window().whole;
}

/* modern.c: a video is being shown (any key / click may skip it) */
int view_is_video(void) { return g_view_is_video; }

/* controls.c (throw camera, moving the pointer with the camera): the exact battlefield game -> client mapping of
 * view_game_to_client as doubles, client = a + k x game (view_client_to_game rounds the inverse to the nearest game
 * pixel). 0 while zoomed (zoom.c maps through its own centre: the caller samples instead). */
int view_game_client_linear(double *ax, double *ay, double *kx, double *ky)
{
    if (zoom_active() && zoom_factor() > 1.001f) return 0;
    ViewLayout L = layout_for_window();
    int vw = L.view.right - L.view.left, vh = L.view.bottom - L.view.top;
    if (vw <= 0 || vh <= 0 || L.dest.right <= L.dest.left || L.dest.bottom <= L.dest.top) return 0;
    *kx = (double)(L.dest.right - L.dest.left) / vw;
    *ky = (double)(L.dest.bottom - L.dest.top) / vh;
    *ax = L.dest.left - L.view.left * *kx;
    *ay = L.dest.top - L.view.top * *ky;
    return 1;
}
