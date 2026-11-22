/*
 * savestate.c - quicksave / quickload of the running mission (2026-11-21, [savestate]).
 *
 * The original game only keeps progress BETWEEN missions (the army file, FUN_0041a3e0 write / FUN_0041a970 read).
 * This saves the whole running mission as a memory snapshot and puts it back in place:
 *
 *   1. The game heap. Everything the game allocates goes through the exe's own CRT (statically linked, VC5):
 *      malloc 0x4708e0, free 0x470870, realloc 0x471130, calloc 0x47cd60, _expand 0x473e80, _msize 0x473f30 over a
 *      small-block heap (regions VirtualAlloc'd, list 0x4d49f0) and the Win32 heap *0x6ace64 (HeapCreate in
 *      _heap_init 0x476c90). Neither can be copied back in place safely (Windows' heap keeps state outside its
 *      pages: low-fragmentation front end, encoded headers), so the six functions are REWRITTEN here (hook_jmp,
 *      installed from DllMain, i.e. before the exe's CRT start-up made its first allocation): the game's blocks come
 *      from our own allocator in ONE reserved address range (the arena, all bookkeeping inside it), so a copy of
 *      the arena's used part is the complete heap state. Callers that hold LIVE outside state keep the Win32 heap
 *      ("keep heap", never saved or restored): the CRT itself (0x470000..0x48bf00: stdio buffers, lock objects,
 *      per-thread data, locale) except _strdup / _fullpath (they allocate for the game), the sound module
 *      0x40f1c0..0x412840 (DirectSound buffers per playing effect, wave records with FILE*, music streams) and the
 *      radio voice module 0x4688a0..0x46a6e0. free / realloc / _msize decide by the block's address.
 *   2. The exe's writable data (.data + .bss, 0x4ab000 .. end of .data, ~2 MB), except the ranges in KEEP[] that
 *      describe live outside resources: CRT / MFC state (stdio FILEs with their OS handles, locks, heap handle,
 *      atexit, MFC objects), the sound module's globals (DirectSound, the instance table, wave cache, music stream
 *      slots + their multimedia timers) and the voice module's globals. Those are never written by a load, so a
 *      timer callback streaming music or a voice never sees a half-restored state.
 *   3. The mod's own per-mission state (weapons boxes, medkits, awareness, captured vehicle, patrols, mines,
 *      projectile tables ...): each file registers its variables (savestate_region, the *_savestate functions).
 *   4. The game clock: our GetTickCount (clock.c) is set back to the value it had at the save, so every time stamp
 *      in the restored state is in the past by the same amount as at the save.
 *
 * Safe point: the load (and the save) runs in the game's idle loop (CWinApp::Run override 0x4014e0: PeekMessage
 * loop, call 0x40150d -> the frame FUN_00407460), i.e. between two frames, with no game function on the stack.
 * Before the copy every playing sound effect is stopped with the game's own stop (FUN_0040f920, ecx = handle) -
 * a looping engine of a vehicle that no longer exists would otherwise never stop; music and radio voices go on.
 * The ground layer is redrawn in full (0x4e30e4 = 1, FUN_0042d610 -> FUN_0042e3e0): its pixels are in a surface,
 * not in the snapshot. Held inputs / pending clicks of the modern controls are dropped (controls_reset).
 * Slots: [savestate] slots (default 3); a slot belongs to one mission RUN (gameplay_run: any (re)start drops it -
 * a new run loads new sprites, the old surface pointers would be stale). Refused while not in play, while a game
 * menu is up / the game is paused, while Sarge is dead (save), or for an empty slot.
 * Keys ([savestate] save_key / load_key / slot_key, defaults Insert / Home / End; "Ctrl+", "Shift+", "Alt+" allowed):
 * F5 and F9 are taken (F5 = the game's air strike, F9 = the mod's CRT filter and the game's second key of an
 * action), so they are not the defaults. Navigation keys only count as the dedicated keys (extended flag), never
 * the number pad with Num Lock off.
 * Surfaces: the game keeps pointers to our DirectDraw surfaces (Wine, mission 6: 72 pointers to 71 surfaces in the
 * snapshot, [savestate] scan=1). While any slot is in use, a surface the game releases is kept alive (surface.c
 * zombies) and a load gives every surface of the save its reference count of then; zombies no slot lists are freed.
 * A display reset since the save (another primary surface) refuses the load.
 * Not written to disk (tried to judge, 2026-11-21): the snapshot holds addresses of objects that only exist in this
 * process and are made again at OTHER addresses in a new one - our surfaces (process heap, the 72 pointers above),
 * their pixel memory, MFC42 / msvcrt objects, Windows handles; and the arena itself is placed wherever Windows has
 * room (a fixed address split Wine's large free range and starved the HD mode). Mapping them would need to know
 * every such pointer, i.e. a semantic save of the game's objects. A quicksave lives until the game is quit or the
 * mission run ends. The slots are temp files (%TEMP%\amq*.tmp, deleted on close), not memory: address space the
 * 32-bit HD mode needs.
 * Checks: after every save and load the arena is walked (ar_check; a broken heap switches quicksave off).
 */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- game addresses */
#define CRT_HEAP          (*(HANDLE *)0x6ace64)   /* the exe CRT's Win32 heap (HeapCreate in _heap_init 0x476c90) */
#define GAME              (*(BYTE **)0x4b3f70)    /* gpGame */
#define GAME_TICK_LAST(g) (*(DWORD *)((g) + 0x40)) /* GetTickCount of the last frame (FUN_00407460) */
#define SARGE             (*(BYTE **)0x697520)
#define SARGE_HEALTH(u)   (*(short *)((u) + 0x1c))
#define FIELD_REDRAW      (*(int *)0x4e30e4)      /* 1 = FUN_0042d610 redraws the whole ground layer (FUN_0042e3e0) */
#define SFX_ON            (*(int *)0x4e3ac8)      /* sound module up */
#define SFX_TABLE         (*(BYTE **)0x4e3ad0)    /* effect instances, 0x28 bytes each (+4 handle, -1 = free) */
#define SFX_COUNT         (*(unsigned *)0x4e3ac4)
#define SFX_SIZE          0x28
#define SFX_STOP          ((void (__fastcall *)(int))0x40f920)   /* stop + release one effect (ecx = handle) */
#define FRAME_CALL        0x40150d                /* CWinApp::Run override: call FUN_00407460 (one frame) */
#define FRAME_FN          0x407460

/* exe ranges never written by a load (see the header) */
static const struct { uintptr_t lo, hi; const char *what; } KEEP[] = {
    { 0x4ab000, 0x4ab180, "CRT start-up tables" },
    { 0x4b280c, 0x4b2840, "sound module (DirectSound)" },
    { 0x4cfea8, 0x4cff40, "CRT data" },
    { 0x4d3afc, 0x4d3b24, "voice module" },
    { 0x4d3c00, 0x4d8000, "CRT / MFC data (stdio, locks, heap, locale)" },
    { 0x4e3ac0, 0x4e3b9c, "sound module (instances, wave cache, music streams)" },
    { 0x4e9f38, 0x4e9f3c, "sound module" },
    { 0x6a5b88, 0x6a5bc0, "voice module" },
    { 0x6a63b8, 0x6a63c0, "voice module" },
    { 0x6a99d8, 0x6ae000, "MFC objects, CRT data" },
};
#define NKEEP ((int)(sizeof KEEP / sizeof KEEP[0]))

/* callers whose blocks stay on the keep heap */
static int keep_caller(uintptr_t ra)
{
    if (ra >= 0x470000 && ra < 0x48bf00)          /* CRT, except the two that allocate FOR the game */
        return !(ra >= 0x473180 && ra < 0x4731d0) && !(ra >= 0x471fd0 && ra < 0x47209a);
    return (ra >= 0x40f1c0 && ra < 0x412840) || (ra >= 0x4688a0 && ra < 0x46a6e0);   /* sound, voice */
}

/* ---------------------------------------------------------------- settings */
static struct {
    int enabled, slots, heap_mb, scan;
    struct { int vk, mods; } key[3];               /* save, load, slot */
} C;
enum { K_SAVE, K_LOAD, K_SLOT };
enum { M_CTRL = 1, M_SHIFT = 2, M_ALT = 4 };

static const struct { WORD vk; const char *name; } KEYS[] = {
    { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" }, { VK_HOME, "Home" }, { VK_END, "End" }, { VK_PRIOR, "PgUp" },
    { VK_NEXT, "PgDn" }, { VK_PAUSE, "Pause" }, { VK_SCROLL, "ScrollLock" }, { VK_BACK, "Backspace" }, { VK_TAB, "Tab" },
    { VK_OEM_4, "[" }, { VK_OEM_6, "]" }, { VK_OEM_5, "\\" }, { VK_OEM_1, ";" }, { VK_OEM_7, "'" }, { VK_OEM_COMMA, "," },
    { VK_OEM_PERIOD, "." }, { VK_OEM_2, "/" }, { VK_OEM_3, "`" }, { VK_OEM_MINUS, "-" }, { VK_OEM_PLUS, "=" },
};
static int nav_key(int vk) { return vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT; }

static void parse_key(const char *s, int *vk, int *mods)
{
    *vk = 0; *mods = 0;
    for (;;) {
        if (!_strnicmp(s, "Ctrl+", 5)) { *mods |= M_CTRL; s += 5; }
        else if (!_strnicmp(s, "Shift+", 6)) { *mods |= M_SHIFT; s += 6; }
        else if (!_strnicmp(s, "Alt+", 4)) { *mods |= M_ALT; s += 4; }
        else break;
    }
    if (!s[0] || !lstrcmpiA(s, "none")) return;
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) { *vk = s[0]; return; }
    if (!s[1] && s[0] >= 'a' && s[0] <= 'z') { *vk = s[0] - 32; return; }
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') { int f = atoi(s + 1); if (f >= 1 && f <= 24) *vk = VK_F1 + f - 1; return; }
    if (!_strnicmp(s, "Num", 3) && s[3] >= '0' && s[3] <= '9' && !s[4]) { *vk = VK_NUMPAD0 + s[3] - '0'; return; }
    if (!_strnicmp(s, "vk", 2)) { int v = atoi(s + 2); *vk = v > 0 && v < 256 ? v : 0; return; }
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) if (!lstrcmpiA(KEYS[i].name, s)) { *vk = KEYS[i].vk; return; }
}

/* ---------------------------------------------------------------- the arena (the game's heap)
 * One reserved range, committed as it grows. Blocks: 8-byte header { size | in use, size of the block before }, sizes
 * in 16s, payload 16-aligned (block start = 8 mod 16), at least the CRT's round-up-to-16 of the request (some old
 * game code may write a little past what it asked for - the CRT always gave that much). Free blocks: doubly linked
 * in 128 exact bins (16 .. 2032) and log bins above (4 per power of two); merged with free neighbours; a free block
 * at the end gives its space back to the top. Everything (bins, top) lives in the Arena header at the start. */
#define AR_MAGIC   0x5341524du
#define NB_EXACT   128
#define NBINS      (NB_EXACT + 84)
#define AR_COMMIT  (1u << 20)
typedef struct Blk { uint32_t size, prev; struct Blk *nf, *pf; } Blk;   /* nf / pf only while free */
typedef struct {
    uint32_t magic, top, last, committed, reserved;
    uint32_t used, blocks, peak, full;
    uint32_t map[(NBINS + 31) / 32];
    Blk *bin[NBINS];
} Arena;
static Arena *A;
static CRITICAL_SECTION g_acs;
#define AB         ((BYTE *)A)
#define BSIZE(b)   ((b)->size & ~15u)
#define INUSE(b)   ((b)->size & 1u)
#define FIRST_OFF  ((((uint32_t)sizeof(Arena) + 15) & ~15u) + 8)

static int bin_of(uint32_t sz)
{
    if (sz < NB_EXACT * 16) return (int)(sz >> 4);
    int lg = 31 - __builtin_clz(sz);
    int b = NB_EXACT + (lg - 11) * 4 + (int)((sz >> (lg - 2)) & 3);
    return b < NBINS ? b : NBINS - 1;
}
static void bin_put(Blk *b)
{
    int i = bin_of(BSIZE(b));
    b->pf = NULL; b->nf = A->bin[i];
    if (b->nf) b->nf->pf = b;
    A->bin[i] = b;
    A->map[i >> 5] |= 1u << (i & 31);
}
static void bin_take(Blk *b)
{
    int i = bin_of(BSIZE(b));
    if (b->pf) b->pf->nf = b->nf; else A->bin[i] = b->nf;
    if (b->nf) b->nf->pf = b->pf;
    if (!A->bin[i]) A->map[i >> 5] &= ~(1u << (i & 31));
}
static int ar_commit(uint32_t upto)
{
    if (upto <= A->committed) return 1;
    if (upto > A->reserved) return 0;
    uint32_t want = (upto + AR_COMMIT - 1) & ~(AR_COMMIT - 1);
    if (want > A->reserved) want = A->reserved;
    if (!VirtualAlloc(AB + A->committed, want - A->committed, MEM_COMMIT, PAGE_READWRITE)) return 0;
    A->committed = want;
    return 1;
}
static uint32_t need_of(size_t n) { return (uint32_t)(((n ? n : 1) + 15) & ~(size_t)15) + 16; }
static int in_arena(const void *p) { return A && (const BYTE *)p >= AB + FIRST_OFF && (const BYTE *)p < AB + A->reserved; }

static void *ar_alloc(size_t n)                    /* g_acs held */
{
    if (n > 0x40000000) return NULL;
    uint32_t sz = need_of(n);
    int i = bin_of(sz);
    Blk *b = NULL;
    if (i >= NB_EXACT) { for (Blk *f = A->bin[i]; f; f = f->nf) if (BSIZE(f) >= sz) { b = f; break; } }
    else b = A->bin[i];
    if (!b) {
        for (int j = i + 1; j < NBINS; j++) {
            uint32_t m = A->map[j >> 5] >> (j & 31);
            if (!m) { j = (j | 31); continue; }
            j += __builtin_ctz(m);
            if (j < NBINS) b = A->bin[j];
            break;
        }
    }
    if (b) {
        bin_take(b);
        uint32_t have = BSIZE(b);
        if (have - sz >= 32) {
            Blk *r = (Blk *)((BYTE *)b + sz);
            r->size = have - sz; r->prev = sz;
            ((Blk *)((BYTE *)r + BSIZE(r)))->prev = BSIZE(r);     /* a free block never ends at the top */
            bin_put(r);
            b->size = sz | 1;
        } else b->size = have | 1;
    } else {
        if (!ar_commit(A->top + sz)) { A->full++; return NULL; }
        b = (Blk *)(AB + A->top);
        b->size = sz | 1; b->prev = A->last;
        A->last = sz; A->top += sz;
        if (A->top > A->peak) A->peak = A->top;
    }
    A->used += BSIZE(b); A->blocks++;
    return (BYTE *)b + 8;
}

static void ar_free(void *p)                       /* g_acs held */
{
    Blk *b = (Blk *)((BYTE *)p - 8);
    if ((BYTE *)p >= AB + A->top || !INUSE(b) || BSIZE(b) < 16) { mlog("savestate: heap: free of a block not in use (%p) - ignored", p); return; }
    uint32_t sz = BSIZE(b);
    A->used -= sz; A->blocks--;
    Blk *n = (Blk *)((BYTE *)b + sz);
    if ((BYTE *)n < AB + A->top && !INUSE(n)) { bin_take(n); sz += BSIZE(n); }
    if (b->prev) {
        Blk *pv = (Blk *)((BYTE *)b - b->prev);
        if (!INUSE(pv)) { bin_take(pv); sz += BSIZE(pv); b = pv; }
    }
    if ((BYTE *)b + sz == AB + A->top) { A->top = (uint32_t)((BYTE *)b - AB); A->last = b->prev; return; }
    b->size = sz;
    ((Blk *)((BYTE *)b + sz))->prev = sz;
    bin_put(b);
}

static void *ar_resize(void *p, size_t n, int may_move)   /* g_acs held; NULL = could not */
{
    Blk *b = (Blk *)((BYTE *)p - 8);
    uint32_t cur = BSIZE(b), sz = need_of(n);
    if (sz <= cur) return p;
    BYTE *end = (BYTE *)b + cur;
    if (end == AB + A->top) {                       /* the last block: grow into the top */
        if (ar_commit(A->top + (sz - cur))) {
            A->top += sz - cur; A->last = sz; A->used += sz - cur;
            if (A->top > A->peak) A->peak = A->top;
            b->size = sz | 1;
            return p;
        }
    } else {
        Blk *nx = (Blk *)end;
        if (!INUSE(nx) && cur + BSIZE(nx) >= sz) {
            bin_take(nx);
            uint32_t tot = cur + BSIZE(nx);
            if (tot - sz >= 32) {
                Blk *r = (Blk *)((BYTE *)b + sz);
                r->size = tot - sz; r->prev = sz;
                ((Blk *)((BYTE *)r + BSIZE(r)))->prev = BSIZE(r);
                bin_put(r);
                b->size = sz | 1;
            } else {
                b->size = tot | 1;
                ((Blk *)((BYTE *)b + tot))->prev = tot;
            }
            A->used += BSIZE(b) - cur;
            return p;
        }
    }
    if (!may_move) return NULL;
    void *q = ar_alloc(n);
    if (!q) return NULL;
    memcpy(q, p, cur - 8);
    ar_free(p);
    return q;
}

/* ---- the exe's CRT heap functions, rewritten (cdecl; the return address tells the caller) */
static void *keep_alloc(size_t n, DWORD flags)
{
    HANDLE h = CRT_HEAP ? CRT_HEAP : GetProcessHeap();
    return HeapAlloc(h, flags, (n + 15) & ~(size_t)15 ? (n + 15) & ~(size_t)15 : 16);
}
static void *alloc_for(uintptr_t ra, size_t n, int zero)
{
    if (!keep_caller(ra)) {
        EnterCriticalSection(&g_acs);
        void *p = ar_alloc(n);
        LeaveCriticalSection(&g_acs);
        if (p) { if (zero) memset(p, 0, n); return p; }
        static int told;
        if (!told++) mlog("savestate: the game heap is FULL (%u MB) - quicksave off for this session", C.heap_mb);
    }
    return keep_alloc(n, zero ? HEAP_ZERO_MEMORY : 0);
}
static void *__cdecl crt_malloc(size_t n) { return alloc_for((uintptr_t)__builtin_return_address(0), n, 0); }
static void *__cdecl crt_calloc(size_t num, size_t size)
{
    unsigned long long t = (unsigned long long)num * size;
    return t > 0xffffffe0u ? NULL : alloc_for((uintptr_t)__builtin_return_address(0), (size_t)t, 1);
}
static void __cdecl crt_free(void *p)
{
    if (!p) return;
    if (in_arena(p)) { EnterCriticalSection(&g_acs); ar_free(p); LeaveCriticalSection(&g_acs); return; }
    HeapFree(CRT_HEAP ? CRT_HEAP : GetProcessHeap(), 0, p);
}
static void *__cdecl crt_realloc(void *p, size_t n)
{
    if (!p) return alloc_for((uintptr_t)__builtin_return_address(0), n, 0);
    if (!n) { crt_free(p); return NULL; }
    if (in_arena(p)) {
        EnterCriticalSection(&g_acs);
        void *q = ar_resize(p, n, 1);
        LeaveCriticalSection(&g_acs);
        return q;
    }
    return HeapReAlloc(CRT_HEAP, 0, p, (n + 15) & ~(size_t)15);
}
static void *__cdecl crt_expand(void *p, size_t n)
{
    if (!p || n > 0xffffffe0u) return NULL;
    if (in_arena(p)) {
        EnterCriticalSection(&g_acs);
        void *q = ar_resize(p, n, 0);
        LeaveCriticalSection(&g_acs);
        return q;
    }
    return HeapReAlloc(CRT_HEAP, HEAP_REALLOC_IN_PLACE_ONLY, p, n ? (n + 15) & ~(size_t)15 : 16);
}
static size_t __cdecl crt_msize(void *p)
{
    if (in_arena(p)) return BSIZE((Blk *)((BYTE *)p - 8)) - 8;
    return HeapSize(CRT_HEAP, 0, p);
}

/* ---------------------------------------------------------------- mod state regions */
#define MAX_REGIONS 96
static struct { const char *name; void *p; size_t n; } g_reg[MAX_REGIONS];
static int g_nreg;
static size_t g_reg_bytes;
void savestate_region(const char *name, void *p, size_t n)
{
    if (g_nreg >= MAX_REGIONS) { mlog("savestate: too many regions (%s not saved)", name); return; }
    g_reg[g_nreg].name = name; g_reg[g_nreg].p = p; g_reg[g_nreg].n = n;
    g_nreg++; g_reg_bytes += n;
}

/* ---------------------------------------------------------------- slots */
typedef struct {
    int used;
    unsigned run; int scen, mission;
    DWORD tick;                     /* game clock (our GetTickCount) at the save */
    int game_ms;                    /* gpGame +0x44, for the log */
    HANDLE file;                    /* temp file (deleted on close): .data + .bss, the arena's used part, the mod regions */
    uint32_t heap_n;                /* the arena's used part */
    unsigned surf_made, surf_gone;  /* surface.c counters at the save */
    SurfRef *surf; int nsurf;       /* surfaces alive at the save + their reference counts (surface.c zombies) */
    void *primary;                  /* the primary surface then (a display reset since = no load) */
} Slot;
#define MAX_SLOTS 9
static Slot g_slot[MAX_SLOTS];
static void scan_surfaces(Slot *s);
static int g_cur;                   /* active slot */
static uintptr_t g_data_lo, g_data_hi;   /* the exe's writable data */
static int g_ready;                 /* arena + hooks installed */
static volatile int g_req = -1;     /* key pressed (K_*), done at the next safe point */
static char g_banner[64]; static DWORD g_banner_at;

static int ar_check(const char *when);
static void scan_foreign(const BYTE *exe, uint32_t exe_n, const BYTE *heap, uint32_t heap_n);

static void banner(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_banner, sizeof g_banner, fmt, ap);
    va_end(ap);
    g_banner[sizeof g_banner - 1] = 0;
    g_banner_at = GetTickCount();
    mlog("savestate: \"%s\"", g_banner);
}
const char *savestate_banner(void)
{
    if (!g_banner[0] || GetTickCount() - g_banner_at > 2500) return NULL;
    return g_banner;
}

static void slot_drop(Slot *s)
{
    if (s->file && s->file != INVALID_HANDLE_VALUE) CloseHandle(s->file);
    free(s->surf);
    memset(s, 0, sizeof *s);
}
/* zombie surfaces (surface.c) are kept while any slot is in use, and only those a slot lists */
static int surf_needed(Surf *x)
{
    for (int k = 0; k < MAX_SLOTS; k++)
        for (int i = 0; g_slot[k].used && i < g_slot[k].nsurf; i++) if (g_slot[k].surf[i].s == x) return 1;
    return 0;
}
static void surf_upkeep(void)
{
    int any = 0;
    for (int k = 0; k < MAX_SLOTS; k++) any |= g_slot[k].used;
    surf_keep_released(any);
    int n = surf_purge(surf_needed);
    if (n) mlog("savestate: %d released surface(s) no quicksave needs destroyed", n);
}

/* Snapshots go to temp files (2026-11-21): ~14 MB per slot in memory pushed the HD mode (Wine, 32-bit address space
 * near its end with the HD atlas) into std::bad_alloc; a file costs no address space and the OS keeps it cached. */
static HANDLE slot_file(void)
{
    char dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(sizeof dir, dir) || !GetTempFileNameA(dir, "amq", 0, path)) return INVALID_HANDLE_VALUE;
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
}
static int put(HANDLE h, const void *p, size_t n) { DWORD w; return WriteFile(h, p, (DWORD)n, &w, NULL) && w == n; }
static int get(HANDLE h, void *p, size_t n) { DWORD r; return ReadFile(h, p, (DWORD)n, &r, NULL) && r == n; }

static void log_sarge(const char *when)
{
    BYTE *u = SARGE;
    if (u) mlog("savestate: %s: Sarge at %d,%d health %d, game clock %d ms, heap %u blocks / %u KB", when, *(short *)(u + 4), *(short *)(u + 6),
                SARGE_HEALTH(u), GAME ? *(int *)(GAME + 0x44) : -1, A->blocks, A->used >> 10);
}

static const char *why_not(int saving)
{
    BYTE *g = GAME;
    if (!g_ready) return "restart the game first";   /* switched on in the F10 panel: the heap is chosen at start-up */
    if (!g_cfg.quicksave) return "switched off";
    if (A->full) return "game heap full - quicksave off";
    if (!g || !game_in_play()) return "not in a mission";
    if (*(DWORD *)(g + 8) & 8 || game_menu_up()) return "not while paused";
    if (saving && (!SARGE || SARGE_HEALTH(SARGE) <= 0)) return "Sarge is down";
    return NULL;
}

static void do_save(void)
{
    const char *no = why_not(1);
    if (no) { banner("CANNOT SAVE: %s", no); return; }
    Slot *s = &g_slot[g_cur];
    slot_drop(s);
    s->run = gameplay_run(&s->scen, &s->mission);
    s->tick = clock_game_ms();
    s->game_ms = *(int *)(GAME + 0x44);
    s->surf_made = surf_count_made; s->surf_gone = surf_count_gone;
    LARGE_INTEGER t0, t1, f; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&f);
    EnterCriticalSection(&g_acs);
    s->heap_n = A->top;
    s->file = slot_file();
    int ok = s->file != INVALID_HANDLE_VALUE && put(s->file, (void *)g_data_lo, g_data_hi - g_data_lo) && put(s->file, AB, s->heap_n);
    LeaveCriticalSection(&g_acs);
    for (int i = 0; ok && i < g_nreg; i++) ok = put(s->file, g_reg[i].p, g_reg[i].n);
    if (!ok) { slot_drop(s); banner("CANNOT SAVE: no temp file"); return; }
    s->surf = malloc(sizeof *s->surf * 4096);
    s->nsurf = s->surf ? surf_snapshot(s->surf, 4096) : 0;
    s->primary = g_dd ? (void *)g_dd->primary : NULL;
    s->used = 1;
    surf_upkeep();
    QueryPerformanceCounter(&t1);
    mlog("savestate: slot %d saved - run %u scenario %d mission %d, game clock %d ms, tick %lu; exe data %u KB, heap %u KB "
         "(%u blocks, %u KB in use, peak %u KB), mod %u bytes in %d regions; %.1f ms",
         g_cur + 1, s->run, s->scen, s->mission, s->game_ms, s->tick, (unsigned)((g_data_hi - g_data_lo) >> 10), s->heap_n >> 10,
         A->blocks, A->used >> 10, A->peak >> 10, (unsigned)g_reg_bytes, g_nreg, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    ar_check("at the save");
    if (C.scan) scan_foreign((const BYTE *)g_data_lo, (uint32_t)(g_data_hi - g_data_lo), AB, s->heap_n);
    if (C.scan) scan_surfaces(s);
    log_sarge("at the save");
    banner("QUICKSAVE %d SAVED", g_cur + 1);
}

/* The arena walked block by block: sizes chain up to the top, every "size of the block before" matches, no two free
 * blocks side by side, the free blocks found = the blocks in the bins. Run after every save and load (~13 000 blocks,
 * well under a millisecond); a failure is logged (and quicksave switched off: something wrote outside its block). */
static int ar_check(const char *when)
{
    uint32_t off = FIRST_OFF, prev = 0, nfree = 0, nused = 0, inuse_bytes = 0, binned = 0;
    int prev_free = 0;
    const char *bad = NULL;
    while (off < A->top) {
        Blk *b = (Blk *)(AB + off);
        uint32_t sz = BSIZE(b);
        if (sz < 16 || off + sz > A->top) { bad = "block size"; break; }
        if (b->prev != prev) { bad = "size of the block before"; break; }
        if (INUSE(b)) { nused++; inuse_bytes += sz; prev_free = 0; }
        else { if (prev_free) { bad = "two free blocks side by side"; break; } nfree++; prev_free = 1; }
        prev = sz; off += sz;
    }
    if (!bad && off != A->top) bad = "blocks do not end at the top";
    if (!bad && prev != A->last && A->top != FIRST_OFF) bad = "last block size";
    if (!bad && prev_free) bad = "free block at the top";
    for (int i = 0; !bad && i < NBINS; i++)
        for (Blk *f = A->bin[i]; f; f = f->nf) {
            if ((BYTE *)f < AB + FIRST_OFF || (BYTE *)f >= AB + A->top || INUSE(f) || bin_of(BSIZE(f)) != i) { bad = "bin list"; break; }
            if (++binned > nfree) { bad = "bin list loops"; break; }
        }
    if (!bad && binned != nfree) bad = "free blocks missing from the bins";
    if (!bad && (nused != A->blocks || inuse_bytes != A->used)) bad = "block count";
    if (bad) { mlog("savestate: HEAP CHECK FAILED %s: %s at offset %#x (top %#x) - quicksave off", when, bad, off, A->top); A->full = 1; return 0; }
    mlog("savestate: heap check %s ok: %u blocks in use (%u KB), %u free", when, nused, inuse_bytes >> 10, nfree);
    return 1;
}

/* [savestate] scan=1 (research, log): dwords of the snapshot that point at memory outside the exe image and the
 * arena - the reason the snapshot cannot be written to disk and loaded in another process (see the header). Grouped
 * by what they point into. Values below 0x10000 and odd values are skipped (numbers, not pointers). */
static void scan_foreign(const BYTE *exe, uint32_t exe_n, const BYTE *heap, uint32_t heap_n)
{
    struct { void *base; char what[64]; unsigned n, in_exe, in_heap; uintptr_t first_at; } grp[48];
    int ng = 0;
    for (int part = 0; part < 2; part++) {
        const BYTE *src = part ? heap : exe; uint32_t n = part ? heap_n : exe_n;
        uintptr_t live = part ? (uintptr_t)AB : g_data_lo;
        for (uint32_t i = 0; i + 4 <= n; i += 4) {
            uintptr_t v = *(const uint32_t *)(src + i);
            if (v < 0x10000 || (v & 1) || (v >= 0x400000 && v < 0x6ba000) || (v >= (uintptr_t)AB && v < (uintptr_t)AB + A->reserved)) continue;
            if (part == 0) {                        /* KEEP ranges are never restored: their pointers do not count */
                int kept = 0;
                for (int k = 0; k < NKEEP; k++) if (live + i >= KEEP[k].lo && live + i < KEEP[k].hi) kept = 1;
                if (kept) continue;
            }
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)v, &mi, sizeof mi) || mi.State != MEM_COMMIT) continue;
            int g;
            for (g = 0; g < ng; g++) if (grp[g].base == mi.AllocationBase) break;
            if (g == ng) {
                if (ng == 48) continue;
                grp[g].base = mi.AllocationBase; grp[g].n = grp[g].in_exe = grp[g].in_heap = 0; grp[g].first_at = live + i;
                char mod[MAX_PATH] = "";
                if (mi.Type == MEM_IMAGE && GetModuleFileNameA((HMODULE)mi.AllocationBase, mod, sizeof mod)) {
                    const char *s = strrchr(mod, '\\'); _snprintf(grp[g].what, sizeof grp[g].what, "image %s", s ? s + 1 : mod);
                } else _snprintf(grp[g].what, sizeof grp[g].what, "%s %s", mi.Type == MEM_MAPPED ? "mapped" : "private",
                                 mi.AllocationBase == GetProcessHeap() ? "(process heap)" : "");
                grp[g].what[sizeof grp[g].what - 1] = 0;
                ng++;
            }
            grp[g].n++; if (part) grp[g].in_heap++; else grp[g].in_exe++;
        }
    }
    mlog("savestate: scan: %d foreign memory regions referenced by the snapshot", ng);
    for (int g = 0; g < ng; g++)
        mlog("savestate: scan:   %p %-34s %5u dwords (%u in exe data, %u in the heap; first at %#x)", grp[g].base, grp[g].what,
             grp[g].n, grp[g].in_exe, grp[g].in_heap, (unsigned)grp[g].first_at);
}

/* [savestate] scan=1: how many dwords of the snapshot are pointers to OUR surface objects (exact matches) */
static int cmp_ptr(const void *a, const void *b) { uintptr_t x = (uintptr_t)((const SurfRef *)a)->s, y = (uintptr_t)((const SurfRef *)b)->s; return x < y ? -1 : x > y; }
static void scan_surfaces(Slot *s)
{
    SurfRef *l = malloc(sizeof *l * (s->nsurf + 1));
    if (!l) return;
    memcpy(l, s->surf, sizeof *l * s->nsurf);
    qsort(l, s->nsurf, sizeof *l, cmp_ptr);
    unsigned in_exe = 0, in_heap = 0;
    for (int part = 0; part < 2; part++) {
        const BYTE *src = part ? AB : (const BYTE *)g_data_lo; uint32_t n = part ? s->heap_n : (uint32_t)(g_data_hi - g_data_lo);
        for (uint32_t i = 0; i + 4 <= n; i += 4) {
            SurfRef key = { (Surf *)(uintptr_t)*(const uint32_t *)(src + i), 0 };
            if (bsearch(&key, l, s->nsurf, sizeof *l, cmp_ptr)) { if (part) in_heap++; else in_exe++; }
        }
    }
    free(l);
    mlog("savestate: scan: %d surfaces alive; the snapshot holds %u pointers to them (%u in exe data, %u in the heap)", s->nsurf, in_exe + in_heap, in_exe, in_heap);
}

static void stop_effects(void)
{
    if (!SFX_ON || !SFX_TABLE || SFX_TABLE == (BYTE *)-1) return;
    int n = 0;
    for (unsigned i = 0; i < SFX_COUNT && i < 4096; i++) {
        int h = *(int *)(SFX_TABLE + i * SFX_SIZE + 4);
        if (h != -1) { SFX_STOP(h); n++; }
    }
    if (n) mlog("savestate: %d sound effect(s) stopped", n);
}

/* copy the snapshot's exe data back, skipping KEEP[] */
static void restore_exe(const BYTE *src)
{
    uintptr_t a = g_data_lo;
    while (a < g_data_hi) {
        uintptr_t stop = g_data_hi, next = g_data_hi;
        for (int k = 0; k < NKEEP; k++) {
            if (KEEP[k].hi <= a || KEEP[k].lo >= stop) continue;
            if (KEEP[k].lo <= a) { stop = a; next = KEEP[k].hi; break; }
            stop = KEEP[k].lo; next = KEEP[k].hi;
        }
        if (stop > a) memcpy((void *)a, src + (a - g_data_lo), stop - a);
        if (next <= a) break;
        a = next;
    }
}

static void do_load(void)
{
    Slot *s = &g_slot[g_cur];
    const char *no = why_not(0);
    if (no) { banner("CANNOT LOAD: %s", no); return; }
    int scen, mission;
    unsigned run = gameplay_run(&scen, &mission);
    if (s->used && s->run != run) {
        mlog("savestate: slot %d is of mission run %u (scenario %d mission %d), now run %u - dropped", g_cur + 1, s->run, s->scen, s->mission, run);
        slot_drop(s);
        surf_upkeep();
    }
    if (!s->used) { banner("QUICKSAVE %d IS EMPTY", g_cur + 1); return; }
    if (g_dd && s->primary != (void *)g_dd->primary) { banner("CANNOT LOAD: the display was reset since"); slot_drop(s); surf_upkeep(); return; }
    LARGE_INTEGER t0, t1, f; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&f);
    log_sarge("before the load");
    BYTE *exe = malloc(g_data_hi - g_data_lo);
    LARGE_INTEGER z = { 0 };
    if (!exe || !SetFilePointerEx(s->file, z, NULL, FILE_BEGIN) || !get(s->file, exe, g_data_hi - g_data_lo)) { free(exe); banner("CANNOT LOAD: temp file"); return; }
    stop_effects();
    present_lock();                                 /* the display thread reads mod state (overlays) */
    EnterCriticalSection(&g_acs);
    if (!ar_commit(s->heap_n)) { LeaveCriticalSection(&g_acs); present_unlock(); free(exe); banner("CANNOT LOAD: heap"); return; }
    /* the heap is read straight into place; the exe data through a buffer (KEEP ranges are skipped) */
    restore_exe(exe);
    int ok = get(s->file, AB, s->heap_n);
    LeaveCriticalSection(&g_acs);
    for (int i = 0; ok && i < g_nreg; i++) ok = get(s->file, g_reg[i].p, g_reg[i].n);
    free(exe);
    if (!ok) mlog("savestate: READ ERROR in the middle of a load - the game state is mixed");
    int back = surf_restore_refs(s->surf, s->nsurf), newer = surf_count_not_in(s->surf, s->nsurf);
    present_unlock();
    clock_restore(s->tick);
    gameplay_run_resync();
    controls_reset();
    FIELD_REDRAW = 1;
    QueryPerformanceCounter(&t1);
    mlog("savestate: slot %d loaded - game clock back to %d ms (tick %lu), heap %u KB; %.1f ms",
         g_cur + 1, s->game_ms, s->tick, s->heap_n >> 10, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    if (back || newer) mlog("savestate: surfaces: %d released since the save are back, %d made since are no longer used by the game (kept)", back, newer);
    surf_upkeep();
    ar_check("after the load");
    log_sarge("after the load");
    banner("QUICKSAVE %d LOADED", g_cur + 1);
}

/* call 0x40150d: the game's idle loop, between frames */
static void __cdecl frame_call(void)
{
    int r = g_req;
    if (r >= 0) {
        g_req = -1;
        Guard gd;
        if (GUARD_TRY(gd, "savestate")) {
            if (r == K_SAVE) do_save();
            else if (r == K_LOAD) do_load();
            else if (r == K_SLOT) {
                g_cur = (g_cur + 1) % C.slots;
                Slot *s = &g_slot[g_cur];
                if (s->used && s->run != gameplay_run(NULL, NULL)) { slot_drop(s); surf_upkeep(); }
                if (s->used) banner("QUICKSAVE SLOT %d (game clock %d:%02d)", g_cur + 1, s->game_ms / 60000, s->game_ms / 1000 % 60);
                else banner("QUICKSAVE SLOT %d (empty)", g_cur + 1);
            }
            guard_leave(&gd);
        }
    }
    ((void (__cdecl *)(void))FRAME_FN)();
}

/* window hook (ddraw.c): the keys */
int savestate_message(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!g_cfg.quicksave || (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) || (lp & (1 << 30))) return 0;
    int mods = (GetKeyState(VK_CONTROL) < 0 ? M_CTRL : 0) | (GetKeyState(VK_SHIFT) < 0 ? M_SHIFT : 0) | (GetKeyState(VK_MENU) < 0 ? M_ALT : 0);
    for (int k = 0; k < 3; k++) {
        if (!C.key[k].vk || (int)wp != C.key[k].vk || mods != C.key[k].mods) continue;
        if (nav_key((int)wp) && !(lp & (1 << 24))) continue;      /* the number pad with Num Lock off */
        if (k == K_SLOT && C.slots < 2) return 0;
        g_req = k;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- install (DllMain, before the exe's CRT starts) */
void savestate_install(void)
{
    const char *ini = mod_ini_path();
    char b[64];
    C.enabled = GetPrivateProfileIntA("savestate", "enabled", 1, ini);
    g_cfg.quicksave = C.enabled;
    C.slots = GetPrivateProfileIntA("savestate", "slots", 3, ini);
    if (C.slots < 1) C.slots = 1;
    if (C.slots > MAX_SLOTS) C.slots = MAX_SLOTS;
    C.heap_mb = GetPrivateProfileIntA("savestate", "heap_mb", 64, ini);
    C.scan = GetPrivateProfileIntA("savestate", "scan", 0, ini);
    if (C.heap_mb < 32) C.heap_mb = 32;
    if (C.heap_mb > 1024) C.heap_mb = 1024;
    static const char *const NAME[3] = { "save_key", "load_key", "slot_key" }, *const DEF[3] = { "Insert", "Home", "End" };
    for (int k = 0; k < 3; k++) { GetPrivateProfileStringA("savestate", NAME[k], DEF[k], b, sizeof b, ini); parse_key(b, &C.key[k].vk, &C.key[k].mods); }
    if (!C.enabled) { mlog("savestate: off ([savestate] enabled=0) - the game keeps its own heap"); return; }

    /* the exe's writable data: .data incl. .bss (VirtualSize) */
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".data", 6)) { g_data_lo = (uintptr_t)base + sec[i].VirtualAddress; g_data_hi = g_data_lo + sec[i].Misc.VirtualSize; }
    if (g_data_lo != 0x4ab000 || g_data_hi < 0x6a9000 || g_data_hi > 0x6ae000) { mlog("savestate: unexpected exe data section %#x..%#x - off", (unsigned)g_data_lo, (unsigned)g_data_hi); return; }
    g_data_hi = (g_data_hi + 0xfff) & ~(uintptr_t)0xfff;

    /* the arena: wherever Windows puts it (contiguous, reserved only; committed as the game grows) */
    size_t res = (size_t)C.heap_mb << 20;
    void *p = VirtualAlloc(NULL, res, MEM_RESERVE, PAGE_READWRITE);   /* (a fixed 0x20000000 split Wine's big free range: HD mode ran out) */
    while (!p && res > (32u << 20)) { res /= 2; p = VirtualAlloc(NULL, res, MEM_RESERVE, PAGE_READWRITE); }
    if (!p || !VirtualAlloc(p, AR_COMMIT, MEM_COMMIT, PAGE_READWRITE)) { mlog("savestate: no address space for the game heap - off"); return; }
    A = p;
    memset(A, 0, sizeof *A);
    A->magic = AR_MAGIC; A->reserved = (uint32_t)res; A->committed = AR_COMMIT;
    A->top = FIRST_OFF; A->peak = A->top; A->last = 0;
    InitializeCriticalSection(&g_acs);

    static const BYTE malloc_o[] = { 0xa1,0x7c,0xba,0x6a,0x00, 0x8b,0x4c,0x24,0x04 };   /* mov eax,[6aba7c]; mov ecx,[esp+4] */
    static const BYTE free_o[] = { 0x51, 0x56, 0x8b,0x74,0x24,0x0c };                   /* push ecx; push esi; mov esi,[esp+0c] */
    static const BYTE realloc_o[] = { 0x83,0xec,0x08, 0x53, 0x55, 0x56 };               /* sub esp,8; push ebx/ebp/esi */
    static const BYTE calloc_o[] = { 0x53, 0x8b,0x5c,0x24,0x0c };                       /* push ebx; mov ebx,[esp+0c] */
    static const BYTE expand_o[] = { 0x51, 0x8b,0x44,0x24,0x0c };                       /* push ecx; mov eax,[esp+0c] */
    static const BYTE msize_o[] = { 0x83,0xec,0x08, 0x56, 0x6a,0x09 };                  /* sub esp,8; push esi; push 9 */
    static const struct { const char *name; uintptr_t va; const BYTE *o; size_t n; void *fn; } H[] = {
        { "savestate: malloc (0x4708e0) -> game heap", 0x4708e0, malloc_o, sizeof malloc_o, (void *)crt_malloc },
        { "savestate: free (0x470870) -> game heap", 0x470870, free_o, sizeof free_o, (void *)crt_free },
        { "savestate: realloc (0x471130) -> game heap", 0x471130, realloc_o, sizeof realloc_o, (void *)crt_realloc },
        { "savestate: calloc (0x47cd60) -> game heap", 0x47cd60, calloc_o, sizeof calloc_o, (void *)crt_calloc },
        { "savestate: _expand (0x473e80) -> game heap", 0x473e80, expand_o, sizeof expand_o, (void *)crt_expand },
        { "savestate: _msize (0x473f30) -> game heap", 0x473f30, msize_o, sizeof msize_o, (void *)crt_msize },
    };
    /* all or nothing: a partly replaced heap would hand blocks of one allocator to the other */
    for (size_t i = 0; i < sizeof H / sizeof *H; i++)
        if (memcmp((const void *)H[i].va, H[i].o, H[i].n)) { mlog("savestate: %s: unexpected bytes - off", H[i].name); return; }
    const BYTE *fc = (const BYTE *)FRAME_CALL;
    if (fc[0] != 0xE8 || FRAME_CALL + 5 + *(const int32_t *)(fc + 1) != FRAME_FN) { mlog("savestate: idle loop call 0x40150d not as expected - off"); return; }
    for (size_t i = 0; i < sizeof H / sizeof *H; i++) hook_jmp(H[i].name, H[i].va, H[i].o, H[i].n, H[i].fn);
    hook_call("savestate: idle loop frame (call 0x40150d) -> safe point", FRAME_CALL, FRAME_FN, (void *)frame_call);
    g_ready = 1;
    weapons_savestate(); allies_savestate(); aware_savestate(); capture_savestate(); patrol_savestate();
    mines_savestate(); bullets_savestate(); objectives_savestate(); vehicles_savestate(); modern_savestate();
    fences_savestate(); missionpath_savestate(); controls_savestate();
    char kn[3][24];
    for (int k = 0; k < 3; k++) GetPrivateProfileStringA("savestate", NAME[k], DEF[k], kn[k], sizeof kn[k], ini);
    mlog("savestate: on - game heap at %p (%u MB reserved), exe data %#x..%#x, %d slot(s), keys save %s / load %s / slot %s, "
         "%d mod regions (%u bytes)", (void *)A, (unsigned)(res >> 20), (unsigned)g_data_lo, (unsigned)g_data_hi, C.slots, kn[0], kn[1], kn[2],
         g_nreg, (unsigned)g_reg_bytes);
}
