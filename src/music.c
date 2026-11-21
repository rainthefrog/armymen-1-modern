/*
 * music.c - Army Men II background music (2026-11-21, [audio] am2_music=1, F10 "Army Men II music", Mod Settings
 * AUDIO/UI "AM II music"). Default 0 = the original music.
 *
 * The game's music is ten streams in Audio\streams.asd (same "V0.0" archive as sounds.asd, see audio.c; ids =
 * the original game table): 2000 TITLE_MUSIC, 2001/2002 ALPINE1/2 (also Boot Camp), 2003/2004 DESERT1/2, 2005..2007
 * BAYOU1..3, 2008 WIN, 2009 LOSE - all 11025 Hz 8-bit mono. Campaign.cpn / BootCamp.cpn give each mission a low
 * and a high intensity stream (MUSIC lo hi; the scripts can switch) and each campaign one for its map screen.
 * A music stream (play flag 8 = SND_MUSIC_FLAG; flag 4 = SND_VOICE_FLAG is the radio-voice module at 0x468b50..,
 * same design, not touched) is a looping DirectSound buffer of 6 x 0.2 s refilled from the file:
 *     FUN_00410d50  create (ECX = file id, EDX = sound id): wave record via FUN_004107b0 (call 0x410df1), the
 *                   stream slot (table *0x4e3b0c, count 0x4e3b1c, 0x48 bytes; +0xc handle, +0x14 buffer) gets
 *                   +0x18 = rec + 0x1c (format, KEPT AS A POINTER), +0x1c / +0x20 = rec +0x14 (data start / read
 *                   position), +0x24 = rec +0x18 (data bytes), chunk = rec +0x24 (bytes per second) / 5;
 *                   first fill FUN_00411df0 (call 0x411e4f)
 *     FUN_00411250  play (record: call 0x4112aa; needs rec +0x10 FILE* != 0), timeSetEvent 50 ms
 *     refill        FUN_00411f90 (call 0x411ff9) = fseek(rec +0x10, slot +0x20), fread, slot +0x20 = ftell; at the
 *                   end of the data: silence, then with the loop flag 0x800 a rewind FUN_00411120 (call 0x411175)
 *                   / FUN_00410ff0 (call 0x411043): slot +0x20 = +0x1c, FUN_00411df0 refills from the start.
 * The record cache (FUN_00410190 / FUN_004103f0) is keyed by sound id and never sees our records.
 * Kept: all of the above, the game's volume / pan / loop / stop handling, ddraw.c's pause of the music / voice
 * streams during a modal loop (it works on the DirectSound buffers).
 * Changed, while am2_music is on and <game>\am2_music\map.txt has a line for the stream (and the mission):
 * the six FUN_004107b0 calls above (music module only) get a copy of the stream's record whose FILE* (opened with the exe's own
 * fopen 0x470fc0, so its fseek / fread / ftell work on it), data start, size and format are those of the AM II
 * wav - the game then streams the AM II track from disk exactly as it streams its own. The choice is made when a
 * stream is created (call 0x410df1) and kept for that stream id until it is created again, so switching the
 * option or a new mission never mixes two files in one playing stream; switching takes effect the next time a
 * track starts (next screen / mission). A missing / bad file = the original, logged.
 * Pack: scripts/am2_music.py (track table, loudness matched to the AM1 music).
 */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

#define WREC_SIZE       0x30         /* wave record (audio.c): +0 id, +0x10 FILE*, +0x14 data start, +0x18 bytes, +0x1c format */
#define WREC_ID         0x00
#define WREC_FILE       0x10
#define WREC_START      0x14
#define WREC_BYTES      0x18
#define WREC_FMT        0x1c
#define CRT_FOPEN       0x470fc0     /* the exe's own fopen (cdecl: path, mode) */
#define GAME            (*(BYTE **)0x4b3f70)
#define GAME_MISSION(g) (*(int *)((g) + 0x74))   /* gpGame->MissionID: campaign 1..28, Boot Camp 50 */
#define MUS_FIRST       2000
#define MUS_COUNT       10
#define MAX_LINES       64
#define MAX_TRACKS      32

#pragma pack(push, 1)
typedef struct { WORD tag, ch; DWORD rate, bytes_per_s; WORD align, bits, cbsize; } WaveFmt;   /* WAVEFORMATEX */
#pragma pack(pop)

static const char *const NAMES[MUS_COUNT] = {
    "TITLE_MUSIC", "ALPINE1_MUSIC", "ALPINE2_MUSIC", "DESERT1_MUSIC", "DESERT2_MUSIC",
    "BAYOU1_MUSIC", "BAYOU2_MUSIC", "BAYOU3_MUSIC", "WIN_MUSIC", "LOSE_MUSIC",
};

typedef struct {                     /* one AM II file, opened once, never closed (a playing stream may read it) */
    char name[64];
    int state;                       /* 0 = not tried, 1 = open, -1 = missing / bad */
    void *file;                      /* the exe's FILE* */
    DWORD start, bytes;
    WaveFmt fmt;
} Track;

typedef struct {                     /* map.txt line: stream id, missions (lo..hi ranges, none = any), track */
    int id, nr, lo[8], hi[8];
    int track;
} Line;

typedef struct { int id, track; BYTE rec[WREC_SIZE]; } Rec;   /* a record copy per (stream id, track); never freed */

static Track g_tracks[MAX_TRACKS];
static int g_ntracks;
static Line g_lines[MAX_LINES];
static int g_nlines;
static int g_map_state;              /* 0 = not read, 1 = read, -1 = no map.txt */
static Rec g_recs[MUS_COUNT * MAX_TRACKS];
static int g_nrecs;
static Rec *volatile g_cur[MUS_COUNT];   /* the record the stream id plays from now (NULL = the original) */

static int track_index(const char *name)
{
    for (int i = 0; i < g_ntracks; i++) if (!lstrcmpiA(g_tracks[i].name, name)) return i;
    if (g_ntracks == MAX_TRACKS) return -1;
    lstrcpynA(g_tracks[g_ntracks].name, name, sizeof g_tracks[0].name);
    return g_ntracks++;
}

/* "<stream id> <missions> <file>"; missions = * or 1-3,8 */
static void read_map(void)
{
    char path[MAX_PATH], line[256];
    hdart_game_path(path, sizeof path, "am2_music\\map.txt");
    FILE *f = fopen(path, "r");
    if (!f) { g_map_state = -1; mlog("music: am2_music on, but %s not found - original music", path); return; }
    while (fgets(line, sizeof line, f) && g_nlines < MAX_LINES) {
        int id;
        char ms[64], name[64];
        if (line[0] == '#' || sscanf(line, "%d %63s %63s", &id, ms, name) != 3) continue;
        if (id < MUS_FIRST || id >= MUS_FIRST + MUS_COUNT || name[0] == '#') continue;
        Line *l = &g_lines[g_nlines];
        memset(l, 0, sizeof *l);
        l->id = id;
        if (ms[0] != '*')
            for (char *p = ms; *p && l->nr < 8;) {
                int a = (int)strtol(p, &p, 10), b = a;
                if (*p == '-') b = (int)strtol(p + 1, &p, 10);
                l->lo[l->nr] = a; l->hi[l->nr] = b; l->nr++;
                if (*p == ',') p++; else break;
            }
        if ((l->track = track_index(name)) < 0) continue;
        g_nlines++;
    }
    fclose(f);
    g_map_state = 1;
    mlog("music: %s: %d line(s), %d Army Men II track(s)", path, g_nlines, g_ntracks);
}

/* Header of a RIFF PCM wav: data start / size / format (the data itself is streamed by the game). */
static int open_track(Track *t)
{
    char rel[96], path[MAX_PATH];
    wsprintfA(rel, "am2_music\\%s", t->name);
    hdart_game_path(path, sizeof path, rel);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { mlog("music: %s missing - original music", path); return 0; }
    DWORD size = GetFileSize(h, NULL), got = 0, p = 12;
    BYTE hd[12], ck[8];
    WaveFmt w = { 0 };
    int ok = ReadFile(h, hd, 12, &got, NULL) && got == 12 && !memcmp(hd, "RIFF", 4) && !memcmp(hd + 8, "WAVE", 4), have_fmt = 0;
    while (ok) {
        if (SetFilePointer(h, p, NULL, FILE_BEGIN) != p || !ReadFile(h, ck, 8, &got, NULL) || got != 8) { ok = 0; break; }
        DWORD n = *(DWORD *)(ck + 4);
        if (!memcmp(ck, "fmt ", 4) && n >= 16) { ok = ReadFile(h, &w, 16, &got, NULL) && got == 16; have_fmt = 1; }
        else if (!memcmp(ck, "data", 4)) { t->start = p + 8; t->bytes = n < size - p - 8 ? n : size - p - 8; break; }
        p += 8 + n + (n & 1);
    }
    CloseHandle(h);
    if (!ok || !have_fmt || !t->bytes || w.tag != 1 || (w.ch != 1 && w.ch != 2) || (w.bits != 8 && w.bits != 16) ||
        w.rate < 8000 || w.rate > 48000) {
        mlog("music: %s is not a PCM wav (8/16-bit, mono/stereo, 8..48 kHz) - original music", path);
        return 0;
    }
    w.align = (WORD)(w.ch * w.bits / 8);
    w.bytes_per_s = w.rate * w.align;
    w.cbsize = 0;
    t->bytes -= t->bytes % w.align;
    t->fmt = w;
    t->file = ((void *(__cdecl *)(const char *, const char *))CRT_FOPEN)(path, "rb");
    if (!t->file) { mlog("music: %s could not be opened - original music", path); return 0; }
    mlog("music: %s opened (%lu Hz %d-bit %s, %.1f s)", rel, w.rate, w.bits, w.ch == 2 ? "stereo" : "mono",
         (double)t->bytes / w.bytes_per_s);
    return 1;
}

static int mission_now(void) { BYTE *g = GAME; return g ? GAME_MISSION(g) : -1; }

static Track *choose(int id, int mission)
{
    if (g_map_state == 0) read_map();
    if (g_map_state < 0) return NULL;
    for (int i = 0; i < g_nlines; i++) {
        Line *l = &g_lines[i];
        if (l->id != id) continue;
        int hit = !l->nr;
        for (int k = 0; k < l->nr && !hit; k++) hit = mission >= l->lo[k] && mission <= l->hi[k];
        if (!hit) continue;
        Track *t = &g_tracks[l->track];
        if (t->state == 0) t->state = open_track(t) ? 1 : -1;
        return t->state > 0 ? t : NULL;            /* first matching line decides (a bad file = the original) */
    }
    return NULL;
}

static Rec *record_for(int id, int track, const BYTE *real)
{
    for (int i = 0; i < g_nrecs; i++) if (g_recs[i].id == id && g_recs[i].track == track) return &g_recs[i];
    if (g_nrecs == (int)(sizeof g_recs / sizeof g_recs[0])) return NULL;
    Rec *r = &g_recs[g_nrecs];
    Track *t = &g_tracks[track];
    r->id = id;
    r->track = track;
    memcpy(r->rec, real, WREC_SIZE);
    *(void **)(r->rec + WREC_FILE) = t->file;
    *(DWORD *)(r->rec + WREC_START) = t->start;
    *(DWORD *)(r->rec + WREC_BYTES) = t->bytes;
    memcpy(r->rec + WREC_FMT, &t->fmt, sizeof t->fmt);
    g_nrecs++;                                       /* published after it is complete (the refill may run on a timer thread) */
    return r;
}

enum { SITE_CREATE, SITE_PLAY, SITE_FILL, SITE_REWIND };

/* After FUN_004107b0 at one of the stream calls: *out = the stream's wave record. */
void __cdecl music_wave_record(BYTE **out, int site)
{
    BYTE *r = *out;
    if (!r || r == (BYTE *)-1) return;
    int id = (int)*(DWORD *)(r + WREC_ID);
    if (id < MUS_FIRST || id >= MUS_FIRST + MUS_COUNT) return;
    int i = id - MUS_FIRST;
    if (site == SITE_CREATE) {
        int m = mission_now();
        Track *t = g_cfg.am2_music ? choose(id, m) : NULL;
        Rec *n = t ? record_for(id, (int)(t - g_tracks), r) : NULL;
        g_cur[i] = n;
        if (n) mlog("music: stream %d %s (mission %d) -> Army Men II am2_music\\%s", id, NAMES[i], m, t->name);
        else mlog("music: stream %d %s (mission %d) -> original%s", id, NAMES[i], m, g_cfg.am2_music ? " (no AM II track)" : "");
    }
    Rec *c = g_cur[i];
    if (site == SITE_REWIND) mlog("music: stream %d %s loops (%s)", id, NAMES[i], c ? g_tracks[c->track].name : "original");
    if (c) *out = c->rec;
}

/* One stub per kind of call: ECX = handle (passed on), EDX = BYTE** out, [esp+4] = index out; ret 4. */
#define MUSIC_STUB(name, site)                                                   \
    __asm__(".globl _" #name "\n"                                                \
            "_" #name ":\n"                                                      \
            "    push %edx\n"                                                    \
            "    pushl 8(%esp)\n"                                                \
            "    mov $0x4107b0, %eax\n"                                          \
            "    call *%eax\n"                                                   \
            "    pop %edx\n"                                                     \
            "    push %eax\n"                                                    \
            "    push $" #site "\n"                                              \
            "    push %edx\n"                                                    \
            "    call _music_wave_record\n"                                      \
            "    add $8, %esp\n"                                                 \
            "    pop %eax\n"                                                     \
            "    ret $4\n")
MUSIC_STUB(music_stub_create, 0);
MUSIC_STUB(music_stub_play, 1);
MUSIC_STUB(music_stub_fill, 2);
MUSIC_STUB(music_stub_rewind, 3);
