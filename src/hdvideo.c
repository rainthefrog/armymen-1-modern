/* ============================================================================================
 * hdvideo.c - upscaled videos
 *
 * The game plays its Smacker videos (AVI\*.smk) through smackw32.dll: SmackOpen, SmackDoFrame /
 * SmackNextFrame per frame, SmackClose. Upscaled versions are H.264 MP4 files in <game>\hd\AVI\ with
 * the same base name (tooling/scripts/convert_videos.sh; the upscaler writes Smacker 4, which the
 * game's 1998 smackw32.dll cannot read). When one exists, it is decoded with Media Foundation in step
 * with the game's frame number (sound stays the game's), and the display shows that frame instead of
 * the game's 640x480 picture (present.c, video view only).
 * Hooks: the exe's imports _SmackOpen@12, _SmackDoFrame@4, _SmackClose@4.
 * [display] hd_videos = 1
 * ============================================================================================ */
#define COBJMACROS
#include "common.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

typedef struct {
    uint32_t Version, Width, Height, Frames, MSPerFrame, SmackerType, LargestInTrack[7];
    uint32_t tablesize, codesize, absize, detailsize, typesize, TrackType[7], extra, NewPalette;
    BYTE Palette[772];
    uint32_t PalType, FrameNum;
} Smack;

typedef Smack *(WINAPI *OpenFn)(const char *name, uint32_t flags, uint32_t extrabuf);
typedef uint32_t (WINAPI *DoFrameFn)(Smack *s);
typedef void (WINAPI *CloseFn)(Smack *s);
static OpenFn real_open; static DoFrameFn real_doframe; static CloseFn real_close;

static Smack *g_game;
static IMFSourceReader *g_reader;
static int g_next;                       /* index of the next frame the reader returns */
static int g_w, g_h, g_stride, g_have, g_mf;
static uint32_t *g_rgb; static size_t g_rgb_cap;

static void close_hd(void)
{
    present_lock();
    g_have = 0;
    present_unlock();
    if (g_reader) { IMFSourceReader_Release(g_reader); g_reader = NULL; }
    g_game = NULL;
}

static int open_hd(const char *path)
{
    if (!g_mf) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { mlog("hdvideo: Media Foundation not available"); g_mf = -1; }
        else g_mf = 1;
    }
    if (g_mf < 0) return 0;
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH);
    IMFAttributes *attr = NULL;
    IMFMediaType *mt = NULL, *cur = NULL;
    int ok = 0;
    if (SUCCEEDED(MFCreateAttributes(&attr, 1))
        && SUCCEEDED(IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))
        && SUCCEEDED(MFCreateSourceReaderFromURL(w, attr, &g_reader))
        && SUCCEEDED(MFCreateMediaType(&mt))
        && SUCCEEDED(IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video))
        && SUCCEEDED(IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32))
        && SUCCEEDED(IMFSourceReader_SetCurrentMediaType(g_reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt))
        && SUCCEEDED(IMFSourceReader_GetCurrentMediaType(g_reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
        UINT64 size = 0;
        IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &size);
        g_w = (int)(size >> 32); g_h = (int)(size & 0xffffffff);
        UINT32 stride = 0;
        g_stride = SUCCEEDED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &stride)) ? (int)(INT32)stride : g_w * 4;
        ok = g_w > 0 && g_h > 0 && g_w <= 4096 && g_h <= 4096;
    }
    if (cur) IMFMediaType_Release(cur);
    if (mt) IMFMediaType_Release(mt);
    if (attr) IMFAttributes_Release(attr);
    if (!ok && g_reader) { IMFSourceReader_Release(g_reader); g_reader = NULL; }
    g_next = 0;
    return ok;
}

/* the open Smacker video (also when it is shown as the upscaled MP4, where the game's own frame blit - which
 * view.c uses to know "a video is showing" - does not happen; user 2026-11-17: the intro needed Esc) */
static volatile Smack *g_smack_open;
int hdvideo_playing(void) { return g_smack_open != NULL; }

static Smack *WINAPI hd_SmackOpen(const char *name, uint32_t flags, uint32_t extrabuf)
{
    Smack *s = real_open(name, flags, extrabuf);
    if (s) g_smack_open = s;                     /* a video is playing (modern.c: any key / click skips it) */
    if (!s || !g_cfg.hd_videos || !name || (uintptr_t)name < 0x10000 || (flags & 0x1000)) return s;
    if (g_reader) close_hd();
    const char *file = strrchr(name, '\\');
    file = file ? file + 1 : name;
    char rel[MAX_PATH], path[MAX_PATH];
    wsprintfA(rel, "hd\\AVI\\%s", file);
    char *dot = strrchr(rel, '.');
    if (dot) lstrcpyA(dot, ".mp4");
    hdart_game_path(path, MAX_PATH, rel);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return s;
    if (!open_hd(path)) { mlog("hdvideo: %s could not be opened", path); return s; }
    g_game = s;
    mlog("hdvideo: %s -> %s (%dx%d)", file, path, g_w, g_h);
    return s;
}

/* Reads frames up to the game's current one and shows it. */
static void show_frame(int target)
{
    IMFSample *keep = NULL;
    while (g_reader && g_next <= target) {
        DWORD stream, fl = 0; LONGLONG ts;
        IMFSample *smp = NULL;
        if (FAILED(IMFSourceReader_ReadSample(g_reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &fl, &ts, &smp))
            || (fl & MF_SOURCE_READERF_ENDOFSTREAM)) { if (smp) IMFSample_Release(smp); break; }
        if (!smp) continue;
        if (g_next++ == target) { keep = smp; break; }
        IMFSample_Release(smp);
    }
    if (!keep) return;
    IMFMediaBuffer *buf = NULL;
    BYTE *p = NULL; DWORD len = 0;
    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(keep, &buf)) && SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) {
        int stride = g_stride ? g_stride : g_w * 4;
        BYTE *top = stride < 0 ? p + (size_t)(-stride) * (g_h - 1) : p;
        present_lock();
        size_t need = (size_t)g_w * g_h;
        if (need > g_rgb_cap) { free(g_rgb); g_rgb = malloc(need * 4); g_rgb_cap = g_rgb ? need : 0; }
        if (g_rgb && (size_t)abs(stride) * g_h <= len) {
            for (int y = 0; y < g_h; y++) {
                const uint32_t *src = (const uint32_t *)(top + (ptrdiff_t)stride * y);
                uint32_t *dst = g_rgb + (size_t)y * g_w;
                for (int x = 0; x < g_w; x++) dst[x] = src[x] | 0xFF000000u;
            }
            g_have = 1;
        }
        present_unlock();
        IMFMediaBuffer_Unlock(buf);
    }
    if (buf) IMFMediaBuffer_Release(buf);
    IMFSample_Release(keep);
}

static uint32_t WINAPI hd_SmackDoFrame(Smack *s)
{
    uint32_t r = real_doframe(s);
    if (s == g_game && g_reader) show_frame((int)s->FrameNum);
    return r;
}

static void WINAPI hd_SmackClose(Smack *s)
{
    if (s == g_game) close_hd();
    if (s == g_smack_open) g_smack_open = NULL;
    real_close(s);
}

void hdvideo_install(void)
{
    if (!g_cfg.hd_videos) return;
    real_open = iat_hook("smackw32.dll", "_SmackOpen@12", (void *)hd_SmackOpen);
    real_doframe = iat_hook("smackw32.dll", "_SmackDoFrame@4", (void *)hd_SmackDoFrame);
    real_close = iat_hook("smackw32.dll", "_SmackClose@4", (void *)hd_SmackClose);
    mlog("hook  hdvideo: Smacker imports %s", real_open && real_doframe && real_close ? "installed" : "NOT all installed");
}

/* present.c (under the present lock): the current HD video frame, if a video with one is showing. */
const uint32_t *hdvideo_frame(int *w, int *h)
{
    if (g_have != 1 || !g_rgb) return NULL;
    *w = g_w; *h = g_h;
    return g_rgb;
}
