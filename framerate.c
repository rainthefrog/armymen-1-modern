/* ============================================================================================
 * framerate.c - the game behaving the same at 120 / 144 Hz (or a frame cap) as at 60 Hz
 * (2026-11-17, user: "support monitors with 120"; audit of the exe's per-frame math)
 *
 * The game steps everything by the frame time dt (game+0x4c, ms, capped at 66) and keeps a running total
 * (game+0x44). Most of it is exact at any dt (movement keeps fractions in floats); these were not:
 *
 *   GRAVITY  UpdateProjectile 0x436076: vz -= (dt * 400) / 1000 in whole numbers. At 60 Hz (16/17 ms)
 *            that is 6 per frame; at 144 Hz (7 ms) 2 per frame = 25 % less gravity, throws fly further.
 *            Now the exact step: floor(A * G / 1000) - floor((A - dt) * G / 1000), A = game+0x44, which
 *            adds up to exactly G per second at any frame rate. G = 375 = what the measured throws at 60 Hz
 *            fitted (controls.c lob_gravity uses the same value, so the throw preview stays right).
 *   ANIMATION AnimUpdate 0x415200: an animation frame ends when last + frame_time <= now, then last = now
 *            - the remainder is dropped, so each frame lasts frame_time rounded UP to a whole game frame:
 *            50 ms took 50 at 60 Hz, 41.7 at 120 Hz (walk cycles, shooting, throwing ran faster), 55.6 at
 *            144 Hz. Now last advances on the 60 Hz grid: last += ceil(frame_time / 16.67) x 16.67 (at
 *            least one 60 Hz frame), back in step with now when it falls more than 4 frames behind - the
 *            same cadence as at 60 Hz at any frame rate.
 * Both are code patches through small stubs (VirtualAlloc'd), each checking the original bytes first.
 * Not changed (smaller, see notes/armymen1-display.md): vehicle turn ticks / acceleration, driverless
 * coasting, staggered explosions, map object animations, classic camera.
 * ============================================================================================ */
#include "common.h"

#define GAME           (*(BYTE **)0x4b3f70)
#define GAME_DT(g)     (*(DWORD *)((g) + 0x4c))      /* this frame's step, ms */
#define GAME_TIME(g)   (*(DWORD *)((g) + 0x44))      /* running total of the steps */
#define GRAVITY_EXACT  375                           /* per second (see above) */
#define FRAME60_X3     50                            /* one 60 Hz frame = 50/3 ms */

static int g_grav_on;
int framerate_gravity(void) { return g_grav_on ? GRAVITY_EXACT : 0; }   /* controls.c: the throw preview (0 = not patched) */

/* gravity for this frame: the exact difference of the running totals */
static int __cdecl gravity_step(void)
{
    BYTE *g = GAME;
    if (!g) return 6;
    unsigned long long a = GAME_TIME(g), dt = GAME_DT(g);
    if (dt > a) dt = a;
    return (int)(a * GRAVITY_EXACT / 1000 - (a - dt) * GRAVITY_EXACT / 1000);
}

/* animation: the new "frame started at" time for an object whose frame (frame_ms long) just ended */
static DWORD __cdecl anim_next(BYTE *obj, int frame_ms, DWORD now)
{
    DWORD last = *(DWORD *)(obj + 0x8c);
    int q = frame_ms <= 0 ? 1 : (frame_ms * 3 + FRAME60_X3 - 1) / FRAME60_X3;     /* whole 60 Hz frames, >= 1 */
    DWORD next = last + (DWORD)((q * FRAME60_X3 + 1) / 3);
    if ((LONG)(now - next) > 4 * 17 || (LONG)(next - now) > 17 * 8) next = now;      /* far off: in step again */
    return next;
}

static BYTE *stub_alloc(size_t n) { return VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }
static void put_rel(BYTE *at, uintptr_t target) { int32_t r = (int32_t)(target - ((uintptr_t)at + 4)); memcpy(at, &r, 4); }

void framerate_install(void)
{
    if (!GetPrivateProfileIntA("display", "framerate_fix", 1, mod_ini_path())) { mlog("framerate: fixes off ([display] framerate_fix=0)"); return; }

    /* GRAVITY 0x436076..0x43608d (23 bytes):
     *   mov edx,[0x6918f4]; mov eax,0x10624dd3; imul edx,[0x4b48b0]; mul edx; shr edx,6   -> edx = step
     *   (then 0x43608d: sub [ecx+0xb2],dx - ecx = the projectile, kept)
     * becomes jmp stub; stub: push ecx; call gravity_step; pop ecx; mov edx,eax; jmp 0x43608d */
    static const BYTE grav_o[] = { 0x8b, 0x15, 0xf4, 0x18, 0x69, 0x00, 0xb8, 0xd3, 0x4d, 0x62, 0x10, 0x0f, 0xaf, 0x15,
                                   0xb0, 0x48, 0x4b, 0x00, 0xf7, 0xe2, 0xc1, 0xea, 0x06 };
    BYTE *s = stub_alloc(32);
    if (s) {
        BYTE *p = s;
        *p++ = 0x51;                                      /* push ecx */
        *p++ = 0xe8; put_rel(p, (uintptr_t)gravity_step); p += 4;
        *p++ = 0x59;                                      /* pop ecx */
        *p++ = 0x89; *p++ = 0xc2;                         /* mov edx, eax */
        *p++ = 0xe9; put_rel(p, 0x43608d); p += 4;
        if (hook_jmp("framerate: projectile gravity exact (0x436076)", 0x436076, grav_o, sizeof grav_o, s)) g_grav_on = 1;
        else VirtualFree(s, 0, MEM_RELEASE);
    }

    /* ANIMATION 0x415337..0x415342 (11 bytes): mov edx,[esp+0x10] (now); inc ebx; mov [esi+0x8c],edx
     * becomes jmp stub. ebx = the frame that just ended, ebp = the frame table (12 bytes per frame, time
     * the first short). eax / ecx / edx are free here (all reloaded after 0x415342).
     * stub: mov edx,[esp+0x10]; movsx ecx,bx; lea ecx,[ecx+ecx*2]; movsx ecx,word [ebp+ecx*4];
     *       push edx; push ecx; push esi; call anim_next; add esp,12; mov [esi+0x8c],eax; inc ebx; jmp 0x415342 */
    static const BYTE anim_o[] = { 0x8b, 0x54, 0x24, 0x10, 0x43, 0x89, 0x96, 0x8c, 0x00, 0x00, 0x00 };
    s = stub_alloc(64);
    if (s) {
        BYTE *p = s;
        static const BYTE head[] = { 0x8b, 0x54, 0x24, 0x10,             /* mov edx,[esp+0x10] */
                                     0x0f, 0xbf, 0xcb,                   /* movsx ecx,bx */
                                     0x8d, 0x0c, 0x49,                   /* lea ecx,[ecx+ecx*2] */
                                     0x0f, 0xbf, 0x4c, 0x8d, 0x00,       /* movsx ecx,word [ebp+ecx*4+0] */
                                     0x52, 0x51, 0x56 };                 /* push edx; push ecx; push esi */
        memcpy(p, head, sizeof head); p += sizeof head;
        *p++ = 0xe8; put_rel(p, (uintptr_t)anim_next); p += 4;
        static const BYTE tail[] = { 0x83, 0xc4, 0x0c,                   /* add esp,12 */
                                     0x89, 0x86, 0x8c, 0x00, 0x00, 0x00, /* mov [esi+0x8c],eax */
                                     0x43 };                             /* inc ebx */
        memcpy(p, tail, sizeof tail); p += sizeof tail;
        *p++ = 0xe9; put_rel(p, 0x415342); p += 4;
        if (!hook_jmp("framerate: animation frames on the 60 Hz grid (0x415337)", 0x415337, anim_o, sizeof anim_o, s)) VirtualFree(s, 0, MEM_RELEASE);
    }
}
