/* ============================================================================================
 * par.c - split per-pixel work over the CPU's cores (display thread; hdworld_build)
 *
 * par_for(n, fn, ctx) calls fn(lo, hi, ctx) on ranges covering 0..n-1: one range on the calling
 * thread, the others on up to PAR_MAX - 1 worker threads (cores - 1, at least 1 worker when there
 * are 2+ cores). Returns when all ranges are done. Only ONE thread may call par_for at a time
 * (only the display thread does).
 * ============================================================================================ */
#include "common.h"

#define PAR_MAX 8

typedef struct { HANDLE go, done; int lo, hi; } Worker;
static Worker g_w[PAR_MAX];
static int g_nw = -1;                   /* -1 = not started */
static ParFn g_fn; static void *g_ctx;

static DWORD WINAPI worker(void *arg)
{
    Worker *w = arg;
    for (;;) {
        WaitForSingleObject(w->go, INFINITE);
        g_fn(w->lo, w->hi, g_ctx);
        SetEvent(w->done);
    }
    return 0;
}

static void start(void)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors - 1;   /* leave a core for the game */
    if (n > PAR_MAX - 1) n = PAR_MAX - 1;
    if (n < 0) n = 0;
    g_nw = 0;
    for (int i = 0; i < n; i++) {
        g_w[i].go = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_w[i].done = CreateEventA(NULL, FALSE, FALSE, NULL);
        HANDLE t = g_w[i].go && g_w[i].done ? CreateThread(NULL, 0, worker, &g_w[i], 0, NULL) : NULL;
        if (!t) break;
        SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(t);
        g_nw++;
    }
    mlog("par: %d worker threads (%lu processors)", g_nw, si.dwNumberOfProcessors);
}

void par_for(int n, ParFn fn, void *ctx)
{
    if (g_nw < 0) start();
    int parts = g_nw + 1;
    if (n < parts * 8 || parts == 1) { fn(0, n, ctx); return; }
    g_fn = fn; g_ctx = ctx;
    HANDLE wait[PAR_MAX];
    for (int i = 0; i < g_nw; i++) {
        g_w[i].lo = (int)((long long)n * i / parts);
        g_w[i].hi = (int)((long long)n * (i + 1) / parts);
        wait[i] = g_w[i].done;
        SetEvent(g_w[i].go);
    }
    fn((int)((long long)n * g_nw / parts), n, ctx);
    WaitForMultipleObjects(g_nw, wait, TRUE, INFINITE);
}

int par_threads(void) { if (g_nw < 0) start(); return g_nw + 1; }
