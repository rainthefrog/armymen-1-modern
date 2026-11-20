/* ============================================================================================
 * hdsprites.c - upscaled (HD) versions of the game's pictures
 *
 * <game>\hd\ mirrors the game folder with enlarged versions of its BMPs (same relative paths;
 * .bmp or .png, any scale): menu screens, buttons, briefing pictures, paper maps, minimaps ...
 * The game keeps drawing its original 8-bit pictures; the HD versions are only SHOWN:
 *   1. catalogue: for every HD file, a grid of colour samples of the ORIGINAL (same path in the
 *      game folder) and its size;
 *   2. when a surface is used as a blit source for the first time (or after the game wrote to it
 *      again: Lock / GetDC), its pixels are compared with the catalogue entries of the same size
 *      (>= 95 % of the samples): that surface IS that picture;
 *   3. every blit of an identified picture onto the screen or the back buffer is noted (where, which
 *      part); Flip / the windowed frame blit hand the back buffer's notes to the screen;
 *   4. when a frame is shown, an id map marks every screen pixel that still shows a noted picture
 *      (the screen pixel equals the picture's pixel - text, highlights, the cursor drawn over it
 *      break the match), and gpu.c draws the HD picture exactly there (ps_sprite).
 * [display] hd_sprites = 1
 * ============================================================================================ */
#include "common.h"
#include <math.h>

#define MAX_ENTRIES 4096                 /* menus etc. + ~900 HUD pictures (hd\hud) */
#define GRID 3                         /* sample every GRID pixels of the original */
#define TOL 30                         /* summed colour difference for a matching sample */
#define MAX_OPS 128
#define EDGE_GROW 0                    /* game px the HD picture may reach past its original outline (Codex edges). 0 = off: 2 covered the selection frame, "No unit" and smeared the logo (2026-11-18) */

typedef struct {
    char path[MAX_PATH];               /* the HD file */
    int w, h;                          /* the original's size */
    uint32_t *samples;                 /* original colours on the GRID */
    uint32_t *hd; int hw, hh, tried;   /* HD pixels (BGRA), loaded when first needed */
    unsigned serial;                   /* gpu.c texture cache key */
} Entry;

static Entry g_e[MAX_ENTRIES];
static int is_hd_ink(uint32_t p);
static int g_n, g_scanned;

/* q != 64: an enlarged HUD copy (layout.c). The source pixel shown at screen (X, Y) is EXACTLY the one the copy
 * took: floor(ax + X * 64 / q), floor(ay + Y * 64 / q) - the same arithmetic, so the HD picture lands on the
 * copy's pixels (a separately rounded rectangle was 1-3 px off: doubled lines, smeared letters, 2026-11-18). */
typedef struct { RECT dr; RECT sr; Surf *src; int id; int keyed; DWORD lo, hi; int q; double ax, ay; } Op;
static Op g_screen_ops[MAX_OPS], g_back_ops[MAX_OPS];
static int g_screen_n, g_back_n;

/* ---------------------------------------------------------------- catalogue */

/* An 8-bit BMP's colours on the sample grid (any size), or NULL. */
static uint32_t *original_samples(const char *path, int *w, int *h)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL), got = 0;
    BYTE *d = size > 1078 && size < 16u << 20 ? malloc(size) : NULL;
    if (d && (!ReadFile(f, d, size, &got, NULL) || got != size)) { free(d); d = NULL; }
    CloseHandle(f);
    if (!d) return NULL;
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)(d + 14);
    DWORD off = *(DWORD *)(d + 10);
    uint32_t *out = NULL;
    int W = bi->biWidth, H = labs(bi->biHeight);
    if (d[0] == 'B' && d[1] == 'M' && bi->biBitCount == 8 && !bi->biCompression && W > 0 && H > 0 && W <= 2048 && H <= 2048) {
        RGBQUAD *pal = (RGBQUAD *)((BYTE *)bi + bi->biSize);
        int stride = (W + 3) & ~3, gw = (W + GRID - 1) / GRID, gh = (H + GRID - 1) / GRID;
        if (off + (DWORD)stride * H <= size && (out = malloc((size_t)gw * gh * 4))) {
            for (int gy = 0; gy < gh; gy++)
                for (int gx = 0; gx < gw; gx++) {
                    int y = gy * GRID, x = gx * GRID;
                    BYTE i = d[off + (size_t)(bi->biHeight > 0 ? H - 1 - y : y) * stride + x];
                    out[gy * gw + gx] = (uint32_t)pal[i].rgbRed << 16 | (uint32_t)pal[i].rgbGreen << 8 | pal[i].rgbBlue;
                }
            *w = W; *h = H;
        }
    }
    free(d);
    return out;
}

static void scan_dir(const char *hd_root, const char *rel)
{
    char pattern[MAX_PATH];
    wsprintfA(pattern, "%s\\%s*", hd_root, rel);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char sub[MAX_PATH];
        wsprintfA(sub, "%s%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char next[MAX_PATH];
            wsprintfA(next, "%s\\", sub);
            scan_dir(hd_root, next);
            continue;
        }
        const char *dot = strrchr(fd.cFileName, '.');
        if (!dot || (lstrcmpiA(dot, ".bmp") && lstrcmpiA(dot, ".png")) || g_n >= MAX_ENTRIES) continue;
        if (dot - fd.cFileName >= 5 && !lstrcmpiA(dot - 5, ".orig.bmp")) continue;   /* an original, not an HD picture */
        /* the original: same path in the game folder, as .bmp - or, for pictures the game makes itself (the HUD,
         * from objects.dat; tooling/scripts/hud_hd.py), <name>.orig.bmp next to the HD file */
        char orel[MAX_PATH], opath[MAX_PATH];
        lstrcpynA(orel, sub, sizeof orel);
        lstrcpyA(orel + (strrchr(orel, '.') - orel), ".bmp");
        wsprintfA(opath, "%s\\%s", hd_root, sub);
        lstrcpyA(opath + (strrchr(opath, '.') - opath), ".orig.bmp");
        if (GetFileAttributesA(opath) == INVALID_FILE_ATTRIBUTES) hdart_game_path(opath, MAX_PATH, orel);
        Entry *e = &g_e[g_n];
        if (!(e->samples = original_samples(opath, &e->w, &e->h))) continue;
        wsprintfA(e->path, "%s\\%s", hd_root, sub);
        g_n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static volatile LONG g_ready;          /* the catalogue is complete (scan thread) */

/* Reads the catalogue on its own thread, started when the DLL loads: reading a few hundred
 * originals must not hold up the game (it stalled the opening logo video). */
static DWORD WINAPI scan_thread(void *arg)
{
    char root[MAX_PATH];
    hdart_game_path(root, MAX_PATH, "hd");
    if (GetFileAttributesA(root) != INVALID_FILE_ATTRIBUTES) {
        scan_dir(root, "");
        mlog("hdsprites: %d upscaled pictures in hd\\", g_n);
    }
    InterlockedExchange(&g_ready, 1);
    return 0;
}

void hdsprites_start(void)
{
    if (g_scanned) return;                   /* loaded even when off: F12 can switch it on */
    g_scanned = 1;
    HANDLE t = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else g_ready = 1;
}

/* ---------------------------------------------------------------- identifying surfaces */

static int close_enough(uint32_t a, uint32_t b)
{
    return abs((int)(a >> 16 & 255) - (int)(b >> 16 & 255)) + abs((int)(a >> 8 & 255) - (int)(b >> 8 & 255))
         + abs((int)(a & 255) - (int)(b & 255)) <= TOL;
}

static int identify(Surf *s)
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    if (!p || !s->bits) return -1;
    int best = -1, best_p = 0;
    for (int i = 0; i < g_n; i++) {
        Entry *e = &g_e[i];
        if (e->w != s->w || e->h != s->h) continue;
        int gw = (e->w + GRID - 1) / GRID, gh = (e->h + GRID - 1) / GRID, hit = 0, n = 0;
        for (int gy = 0; gy < gh; gy++)
            for (int gx = 0; gx < gw; gx++, n++) {
                BYTE c = s->bits[(size_t)gy * GRID * s->pitch + gx * GRID];
                uint32_t rgb = (uint32_t)p->e[c].peRed << 16 | (uint32_t)p->e[c].peGreen << 8 | p->e[c].peBlue;
                hit += close_enough(rgb, e->samples[gy * gw + gx]);
            }
        int pc = n ? hit * 100 / n : 0;
        if (pc > best_p) { best_p = pc; best = i; }
    }
    if (best_p < 95) return -1;
    Entry *e = &g_e[best];
    if (!e->tried) {
        e->tried = 1;
        e->hd = hdart_load_image_alpha(e->path, &e->hw, &e->hh);
        static unsigned serial;
        e->serial = ++serial;
        if (g_cfg.log) mlog("hdsprites: %s (%dx%d -> %dx%d)%s", e->path, e->w, e->h, e->hw, e->hh, e->hd ? "" : " could not be read");
    }
    return e->hd ? best : -1;
}

void hdsprites_dirty(Surf *s) { if (s) s->hd_state = 0; }

static int surface_id(Surf *s)
{
    if (!g_ready) return -1;                 /* catalogue still loading: try again later */
    if (!g_n) return -1;
    if (!s->hd_state) { s->hd_state = 1; s->hd_id = identify(s); }
    return s->hd_id;
}

/* ---------------------------------------------------------------- noting blits */

static void push(Op *ops, int *n, const Op *op)
{
    /* the same blit again (same source, same rectangles - a keyed pop-up redrawn every frame) replaces its old
     * note: keyed notes were never replaced, the list filled up to MAX_OPS copies and hdsprites_prepare walked
     * them all every frame under the present lock (snap 20+ ms while a sign's text showed - trace 2026-11-17) */
    for (int i = 0; i < *n; i++)
        if (ops[i].src == op->src && EqualRect(&ops[i].dr, &op->dr) && EqualRect(&ops[i].sr, &op->sr)) {
            memmove(&ops[i], &ops[i + 1], sizeof *ops * (*n - i - 1));
            (*n)--;
            break;
        }
    /* an opaque picture covering an older note replaces it */
    for (int i = 0; i < *n; i++) {
        RECT u;
        if (!op->keyed && UnionRect(&u, &ops[i].dr, &op->dr) && EqualRect(&u, &op->dr)) {
            memmove(&ops[i], &ops[i + 1], sizeof *ops * (*n - i - 1));
            (*n)--; i--;
        }
    }
    if (*n == MAX_OPS) { memmove(&ops[0], &ops[1], sizeof *ops * (MAX_OPS - 1)); (*n)--; }
    ops[(*n)++] = *op;
}

/* surface.c: a blit of s (part sr) to d at dr (same size). */
/* The last whole-screen picture drawn (an index into the catalogue): modern.c lets any key / click leave the
 * title screen (Title_legal.bmp, after the intro videos; the game takes only Esc there - user 2026-11-17). */
static volatile int g_last_big = -1;
int hdsprites_showing(const char *name)
{
    int id = g_last_big;
    if (id < 0 || id >= g_n) return 0;
    char low[MAX_PATH]; lstrcpynA(low, g_e[id].path, sizeof low); CharLowerA(low);
    return strstr(low, name) != NULL;
}

void hdsprites_note_blit(Surf *d, RECT dr, Surf *s, RECT sr, int keyed, DWORD lo, DWORD hi)
{
    if (!g_cfg.hd_sprites || !d || !s || s == d) return;
    if (!d->is_primary) hdsprites_dirty(d);              /* d got new content */
    /* d was filled FROM THE SCREEN (the game saving what is under the mouse cursor): never one of the game's
     * pictures. The 16x16 save surface over a flat green menu matched the 16x16 HUD tile Hud_018_d10 (>= 95 % of
     * the samples), and putting it back painted that tile's HD art into the menu where the pointer had been
     * (2026-11-19, user: "weird rendering issue with text on menus"). */
    if (s->is_primary && !d->is_primary) { d->hd_state = 1; d->hd_id = -1; }
    int to_screen = d->is_primary;
    int to_back = !d->is_primary && d->w == g_cfg.width && d->h == g_cfg.height;   /* the game picture being built */
    if (!to_screen && !to_back) return;
    if (dr.right - dr.left != sr.right - sr.left || dr.bottom - dr.top != sr.bottom - sr.top) return;
    int id = surface_id(s);
    if (dr.right - dr.left >= 600 && dr.bottom - dr.top >= 400) g_last_big = id;   /* a whole screen picture (-1 = unknown) */
    if (id < 0) return;
    Op op = { dr, sr, s, id, keyed, lo, hi, 64 };
    if (to_screen) push(g_screen_ops, &g_screen_n, &op);
    else push(g_back_ops, &g_back_n, &op);
}

/* surface.c: the back buffer became the screen (Flip / windowed frame blit). Caller holds the
 * present lock. */
void hdsprites_frame_shown(Surf *frame)
{
    memcpy(g_screen_ops, g_back_ops, sizeof *g_back_ops * g_back_n);
    g_screen_n = g_back_n;
}

/* ---------------------------------------------------------------- at present time */

static BYTE *g_idmap;
static int g_idmap_cap;
static HdOp g_out[255];

/* Builds the id map for the view of the screen (bits/pitch = the screen snapshot). Returns the
 * number of HD ops (0 = none); *ops / *idmap are valid until the next call. */
int hdsprites_prepare(const BYTE *bits, int pitch, RECT view, const HdOp **ops, const BYTE **idmap)
{
    if (!g_cfg.hd_sprites || !g_screen_n) return 0;
    if (g_cfg.log >= 2) { static DWORD t; if (GetTickCount() - t > 2000) { t = GetTickCount(); mlog("hdsprites: %d notes on the screen, view %ld,%ld-%ld,%ld", g_screen_n, view.left, view.top, view.right, view.bottom); } }
    int vw = view.right - view.left, vh = view.bottom - view.top;
    if ((size_t)vw * vh > (size_t)g_idmap_cap) {
        free(g_idmap);
        g_idmap = malloc((size_t)vw * vh);
        g_idmap_cap = g_idmap ? vw * vh : 0;
        if (!g_idmap) return 0;
    }
    memset(g_idmap, 0, (size_t)vw * vh);
    int n = 0, wide_menu = hdart_active();
    for (int k = 0; k < g_screen_n && n < 127; k++) {
        Op *o = &g_screen_ops[k];
        Entry *e = &g_e[o->id];
        if (!e->hd || !o->src || !o->src->bits) continue;
        if (wide_menu && e->w == 640 && e->h == 480) continue;      /* the widescreen picture shows (hdart.c) */
        if (o->sr.left < 0 || o->sr.top < 0 || o->sr.right > o->src->w || o->sr.bottom > o->src->h) continue;
        RECT c;
        if (!IntersectRect(&c, &o->dr, &view)) continue;
        int hits = 0, own = 0, id = n + 1;
        enum { MAX_UNDO = 16384 };
        static BYTE *undo_at[MAX_UNDO]; static BYTE undo_was[MAX_UNDO];
        int n_undo = 0;
        /* the colours the picture itself uses (its source rect, key excluded): a screen pixel in one of them, next
         * to the picture's own pixel, is the picture (the enlarged HUD copies map pixels a little differently -
         * a one-pixel shift counted as "drawn over" and left speckles of the original, 2026-11-18); text and frames
         * drawn on top use other colours and stay */
        BYTE uses[256] = { 0 };
        for (int y = o->sr.top; y < o->sr.bottom; y++) {
            const BYTE *r = o->src->bits + (size_t)y * o->src->pitch;
            for (int x = o->sr.left; x < o->sr.right; x++) uses[r[x]] = 1;
        }
        if (o->src->has_ck) for (int i = o->src->ck_lo; i <= (int)o->src->ck_hi && i < 256; i++) uses[i] = 0;
        for (int y = c.top; y < c.bottom; y++) {
            const BYTE *fr = bits + (size_t)y * pitch;
            int q = o->q > 0 ? o->q : 64;
            int sy = q == 64 ? o->sr.top + (y - o->dr.top) : (int)floor(o->ay + y * 64.0 / q + 1e-6);
            if (sy < o->sr.top || sy >= o->sr.bottom) continue;
            const BYTE *srow = o->src->bits + (size_t)sy * o->src->pitch;
            BYTE *mp = g_idmap + (size_t)(y - view.top) * vw - view.left;
            for (int x = c.left; x < c.right; x++) {
                int sx = q == 64 ? o->sr.left + (x - o->dr.left) : (int)floor(o->ax + x * 64.0 / q + 1e-6);
                if (sx < o->sr.left || sx >= o->sr.right) continue;
                BYTE v = srow[sx];
                int is_key = (o->keyed && v >= o->lo && v <= o->hi) || (o->src->has_ck && v >= o->src->ck_lo && v <= o->src->ck_hi);
                if (is_key) {
                    /* a key-colour pixel is the picture's where the HD art has ink in it (Codex's soft edges and
                     * shapes a little past the old outline, letter edges of the logo): any of the texels it covers
                     * with alpha; ps_sprite blends by that alpha, so see-through parts keep what is under. (The
                     * first try made black boxes: the loader forced every HD pixel opaque - fixed, 2026-11-18.) */
                    int x0 = (sx - 0) * e->hw / e->w, x1 = (sx + 1) * e->hw / e->w, y0 = sy * e->hh / e->h, y1 = (sy + 1) * e->hh / e->h, ink = 0;
                    for (int ty = y0; ty < y1 && ty < e->hh && !ink; ty++)
                        for (int tx = x0; tx < x1 && tx < e->hw; tx++)
                            if (is_hd_ink(e->hd[(size_t)ty * e->hw + tx])) { ink = 1; break; }   /* not the art's cyan background (BMPs are opaque everywhere) */
                    /* not for whole-screen pictures (menu backgrounds): their dark key-colour areas claimed
                     * whatever was drawn on top without an HD picture of its own - black boxes over the OFF
                     * buttons of Mod Settings on the main menu (2026-11-18) */
                    if (ink && !(e->w >= 640 && e->h >= 480) && n_undo < MAX_UNDO) {
                        undo_at[n_undo] = &mp[x]; undo_was[n_undo++] = mp[x];   /* taken back if the picture is not on screen (below) */
                        mp[x] = (BYTE)id;
                    }
                    continue;
                }
                own++;
                if (fr[x] == v || (q != 64 && uses[fr[x]]))   /* the colour rule only for enlarged HUD copies (one-pixel shifts); at 1:1 it covered menu button labels */ { mp[x] = (BYTE)(id | 0x80); hits++; }
            }
        }
        if (g_cfg.log >= 2) {
            static DWORD last; static int cnt;
            if (GetTickCount() - last > 2000) { last = GetTickCount(); cnt = 0; }
            if (cnt++ < 8) mlog("hdsprites: op %d %s at %ld,%ld %ldx%ld: %d matching pixels", k, e->path, o->dr.left, o->dr.top,
                                o->dr.right - o->dr.left, o->dr.bottom - o->dr.top, hits);
        }
        if (!hits) {
            /* The picture is not on the screen any more (a stale note: every place the mouse cursor was drawn
             * keeps one). Its key-pixel claims above were made with the id the NEXT picture gets, so the next
             * picture's HD art was painted there - the menu button over its own label in the box where the
             * pointer had been ("Qu'it", user 2026-11-19: "weird rendering issue with text on menus"). */
            while (n_undo > 0) { n_undo--; *undo_at[n_undo] = undo_was[n_undo]; }
            continue;
        }
        /* Codex drew past the old outline (soft edges, a slightly bigger shape): the HD picture may also cover its
         * transparent surround up to EDGE_GROW px from its marked pixels, inside its rectangle; the HD alpha blends
         * it over what is there (ps_sprite). Only unmarked pixels are taken (2026-11-18, user). */
        if (EDGE_GROW > 0) {
            int cw = c.right - c.left, ch = c.bottom - c.top;
            static BYTE *tmp; static int tcap;
            if (cw * ch > tcap) { free(tmp); tmp = malloc((size_t)cw * ch); tcap = tmp ? cw * ch : 0; }
            if (tmp) {
                for (int y = 0; y < ch; y++) {
                    const BYTE *mp = g_idmap + (size_t)(c.top + y - view.top) * vw + (c.left - view.left);
                    for (int x = 0; x < cw; x++) tmp[y * cw + x] = (mp[x] & 127) == id;
                }
                for (int y = 0; y < ch; y++) {
                    BYTE *mp = g_idmap + (size_t)(c.top + y - view.top) * vw + (c.left - view.left);
                    for (int x = 0; x < cw; x++) {
                        if (mp[x]) continue;
                        int close_by = 0;
                        for (int dy = -EDGE_GROW; dy <= EDGE_GROW && !close_by; dy++)
                            for (int dx = -EDGE_GROW; dx <= EDGE_GROW; dx++) {
                                int yy = y + dy, xx = x + dx;
                                if (yy >= 0 && yy < ch && xx >= 0 && xx < cw && tmp[yy * cw + xx]) { close_by = 1; break; }
                            }
                        if (close_by) mp[x] = (BYTE)id;
                    }
                }
            }
        }
        HdOp *h = &g_out[n++];
        h->x = o->dr.left - view.left; h->y = o->dr.top - view.top;
        h->w = o->dr.right - o->dr.left; h->h = o->dr.bottom - o->dr.top;
        if (o->q > 0 && o->q != 64) {                      /* the copy's own mapping, continuous */
            h->u0 = (float)((o->ax + o->dr.left * 64.0 / o->q) / e->w); h->v0 = (float)((o->ay + o->dr.top * 64.0 / o->q) / e->h);
            h->u1 = (float)((o->ax + o->dr.right * 64.0 / o->q) / e->w); h->v1 = (float)((o->ay + o->dr.bottom * 64.0 / o->q) / e->h);
        } else {
            h->u0 = (float)o->sr.left / e->w; h->v0 = (float)o->sr.top / e->h;
            h->u1 = (float)o->sr.right / e->w; h->v1 = (float)o->sr.bottom / e->h;
        }
        h->id = id;
        h->pix = e->hd; h->pw = e->hw; h->ph = e->hh; h->serial = e->serial;
    }
    *ops = g_out;
    *idmap = g_idmap;
    return n;
}

/* surface.c: a surface is being freed - drop every note that reads from it. Caller holds the
 * present lock. */
static void forget_in(Op *ops, int *n, Surf *s)
{
    for (int i = 0; i < *n; i++)
        if (ops[i].src == s) { memmove(&ops[i], &ops[i + 1], sizeof *ops * (*n - i - 1)); (*n)--; i--; }
}

void hdsprites_forget(Surf *s)
{
    forget_in(g_screen_ops, &g_screen_n, s);
    forget_in(g_back_ops, &g_back_n, s);
}

/* layout.c enlarged the HUD area src (1x, back buffer) to start at (dx, dy), x q/64: the notes of pictures drawn
 * there move along (else the HD pictures were shown at their 1x places over the enlarged copy - ghosts). A note
 * fully inside src is replaced, one partly inside is split (its outside part stays). Notes already moved
 * (q != 64) are left alone. */
void hdsprites_scale_back(RECT src, int dx, int dy, int q)
{
    if (!g_cfg.hd_sprites || q <= 0) return;
    int n0 = g_back_n;
    for (int i = 0; i < n0 && g_back_n < MAX_OPS; i++) {
        Op *o = &g_back_ops[i];
        RECT c;
        if (o->q != 64 || !IntersectRect(&c, &o->dr, &src)) continue;
        Op m = *o;
        m.sr.left = o->sr.left + (c.left - o->dr.left); m.sr.top = o->sr.top + (c.top - o->dr.top);
        m.sr.right = m.sr.left + (c.right - c.left); m.sr.bottom = m.sr.top + (c.bottom - c.top);
        /* the copy shows 1x pixel src.left + floor((X - dx) * 64 / q) at screen X: the screen range of c, exactly */
        m.dr.left = dx + ((c.left - src.left) * q + 63) / 64; m.dr.top = dy + ((c.top - src.top) * q + 63) / 64;
        m.dr.right = dx + ((c.right - src.left) * q + 63) / 64; m.dr.bottom = dy + ((c.bottom - src.top) * q + 63) / 64;
        m.q = q;
        m.ax = (double)(o->sr.left - o->dr.left + src.left) - dx * 64.0 / q;
        m.ay = (double)(o->sr.top - o->dr.top + src.top) - dy * 64.0 / q;
        if (EqualRect(&c, &o->dr)) *o = m;                    /* fully inside: moved */
        else g_back_ops[g_back_n++] = m;                      /* partly: the inside part, added */
    }
}

/* layout.c, when the game starts drawing a new battlefield frame: the back buffer is redrawn, its old notes
 * are stale (they were never cleared: the list sat full of old HUD notes - at 640x480 places from before the
 * layout - whose pixels sometimes still matched: ghost HD pictures over the HUD, 2026-11-17) */
void hdsprites_new_frame(void) { g_back_n = 0; }

/* ---------------------------------------------------------------- recall pictures: one radio, not two
 * The pop-up radios / signs (Screens\Briefs, the "recall" picture) in hd\ are a different shape from the
 * originals (the user's 2x art: the radio narrower, at another angle). ps_sprite draws the HD picture only where
 * the game pixel is the original's, and where the HD art is background (cyan / see-through) over the original's
 * ink it shows the game pixel - the OLD radio around the new one (user 2026-11-19: "the new radio image is on
 * top of the old radio image"). So for these pictures the game's own blit skips every source pixel the HD art
 * has no ink over: the battlefield stays there (and its upscaled version, hdworld), the HD radio goes on top.
 * Returns 1 and fills *tmp (a copy of s whose bits are a masked copy) when the blit should read *tmp. */
static int is_hd_ink(uint32_t p)
{
    int a = p >> 24, r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
    if (a <= 40) return 0;
    int mn = g < b ? g : b;
    return !(mn - r > 64 && g > 90 && b > 90);          /* the art's cyan background (same test as ps_sprite) */
}

int hdsprites_masked_source(Surf *s, DWORD key, Surf *tmp)
{
    static BYTE *buf; static size_t cap;
    static const char *mask_path; static BYTE *mask; static int mw, mh;
    if (!g_cfg.hd_sprites || !s || !s->bits || s->is_primary) return 0;
    int id = surface_id(s);
    if (id < 0) return 0;
    Entry *e = &g_e[id];
    if (!e->hd || e->w != s->w || e->h != s->h) return 0;
    char low[MAX_PATH]; lstrcpynA(low, e->path, sizeof low); CharLowerA(low);
    if (!strstr(low, "\\briefs\\") || e->w < 200 || e->h < 200) return 0;
    if (mask_path != e->path || mw != e->w || mh != e->h) {      /* per original pixel: 1 = no HD ink over it */
        free(mask); mask = malloc((size_t)e->w * e->h); mask_path = NULL;
        if (!mask) return 0;
        for (int y = 0; y < e->h; y++)
            for (int x = 0; x < e->w; x++) {
                int x0 = x * e->hw / e->w, x1 = (x + 1) * e->hw / e->w, y0 = y * e->hh / e->h, y1 = (y + 1) * e->hh / e->h, ink = 0;
                for (int ty = y0; ty < y1 && !ink; ty++)
                    for (int tx = x0; tx < x1; tx++)
                        if (is_hd_ink(e->hd[(size_t)ty * e->hw + tx])) { ink = 1; break; }
                mask[(size_t)y * e->w + x] = !ink;
            }
        mask_path = e->path; mw = e->w; mh = e->h;
        { int holes = 0; for (int i = 0; i < e->w * e->h; i++) holes += mask[i]; mlog("hdsprites: recall mask for %s: %d of %d pixels have no HD ink (key %lu)", e->path, holes, e->w * e->h, key); }
    }
    size_t need = (size_t)s->w * s->h;
    if (need > cap) { free(buf); buf = malloc(need); cap = buf ? need : 0; if (!buf) return 0; }
    for (int y = 0; y < s->h; y++) {
        const BYTE *sp = s->bits + (size_t)y * s->pitch, *mp = mask + (size_t)y * s->w;
        BYTE *dp = buf + (size_t)y * s->w;
        for (int x = 0; x < s->w; x++) dp[x] = mp[x] ? (BYTE)key : sp[x];
    }
    *tmp = *s;
    tmp->bits = buf; tmp->pitch = s->w;
    tmp->hd_ref = NULL; tmp->hd_tiles = NULL;
    return 1;
}
