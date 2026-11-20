/* ============================================================================================
 * zoom.c - battlefield zoom: bigger soldiers and vehicles, the interface stays the same size
 *
 * The game still draws its whole battlefield at the render size; the display (gpu.c scroll_uv)
 * shows the middle part of it enlarged by the zoom factor, around the point where the camera keeps
 * Sarge. Interface pixels (HUD panel and corners, spare-weapon column, our boxes: the HUD mask in
 * present.c) are not zoomed. Battlefield pixels whose zoomed source falls on the interface (next to
 * the HUD panel's top at small zooms) take the battlefield just above it.
 * Pointer positions over the battlefield are mapped through the zoom (view.c), so aiming, walking
 * and clicks land where the pointer is.
 * Off while a pop-up / sign / pause menu is up, outside gameplay, and in the game's own exclusive
 * fullscreen. Ctrl + mouse wheel, Ctrl + PageUp / PageDown, or the F10 panel; [display] zoom.
 * ============================================================================================ */
#include "common.h"
#include <math.h>
#include <stdio.h>

#define VIEW_W   (*(short *)0x4e2fb8)        /* battlefield view size (FUN_0040c630) */
#define VIEW_H   (*(short *)0x4e2fba)
#define HUD_COVER_H 94                       /* controls.c: Sarge sits this/2 above the view centre */

static const float LEVELS[] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f };
#define NLEVELS ((int)(sizeof LEVELS / sizeof LEVELS[0]))
#define EASE_MS 120.0                        /* zoom changes glide over about this long */

static volatile float g_shown = 1.0f;        /* eased value used for drawing and the pointer */

float zoom_target(void) { return g_cfg.zoom < 1.0f ? 1.0f : g_cfg.zoom; }

/* Zoom applies right now (gameplay, whole picture, nothing popped up). */
int zoom_active(void)
{
    return zoom_target() > 1.0f && layout_view_is_gameplay() && !layout_any_panel_open() && dd_is_windowed()
           && !modern_no_zoom_now();                    /* modern.c: not while driving */
}

/* Current zoom factor (eased; 1 = off). Called by the display thread every frame and by the pointer
 * mapping; the easing runs on the high-resolution clock. */
float zoom_factor(void)
{
    static LARGE_INTEGER last, fq;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    float want = zoom_active() ? zoom_target() : 1.0f;
    double dt = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)fq.QuadPart : 1e9;
    last = now;
    float z = g_shown;
    if (dt > 500 || fabsf(want - z) < 0.002f) z = want;
    else z += (want - z) * (float)(1.0 - exp(-dt / EASE_MS * 3.0));
    g_shown = z;
    return z;
}

/* The fixed point of the zoom, in game pixels: where the camera keeps Sarge. */
void zoom_centre(float *x, float *y)
{
    int vw = VIEW_W > 0 ? VIEW_W : g_cfg.width, vh = VIEW_H > 0 ? VIEW_H : g_cfg.height;
    *x = vw / 2.0f;
    *y = vh / 2.0f - (g_cfg.width > LAYOUT_DESIGN_W ? HUD_COVER_H / 2.0f : 0.0f);
}

/* Is the game-screen point interface (not zoomed)? */
static int on_interface(int x, int y)
{
    return layout_on_hud_panel(x, y) || weapons_ui_at(x, y) || overlay_at(x, y);
}

/* Unzoomed game position (as shown) -> the battlefield position the game must use. */
void zoom_to_game(POINT *p)
{
    float z = g_shown;
    if (z <= 1.001f || !zoom_active() || on_interface(p->x, p->y)) return;
    float cx, cy; zoom_centre(&cx, &cy);
    p->x = (LONG)floorf(cx + (p->x + 0.5f - cx) / z);
    p->y = (LONG)floorf(cy + (p->y + 0.5f - cy) / z);
}

/* Battlefield position -> where it is shown (unzoomed game position). */
void zoom_from_game(POINT *p)
{
    float z = g_shown;
    if (z <= 1.001f || !zoom_active() || on_interface(p->x, p->y)) return;
    float cx, cy; zoom_centre(&cx, &cy);
    p->x = (LONG)floorf(cx + (p->x + 0.5f - cx) * z);
    p->y = (LONG)floorf(cy + (p->y + 0.5f - cy) * z);
}

void zoom_step(int dir)
{
    float cur = zoom_target();
    int i = 0;
    while (i < NLEVELS - 1 && LEVELS[i] < cur - 0.01f) i++;   /* nearest level at or above */
    if (fabsf(LEVELS[i] - cur) > 0.01f && dir < 0) i++;       /* between levels: "down" = that level's lower one */
    i += dir;
    if (i < 0) i = 0;
    if (i >= NLEVELS) i = NLEVELS - 1;
    g_cfg.zoom = LEVELS[i];
    char buf[16]; snprintf(buf, sizeof buf, "%.2f", g_cfg.zoom);
    WritePrivateProfileStringA("display", "zoom", buf, mod_ini_path());
    mlog("zoom: %.2fx (now active %d: gameplay %d, pop-up %d, windowed %d)", g_cfg.zoom, zoom_active(),
         layout_view_is_gameplay(), layout_any_panel_open(), dd_is_windowed());
}

/* F10 panel: click cycles through the levels. */
void zoom_cycle(void)
{
    int i = 0;
    while (i < NLEVELS && LEVELS[i] <= zoom_target() + 0.01f) i++;
    float z = LEVELS[i < NLEVELS ? i : 0];
    g_cfg.zoom = z;
    char buf[16]; snprintf(buf, sizeof buf, "%.2f", z);
    WritePrivateProfileStringA("display", "zoom", buf, mod_ini_path());
    mlog("zoom: %.2fx", z);
}

/* Window hook: Ctrl + wheel, Ctrl + PageUp / PageDown. Returns 1 when used. */
int zoom_message(UINT msg, WPARAM wp)
{
    if (!layout_view_is_gameplay()) return 0;
    if (msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)) {
        zoom_step(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
        return 1;
    }
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && (wp == VK_PRIOR || wp == VK_NEXT)) {
        zoom_step(wp == VK_PRIOR ? 1 : -1);
        return 1;
    }
    return 0;
}
