/* ============================================================================================
 * hud.c - the game's interface drawing routines, made resolution independent
 *
 * Every routine the game uses to draw interface (HUD, pop-ups, in-game menus, text, minimap) is
 * replaced or wrapped here, and each one asks layout.c where a 640x480 design position goes on the
 * real screen. The full list of call sites is in tooling/docs/ui-inventory.md.
 *
 *   routine        what                                   how                    here
 *   FUN_00412f70   picture -> game picture (back buffer)  rewritten              hud_draw_picture
 *   FUN_00413050   picture -> screen (pause menu, ...)    rewritten              hud_draw_picture_on_screen
 *   FUN_0044b760   text with the cached game font         wrapped (trampoline)   hud_draw_string
 *   FUN_00413a30   text with GDI (menu labels)            wrapped (trampoline)   hud_draw_gdi_text
 *   FUN_00405980   minimap viewport box                   wrapped                hud_minimap_rect
 *   FUN_004057b0   minimap unit dot                       wrapped                hud_minimap_plot
 *   FUN_004206a0   mouse cursor                           rewritten              hud_draw_cursor
 *   FUN_00406390   colour fill on screen (menu boxes)     rewritten (+406410)    hud_fill_on_screen
 *
 * The HUD panel keeps its 640 width; the battlefield shows beside it (layout.c).
 * Battlefield sprites are not interface and are left alone.
 * ============================================================================================ */
#include "common.h"

#define BACK_BUFFER  (*(void ***)0x4e2e4c)   /* IDirectDrawSurface* the game draws each frame into */
#define SCREEN       (*(void ***)0x4e2e48)   /* IDirectDrawSurface* of the screen (primary) */
#define GAME_CLIENT_SCREEN_RECT (*(RECT *)0x4e2dd8)   /* windowed: client origin; fullscreen: 0,0 */

/* A picture record as used by FUN_00412f70 / FUN_00413050 / FUN_00413610 (ecx). */
typedef struct UiPicture {
    void *image;             /* +0x00 IDirectDrawSurface*, or packed pixels when flags & 2 */
    BYTE  _pad0[0x14];
    RECT  src;               /* +0x18 part of the image to draw */
    short hot_x, hot_y;      /* +0x28 hot-spot: drawn at (x - hot_x, y - hot_y) */
    BYTE  _pad1[0x08];
    void *shade;             /* +0x34 shading table for packed pixels (0 = none) */
    short _pad2;
    unsigned short flags;    /* +0x3a 1 = no transparency, 2 = packed pixels, 0x3c = hidden */
} UiPicture;

/* IDirectDrawSurface methods we call through the game's own surface pointers. */
typedef HRESULT (WINAPI *BltFn)(void *, RECT *, void *, RECT *, DWORD, DDBLTFX *);
typedef HRESULT (WINAPI *BltFastFn)(void *, DWORD, DWORD, void *, RECT *, DWORD);
typedef HRESULT (WINAPI *LockFn)(void *, RECT *, DDSURFACEDESC *, DWORD, HANDLE);
typedef HRESULT (WINAPI *UnlockFn)(void *, void *);
#define VTBL(obj) (*(void ***)(obj))

/* Game routines that unpack a packed picture: ecx = destination, edx = pitch. */
typedef void (__fastcall *UnpackFn)(BYTE *dst, int pitch, void *data, int height);
typedef void (__fastcall *UnpackShadedFn)(BYTE *dst, int pitch, void *data, int height, void *shade);
#define UNPACK          ((UnpackFn)0x406c50)
#define UNPACK_SHADED   ((UnpackShadedFn)0x406dc0)

static int picture_w(const UiPicture *p) { return p->src.right - p->src.left; }
static int picture_h(const UiPicture *p) { return p->src.bottom - p->src.top; }

/* ---------------------------------------------------------------- HUD panel
 * The panel picture is 640 wide and stays that way (centred at the bottom, as designed). On a
 * wider screen the battlefield is full height (layout.c patches its view and surface size), so
 * the game shows beside the panel. */

/* Move a picture's design position (top-left, 640x480) for the current screen. Returns 1 for a centred
 * picture (pop-up, sign, radio): the caller notes it as an open panel (note_centred) once it is really drawn.
 * 2026-11-20 (user: "this mission complete is still drawing the BG"): the panel used to be noted here, before
 * radio_take - so the radio the MOD draws as an overlay was an "open panel" too, and gpu.c ps_resolve shows no
 * battlefield art inside open panels: the 8-bit battlefield in the radio's whole rectangle. On the mission-over
 * screen the game stops counting frames, so that panel never closed. */
static int place_picture(UiPicture *pic, int *px, int *py, int on_back_buffer)
{
    (void)pic; (void)on_back_buffer;
    if (!layout_active()) return 0;
    return layout_place(px, py) == ANCHOR_CENTER;
}
static void note_centred(UiPicture *pic, int px, int py)
{
    RECT r = { px, py, px + picture_w(pic), py + picture_h(pic) };
    layout_note_panel(ANCHOR_CENTER, &r);
}

/* ---------------------------------------------------------------- FUN_00412f70 */

/* Rewrite of FUN_00412f70: draw a picture into the back buffer at design position (x, y).
 * __fastcall: ecx = picture, edx = x, y on the stack (ret 4). */
/* The HUD panel picture (640x115, last seen): weapons.c copies its weapon boxes for the spare column. */
#define HUD_PANEL_W 640
#define HUD_PANEL_H 115
static int g_hud_fresh;                     /* the HUD panel was drawn since the last finished frame */
static void *g_panel_image;
static RECT g_panel_src;

int hud_panel_art(void **image, RECT *src)
{
    if (!g_panel_image) return 0;
    *image = g_panel_image;
    *src = g_panel_src;
    return 1;
}

/* A picture pointer the game hands us may be stale (crash 2026-11-21, mission 10 restart: pic = ff2d2418, read in
 * this hook - most likely a freed weapon box record, see weapons.c f950aaa). Unreadable = not drawn, logged with the
 * calling game function. */
static int bad_picture(const UiPicture *pic, void *from)
{
    if ((uintptr_t)pic >= 0x10000 && !IsBadReadPtr(pic, sizeof *pic)) return 0;
    static int n;
    if (n++ < 20) mlog("hud: picture %p unreadable - not drawn (from %p)", (void *)pic, from);
    return 1;
}

static void __fastcall hud_draw_picture_px(UiPicture *pic, int x, int y) {
    if (!pic || !pic->image || (pic->flags & 0x3c)) return;
    int px = x - pic->hot_x, py = y - pic->hot_y;
    if (g_cfg.log >= 5 && layout_active()) {         /* debugging: what is drawn in gameplay */
        static int n;
        if (n++ < 300) mlog("picture %dx%d at %d,%d flags %#x from %p", picture_w(pic), picture_h(pic),
                            px, py, pic->flags, __builtin_return_address(0));
    }
    int is_panel = picture_w(pic) == HUD_PANEL_W && picture_h(pic) == HUD_PANEL_H && !(pic->flags & 2);
    if (is_panel) {
        g_panel_image = pic->image;                  /* weapons.c copies its weapon boxes */
        g_hud_fresh = 1;                             /* this frame has a new HUD (hud_finish_frame) */
        g_panel_src = pic->src;
    }
    int design_x = px;
    int centred = place_picture(pic, &px, &py, 1);
    if (!is_panel) {                                 /* a radio message (briefing, script call): drawn by radio.c */
        RECT pr = { px, py, px + picture_w(pic), py + picture_h(pic) };
        if (radio_take(pic, 0, &pr)) return;         /* an overlay, not a panel on the picture: the battlefield stays HD */
    }
    if (centred) note_centred(pic, px, py);
    void *back = BACK_BUFFER;
    if (!(pic->flags & 2)) {
        DWORD how = (pic->flags & 1) ? DDBLTFAST_WAIT : DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY;
        if (is_panel && layout_active() && design_x == 0) {
            /* the panel is drawn in zones (layout.c): corners, the centre part (split for the
             * spare-weapon column, weapons.c) and air support each at their own place */
            static const int cut[] = { 0, 186, LAYOUT_HUD_SPLIT_X, 434, 505, 625, LAYOUT_DESIGN_W };   /* 625: the knob strip (layout.c) */
            for (int i = 0; i < 6; i++) {
                RECT part = pic->src;
                part.left = pic->src.left + cut[i];
                part.right = pic->src.left + cut[i + 1];
                ((BltFastFn)VTBL(back)[7])(back, cut[i] + layout_hud_dx(cut[i]), py, pic->image, &part, how);
            }
            return;
        }
        ((BltFastFn)VTBL(back)[7])(back, px, py, pic->image, &pic->src, how);
        minimap_picture_drawn(back, pic, px, py, picture_w(pic), picture_h(pic));   /* minimap.c: map picture contrast */
        return;
    }
    DDSURFACEDESC d = { .dwSize = sizeof d };
    if (((LockFn)VTBL(back)[25])(back, NULL, &d, DDLOCK_WAIT, NULL) != DD_OK) return;
    BYTE *dst = (BYTE *)d.lpSurface + d.lPitch * py + px;
    if (pic->shade) UNPACK_SHADED(dst, d.lPitch, pic->image, picture_h(pic), pic->shade);
    else UNPACK(dst, d.lPitch, pic->image, picture_h(pic));
    ((UnlockFn)VTBL(back)[32])(back, d.lpSurface);
    minimap_picture_drawn(back, pic, px, py, picture_w(pic), picture_h(pic));
}
void __fastcall hud_draw_picture(UiPicture *pic, int x, int y) { if (pic && bad_picture(pic, __builtin_return_address(0))) return; PX_BEGIN(px); hud_draw_picture_px(pic, x, y); PX_END(px, PR_UI); }   /* timed: stage PR_UI (exclusive, common.h) */

/* ---------------------------------------------------------------- FUN_00413050 */

/* Rewrite of FUN_00413050: draw a picture straight onto the screen at design position (x, y),
 * relative to the window's client area. Used for menu backgrounds, the pause menu ("Game
 * Options"), and some HUD parts. __fastcall: ecx = picture, edx = x, y on the stack (ret 4).
 * The original sized the destination as src.right x src.bottom (it assumes src starts at 0,0). */
static void __fastcall hud_draw_picture_on_screen_px(UiPicture *pic, int x, int y) {
    if (!pic || !pic->image || (pic->flags & 0x3e)) return;
    int px = x - pic->hot_x, py = y - pic->hot_y;
    int k = layout_menu_scale();                     /* an in-game menu: placed and stretched x k/64 (layout.c) */
    int dw = pic->src.right, dh = pic->src.bottom, centred = 0;
    if (k) {
        int qx = px + picture_w(pic), qy = py + picture_h(pic), ex = px + pic->src.right, ey = py + pic->src.bottom;
        layout_menu_place(k, &px, &py); layout_menu_place(k, &qx, &qy); layout_menu_place(k, &ex, &ey);
        RECT r = { px, py, qx, qy };
        layout_note_panel(ANCHOR_CENTER, &r);
        dw = ex - px; dh = ey - py;
    } else {
        int px0 = px, py0 = py;
        centred = place_picture(pic, &px, &py, 0);
        /* a picture moved by the layout (e.g. the mission-over CONTINUE button beside the radio): clicks on it are
         * mapped back to where the game tests them (2026-11-20, user: the win screen "requires enter button") */
        if (px != px0 || py != py0) {
            RECT on = { px, py, px + pic->src.right, py + pic->src.bottom };
            layout_note_click(&on, px - px0, py - py0);
        }
    }
    {   /* the mission-over radio: drawn by radio.c; what the game draws after it (buttons, panel) is cut out there */
        RECT pr = { px, py, px + dw, py + dh };
        if (radio_take(pic, 1, &pr)) return;         /* an overlay, not a panel on the screen (see place_picture) */
        radio_note_screen_picture(&pr);
    }
    if (centred) note_centred(pic, px, py);
    RECT d;
    d.left = GAME_CLIENT_SCREEN_RECT.left + px;
    d.top = GAME_CLIENT_SCREEN_RECT.top + py;
    d.right = d.left + dw;
    d.bottom = d.top + dh;
    DWORD how = (pic->flags & 1) ? DDBLT_WAIT : DDBLT_WAIT | DDBLT_KEYSRC;
    void *screen = SCREEN;
    ((BltFn)VTBL(screen)[5])(screen, &d, pic->image, &pic->src, how, NULL);
}
void __fastcall hud_draw_picture_on_screen(UiPicture *pic, int x, int y) { if (pic && bad_picture(pic, __builtin_return_address(0))) return; PX_BEGIN(px); hud_draw_picture_on_screen_px(pic, x, y); PX_END(px, PR_UI); }   /* timed: stage PR_UI (exclusive, common.h) */

/* ---------------------------------------------------------------- FUN_00406390 / FUN_00406410 */

/* Rewrite of FUN_00406390 and FUN_00406410 (identical): fill a rectangle on the screen with a
 * colour - e.g. the label boxes and volume bars of the Audio Options menu.
 * __fastcall: ecx = RECT* in design coordinates (relative to the client area), edx = colour
 * (COLORREF), which FUN_00405610 (ecx = surface, edx = colour) turns into a palette index. */
typedef DWORD (__fastcall *ColourIndexFn)(void *surface, DWORD colour);
#define COLOUR_INDEX ((ColourIndexFn)0x405610)

static void __fastcall hud_fill_on_screen_px(RECT *r, DWORD colour) {
    void *screen = SCREEN;
    DDBLTFX fx;
    memset(&fx, 0, sizeof fx);
    fx.dwSize = sizeof fx;
    fx.dwFillColor = COLOUR_INDEX(screen, colour);
    RECT d = *r;
    int k = layout_menu_scale();
    if (k) {                                          /* an in-game menu: x k around the centre (layout.c) */
        int x0 = d.left, y0 = d.top, x1 = d.right, y1 = d.bottom;
        layout_menu_place(k, &x0, &y0); layout_menu_place(k, &x1, &y1);
        SetRect(&d, x0, y0, x1, y1);
    } else if (layout_active()) {
        int x = d.left, y = d.top;
        layout_place(&x, &y);
        OffsetRect(&d, x - d.left, y - d.top);
    }
    OffsetRect(&d, GAME_CLIENT_SCREEN_RECT.left, GAME_CLIENT_SCREEN_RECT.top);
    ((BltFn)VTBL(screen)[5])(screen, &d, NULL, NULL, DDBLT_WAIT | DDBLT_COLORFILL, &fx);
}
void __fastcall hud_fill_on_screen(RECT *r, DWORD colour) { PX_BEGIN(px); hud_fill_on_screen_px(r, colour); PX_END(px, PR_UI); }   /* timed: stage PR_UI (exclusive, common.h) */

/* ---------------------------------------------------------------- text */

/* Text drawn straight onto the SCREEN comes in desktop coordinates in windowed mode (design
 * position + the window's client origin). The anchor must be chosen from the design position, so
 * the origin is taken off, the position placed, and the origin put back. */
static void place_screen_position(int *x, int *y)
{
    int ox = 0, oy = 0;
    if (dd_is_windowed()) { ox = GAME_CLIENT_SCREEN_RECT.left; oy = GAME_CLIENT_SCREEN_RECT.top; }
    *x -= ox; *y -= oy;
    layout_place(x, y);
    *x += ox; *y += oy;
}

/* Wrap of FUN_0044b760 (draw a string with the game's cached font).
 * __fastcall: ecx = surface, edx = x; stack: y, text, font, clip box (int[2] width/height or NULL),
 * colour (ret 0x14). Without a clip box it allows text up to (624 - x, 480 - y): the 640x480 layout
 * again, so we pass that box explicitly for the original position before moving the text. */
DrawStringFn hud_draw_string_original;

static void __fastcall hud_draw_string_px(void *surface, int x, int y, const char *text, int font, int *clip, int colour) {
    int box[2];
    if (surface == BACK_BUFFER) weapons_note_text(x, y, font, colour);   /* ammo counts (weapons.c) */
    if (surface == BACK_BUFFER) objectives_note_text(x, y, font, colour); /* message box font (objectives.c) */
    if (layout_active() && (surface == BACK_BUFFER || surface == SCREEN)) {
        if (!clip) { box[0] = 0x270 - x; box[1] = 0x1e0 - y; clip = box; }
        int k = surface == SCREEN ? layout_menu_scale() : 0;
        if (k) {                                       /* an in-game menu (layout.c): position only, this font cannot grow */
            int ox = dd_is_windowed() ? GAME_CLIENT_SCREEN_RECT.left : 0, oy = dd_is_windowed() ? GAME_CLIENT_SCREEN_RECT.top : 0;
            x -= ox; y -= oy; layout_menu_place(k, &x, &y); x += ox; y += oy;
        } else if (surface == SCREEN) place_screen_position(&x, &y);
        else layout_place(&x, &y);
    }
    PX_GAME(hud_draw_string_original(surface, x, y, text, font, clip, colour));
}
void __fastcall hud_draw_string(void *surface, int x, int y, const char *text, int font, int *clip, int colour) { PX_BEGIN(px); hud_draw_string_px(surface, x, y, text, font, clip, colour); PX_END(px, PR_UI); }   /* timed: stage PR_UI (exclusive, common.h) */

/* Wrap of FUN_00413a30 (draw a string with GDI TextOut; menu labels such as "Controls", "Audio",
 * "Abort Mission" in the pause menu). __fastcall: ecx = surface, edx = x; stack: y, text, font,
 * size out (ret 0x10). Only text drawn straight onto the SCREEN in gameplay is moved: the game
 * builds its fonts (at start-up and again when a mission starts) by drawing every character with
 * TextOut at 0,0 into the back buffer and copying it out, and that must stay where it is. */
GdiTextFn hud_draw_gdi_text_original;

#define GAME_FONTS ((HFONT *)0x4e9f48)       /* the game's menu fonts (FUN_00413a30 selects GAME_FONTS[font]) */
static int __fastcall hud_draw_gdi_text_px(void *surface, int x, int y, const char *text, int font, int *size_out) {
    if (g_cfg.log >= 2 && !layout_view_is_gameplay()) {      /* menu labels: which surface, where, colour (menu renderer) */
        static int n;
        if (n++ < 300) mlog("menutext: %s %d,%d font %d colour %08lx \"%.40s\"", surface == SCREEN ? "SCREEN" : surface == BACK_BUFFER ? "BACK" : "other",
                            x, y, font, *(DWORD *)0x4e9f70, text ? text : "");
    }
    if (menutext_capture(surface, x, y, text, font, size_out)) return 1;   /* full-screen menus: drawn by menutext.c */
    int k = surface == SCREEN ? layout_menu_scale() : 0;
    if (k > 64 && font >= 0 && font < 8 && GAME_FONTS[font]) {
        /* an in-game menu at x k/64 (layout.c, half steps): the same font that much bigger (sharp, not
         * pixel-stretched), the position around the centre; the size reported back in menu units (the game
         * lays out with it) */
        static HFONT big[8][8]; static HFONT made_from[8];
        int ki = k / 32;                                  /* 3..6 = x1.5 .. x3 */
        if (ki > 7) ki = 7;
        if (made_from[font] != GAME_FONTS[font]) {
            for (int i = 0; i < 8; i++) if (big[font][i]) { DeleteObject(big[font][i]); big[font][i] = NULL; }
            made_from[font] = GAME_FONTS[font];
        }
        if (!big[font][ki]) {
            LOGFONTA lf;
            if (GetObjectA(GAME_FONTS[font], sizeof lf, &lf)) {
                lf.lfHeight = lf.lfHeight * k / 64; lf.lfWidth = lf.lfWidth * k / 64;
                lf.lfQuality = NONANTIALIASED_QUALITY;
                big[font][ki] = CreateFontIndirectA(&lf);
            }
        }
        int ox = 0, oy = 0;
        if (dd_is_windowed()) { ox = GAME_CLIENT_SCREEN_RECT.left; oy = GAME_CLIENT_SCREEN_RECT.top; }
        x -= ox; y -= oy;
        layout_menu_place(k, &x, &y);
        x += ox; y += oy;
        HFONT keep = GAME_FONTS[font];
        if (big[font][ki]) GAME_FONTS[font] = big[font][ki];
        int r = PX_CALL(hud_draw_gdi_text_original(surface, x, y, text, font, size_out));
        GAME_FONTS[font] = keep;
        if (size_out) { size_out[0] = (size_out[0] * 64 + k - 1) / k; size_out[1] = (size_out[1] * 64 + k - 1) / k; }
        return r;
    }
    if (k == 64) {                                     /* menu at 1x: every row centred */
        int ox = 0, oy = 0;
        if (dd_is_windowed()) { ox = GAME_CLIENT_SCREEN_RECT.left; oy = GAME_CLIENT_SCREEN_RECT.top; }
        x -= ox; y -= oy;
        layout_menu_place(64, &x, &y);
        x += ox; y += oy;
    } else if (layout_active() && surface == SCREEN) place_screen_position(&x, &y);
    return PX_CALL(hud_draw_gdi_text_original(surface, x, y, text, font, size_out));
}
int __fastcall hud_draw_gdi_text(void *surface, int x, int y, const char *text, int font, int *size_out) { PX_BEGIN(px); int r_ = hud_draw_gdi_text_px(surface, x, y, text, font, size_out); PX_END(px, PR_UI); return r_; }   /* timed: stage PR_UI (exclusive, common.h) */

/* ---------------------------------------------------------------- minimap overlay */

/* The viewport box and unit dots are drawn pixel by pixel by two routines that ONLY the minimap
 * uses (see ui-inventory.md):
 *   FUN_00405980  box outline  __fastcall: ecx x1, edx y1, stack x2, y2 (ret 8); caller 0x438b40
 *   FUN_004057b0  one dot      __fastcall: ecx x, edx y (ret);             callers 0x438d30, 0x446ad0
 * Both use HUD design positions. */
MinimapRectFn hud_minimap_rect_original;
MinimapPlotFn hud_minimap_plot_original;

static void __fastcall hud_minimap_rect_px(int x1, int y1, int x2, int y2) {
    minimap_note_frame(x1, y1, x2, y2);          /* minimap.c: drawn after the pins - copy them */
    if (layout_active()) {
        int dx, dy;
        layout_offset(ANCHOR_HUD, &dx, &dy);
        dx = layout_hud_dx(x1);                  /* right of the split: the minimap */
        x1 += dx; x2 += dx;
        y1 += dy; y2 += dy;
    }
    PX_GAME(hud_minimap_rect_original(x1, y1, x2, y2));
}
void __fastcall hud_minimap_rect(int x1, int y1, int x2, int y2) { PX_BEGIN(px); hud_minimap_rect_px(x1, y1, x2, y2); PX_END(px, PR_UI); }   /* timed: stage PR_UI (exclusive, common.h) */

static void __fastcall hud_minimap_plot_px(int x, int y) {
    if (layout_active()) {
        int dx, dy;
        layout_offset(ANCHOR_HUD, &dx, &dy);
        x += layout_hud_dx(x);
        y += dy;
    }
    PX_GAME(hud_minimap_plot_original(x, y));
}
void __fastcall hud_minimap_plot(int x, int y) { PX_BEGIN(px); hud_minimap_plot_px(x, y); PX_END(px, PR_UI); }   /* timed: stage PR_UI (exclusive, common.h) */

/* ---------------------------------------------------------------- FUN_004206a0 */

#define CURSOR_VISIBLE  (*(int *)0x4fe448)
#define CURSOR_X        (*(int *)0x4fe3e8)
#define CURSOR_Y        (*(int *)0x4fe3ec)
#define SAVED_AT_X      (*(int *)0x4fe168)   /* where the background under the cursor was saved */
#define SAVED_AT_Y      (*(int *)0x4fe16c)
#define SAVED_VALID     (*(int *)0x4fe498)
#define CURSOR_PICTURE  ((UiPicture *)0x4fe3f8)
typedef void (__cdecl *VoidFn)(void);
#define RESTORE_UNDER_CURSOR ((VoidFn)0x420470)
#define SAVE_UNDER_CURSOR    ((VoidFn)0x420510)
typedef void (__fastcall *DrawPictureFn)(UiPicture *pic, int x, int y);
#define DRAW_CURSOR_PICTURE  ((DrawPictureFn)0x413610)

/* Rewrite of FUN_004206a0: draw the mouse cursor on the screen (erase the old one, save what is
 * under the new position, draw). Same logic as the original, but at the real pointer position:
 * while the pointer is over moved interface, the cursor globals hold the design position
 * (layout_pointer_to_game), so the real one is swapped in for the duration. */
static void __cdecl hud_draw_cursor_px(void) {
    mouse_poll_pointer();
    if (!CURSOR_VISIBLE) return;
    /* one cursor only (user): in gameplay, while Windows shows a cursor (the arrow over the interface,
     * the walk ring / enter arrow on the battlefield - mouse.c WM_SETCURSOR), the game's is not drawn */
    int windows_cursor = layout_view_is_gameplay() && (controls_pointer_on_ui() || controls_cursor());
    if (hdart_active() || windows_cursor) {   /* HD menu: a Windows cursor shows instead (hdart.c) */
        if (SAVED_VALID) { RESTORE_UNDER_CURSOR(); SAVED_VALID = 0; }
        return;
    }
    int game_x = CURSOR_X, game_y = CURSOR_Y;
    int real_x, real_y;
    mouse_real_cursor(&real_x, &real_y);
    CURSOR_X = real_x;
    CURSOR_Y = real_y;

    if (SAVED_AT_X != CURSOR_X || SAVED_AT_Y != CURSOR_Y) {
        RESTORE_UNDER_CURSOR();
        SAVE_UNDER_CURSOR();
    } else if (!SAVED_VALID) {
        SAVE_UNDER_CURSOR();
    }
    DRAW_CURSOR_PICTURE(CURSOR_PICTURE, CURSOR_X, CURSOR_Y);

    CURSOR_X = game_x;
    CURSOR_Y = game_y;
}
void __cdecl hud_draw_cursor(void) { PX_BEGIN(px); hud_draw_cursor_px(); PX_END(px, PR_UI); }   /* timed: stage PR_UI (exclusive, common.h) */

/* A HUD picture at a final game-screen position (no layout offset): weapons.c's spare icons. */
void hud_draw_picture_raw(void *picture, int x, int y)
{
    UiPicture *pic = picture;
    if (!pic || bad_picture(pic, __builtin_return_address(0)) || !pic->image || (pic->flags & 0x3e)) return;
    void *back = BACK_BUFFER;
    DWORD how = (pic->flags & 1) ? DDBLTFAST_WAIT : DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY;
    ((BltFastFn)VTBL(back)[7])(back, x - pic->hot_x, y - pic->hot_y, pic->image, &pic->src, how);
}

/* ---------------------------------------------------------------- finished frame
 * Mod drawing on the finished game picture, before it is shown (Flip, or the windowed frame blit),
 * in this order: weapon columns, SPRINT label, mission target on the minimap, pinned objective,
 * then the corner zones are enlarged (layout.c) - so what is drawn into them is enlarged too. */
static void hud_finish_frame_body(Surf *s)
{
    modmenu_tick();                                  /* F10 from play: on into Mod Settings (also on paused frames) */
    /* Only on frames the game really drew: while paused (pause menu) the game shows the same back
     * buffer again without redrawing it, and enlarging / darkening it again would stack up. */
    if (layout_view_is_gameplay() && !g_hud_fresh) return;
    g_hud_fresh = 0;
    { PROF_BEGIN(t); weapons_draw_markers(s); controls_draw_sprint(s); PROF_END(t, PR_H_WEAP); }
    { PROF_BEGIN(t); modern_draw(s); PROF_END(t, PR_H_MODERN); }                /* modern.c: hit markers, arrows, route ... */
    { PROF_BEGIN(t); objectives_draw(s); PROF_END(t, PR_H_OBJ); }
    { PROF_BEGIN(t); layout_scale_corners(s); PROF_END(t, PR_H_CORNERS); }
    { PROF_BEGIN(t); minimap_draw(s); PROF_END(t, PR_H_MINIMAP); }              /* minimap.c: blips over the enlarged minimap */
    PROF_BEGIN(tr);
    layout_draw_resize_icon(s);                      /* the resize icon of the HUD group under the pointer */
    tips_draw(s);
    minimap_draw_tip(s);                             /* minimap.c: details of the blip under the pointer */
    panel_draw(s);
    debugview_draw(s);                               /* debugview.c: hitboxes + names (Ctrl+Shift+H) */
    PROF_END(tr, PR_H_REST);
}

void hud_finish_frame(Surf *s) { PROF_BEGIN(t); hud_finish_frame_body(s); PROF_END(t, PR_HUD); }   /* timed (trace) */
