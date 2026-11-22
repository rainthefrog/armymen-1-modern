/* ============================================================================================
 * hdworld.c - upscaled battlefield: units, vehicles, explosions, objects and terrain tiles
 *
 * <game>\hd\world.pak (tooling/scripts/pack_hd.py) holds 2x versions of every sprite and tile,
 * keyed by an FNV-1a hash of the sprite's RLE data in memory (or of a tile's 256 pixel bytes).
 *
 *  1. The game draws sprites with four software blitters into a locked surface (the map surface or
 *     the back buffer): FUN_004041b0 (WRLE), FUN_004040c0 (DRLE), FUN_004042a0 (WRLE through an army
 *     remap table), FUN_00404390 (shadow). __fastcall ecx = x, edx = y; stack: data, x0, y0, x1, y1
 *     [, remap] - sprite part [x0,x1) x [y0,y1) lands at (x, y). Wrapped: after drawing, every opaque
 *     pixel gets a REFERENCE in the surface's ref buffer: pack entry, pixel in the sprite, the byte it
 *     wrote (to check later), army and shadow flags.
 *  2. Terrain tiles are BltFast'd from the tile atlas surface; surface.c reports every copy
 *     (hdworld_note_copy): tile copies write tile references, copies between surfaces with ref
 *     buffers (map surface scrolling, map -> back buffer) carry them along, other copies clear them.
 *  3. Flip / the windowed frame blit: the back buffer's refs are the frame's (hdworld_frame_shown).
 *  4. The display thread (hdworld_prepare) keeps only refs whose byte still equals the shown pixel
 *     (anything drawn over them since - HUD, text, fog - breaks the match), and turns them into atlas
 *     positions; gpu.c ps_world draws the 2x pixels from the atlas over the game picture. Images are
 *     decoded on a loader thread when first needed and uploaded into the atlas (4 x 4096^2).
 * Army colours: units are packed in green; ps_world maps the HD pixel's brightness onto the army's
 * colour ramp. [display] hd_world = 1
 * ============================================================================================ */
#define COBJMACROS
#include "common.h"
#include <wincodec.h>

#define LOCKED_BITS   (*(BYTE **)0x4e2444)
#define MAP_SURF      (*(Surf **)0x4e2e5c)
#define BACK_SURF     (*(Surf **)0x4e2e4c)
#define TERRAIN_NAME  ((const char *)0x4e2f59)
#define MAP_NAME      ((const char *)0x4e2f38)

typedef struct { uint16_t e, sx, sy; BYTE expect, fl; } Ref;          /* e = entry + 1 */
enum { FL_SHADOW = 1, FL_RECOLOUR = 2 };                                /* army in bits 2..3 */

#pragma pack(push, 1)
typedef struct { uint32_t key; uint16_t w, h; BYTE kind, terrain, flags, pad; uint32_t blob; } Entry;
typedef struct { uint32_t off, size; uint16_t w, h; } Blob;
#pragma pack(pop)

static Entry *g_e; static int g_ne;
static Blob *g_b; static int g_nb;
static HANDLE g_pak = INVALID_HANDLE_VALUE;
static volatile LONG g_ready;
static int g_terrain = -1;
static char g_map[33];

/* per blob: 0 none, 1 requested, 2 decoded, 3 resident, 4 failed */
static volatile LONG *g_state;
static volatile LONG g_ent_dirty = 1;      /* an image became resident: the GPU entry table is stale */
/* low objects (rocks, bushes, fallen trees - roofs.c tells while the game draws such a cell): their images
 * sort BEHIND standing figures in the GPU painter depth (user: Sarge over rocks, behind trees) */
#define EF_LOW 0x80                         /* Entry.flags: drawn as a low object (pak flags use 1, 2) */
#define EF_TOP 0x40                         /* Entry.flags: a vehicle turret - above the hull at the same row */
static volatile int g_cell_low;
void hdworld_cell_low(int low) { g_cell_low = low; }
static volatile int g_cell_top;
void hdworld_cell_top(int top) { g_cell_top = top; }
/* Painter depth = the game's own sort key (2026-11-19, user: Sarge in front of a building drawn partly under
 * its edge). The game sorts by ANIM world_pos.y (game binary compare, ties by order), and wrect.top =
 * world_pos.y - hotspot.y - world_z, so the key inside the image is hotspot.y + world_z = pos.y - wrect.top
 * (ANIMCELL +0x12 pos y, +0x20 wrect top). Before, the GPU used the image's bottom row: a building's lowest corner
 * lies below the feet of a soldier standing in front of its wall, so the building's edge was drawn over him.
 * Per entry (image), the last value seen; 0 = unknown (then the bottom row). */
static volatile int g_cell_sort;
static uint16_t *g_sorty;
void hdworld_cell_sort(int y) { g_cell_sort = y; }
static uint32_t **g_pix;                    /* decoded RGBA (state 2) */
static uint32_t *g_pos;                     /* resident: x | y << 12 | slice << 24 */
static HANDLE g_wake;

/* ---------------------------------------------------------------- pack */

static DWORD WINAPI loader(void *arg);

static DWORD WINAPI open_pack(void *arg)
{
    char path[MAX_PATH];
    hdart_game_path(path, MAX_PATH, "hd\\world.pak");
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { InterlockedExchange(&g_ready, -1); return 0; }
    DWORD got;
    struct { char magic[4]; uint32_t ver, ne, nb; } h;
    if (!ReadFile(f, &h, sizeof h, &got, NULL) || got != sizeof h || memcmp(h.magic, "AMHD", 4) || h.ver != 1) {
        CloseHandle(f); InterlockedExchange(&g_ready, -1); mlog("hdworld: world.pak not readable"); return 0;
    }
    free(g_sorty); g_sorty = calloc(h.ne + 1, sizeof *g_sorty);
    g_e = malloc(sizeof *g_e * h.ne); g_b = malloc(sizeof *g_b * h.nb);
    g_state = calloc(h.nb, sizeof *g_state); g_pix = calloc(h.nb, sizeof *g_pix); g_pos = calloc(h.nb, sizeof *g_pos);
    if (!g_e || !g_b || !g_state || !g_pix || !g_pos
        || !ReadFile(f, g_e, sizeof *g_e * h.ne, &got, NULL) || !ReadFile(f, g_b, sizeof *g_b * h.nb, &got, NULL)) {
        CloseHandle(f); InterlockedExchange(&g_ready, -1); return 0;
    }
    g_ne = h.ne; g_nb = h.nb; g_pak = f;
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, loader, NULL, 0, NULL);
    if (t) CloseHandle(t);
    mlog("hdworld: world.pak: %d sprites/tiles, %d images", g_ne, g_nb);
    InterlockedExchange(&g_ready, 1);
    return 0;
}

static CRITICAL_SECTION g_dirty_cs;     /* map dirty list: game thread adds, display thread (take) empties */
static CRITICAL_SECTION g_atlas_cs; static volatile LONG g_atlas_cs_ready;   /* atlas packing: display thread uploads, game thread restarts it */
static volatile LONG g_dirty_cs_ready;
static void hdworld_start_pack(void);
void hdworld_start(void)
{
    if (!g_dirty_cs_ready) { InitializeCriticalSection(&g_dirty_cs); g_dirty_cs_ready = 1; }
    if (!g_atlas_cs_ready) { InitializeCriticalSection(&g_atlas_cs); g_atlas_cs_ready = 1; }
    hdworld_start_pack();
}
static void hdworld_start_pack(void)
{
    HANDLE t = CreateThread(NULL, 0, open_pack, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static int terrain_now(void)
{
    char t[16];
    lstrcpynA(t, TERRAIN_NAME, sizeof t);
    CharLowerA(t);
    if (!strncmp(t, "desert", 6)) return 2;
    if (!strncmp(t, "bayou", 5)) return 1;
    return 0;                                   /* alpine, bootcamp */
}
/* Load the image of entry i the first time it is used (the loader thread decodes it, hdworld_upload
 * puts it into the atlas; images stay there). Lookups happen once per sprite / tile, not per pixel. */
static void request(int i)
{
    int b = g_e[i].blob;
    if (g_state[b] == 0 && InterlockedCompareExchange(&g_state[b], 1, 0) == 0) SetEvent(g_wake);
}


/* The pack entry (index) for a key, or -1. Prefers the current terrain. */
static int lookup(uint32_t key, int w, int h, int kind)
{
    int lo = 0, hi = g_ne - 1, best = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_e[m].key < key) lo = m + 1; else hi = m - 1; }
    for (int i = lo; i < g_ne && g_e[i].key == key; i++) {
        Entry *e = &g_e[i];
        if (e->w != w || e->h != h || e->kind != kind) continue;
        if (e->terrain == g_terrain || e->terrain == 255) { request(i); return i; }
        if (best < 0) best = i;
    }
    if (best >= 0) request(best);
    return best;
}

/* ---------------------------------------------------------------- sprite keys */

static uint32_t fnv(const BYTE *p, size_t n)
{
    uint32_t h = 0x811c9dc5;
    while (n--) { h ^= *p++; h *= 0x01000193; }
    return h;
}

/* End of an RLE blob (rows of [skip, run, pixels]); 0 if it does not look like one. */
static size_t rle_size(const BYTE *d, int dword, int pixels)
{
    int w = *(const uint16_t *)d, h = *(const uint16_t *)(d + 2);
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return 0;
    size_t end = 4 + (size_t)h * (dword ? 4 : 2);
    for (int y = 0; y < h; y++) {
        size_t p = dword ? ((const uint32_t *)(d + 4))[y] : ((const uint16_t *)(d + 4))[y];
        if (p > 4u << 20) return 0;
        int x = 0;
        for (int guard = 0; guard < 4096; guard++) {
            x += d[p++];
            if (x >= w) break;
            int n = d[p++];
            if (pixels) p += n;
            x += n;
            if (x >= w) break;
        }
        if (p > end) end = p;
    }
    return end;
}

#define CACHE 8192
/* data pointer -> entry, checked by a signature of the first bytes (size + row table): the game frees sprite
 * data and loads other sprites at the same address (a damaged tree's frame where a watchtower part was:
 * the tree showed the tower's planks - user 2026-11-17, "damage mismatch"). A changed signature = look up again. */
typedef struct { const BYTE *data; uint32_t sig; int entry; } KeyCache;
static KeyCache g_cache[CACHE];

static uint32_t sprite_sig(const BYTE *data) { return fnv(data, 32); }

static int sprite_entry(const BYTE *data, int dword)
{
    unsigned slot = ((uintptr_t)data >> 2) * 2654435761u % CACHE;
    uint32_t sig = sprite_sig(data);
    KeyCache *reuse = NULL;
    for (int i = 0; i < 8; i++) {
        KeyCache *c = &g_cache[(slot + i) % CACHE];
        if (c->data == data) { if (c->sig == sig) { if (c->entry >= 0) request(c->entry); return c->entry; } reuse = c; break; }   /* same place, new sprite */
        if (!c->data) break;
    }
    size_t n = rle_size(data, dword, 1);
    int e = n ? lookup(fnv(data, n), *(const uint16_t *)data, *(const uint16_t *)(data + 2), 0) : -1;
    if (g_cfg.log >= 2) {
        static int logged;
        if (logged++ < 40 || reuse) mlog("hdworld: sprite %p %dx%d key %08x -> %s%s", data, *(const uint16_t *)data,
                                         *(const uint16_t *)(data + 2), n ? fnv(data, n) : 0, e >= 0 ? "upscaled" : "not in pack",
                                         reuse ? " (address reused by another sprite)" : "");
    }
    if (reuse) { reuse->sig = sig; reuse->entry = e; return e; }
    for (int i = 0; i < 8; i++) {
        KeyCache *c = &g_cache[(slot + i) % CACHE];
        if (!c->data || i == 7) { c->data = data; c->sig = sig; c->entry = e; break; }
    }
    return e;
}

/* ---------------------------------------------------------------- reference buffers */

/* The MAP surface's references use a ROLLING layout (2026-11-17). The game scrolls the view part of its map
 * surface (full width, rows 0..view height; the map surface is a few rows taller) by copying it onto itself
 * every walking frame; moving our 8-byte references along cost ~2 ms per frame at 2048x1080. Now such a shift
 * only re-points: each row is reached through its own pointer (rows[y]); a vertical shift rotates the pointers
 * of the scrolled band, a horizontal one moves each pointer inside its row's storage, which has ROLL_PAD spare
 * references on each side (past the spare: that row is re-centred once, a real move).
 * The strips a shift exposes hold stale references, as before - the game redraws them.
 * Every access goes through ref_row(); other surfaces stay plain (base + y * w). */
#define ROLL_PAD 512
typedef struct { Surf *s; Ref *mem; Ref **rows, **home, **tmp; int w, h, stride; } Roll;   /* home: row centred */
static Roll g_roll;
static LONG g_roll_hits, g_roll_misses, g_roll_centres;           /* log */
static Ref *ref_row(Surf *s, Ref *base, int y)
{
    if (s == g_roll.s && base == g_roll.mem) return g_roll.rows[y];
    return base + (size_t)y * s->w;
}
static void roll_free(void)
{
    free(g_roll.rows); free(g_roll.home); free(g_roll.tmp);
    memset(&g_roll, 0, sizeof g_roll);
}
/* surface.c, before a surface's references are freed */
void hdworld_forget(Surf *s)
{
    if (s && s == g_roll.s) roll_free();                          /* mem itself is s->hd_ref (freed there) */
}
static Ref *refs(Surf *s, int create)
{
    if (!s) return NULL;
    if (!s->hd_ref && create) {
        if (s == MAP_SURF) {
            int stride = s->w + 2 * ROLL_PAD;
            Ref *mem = calloc((size_t)stride * s->h, sizeof(Ref));
            Ref **rows = malloc(s->h * sizeof(Ref *)), **home = malloc(s->h * sizeof(Ref *)), **tmp = malloc(s->h * sizeof(Ref *));
            if (mem && rows && home && tmp) {
                if (g_roll.s) roll_free();
                for (int y = 0; y < s->h; y++) rows[y] = home[y] = mem + (size_t)y * stride + ROLL_PAD;
                g_roll = (Roll){ s, mem, rows, home, tmp, s->w, s->h, stride };
                s->hd_ref = mem;
            } else { free(mem); free(rows); free(home); free(tmp); }
        } else s->hd_ref = calloc((size_t)s->w * s->h, sizeof(Ref));
    }
    return s->hd_ref;
}
/* clear all of a surface's references */
static void refs_clear(Surf *s)
{
    if (!s || !s->hd_ref) return;
    if (s == g_roll.s && s->hd_ref == g_roll.mem) memset(g_roll.mem, 0, (size_t)g_roll.stride * g_roll.h * sizeof(Ref));
    else memset(s->hd_ref, 0, (size_t)s->w * s->h * sizeof(Ref));
}
/* A copy of the rolled surface onto itself that shifts the band of rows 0..bh by (dx, dy), full width
 * (new(x,y) = old(x+dx,y+dy)): done by re-pointing. Returns 0 if the copy is not such a shift. */
static int roll_shift(Surf *s, RECT dr, RECT sr)
{
    if (s != g_roll.s || s->hd_ref != g_roll.mem || s->w != g_roll.w || s->h != g_roll.h) return 0;
    int dx = sr.left - dr.left, dy = sr.top - dr.top, w = g_roll.w;
    int bh = dr.bottom > sr.bottom ? dr.bottom : sr.bottom;        /* the scrolled band */
    if (bh <= 0 || bh > g_roll.h || (dx > 0 ? dx : -dx) >= w || (dy > 0 ? dy : -dy) >= bh) return 0;
    RECT want = { dx > 0 ? 0 : -dx, dy > 0 ? 0 : -dy, dx > 0 ? w - dx : w, dy > 0 ? bh - dy : bh };
    if (!EqualRect(&dr, &want)) return 0;
    if (dy) {                                                       /* rotate the band's rows (with their homes) */
        Ref **arr[2] = { g_roll.rows, g_roll.home };
        for (int a = 0; a < 2; a++) {
            for (int y = 0; y < bh; y++) g_roll.tmp[y] = arr[a][((y + dy) % bh + bh) % bh];
            memcpy(arr[a], g_roll.tmp, bh * sizeof(Ref *));
        }
    }
    if (dx) {
        int n = w - (dx > 0 ? dx : -dx), centred = 0;              /* references that carry over */
        for (int y = 0; y < bh; y++) {
            Ref *r = g_roll.rows[y], *h0 = g_roll.home[y];
            ptrdiff_t off = (r - h0) + dx;
            if (off >= -ROLL_PAD && off <= ROLL_PAD) { g_roll.rows[y] = r + dx; continue; }
            if (dx > 0) memmove(h0, r + dx, n * sizeof(Ref));       /* past the spare: re-centre this row */
            else memmove(h0 - dx, r, n * sizeof(Ref));
            g_roll.rows[y] = h0;
            centred = 1;
        }
        g_roll_centres += centred;
    }
    return 1;
}

static Surf *locked_target(void)
{
    BYTE *b = LOCKED_BITS;
    Surf *m = MAP_SURF, *k = BACK_SURF;
    if (m && b == m->bits) return m;
    if (k && b == k->bits) return k;
    return NULL;
}

/* ---------------------------------------------------------------- map links, dirty rectangles, overrides
 * The battlefield on the screen (back buffer) is a copy of the map surface, made every frame (17 MB of
 * references at 2048x1080 - most of the game thread's upscale cost). Instead of copying references:
 *   - the map surface's references live on the GPU (gpu.c g_maprefs); only rectangles the game changed
 *     (sprites, tiles, fills) are uploaded: g_dirty, collected on the game thread, copied out at take;
 *   - a map -> screen copy is recorded as a LINK (screen rect + offset into the map); ps_resolve reads the
 *     map references through it;
 *   - anything drawn on the screen afterwards (HUD, signs, text boxes, enlarged corners) marks g_over
 *     (1 byte per pixel); there the screen's own references (or none) are used.
 * Links and the override mask belong to a frame and are handed over at hdworld_frame_shown. */
#define MAX_LINKS 4
#define MAX_DIRTY 64
typedef struct { RECT r; int ox, oy; } Link;
static Link g_link_build[MAX_LINKS]; static int g_nlink_build;        /* game thread, this frame */
static RECT g_dirty[MAX_DIRTY + 1]; static int g_ndirty, g_dirty_all; /* map surface, since the last take */
#define MERGE_WASTE 4096                                               /* px a merge may add (unchanged ones uploaded) */
static LONGLONG rect_area(RECT r) { return (LONGLONG)(r.right - r.left) * (r.bottom - r.top); }
static Surf *g_dirty_surf;                                             /* the map surface they belong to */
static BYTE *g_obuf[2]; static int g_oi; static RECT g_obox[2];        /* override masks (back-buffer size) */
static int g_ow, g_oh;

static void dirty_map_locked(Surf *m, RECT r);
static void dirty_map(Surf *m, RECT r)
{
    if (!g_dirty_cs_ready) return;
    EnterCriticalSection(&g_dirty_cs);
    dirty_map_locked(m, r);
    LeaveCriticalSection(&g_dirty_cs);
}
static void dirty_map_locked(Surf *m, RECT r)
{
    if (m != g_dirty_surf) { g_dirty_surf = m; g_dirty_all = 1; g_ndirty = 0; return; }
    if (g_dirty_all) return;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > m->w) r.right = m->w;
    if (r.bottom > m->h) r.bottom = m->h;
    if (r.right <= r.left || r.bottom <= r.top) return;
    /* Merge into an existing rectangle only where the union wastes little (2026-11-17: merging everything that
     * touched turned a scroll's top strip + left strip into the whole map). Full list: the cheapest pair. */
    for (;;) {
        int best = -1; LONGLONG waste = 0;
        for (int i = 0; i < g_ndirty; i++) {
            RECT u; UnionRect(&u, &g_dirty[i], &r);
            LONGLONG w = rect_area(u) - rect_area(g_dirty[i]) - rect_area(r);
            if (w <= MERGE_WASTE && (best < 0 || w < waste)) { best = i; waste = w; }
        }
        if (best < 0) break;
        RECT u; UnionRect(&u, &g_dirty[best], &r);                     /* grown: may now swallow others too */
        g_dirty[best] = g_dirty[--g_ndirty];
        r = u;
    }
    if (g_ndirty < MAX_DIRTY) { g_dirty[g_ndirty++] = r; return; }
    g_dirty[g_ndirty++] = r;                                           /* MAX_DIRTY + 1 slots */
    int bi = 0, bj = 1; LONGLONG bw = -1;
    for (int i = 0; i < g_ndirty; i++) for (int j = i + 1; j < g_ndirty; j++) {
        RECT u; UnionRect(&u, &g_dirty[i], &g_dirty[j]);
        LONGLONG w = rect_area(u) - rect_area(g_dirty[i]) - rect_area(g_dirty[j]);
        if (bw < 0 || w < bw) { bw = w; bi = i; bj = j; }
    }
    UnionRect(&g_dirty[bi], &g_dirty[bi], &g_dirty[bj]);
    g_dirty[bj] = g_dirty[--g_ndirty];
}

/* Map scrolls (2026-11-17). The game scrolls its map surface by copying it onto itself (sr -> dr, same
 * surface). Marking dr changed re-staged and re-uploaded ~2.2 M references (17 MB) every walking frame in
 * fullscreen (trace: map 2211k, wup 4-6 ms, shown 2.7 ms). Now such a copy is recorded as a MOVE: gpu.c
 * repeats it on its copy of the map references (GPU to GPU), and only rectangles really drawn are uploaded.
 * Rule that keeps it exact: moves are applied first, then every changed rectangle is uploaded with its FINAL
 * references. So a rectangle changed before a move must also be marked where the move takes it (its old
 * contents on the GPU are stale and get moved along) - map_move_locked / dirty_before do that.
 * Too many moves: the destination is simply marked changed (the old way). */
#define MAX_MOVES 8
typedef struct { RECT s, d; } Move;
static Move g_move[MAX_MOVES]; static int g_nmove;                  /* since the last commit, in order */
static RECT shift_part(RECT r, const Move *mv)                    /* the part of r inside mv->s, moved */
{
    RECT x;
    if (!IntersectRect(&x, &r, &mv->s)) { SetRectEmpty(&x); return x; }
    OffsetRect(&x, mv->d.left - mv->s.left, mv->d.top - mv->s.top);
    return x;
}
/* r changed BEFORE moves mv[0..n): mark it and wherever those moves carried it */
static void dirty_before(Surf *m, RECT r, const Move *mv, int n)
{
    if (IsRectEmpty(&r)) return;
    for (int i = 0; i < n; i++) {
        RECT x = shift_part(r, &mv[i]);
        if (!IsRectEmpty(&x)) dirty_before(m, x, mv + i + 1, n - i - 1);
    }
    dirty_map_locked(m, r);
}
static void map_move_locked(Surf *m, RECT sr, RECT dr)
{
    if (m != g_dirty_surf) { dirty_map_locked(m, dr); return; }   /* new surface: everything anyway */
    if (g_dirty_all) return;
    RECT b = { 0, 0, m->w, m->h }, t;
    int inside = IntersectRect(&t, &sr, &b) && EqualRect(&t, &sr) && IntersectRect(&t, &dr, &b) && EqualRect(&t, &dr);
    if (!inside || g_nmove >= MAX_MOVES) { dirty_map_locked(m, dr); return; }
    Move mv = { sr, dr };
    RECT old[MAX_DIRTY]; int n = g_ndirty;
    memcpy(old, g_dirty, n * sizeof(RECT));
    for (int i = 0; i < n; i++) { RECT x = shift_part(old[i], &mv); if (!IsRectEmpty(&x)) dirty_map_locked(m, x); }
    if (g_dirty_all) return;
    g_move[g_nmove++] = mv;
}
static void map_move(Surf *m, RECT sr, RECT dr)
{
    if (!g_dirty_cs_ready) return;
    EnterCriticalSection(&g_dirty_cs);
    map_move_locked(m, sr, dr);
    LeaveCriticalSection(&g_dirty_cs);
}

static BYTE *over_buf(Surf *b)
{
    if (!b || b != BACK_SURF) return NULL;
    if (g_ow != b->w || g_oh != b->h) {
        for (int i = 0; i < 2; i++) { free(g_obuf[i]); g_obuf[i] = calloc((size_t)b->w * b->h, 1); SetRectEmpty(&g_obox[i]); }
        g_ow = g_obuf[0] && g_obuf[1] ? b->w : 0; g_oh = g_ow ? b->h : 0;
    }
    return g_ow ? g_obuf[g_oi] : NULL;
}

/* Something was drawn on the screen buffer over r (keyed: only where src is not the colour key). */
static void mark_over(Surf *b, RECT r, const BYTE *src, int spitch, int keyed, DWORD lo, DWORD hi)
{
    BYTE *o = over_buf(b);
    if (!o) return;
    RECT c = { 0, 0, b->w, b->h };
    RECT rr;
    if (!IntersectRect(&rr, &r, &c)) return;
    for (int y = rr.top; y < rr.bottom; y++) {
        BYTE *row = o + (size_t)y * b->w;
        if (!keyed || !src) { memset(row + rr.left, 1, rr.right - rr.left); continue; }
        const BYTE *s = src + (size_t)(y - r.top) * spitch + (rr.left - r.left);
        for (int x = rr.left; x < rr.right; x++, s++) if (!(*s >= lo && *s <= hi)) row[x] = 1;
    }
    UnionRect(&g_obox[g_oi], &g_obox[g_oi], &rr);
}

/* The same for a stretched copy (sr scaled to dr): each screen pixel looks up its source pixel (nearest). */
static void mark_over_scaled(Surf *b, RECT dr, const Surf *s, RECT sr, int keyed, DWORD lo, DWORD hi)
{
    BYTE *o = over_buf(b);
    if (!o) return;
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top, sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    RECT c = { 0, 0, b->w, b->h }, rr;
    if (!IntersectRect(&rr, &dr, &c)) return;
    for (int y = rr.top; y < rr.bottom; y++) {
        BYTE *row = o + (size_t)y * b->w;
        if (!keyed) { memset(row + rr.left, 1, rr.right - rr.left); continue; }
        int sy = sr.top + (int)((LONGLONG)(y - dr.top) * sh / dh);
        if (sy < 0 || sy >= s->h) continue;
        const BYTE *srow = s->bits + (size_t)sy * s->pitch;
        for (int x = rr.left; x < rr.right; x++) {
            int sx = sr.left + (int)((LONGLONG)(x - dr.left) * sw / dw);
            if (sx >= 0 && sx < s->w && !(srow[sx] >= lo && srow[sx] <= hi)) row[x] = 1;
        }
    }
    UnionRect(&g_obox[g_oi], &g_obox[g_oi], &rr);
}

/* Which upscaled image a cell was last drawn with (2026-11-19, for the target outline around the objects on a
 * sector border, objectives.c / gpu.c zone_paint). roofs.c names the cell the game is drawing; note_sprite, which
 * finds the image, files it here. hdworld_cell_atlas then gives where that image's pixel (0,0) lies in the atlas
 * (half-resolution x, y as in the wref format, and the slice): for every screen pixel of that drawn image,
 * (wref xy - screen xy) is the same value, which tells THIS object's pixels from any other sprite's. */
#define CELLMAP 2048
#define CELL_IMAGES 8                       /* a building is drawn as several images through ONE cell (Wine log: the
                                             * same cell, same rectangle, another image and sort row each time) */
typedef struct { const BYTE *cell; int n; int entry[CELL_IMAGES]; short wl[CELL_IMAGES], wt[CELL_IMAGES]; } CellImages;
static CellImages g_cellmap[CELLMAP];
static const BYTE *volatile g_cur_cell;
void hdworld_cell_begin(const BYTE *cell) { g_cur_cell = cell; }
static CellImages *cellmap_find(const BYTE *cell, int make)
{
    unsigned h = (unsigned)((uintptr_t)cell >> 4) * 2654435761u % CELLMAP;
    for (int i = 0; i < 8; i++) {
        CellImages *c = &g_cellmap[(h + i) % CELLMAP];
        if (c->cell == cell) return c;
        if (!c->cell) { if (!make) return NULL; c->cell = cell; c->n = 0; return c; }
    }
    if (!make) return NULL;
    g_cellmap[h].cell = cell; g_cellmap[h].n = 0;       /* full around here: replace */
    return &g_cellmap[h];
}
static void cellmap_put(const BYTE *cell, int e)
{
    CellImages *c = cellmap_find(cell, 1);
    short wl = (short)*(const int *)(cell + 0x1c), wt = (short)*(const int *)(cell + 0x20);   /* ANIMCELL wrect: where it is drawn */
    for (int i = 0; i < c->n; i++) if (c->entry[i] == e) { c->wl[i] = wl; c->wt[i] = wt; return; }
    int k = c->n < CELL_IMAGES ? c->n++ : CELL_IMAGES - 1;
    c->entry[k] = e; c->wl[k] = wl; c->wt[k] = wt;
}

static int army_of(const BYTE *remap)
{
    if (!remap) return 0;
    int v = remap[8];
    if (v >= 224 && v < 240) return 1;          /* tan */
    if (v >= 176 && v < 192) return 2;          /* blue */
    if (v >= 192 && v < 208) return 3;          /* grey */
    return 0;                                   /* green */
}

/* After a sprite (or shadow) was drawn: write its references. */
static void note_sprite(int x, int y, const BYTE *data, int x0, int y0, int x1, int y1, int dword, int shadow, const BYTE *remap)
{
    if (g_ready != 1 || !g_cfg.hd_world) return;
    Surf *s = locked_target();
    Ref *r = refs(s, !shadow);
    if (!r) return;
    int e = shadow ? -1 : sprite_entry(data, dword);
    if (e >= 0 && g_cur_cell) cellmap_put(g_cur_cell, e);
    if (e >= 0 && g_cell_low && !(g_e[e].flags & EF_LOW)) { g_e[e].flags |= EF_LOW; InterlockedExchange(&g_ent_dirty, 1); }
    if (e >= 0 && g_cell_top && !(g_e[e].flags & EF_TOP)) { g_e[e].flags |= EF_TOP; InterlockedExchange(&g_ent_dirty, 1); }
    if (e >= 0 && g_sorty && g_cell_sort > 0 && g_cell_sort < 0x7ff && g_sorty[e] != g_cell_sort) { g_sorty[e] = (uint16_t)g_cell_sort; InterlockedExchange(&g_ent_dirty, 1); }
    int w = *(const uint16_t *)data, h = *(const uint16_t *)(data + 2);
    if (y1 > h) y1 = h;
    if (x1 > w) x1 = w;
    RECT box = { x, y, x + (x1 - x0), y + (y1 - y0) };
    if (s == MAP_SURF) dirty_map(s, box);                  /* the GPU copy of the map references */
    BYTE *ov = s == MAP_SURF ? NULL : over_buf(s);        /* drawn on the screen itself (roofs x-ray) */
    if (ov) {
        /* the box takes over the map's references first (through this frame's links), so the screen's
         * own references there are complete - as before links. Only the pixels the sprite really draws are
         * marked overridden (the run loop below), the box's see-through pixels still show the map through the
         * link: 2026-11-20, the whole box marked 1 cut a rectangle out of the mission-path lane round Sarge
         * (roofs.c redraws him onto the screen every frame he is covered; the lane leaves mask-1 pixels alone). */
        RECT c = { 0, 0, s->w, s->h }, bb;
        Surf *m = MAP_SURF;
        Ref *mr = m ? m->hd_ref : NULL;
        if (IntersectRect(&bb, &box, &c)) {
            for (int yy = bb.top; yy < bb.bottom; yy++)
                for (int xx = bb.left; xx < bb.right; xx++) {
                    BYTE *o = &ov[(size_t)yy * s->w + xx];
                    if (*o) continue;
                    Ref v = { 0 };
                    for (int i = 0; i < g_nlink_build && mr; i++) {
                        const Link *l = &g_link_build[i];
                        if (xx < l->r.left || xx >= l->r.right || yy < l->r.top || yy >= l->r.bottom) continue;
                        int mx = xx + l->ox, my = yy + l->oy;
                        if (mx >= 0 && my >= 0 && mx < m->w && my < m->h) v = ref_row(m, mr, my)[mx];
                        break;
                    }
                    ref_row(s, r, yy)[xx] = v;
                }
            UnionRect(&g_obox[g_oi], &g_obox[g_oi], &bb);
        }
    }
    BYTE fl = (BYTE)((e >= 0 && (g_e[e].flags & 1) ? FL_RECOLOUR : 0) | army_of(remap) << 2);
    for (int sy = y0; sy < y1; sy++) {
        int dy = y + sy - y0;
        if (dy < 0 || dy >= s->h) continue;
        size_t p = dword ? ((const uint32_t *)(data + 4))[sy] : ((const uint16_t *)(data + 4))[sy];
        Ref *row = ref_row(s, r, dy);
        const BYTE *pix = s->bits + (size_t)dy * s->pitch;
        int sx = 0;
        for (int guard = 0; guard < 4096; guard++) {
            sx += data[p++];
            if (sx >= w) break;
            int n = data[p++];
            for (int k = 0; k < n; k++, sx++) {
                if (sx < x0 || sx >= x1) continue;
                int dx = x + sx - x0;
                if (dx < 0 || dx >= s->w) continue;
                Ref *q = &row[dx];
                if (ov) { BYTE *o = &ov[(size_t)dy * s->w + dx]; if (*o == 0 || *o == 2) *o = 1; }   /* drawn on the screen */

                if (shadow) { if (q->e) { q->fl |= FL_SHADOW; q->expect = pix[dx]; } continue; }
                if (e < 0) { q->e = 0; continue; }
                *q = (Ref){ (uint16_t)(e + 1), (uint16_t)sx, (uint16_t)sy, pix[dx], fl };
            }
            if (!shadow) p += n;
            if (sx >= w) break;
        }
    }
}

Blit5Fn hdworld_wrle_original, hdworld_drle_original, hdworld_shadow_original;
Blit6Fn hdworld_remap_original;

void __fastcall hdworld_wrle(int x, int y, const BYTE *d, int x0, int y0, int x1, int y1)
{ hdworld_wrle_original(x, y, d, x0, y0, x1, y1); note_sprite(x, y, d, x0, y0, x1, y1, 0, 0, NULL); }
void __fastcall hdworld_drle(int x, int y, const BYTE *d, int x0, int y0, int x1, int y1)
{ hdworld_drle_original(x, y, d, x0, y0, x1, y1); note_sprite(x, y, d, x0, y0, x1, y1, 1, 0, NULL); }
void __fastcall hdworld_remap(int x, int y, const BYTE *d, int x0, int y0, int x1, int y1, const BYTE *remap)
{ hdworld_remap_original(x, y, d, x0, y0, x1, y1, remap); note_sprite(x, y, d, x0, y0, x1, y1, 0, 0, remap); }
/* roofs.c draws a figure a second time over a building front: its shadow was drawn with the first (before the
 * front) - not again over the front (game thread only) */
static int g_skip_shadows;
void hdworld_skip_shadows(int on) { g_skip_shadows = on; }
void __fastcall hdworld_shadow(int x, int y, const BYTE *d, int x0, int y0, int x1, int y1)
{
    if (g_skip_shadows) return;
    hdworld_shadow_original(x, y, d, x0, y0, x1, y1); note_sprite(x, y, d, x0, y0, x1, y1, 0, 1, NULL);
}

/* ---------------------------------------------------------------- copies between surfaces */

static int tile_entry(Surf *s, int tx, int ty)
{
    int cols = s->w / 16, rows = s->h / 16, i = ty * cols + tx;
    if (!s->hd_tiles) s->hd_tiles = calloc((size_t)cols * rows, sizeof(int));
    if (!s->hd_tiles) return -1;
    if (!s->hd_tiles[i]) {
        BYTE t[256];
        for (int y = 0; y < 16; y++) memcpy(t + y * 16, s->bits + (size_t)(ty * 16 + y) * s->pitch + tx * 16, 16);
        int e = lookup(fnv(t, 256), 16, 16, 1);
        if (g_cfg.log >= 2) { static int n; if (n++ < 20) mlog("hdworld: tile %d,%d of %dx%d atlas key %08x -> %s", tx, ty, s->w, s->h, fnv(t, 256), e >= 0 ? "upscaled" : "not in pack"); }
        s->hd_tiles[i] = e + 2;                    /* 1 = not in the pack */
    }
    if (s->hd_tiles[i] >= 2) request(s->hd_tiles[i] - 2);       /* (again after an atlas restart) */
    return s->hd_tiles[i] - 2;
}

/* surface.c, after copying s (part sr) to d at dr (same size). keyed: source pixels in [lo, hi] skipped. */
static void cat_note(int k, LONGLONG bytes);
static void note_copy(Surf *d, RECT dr, Surf *s, RECT sr, int keyed, DWORD lo, DWORD hi)
{
    if (g_ready != 1 || !g_cfg.hd_world || !d || d->is_primary) return;
    int w = dr.right - dr.left, h = dr.bottom - dr.top;
    if (w <= 0 || h <= 0) return;
    if (sr.right - sr.left != w || sr.bottom - sr.top != h) {
        /* A STRETCHED picture on the screen (2026-11-20: the in-game menus drawn x k, hud.c): it is interface, it
         * must override what the battlefield pass marked there - the Sarge ghost (mask 3) showed through the pause
         * menu because this path returned before marking. Keyed: only the picture's own (non-key) pixels. */
        if (d == BACK_SURF && s && s->bits) mark_over_scaled(d, dr, s, sr, keyed, lo, hi);
        else if (d == BACK_SURF) mark_over(d, dr, NULL, 0, 0, 0, 0);
        return;
    }
    Ref *sref = s ? refs(s, 0) : NULL;
    int tiles = s && !sref && s->w == 512 && s->h >= 16;      /* tile sheet: whole or clipped tiles */
    if (d == MAP_SURF) {
        static int moves = -1;                             /* [display] hd_map_moves=0: upload scrolls (old way) */
        if (moves < 0) moves = GetPrivateProfileIntA("display", "hd_map_moves", 1, mod_ini_path());
        if (moves && s == d && sref && !keyed) map_move(d, sr, dr);   /* a scroll: repeated on the GPU, not uploaded */
        else dirty_map(d, dr);
    }
    else if (d == BACK_SURF) {
        static int links = -1;
        if (links < 0) links = GetPrivateProfileIntA("display", "hd_links", 1, mod_ini_path());   /* 0 = copy refs (debug) */
        if (links && sref && s == MAP_SURF && !keyed && g_nlink_build < MAX_LINKS) {
            /* the battlefield, map -> screen: a link instead of copying the references (gpu.c reads the map's) */
            g_link_build[g_nlink_build++] = (Link){ dr, sr.left - dr.left, sr.top - dr.top };
            BYTE *o = over_buf(d);                         /* the link's pixels are the map's, not drawn over */
            if (o) for (int y = dr.top; y < dr.bottom; y++) if (y >= 0 && y < d->h) {
                int a = dr.left < 0 ? 0 : dr.left, b = dr.right > d->w ? d->w : dr.right;
                if (b > a) memset(o + (size_t)y * d->w + a, 0, b - a);
            }
            return;
        }
        mark_over(d, dr, s ? s->bits + (size_t)sr.top * s->pitch + sr.left : NULL, s ? s->pitch : 0, keyed, lo, hi);
    }
    Ref *dref = refs(d, sref || tiles);
    if (!dref) return;
    if (g_cfg.log >= 2) cat_note(s == d ? 0 : sref && s == MAP_SURF && d == BACK_SURF ? 1 : sref ? 2 : tiles ? 3 : 4, (LONGLONG)w * h * sizeof(Ref));
    if (sref && s == d && !keyed && s == g_roll.s) {          /* the map scrolled: re-point, no move */
        if (roll_shift(d, dr, sr)) { g_roll_hits++; return; }
        g_roll_misses++;
        if (g_cfg.log >= 2) { static int n; if (n++ < 10) mlog("hdworld: map self-copy not a whole shift: %ld,%ld,%ld,%ld <- %ld,%ld (map %dx%d)", dr.left, dr.top, dr.right, dr.bottom, sr.left, sr.top, d->w, d->h); }
    }
    if (sref && s == d && dr.top > sr.top) {                  /* overlapping scroll downwards: bottom up */
        for (int y = h - 1; y >= 0; y--)
            memmove(ref_row(d, dref, dr.top + y) + dr.left, ref_row(s, sref, sr.top + y) + sr.left, w * sizeof(Ref));
        return;
    }
    for (int y = 0; y < h; y++) {
        Ref *out = ref_row(d, dref, dr.top + y) + dr.left;
        const BYTE *src = s ? s->bits + (size_t)(sr.top + y) * s->pitch + sr.left : NULL;
        if (sref) {
            const Ref *in = ref_row(s, sref, sr.top + y) + sr.left;
            if (!keyed) { memmove(out, in, w * sizeof(Ref)); continue; }
            for (int x = 0; x < w; x++) if (!(src[x] >= lo && src[x] <= hi)) out[x] = in[x];
            continue;
        }
        if (tiles) {
            const BYTE *dst = d->bits + (size_t)(dr.top + y) * d->pitch + dr.left;
            int ty = (sr.top + y) / 16, sy = (sr.top + y) & 15;
            for (int x = 0; x < w; x++) {
                if (keyed && src[x] >= lo && src[x] <= hi) continue;
                int e = tile_entry(s, (sr.left + x) / 16, ty);
                out[x] = e >= 0 ? (Ref){ (uint16_t)(e + 1), (uint16_t)((sr.left + x) & 15), (uint16_t)sy, dst[x], 0 } : (Ref){ 0 };
            }
            continue;
        }
        if (g_cfg.log >= 3 && y == 0) { static int n; if (n++ < 30) mlog("hdworld: copy %dx%d from %p (%dx%d, no refs) to %p (%dx%d) at %ld,%ld - cleared", w, h, s, s ? s->w : 0, s ? s->h : 0, d, d->w, d->h, dr.left, dr.top); }
        if (!keyed) { memset(out, 0, w * sizeof(Ref)); continue; }
        for (int x = 0; x < w; x++) if (!(src[x] >= lo && src[x] <= hi)) out[x].e = 0;
    }
}
/* surface.c: a colour fill. */
void hdworld_note_fill(Surf *d, RECT r)
{
    if (g_ready == 1 && d == MAP_SURF) dirty_map(d, r);
    else if (g_ready == 1 && d == BACK_SURF) mark_over(d, r, NULL, 0, 0, 0, 0);
    Ref *dref = d ? refs(d, 0) : NULL;
    if (!dref) return;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > d->w) r.right = d->w;
    if (r.bottom > d->h) r.bottom = d->h;
    for (int y = r.top; y < r.bottom; y++) memset(ref_row(d, dref, y) + r.left, 0, (r.right - r.left) * sizeof(Ref));
}

/* layout.c drew interface pixels x0..x1 of row y itself (enlarged HUD corners). */
void hdworld_clear_run(Surf *d, int y, int x0, int x1)
{
    if (g_ready == 1) mark_over(d, (RECT){ x0, y, x1, y + 1 }, NULL, 0, 0, 0, 0);
    Ref *dref = d ? refs(d, 0) : NULL;
    if (!dref || y < 0 || y >= d->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 > d->w) x1 = d->w;
    if (x1 > x0) memset(ref_row(d, dref, y) + x0, 0, (x1 - x0) * sizeof(Ref));
}

/* layout.c: pixels x0..x1 of row y should show the BATTLEFIELD again (the see-through margin of an enlarged
 * HUD group lies over its own 1x picture, 2026-11-17): the bytes come back from the map surface through this
 * frame's link (map -> screen copy), the override mask is cleared, so the upscaled battlefield shows there
 * too. Pixels outside every link are left alone. */
void hdworld_restore_field(Surf *b, int y, int x0, int x1)
{
    if (g_ready != 1 || !b || b != BACK_SURF || y < 0 || y >= b->h) return;
    Surf *m = MAP_SURF;
    if (!m || !m->bits) return;
    if (x0 < 0) x0 = 0;
    if (x1 > b->w) x1 = b->w;
    BYTE *o = over_buf(b);
    for (int i = 0; i < g_nlink_build; i++) {
        const Link *l = &g_link_build[i];
        if (y < l->r.top || y >= l->r.bottom) continue;
        int a = x0 > l->r.left ? x0 : l->r.left, e = x1 < l->r.right ? x1 : l->r.right;
        int my = y + l->oy;
        if (e <= a || my < 0 || my >= m->h) continue;
        for (int x = a; x < e; x++) {
            int mx = x + l->ox;
            if (mx < 0 || mx >= m->w) continue;
            b->bits[(size_t)y * b->pitch + x] = m->bits[(size_t)my * m->pitch + mx];
            if (o) o[(size_t)y * b->w + x] = 0;
        }
    }
}

/* A GROUND-LEVEL overlay pixel run (modern.c route line), drawn into the back buffer after the battlefield:
 * the shader shows it over the ground but behind every object (upscaled images, their soft edges and
 * margins included), at 30 % where one covers it. Mask value 2 (1 = interface, on top: hdworld_clear_run);
 * the map's references stay in use there, the byte is not checked against them. */
void hdworld_under_run(Surf *d, int y, int x0, int x1)
{
    if (g_ready != 1 || !d || y < 0 || y >= d->h) return;
    BYTE *o = over_buf(d);
    if (!o) return;
    if (x0 < 0) x0 = 0;
    if (x1 > d->w) x1 = d->w;
    if (x1 <= x0) return;
    BYTE *row = o + (size_t)y * d->w;
    for (int x = x0; x < x1; x++) if (!row[x]) row[x] = 2;
    RECT rr = { x0, y, x1, y + 1 };
    UnionRect(&g_obox[g_oi], &g_obox[g_oi], &rr);
}

/* The same for a whole run in one colour (modern.c mission path band, 2026-11-20): byte c is written at x0..x1 of
 * row y and the pixel marked ground-level overlay - except where something is already drawn over the battlefield
 * on the screen itself (mask 1: Sarge redrawn over a rock by roofs.c, explosions, interface): those stay in front.
 * Returns 0 when the override mask is not available (upscaled battlefield off): the caller draws per pixel. */
int hdworld_under_fill(Surf *d, int y, int x0, int x1, BYTE c)
{
    if (g_ready != 1 || !d || !d->bits || y < 0 || y >= d->h) return 0;
    BYTE *o = over_buf(d);
    if (!o) return 0;
    if (x0 < 0) x0 = 0;
    if (x1 > d->w) x1 = d->w;
    if (x1 <= x0) return 1;
    BYTE *row = o + (size_t)y * d->w, *px = d->bits + (size_t)y * d->pitch;
    for (int x = x0; x < x1; x++) {
        if (row[x] == 1) continue;
        px[x] = c;
        if (!row[x]) row[x] = 2;
    }
    RECT rr = { x0, y, x1, y + 1 };
    UnionRect(&g_obox[g_oi], &g_obox[g_oi], &rr);
    return 1;
}

/* layout.c moved the battlefield rows of the back buffer up by n (map bottom): the links move up too
 * (reading n rows further down the map), the override mask moves with the rows, the bottom n rows are
 * black (overridden, no references). */
void hdworld_shift_rows(Surf *b, int n)
{
    if (!b || n <= 0 || n >= b->h) return;
    for (int i = 0; i < g_nlink_build; i++) {
        Link *l = &g_link_build[i];
        l->r.top -= n; l->r.bottom -= n; l->oy += n;
        if (l->r.top < 0) l->r.top = 0;
    }
    BYTE *o = over_buf(b);
    if (o) {
        memmove(o, o + (size_t)n * b->w, (size_t)(b->h - n) * b->w);
        mark_over(b, (RECT){ 0, b->h - n, b->w, b->h }, NULL, 0, 0, 0, 0);
        g_obox[g_oi].top = 0;                          /* everything above may have moved */
        if (IsRectEmpty(&g_obox[g_oi])) SetRect(&g_obox[g_oi], 0, 0, b->w, b->h);
    }
    Ref *r = refs(b, 0);
    if (!r) return;
    memmove(r, r + (size_t)n * b->w, (size_t)(b->h - n) * b->w * sizeof(Ref));
    memset(r + (size_t)(b->h - n) * b->w, 0, (size_t)n * b->w * sizeof(Ref));
}

/* ---------------------------------------------------------------- the shown frame */

/* The shown frame's references, double-buffered: hdworld_frame_shown (game thread, present lock) writes
 * slot g_fi (screen references swapped in, the frame's links, its override mask); hdworld_take (display
 * thread, present lock) takes that slot and switches g_fi to the other one, so gpu.c (hdworld_gpu_world)
 * can read it OUTSIDE the lock while the game draws on. Buffers are (re)allocated only in hdworld_take. */
static Ref *g_fbuf[2]; static int g_fvalid[2], g_fi, g_fw, g_fh, g_want_w, g_want_h;
static BYTE *g_fover[2]; static RECT g_fobox[2];                       /* override mask per slot */
static Link g_flink[2][MAX_LINKS]; static int g_fnlink[2];
static unsigned g_fseq, g_take_seq; static int g_taken = -1;

static void commit_map_changes(void);
static void atlas_restart(void);
/* Flip / windowed frame blit (caller holds the present lock). */
static void hdworld_frame_shown_body(Surf *b)
{
    if (g_ready != 1 || !g_cfg.hd_world || !b) return;
    int t = terrain_now();
    if (t != g_terrain || strncmp(g_map, MAP_NAME, 32)) {     /* new mission: sprites are new data */
        g_terrain = t;
        lstrcpynA(g_map, MAP_NAME, sizeof g_map);
        memset(g_cache, 0, sizeof g_cache);
        Surf *m = MAP_SURF;
        refs_clear(m);
        g_dirty_all = 1;
        if (b->hd_ref) memset(b->hd_ref, 0, (size_t)b->w * b->h * sizeof(Ref));
        for (int i = 0; i < 2; i++) if (g_fbuf[i] && i != g_taken) memset(g_fbuf[i], 0, (size_t)g_fw * g_fh * sizeof(Ref));
        atlas_restart();                                       /* the last mission's images make room */
        mlog("hdworld: map %s, terrain %d", g_map, g_terrain);
    }
    commit_map_changes();                                      /* the map references of THIS frame (see take_map_changes) */
    { static DWORD last; DWORD now = GetTickCount();
      if (g_cfg.log >= 1 && now - last > 10000 && (g_roll_hits || g_roll_misses)) {
          last = now;
          mlog("hdworld: map scrolls re-pointed %ld, moved %ld, re-centred %ld", g_roll_hits, g_roll_misses, g_roll_centres);
          g_roll_hits = g_roll_misses = g_roll_centres = 0;
      } }
    int nl = g_nlink_build; g_nlink_build = 0;
    /* a frame shown again without redrawing the battlefield (pause menu): the last links still hold */
    static Link last_links[MAX_LINKS]; static int last_n;
    if (nl) { memcpy(last_links, g_link_build, sizeof last_links); last_n = nl; }
    int reshown = 0;
    if (!nl && last_n && layout_view_is_gameplay()) { memcpy(g_link_build, last_links, sizeof last_links); nl = last_n; reshown = 1; }
    Ref *r = refs(b, 0);
    if (!layout_view_is_gameplay() || (!r && !nl)) { g_fvalid[0] = g_fvalid[1] = 0; return; }
    if (!r) r = refs(b, 1);
    if (!r) return;
    if (g_fw != b->w || g_fh != b->h || !g_fbuf[g_fi] || !g_fover[g_fi]) { g_want_w = b->w; g_want_h = b->h; g_fvalid[0] = g_fvalid[1] = 0; return; }
    /* screen references: swap (they are only read where the override mask says so) */
    b->hd_ref = g_fbuf[g_fi]; g_fbuf[g_fi] = r;
    /* the override mask: the slot gets this frame's marks (inside its box), the game's mask is cleared */
    BYTE *o = over_buf(b), *fo = g_fover[g_fi];
    RECT old = g_fobox[g_fi], nb = o ? g_obox[g_oi] : (RECT){ 0 };
    for (int y = old.top; y < old.bottom; y++) memset(fo + (size_t)y * b->w + old.left, 0, old.right - old.left);
    if (o) for (int y = nb.top; y < nb.bottom; y++) {
        memcpy(fo + (size_t)y * b->w + nb.left, o + (size_t)y * b->w + nb.left, nb.right - nb.left);
        memset(o + (size_t)y * b->w + nb.left, 0, nb.right - nb.left);
    }
    if (reshown && IsRectEmpty(&nb) && g_fover[g_fi ^ 1]) {     /* nothing redrawn: the last frame's marks hold too */
        RECT pb = g_fobox[g_fi ^ 1];
        for (int y = pb.top; y < pb.bottom; y++) memcpy(fo + (size_t)y * b->w + pb.left, g_fover[g_fi ^ 1] + (size_t)y * b->w + pb.left, pb.right - pb.left);
        nb = pb;
    }
    g_fobox[g_fi] = nb;
    if (o) SetRectEmpty(&g_obox[g_oi]);
    memcpy(g_flink[g_fi], g_link_build, sizeof g_link_build);
    g_fnlink[g_fi] = nl;
    g_fvalid[g_fi] = 1;
    g_fseq++;
}

/* ---------------------------------------------------------------- display thread */

static BYTE *g_tbits; static size_t g_tbits_cap; static RECT g_tview; static int g_tok;
/* map references changed since the last take, copied out under the lock (gpu.c uploads them) */
static Ref *g_stage; static size_t g_stage_cap; static RECT g_tdirty[MAX_DIRTY]; static size_t g_tdirty_at[MAX_DIRTY];
static int g_tndirty, g_tmap_w, g_tmap_h;
static volatile LONG g_force_map;          /* gpu.c made a new map texture: upload everything */
void hdworld_force_map(void) { InterlockedExchange(&g_force_map, 1); }

/* Map references are COMMITTED at the frame boundary (hdworld_frame_shown, game thread, present lock) and
 * only commits are taken. Before, a take copied the LIVE map references - and the display thread also takes
 * between frames (small screen writes such as the cursor, or the game's 50 ms wait timing out), while the
 * game is already drawing the next frame onto the map surface: the shown bytes were frame N, the upper part
 * of the map references frame N+1 -> a band where the resolve fell back to the original pixels ("tearing").
 * A commit not taken yet is merged into the next one (its rectangles are copied again, current = correct). */
static RECT g_cdirty[MAX_DIRTY]; static size_t g_cdirty_at[MAX_DIRTY]; static int g_cndirty, g_ctaken = 1;
#define CMAX_MOVES 32
static Move g_cmove[CMAX_MOVES], g_tmove[CMAX_MOVES]; static int g_cnmove, g_tnmove;   /* committed / taken moves */
static Ref *g_cstage; static size_t g_cstage_cap; static int g_cmap_w, g_cmap_h;
static void commit_map_changes(void)
{
    if (!g_dirty_cs_ready) return;
    EnterCriticalSection(&g_dirty_cs);
    Surf *m = MAP_SURF;
    Ref *mr = m ? m->hd_ref : NULL;
    if (!mr) { g_cmap_w = g_cmap_h = 0; LeaveCriticalSection(&g_dirty_cs); return; }
    if (m != g_dirty_surf || m->w != g_cmap_w || m->h != g_cmap_h || InterlockedExchange(&g_force_map, 0)) { g_dirty_surf = m; g_dirty_all = 1; }
    if (!g_ctaken) {                                           /* the last commit was never shown: carry it */
        /* its rectangles changed before this frame's moves; its moves come first */
        for (int i = 0; i < g_cndirty && !g_dirty_all; i++) dirty_before(m, g_cdirty[i], g_move, g_nmove);
        if (g_cnmove + g_nmove > CMAX_MOVES) g_dirty_all = 1;
    } else g_cnmove = 0;
    if (g_dirty_all) g_cnmove = 0;
    else for (int i = 0; i < g_nmove; i++) g_cmove[g_cnmove++] = g_move[i];
    g_nmove = 0;
    g_cmap_w = m->w; g_cmap_h = m->h;
    int n = g_dirty_all ? 1 : g_ndirty;
    RECT all = { 0, 0, m->w, m->h };
    size_t need = 0;
    for (int i = 0; i < n; i++) { RECT r = g_dirty_all ? all : g_dirty[i]; need += (size_t)(r.right - r.left) * (r.bottom - r.top); }
    if (need > g_cstage_cap) { free(g_cstage); g_cstage = malloc(need * sizeof(Ref)); g_cstage_cap = g_cstage ? need : 0; }
    if (!g_cstage) { g_cstage_cap = 0; LeaveCriticalSection(&g_dirty_cs); return; }   /* keep them for the next frame */
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        RECT r = g_dirty_all ? all : g_dirty[i];
        int w = r.right - r.left;
        g_cdirty[i] = r; g_cdirty_at[i] = at;
        for (int y = r.top; y < r.bottom; y++, at += w) memcpy(g_cstage + at, ref_row(m, mr, y) + r.left, w * sizeof(Ref));
    }
    g_cndirty = n; g_ctaken = 0;
    g_ndirty = 0; g_dirty_all = 0;
    LeaveCriticalSection(&g_dirty_cs);
}
/* display thread, present lock: hand the last commit over (stage buffers swapped, gpu.c uploads it after
 * the lock; the game commits into the other buffer meanwhile) */
static void take_map_changes(void)
{
    if (!g_dirty_cs_ready) return;
    EnterCriticalSection(&g_dirty_cs);
    g_tnmove = 0;
    g_tndirty = 0;
    g_tmap_w = g_cmap_w; g_tmap_h = g_cmap_h;
    if (!g_ctaken) {
        Ref *t = g_stage; size_t tc = g_stage_cap;
        g_stage = g_cstage; g_stage_cap = g_cstage_cap;
        g_cstage = t; g_cstage_cap = tc;
        for (int i = 0; i < g_cndirty; i++) { g_tdirty[i] = g_cdirty[i]; g_tdirty_at[i] = g_cdirty_at[i]; }
        g_tndirty = g_cndirty;
        for (int i = 0; i < g_cnmove; i++) g_tmove[i] = g_cmove[i];
        g_tnmove = g_cnmove; g_cnmove = 0;
        g_ctaken = 1;
    }
    LeaveCriticalSection(&g_dirty_cs);
}

/* Under the present lock: take the newest frame (screen references, links, override mask), the map
 * references changed since the last take, and a copy of the shown bytes. */
int hdworld_take(const BYTE *bits, int pitch, RECT view)
{
    g_tok = 0;
    if (g_want_w) {                                                /* new size: (re)allocate, nothing reads them now */
        for (int i = 0; i < 2; i++) {
            free(g_fbuf[i]); g_fbuf[i] = calloc((size_t)g_want_w * g_want_h, sizeof(Ref));   /* zeroed: swapped into the surface */
            free(g_fover[i]); g_fover[i] = calloc((size_t)g_want_w * g_want_h, 1);
            SetRectEmpty(&g_fobox[i]); g_fvalid[i] = 0;
        }
        g_fw = g_fbuf[0] && g_fbuf[1] && g_fover[0] && g_fover[1] ? g_want_w : 0; g_fh = g_fw ? g_want_h : 0;
        g_want_w = g_want_h = 0; g_taken = -1;
        g_dirty_all = 1;
        return 0;
    }
    { static int n; if (g_cfg.log >= 2 && n++ % 600 == 0) mlog("hdworld: take ready %d fw %d x %d view %ld,%ld,%ld,%ld taken %d valid %d %d seq %ld", (int)g_ready, g_fw, g_fh, view.left, view.top, view.right, view.bottom, g_taken, g_fvalid[0], g_fvalid[1], (long)g_fseq); }
    if (g_ready != 1 || !g_cfg.hd_world || g_cfg.crt || !g_fw) return 0;
    if (view.left != 0 || view.top != 0 || view.right > g_fw || view.bottom > g_fh) return 0;
    if (g_fseq != g_take_seq && g_fvalid[g_fi]) { g_taken = g_fi; g_fi ^= 1; g_take_seq = g_fseq; }
    if (g_taken < 0 || !g_fvalid[g_taken]) return 0;
    int vw = view.right - view.left, vh = view.bottom - view.top;
    if ((size_t)vw * vh > g_tbits_cap) { free(g_tbits); g_tbits = malloc((size_t)vw * vh); g_tbits_cap = g_tbits ? (size_t)vw * vh : 0; }
    if (!g_tbits) return 0;
    for (int y = 0; y < vh; y++) memcpy(g_tbits + (size_t)y * vw, bits + (size_t)(view.top + y) * pitch + view.left, vw);
    take_map_changes();
    g_tview = view;
    g_tok = 1;
    return 1;
}
/* The per-pixel work is done on the graphics card (gpu.c ps_resolve / ps_near): references (Ref = two
 * uint32: e | sx << 16, sy | expect << 16 | flags << 24) from the map (through the links) or the screen
 * (where the override mask is set), the shown bytes, and this table of entries (two uint32 per entry):
 *   [0] bit 31 resident | slice << 24 | atlas y of the sprite's own pixels << 12 | atlas x  (margin included)
 *   [1] original width | mirror << 16 | sprite << 17 | has a margin << 18 | original height (11 bits) << 19 | turret << 30 | low << 31 */
#define HD_MARGIN 4
static uint32_t *g_ents; static int g_ents_n;

static void build_entries(void)
{
    if (!g_ents) { g_ents = calloc((size_t)(g_ne + 256) * 2, sizeof(uint32_t)); if (!g_ents) return; }
    for (int i = 0; i < g_ne; i++) {
        const Entry *en = &g_e[i];
        int b = en->blob;
        uint32_t a = 0, c = 0;
        if (g_state[b] == 3) {
            uint32_t p = g_pos[b];
            int mx = (g_b[b].w - en->w * 2) / 2, my = (g_b[b].h - en->h * 2) / 2;
            if (mx < 0 || my < 0) mx = my = 0;
            int sprite = en->kind == 0, margin = sprite && mx >= 2 * HD_MARGIN && my >= 2 * HD_MARGIN, mir = (en->flags & 2) != 0;
            a = 0x80000000u | (p >> 24 & 3) << 24 | ((((p >> 12) & 0xfff) + my) & 0xfff) << 12 | (((p & 0xfff) + mx) & 0xfff);   /* modulo: g_pos may be a corner outside the slice (cropped images) */
            c = (uint32_t)en->w | (uint32_t)mir << 16 | (uint32_t)sprite << 17 | (uint32_t)margin << 18 | (uint32_t)((g_sorty && g_sorty[i] ? g_sorty[i] : en->h) & 0x7ff) << 19
                | (uint32_t)((en->flags & EF_TOP) != 0) << 30   /* vehicle turret: above its hull */
                | (uint32_t)((en->flags & EF_LOW) != 0) << 31;   /* low object: behind standing figures */
        }
        g_ents[2 * i] = a; g_ents[2 * i + 1] = c;
    }
    g_ents_n = g_ne;
}

/* gpu.c, after hdworld_take succeeded (display thread): this frame's inputs. */
int hdworld_gpu_world(HdWorldIn *in)
{
    if (!g_tok || g_taken < 0 || !g_fbuf[g_taken] || !g_fover[g_taken]) return 0;
    in->view = g_tview;
    in->screen_refs = g_fbuf[g_taken];
    in->screen_pitch = g_fw * (int)sizeof(Ref);
    in->over = g_fover[g_taken];
    in->over_box = g_fobox[g_taken];
    in->bytes = g_tbits;
    in->nlinks = g_fnlink[g_taken];
    for (int i = 0; i < in->nlinks; i++) {
        const Link *l = &g_flink[g_taken][i];
        in->link[i][0] = l->r.left; in->link[i][1] = l->r.top; in->link[i][2] = l->r.right; in->link[i][3] = l->r.bottom;
        in->link[i][4] = l->ox; in->link[i][5] = l->oy;
    }
    in->map_w = g_tmap_w; in->map_h = g_tmap_h;
    in->ndirty = g_tndirty;
    for (int i = 0; i < g_tndirty; i++) { in->dirty[i] = g_tdirty[i]; in->dirty_refs[i] = g_stage + g_tdirty_at[i]; }
    in->nmoves = g_tnmove;
    for (int i = 0; i < g_tnmove; i++) { in->move_src[i] = g_tmove[i].s; in->move_dst[i] = g_tmove[i].d; }
    g_tnmove = 0;
    g_tndirty = 0;                                                 /* handed over once */
    in->ents = NULL; in->nents = g_ne;
    if (InterlockedExchange(&g_ent_dirty, 0) || !g_ents) { build_entries(); in->ents = g_ents; }
    return g_ents != NULL;
}

/* ---------------------------------------------------------------- loading and the atlas */

static const GUID CLSID_WicFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID IID_WicFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID FMT_BGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };

static uint32_t *decode(IWICImagingFactory *fac, BYTE *data, DWORD size, int w, int h)
{
    IWICStream *st = NULL; IWICBitmapDecoder *dec = NULL; IWICBitmapFrameDecode *frm = NULL; IWICFormatConverter *cv = NULL;
    uint32_t *out = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(fac, &st)) && SUCCEEDED(IWICStream_InitializeFromMemory(st, data, size))
        && SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(fac, (IStream *)st, NULL, WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frm)) && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(fac, &cv))
        && SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)frm, &FMT_BGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))
        && (out = malloc((size_t)w * h * 4))) {
        if (FAILED(IWICFormatConverter_CopyPixels(cv, NULL, w * 4, w * h * 4, (BYTE *)out))) { free(out); out = NULL; }
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (frm) IWICBitmapFrameDecode_Release(frm);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    return out;
}

static DWORD WINAPI loader(void *arg)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *fac = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WicFactory, NULL, CLSCTX_INPROC_SERVER, &IID_WicFactory, (void **)&fac))) return 0;
    BYTE *buf = NULL; DWORD cap = 0;
    for (;;) {
        WaitForSingleObject(g_wake, 500);
        for (int b = 0; b < g_nb; b++) {
            if (g_state[b] != 1) continue;
            Blob *bl = &g_b[b];
            if (bl->size > cap) { free(buf); buf = malloc(bl->size); cap = buf ? bl->size : 0; }
            OVERLAPPED ov = { 0 }; ov.Offset = bl->off;
            DWORD got = 0;
            uint32_t *px = NULL;
            if (buf && ReadFile(g_pak, buf, bl->size, &got, &ov) && got == bl->size) px = decode(fac, buf, bl->size, bl->w, bl->h);
            if (!px) { InterlockedExchange(&g_state[b], 4); continue; }
            g_pix[b] = px;
            InterlockedExchange(&g_state[b], 2);
        }
    }
}

/* Shelf packing in ATLAS_SLICES slices of ATLAS_SIZE^2.
 * The atlas holds 4 x 4096^2 = 67 Mpx and the reference format (gpu.c wref: 2 bits of slice, 11 + 11 bits of
 * position) has no room for more. The Desert art alone is 170 Mpx as packed in world.pak, mostly EMPTY: a
 * building piece is a 1102x860 image with a small drawn part (measured 2026-11-19: images >= 0.5 Mpx are 80 %
 * empty, the whole Desert set 50 %). Walking into the Desert1 town filled the atlas within a minute ("hdworld:
 * atlas full" in the user's log) and everything first seen after that stayed low-res for the rest of the
 * session - walls, burnt bushes, damaged states next to upscaled art (user: "layering issues").
 * So: (1) only the drawn part of an image goes into the atlas (its alpha box + ATLAS_PAD texels, even edges);
 * the position kept in g_pos is where the WHOLE image's corner would be (modulo 4096 - it can lie outside),
 * so every reference still computes the same texel; (2) a new mission starts the atlas over. */
#define ATLAS_PAD 16                       /* texels kept around the drawn part: neighbours read up to 4 game px (8 texels) past a sprite's pixels */
#define ATLAS_CROP_MIN (64 * 64)           /* smaller images (ground tiles, small sprites) go in whole */
static int g_slice, g_shelf_x, g_shelf_y, g_shelf_h, g_full;

static int place(int w, int h, int *x, int *y, int *slice)
{
    if (g_full || w > ATLAS_SIZE || h > ATLAS_SIZE) return 0;
    if (g_shelf_x + w > ATLAS_SIZE) { g_shelf_x = 0; g_shelf_y += g_shelf_h; g_shelf_h = 0; }
    if (g_shelf_y + h > ATLAS_SIZE) {
        if (++g_slice >= ATLAS_SLICES) { g_full = 1; mlog("hdworld: atlas full"); return 0; }
        g_shelf_x = g_shelf_y = g_shelf_h = 0;
    }
    *x = g_shelf_x; *y = g_shelf_y; *slice = g_slice;
    g_shelf_x += (w + 2) & ~1;                     /* even positions: refs store x / 2, y / 2 */
    if (((h + 2) & ~1) > g_shelf_h) g_shelf_h = (h + 2) & ~1;
    return 1;
}

/* The drawn part of a decoded image (BGRA, alpha in the top byte) with ATLAS_PAD around it, even edges. */
static void drawn_box(const uint32_t *px, int w, int h, int *x0, int *y0, int *x1, int *y1)
{
    int l = w, t = h, r = 0, b = 0;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = px + (size_t)y * w;
        int a = 0, e = w - 1;
        while (a < w && !(row[a] >> 24)) a++;
        if (a == w) continue;
        while (e > a && !(row[e] >> 24)) e--;
        if (a < l) l = a;
        if (e + 1 > r) r = e + 1;
        if (y < t) t = y;
        b = y + 1;
    }
    if (r <= l || b <= t) { l = t = 0; r = b = 2; }       /* nothing drawn */
    l -= ATLAS_PAD; t -= ATLAS_PAD; r += ATLAS_PAD; b += ATLAS_PAD;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > w) r = w;
    if (b > h) b = h;
    *x0 = l & ~1; *y0 = t & ~1; *x1 = (r + 1) & ~1; *y1 = (b + 1) & ~1;
    if (*x1 > w) *x1 = w;
    if (*y1 > h) *y1 = h;
}

/* A new mission (game thread): every image leaves the atlas and is asked for again when it is drawn
 * (sprite_entry / tile_entry request on every use). The references were cleared just before. */
static void atlas_restart(void)
{
    if (!g_atlas_cs_ready) return;
    EnterCriticalSection(&g_atlas_cs);
    int n = 0;
    for (int b = 0; b < g_nb; b++)
        if (g_state[b] == 3 || g_state[b] == 4) { InterlockedExchange(&g_state[b], 0); n++; }
    g_slice = g_shelf_x = g_shelf_y = g_shelf_h = g_full = 0;
    g_ent_dirty = 1;
    LeaveCriticalSection(&g_atlas_cs);
    if (n) mlog("hdworld: atlas started over (%d images left it)", n);
}

/* gpu.c (display thread, at the start of a frame): upload decoded images. */
void hdworld_upload(void *ctx)
{
    if (g_ready != 1 || !g_atlas_cs_ready) return;
    EnterCriticalSection(&g_atlas_cs);
    int n = 0;
    for (int b = 0; b < g_nb && n < 256; b++) {
        if (g_state[b] != 2) continue;
        int w = g_b[b].w, h = g_b[b].h, x, y, s;
        int x0 = 0, y0 = 0, x1 = w, y1 = h;
        if (w * h >= ATLAS_CROP_MIN && !(w & 1) && !(h & 1)) drawn_box(g_pix[b], w, h, &x0, &y0, &x1, &y1);
        if (place(x1 - x0, y1 - y0, &x, &y, &s)
            && gpu_world_put(ctx, s, x, y, x1 - x0, y1 - y0, g_pix[b] + (size_t)y0 * w + x0, w * 4)) {
            /* where the whole image's corner would be, modulo the slice size */
            g_pos[b] = (uint32_t)((x - x0) & 0xfff) | (uint32_t)((y - y0) & 0xfff) << 12 | (uint32_t)s << 24;
            InterlockedExchange(&g_state[b], 3);
        } else InterlockedExchange(&g_state[b], 4);
        free(g_pix[b]); g_pix[b] = NULL;
        n++;
        g_ent_dirty = 1;                           /* gpu.c re-uploads the entry table */
    }
    { static DWORD last; DWORD now = GetTickCount();      /* how full the atlas is, once a minute */
      if (g_cfg.log >= 1 && n && now - last > 60000) { last = now; mlog("hdworld: atlas at slice %d of %d, row %d of %d", g_slice + 1, ATLAS_SLICES, g_shelf_y, ATLAS_SIZE); } }
    LeaveCriticalSection(&g_atlas_cs);
}

/* Army colour ramps for ps_world: 4 armies x 16 colours (palette 208/224/176/192 + i). */
void hdworld_ramps(float out[64][4])
{
    static const int base[4] = { 208, 224, 176, 192 };
    Pal *p = g_dd ? g_dd->pal : NULL;
    for (int a = 0; a < 4; a++)
        for (int i = 0; i < 16; i++) {
            PALETTEENTRY c = p ? p->e[base[a] + i] : (PALETTEENTRY){ 0 };
            out[a * 16 + i][0] = c.peRed / 255.f; out[a * 16 + i][1] = c.peGreen / 255.f;
            out[a * 16 + i][2] = c.peBlue / 255.f; out[a * 16 + i][3] = 0;
        }
    if (g_cfg.log >= 2) {
        static DWORD last;
        if (GetTickCount() - last > 5000) {
            last = GetTickCount();
            for (int a = 0; a < 4; a++) {
                PALETTEENTRY c0 = p ? p->e[base[a]] : (PALETTEENTRY){ 0 }, c8 = p ? p->e[base[a] + 8] : (PALETTEENTRY){ 0 };
                mlog("hdworld: ramp %d pal %p [0] %d,%d,%d [8] %d,%d,%d", a, (void *)p, c0.peRed, c0.peGreen, c0.peBlue, c8.peRed, c8.peGreen, c8.peBlue);
            }
        }
    }
}

/* surface.c: a copy between surfaces (timed for the perf log: the game thread's share of the upscale work) */
void hdworld_note_copy(Surf *d, RECT dr, Surf *s, RECT sr, int keyed, DWORD lo, DWORD hi)
{
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    note_copy(d, dr, s, sr, keyed, lo, hi);
    QueryPerformanceCounter(&b);
    present_perf_refs(b.QuadPart - a.QuadPart);
    {   /* slow copies (> 2 ms), a few per 10 s: what they are */
        static LARGE_INTEGER f; static DWORD t0; static int n;
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
        DWORD now = GetTickCount(); if (now - t0 > 10000) { t0 = now; n = 0; }
        if (ms > 2.0 && n++ < 4) mlog("hdworld: slow copy %.1f ms %ldx%ld -> %p (%dx%d%s%s) from %p (%dx%d%s)%s", ms, dr.right - dr.left, dr.bottom - dr.top, (void *)d, d ? d->w : 0, d ? d->h : 0, d && d->is_primary ? " primary" : "", d == BACK_SURF ? " back" : d == MAP_SURF ? " map" : "", (void *)s, s ? s->w : 0, s ? s->h : 0, s && s->is_primary ? " primary" : "", keyed ? " keyed" : "");
    }
}

void hdworld_force_entries(void) { InterlockedExchange(&g_ent_dirty, 1); }   /* gpu.c: new entry texture */

/* log=2: where the game thread's reference copying goes (bytes per frame, by kind), every 5 s */
static LONGLONG g_cat_bytes[5]; static const char *const CAT_NAME[5] = { "self-scroll", "map->screen", "other refs", "tiles", "clear" };
static void cat_note(int k, LONGLONG bytes)
{
    static DWORD last; static int frames;
    g_cat_bytes[k] += bytes;
    if (k == 1) frames++;
    DWORD now = GetTickCount();
    if (g_cfg.log >= 2 && now - last >= 5000 && frames) {
        mlog("hdworld: ref copies per frame: %s %d KB, %s %d KB, %s %d KB, %s %d KB, %s %d KB",
             CAT_NAME[0], (int)(g_cat_bytes[0] / frames / 1024), CAT_NAME[1], (int)(g_cat_bytes[1] / frames / 1024),
             CAT_NAME[2], (int)(g_cat_bytes[2] / frames / 1024), CAT_NAME[3], (int)(g_cat_bytes[3] / frames / 1024),
             CAT_NAME[4], (int)(g_cat_bytes[4] / frames / 1024));
        memset(g_cat_bytes, 0, sizeof g_cat_bytes); frames = 0; last = now;
    }
}

/* roofs.c: the current override mask of the screen buffer (pixels it puts back are the map's again). */
BYTE *hdworld_over_mask(Surf *b) { return g_ready == 1 ? over_buf(b) : NULL; }

void hdworld_frame_shown(Surf *b) { PROF_BEGIN(t); hdworld_frame_shown_body(b); PROF_END(t, PR_SHOWN); }   /* timed (trace) */

/* objectives.c: image number idx drawn through this cell (see g_cellmap): its atlas place (half-resolution x, y as
 * in the wref format, slice) and the world corner it was drawn at. 1 = filled in; 0 = that image is not usable
 * (not loaded yet, or mirrored: its columns run the other way); -1 = no more images. */
int hdworld_cell_image(const BYTE *cell, int idx, int *hx, int *hy, int *slice, int *wl, int *wt)
{
    if (g_ready != 1 || !cell || idx < 0) return -1;
    const CellImages *c = cellmap_find(cell, 0);
    if (!c || idx >= c->n) return -1;
    int e = c->entry[idx];
    if (e < 0 || e >= g_ne) return 0;
    const Entry *en = &g_e[e];
    int b = en->blob;
    if (g_state[b] != 3 || (en->flags & 2)) return 0;
    uint32_t p = g_pos[b];
    int mx = (g_b[b].w - en->w * 2) / 2, my = (g_b[b].h - en->h * 2) / 2;
    if (mx < 0 || my < 0) mx = my = 0;
    *hx = (int)(((p & 0xfff) + mx) & 0xfff) >> 1;
    *hy = (int)((((p >> 12) & 0xfff) + my) & 0xfff) >> 1;
    *slice = (int)(p >> 24 & 3);
    *wl = c->wl[idx]; *wt = c->wt[idx];
    return 1;
}

/* roofs.c (building interior, 2026-11-20): the atlas entry of the image drawn at back-buffer pixel (x, y) - the map
 * pixel shown there through this frame's links (map -> screen copy) - or -1. */
int hdworld_entry_at(int x, int y)
{
    Surf *m = MAP_SURF;
    if (g_ready != 1 || !m || !m->hd_ref) return -1;
    for (int i = 0; i < g_nlink_build; i++) {
        const Link *l = &g_link_build[i];
        if (x < l->r.left || x >= l->r.right || y < l->r.top || y >= l->r.bottom) continue;
        int mx = x + l->ox, my = y + l->oy;
        if (mx < 0 || my < 0 || mx >= m->w || my >= m->h) return -1;
        Ref r = ref_row(m, m->hd_ref, my)[mx];
        return r.e ? (int)r.e - 1 : -1;
    }
    return -1;
}
/* is entry e one of the images drawn through this cell (g_cellmap)? */
int hdworld_cell_has_entry(const BYTE *cell, int e)
{
    if (g_ready != 1 || !cell || e < 0) return 0;
    const CellImages *c = cellmap_find(cell, 0);
    if (!c) return 0;
    for (int i = 0; i < c->n; i++) if (c->entry[i] == e) return 1;
    return 0;
}

/* objectives.c (2026-11-21, user: "why is the objective fade not consistent?"): does the GPU show the UPSCALED picture
 * at back-buffer pixel (x, y)? Only where a map link covers it, the map has an image reference there and the screen
 * byte is still the map's (the game's map-edge dither, sprites drawn into the buffer etc. change it -> the game pixel
 * is shown). The pinned objective box darkens the others in the bytes, the upscaled ones on the GPU (td). */
int hdworld_pixel_upscaled(const Surf *b, int x, int y)
{
    Surf *m = MAP_SURF;
    if (g_ready != 1 || !m || !m->hd_ref || !m->bits || !b || !b->bits) return 0;
    for (int i = 0; i < g_nlink_build; i++) {
        const Link *l = &g_link_build[i];
        if (x < l->r.left || x >= l->r.right || y < l->r.top || y >= l->r.bottom) continue;
        int mx = x + l->ox, my = y + l->oy;
        if (mx < 0 || my < 0 || mx >= m->w || my >= m->h) return 0;
        if (!ref_row(m, m->hd_ref, my)[mx].e) return 0;
        return m->bits[(size_t)my * m->pitch + mx] == b->bits[(size_t)y * b->pitch + x];
    }
    return 0;
}

/* savestate.c, a load from a save file put the saved pixels into s: what hdworld knew about its pixels is stale */
void hdworld_surface_replaced(Surf *s)
{
    if (!s) return;
    hdworld_forget(s);                            /* the rolled map surface: its row tables (mem = s->hd_ref) */
    free(s->hd_ref); s->hd_ref = NULL;
    free(s->hd_tiles); s->hd_tiles = NULL;
}
