/* IDirectDrawSurface (1/2/3): 8-bit DIB-backed surfaces and a software blitter. */
#include "common.h"

static const GUID G_IUnknown = {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID G_SURF  = {0x6C14DB81,0xA733,0x11CE,{0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60}};
static const GUID G_SURF2 = {0x57805885,0x6EEC,0x11CF,{0x94,0x41,0xA8,0x23,0x03,0xC1,0x0E,0x27}};
static const GUID G_SURF3 = {0xDA044E00,0x69B2,0x11D0,{0xA1,0xD5,0x00,0xAA,0x00,0xB8,0xDF,0xBB}};

#define S(p) ((Surf *)(p))

static void fill_pixfmt(DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
    pf->dwRGBBitCount = 8;
}

static void fill_desc(Surf *s, DDSURFACEDESC *d)
{
    memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = s->w;
    d->dwHeight = s->h;
    d->lPitch = s->pitch;
    fill_pixfmt(&d->ddpfPixelFormat);
    d->ddsCaps.dwCaps = s->caps;
    if (s->back) { d->dwFlags |= DDSD_BACKBUFFERCOUNT; d->dwBackBufferCount = 1; }
    if (s->has_ck) {
        d->dwFlags |= DDSD_CKSRCBLT;
        d->ddckCKSrcBlt.dwColorSpaceLowValue = s->ck_lo;
        d->ddckCKSrcBlt.dwColorSpaceHighValue = s->ck_hi;
    }
}

static void after_write(Surf *s, int frame_end)
{
    if (s->is_primary) dd_present(s->dd, frame_end);
}

/* Writes to the screen surface are serialised with the display thread's snapshot. */
static void lock_if_primary(Surf *s) { if (s->is_primary) present_lock(); }
static void unlock_if_primary(Surf *s) { if (s->is_primary) present_unlock(); }

/* ---------------------------------------------------------------- blitting */

static int rect_inside(const RECT *r, int w, int h)
{
    return r->left >= 0 && r->top >= 0 && r->right <= w && r->bottom <= h &&
           r->left < r->right && r->top < r->bottom;
}

static void fill_rect(Surf *d, RECT r, BYTE c)
{
    for (int y = r.top; y < r.bottom; y++)
        memset(d->bits + y * d->pitch + r.left, c, r.right - r.left);
}

/* Copy src rect sr onto dst rect dr. dr is already clipped to the destination; full_dr is the
 * unclipped destination, so the source position follows the unclipped mapping. Same size: plain
 * copy (keyed: skip colour-key indices lo..hi); different size: nearest-neighbour stretch. */
static void copy_rect(Surf *d, RECT dr, RECT full_dr, Surf *s, RECT sr, int keyed, DWORD lo, DWORD hi)
{
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    int fw = full_dr.right - full_dr.left, fh = full_dr.bottom - full_dr.top;
    int w = dr.right - dr.left, h = dr.bottom - dr.top;
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;

    if (sw == fw && sh == fh) {
        int sx = sr.left + (dr.left - full_dr.left), sy = sr.top + (dr.top - full_dr.top);
        int y0 = 0, y1 = h, step = 1;
        if (s == d && dr.top > sy) { y0 = h - 1; y1 = -1; step = -1; }   /* overlapping, copy upwards */
        for (int y = y0; y != y1; y += step) {
            BYTE *dp = d->bits + (dr.top + y) * d->pitch + dr.left;
            const BYTE *sp = s->bits + (sy + y) * s->pitch + sx;
            if (!keyed) { memmove(dp, sp, w); continue; }
            for (int x = 0; x < w; x++) {
                BYTE c = sp[x];
                if (c < lo || c > hi) dp[x] = c;
            }
        }
        return;
    }
    /* stretch, nearest neighbour (16.16 fixed point) */
    uint32_t stepx = ((uint32_t)sw << 16) / fw, stepy = ((uint32_t)sh << 16) / fh;
    for (int y = 0; y < h; y++) {
        int sy = sr.top + (int)(((uint32_t)(dr.top - full_dr.top + y) * stepy) >> 16);
        const BYTE *srow = s->bits + sy * s->pitch;
        BYTE *dp = d->bits + (dr.top + y) * d->pitch + dr.left;
        uint32_t fx = (uint32_t)(dr.left - full_dr.left) * stepx;
        for (int x = 0; x < w; x++, fx += stepx) {
            BYTE c = srow[sr.left + (fx >> 16)];
            if (!keyed || c < lo || c > hi) dp[x] = c;
        }
    }
}

/* ---- Windowed mode: the screen surface holds game pixels.
 *
 * In windowed mode the game addresses the screen surface in DESKTOP coordinates:
 *   - its per-frame blit (FUN_004279c0), screen clear and video centring use the client
 *     rectangle GAME_CLIENT_SCREEN_RECT; window.c keeps it at "window's client origin on the
 *     desktop + game size";
 *   - its menu screens (0x4130ca) and mouse cursor (0x4136ea, 0x420504) are drawn at
 *     "client origin + game position".
 * The screen surface is exactly game-sized, so every rectangle just needs the client origin
 * removed. The display thread (present.c / view.c) then enlarges the picture for the window,
 * the same way as in fullscreen. */
#define GAME_CLIENT_SCREEN_RECT (*(RECT *)0x4e2dd8)
#define VIDEO_FRAME_BLIT ((void *)0x46c1ed)   /* return address of FUN_0046c130's Blt (video frame -> screen) */

static RECT screen_to_game(const Surf *screen, RECT r)
{
    if (!dd_is_windowed()) return r;                 /* fullscreen: already game pixels */
    OffsetRect(&r, -GAME_CLIENT_SCREEN_RECT.left, -GAME_CLIENT_SCREEN_RECT.top);
    return r;
}

/* Debugging aid: [debug] log=3 lists the first blits onto the screen and which game code made them. */
static void log_screen_blit(const RECT *dr, const Surf *s, DWORD flags, void *caller)
{
    static int n;
    if (caller == (void *)0x4136ea || caller == (void *)0x420504) return;   /* the mouse cursor: too many */
    if (g_cfg.log >= 3 && n++ < 400)
        mlog("Blt to screen from %p: dst %ld,%ld-%ld,%ld src %p flags %#lx", caller,
             dr->left, dr->top, dr->right, dr->bottom, s, flags);
}

static HRESULT do_blt(Surf *d, LPRECT pdr, Surf *s, LPRECT psr, DWORD flags, LPDDBLTFX fx, void *caller)
{
    /* windowed: the whole game picture blitted to the screen = the finished frame */
    if (d->is_primary && s && !s->is_primary && s->w == d->w && s->h == d->h) hud_finish_frame(s);
    RECT dr = { 0, 0, d->w, d->h };
    if (pdr) dr = *pdr;
    if (d->is_primary) {
        log_screen_blit(&dr, s, flags, caller);
        if (pdr) dr = screen_to_game(d, dr);
    }
    RECT src_override;
    if (s && s->is_primary && psr) {                 /* reading back from the screen (cursor save) */
        src_override = screen_to_game(s, *psr);
        psr = &src_override;
    }

    if (flags & DDBLT_COLORFILL) {
        RECT c = dr;
        if (d->clip || d->is_primary) IntersectRect(&c, &dr, &(RECT){ 0, 0, d->w, d->h });
        else if (!rect_inside(&c, d->w, d->h)) return DDERR_INVALIDRECT;
        lock_if_primary(d);
        fill_rect(d, c, (BYTE)(fx ? fx->dwFillColor : 0));
        hdworld_note_fill(d, c);
        unlock_if_primary(d);
        after_write(d, 0);
        return DD_OK;
    }
    if (!s) return DD_OK;   /* ROP/FX-only blits are not used by the game */

    RECT sr = { 0, 0, s->w, s->h };
    if (psr) sr = *psr;
    if (!rect_inside(&sr, s->w, s->h)) return DDERR_INVALIDRECT;
    if (dr.right <= dr.left || dr.bottom <= dr.top) return DDERR_INVALIDRECT;

    RECT c = dr;
    if (d->clip || d->is_primary) {
        if (!IntersectRect(&c, &dr, &(RECT){ 0, 0, d->w, d->h })) return DD_OK;
    } else if (!rect_inside(&dr, d->w, d->h)) {
        return DDERR_INVALIDRECT;
    }

    int keyed = 0;
    DWORD lo = 0, hi = 0;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) {
        keyed = 1; lo = fx->ddckSrcColorkey.dwColorSpaceLowValue; hi = fx->ddckSrcColorkey.dwColorSpaceHighValue;
    } else if ((flags & DDBLT_KEYSRC) && s->has_ck) {
        keyed = 1; lo = s->ck_lo; hi = s->ck_hi;
    }
    if (hi < lo) hi = lo;
    Surf masked, *from = s;                          /* recall pictures: only where the HD art has ink (hdsprites.c) */
    if (keyed && hdsprites_masked_source(s, lo, &masked)) from = &masked;
    lock_if_primary(d);
    copy_rect(d, c, dr, from, sr, keyed, lo, hi);
    if (d->is_primary && !s->is_primary && s->w == d->w && s->h == d->h) { hdsprites_frame_shown(s); hdworld_frame_shown(s); controls_frame_shown(); }   /* windowed frame */
    else hdsprites_note_blit(d, dr, s, sr, keyed, lo, hi);           /* HD pictures (hdsprites.c) */
    hdworld_note_copy(d, dr, from, sr, keyed, lo, hi);              /* upscaled battlefield (hdworld.c) */
    if (d->is_primary) {                             /* menus/videos decide what is shown (view.c) */
        if (caller == VIDEO_FRAME_BLIT) view_note_video(&c);
        else view_note_picture(&c);
        if (!layout_view_is_gameplay() || game_menu_up()) menutext_note_blit(&c, 1);   /* menutext.c: labels under a new picture are gone */
    }
    unlock_if_primary(d);
    /* A big blit onto the screen is the windowed game's "show frame" (FUN_004279c0) or a menu
     * picture; small ones are its cursor being drawn or erased. */
    int frame_end = (c.right - c.left) * (c.bottom - c.top) >= 320 * 240;
    /* in-game menus drawn x k (layout.c, 2026-11-17): judge their pictures by their 640x480 size - at 2x the
     * buttons and labels crossed the threshold too and each waited a refresh (menu fps tanked) */
    { int k = d->is_primary ? layout_menu_scale() : 0;
      if (k > 64 && (c.right - c.left) * (c.bottom - c.top) < g_cfg.width * g_cfg.height / 2) frame_end = (c.right - c.left) * (c.bottom - c.top) * 64 / k * 64 / k >= 320 * 240; }
    after_write(d, frame_end);
    return DD_OK;
}

/* ---------------------------------------------------------------- methods */

static HRESULT WINAPI s_qi(void *p, REFIID r, void **o)
{
    if (IsEqualGUID(r, &G_IUnknown) || IsEqualGUID(r, &G_SURF) || IsEqualGUID(r, &G_SURF2) ||
        IsEqualGUID(r, &G_SURF3)) {
        S(p)->ref++;
        *o = p;
        return S_OK;
    }
    mlog("IDirectDrawSurface::QueryInterface: unsupported interface %08lX", r->Data1);
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI s_addref(void *p) { return ++S(p)->ref; }
static ULONG WINAPI s_release(void *p) { return surf_release(S(p)); }

static HRESULT WINAPI s_add_attached(void *p, void *a) { return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_add_overlay_dirty(void *p, LPRECT r) { return DDERR_UNSUPPORTED; }

/* Debugging aid: [debug] log=4 lists blits of wide pictures (>= 400 px, e.g. the 640x115 HUD panel)
 * onto other surfaces, and which game code made them. */
static void log_wide_blit(const char *kind, const Surf *d, const Surf *s, const RECT *sr, long x, long y, void *caller)
{
    static int n;
    if (g_cfg.log < 4 || !s || d->is_primary || n >= 400) return;
    /* log=4: wide pictures; log=5: also anything drawn low on the screen (HUD area) */
    if (sr->right - sr->left < 400 && !(g_cfg.log >= 5 && y >= 360)) return;
    n++;
    mlog("%s from %p: src %p %ldx%ld rect %ld,%ld-%ld,%ld -> dst %p (%dx%d) at %ld,%ld", kind, caller, s,
         (long)s->w, (long)s->h, sr->left, sr->top, sr->right, sr->bottom, d, d->w, d->h, x, y);
}

static HRESULT s_blt_body(void *p, LPRECT dr, void *src, LPRECT sr, DWORD flags, LPDDBLTFX fx, void *caller)
{
    if (src) { RECT r = sr ? *sr : (RECT){ 0, 0, S(src)->w, S(src)->h };
               log_wide_blit("Blt", S(p), S(src), &r, dr ? dr->left : 0, dr ? dr->top : 0, caller); }
    return do_blt(S(p), dr, S(src), sr, flags, fx, caller);
}
/* timed (trace stage "dd", 2026-11-21): the game's blits between its own surfaces - our DirectDraw copy incl. the
 * hdworld reference bookkeeping (refs). Not the ones onto the screen (a frame end there waits for the display). */
static HRESULT WINAPI s_blt(void *p, LPRECT dr, void *src, LPRECT sr, DWORD flags, LPDDBLTFX fx)
{
    void *caller = __builtin_return_address(0);
    if (S(p)->is_primary) return s_blt_body(p, dr, src, sr, flags, fx, caller);
    PX_BEGIN(px); HRESULT r = s_blt_body(p, dr, src, sr, flags, fx, caller); PX_END(px, PR_DD); return r;
}
static HRESULT WINAPI s_blt_batch(void *p, LPDDBLTBATCH b, DWORD n, DWORD f) { return DDERR_UNSUPPORTED; }

static HRESULT s_blt_fast_body(void *p, DWORD x, DWORD y, void *src, LPRECT psr, DWORD flags, void *caller)
{
    Surf *d = S(p), *s = S(src);
    if (!s) return DDERR_INVALIDPARAMS;
    RECT sr = { 0, 0, s->w, s->h };
    if (psr) sr = *psr;
    RECT dr = { (LONG)x, (LONG)y, (LONG)x + (sr.right - sr.left), (LONG)y + (sr.bottom - sr.top) };
    log_wide_blit("BltFast", d, s, &sr, x, y, caller);
    if (d->is_primary) {                             /* windowed: desktop -> game pixels */
        RECT g = screen_to_game(d, dr);
        OffsetRect(&dr, g.left - dr.left, g.top - dr.top);
    }
    if (!rect_inside(&sr, s->w, s->h) || !rect_inside(&dr, d->w, d->h)) return DDERR_INVALIDRECT;
    int keyed = (flags & DDBLTFAST_SRCCOLORKEY) && s->has_ck;
    Surf masked, *from = s;                          /* recall pictures: only where the HD art has ink (hdsprites.c) */
    if (keyed && hdsprites_masked_source(s, s->ck_lo, &masked)) from = &masked;
    lock_if_primary(d);
    copy_rect(d, dr, dr, from, sr, keyed, s->ck_lo, s->ck_hi < s->ck_lo ? s->ck_lo : s->ck_hi);
    hdsprites_note_blit(d, dr, s, sr, keyed, s->ck_lo, s->ck_hi < s->ck_lo ? s->ck_lo : s->ck_hi);
    hdworld_note_copy(d, dr, from, sr, keyed, s->ck_lo, s->ck_hi < s->ck_lo ? s->ck_lo : s->ck_hi);
    if (d->is_primary && (!layout_view_is_gameplay() || game_menu_up())) menutext_note_blit(&dr, 0);
    unlock_if_primary(d);
    after_write(d, 0);
    return DD_OK;
}
static HRESULT WINAPI s_blt_fast(void *p, DWORD x, DWORD y, void *src, LPRECT psr, DWORD flags)   /* timed like s_blt */
{
    void *caller = __builtin_return_address(0);
    if (S(p)->is_primary) return s_blt_fast_body(p, x, y, src, psr, flags, caller);
    PX_BEGIN(px); HRESULT r = s_blt_fast_body(p, x, y, src, psr, flags, caller); PX_END(px, PR_DD); return r;
}

static HRESULT WINAPI s_delete_attached(void *p, DWORD f, void *a) { return DDERR_UNSUPPORTED; }
static HRESULT WINAPI s_enum_attached(void *p, LPVOID c, LPDDENUMSURFACESCALLBACK cb)
{
    Surf *s = S(p);
    if (s->back) {
        DDSURFACEDESC d;
        fill_desc(s->back, &d);
        s->back->ref++;
        cb((LPDIRECTDRAWSURFACE)s->back, &d, c);
    }
    return DD_OK;
}
static HRESULT WINAPI s_enum_overlay(void *p, DWORD f, LPVOID c, LPDDENUMSURFACESCALLBACK cb) { return DD_OK; }

static HRESULT WINAPI s_flip(void *p, void *target, DWORD flags)
{
    Surf *s = S(p);
    Surf *from = target ? S(target) : s->back;
    if (!s->is_primary || !from) return DDERR_NOTFLIPPABLE;
    hud_finish_frame(from);                          /* hud.c: mod drawing on the finished frame */
    /* Copy rather than swap: the back buffer keeps the frame just shown, which is what the
     * game's renderer expects in practice and is never worse than real flip semantics. */
    present_lock();
    for (int y = 0; y < s->h && y < from->h; y++)
        memcpy(s->bits + y * s->pitch, from->bits + y * from->pitch, s->w < from->w ? s->w : from->w);
    view_note_whole_frame();                         /* a full game frame: show all of it */
    hdsprites_frame_shown(from);
    hdworld_frame_shown(from);
    controls_frame_shown();
    present_unlock();
    dd_present(s->dd, 1);
    return DD_OK;
}

static HRESULT WINAPI s_get_attached(void *p, LPDDSCAPS caps, void **o)
{
    Surf *s = S(p);
    if (s->back && (caps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP))) {
        s->back->ref++;
        *o = s->back;
        return DD_OK;
    }
    *o = NULL;
    return DDERR_NOTFOUND;
}
static HRESULT WINAPI s_get_blt_status(void *p, DWORD f) { return DD_OK; }
static HRESULT WINAPI s_get_caps(void *p, LPDDSCAPS c) { c->dwCaps = S(p)->caps; return DD_OK; }
static HRESULT WINAPI s_get_clipper(void *p, void **o)
{
    Clip *c = S(p)->clip;
    *o = c;
    if (!c) return DDERR_NOCLIPPERATTACHED;
    c->ref++;
    return DD_OK;
}
static HRESULT WINAPI s_get_color_key(void *p, DWORD f, LPDDCOLORKEY k)
{
    Surf *s = S(p);
    if (!(f & DDCKEY_SRCBLT) || !s->has_ck) return DDERR_NOCOLORKEY;
    k->dwColorSpaceLowValue = s->ck_lo;
    k->dwColorSpaceHighValue = s->ck_hi;
    return DD_OK;
}

static HRESULT WINAPI s_get_dc(void *p, HDC *out)
{
    Surf *s = S(p);
    if (!s->is_primary) hdsprites_dirty(s);         /* GDI drawing changes it: identify again */
    if (g_cfg.log >= 5) {                            /* debugging: who draws text with GDI */
        static int n;
        if (n++ < 60) mlog("GetDC on %s %p from %p", s->is_primary ? "screen" : "surface", s, __builtin_return_address(0));
    }
    if (!s->dc) {
        s->dc = CreateCompatibleDC(NULL);
        s->old_bmp = SelectObject(s->dc, s->dib);
    }
    /* Windowed: the game draws text onto the screen in desktop coordinates, like its blits
     * (see screen_to_game). Shift the DC origin so it lands in game pixels. */
    POINT org = { 0, 0 };
    if (s->is_primary && dd_is_windowed()) {
        org.x = -GAME_CLIENT_SCREEN_RECT.left;
        org.y = -GAME_CLIENT_SCREEN_RECT.top;
    }
    SetViewportOrgEx(s->dc, org.x, org.y, NULL);
    /* keep the DIB colour table equal to the screen palette so GDI picks the right indices */
    Pal *pal = s->pal ? s->pal : s->dd->pal;
    if (pal) {
        RGBQUAD q[256];
        for (int i = 0; i < 256; i++) {
            q[i].rgbRed = pal->e[i].peRed; q[i].rgbGreen = pal->e[i].peGreen;
            q[i].rgbBlue = pal->e[i].peBlue; q[i].rgbReserved = 0;
        }
        SetDIBColorTable(s->dc, 0, 256, q);
    }
    *out = s->dc;
    return DD_OK;
}
static HRESULT WINAPI s_get_flip_status(void *p, DWORD f) { return DD_OK; }
static HRESULT WINAPI s_get_overlay_pos(void *p, LPLONG x, LPLONG y) { return DDERR_NOTAOVERLAYSURFACE; }
static HRESULT WINAPI s_get_palette(void *p, void **o)
{
    Pal *pl = S(p)->pal;
    *o = pl;
    if (!pl) return DDERR_NOPALETTEATTACHED;
    pl->ref++;
    return DD_OK;
}
static HRESULT WINAPI s_get_pixfmt(void *p, LPDDPIXELFORMAT pf) { fill_pixfmt(pf); return DD_OK; }
static HRESULT WINAPI s_get_desc(void *p, LPDDSURFACEDESC d) { fill_desc(S(p), d); return DD_OK; }
static HRESULT WINAPI s_initialize(void *p, void *dd, LPDDSURFACEDESC d) { return DDERR_ALREADYINITIALIZED; }
static HRESULT WINAPI s_is_lost(void *p) { return DD_OK; }

static HRESULT WINAPI s_lock(void *p, LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e)
{
    Surf *s = S(p);
    hdsprites_dirty(s);                              /* the game writes into it: identify again */
    fill_desc(s, d);
    d->dwFlags |= DDSD_LPSURFACE;
    d->lpSurface = s->bits;
    if (r) {
        if (!rect_inside(r, s->w, s->h)) return DDERR_INVALIDRECT;
        d->lpSurface = s->bits + r->top * s->pitch + r->left;
    }
    return DD_OK;
}
static HRESULT WINAPI s_release_dc(void *p, HDC dc) { GdiFlush(); after_write(S(p), 0); return DD_OK; }
static HRESULT WINAPI s_restore(void *p) { return DD_OK; }
static HRESULT WINAPI s_set_clipper(void *p, void *c)
{
    Surf *s = S(p);
    if (c) ((Clip *)c)->ref++;
    if (s->clip) ((ULONG (WINAPI *)(void *))s->clip->vtbl[2])(s->clip);
    s->clip = c;
    return DD_OK;
}
static HRESULT WINAPI s_set_color_key(void *p, DWORD f, LPDDCOLORKEY k)
{
    Surf *s = S(p);
    if (!(f & DDCKEY_SRCBLT)) return DD_OK;   /* only source keys matter for a software blitter */
    if (!k) { s->has_ck = 0; return DD_OK; }
    s->has_ck = 1;
    s->ck_lo = k->dwColorSpaceLowValue;
    s->ck_hi = (f & DDCKEY_COLORSPACE) ? k->dwColorSpaceHighValue : k->dwColorSpaceLowValue;
    return DD_OK;
}
static HRESULT WINAPI s_set_overlay_pos(void *p, LONG x, LONG y) { return DDERR_NOTAOVERLAYSURFACE; }
static HRESULT WINAPI s_set_palette(void *p, void *pal)
{
    Surf *s = S(p);
    Pal *np = pal;
    if (np) np->ref++;
    if (s->pal) ((ULONG (WINAPI *)(void *))s->pal->vtbl[2])(s->pal);
    s->pal = np;
    if (s->is_primary) {
        present_lock();
        s->dd->pal = np;
        present_unlock();
        dd_present(s->dd, 0);
    }
    return DD_OK;
}
static HRESULT WINAPI s_unlock(void *p, LPVOID bits) { after_write(S(p), 0); return DD_OK; }
static HRESULT WINAPI s_update_overlay(void *p, LPRECT a, void *d, LPRECT b, DWORD f, LPDDOVERLAYFX fx) { return DDERR_NOTAOVERLAYSURFACE; }
static HRESULT WINAPI s_update_overlay_display(void *p, DWORD f) { return DDERR_NOTAOVERLAYSURFACE; }
static HRESULT WINAPI s_update_overlay_z(void *p, DWORD f, void *r) { return DDERR_NOTAOVERLAYSURFACE; }
static HRESULT WINAPI s_get_dd(void *p, void **o) { *o = &S(p)->dd->vtbl1; S(p)->dd->ref++; return DD_OK; }
static HRESULT WINAPI s_page_lock(void *p, DWORD f) { return DD_OK; }
static HRESULT WINAPI s_page_unlock(void *p, DWORD f) { return DD_OK; }
static HRESULT WINAPI s_set_desc(void *p, LPDDSURFACEDESC d, DWORD f) { return DDERR_UNSUPPORTED; }

static const void *const vt_surf[] = {
    s_qi, s_addref, s_release, s_add_attached, s_add_overlay_dirty, s_blt, s_blt_batch, s_blt_fast,
    s_delete_attached, s_enum_attached, s_enum_overlay, s_flip, s_get_attached, s_get_blt_status,
    s_get_caps, s_get_clipper, s_get_color_key, s_get_dc, s_get_flip_status, s_get_overlay_pos,
    s_get_palette, s_get_pixfmt, s_get_desc, s_initialize, s_is_lost, s_lock, s_release_dc, s_restore,
    s_set_clipper, s_set_color_key, s_set_overlay_pos, s_set_palette, s_unlock, s_update_overlay,
    s_update_overlay_display, s_update_overlay_z, s_get_dd, s_page_lock, s_page_unlock, s_set_desc,
};

/* ---------------------------------------------------------------- lifetime */

unsigned surf_count_made, surf_count_gone;   /* savestate.c: surfaces made / destroyed (log) */

/* Every surface alive (savestate.c, 2026-11-21): a quickload puts the game back to a moment when it held other
 * surfaces than now. While a quicksave exists (surf_keep_released), a surface the game releases is not destroyed but
 * kept as a ZOMBIE (ref 0, memory and pixels kept): a load brings back the reference counts of that moment
 * (surf_restore_refs), so a surface the snapshot still uses is alive again; zombies no slot needs any more are
 * destroyed (surf_purge). The primary surface and its back buffer are never kept. */
static Surf **g_all; static int g_nall, g_allcap;
static int g_keep_released;
static void all_add(Surf *s)
{
    if (g_nall == g_allcap) {
        int cap = g_allcap ? g_allcap * 2 : 256;
        Surf **n = realloc(g_all, sizeof *n * cap);
        if (!n) return;
        g_all = n; g_allcap = cap;
    }
    g_all[g_nall++] = s;
}
static void all_remove(Surf *s) { for (int i = 0; i < g_nall; i++) if (g_all[i] == s) { g_all[i] = g_all[--g_nall]; return; } }
static void surf_destroy(Surf *s);

static Surf *surf_alloc(DDraw *dd, int w, int h, DWORD caps)
{
    Surf *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    if (!s) return NULL;
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi = { 0 };
    bi.h.biSize = sizeof bi.h;
    bi.h.biWidth = w;
    bi.h.biHeight = -h;            /* top-down, like a locked DirectDraw surface */
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 8;
    bi.h.biCompression = BI_RGB;
    bi.h.biClrUsed = 256;
    for (int i = 0; i < 256; i++) bi.c[i].rgbRed = bi.c[i].rgbGreen = bi.c[i].rgbBlue = (BYTE)i;
    void *bits = NULL;
    s->dib = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!s->dib) { HeapFree(GetProcessHeap(), 0, s); return NULL; }
    all_add(s);
    s->vtbl = vt_surf;
    s->ref = 1;
    surf_count_made++;
    s->dd = dd;
    s->caps = caps;
    s->w = w; s->h = h;
    s->pitch = (w + 3) & ~3;
    s->bits = bits;
    memset(s->bits, 0, (size_t)s->pitch * h);
    return s;
}

ULONG surf_release(Surf *s)
{
    if (s->ref <= 0) return 0;                       /* a zombie (savestate) released again by a stale caller */
    ULONG r = --s->ref;
    if (r) return r;
    if (g_keep_released && !s->is_primary && !(s->caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER))) return 0;   /* zombie */
    surf_destroy(s);
    return 0;
}

static void surf_destroy(Surf *s)
{
    surf_count_gone++;
    all_remove(s);
    present_lock();                                 /* the display thread may be reading it (hdsprites.c) */
    hdsprites_forget(s);
    present_unlock();
    if (s->back) surf_release(s->back);
    if (s->dc) { SelectObject(s->dc, s->old_bmp); DeleteDC(s->dc); }
    DeleteObject(s->dib);
    hdworld_forget(s);
    free(s->hd_ref); free(s->hd_tiles);              /* hdworld.c */
    if (s->pal) ((ULONG (WINAPI *)(void *))s->pal->vtbl[2])(s->pal);
    if (s->clip) ((ULONG (WINAPI *)(void *))s->clip->vtbl[2])(s->clip);
    if (s->is_primary && s->dd->primary == s) {
        present_lock();
        s->dd->primary = NULL;
        s->dd->pal = NULL;
        present_unlock();
    }
    HeapFree(GetProcessHeap(), 0, s);
}

/* savestate.c: the surfaces alive now with their reference counts */
int surf_snapshot(SurfRef *out, int max)
{
    int n = 0;
    for (int i = 0; i < g_nall && n < max; i++) if (g_all[i]->ref > 0) { out[n].s = g_all[i]; out[n].ref = g_all[i]->ref; n++; }
    return n;
}
/* savestate.c, on a load: the reference counts of the save (zombies come back); returns how many came back */
int surf_restore_refs(const SurfRef *list, int n)
{
    int back = 0;
    for (int k = 0; k < n; k++)
        for (int i = 0; i < g_nall; i++)
            if (g_all[i] == list[k].s) { if (g_all[i]->ref <= 0) back++; g_all[i]->ref = list[k].ref; break; }
    return back;
}
void surf_keep_released(int on) { g_keep_released = on; }
/* destroy the zombies no slot needs (needed(s) = 0); returns how many */
int surf_purge(int (*needed)(Surf *s))
{
    int n = 0;
    for (int i = g_nall - 1; i >= 0; i--) {
        if (i >= g_nall) continue;
        Surf *s = g_all[i];
        if (s->ref <= 0 && !needed(s)) { surf_destroy(s); n++; }
    }
    return n;
}
/* surfaces alive (ref > 0) that are not in the list: made after that save (log) */
int surf_count_not_in(const SurfRef *list, int n)
{
    int c = 0;
    for (int i = 0; i < g_nall; i++) {
        if (g_all[i]->ref <= 0) continue;
        int in = 0;
        for (int k = 0; k < n && !in; k++) in = list[k].s == g_all[i];
        if (!in) c++;
    }
    return c;
}

HRESULT surf_create(DDraw *dd, DDSURFACEDESC *desc, Surf **out)
{
    *out = NULL;
    DWORD caps = (desc->dwFlags & DDSD_CAPS) ? desc->ddsCaps.dwCaps : DDSCAPS_OFFSCREENPLAIN;
    int w, h;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        /* fullscreen: the display mode the game asked for. windowed: game-sized, see
         * screen_to_game() (real DirectDraw would give the whole desktop here) */
        if (dd->coop & DDSCL_FULLSCREEN) {
            w = dd->mode_w ? dd->mode_w : g_cfg.width;
            h = dd->mode_h ? dd->mode_h : g_cfg.height;
        } else {
            w = g_cfg.width;
            h = g_cfg.height;
        }
    } else {
        if (!(desc->dwFlags & DDSD_WIDTH) || !(desc->dwFlags & DDSD_HEIGHT)) return DDERR_INVALIDPARAMS;
        w = desc->dwWidth; h = desc->dwHeight;
    }
    if (w <= 0 || h <= 0) return DDERR_INVALIDPARAMS;

    Surf *s = surf_alloc(dd, w, h, caps | DDSCAPS_PALETTE);
    if (!s) return DDERR_OUTOFMEMORY;
    if (desc->dwFlags & DDSD_CKSRCBLT) {
        s->has_ck = 1;
        s->ck_lo = desc->ddckCKSrcBlt.dwColorSpaceLowValue;
        s->ck_hi = desc->ddckCKSrcBlt.dwColorSpaceHighValue;
        if (s->ck_hi < s->ck_lo) s->ck_hi = s->ck_lo;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s->is_primary = 1;
        s->caps |= DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER;
        if ((desc->dwFlags & DDSD_BACKBUFFERCOUNT) && desc->dwBackBufferCount > 0) {
            s->back = surf_alloc(dd, w, h, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_PALETTE);
            if (!s->back) { surf_release(s); return DDERR_OUTOFMEMORY; }
        }
        dd->primary = s;
    }
    mlog("CreateSurface caps %#lx %dx%d%s%s -> %p", caps, w, h, s->back ? " +backbuffer" : "",
         s->has_ck ? " colorkey" : "", s);
    *out = s;
    return DD_OK;
}
