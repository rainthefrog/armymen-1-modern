/*
 * volume.c - the SFX / Music / Voice sliders apply to EVERY sound (2026-11-26, [audio] volume_all=1, default on).
 * User report: "some SFX are just not affected by the in-game volume settings"; wanted: catch them all.
 *
 * The game (RE 2026-11-26, objdump):
 *   Sliders (percent 0..100, Audio Options menu, saved as AudioSFX / AudioMUSIC / AudioVOICE):
 *     SFX   0x4b2810 (get 0x40fef0, set 0x40ff00)     sound type 1, module 0x40e000..0x4104ff
 *     Music 0x4b2838 (get 0x4123c0, set 0x4123d0)     sound type 8, module 0x410d00..0x4125ff
 *     Voice 0x4d3afc (get 0x46a150, set 0x46a160)     sound type 4, module 0x468000..0x46a3ff
 *     FUN_00402510 (ECX = type) = get slider, FUN_004024d0 (ECX = type, EDX = %) = set slider.
 *   FUN_00403140 (ECX = handle, EDX = %, [esp+4], ret 4): % -> DirectSound centibels: 0 = -10000 (silent), else
 *     (% - 100) * 18 clamped to -1800. Positional sounds (FUN_00402f90 / 00402fe0 / 00403030 / 004030d0) pass
 *     slider * distance% / 100; the resume paths (0x40f5d0 / 40f650 / 40f6d0, 0x411330, 0x469120) and the
 *     "apply to all" loops (0x402690 / 402770 / 402840) pass the slider; a slider change (the setters above) sets
 *     every instance of that type to the slider (distance lost). Other paths set centibels directly
 *     (FUN_00402370, FUN_00402c80: current + offset) or never set a volume at all (buffer stays at 0 = full) -
 *     those are the sounds the user heard ignore the slider.
 *   Buffers: one IDirectSound::CreateSoundBuffer call per module: 0x40f4b3 (effects, FUN_0040f390, a new buffer
 *     per play), 0x410ebc (music streams), 0x468cbb (voice streams). Volume wrappers 0x410500 GetVolume,
 *     0x410510 SetVolume.
 *
 * Changed (all of it only with volume_all=1; 0 = nothing installed, the original game):
 *   - The game's own slider use is neutralised: the 13 slider reads in the play / resume / apply-all code
 *     (hook_call on FUN_00402510, list in volume_install) return 100, and the setters' per-instance loops
 *     (calls of FUN_00403140 at 0x40ff38 / 0x412408 / 0x46a198) pass 100. The menu, the save code and every
 *     other reader still see the real slider, so the menu shows and saves it as before.
 *   - Every buffer the three create sites make is remembered with its bus (by the call's return address).
 *     IDirectSoundBuffer SetVolume / Play / GetVolume / Release are patched IN PLACE in the real vtable (as the
 *     DI keyboard in controls.c); other buffers (primary, Smacker videos) pass straight through.
 *     SetVolume stores the game's value (now without the slider) and sets value + slider attenuation;
 *     Play applies it first (a buffer the game never set plays at 0 + slider); GetVolume returns the game's value,
 *     so its "current + offset" code keeps working. A slider change re-applies every live buffer of that bus.
 *   - Slider curve: amplitude, 2000 * log10(% / 100) centibels (50 % = -6 dB, 10 % = -20 dB, 1 % = -40 dB, 0 =
 *     silent) - the game's own curve only reached -18 dB at 1 %.
 * Kept: the game's distance falloff, pan, loops, its instance tables, the AM II packs (audio.c / music.c).
 * Log: "volume:" lines - hooks, per bus the first plays whose buffer the game never gave a volume (= the
 * sounds that used to ignore the slider; caller address of the Play), slider changes.
 */
#include "common.h"
#include <math.h>

#define BUS_NONE   0
#define BUS_SFX    1
#define BUS_MUSIC  2
#define BUS_VOICE  3
#define BUS_COUNT  4

#define SLIDER_SFX    0x4b2810
#define SLIDER_MUSIC  0x4b2838
#define SLIDER_VOICE  0x4d3afc

#define FN_GET_TYPE_SLIDER  0x402510     /* __fastcall int (int type) */
#define FN_SET_PERCENT      0x403140     /* __fastcall void (int handle, int percent, int unused), ret 4 */

/* return addresses of the three IDirectSound::CreateSoundBuffer calls */
#define CREATE_RET_SFX    0x40f4b6
#define CREATE_RET_MUSIC  0x410ebf
#define CREATE_RET_VOICE  0x468cbe

/* COM vtable slots */
#define DS_CREATE_BUFFER  3              /* IDirectSound::CreateSoundBuffer */
#define DSB_RELEASE       2
#define DSB_GET_VOLUME    6
#define DSB_PLAY          12
#define DSB_SET_VOLUME    15

#define VOL_MIN  (-10000)
#define MAX_BUFFERS 512
#define MAX_VTABLES 4

typedef HRESULT (WINAPI *CreateBufferFn)(void *ds, const void *desc, void **out, void *outer);
typedef ULONG   (WINAPI *ReleaseFn)(void *buf);
typedef HRESULT (WINAPI *GetVolumeFn)(void *buf, LONG *vol);
typedef HRESULT (WINAPI *PlayFn)(void *buf, DWORD r1, DWORD prio, DWORD flags);
typedef HRESULT (WINAPI *SetVolumeFn)(void *buf, LONG vol);
typedef HRESULT (WINAPI *DSCreateFn)(const GUID *dev, void **ds, void *outer);
typedef int  (__fastcall *GetTypeSliderFn)(int type);
typedef void (__fastcall *SetPercentFn)(int handle, int percent, int unused);

typedef struct { void **vt; ReleaseFn release; GetVolumeFn get; PlayFn play; SetVolumeFn set; } Vt;
typedef struct { void *buf; int bus; int set; LONG game; } Buf;

static Vt g_vt[MAX_VTABLES];
static int g_nvt;
static Buf g_buf[MAX_BUFFERS];
static CRITICAL_SECTION g_lock;
static DSCreateFn g_real_ds_create;
static CreateBufferFn g_real_create;
static void **g_ds_vt;
static int g_slider_seen[BUS_COUNT] = { -1, -1, -1, -1 };
static unsigned g_unset_logged[BUS_COUNT];
static unsigned g_creates[BUS_COUNT], g_plays[BUS_COUNT], g_unset_plays[BUS_COUNT];
static int log_count(unsigned n) { return n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000; }
static const char *const BUS_NAME[BUS_COUNT] = { "-", "SFX", "music", "voice" };

static int slider(int bus)
{
    switch (bus) {
    case BUS_SFX:   return *(volatile int *)SLIDER_SFX;
    case BUS_MUSIC: return *(volatile int *)SLIDER_MUSIC;
    case BUS_VOICE: return *(volatile int *)SLIDER_VOICE;
    }
    return 100;
}

static LONG attenuation(int bus)
{
    int s = slider(bus);
    if (s >= 100) return 0;
    if (s <= 0) return VOL_MIN;
    return (LONG)lround(2000.0 * log10(s / 100.0));
}

static LONG final_volume(const Buf *b)
{
    LONG v = (b->set ? b->game : 0) + attenuation(b->bus);
    return v < VOL_MIN ? VOL_MIN : v > 0 ? 0 : v;
}

static Vt *vt_of(void *buf)
{
    void **vt = *(void ***)buf;
    for (int i = 0; i < g_nvt; i++) if (g_vt[i].vt == vt) return &g_vt[i];
    return NULL;
}

static Buf *find(void *buf)
{
    for (int i = 0; i < MAX_BUFFERS; i++) if (g_buf[i].buf == buf) return &g_buf[i];
    return NULL;
}

/* ---- IDirectSoundBuffer replacements (every buffer of the vtable comes here; untracked ones pass through) ---- */

static HRESULT WINAPI vol_set(void *buf, LONG vol)
{
    Vt *t = vt_of(buf);
    EnterCriticalSection(&g_lock);
    Buf *b = find(buf);
    if (b) { b->game = vol; b->set = 1; vol = final_volume(b); }
    LeaveCriticalSection(&g_lock);
    return t->set(buf, vol);
}

static HRESULT WINAPI vol_get(void *buf, LONG *vol)
{
    Vt *t = vt_of(buf);
    HRESULT hr = t->get(buf, vol);
    EnterCriticalSection(&g_lock);
    Buf *b = find(buf);
    if (b && hr == 0 && vol) *vol = b->set ? b->game : 0;
    LeaveCriticalSection(&g_lock);
    return hr;
}

static HRESULT WINAPI vol_play(void *buf, DWORD r1, DWORD prio, DWORD flags)
{
    Vt *t = vt_of(buf);
    EnterCriticalSection(&g_lock);
    Buf *b = find(buf);
    if (b) {
        g_plays[b->bus]++;
        if (!b->set) g_unset_plays[b->bus]++;
        if (log_count(g_plays[b->bus]))
            mlog("volume: %s plays %u (without a game volume %u), buffers made %u, slider %d%%", BUS_NAME[b->bus],
                 g_plays[b->bus], g_unset_plays[b->bus], g_creates[b->bus], slider(b->bus));
        if (!b->set && g_unset_logged[b->bus] < 20) {
            g_unset_logged[b->bus]++;
            mlog("volume: %s buffer played without a game volume (Play called from 0x%06x) - slider applied",
                 BUS_NAME[b->bus], (unsigned)(uintptr_t)__builtin_return_address(0));
        }
        t->set(buf, final_volume(b));
    }
    LeaveCriticalSection(&g_lock);
    return t->play(buf, r1, prio, flags);
}

static ULONG WINAPI vol_release(void *buf)
{
    Vt *t = vt_of(buf);
    ULONG n = t->release(buf);
    if (n == 0) {
        EnterCriticalSection(&g_lock);
        Buf *b = find(buf);
        if (b) b->buf = NULL;
        LeaveCriticalSection(&g_lock);
    }
    return n;
}

static int patch_slot(void **vt, int slot, void *fn)
{
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) return 0;
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
    return 1;
}

static int patch_buffer_vtable(void *buf)
{
    void **vt = *(void ***)buf;
    if (vt_of(buf)) return 1;
    if (g_nvt >= MAX_VTABLES) return 0;
    Vt *t = &g_vt[g_nvt];
    t->vt = vt;
    t->release = (ReleaseFn)vt[DSB_RELEASE];
    t->get = (GetVolumeFn)vt[DSB_GET_VOLUME];
    t->play = (PlayFn)vt[DSB_PLAY];
    t->set = (SetVolumeFn)vt[DSB_SET_VOLUME];
    g_nvt++;                                     /* before patching: another thread may already call through */
    int ok = patch_slot(vt, DSB_RELEASE, (void *)vol_release) && patch_slot(vt, DSB_GET_VOLUME, (void *)vol_get) &&
             patch_slot(vt, DSB_PLAY, (void *)vol_play) && patch_slot(vt, DSB_SET_VOLUME, (void *)vol_set);
    mlog("volume: sound buffer vtable %p %s", (void *)vt, ok ? "patched" : "patch FAILED");
    return ok;
}

/* ---- IDirectSound::CreateSoundBuffer: remember the game's buffers with their bus ---- */

static HRESULT WINAPI vol_create(void *ds, const void *desc, void **out, void *outer)
{
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    HRESULT hr = g_real_create(ds, desc, out, outer);
    int bus = ret == CREATE_RET_SFX ? BUS_SFX : ret == CREATE_RET_MUSIC ? BUS_MUSIC : ret == CREATE_RET_VOICE ? BUS_VOICE : BUS_NONE;
    if (hr != 0 || !out || !*out || bus == BUS_NONE) return hr;
    EnterCriticalSection(&g_lock);
    if (patch_buffer_vtable(*out)) {
        Buf *b = find(NULL);
        if (b) {
            b->buf = *out; b->bus = bus; b->set = 0; b->game = 0;
            g_creates[bus]++;
            vt_of(*out)->set(*out, final_volume(b));
        } else {
            static int full_logged;
            if (!full_logged++) mlog("volume: buffer table full (%d) - a %s buffer keeps the game's volume", MAX_BUFFERS, BUS_NAME[bus]);
        }
    }
    LeaveCriticalSection(&g_lock);
    return hr;
}

static HRESULT WINAPI vol_DirectSoundCreate(const GUID *dev, void **ds, void *outer)
{
    HRESULT hr = g_real_ds_create(dev, ds, outer);
    if (hr == 0 && ds && *ds) {
        void **vt = *(void ***)*ds;
        if (vt != g_ds_vt && vt[DS_CREATE_BUFFER] != (void *)vol_create) {
            g_real_create = (CreateBufferFn)vt[DS_CREATE_BUFFER];
            int ok = patch_slot(vt, DS_CREATE_BUFFER, (void *)vol_create);
            g_ds_vt = vt;
            mlog("volume: IDirectSound::CreateSoundBuffer %s", ok ? "hooked" : "hook FAILED");
        }
    }
    return hr;
}

/* ---- the game's own slider use, neutralised ---- */

int __fastcall volume_game_slider(int type)
{
    (void)type;
    return 100;
}

void __fastcall volume_set_percent_full(int handle, int percent, int unused)
{
    (void)percent;
    ((SetPercentFn)FN_SET_PERCENT)(handle, 100, unused);
}

/* dd_present (game thread): a slider moved -> re-apply every live buffer of that bus */
void volume_frame(void)
{
    if (!g_cfg.volume_all || !g_real_create) return;
    for (int bus = BUS_SFX; bus < BUS_COUNT; bus++) {
        int s = slider(bus);
        if (s == g_slider_seen[bus]) continue;
        if (g_slider_seen[bus] >= 0) mlog("volume: %s slider %d%% -> %ld cB", BUS_NAME[bus], s, attenuation(bus));
        g_slider_seen[bus] = s;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < MAX_BUFFERS; i++) {
            Buf *b = &g_buf[i];
            if (b->buf && b->bus == bus) vt_of(b->buf)->set(b->buf, final_volume(b));
        }
        LeaveCriticalSection(&g_lock);
    }
}

void volume_install(void)
{
    if (!g_cfg.volume_all) { mlog("volume: [audio] volume_all=0 - the game's own volume handling"); return; }
    InitializeCriticalSection(&g_lock);
    g_real_ds_create = (DSCreateFn)iat_hook("dsound.dll", "DirectSoundCreate", (void *)vol_DirectSoundCreate);
    mlog("hook  volume: DirectSoundCreate import %s", g_real_ds_create ? "installed" : "NOT installed");
    if (!g_real_ds_create) return;
    static const uintptr_t reads[] = {
        0x4026be, 0x40278e, 0x402862,             /* apply-to-all loops: voice, music, SFX */
        0x402fa1, 0x402ff1, 0x403040, 0x4030e0,   /* positional SFX: slider * distance */
        0x40f60d, 0x40f68c, 0x40f70c,             /* SFX resume paths */
        0x411348, 0x469138,                       /* music / voice resume */
        0x421017,                                 /* voice: slider * level */
    };
    char name[96];
    for (size_t i = 0; i < sizeof reads / sizeof reads[0]; i++) {
        wsprintfA(name, "volume: slider read (call 0x%06x) -> 100", (unsigned)reads[i]);
        hook_call(name, reads[i], FN_GET_TYPE_SLIDER, (void *)volume_game_slider);
    }
    hook_call("volume: SFX slider set, per instance (call 0x40ff38) -> 100", 0x40ff38, FN_SET_PERCENT, (void *)volume_set_percent_full);
    hook_call("volume: music slider set, per instance (call 0x412408) -> 100", 0x412408, FN_SET_PERCENT, (void *)volume_set_percent_full);
    hook_call("volume: voice slider set, per instance (call 0x46a198) -> 100", 0x46a198, FN_SET_PERCENT, (void *)volume_set_percent_full);
}
