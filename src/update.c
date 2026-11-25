/* ============================================================================================
 * update.c - built-in GitHub Releases update checker & self-updater for ddraw.dll
 *
 * At startup (unless [update] check=0 or running under the automated Wine rig), a background
 * thread checks GitHub Releases for rainthefrog/armymen-1-modern. Because early builds are
 * alpha (v0.1.x) and may be marked as GitHub prereleases, the API query uses
 *   https://api.github.com/repos/<owner>/<repo>/releases?per_page=5
 * and selects the newest non-draft release (falling back cleanly for single-object JSON too).
 *
 * When a newer tag is found with both ddraw.dll and ddraw.dll.sha256 attached:
 *   1. Prompts the player on the UI thread (Yes = update now, No = later).
 *   2. Downloads ddraw.dll.sha256 and ddraw.dll with a small progress box.
 *   3. Verifies the SHA-256 digest before touching the installed DLL.
 *   4. Writes ddraw.dll.new, renames the loaded ddraw.dll -> ddraw.dll.old (Windows allows
 *      renaming a loaded DLL), renames ddraw.dll.new -> ddraw.dll, and restarts Armymen.exe.
 *   5. The next launch deletes ddraw.dll.old automatically.
 *
 * If the folder is read-only or the swap fails, nothing is left broken and the player is
 * offered to open the GitHub Releases page in their browser.
 * ============================================================================================ */
#include "common.h"
#include <wininet.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MOD_VERSION
#define MOD_VERSION "0.1.0"
#endif
#ifndef MOD_REPO_URL
#define MOD_REPO_URL "https://github.com/rainthefrog/armymen-1-modern"
#endif

#define UPD_ASSET    "ddraw.dll"
#define UPD_MAX_ASSETS 16
#define WM_MOD_UPDATE (WM_APP + 0x55)

typedef struct {
    char tag[64];
    char page[512];
    int nassets;
    struct { char name[128]; char url[512]; } asset[UPD_MAX_ASSETS];
} UpdRelease;

/* ---- compact SHA-256 (FIPS 180-4) ---- */
static const uint32_t SHA_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const uint8_t b[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) | ((uint32_t)b[i * 4 + 2] << 8) | b[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b0 = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b0) ^ (a & c) ^ (b0 & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b0; b0 = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b0; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256_calc(const void *data, size_t n, uint8_t out[32])
{
    uint32_t h[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    while (off + 64 <= n) { sha256_block(h, p + off); off += 64; }
    uint8_t tail[128];
    memset(tail, 0, sizeof tail);
    size_t rem = n - off;
    if (rem) memcpy(tail, p + off, rem);
    tail[rem] = 0x80;
    size_t blocks = rem + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[blocks - 1 - i] = (uint8_t)(bits >> (i * 8));
    sha256_block(h, tail);
    if (blocks == 128) sha256_block(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(h[i]);
    }
}

static int sha256_matches(const void *data, size_t n, const char *sumfile, size_t sumlen)
{
    char want[65];
    size_t k = 0;
    for (size_t i = 0; i < sumlen && k < 64; i++) {
        char c = sumfile[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            want[k++] = (char)(c | 0x20);
        else if (k) break;
    }
    if (k != 64) return 0;
    want[64] = 0;
    uint8_t h[32];
    char got[65];
    sha256_calc(data, n, h);
    for (int i = 0; i < 32; i++) snprintf(got + i * 2, 3, "%02x", h[i]);
    return !strcmp(got, want);
}

/* ---- minimal JSON parsing for GitHub Releases API ---- */
static const char *find_key(const char *from, const char *end, const char *key)
{
    size_t k = strlen(key);
    for (const char *p = from; p && p < end; p++) {
        p = strstr(p, key);
        if (!p || p >= end) return NULL;
        if (p == from || p[-1] != '"' || p[k] != '"' || (p - 2 >= from && p[-2] == '\\')) continue;
        const char *q = p + k + 1;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        return q;
    }
    return NULL;
}

static int read_str(const char *p, const char *end, char *out, size_t n)
{
    if (!p || p >= end || *p != '"' || !n) return 0;
    size_t o = 0;
    for (p++; p < end && *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            if (++p >= end || !*p) return 0;
            c = *p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') { for (int i = 0; i < 4 && p + 1 < end && p[1]; i++) p++; c = '?'; }
        }
        if (o + 1 < n) out[o++] = c;
    }
    out[o] = 0;
    return p < end && *p == '"';
}

/* Parse one release object in [from, end). Returns 1 if valid & non-draft. */
static int parse_one_release(const char *from, const char *end, UpdRelease *r)
{
    memset(r, 0, sizeof *r);
    const char *assets = find_key(from, end, "assets");
    const char *top_end = assets ? assets : end;
    const char *draft = find_key(from, top_end, "draft");
    if (draft && !strncmp(draft, "true", 4)) return 0;
    if (!read_str(find_key(from, top_end, "tag_name"), top_end, r->tag, sizeof r->tag)) return 0;
    read_str(find_key(from, top_end, "html_url"), top_end, r->page, sizeof r->page);
    if (!assets) return 1;
    for (const char *p = assets; r->nassets < UPD_MAX_ASSETS;) {
        const char *u = find_key(p, end, "browser_download_url");
        if (!u) break;
        const char *nm = NULL;
        for (const char *q = p; (q = find_key(q, u, "name")) != NULL; q++) nm = q;
        if (nm && read_str(nm, u, r->asset[r->nassets].name, sizeof r->asset[0].name) &&
            read_str(u, end, r->asset[r->nassets].url, sizeof r->asset[0].url))
            r->nassets++;
        p = u + 1;
    }
    return 1;
}

/* Advance over one JSON object starting at '{' (respecting strings/escapes). */
static const char *skip_obj(const char *p, const char *end)
{
    int depth = 0, in_str = 0;
    for (; p < end; p++) {
        if (in_str) {
            if (*p == '\\' && p + 1 < end) p++;
            else if (*p == '"') in_str = 0;
        } else {
            if (*p == '"') in_str = 1;
            else if (*p == '{') depth++;
            else if (*p == '}' && --depth == 0) return p + 1;
        }
    }
    return end;
}

static int parse_releases_json(const char *json, UpdRelease *r)
{
    if (!json) return 0;
    const char *p = json, *end = json + strlen(json);
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end) return 0;
    if (*p == '[') {
        /* Array from /releases?per_page=5: pick first non-draft release */
        for (p++; p < end; ) {
            while (p < end && *p != '{') p++;
            if (p >= end) break;
            const char *obj_end = skip_obj(p, end);
            if (parse_one_release(p, obj_end, r)) return 1;
            p = obj_end;
        }
        return 0;
    }
    return parse_one_release(p, end, r);
}

static int version_parts(const char *v, long part[4])
{
    int n = 0;
    if (*v == 'v' || *v == 'V') v++;
    for (; n < 4; n++) {
        if (*v < '0' || *v > '9') break;
        char *e;
        part[n] = strtol(v, &e, 10);
        v = e;
        if (*v != '.') { n++; break; }
        v++;
    }
    for (int i = n; i < 4; i++) part[i] = 0;
    return n;
}

static int version_newer(const char *tag, const char *current)
{
    long a[4], b[4];
    if (!version_parts(tag, a) || !version_parts(current, b)) return 0;
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

static const char *find_asset(const UpdRelease *r, const char *name)
{
    for (int i = 0; i < r->nassets; i++)
        if (!strcmp(r->asset[i].name, name)) return r->asset[i].url;
    return NULL;
}

static int build_api_url(char *out, size_t n)
{
    char custom[512] = "";
    GetPrivateProfileStringA("update", "url", "", custom, sizeof custom, mod_ini_path());
    if (custom[0]) { snprintf(out, n, "%s", custom); return 1; }
    static const char GH[] = "https://github.com/";
    const char *r = MOD_REPO_URL;
    if (strncmp(r, GH, sizeof GH - 1)) return 0;
    snprintf(out, n, "https://api.github.com/repos/%s/releases?per_page=5", r + sizeof GH - 1);
    return 1;
}

/* ---- progress window & HTTP download ---- */
static HWND g_note;
static void note_show(const wchar_t *text)
{
    if (!g_note) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int w = 420, h = 90;
        g_note = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"",
                                 WS_POPUP | WS_BORDER | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                 (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2, w, h,
                                 NULL, NULL, GetModuleHandleW(NULL), NULL);
        if (g_note) SendMessageW(g_note, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    }
    if (!g_note) return;
    SetWindowTextW(g_note, text);
    UpdateWindow(g_note);
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}
static void note_close(void) { if (g_note) DestroyWindow(g_note); g_note = NULL; }

static char *http_get(const char *url, size_t *len, int show, const char *what)
{
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 1024);
    char *data = NULL;
    *len = 0;
    HINTERNET net = InternetOpenW(L"ArmyMenModern-updater/" L"" MOD_VERSION, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return NULL;
    DWORD to = 12000;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof to);
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof to);
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  (strncmp(url, "https:", 6) ? 0 : INTERNET_FLAG_SECURE);
    HINTERNET h = InternetOpenUrlW(net, w, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, flags, 0);
    if (h) {
        DWORD status = 0, total = 0, sl = sizeof status;
        HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sl, NULL);
        sl = sizeof total;
        HttpQueryInfoW(h, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &total, &sl, NULL);
        size_t cap = 1 << 16, n = 0;
        data = (char *)malloc(cap + 1);
        DWORD got = 0, last = GetTickCount() - 1000;
        while (data && InternetReadFile(h, data + n, (DWORD)(cap - n), &got) && got) {
            n += got;
            if (n == cap) {
                char *nd = cap < (64u << 20) ? (char *)realloc(data, cap * 2 + 1) : NULL;
                if (!nd) { free(data); data = NULL; break; }
                data = nd; cap *= 2;
            }
            if (show && GetTickCount() - last > 100) {
                wchar_t t[200], wwhat[100];
                MultiByteToWideChar(CP_UTF8, 0, what, -1, wwhat, 100);
                if (total) swprintf(t, 200, L"%ls (%d%%)", wwhat, (int)(n * 100 / total));
                else swprintf(t, 200, L"%ls (%.1f MB)", wwhat, n / 1048576.0);
                note_show(t);
                last = GetTickCount();
            }
        }
        if (data) data[n] = 0;
        if (status != 200 || !data || (total && n != total)) { free(data); data = NULL; }
        else *len = n;
        InternetCloseHandle(h);
    }
    InternetCloseHandle(net);
    return data;
}

static int write_all_w(const wchar_t *path, const void *d, size_t n)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(f, d, (DWORD)n, &put, NULL) && put == n;
    return CloseHandle(f) && ok;
}

static void dll_path_w(wchar_t *out, size_t n)
{
    GetModuleFileNameW(g_self, out, (DWORD)n);
}

static int install_release(const UpdRelease *r, char *err, size_t errlen)
{
    const char *url = find_asset(r, UPD_ASSET);
    const char *sumurl = find_asset(r, UPD_ASSET ".sha256");
    if (!url || !sumurl) {
        snprintf(err, errlen, "Release %s is missing %s or its checksum.", r->tag, UPD_ASSET);
        return 0;
    }
    wchar_t curp[MAX_PATH * 2], newp[MAX_PATH * 2 + 8], oldp[MAX_PATH * 2 + 8];
    dll_path_w(curp, MAX_PATH * 2);
    swprintf(newp, MAX_PATH * 2 + 8, L"%ls.new", curp);
    swprintf(oldp, MAX_PATH * 2 + 8, L"%ls.old", curp);

    if (!write_all_w(newp, "", 0)) {
        snprintf(err, errlen, "The game folder is read-only.");
        return 0;
    }
    size_t sl = 0, n = 0;
    char *sum = http_get(sumurl, &sl, 0, "");
    char *bin = sum ? http_get(url, &n, 1, "Downloading Army Men Modern update") : NULL;
    int ok = 0;
    if (!sum || !bin) snprintf(err, errlen, "Download failed (no internet connection?).");
    else if (!sha256_matches(bin, n, sum, sl)) snprintf(err, errlen, "Checksum mismatch; nothing was changed.");
    else if (!write_all_w(newp, bin, n)) snprintf(err, errlen, "Could not write %s.new.", UPD_ASSET);
    else {
        DeleteFileW(oldp);
        if (!MoveFileExW(curp, oldp, MOVEFILE_REPLACE_EXISTING))
            snprintf(err, errlen, "Could not move the active %s aside.", UPD_ASSET);
        else if (!MoveFileExW(newp, curp, MOVEFILE_REPLACE_EXISTING)) {
            MoveFileExW(oldp, curp, MOVEFILE_REPLACE_EXISTING);
            snprintf(err, errlen, "Could not place the new %s.", UPD_ASSET);
        } else {
            ok = 1;
        }
    }
    if (!ok) DeleteFileW(newp);
    free(sum);
    free(bin);
    return ok;
}

static void open_releases_page(const UpdRelease *r)
{
    wchar_t w[600];
    MultiByteToWideChar(CP_UTF8, 0, (r && r->page[0]) ? r->page : MOD_REPO_URL "/releases", -1, w, 600);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- async check thread & UI prompt ---- */
typedef struct {
    int manual;
    int ok;
    UpdRelease rel;
} UpdResult;

static volatile LONG g_checking;

static DWORD WINAPI check_thread(LPVOID param)
{
    int manual = (int)(intptr_t)param;
    if (!manual) Sleep(2500);                                /* let the game window appear first */

    char api[600];
    UpdResult *res = (UpdResult *)calloc(1, sizeof *res);
    if (!res) { InterlockedExchange(&g_checking, 0); return 0; }
    res->manual = manual;

    if (build_api_url(api, sizeof api)) {
        size_t n = 0;
        char *json = http_get(api, &n, 0, "");
        if (json) {
            res->ok = parse_releases_json(json, &res->rel);
            free(json);
        }
    }
    mlog("update: %s check %s (current %s, latest %s)",
         manual ? "manual" : "startup",
         res->ok ? "ok" : "failed", MOD_VERSION, res->ok ? res->rel.tag : "?");

    HWND hwnd = g_dd ? g_dd->hwnd : NULL;
    if (hwnd && IsWindow(hwnd)) {
        PostMessageA(hwnd, WM_MOD_UPDATE, 0, (LPARAM)res);
    } else {
        free(res);
    }
    InterlockedExchange(&g_checking, 0);
    return 0;
}

void update_cleanup(void)
{
    wchar_t curp[MAX_PATH * 2], oldp[MAX_PATH * 2 + 8];
    dll_path_w(curp, MAX_PATH * 2);
    swprintf(oldp, MAX_PATH * 2 + 8, L"%ls.old", curp);
    for (int i = 0; i < 6 && GetFileAttributesW(oldp) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(oldp); i++)
        Sleep(150);
}

void update_check_async(int manual)
{
    if (!manual) {
        if (!g_cfg.update_check) return;
        const char *ini = mod_ini_path();
        char rep[16] = "";
        GetPrivateProfileStringA("debug", "replay", "", rep, sizeof rep, ini);
        if (rep[0] || GetPrivateProfileIntA("debug", "rig_coords", 0, ini) > 0) return;   /* skip under automated test rig */
    }
    if (InterlockedCompareExchange(&g_checking, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, check_thread, (LPVOID)(intptr_t)manual, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_checking, 0);
}

int update_message(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (msg != WM_MOD_UPDATE) return 0;
    UpdResult *res = (UpdResult *)lp;
    if (!res) return 1;

    wchar_t buf[900];
    if (!res->ok) {
        if (res->manual)
            MessageBoxW(h, L"Army Men Modern could not reach GitHub to check for updates.", L"Army Men Modern", MB_ICONWARNING);
        free(res);
        return 1;
    }

    const char *latest = res->rel.tag[0] == 'v' || res->rel.tag[0] == 'V' ? res->rel.tag + 1 : res->rel.tag;
    if (!version_newer(res->rel.tag, MOD_VERSION) || !find_asset(&res->rel, UPD_ASSET)) {
        if (res->manual) {
            swprintf(buf, 900, L"Army Men Modern v%hs is up to date.", MOD_VERSION);
            MessageBoxW(h, buf, L"Army Men Modern", MB_ICONINFORMATION);
        }
        free(res);
        return 1;
    }

    swprintf(buf, 900,
             L"Army Men Modern v%hs is available (you have v%hs).\n\n"
             L"Update now? The mod will download ddraw.dll, verify its SHA-256 checksum, "
             L"and restart the game. Your saves and settings stay.\n\n"
             L"(No = later. To disable startup checks: Gameplay menu > Check for updates at start.)",
             latest, MOD_VERSION);
    int ans = MessageBoxW(h, buf, L"Army Men Modern - Update Available", MB_YESNO | MB_ICONINFORMATION);
    if (ans == IDYES) {
        char err[300] = "";
        note_show(L"Downloading Army Men Modern update...");
        int ok = install_release(&res->rel, err, sizeof err);
        note_close();
        if (!ok) {
            wchar_t werr[300];
            MultiByteToWideChar(CP_UTF8, 0, err, -1, werr, 300);
            swprintf(buf, 900, L"The update failed: %ls\n\nOpen the GitHub Releases page instead?", werr);
            if (MessageBoxW(h, buf, L"Army Men Modern", MB_YESNO | MB_ICONWARNING) == IDYES)
                open_releases_page(&res->rel);
            free(res);
            return 1;
        }
        wchar_t exe[MAX_PATH * 2];
        GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi;
        mlog("update: installed v%s -> restarting %ls", latest, exe);
        free(res);
        record_shutdown();
        if (CreateProcessW(exe, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            MessageBoxW(h, L"Army Men Modern was updated. Please start the game again.", L"Army Men Modern", MB_ICONINFORMATION);
        }
        ExitProcess(0);
    }
    free(res);
    return 1;
}

const char *update_version(void) { return MOD_VERSION; }
