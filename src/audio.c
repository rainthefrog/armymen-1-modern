/*
 * audio.c - Army Men II sound effects (2026-11-21, [audio] am2_sounds=1, F10 "Army Men II sounds", Mod Settings
 * AUDIO/UI "AM II sounds"). Default 0 = the original sounds.
 *
 * The game's effects live in Audio\sounds.asd ("V0.0" archive: u32 count, { u32 id, u32 offset } table in the
 * first 0x800 bytes; each entry = "fmt " chunk (PCMWAVEFORMAT) + "data" chunk, no RIFF header). Ids 1000..1064
 * are the original game table (BAZOOKA_EXPLODE .. TROOPER_DIE8, names below). Every time the game plays an effect
 * it makes a new DirectSound buffer for it in FUN_0040f390 (sfx create: ECX = file id, EDX = sound id, stack =
 * variant index, flags; ret 8):
 *     FUN_0040fe60   offset of the sound in sounds.asd
 *     FUN_00410190   the sound's wave record (cached by id, 0x30 bytes in the table DAT_004e3ae4:
 *                    +0 id, +4 handle, +8 flags, +0xa refs, +0x10 FILE*, +0x14 data offset, +0x18 data bytes,
 *                    +0x1c PCMWAVEFORMAT)
 *     FUN_004107b0   handle -> record pointer           <- call 0x40f42d, hooked (audio_find_wave_stub)
 *     CreateSoundBuffer (flags 0xe2, size = rec +0x18, format = rec +0x1c), Lock,
 *     fseek(rec +0x10, rec +0x14), fread(locked, 1, n, rec +0x10)   <- call 0x40f515, hooked (audio_fread)
 *     Unlock
 * Kept: everything the game does (its record cache, instance table, volume / pan, loops, the fseek).
 * Changed, only while am2_sounds is on and the sound has a file in <game folder>\am2_sounds\map.txt:
 *     FUN_004107b0's result is swapped for a copy of the record with the replacement's size and format (the
 *     game's own cached record is never written), and the following fread fills the buffer from the
 *     replacement instead of sounds.asd. The game never sets the frequency of an effect buffer (sndSetFrequency
 *     0x40fbb0 has no callers), so any rate plays at its own speed.
 * Files are read on first use (any RIFF PCM wav, 8/16-bit, mono/stereo); a missing / bad file = the original,
 * logged once. Voices (voices.asd, FUN_00410d50) and music (streams.asd) do not pass here and stay original.
 * Switching takes effect with the next sound played; a vehicle engine loop already running keeps its sound
 * until it is made again (next vehicle / mission).
 * Pack: scripts/am2_sounds.py (mapping table, conversion to 22050 Hz 16-bit mono, loudness matched to AM1).
 */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

#define SFX_FIND_WAVE   0x4107b0     /* FUN_004107b0: ECX = wave handle, EDX = BYTE** record out, [esp+4] = index out; ret 4; 2 = none */
#define CRT_FREAD       0x470e00     /* the exe's own fread (cdecl: buf, size, count, FILE*) */
#define WREC_SIZE       0x30         /* wave record */
#define WREC_ID         0x00         /* sound id (game table) */
#define WREC_BYTES      0x18         /* PCM data bytes = DirectSound buffer size */
#define WREC_FMT        0x1c         /* PCMWAVEFORMAT; the game passes rec + 0x1c as DSBUFFERDESC.lpwfx */
#define SND_FIRST       1000         /* sounds.asd ids */
#define SND_COUNT       65

#pragma pack(push, 1)
typedef struct { WORD tag, ch; DWORD rate, bytes_per_s; WORD align, bits, cbsize; } WaveFmt;   /* WAVEFORMATEX */
#pragma pack(pop)

typedef struct {
    int state;                       /* 0 = not tried, 1 = loaded, -1 = no / bad file */
    BYTE *pcm;
    DWORD len;
    WaveFmt fmt;
    BYTE rec[WREC_SIZE];             /* the copy handed to FUN_0040f390 */
    unsigned plays, orig_plays;
} Sub;

static const char *const NAMES[SND_COUNT] = {
    "BAZOOKA_EXPLODE", "MORTAR_EXPLODE", "GRENADE_EXPLODE", "MINE_EXPLODE", "BIG_EXPLOSION", "BARREL_EXPLODE",
    "ROCK_HIT", "WATER_EXPLODE", "BULLET_HIT_OBJECT", "WATER_HIT", "DESTROYED", "BULLET_HIT_TROOPER",
    "EXPLODE_TROOPER", "FLAME_HIT_TROOPER", "BULLET_HIT_SARGE", "SQUASH_SARGE", "SQUASH_TROOPER", "HIT_TROOPER1",
    "HIT_TROOPER2", "VEHICLE_HIGH_IMPACT", "VEHICLE_LOW_IMPACT", "BULLET_HIT_VEHICLE", "TRUCK_FULL_LOOP",
    "HT_FULL_LOOP", "JEEP_FULL_LOOP", "TANK_FULL_LOOP", "FLAME_BEGIN", "FLAME_LOOP", "FLAME_END", "HEAVY_MG",
    "LIGHT_MG", "MEDIUM_MG", "CANNON", "BAZOOKA", "MORTAR", "MORTAR_WHISTLE", "GRENADE", "RIFLE", "TOWER_GUN",
    "PLANE_SOUND", "WEAPON_ACTIVATE", "STEPS", "SWAMP_STEPS", "DESERT_SOUND1", "DESERT_SOUND2", "BAYOU_SOUND1",
    "BAYOU_SOUND2", "ALPINE_SOUND1", "ALPINE_SOUND2", "BUTTONSPRINGIN", "CHOPPER_LOOP", "AIR_RAID_LOOP",
    "CHUGGA_LOOP", "TICKING_LOOP", "ALARM_LOOP", "SARGE_FULLY_LOADED", "PORTAL", "TROOPER_DIE1", "TROOPER_DIE2",
    "TROOPER_DIE3", "TROOPER_DIE4", "TROOPER_DIE5", "TROOPER_DIE6", "TROOPER_DIE7", "TROOPER_DIE8",
};

static Sub g_sub[SND_COUNT];
static char g_file[SND_COUNT][64];   /* from map.txt; "" = keep the original */
static int g_map_state;              /* 0 = not read, 1 = read, -1 = no map.txt */
static Sub *g_pending;               /* set by the record hook, used by the fread right after it */

static void read_map(void)
{
    char path[MAX_PATH], line[256];
    hdart_game_path(path, sizeof path, "am2_sounds\\map.txt");
    FILE *f = fopen(path, "r");
    if (!f) { g_map_state = -1; mlog("audio: am2_sounds on, but %s not found - original sounds", path); return; }
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        int id;
        char name[64];
        if (line[0] == '#' || sscanf(line, "%d %63s", &id, name) != 2) continue;
        if (id < SND_FIRST || id >= SND_FIRST + SND_COUNT || name[0] == '#') continue;
        lstrcpynA(g_file[id - SND_FIRST], name, sizeof g_file[0]);
        n++;
    }
    fclose(f);
    g_map_state = 1;
    mlog("audio: %s: %d Army Men II sound(s) mapped", path, n);
}

/* RIFF PCM wav -> s->pcm / fmt, or 0 */
static int load_wav(Sub *s, const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(h, NULL), got = 0;
    BYTE *d = size >= 44 && size < (16u << 20) ? malloc(size) : NULL;
    int ok = d && ReadFile(h, d, size, &got, NULL) && got == size;
    CloseHandle(h);
    if (!ok || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { free(d); return 0; }
    const BYTE *fmt = NULL, *data = NULL;
    DWORD flen = 0, dlen = 0;
    for (DWORD p = 12; p + 8 <= size;) {
        DWORD n = *(DWORD *)(d + p + 4);
        if (n > size - p - 8) n = size - p - 8;
        if (!memcmp(d + p, "fmt ", 4)) { fmt = d + p + 8; flen = n; }
        if (!memcmp(d + p, "data", 4)) { data = d + p + 8; dlen = n; }
        p += 8 + n + (n & 1);
    }
    WaveFmt w = { 0 };
    if (fmt && flen >= 16) memcpy(&w, fmt, 16);
    if (!data || !dlen || w.tag != 1 || (w.ch != 1 && w.ch != 2) || (w.bits != 8 && w.bits != 16) || w.rate < 4000 || w.rate > 96000) {
        free(d);
        return 0;
    }
    w.align = (WORD)(w.ch * w.bits / 8);
    w.bytes_per_s = w.rate * w.align;
    w.cbsize = 0;
    dlen -= dlen % w.align;
    s->pcm = malloc(dlen);
    if (!s->pcm) { free(d); return 0; }
    memcpy(s->pcm, data, dlen);
    s->len = dlen;
    s->fmt = w;
    free(d);
    return 1;
}

static Sub *substitute(int id)
{
    if (g_map_state == 0) read_map();
    int i = id - SND_FIRST;
    if (g_map_state < 0 || !g_file[i][0]) return NULL;
    Sub *s = &g_sub[i];
    if (s->state == 0) {
        char rel[96], path[MAX_PATH];
        wsprintfA(rel, "am2_sounds\\%s", g_file[i]);
        hdart_game_path(path, sizeof path, rel);
        s->state = load_wav(s, path) ? 1 : -1;
        if (s->state > 0)
            mlog("audio: sound %d %s -> %s loaded (%lu Hz %d-bit %s, %.2f s)", id, NAMES[i], rel, s->fmt.rate, s->fmt.bits,
                 s->fmt.ch == 2 ? "stereo" : "mono", (double)s->len / s->fmt.bytes_per_s);
        else
            mlog("audio: sound %d %s: %s missing or not a PCM wav - original sound", id, NAMES[i], path);
    }
    return s->state > 0 ? s : NULL;
}

static int log_count(unsigned n) { return n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000; }

/* After FUN_004107b0 (call 0x40f42d): *out = the wave record about to be turned into a DirectSound buffer. */
void __cdecl audio_wave_record(BYTE **out)
{
    static int last = -1;
    g_pending = NULL;
    if (g_cfg.am2_sounds != last) {
        last = g_cfg.am2_sounds;
        mlog("audio: Army Men II sounds %s ([audio] am2_sounds=%d)", last ? "on" : "off", last);
    }
    BYTE *r = *out;
    if (!r || r == (BYTE *)-1) return;
    int id = (int)*(DWORD *)(r + WREC_ID);
    if (id < SND_FIRST || id >= SND_FIRST + SND_COUNT) return;
    Sub *slot = &g_sub[id - SND_FIRST];
    Sub *s = g_cfg.am2_sounds ? substitute(id) : NULL;
    if (!s) {
        if (log_count(++slot->orig_plays))
            mlog("audio: sound %d %s played (original, %u)", id, NAMES[id - SND_FIRST], slot->orig_plays);
        return;
    }
    memcpy(s->rec, r, WREC_SIZE);
    *(DWORD *)(s->rec + WREC_BYTES) = s->len;
    memcpy(s->rec + WREC_FMT, &s->fmt, sizeof s->fmt);
    *out = s->rec;
    g_pending = s;
    if (log_count(++s->plays))
        mlog("audio: sound %d %s played (Army Men II %s, %u)", id, NAMES[id - SND_FIRST], g_file[id - SND_FIRST], s->plays);
}

__asm__(".globl _audio_find_wave_stub\n"
        "_audio_find_wave_stub:\n"
        "    push %edx\n"                                /* BYTE** record out */
        "    pushl 8(%esp)\n"                            /* the index out pointer */
        "    mov $0x4107b0, %eax\n"                      /* SFX_FIND_WAVE (ECX = handle, unchanged), pops 4 */
        "    call *%eax\n"
        "    pop %edx\n"
        "    push %eax\n"
        "    push %edx\n"
        "    call _audio_wave_record\n"
        "    add $4, %esp\n"
        "    pop %eax\n"
        "    ret $4\n");

/* The fread at 0x40f515 (into the locked DirectSound buffer). */
typedef size_t (__cdecl *FreadFn)(void *, size_t, size_t, void *);
size_t __cdecl audio_fread(void *buf, size_t size, size_t count, void *file)
{
    Sub *s = g_pending;
    g_pending = NULL;
    if (!s) return ((FreadFn)CRT_FREAD)(buf, size, count, file);
    size_t n = size * count, c = n < s->len ? n : s->len;
    memcpy(buf, s->pcm, c);
    if (c < n) memset((BYTE *)buf + c, s->fmt.bits == 8 ? 0x80 : 0, n - c);
    return count;
}
