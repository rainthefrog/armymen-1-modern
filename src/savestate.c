/*
 * savestate.c - quicksave / quickload of the running mission (2026-11-21, [savestate]); saves are FILES that survive
 * quitting the game (<game folder>\SaveGames\quick<n>.ams, [savestate] dir).
 *
 * The original game only keeps progress BETWEEN missions (the army file, FUN_0041a3e0 write / FUN_0041a970 read).
 * This saves the whole running mission as a memory snapshot and puts it back in place:
 *
 *   1. The game heap. Everything the game allocates goes through the exe's own CRT (statically linked, VC5; MFC is
 *      statically linked too): malloc 0x4708e0, free 0x470870, realloc 0x471130, calloc 0x47cd60, _expand 0x473e80,
 *      _msize 0x473f30 (operator new 0x48bf13 calls malloc). These six are REWRITTEN here (hook_jmp, installed from
 *      DllMain, i.e. before the exe's CRT start-up made its first allocation): the game's blocks come from our own
 *      allocator in ONE reserved address range (the arena, all bookkeeping inside it), so a copy of the arena's used
 *      part is the complete heap state. Every block carries a TAG: the return address of its malloc / new call (which
 *      code made it). Callers that hold LIVE outside state keep the CRT's Win32 heap ("keep heap", never saved or
 *      restored): the CRT itself (0x470000..0x48bf00) except _strdup / _fullpath, the sound module 0x40f1c0..0x412840
 *      and the radio voice module 0x4688a0..0x46a6e0. free / realloc / _msize decide by the block's address.
 *   2. The exe's writable data (.data + .bss, 0x4ab000 .. end of .data, ~2 MB), except the ranges in KEEP[] that
 *      describe live outside resources (CRT / MFC state, the sound and voice modules' globals).
 *   3. The mod's own per-mission state (each file registers its variables: savestate_region, the *_savestate functions).
 *   4. The game clock: our GetTickCount (clock.c) is set to the value it had at the save, so every time stamp in the
 *      restored state is in the past by the same amount as at the save.
 *   5. Our DirectDraw surfaces (descriptions + pixels, PackBits), palettes, clippers, and the saving process's address
 *      space map, so that a NEW process can map them (below).
 *
 * Loading - two paths:
 *   - the same mission run of the same process (the old in-session quickload): the snapshot is copied back; every
 *     pointer in it is still valid (surfaces the game released since the save are kept alive: surface.c zombies).
 *   - anything else (another run of the mission, or the game was quit and started again): "load from the file", the
 *     RELOCATION TABLE design (load_file): the save's scenario must be running - from the campaign select screen the
 *     load key starts it (missionpath_request_scenario: its FIRST part, the map of every part; later parts cannot
 *     start cold) and the load follows once it is in play. Then the snapshot replaces the state and what in it
 *     belongs to the saving process is mapped onto this one:
 *       * our objects by identity: each saved surface gets the screen / back buffer (role), a fresh surface with the
 *         same picture (same size, caps, colour key, pixel hash - the mission start loaded it again), one of the same
 *         kind in creation order (the save's pixels copied in), or a NEW one with the save's pixels (pictures that
 *         appeared after the mission start: spawned units, air strikes, radio, on-demand sprites - always
 *         recreatable, the save has their pixels). The DirectDraw object (both interfaces), the screen palette (its
 *         entries of then put back), clippers. Every dword equal to an old object address is set to the new one -
 *         exact matches only (KNOWN_PTR lists where they were found; others are patched and logged). The one
 *         pointer INTO pixel memory the game keeps (BITS_PTR) follows its surface.
 *       * the process's own resources keep the values of now: OWN[] (window / GDI / kernel handles, DirectInput,
 *         fonts, threads, the app object), exe data our patches wrote (savestate_note_patch: the screen state table
 *         holds pointers into this dll), exe dwords that pointed into the saving process's keep heap (by its saved
 *         address-space map), and the "own" heap blocks: made by MFC or the main window object (tags, own_tag) - made
 *         at start-up, at the same place in every run (checked: the load is refused when the save has other blocks
 *         there). Pointers into our dll in the mod regions move with the dll (it is relocatable).
 *     Found by research ([savestate] scan=1 + research_autosave_ms + research_perturb, research() below): two runs
 *     saved at the same game time and a third with its outside objects shifted (other heap addresses, handle values,
 *     address space); the dwords equal in the first two but different in the third are this process's - their
 *     classes are OWN[], the own blocks and our objects. Pointers INTO pixels other than BITS_PTR are not
 *     distinguishable from picture data (millions of false hits) and none were found that the game uses.
 *   Refused with a reason (banner + log), never a partial load: another Armymen.exe (PE header hash), another mod
 *   state layout (region names + sizes), a game heap at another address ([savestate] heap_base / SaveGames\heap.base:
 *   the arena is reserved at the address of the first run), another display size, own blocks that differ, another
 *   scenario running (outside the campaign screen).
 * Safe point: save and load run in the game's idle loop (CWinApp::Run override 0x4014e0: call 0x40150d -> the frame
 * FUN_00407460), between two frames, with no game function on the stack. Before a load every playing sound effect is
 * stopped (FUN_0040f920); the ground layer is redrawn (0x4e30e4 = 1); held inputs are dropped (controls_reset).
 * Keys ([savestate] save_key / load_key / slot_key, defaults Insert / Home / End; "Ctrl+", "Shift+", "Alt+" allowed;
 * F5 / F9 are taken by the game / the CRT filter). Navigation keys only count as the dedicated keys.
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
    { 0x6a5b88, 0x6a63b8, "voice module (incl. the voice text table 0x6a5bb8: 256 x { id, string on the keep heap }, game binary FUN_0046a1d0)" },
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
    int enabled, slots, heap_mb, scan, auto_ms, perturb, newsurf;
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
/* 16-byte header (save files, 2026-11-21): size | in use, size of the block before; while free the bin links, while in
 * use the TAG = return address of the game's malloc call (for `new`: of the operator new call), i.e. which code made it */
typedef struct Blk { uint32_t size, prev; union { struct Blk *nf; uint32_t tag; }; struct Blk *pf; } Blk;
#define HDR 16
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
#define FIRST_OFF  (((uint32_t)sizeof(Arena) + 15) & ~15u)

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
static int in_arena(const void *p) { return A && (const BYTE *)p >= AB + FIRST_OFF + HDR && (const BYTE *)p < AB + A->reserved; }

static void *ar_alloc(size_t n, uint32_t tag)      /* g_acs held */
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
    b->tag = tag; b->pf = NULL;
    return (BYTE *)b + HDR;
}

static void ar_free(void *p)                       /* g_acs held */
{
    Blk *b = (Blk *)((BYTE *)p - HDR);
    if ((BYTE *)p >= AB + A->top || !INUSE(b) || BSIZE(b) < 32) { mlog("savestate: heap: free of a block not in use (%p) - ignored", p); return; }
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
    Blk *b = (Blk *)((BYTE *)p - HDR);
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
    void *q = ar_alloc(n, b->tag);
    if (!q) return NULL;
    memcpy(q, p, cur - HDR);
    ar_free(p);
    return q;
}

/* ---- the exe's CRT heap functions, rewritten (cdecl; the return address tells the caller) */
static void *keep_alloc(size_t n, DWORD flags)
{
    HANDLE h = CRT_HEAP ? CRT_HEAP : GetProcessHeap();
    return HeapAlloc(h, flags, (n + 15) & ~(size_t)15 ? (n + 15) & ~(size_t)15 : 16);
}
static void *alloc_for(uintptr_t ra, uint32_t tag, size_t n, int zero)
{
    if (!keep_caller(ra)) {
        EnterCriticalSection(&g_acs);
        void *p = ar_alloc(n, tag);
        LeaveCriticalSection(&g_acs);
        if (p) { if (zero) memset(p, 0, n); return p; }
        static int told;
        if (!told++) mlog("savestate: the game heap is FULL (%u MB) - quicksave off for this session", C.heap_mb);
    }
    return keep_alloc(n, zero ? HEAP_ZERO_MEMORY : 0);
}
/* the block's tag: the return address of the malloc call; for operator new (0x48bf13, its malloc call returns to
 * 0x48bf26 with [n, edi, esi, ebx, return address] above) the return address of the `new` */
#define OP_NEW_RET 0x48bf26u
static __attribute__((noinline)) void *__cdecl crt_malloc(size_t n)
{
    const uint32_t *f = (const uint32_t *)__builtin_frame_address(0) + 1;
    return alloc_for(f[0], f[0] == OP_NEW_RET ? f[5] : f[0], n, 0);
}
static void *__cdecl crt_calloc(size_t num, size_t size)
{
    unsigned long long t = (unsigned long long)num * size;
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    return t > 0xffffffe0u ? NULL : alloc_for(ra, (uint32_t)ra, (size_t)t, 1);
}
static void __cdecl crt_free(void *p)
{
    if (!p) return;
    if (in_arena(p)) { EnterCriticalSection(&g_acs); ar_free(p); LeaveCriticalSection(&g_acs); return; }
    HeapFree(CRT_HEAP ? CRT_HEAP : GetProcessHeap(), 0, p);
}
static void *__cdecl crt_realloc(void *p, size_t n)
{
    if (!p) { uintptr_t ra = (uintptr_t)__builtin_return_address(0); return alloc_for(ra, (uint32_t)ra, n, 0); }
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
    if (in_arena(p)) return BSIZE((Blk *)((BYTE *)p - HDR)) - HDR;
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

/* places in the exe's data our patches wrote (patches.c write_mem: e.g. the screen state table entries that point
 * into this dll): a load from a file keeps the values of this process there */
#define MAX_PATCHED 128
static struct { uintptr_t lo, hi; } g_patched[MAX_PATCHED];
static int g_npatched;
void savestate_note_patch(uintptr_t va, size_t n)
{
    if (va >= 0x4ab000 && va < 0x6ae000 && g_npatched < MAX_PATCHED) { g_patched[g_npatched].lo = va; g_patched[g_npatched].hi = va + n; g_npatched++; }
}

/* ---------------------------------------------------------------- a load from a file: what belongs to THIS process
 * Found 2026-11-21 (research, see the header): two runs of the same mission saved at the same game time, and a third
 * run whose outside objects were shifted (research_perturb: other heap addresses, handle values, address space) - the
 * dwords equal in the first two and different in the third - plus the binary's stores of CreateMutex / CreateEvent /
 * CreateThread / CreateFontIndirect / LoadCursor / DirectInputCreate results. A load from a file keeps these at the
 * values of the running process. */
static const struct { uintptr_t lo, hi; const char *what; } OWN[] = {
    { 0x4d8000, 0x4d8108, "the app object (CWinApp: main window, instance mutex, CPU-speed DLL entry points 0x401610)" },
    { 0x4e2db0, 0x4e2db4, "GDI handle of the main window (0x403ae0)" },
    { 0x4e2e60, 0x4e2e64, "menu handle" },
    { 0x4e9f48, 0x4e9f64, "fonts (FUN_00413800 -> FUN_00413d30)" },
    { 0x4fe1b8, 0x4fe1bc, "cursor (LoadCursorA)" },
    { 0x4fe1c0, 0x4fe1c4, "input window (FUN_00420200)" },
    { 0x4fe474, 0x4fe480, "DirectInput, keyboard and mouse devices (FUN_00420200)" },
    { 0x507db0, 0x507db4, "kernel handle (found by the perturbed run)" },
    { 0x6101c0, 0x6101c4, "queue mutex (FUN_00433390 -> FUN_00432750)" },
    { 0x6101d0, 0x6101d4, "queue mutex" },
    { 0x6101e0, 0x6101e8, "two events (FUN_00433390)" },
    { 0x6742e0, 0x6742e4, "queue mutex of player record 0 (FUN_00433900, 0x674268 + i * 0x7e0 + 0x78)" },
    { 0x674ac0, 0x674ac4, "queue mutex of player record 1" },
    { 0x6752a0, 0x6752a4, "queue mutex of player record 2" },
    { 0x675a80, 0x675a84, "queue mutex of player record 3" },
    { 0x676260, 0x676264, "queue mutex of player record 4" },
    { 0x676a40, 0x676a44, "queue mutex of player record 5" },
    { 0x6771a8, 0x6771b4, "window, event, queue mutex" },
    { 0x6771c0, 0x6771c4, "thread (FUN_00433390, 0x433180)" },
    { 0x67b068, 0x67b06c, "queue mutex" },
    { 0x699a98, 0x699a9c, "font (dialog FUN_0044f240)" },
    { 0x699b10, 0x699b14, "font (dialog)" },
    { 0x699b4c, 0x699b50, "font (dialog)" },
};
#define NOWN ((int)(sizeof OWN / sizeof OWN[0]))
#define BITS_PTR 0x4e2444u   /* the pixels of a surface (the frame being drawn; FUN_004040c0 ...): moved with the surface */
/* blocks of the game heap that are this process's own objects: made by MFC (handle maps with window / GDI handles:
 * CHandleMap 0x4946b2 / 0x490c80 / 0x48dbbf, their CMap hash tables 0x48a742 and CPlex blocks 0x48a6c9; the thread /
 * module state objects) and the main window object (operator new 0x118 at 0x4013db, theApp +0x1c: its window and GDI
 * handles, the palette) - NOT MFC's CString buffers (CString::AllocBuffer 0x48c5cf, tag 0x48c5ed: game data, e.g. the
 * map name gpGame +0x54) - plus the strings these objects and the app object (OWN 0x4d8000) hold: CString buffers and
 * _strdup copies (tag 0x47319c: theApp's m_pszAppName / m_pszExeName ...). All made at start-up, so a new process has
 * them at the same place: a load keeps their contents of now and refuses when the save has other blocks there.
 * (Wine 2026-11-21: a save made after the menu path and loaded after start_mission had a CString of the menus at a
 * place where the other process had another block - CStrings must not count as own by their tag alone.) */
#define TAG_CSTRING 0x48c5edu   /* CString::AllocBuffer */
#define TAG_STRDUP  0x47319cu   /* _strdup */
static int own_tag(uint32_t t) { return ((t >= 0x46dc00 && t < 0x470000) || (t >= 0x484e00 && t < 0x4a0000) || t == 0x4013e0) && t != TAG_CSTRING; }
static int cmp_u32(const void *a, const void *b);
/* the own blocks of the arena now: sorted block offsets (malloc'd, caller frees); g_acs held or the game thread */
static int own_blocks(uint32_t **out)
{
    int n = 0, cap = 256, nb = 0;
    uint32_t *o = malloc(sizeof *o * cap), *all = malloc(sizeof *all * (A->blocks + 16));
    BYTE *mark = calloc(A->blocks + 16, 1);
    *out = NULL;
    if (!o || !all || !mark) { free(o); free(all); free(mark); return -1; }
    for (uint32_t off = FIRST_OFF; off < A->top; ) {
        Blk *b = (Blk *)(AB + off); uint32_t sz = BSIZE(b);
        if (sz < 16) break;
        if (INUSE(b) && nb < (int)A->blocks + 16) { if (own_tag(b->tag)) mark[nb] = 1; all[nb++] = off; }
        off += sz;
    }
    /* the strings they (and the app object) point at - repeated while new blocks get marked (own objects point at
     * each other: handle map -> hash table -> assoc blocks) */
    for (int pass = 0, more = 1; more && pass < 8; pass++) {
        more = 0;
        for (int src = -1; src < nb; src++) {
            const uint32_t *w; uint32_t nw;
            if (src < 0) { w = (const uint32_t *)0x4d8000; nw = 0x108 / 4; }         /* the app object (OWN) */
            else if (mark[src] == 1) { Blk *b = (Blk *)(AB + all[src]); w = (const uint32_t *)((BYTE *)b + HDR); nw = (BSIZE(b) - HDR) / 4; mark[src] = 2; }
            else continue;
            for (uint32_t i = 0; i < nw; i++) {
                uint32_t v = w[i];
                if (v < (uint32_t)(uintptr_t)AB + FIRST_OFF || v >= (uint32_t)(uintptr_t)AB + A->top) continue;
                uint32_t off = v - (uint32_t)(uintptr_t)AB;
                int lo = 0, hi = nb - 1, k = -1;
                while (lo <= hi) { int m = (lo + hi) / 2; if (all[m] <= off) { k = m; lo = m + 1; } else hi = m - 1; }
                if (k < 0 || mark[k]) continue;
                Blk *t = (Blk *)(AB + all[k]);
                if (off < all[k] + HDR || off >= all[k] + BSIZE(t)) continue;
                if (t->tag == TAG_CSTRING || t->tag == TAG_STRDUP || own_tag(t->tag)) { mark[k] = 1; more = 1; }
            }
        }
    }
    for (int i = 0; i < nb; i++) if (mark[i]) {
        if (n == cap) { uint32_t *x = realloc(o, sizeof *o * (cap *= 2)); if (!x) { free(o); free(all); free(mark); return -1; } o = x; }
        o[n++] = all[i];
    }
    free(all); free(mark);
    *out = o;
    return n;
}
static int is_own(const uint32_t *own, int n, uint32_t off) { return n > 0 && bsearch(&off, own, n, sizeof *own, cmp_u32) != NULL; }
/* where the snapshot is known to hold pointers to our DirectDraw objects (research 2026-11-21, Desert 1 / mission 6);
 * other places that hold one are patched too but logged */
static const struct { uint32_t where; uint32_t off; } KNOWN_PTR[] = {   /* where < 0x1000000: exe address, else block tag */
    { 0x4e2db4, 0 }, { 0x4e2e40, 0 }, { 0x4e2e44, 0 }, { 0x4e2e48, 0 }, { 0x4e2e4c, 0 }, { 0x4e2e50, 0 }, { 0x4e2e5c, 0 },
    { 0x4e2ea4, 0 }, { 0x4fe3f8, 0 }, { 0x4fe49c, 0 }, { 0x6a4df0, 0 },
    { 0x14454d7, 0 }, { 0x14385e8, 4 },
};

/* ---------------------------------------------------------------- save files
 * <dir>\quick<n>.ams ([savestate] dir, default SaveGames in the game folder). Layout: AmsHdr, then the sections at the
 * offsets it gives: exe data (.data + .bss), the arena's used part (its blocks carry their tags), the mod regions in
 * registration order, SurfRec[nsurf], the surfaces' pixels (PackBits per surface, rows of w bytes), ObjRec[nobj]
 * (palettes, clippers), VmRec[nvm] (the saving process's address space: tells a pointer into its CRT heap). */
#define AMS_MAGIC   0x53514d41u            /* "AMQS" */
#define AMS_VERSION 1
#define ARENA_FMT   2                      /* 16-byte block headers with tags */
typedef struct {
    uint32_t magic, version, hdr_size, arena_fmt;
    uint32_t exe_id, session, run, pid;
    char build[32];
    int32_t scen, mission, game_ms; uint32_t tick;
    uint32_t data_lo, data_hi, arena_base, arena_reserved, heap_n;
    uint32_t dll_base, dll_size;
    uint32_t nreg, reg_bytes, reg_sig;
    uint32_t nsurf, nobj, nvm, pix_bytes;
    uint32_t off_exe, off_arena, off_reg, off_surf, off_pix, off_obj, off_vm, off_end;
    uint32_t dd, pal, clip;                /* our DirectDraw object, the screen palette, the screen clipper then */
    int32_t sarge_x, sarge_y, sarge_health;
    FILETIME saved_at;
    uint32_t reserved[16];
} AmsHdr;
enum { SR_PRIMARY = 1, SR_BACK = 2, SR_CK = 4 };
typedef struct { uint32_t old, serial, w, h, caps, flags, ck_lo, ck_hi; int32_t ref; uint32_t pal, clip, hash, bits, pix_n, pad[2]; } SurfRec;
enum { OB_PAL = 1, OB_CLIP = 2 };
typedef struct { uint32_t old, kind; int32_t ref; uint32_t pad; PALETTEENTRY e[256]; } ObjRec;
typedef struct { uint32_t base, size, alloc, state, type, kind; } VmRec;     /* kind: VK_* */
enum { VK_OTHER, VK_CRTHEAP, VK_PROCHEAP, VK_ARENA, VK_DLL, VK_IMAGE };

static uint32_t fnv(uint32_t h, const void *p, size_t n) { const BYTE *b = p; while (n--) { h ^= *b++; h *= 16777619u; } return h; }
static uint32_t exe_id(void) { return fnv(2166136261u, (const void *)0x400000, 0x1000); }   /* the PE headers */
static uint32_t reg_sig(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < g_nreg; i++) { h = fnv(h, g_reg[i].name, strlen(g_reg[i].name)); uint32_t n = (uint32_t)g_reg[i].n; h = fnv(h, &n, 4); }
    return h;
}
static uint32_t surf_hash(const Surf *s)
{
    uint32_t h = 2166136261u;
    for (int y = 0; y < s->h; y++) h = fnv(h, s->bits + (size_t)y * s->pitch, s->w);
    return h;
}
static void dll_range(uint32_t *base, uint32_t *size)
{
    BYTE *b = (BYTE *)g_self;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    *base = (uint32_t)(uintptr_t)b; *size = nt->OptionalHeader.SizeOfImage;
}

/* PackBits: n < 128: n+1 literal bytes follow; n >= 129: the next byte 257-n times */
static size_t pack(const BYTE *s, size_t n, BYTE *o)
{
    size_t i = 0, k = 0;
    while (i < n) {
        size_t r = 1;
        while (i + r < n && r < 128 && s[i + r] == s[i]) r++;
        if (r >= 3) { o[k++] = (BYTE)(257 - r); o[k++] = s[i]; i += r; continue; }
        size_t l = 0;
        while (i + l < n && l < 128) { if (i + l + 2 < n && s[i + l] == s[i + l + 1] && s[i + l] == s[i + l + 2]) break; l++; }
        o[k++] = (BYTE)(l - 1); memcpy(o + k, s + i, l); k += l; i += l;
    }
    return k;
}
static int unpack(const BYTE *s, size_t n, BYTE *o, size_t on)
{
    size_t i = 0, k = 0;
    while (i < n) {
        BYTE c = s[i++];
        if (c < 128) { size_t l = (size_t)c + 1; if (i + l > n || k + l > on) return 0; memcpy(o + k, s + i, l); i += l; k += l; }
        else { size_t r = 257 - (size_t)c; if (i >= n || k + r > on) return 0; memset(o + k, s[i++], r); k += r; }
    }
    return k == on;
}

/* ---------------------------------------------------------------- slots
 * The file is the save. In memory a slot only remembers what THIS process saved into it in the current mission run
 * (surfaces alive then: kept as zombies so the fast in-process load can give them back). */
typedef struct {
    int used;
    unsigned run;
    SurfRef *surf; int nsurf;       /* surfaces alive at the save + their reference counts (surface.c zombies) */
    void *primary;                  /* the primary surface then (a display reset since = the file path) */
} Slot;
#define MAX_SLOTS 9
static Slot g_slot[MAX_SLOTS];
static int g_cur;                   /* active slot */
static uintptr_t g_data_lo, g_data_hi;   /* the exe's writable data */
static int g_ready;                 /* arena + hooks installed */
static uint32_t g_session;          /* this process (random): a file of this session may take the fast path */
static volatile int g_req = -1;     /* key pressed (K_*), done at the next safe point */
static int g_pending = -1;          /* slot whose load waits for its mission (started from the campaign screen) */
static unsigned g_pending_run; static DWORD g_pending_at;
static char g_banner[96]; static DWORD g_banner_at;

static int ar_check(const char *when);
static void research(void);
static const char *save_dir(void);
static int in_keep(uintptr_t a);
static int in_own(uintptr_t a);

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
    if (!g_banner[0] || GetTickCount() - g_banner_at > 3500) return NULL;
    return g_banner;
}

static void slot_path(int k, char *out, size_t n, const char *ext) { _snprintf(out, n, "%squick%d.%s", save_dir(), k + 1, ext); out[n - 1] = 0; }
static void slot_drop(Slot *s) { free(s->surf); memset(s, 0, sizeof *s); }
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

static int put(HANDLE h, const void *p, size_t n) { DWORD w; return WriteFile(h, p, (DWORD)n, &w, NULL) && w == n; }
static int get(HANDLE h, void *p, size_t n) { DWORD r; return ReadFile(h, p, (DWORD)n, &r, NULL) && r == n; }
static int seek(HANDLE h, uint32_t off) { LARGE_INTEGER z; z.QuadPart = off; return SetFilePointerEx(h, z, NULL, FILE_BEGIN); }
static uint32_t tell(HANDLE h) { LARGE_INTEGER z = { 0 }, p; return SetFilePointerEx(h, z, &p, FILE_CURRENT) ? (uint32_t)p.QuadPart : 0; }

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

/* the address space of this process, coarse (one record per allocation): what a saved pointer pointed into */
static int vm_map(VmRec *out, int max)
{
    uint32_t dlo, dn; dll_range(&dlo, &dn);
    int n = 0; MEMORY_BASIC_INFORMATION mi; uintptr_t a = 0x10000;
    while (a < 0x7fff0000 && VirtualQuery((void *)a, &mi, sizeof mi)) {
        if (mi.State == MEM_COMMIT && n < max) {
            uint32_t ab = (uint32_t)(uintptr_t)mi.AllocationBase, k = VK_OTHER;
            if (mi.AllocationBase == CRT_HEAP) k = VK_CRTHEAP;
            else if (mi.AllocationBase == GetProcessHeap()) k = VK_PROCHEAP;
            else if ((BYTE *)mi.AllocationBase == AB) k = VK_ARENA;
            else if (ab == dlo) k = VK_DLL;
            else if (mi.Type == MEM_IMAGE) k = VK_IMAGE;
            if (n && out[n - 1].alloc == ab && out[n - 1].base + out[n - 1].size == (uint32_t)(uintptr_t)mi.BaseAddress && out[n - 1].kind == k)
                out[n - 1].size += (uint32_t)mi.RegionSize;
            else out[n++] = (VmRec){ (uint32_t)(uintptr_t)mi.BaseAddress, (uint32_t)mi.RegionSize, ab, mi.State, mi.Type, k };
        }
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    /* the CRT heap may have more segments than its first allocation: every region its HeapWalk visits */
    PROCESS_HEAP_ENTRY e; memset(&e, 0, sizeof e);
    HeapLock(CRT_HEAP);
    while (HeapWalk(CRT_HEAP, &e))
        if ((e.wFlags & PROCESS_HEAP_REGION) && n < max)
            out[n++] = (VmRec){ (uint32_t)(uintptr_t)e.lpData, (uint32_t)e.Region.dwCommittedSize + (uint32_t)e.Region.dwUnCommittedSize,
                                (uint32_t)(uintptr_t)e.lpData, MEM_COMMIT, MEM_PRIVATE, VK_CRTHEAP };
    HeapUnlock(CRT_HEAP);
    return n;
}
static int vm_kind(const VmRec *vm, int n, uint32_t v)
{
    int k = -1;
    for (int i = 0; i < n; i++) if (v >= vm[i].base && v - vm[i].base < vm[i].size) { k = (int)vm[i].kind; if (k == VK_CRTHEAP) return k; }
    return k;
}

/* A digest of the state a load must reproduce exactly (log; compare "at the save" with "after the load"): the exe
 * data outside KEEP / OWN / patched / BITS_PTR, the heap blocks outside the own blocks, the mod regions - with every
 * pointer to one of our DirectDraw objects counted as 0 (those differ by design) and the ground-redraw flag
 * FIELD_REDRAW (set by every load). Equal digests = the game state is back bit for bit. */
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static void log_digest(const char *when)
{
    uint32_t *obj = malloc(sizeof *obj * 4200); int no = 0;
    Surf **all = malloc(sizeof *all * 4096);
    VmRec *vm = malloc(sizeof *vm * 4096);
    if (!obj || !all || !vm) { free(obj); free(all); free(vm); return; }
    int na = surf_list_all(all, 4096);
    for (int i = 0; i < na; i++) { obj[no++] = (uint32_t)(uintptr_t)all[i]; if (all[i]->pal && no < 4190) obj[no++] = (uint32_t)(uintptr_t)all[i]->pal; if (all[i]->clip && no < 4190) obj[no++] = (uint32_t)(uintptr_t)all[i]->clip; }
    if (g_dd) { obj[no++] = (uint32_t)(uintptr_t)g_dd; obj[no++] = (uint32_t)(uintptr_t)g_dd + 4; if (g_dd->pal) obj[no++] = (uint32_t)(uintptr_t)g_dd->pal; }
    qsort(obj, no, sizeof *obj, cmp_u32);
    uint32_t omin = no ? obj[0] : 1, omax = no ? obj[no - 1] : 0;
#define DV(v) (((v) >= omin && (v) <= omax && bsearch(&(v), obj, no, sizeof *obj, cmp_u32)) ? 0u : (v))
    uint32_t he = 2166136261u, hh = 2166136261u, hr = 2166136261u;
    for (uintptr_t a = g_data_lo; a + 4 <= g_data_hi; a += 4) {
        if (in_keep(a) || in_own(a) || a == BITS_PTR || a == (uintptr_t)&FIELD_REDRAW) continue;   /* (set by the load) */
        uint32_t v = *(uint32_t *)a; v = DV(v); he = fnv(he, &v, 4);
    }
    EnterCriticalSection(&g_acs);
    uint32_t *own; int nown = own_blocks(&own);
    for (uint32_t off = FIRST_OFF; off < A->top; ) {
        Blk *b = (Blk *)(AB + off); uint32_t sz = BSIZE(b);
        if (sz < 16) break;
        if (INUSE(b) && !is_own(own, nown, off)) for (uint32_t i = HDR; i + 4 <= sz; i += 4) { uint32_t v = *(uint32_t *)((BYTE *)b + i); v = DV(v); hh = fnv(hh, &v, 4); }
        off += sz;
    }
    free(own);
    LeaveCriticalSection(&g_acs);
    for (int i = 0; i < g_nreg; i++) for (size_t o = 0; o + 4 <= g_reg[i].n; o += 4) { uint32_t v = *(uint32_t *)((BYTE *)g_reg[i].p + o); v = DV(v); hr = fnv(hr, &v, 4); }
    mlog("savestate: digest %s: exe %08x heap %08x mod %08x", when, he, hh, hr);
    if (C.scan) {   /* research: the exe data as the digest saw it */
        char path[MAX_PATH]; _snprintf(path, sizeof path, "%sdigest_%lu_%s.bin", save_dir(), GetCurrentProcessId(), when[0] == 'a' && when[1] == 't' ? "save" : "load"); path[sizeof path - 1] = 0;
        FILE *f = fopen(path, "wb");
        if (f) {
            for (uintptr_t a = g_data_lo; a + 4 <= g_data_hi; a += 4) {
                uint32_t v = *(uint32_t *)a;
                if (in_keep(a) || in_own(a) || a == BITS_PTR || a == (uintptr_t)&FIELD_REDRAW) v = 0; else v = DV(v);
                fwrite(&v, 4, 1, f);
            }
            fclose(f);
        }
    }
#undef DV
    free(obj); free(all); free(vm);
}

/* ---------------------------------------------------------------- save */
static void do_save(void)
{
    const char *no = why_not(1);
    if (no) { banner("CANNOT SAVE: %s", no); return; }
    Slot *s = &g_slot[g_cur];
    slot_drop(s);
    LARGE_INTEGER t0, t1, f; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&f);
    char tmp[MAX_PATH], path[MAX_PATH];
    slot_path(g_cur, tmp, sizeof tmp, "tmp"); slot_path(g_cur, path, sizeof path, "ams");
    HANDLE h = CreateFileA(tmp, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { banner("CANNOT SAVE: cannot write %s", path); mlog("savestate: CreateFile %s failed (%lu)", tmp, GetLastError()); surf_upkeep(); return; }

    AmsHdr H; memset(&H, 0, sizeof H);
    H.magic = AMS_MAGIC; H.version = AMS_VERSION; H.hdr_size = sizeof H; H.arena_fmt = ARENA_FMT;
    H.exe_id = exe_id(); H.session = g_session; H.pid = GetCurrentProcessId();
    lstrcpynA(H.build, __DATE__ " " __TIME__, sizeof H.build);
    int scen, mission;
    H.run = gameplay_run(&scen, &mission); H.scen = scen; H.mission = mission;
    H.tick = clock_game_ms(); H.game_ms = *(int *)(GAME + 0x44);
    H.data_lo = (uint32_t)g_data_lo; H.data_hi = (uint32_t)g_data_hi;
    H.arena_base = (uint32_t)(uintptr_t)AB; H.arena_reserved = A->reserved;
    dll_range(&H.dll_base, &H.dll_size);
    H.nreg = g_nreg; H.reg_bytes = (uint32_t)g_reg_bytes; H.reg_sig = reg_sig();
    H.dd = (uint32_t)(uintptr_t)g_dd;
    H.pal = g_dd ? (uint32_t)(uintptr_t)g_dd->pal : 0;
    H.clip = g_dd && g_dd->primary ? (uint32_t)(uintptr_t)g_dd->primary->clip : 0;
    if (SARGE) { H.sarge_x = *(short *)(SARGE + 4); H.sarge_y = *(short *)(SARGE + 6); H.sarge_health = SARGE_HEALTH(SARGE); }
    GetSystemTimeAsFileTime(&H.saved_at);

    int ok = put(h, &H, sizeof H);
    H.off_exe = tell(h);
    ok = ok && put(h, (void *)g_data_lo, g_data_hi - g_data_lo);
    EnterCriticalSection(&g_acs);
    H.heap_n = A->top;
    H.off_arena = tell(h);
    ok = ok && put(h, AB, H.heap_n);
    LeaveCriticalSection(&g_acs);
    H.off_reg = tell(h);
    for (int i = 0; ok && i < g_nreg; i++) ok = put(h, g_reg[i].p, g_reg[i].n);

    /* surfaces: alive ones (ref > 0) - descriptions, then the pixels */
    Surf **all = malloc(sizeof *all * 4096);
    int na = all ? surf_list_all(all, 4096) : 0, ns = 0;
    SurfRec *sr = malloc(sizeof *sr * (na + 1));
    ObjRec *orr = malloc(sizeof *orr * 16); int no_ = 0;
    if (!all || !sr || !orr) ok = 0;
    for (int i = 0; ok && i < na; i++) {
        Surf *x = all[i];
        if (x->ref <= 0) continue;
        SurfRec *r = &sr[ns++];
        memset(r, 0, sizeof *r);
        r->old = (uint32_t)(uintptr_t)x; r->serial = x->serial; r->w = x->w; r->h = x->h; r->caps = x->caps;
        r->flags = (x->is_primary ? SR_PRIMARY : 0) | (g_dd && g_dd->primary && g_dd->primary->back == x ? SR_BACK : 0) | (x->has_ck ? SR_CK : 0);
        r->ck_lo = x->ck_lo; r->ck_hi = x->ck_hi; r->ref = x->ref;
        r->pal = (uint32_t)(uintptr_t)x->pal; r->clip = (uint32_t)(uintptr_t)x->clip;
        r->hash = surf_hash(x); r->bits = (uint32_t)(uintptr_t)x->bits;
        /* palettes / clippers they use (and the screen's) */
        void *objs[2] = { x->pal, x->clip };
        for (int k = 0; k < 2; k++) {
            if (!objs[k]) continue;
            int j; for (j = 0; j < no_; j++) if (orr[j].old == (uint32_t)(uintptr_t)objs[k]) break;
            if (j < no_ || no_ >= 16) continue;
            memset(&orr[no_], 0, sizeof orr[no_]);
            orr[no_].old = (uint32_t)(uintptr_t)objs[k]; orr[no_].kind = k ? OB_CLIP : OB_PAL;
            if (!k) { orr[no_].ref = ((Pal *)objs[k])->ref; memcpy(orr[no_].e, ((Pal *)objs[k])->e, sizeof orr[no_].e); }
            else orr[no_].ref = ((Clip *)objs[k])->ref;
            no_++;
        }
    }
    if (ok && g_dd && g_dd->pal) {
        int j; for (j = 0; j < no_; j++) if (orr[j].old == H.pal) break;
        if (j == no_ && no_ < 16) { memset(&orr[no_], 0, sizeof orr[no_]); orr[no_].old = H.pal; orr[no_].kind = OB_PAL; orr[no_].ref = g_dd->pal->ref; memcpy(orr[no_].e, g_dd->pal->e, sizeof orr[no_].e); no_++; }
    }
    H.nsurf = ns; H.nobj = no_;
    H.off_surf = tell(h);
    ok = ok && put(h, sr, sizeof *sr * ns);
    H.off_pix = tell(h);
    BYTE *row = NULL, *pk = NULL; size_t cap = 0;
    for (int i = 0, j = 0; ok && i < na; i++) {
        Surf *x = all[i];
        if (x->ref <= 0) continue;
        SurfRec *r = &sr[j++];
        if (r->flags & (SR_PRIMARY | SR_BACK)) { r->pix_n = 0; continue; }   /* redrawn every frame */
        size_t n = (size_t)x->w * x->h;
        if (n + n / 64 + 16 > cap) { free(row); free(pk); cap = n + n / 64 + 16; row = malloc(cap); pk = malloc(cap); if (!row || !pk) { ok = 0; break; } }
        for (int y = 0; y < x->h; y++) memcpy(row + (size_t)y * x->w, x->bits + (size_t)y * x->pitch, x->w);
        r->pix_n = (uint32_t)pack(row, n, pk);
        ok = put(h, pk, r->pix_n);
        H.pix_bytes += r->pix_n;
    }
    free(row); free(pk);
    H.off_obj = tell(h);
    ok = ok && put(h, orr, sizeof *orr * no_);
    VmRec *vm = malloc(sizeof *vm * 4096);
    H.nvm = vm ? vm_map(vm, 4096) : 0;
    H.off_vm = tell(h);
    ok = ok && put(h, vm, sizeof *vm * H.nvm);
    H.off_end = tell(h);
    ok = ok && seek(h, H.off_surf) && put(h, sr, sizeof *sr * ns);   /* with their pixel sizes */
    ok = ok && seek(h, 0) && put(h, &H, sizeof H);
    free(vm); free(orr); free(sr); free(all);
    CloseHandle(h);
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        banner("CANNOT SAVE: write error");
        mlog("savestate: writing %s failed (%lu)", path, GetLastError());
        surf_upkeep();
        return;
    }
    /* the fast in-process path: the surfaces alive now are kept (zombies) while this slot is used */
    s->run = H.run;
    s->surf = malloc(sizeof *s->surf * 4096);
    s->nsurf = s->surf ? surf_snapshot(s->surf, 4096) : 0;
    s->primary = g_dd ? (void *)g_dd->primary : NULL;
    s->used = 1;
    surf_upkeep();
    QueryPerformanceCounter(&t1);
    mlog("savestate: slot %d saved to %s - run %u scenario %d mission %d, game clock %d ms, tick %lu; exe data %u KB, heap %u KB "
         "(%u blocks, %u KB in use), mod %u bytes in %d regions, %u surfaces (%u KB pixels packed), %u objects; file %u KB; %.1f ms",
         g_cur + 1, path, H.run, H.scen, H.mission, H.game_ms, (unsigned long)H.tick, (unsigned)((g_data_hi - g_data_lo) >> 10), H.heap_n >> 10,
         A->blocks, A->used >> 10, (unsigned)g_reg_bytes, g_nreg, H.nsurf, H.pix_bytes >> 10, H.nobj, H.off_end >> 10,
         (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    ar_check("at the save");
    if (C.scan) research();
    log_sarge("at the save");
    log_digest("at the save");
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

static int in_keep(uintptr_t a) { for (int k = 0; k < NKEEP; k++) if (a >= KEEP[k].lo && a < KEEP[k].hi) return 1; return 0; }
static int in_own(uintptr_t a)
{
    for (int k = 0; k < NOWN; k++) if (a >= OWN[k].lo && a < OWN[k].hi) return 1;
    for (int k = 0; k < g_npatched; k++) if (a >= g_patched[k].lo && a < g_patched[k].hi) return 1;
    return 0;
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

static void loaded(int slot, const AmsHdr *H, const char *how, LARGE_INTEGER t0)
{
    clock_restore(H->tick);
    gameplay_run_resync();
    controls_reset();
    FIELD_REDRAW = 1;
    LARGE_INTEGER t1, f; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    mlog("savestate: slot %d loaded (%s) - game clock back to %d ms (tick %lu), heap %u KB; %.1f ms",
         slot + 1, how, H->game_ms, (unsigned long)H->tick, H->heap_n >> 10, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    surf_upkeep();
    ar_check("after the load");
    log_sarge("after the load");
    log_digest("after the load");
    banner("QUICKSAVE %d LOADED", slot + 1);
}

/* the fast path: a save this process made in this mission run - every pointer of the snapshot is still valid */
static void load_same_run(int slot, HANDLE h, const AmsHdr *H)
{
    Slot *s = &g_slot[slot];
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    log_sarge("before the load");
    BYTE *exe = malloc(g_data_hi - g_data_lo);
    if (!exe || !seek(h, H->off_exe) || !get(h, exe, g_data_hi - g_data_lo)) { free(exe); banner("CANNOT LOAD: read error"); return; }
    stop_effects();
    present_lock();                                 /* the display thread reads mod state (overlays) */
    EnterCriticalSection(&g_acs);
    if (!ar_commit(H->heap_n)) { LeaveCriticalSection(&g_acs); present_unlock(); free(exe); banner("CANNOT LOAD: heap"); return; }
    uint32_t committed = A->committed;
    restore_exe(exe);
    int ok = seek(h, H->off_arena) && get(h, AB, H->heap_n);
    A->committed = committed;                       /* what is really committed (the save's count is of then) */
    LeaveCriticalSection(&g_acs);
    ok = ok && seek(h, H->off_reg);
    for (int i = 0; ok && i < g_nreg; i++) ok = get(h, g_reg[i].p, g_reg[i].n);
    free(exe);
    if (!ok) mlog("savestate: READ ERROR in the middle of a load - the game state is mixed");
    int back = surf_restore_refs(s->surf, s->nsurf), newer = surf_count_not_in(s->surf, s->nsurf);
    present_unlock();
    if (back || newer) mlog("savestate: surfaces: %d released since the save are back, %d made since are no longer used by the game (kept)", back, newer);
    loaded(slot, H, "same run", t0);
}

/* ---------------------------------------------------------------- a load from a file (another run / process)
 * The mission of the save is running (started fresh). Its exe data, heap and mod state are replaced by the save's;
 * what the snapshot holds of the SAVING process is mapped onto this one:
 *   - our DirectDraw objects: every surface of the save gets one of now - the screen / back buffer by role, else one
 *     of the same size, caps and colour key with the same pixels (the same picture loaded again: its HD state
 *     stays), else one of the same size in creation order (pixels of the save copied in), else a NEW surface with the
 *     save's pixels (pictures loaded after the mission start - air strike, paratroopers, radio, on-demand sprites);
 *     the screen palette (entries of the save), the screen clipper, the DirectDraw object. Every dword of the
 *     snapshot equal to such an object's old address is set to the new one (exe data outside KEEP / OWN, the heap
 *     blocks outside the own blocks, the mod regions); BITS_PTR follows its surface's pixels.
 *   - OWN[] / patched exe data and the own heap blocks (MFC, main window) keep the values of now; an exe dword that
 *     pointed into the saving process's CRT heap (the keep heap, not saved) keeps the value of now too.
 *   - the game heap must be at the same address (it is reserved early in DllMain - same place in every run of the
 *     same set-up; [savestate] heap_base pins it) and hold the same own blocks at the same places: else refused.
 * A fresh-mission surface the save does not use stays alive, unused (as after an in-process load). */
typedef struct { const SurfRec *r; Surf *now; int how; } SMap;     /* how: 1 role, 2 same pixels, 3 same size, 4 new */
typedef struct { uint32_t old, now; } PMap;
static int cmp_pmap(const void *a, const void *b) { uint32_t x = ((const PMap *)a)->old, y = ((const PMap *)b)->old; return x < y ? -1 : x > y; }
static int cmp_serial(const void *a, const void *b) { uint32_t x = ((const SMap *)a)->r->serial, y = ((const SMap *)b)->r->serial; return x < y ? -1 : x > y; }
static int cmp_surf_serial(const void *a, const void *b) { unsigned x = (*(Surf *const *)a)->serial, y = (*(Surf *const *)b)->serial; return x < y ? -1 : x > y; }

static int known_ptr(uint32_t where, uint32_t off)
{
    for (size_t i = 0; i < sizeof KNOWN_PTR / sizeof *KNOWN_PTR; i++) if (KNOWN_PTR[i].where == where && KNOWN_PTR[i].off == off) return 1;
    return 0;
}

static void load_file(int slot, HANDLE h, const AmsHdr *H)
{
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    char why[96] = "";
    SurfRec *sr = malloc(sizeof *sr * (H->nsurf + 1));
    ObjRec *orr = malloc(sizeof *orr * (H->nobj + 1));
    VmRec *vm = malloc(sizeof *vm * (H->nvm + 1));
    SMap *sm = malloc(sizeof *sm * (H->nsurf + 1));
    Surf **cur = malloc(sizeof *cur * 4096);
    uint32_t *chash = malloc(sizeof *chash * 4096);
    BYTE *exe = malloc(g_data_hi - g_data_lo), *now = malloc(g_data_hi - g_data_lo);
    PMap *pm = malloc(sizeof *pm * (H->nsurf + H->nobj + 4));
    char *cused = calloc(4096, 1);
    BYTE *keep = NULL; uint32_t keep_n = 0, *keep_off = NULL, *own = NULL;
    int ncur = 0, npm = 0;
    if (!sr || !orr || !vm || !sm || !cur || !chash || !exe || !now || !pm || !cused) { lstrcpyA(why, "out of memory"); goto out; }
    if (!seek(h, H->off_surf) || !get(h, sr, sizeof *sr * H->nsurf) || !seek(h, H->off_obj) || !get(h, orr, sizeof *orr * H->nobj) ||
        !seek(h, H->off_vm) || !get(h, vm, sizeof *vm * H->nvm) || !seek(h, H->off_exe) || !get(h, exe, g_data_hi - g_data_lo)) { lstrcpyA(why, "read error"); goto out; }

    /* 1. the own heap blocks of now must be blocks of the save at the same place (same size, tag) */
    EnterCriticalSection(&g_acs);
    int nown = own_blocks(&own);
    LeaveCriticalSection(&g_acs);
    if (nown < 0) { lstrcpyA(why, "out of memory"); goto out; }
    for (int j = 0; j < nown; j++) {
        uint32_t off = own[j];
        Blk *b = (Blk *)(AB + off);
        Blk sb;
        if (off + BSIZE(b) > H->heap_n || !seek(h, H->off_arena + off) || !get(h, &sb, sizeof sb) || sb.size != b->size || sb.tag != b->tag) {
            _snprintf(why, sizeof why, "the game's window objects differ (block +%#x)", off);
            mlog("savestate: own block +%#x (tag %08x, %u bytes) is not in the save at the same place (save there: %08x tag %08x) - refused",
                 off, b->tag, BSIZE(b), off + BSIZE(b) > H->heap_n ? 0 : sb.size, off + BSIZE(b) > H->heap_n ? 0 : sb.tag);
            goto out;
        }
        keep_n += BSIZE(b);
    }
    keep = malloc(keep_n + 1); keep_off = malloc(sizeof *keep_off * (nown + 1));
    if (!keep || !keep_off) { lstrcpyA(why, "out of memory"); goto out; }

    /* 2. surfaces of the save -> surfaces of now */
    ncur = surf_list_all(cur, 4096);
    for (int i = 0; i < ncur; i++) chash[i] = cur[i]->ref > 0 && !cur[i]->is_primary && cur[i]->bits ? surf_hash(cur[i]) : 0;
    Surf *pri = g_dd ? g_dd->primary : NULL;
    for (uint32_t i = 0; i < H->nsurf; i++) {
        sm[i].r = &sr[i]; sm[i].now = NULL; sm[i].how = 0;
        if (sr[i].flags & SR_PRIMARY) sm[i].now = pri, sm[i].how = 1;
        else if (sr[i].flags & SR_BACK) sm[i].now = pri ? pri->back : NULL, sm[i].how = 1;
        if (sm[i].how == 1) {
            if (!sm[i].now || (uint32_t)sm[i].now->w != sr[i].w || (uint32_t)sm[i].now->h != sr[i].h) {
                _snprintf(why, sizeof why, "the display size differs (save %ux%u)", sr[i].w, sr[i].h); goto out;
            }
            for (int k = 0; k < ncur; k++) if (cur[k] == sm[i].now) cused[k] = 1;
        }
    }
#define SAME_KIND(x, r) ((uint32_t)(x)->w == (r)->w && (uint32_t)(x)->h == (r)->h && (x)->caps == (r)->caps && \
                         !!(x)->has_ck == !!((r)->flags & SR_CK) && (!(x)->has_ck || ((x)->ck_lo == (r)->ck_lo && (x)->ck_hi == (r)->ck_hi)))
    for (uint32_t i = 0; i < H->nsurf && !C.newsurf; i++) {        /* the same picture */
        if (sm[i].how) continue;
        for (int k = 0; k < ncur; k++)
            if (!cused[k] && cur[k]->ref > 0 && chash[k] == sr[i].hash && SAME_KIND(cur[k], &sr[i])) { sm[i].now = cur[k]; sm[i].how = 2; cused[k] = 1; break; }
    }
    qsort(sm, H->nsurf, sizeof *sm, cmp_serial);
    qsort(cur, ncur, sizeof *cur, cmp_surf_serial);   /* (cused / chash are no longer needed by index) */
    memset(cused, 0, 4096);
    for (uint32_t i = 0; i < H->nsurf; i++) for (int k = 0; k < ncur; k++) if (sm[i].now == cur[k]) cused[k] = 1;
    for (uint32_t i = 0; i < H->nsurf && !C.newsurf; i++) {        /* the same kind, in creation order */
        if (sm[i].how) continue;
        for (int k = 0; k < ncur; k++)
            if (!cused[k] && cur[k]->ref > 0 && !cur[k]->is_primary && SAME_KIND(cur[k], sm[i].r)) { sm[i].now = cur[k]; sm[i].how = 3; cused[k] = 1; break; }
    }
    int n_how[5] = { 0 };
    for (uint32_t i = 0; i < H->nsurf; i++) {        /* made after the mission start: made again */
        if (!sm[i].how) {
            const SurfRec *r = sm[i].r;
            Surf *x = surf_new(g_dd, (int)r->w, (int)r->h, r->caps);
            if (!x) { lstrcpyA(why, "cannot make a surface"); goto out; }
            x->has_ck = !!(r->flags & SR_CK); x->ck_lo = r->ck_lo; x->ck_hi = r->ck_hi;
            sm[i].now = x; sm[i].how = 4;
        }
        n_how[sm[i].how]++;
    }
    /* 3. the pointer map: surfaces, DirectDraw (both interfaces), palettes, clippers */
    for (uint32_t i = 0; i < H->nsurf; i++) pm[npm++] = (PMap){ sm[i].r->old, (uint32_t)(uintptr_t)sm[i].now };
    if (H->dd && g_dd) { pm[npm++] = (PMap){ H->dd, (uint32_t)(uintptr_t)g_dd }; pm[npm++] = (PMap){ H->dd + 4, (uint32_t)(uintptr_t)g_dd + 4 }; }
    Pal *newpal[16] = { 0 };
    for (uint32_t j = 0; j < H->nobj && j < 16; j++) {
        void *x = NULL;
        if (orr[j].kind == OB_PAL) {
            if (orr[j].old == H->pal && g_dd && g_dd->pal) x = g_dd->pal;
            else if (pal_create(g_dd, 0, orr[j].e, &newpal[j]) == DD_OK) { x = newpal[j]; newpal[j]->ref = orr[j].ref; }
        } else if (orr[j].kind == OB_CLIP) {
            if (orr[j].old == H->clip && pri && pri->clip) x = pri->clip;
            else { Clip *c; if (clip_create(&c) == DD_OK) { c->hwnd = g_dd ? g_dd->hwnd : NULL; c->ref = orr[j].ref; x = c; } }
        }
        if (x) pm[npm++] = (PMap){ orr[j].old, (uint32_t)(uintptr_t)x };
    }
    qsort(pm, npm, sizeof *pm, cmp_pmap);

    /* 4. exe data of this process kept; pointers into the saving process's CRT heap -> the value of now */
    memcpy(now, (void *)g_data_lo, g_data_hi - g_data_lo);
    int n_own = 0, n_crt = 0, n_dll = 0, n_heap = 0;
    for (uintptr_t a = g_data_lo; a + 4 <= g_data_hi; a += 4) {
        if (in_keep(a)) continue;
        uint32_t *p = (uint32_t *)(exe + (a - g_data_lo)), nv = *(uint32_t *)(now + (a - g_data_lo));
        if (in_own(a)) { if (*p != nv) n_own++; *p = nv; continue; }
        if (*p == nv || *p < 0x10000 || (*p & 3)) continue;
        /* diagnostics only: a value that points into the saving process's keep heap / process heap may be a pointer the
         * tables do not know (or game data that looks like one: packed y << 16 | x positions do) - kept as saved */
        int k = vm_kind(vm, H->nvm, *p);
        if (k == VK_CRTHEAP && n_crt++ < 8)
            mlog("savestate: exe %#x holds %08x (in the saving process's keep heap) - kept as saved", (unsigned)a, *p);
        else if (k == VK_PROCHEAP && !bsearch(&(PMap){ *p, 0 }, pm, npm, sizeof *pm, cmp_pmap) && n_heap++ < 8)
            mlog("savestate: exe %#x holds %08x (in the saving process's heap, not one of our objects) - kept as saved", (unsigned)a, *p);
    }
    /* pointers to our objects in the exe data (on the buffer: the game's other threads never see a half-done state) */
    uint32_t pmin = npm ? pm[0].old : 1, pmax = npm ? pm[npm - 1].old : 0;
    int n_ptr = 0, n_odd = 0, n_bits = 0;
#define PATCH(p, where, off) do { uint32_t v_ = *(p); if (v_ >= pmin && v_ <= pmax) { PMap key_ = { v_, 0 }, *m_ = bsearch(&key_, pm, npm, sizeof *pm, cmp_pmap); \
        if (m_) { *(p) = m_->now; n_ptr++; if ((where) && !known_ptr((where), (off)) && n_odd++ < 40) mlog("savestate: pointer to our object %08x at %s %#x +%u (not in the table) -> %08x", v_, (where) >= 0x1000000 ? "block tag" : "exe", (unsigned)((where) & 0xffffff), (unsigned)(off), m_->now); } } } while (0)
    for (uintptr_t a = g_data_lo; a + 4 <= g_data_hi; a += 4) if (!in_keep(a) && !in_own(a)) PATCH((uint32_t *)(exe + (a - g_data_lo)), (uint32_t)a, 0);
    {   /* the pixel pointer */
        uint32_t *p = (uint32_t *)(exe + (BITS_PTR - g_data_lo)), v = *p;
        if (v) {
            uint32_t nv = *(uint32_t *)(now + (BITS_PTR - g_data_lo));
            for (uint32_t i = 0; i < H->nsurf; i++) {
                const SurfRec *r = sm[i].r;
                uint32_t pitch = (r->w + 3) & ~3u;
                if (v >= r->bits && v - r->bits < pitch * r->h) { nv = (uint32_t)(uintptr_t)sm[i].now->bits + (v - r->bits); n_bits++; break; }
            }
            *p = nv;
        }
    }
    /* 5. replace the state */
    log_sarge("before the load");
    stop_effects();
    present_lock();
    EnterCriticalSection(&g_acs);
    {   /* the own blocks of now, kept aside (the same set: nothing allocated since, we are on the game thread) */
        BYTE *k = keep;
        for (int n = 0; n < nown; n++) { Blk *b = (Blk *)(AB + own[n]); memcpy(k, b, BSIZE(b)); k += BSIZE(b); keep_off[n] = own[n]; }
    }
    uint32_t old_top = A->top;
    if (!ar_commit(H->heap_n)) { LeaveCriticalSection(&g_acs); present_unlock(); lstrcpyA(why, "game heap"); goto out_mixed_free; }
    uint32_t committed = A->committed, reserved = A->reserved;
    restore_exe(exe);
    int ok = seek(h, H->off_arena) && get(h, AB, H->heap_n);
    A->committed = committed; A->reserved = reserved;   /* what is really committed / reserved in this process */
    {   /* own blocks back, at their places (the save has the same blocks there: checked above) */
        BYTE *k = keep; int differ = 0;
        for (int n = 0; n < nown; n++) {
            uint32_t sz = BSIZE((Blk *)k);
            if (memcmp(AB + keep_off[n], k, sz)) differ++;
            memcpy(AB + keep_off[n], k, sz);
            k += sz;
        }
        mlog("savestate: file load: %d own heap blocks put back (%d differed from the save's)", nown, differ);
    }
    LeaveCriticalSection(&g_acs);
    ok = ok && seek(h, H->off_reg);
    for (int i = 0; ok && i < g_nreg; i++) ok = get(h, g_reg[i].p, g_reg[i].n);
    if (!ok) mlog("savestate: READ ERROR in the middle of a load - the game state is mixed");
    (void)old_top;

    /* 6. pointers to our objects in the heap blocks and the mod regions */
    for (uint32_t off = FIRST_OFF; off < A->top; ) {
        Blk *b = (Blk *)(AB + off);
        uint32_t sz = BSIZE(b);
        if (sz < 16) break;
        if (INUSE(b) && !is_own(own, nown, off)) for (uint32_t i = HDR; i + 4 <= sz; i += 4) PATCH((uint32_t *)((BYTE *)b + i), 0x1000000 | (b->tag & 0xffffff), i - HDR);
        off += sz;
    }
    uint32_t dlo, dn; dll_range(&dlo, &dn);
    for (int i = 0; i < g_nreg; i++)
        for (size_t o = 0; o + 4 <= g_reg[i].n; o += 4) {
            uint32_t *p = (uint32_t *)((BYTE *)g_reg[i].p + o);
            PATCH(p, 0, 0);
            if (H->dll_base != dlo && *p >= H->dll_base && *p - H->dll_base < H->dll_size) { *p = *p - H->dll_base + dlo; n_dll++; }
        }
    /* 7. the surfaces: reference counts, pixels (where they differ), palettes */
    BYTE *pk = NULL, *row = NULL; size_t cap = 0; int n_pix = 0;
    uint32_t pix_off = H->off_pix;
    /* pixel blocks are in the save's surface order (sr[]) */
    uint32_t *pix_at = malloc(sizeof *pix_at * (H->nsurf + 1));
    if (pix_at) for (uint32_t i = 0; i < H->nsurf; i++) { pix_at[i] = pix_off; pix_off += sr[i].pix_n; }
    for (uint32_t i = 0; i < H->nsurf; i++) {
        const SurfRec *r = sm[i].r; Surf *x = sm[i].now;
        x->ref = r->ref;
        void *np = NULL;
        if (r->pal) { PMap key = { r->pal, 0 }, *m = bsearch(&key, pm, npm, sizeof *pm, cmp_pmap); np = m ? (void *)(uintptr_t)m->now : NULL; }
        if (sm[i].how != 1 && (void *)x->pal != np) {
            if (np) ((Pal *)np)->ref++;
            if (x->pal) ((ULONG (WINAPI *)(void *))x->pal->vtbl[2])(x->pal);
            x->pal = np;
        }
        if (sm[i].how < 3 || !r->pix_n || !pix_at) continue;
        size_t n = (size_t)r->w * r->h;
        if (n > cap || r->pix_n > cap) { free(pk); free(row); cap = n > r->pix_n ? n : r->pix_n; pk = malloc(cap); row = malloc(cap); if (!pk || !row) { cap = 0; continue; } }
        if (!seek(h, pix_at[sm[i].r - sr]) || !get(h, pk, r->pix_n) || !unpack(pk, r->pix_n, row, n)) { mlog("savestate: pixels of surface %ux%u unreadable", r->w, r->h); continue; }
        for (uint32_t y = 0; y < r->h; y++) memcpy(x->bits + (size_t)y * x->pitch, row + (size_t)y * r->w, r->w);
        hdsprites_forget(x); hdsprites_dirty(x); hdworld_surface_replaced(x);
        n_pix++;
    }
    free(pk); free(row); free(pix_at);
    present_unlock();
    for (uint32_t j = 0; j < H->nobj && j < 16; j++)     /* the screen palette as it was (a fade in progress) */
        if (orr[j].kind == OB_PAL && orr[j].old == H->pal && g_dd && g_dd->pal)
            ((HRESULT (WINAPI *)(void *, DWORD, DWORD, DWORD, LPPALETTEENTRY))g_dd->pal->vtbl[6])(g_dd->pal, 0, 0, 256, orr[j].e);
    mlog("savestate: file load: surfaces %u of the save - %d screen, %d same picture, %d same size (pixels copied), %d made again; "
         "%d pixel sets copied; %d pointers to our objects set (%d outside the table), pixel pointer %s",
         H->nsurf, n_how[1], n_how[2], n_how[3], n_how[4], n_pix, n_ptr, n_odd, n_bits ? "moved" : "not a surface's");
    mlog("savestate: file load: %d own heap blocks (%u KB) kept, %d own exe dwords kept (of %d ranges + %d patched), %d / %d values that look like keep-heap / heap pointers (kept as saved), "
         "%d dll pointers moved (dll %08x, save %08x)", nown, keep_n >> 10, n_own, NOWN, g_npatched, n_crt, n_heap, n_dll, dlo, H->dll_base);
    {
        int unused = 0;
        for (int k = 0; k < ncur; k++) if (cur[k]->ref > 0 && !cur[k]->is_primary) {
            int in = 0; for (uint32_t i = 0; i < H->nsurf && !in; i++) in = sm[i].now == cur[k];
            if (!in && !(pri && pri->back == cur[k])) unused++;
        }
        if (unused) mlog("savestate: file load: %d surface(s) of this mission start are not used by the save (kept)", unused);
    }
    free(own); free(keep); free(keep_off); free(sr); free(orr); free(vm); free(sm); free(cur); free(chash); free(exe); free(now); free(pm); free(cused);
    loaded(slot, H, "from the file", t0);
    return;
out_mixed_free:
out:
    for (uint32_t i = 0; sm && i < H->nsurf; i++) if (sm[i].how == 4 && sm[i].now) surf_release(sm[i].now);
    free(own); free(keep); free(keep_off); free(sr); free(orr); free(vm); free(sm); free(cur); free(chash); free(exe); free(now); free(pm); free(cused);
    banner("CANNOT LOAD: %s", why[0] ? why : "?");
}

/* ---------------------------------------------------------------- load: which path */
static void do_load(int slot)
{
    char path[MAX_PATH];
    slot_path(slot, path, sizeof path, "ams");
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) { banner("QUICKSAVE %d IS EMPTY", slot + 1); return; }
    AmsHdr H;
    const char *bad = NULL;
    uint32_t dlo, dn; dll_range(&dlo, &dn);
    if (!get(h, &H, sizeof H) || H.magic != AMS_MAGIC) bad = "not a save file";
    else if (H.version != AMS_VERSION || H.hdr_size != sizeof H || H.arena_fmt != ARENA_FMT) bad = "save of another mod version";
    else if (H.exe_id != exe_id()) bad = "save of another Armymen.exe";
    else if (H.data_lo != g_data_lo || H.data_hi != g_data_hi) bad = "save of another Armymen.exe";
    else if (H.nreg != (uint32_t)g_nreg || H.reg_bytes != g_reg_bytes || H.reg_sig != reg_sig()) bad = "save of another mod build (state layout)";
    else if (H.arena_base != (uint32_t)(uintptr_t)AB || H.arena_reserved != A->reserved) bad = "game heap at another address (see the log)";
    else if (H.heap_n > A->reserved || H.off_end < H.off_vm) bad = "damaged save file";
    if (bad) {
        if (H.magic == AMS_MAGIC)
            mlog("savestate: %s: version %u fmt %u, exe %08x (now %08x), build %s (now %s), regions %u/%u sig %08x (now %d/%u %08x), heap %08x %u MB (now %p %u MB)",
                 path, H.version, H.arena_fmt, H.exe_id, exe_id(), H.build, __DATE__ " " __TIME__, H.nreg, H.reg_bytes, H.reg_sig, g_nreg,
                 (unsigned)g_reg_bytes, reg_sig(), H.arena_base, H.arena_reserved >> 20, (void *)A, A->reserved >> 20);
        CloseHandle(h); banner("CANNOT LOAD: %s", bad); return;
    }
    int scen, mission;
    unsigned run = gameplay_run(&scen, &mission);
    const char *no = why_not(0);
    if (no && !strcmp(no, "not in a mission")) {
        /* not in play: from the campaign select screen the save's scenario is started, the load follows in play */
        if (missionpath_screen_state() == 2) {
            missionpath_request_scenario(H.scen);
            g_pending = slot; g_pending_run = run; g_pending_at = GetTickCount();
            mlog("savestate: slot %d is of scenario %d mission %d: starting the scenario, the load follows in play", slot + 1, H.scen, H.mission);
            banner("QUICKSAVE %d: STARTING ITS MISSION", slot + 1);
        } else banner("CANNOT LOAD: in a mission or the campaign screen only");
        CloseHandle(h); return;
    }
    if (no) { CloseHandle(h); banner("CANNOT LOAD: %s", no); return; }
    if (H.scen != scen) { CloseHandle(h); banner("CANNOT LOAD: save of another mission - load it at the campaign screen"); return; }
    Slot *s = &g_slot[slot];
    if (H.session == g_session && s->used && s->run == run && H.run == run && g_dd && s->primary == (void *)g_dd->primary) load_same_run(slot, h, &H);
    else {
        if (s->used && s->run != run) { slot_drop(s); surf_upkeep(); }
        mlog("savestate: slot %d: a save of %s (run %u, scenario %d mission %d, saved by build %s) - loading from the file",
             slot + 1, H.session == g_session ? "this session" : "another session", H.run, H.scen, H.mission, H.build);
        if (H.mission != mission) mlog("savestate: the save is of mission %d, mission %d of the same scenario is running (same map)", H.mission, mission);
        load_file(slot, h, &H);
    }
    CloseHandle(h);
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
            else if (r == K_LOAD) { g_pending = -1; do_load(g_cur); }
            else if (r == K_SLOT) {
                g_cur = (g_cur + 1) % C.slots;
                char path[MAX_PATH]; slot_path(g_cur, path, sizeof path, "ams");
                HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                AmsHdr H;
                if (h != INVALID_HANDLE_VALUE && get(h, &H, sizeof H) && H.magic == AMS_MAGIC)
                    banner("QUICKSAVE SLOT %d (mission %d, game clock %d:%02d)", g_cur + 1, H.mission, H.game_ms / 60000, H.game_ms / 1000 % 60);
                else banner("QUICKSAVE SLOT %d (empty)", g_cur + 1);
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
            guard_leave(&gd);
        }
    }
    /* a load waiting for its mission: in play in a new run, Sarge there, the mission running for 1 s */
    if (g_pending >= 0) {
        if (GetTickCount() - g_pending_at > 180000) { mlog("savestate: the pending load of slot %d timed out", g_pending + 1); g_pending = -1; }
        else if (GAME && game_in_play() && SARGE && gameplay_run(NULL, NULL) != g_pending_run && *(int *)(GAME + 0x44) > 1000 && !why_not(0)) {
            int k = g_pending; g_pending = -1;
            Guard gd;
            if (GUARD_TRY(gd, "savestate pending load")) { do_load(k); guard_leave(&gd); }
        }
    }
    static int auto_t0 = -1;
    if (C.auto_ms > 0 && g_ready && GAME && game_in_play() && SARGE && SARGE_HEALTH(SARGE) > 0 && !why_not(1) &&
        (auto_t0 < 0 ? (auto_t0 = *(int *)(GAME + 0x44), 0) : *(int *)(GAME + 0x44) - auto_t0 >= C.auto_ms)) {   /* research */
        C.auto_ms = 0;
        Guard gd;
        if (GUARD_TRY(gd, "savestate research")) { do_save(); guard_leave(&gd); }
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
/* The arena's address: a load from a file needs the game heap where the save had it. It is reserved here, early in
 * DllMain (before Direct3D / OpenGL / the game take address space), at [savestate] heap_base when set, else at the
 * address of the last run (<dir>\heap.base - what Windows chose the first time), else where Windows puts it. */
static void *reserve_arena(size_t *res)
{
    char v[32], hb[MAX_PATH];
    uintptr_t want = 0;
    GetPrivateProfileStringA("savestate", "heap_base", "", v, sizeof v, mod_ini_path());
    if (v[0]) want = strtoul(v, NULL, 16);
    _snprintf(hb, sizeof hb, "%sheap.base", save_dir()); hb[sizeof hb - 1] = 0;
    if (!want) {
        FILE *f = fopen(hb, "r");
        if (f) { unsigned x = 0; if (fscanf(f, "%x", &x) == 1) want = x; fclose(f); }
    }
    void *p = NULL;
    if (want && !(want & 0xffff)) {
        p = VirtualAlloc((void *)want, *res, MEM_RESERVE, PAGE_READWRITE);
        if (!p) mlog("savestate: the game heap could not be placed at %08x (taken) - saves of other runs will not load", (unsigned)want);
    }
    if (!p) p = VirtualAlloc(NULL, *res, MEM_RESERVE, PAGE_READWRITE);   /* (a fixed 0x20000000 split Wine's big free range: HD mode ran out) */
    while (!p && *res > (32u << 20)) { *res /= 2; p = VirtualAlloc(NULL, *res, MEM_RESERVE, PAGE_READWRITE); }
    if (p && !want) {
        FILE *f = fopen(hb, "w");
        if (f) { fprintf(f, "%08x\n", (unsigned)(uintptr_t)p); fclose(f); }
    }
    mlog("savestate: game heap reserved at %p (%s %08x)", p, !want ? "first run, written to" : (uintptr_t)p == want ? "as wanted by" : "NOT as wanted by",
         (unsigned)want);
    return p;
}

void savestate_install(void)
{
    const char *ini = mod_ini_path();
    char b[64];
    /* The replacement heap is still experimental.  A complete Desert 1 run exposed a reproducible null-address
     * crash while the game built the mission-win screen; the same transition is clean on the game's CRT heap.
     * Keep the feature available for focused testing, but never replace every game allocation unless the player
     * explicitly opts in. */
    C.enabled = !g_cfg.classic_gameplay && GetPrivateProfileIntA("savestate", "enabled", 0, ini);
    g_cfg.quicksave = C.enabled;
    C.slots = GetPrivateProfileIntA("savestate", "slots", 3, ini);
    if (C.slots < 1) C.slots = 1;
    if (C.slots > MAX_SLOTS) C.slots = MAX_SLOTS;
    C.heap_mb = GetPrivateProfileIntA("savestate", "heap_mb", 64, ini);
    C.scan = GetPrivateProfileIntA("savestate", "scan", 0, ini);
    C.auto_ms = GetPrivateProfileIntA("savestate", "research_autosave_ms", 0, ini);
    C.perturb = GetPrivateProfileIntA("savestate", "research_perturb", 0, ini);
    C.newsurf = GetPrivateProfileIntA("savestate", "research_new_surfaces", 0, ini);   /* test: every surface made again from the save */
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

    /* the arena (contiguous, reserved only; committed as the game grows) */
    size_t res = (size_t)C.heap_mb << 20;
    void *p = reserve_arena(&res);
    if (!p || !VirtualAlloc(p, AR_COMMIT, MEM_COMMIT, PAGE_READWRITE)) { mlog("savestate: no address space for the game heap - off"); return; }
    A = p;
    memset(A, 0, sizeof *A);
    A->magic = AR_MAGIC; A->reserved = (uint32_t)res; A->committed = AR_COMMIT;
    A->top = FIRST_OFF; A->peak = A->top; A->last = 0;
    InitializeCriticalSection(&g_acs);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    g_session = (uint32_t)q.LowPart ^ (GetCurrentProcessId() << 16) ^ GetTickCount();

    static const BYTE malloc_o[] = { 0xa1,0x7c,0xba,0x6a,0x00, 0x8b,0x4c,0x24,0x04 };   /* mov eax,[6aba7c]; mov ecx,[esp+4] */
    static const BYTE free_o[] = { 0x51, 0x56, 0x8b,0x74,0x24,0x0c };                   /* push ecx; push esi; mov esi,[esp+0c] */
    static const BYTE realloc_o[] = { 0x83,0xec,0x08, 0x53, 0x55, 0x56 };               /* sub esp,8; push ebx/ebp/esi */
    static const BYTE calloc_o[] = { 0x53, 0x8b,0x5c,0x24,0x0c };                       /* push ebx; mov ebx,[esp+0c] */
    static const BYTE expand_o[] = { 0x51, 0x8b,0x44,0x24,0x0c };                       /* push ecx; mov eax,[esp+0c] */
    static const BYTE msize_o[] = { 0x83,0xec,0x08, 0x56, 0x6a,0x09 };                  /* sub esp,8; push esi; push 9 */
    static const BYTE opnew_o[] = { 0x53, 0x8b,0x5c,0x24,0x08, 0x56, 0x8b,0x35,0x54,0x1f,0x4a,0x00, 0x57, 0x53, 0xe8 };   /* operator new 0x48bf13 (tags) */
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
    if (memcmp((const void *)0x48bf13, opnew_o, sizeof opnew_o)) mlog("savestate: operator new 0x48bf13 not as expected - block tags of `new` are its return address");
    const BYTE *fc = (const BYTE *)FRAME_CALL;
    if (fc[0] != 0xE8 || FRAME_CALL + 5 + *(const int32_t *)(fc + 1) != FRAME_FN) { mlog("savestate: idle loop call 0x40150d not as expected - off"); return; }
    for (size_t i = 0; i < sizeof H / sizeof *H; i++) hook_jmp(H[i].name, H[i].va, H[i].o, H[i].n, H[i].fn);
    hook_call("savestate: idle loop frame (call 0x40150d) -> safe point", FRAME_CALL, FRAME_FN, (void *)frame_call);
    g_ready = 1;
    if (C.perturb) {   /* research: make this process's outside objects land elsewhere (other addresses / handle values) */
        for (int i = 0; i < 5 * C.perturb; i++) HeapAlloc(GetProcessHeap(), 0, 0x3000);
        for (int i = 0; i < 3 * C.perturb; i++) VirtualAlloc(NULL, 0x200000, MEM_RESERVE, PAGE_READWRITE);
        for (int i = 0; i < 7 * C.perturb; i++) CreateEventA(NULL, FALSE, FALSE, NULL);
        for (int i = 0; i < 5 * C.perturb; i++) CreateSolidBrush(RGB(i, 1, 2));
        for (int i = 0; i < 3 * C.perturb; i++) CreateWindowExA(0, "STATIC", "x", 0, 0, 0, 1, 1, NULL, NULL, NULL, NULL);
        mlog("savestate: research_perturb=%d", C.perturb);
    }
    weapons_savestate(); allies_savestate(); aware_savestate(); capture_savestate(); patrol_savestate();
    mines_savestate(); bullets_savestate(); objectives_savestate(); vehicles_savestate(); modern_savestate();
    fences_savestate(); missionpath_savestate(); controls_savestate();
    char kn[3][24];
    for (int k = 0; k < 3; k++) GetPrivateProfileStringA("savestate", NAME[k], DEF[k], kn[k], sizeof kn[k], ini);
    mlog("savestate: on - game heap at %p (%u MB reserved), exe data %#x..%#x, %d slot(s) in %s, keys save %s / load %s / slot %s, "
         "%d mod regions (%u bytes)", (void *)A, (unsigned)(res >> 20), (unsigned)g_data_lo, (unsigned)g_data_hi, C.slots, save_dir(), kn[0], kn[1], kn[2],
         g_nreg, (unsigned)g_reg_bytes);
}

/* [savestate] scan=1 (research, 2026-11-21): what in the snapshot depends on this process. Logs the blocks by tag,
 * the pointers from outside the snapshot (KEEP ranges of the exe, the CRT keep heap) into the arena, and writes
 * a raw dump (exe data, arena, address-space map, keep-heap blocks) next to the ini as research<n>.bin */
static Blk *ar_block_of(uintptr_t v)               /* the in-use block holding address v, or NULL (linear walk) */
{
    if (!in_arena((void *)v) || v >= (uintptr_t)AB + A->top) return NULL;
    uint32_t off = FIRST_OFF;
    while (off < A->top) {
        Blk *b = (Blk *)(AB + off);
        uint32_t sz = BSIZE(b);
        if (sz < 16) return NULL;
        if (v >= (uintptr_t)b && v < (uintptr_t)b + sz) return INUSE(b) && v >= (uintptr_t)b + HDR ? b : NULL;
        off += sz;
    }
    return NULL;
}
/* the folder of the save files: [savestate] dir (default SaveGames, relative to the game folder = next to the ini) */
static const char *save_dir(void)
{
    static char d[MAX_PATH];
    if (d[0]) return d;
    char v[MAX_PATH];
    GetPrivateProfileStringA("savestate", "dir", "SaveGames", v, sizeof v, mod_ini_path());
    if ((v[0] && v[1] == ':') || v[0] == '\\' || v[0] == '/') lstrcpynA(d, v, sizeof d);
    else {
        lstrcpynA(d, mod_ini_path(), sizeof d);
        char *sl = strrchr(d, '\\'); if (sl) sl[1] = 0; else d[0] = 0;
        strncat(d, v, sizeof d - strlen(d) - 1);
    }
    size_t n = strlen(d);
    if (n && d[n - 1] != '\\' && d[n - 1] != '/') strncat(d, "\\", sizeof d - n - 1);
    CreateDirectoryA(d, NULL);
    return d;
}

static void research(void)
{
    /* tags */
    struct { uint32_t tag, n, bytes; } t[512]; int nt = 0;
    for (uint32_t off = FIRST_OFF; off < A->top; ) {
        Blk *b = (Blk *)(AB + off); uint32_t sz = BSIZE(b);
        if (INUSE(b)) {
            int i; for (i = 0; i < nt; i++) if (t[i].tag == b->tag) break;
            if (i == nt && nt < 512) { t[nt].tag = b->tag; t[nt].n = t[nt].bytes = 0; nt++; }
            if (i < nt) { t[i].n++; t[i].bytes += sz; }
        }
        off += sz;
    }
    for (int i = 0; i < nt; i++) for (int j = i + 1; j < nt; j++) if (t[j].bytes > t[i].bytes) { __typeof__(t[0]) x = t[i]; t[i] = t[j]; t[j] = x; }
    mlog("research: %d block tags", nt);
    for (int i = 0; i < nt; i++) mlog("research: tag %08x %6u blocks %8u bytes", t[i].tag, t[i].n, t[i].bytes);
    /* KEEP ranges -> arena */
    int nk = 0;
    for (int k = 0; k < NKEEP; k++)
        for (uintptr_t a = KEEP[k].lo; a + 4 <= KEEP[k].hi; a += 4) {
            uintptr_t v = *(uint32_t *)a;
            Blk *b = ar_block_of(v);
            if (b && nk++ < 200) mlog("research: keep %#x (%s) -> arena +%#x = block +%#x tag %08x size %u (+%u)", (unsigned)a, KEEP[k].what,
                                     (unsigned)(v - (uintptr_t)AB), (unsigned)((BYTE *)b - AB), b->tag, BSIZE(b), (unsigned)(v - (uintptr_t)b - HDR));
        }
    mlog("research: %d pointers from KEEP ranges into the arena", nk);
    /* keep heap -> arena */
    PROCESS_HEAP_ENTRY e; memset(&e, 0, sizeof e);
    int nh = 0, nblk = 0;
    HeapLock(CRT_HEAP);
    while (HeapWalk(CRT_HEAP, &e)) {
        if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
        nblk++;
        for (DWORD i = 0; i + 4 <= e.cbData; i += 4) {
            uintptr_t v = *(uint32_t *)((BYTE *)e.lpData + i);
            Blk *b = ar_block_of(v);
            if (b && nh++ < 200) mlog("research: keepheap %p+%#lx (size %lu) -> block +%#x tag %08x size %u (+%u)", e.lpData, i, e.cbData,
                                     (unsigned)((BYTE *)b - AB), b->tag, BSIZE(b), (unsigned)(v - (uintptr_t)b - HDR));
        }
    }
    HeapUnlock(CRT_HEAP);
    mlog("research: %d keep-heap blocks, %d pointers from them into the arena", nblk, nh);
    /* dump */
    static int seq;
    char path[MAX_PATH]; _snprintf(path, sizeof path, "%sresearch%lu_%d.bin", save_dir(), GetCurrentProcessId(), ++seq); path[sizeof path - 1] = 0;
    FILE *f = fopen(path, "wb");
    if (!f) { mlog("research: cannot write %s", path); return; }
    uint32_t h[8] = { 0x53455241, (uint32_t)g_data_lo, (uint32_t)g_data_hi, (uint32_t)(uintptr_t)AB, A->top, 0, 0, 0 };
    fwrite(h, sizeof h, 1, f);
    fwrite((void *)g_data_lo, g_data_hi - g_data_lo, 1, f);
    fwrite(AB, A->top, 1, f);
    /* address space map: base, size, allocbase, state, type, protect, name[40] */
    MEMORY_BASIC_INFORMATION mi; uintptr_t a = 0x10000;
    while (a < 0x7fff0000 && VirtualQuery((void *)a, &mi, sizeof mi)) {
        if (mi.State != MEM_FREE) {
            struct { uint32_t base, size, ab, state, type, prot; char name[40]; } r = { (uint32_t)(uintptr_t)mi.BaseAddress, (uint32_t)mi.RegionSize,
                (uint32_t)(uintptr_t)mi.AllocationBase, mi.State, mi.Type, mi.Protect, "" };
            char mod[MAX_PATH];
            if (mi.Type == MEM_IMAGE && GetModuleFileNameA((HMODULE)mi.AllocationBase, mod, sizeof mod)) { const char *s = strrchr(mod, '\\'); lstrcpynA(r.name, s ? s + 1 : mod, sizeof r.name); }
            else if (mi.AllocationBase == GetProcessHeap()) lstrcpynA(r.name, "process heap", sizeof r.name);
            else if (mi.AllocationBase == CRT_HEAP) lstrcpynA(r.name, "crt heap", sizeof r.name);
            else if ((BYTE *)mi.AllocationBase == AB) lstrcpynA(r.name, "arena", sizeof r.name);
            fputc("V"[0], f); fwrite(&r, sizeof r, 1, f);
        }
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    /* our objects: surfaces (address, w, h, bits), DirectDraw, palette */
    Surf *all[4096]; int ns = surf_list_all(all, 4096);
    for (int i = 0; i < ns; i++) { uint32_t r[8] = { (uint32_t)(uintptr_t)all[i], (uint32_t)all[i]->w, (uint32_t)all[i]->h, (uint32_t)(uintptr_t)all[i]->bits, (uint32_t)all[i]->pitch, (uint32_t)all[i]->ref, all[i]->caps, all[i]->serial }; fputc("S"[0], f); fwrite(r, sizeof r, 1, f); }
    if (g_dd) { uint32_t r[8] = { (uint32_t)(uintptr_t)g_dd, (uint32_t)(uintptr_t)g_dd->pal, (uint32_t)(uintptr_t)g_dd->hwnd, (uint32_t)(uintptr_t)g_self, (uint32_t)(uintptr_t)GetProcessHeap(), (uint32_t)(uintptr_t)CRT_HEAP, 0, 0 }; fputc("D"[0], f); fwrite(r, sizeof r, 1, f); }
    memset(&e, 0, sizeof e);
    HeapLock(CRT_HEAP);
    while (HeapWalk(CRT_HEAP, &e)) if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) { uint32_t r[2] = { (uint32_t)(uintptr_t)e.lpData, e.cbData }; fputc("K"[0], f); fwrite(r, sizeof r, 1, f); }
    HeapUnlock(CRT_HEAP);
    fclose(f);
    mlog("research: dump written to %s", path);
}

