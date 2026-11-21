/* ============================================================================================
 * debugview.c - testing aid: what is under the pointer (Ctrl+Shift+H, [debug] hitboxes)
 *
 * In gameplay, every object / soldier / vehicle under the pointer gets its hitbox frame drawn
 * (yellow = counts as a target for the one-button mouse, grey = not) and a label from the game's
 * own data (there are no descriptive names in the game files - the art is only numbered per category):
 *     "BUILDING #360 hp 40/40 own -"      objects: category (binary), art number, health, owner
 *     "TROOPER tan hp 15/15"              soldiers / Sarge / vehicles: type and army
 * Big objects are made of several linked parts (game note: "a building made up of four small objects so it
 * can be damaged in sections") - each part shows its own frame and health.
 * Found with the same queries the mod uses: GetObjectsIntersectPoint (objects in the point layers) and
 * the drawn-cell registry (roofs.c; the Boot Camp cut-outs, units, vehicles).
 * ============================================================================================ */
#include "common.h"

#define CAMERA_X   (*(short *)0x4e2de8)
#define CAMERA_Y   (*(short *)0x4e2dea)
typedef BYTE *(__fastcall *ObjectsAtFn)(short *point);
#define OBJECTS_AT ((ObjectsAtFn)0x42fca0)

static int g_on = -1;

static const char *const CATEGORY[] = {
    "ROCK", "BUSH", "TREE", "BIG TREE", "FALLEN TREE", "FENCE", "WALL", "BRIDGE", "FOOT BRIDGE", "EXPLOSIVES",
    "GUN TURRET", "AA GUN", "BASE", "AUX BASE", "AIR STRIP", "HELI PAD", "PRISON CAMP", "BUILDING", "PILL BOX",
    "POWER PLANT", "GATE", "WALL GATE", "RADAR", "MISC", "OVERPASS", "SPECIAL ITEM" };
static const char *const ARMY[] = { "green", "tan", "blue", "grey" };

/* 5x7 pixel font: space, 0-9, A-Z, '#', '/', '-', '.' */
static const BYTE FONT[][7] = {
    {0,0,0,0,0,0,0},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},
    {31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{28,18,17,17,17,18,28},{31,16,16,30,16,16,31},
    {31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},
    {31,1,2,4,8,16,31},
    {10,10,31,10,31,10,10},{1,1,2,4,8,16,16},{0,0,0,31,0,0,0},{0,0,0,0,0,12,12} };
static int glyph(char c)
{
    if (c >= '0' && c <= '9') return 1 + c - '0';
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return 11 + c - 'A';
    if (c == '#') return 37;
    if (c == '/') return 38;
    if (c == '-') return 39;
    if (c == '.') return 40;
    return 0;
}

static int nearest(Surf *b, int r, int g, int bl)
{
    Pal *p = g_dd ? g_dd->pal : NULL;
    int best = 0, bd = 1 << 30;
    for (int i = 10; i < 246 && p; i++) {
        int dr = p->e[i].peRed - r, dg = p->e[i].peGreen - g, db = p->e[i].peBlue - bl, d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    (void)b;
    return best;
}

static void put(Surf *b, int x, int y, BYTE c)
{
    if (x < 0 || y < 0 || x >= b->w || y >= b->h) return;
    b->bits[(size_t)y * b->pitch + x] = c;
    hdworld_clear_run(b, y, x, x + 1);                  /* interface pixel: the HD battlefield stays under it */
}

static void text(Surf *b, int x, int y, const char *s, BYTE ink, BYTE shade)
{
    for (int i = 0; s[i]; i++, x += 6) {
        const BYTE *g = FONT[glyph(s[i])];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c)) { put(b, x + c + 1, y + r + 1, shade); put(b, x + c, y + r, ink); }
    }
}

static void frame(Surf *b, RECT r, BYTE ink)
{
    for (int x = r.left; x < r.right; x++) { put(b, x, r.top, ink); put(b, x, r.bottom - 1, ink); }
    for (int y = r.top; y < r.bottom; y++) { put(b, r.left, y, ink); put(b, r.right - 1, y, ink); }
}

/* the world rectangle and label of an item, by its raw type (uid >> 22 & 0xf) */
static int describe(BYTE *it, RECT *wr, char *lab, int n)
{
    DWORD uid = *(DWORD *)it;
    int raw = (int)((uid >> 22) & 0xf), army = (int)((uid >> 26) & 7) - 1;
    short hp = *(short *)(it + 0x1c), mx = *(short *)(it + 0x1e);
    const char *an = army >= 0 && army < 4 ? ARMY[army] : "-";
    const int *r = NULL;
    if (raw >= 7 && raw <= 9) {                                  /* asset / structure / obstacle */
        r = (const int *)(it + 0x98);                            /* its ANIMCELL (+0x7c) world rect */
        int cat = *(short *)(it + 0x68), own = *(signed char *)(it + 0x74);
        wsprintfA(lab, "%s #%d hp %d/%d own %s", cat >= 0 && cat < 26 ? CATEGORY[cat] : "OBJECT", *(short *)(it + 0x6a), hp, mx,
                  own >= 0 && own < 4 ? ARMY[own] : "-");
    } else if (raw == 5 || raw == 6) {                           /* trooper / sarge: the avatar's animation */
        BYTE *av = *(BYTE **)(it + 0x26c), *anim = av ? *(BYTE **)(av + 0x1c) : NULL;
        if (anim) r = (const int *)(anim + 0x1c);
        wsprintfA(lab, "%s %s hp %d/%d", raw == 6 ? "SARGE" : "TROOPER", an, hp, mx);
    } else if (raw == 3) {                                       /* vehicle: its mover's box */
        BYTE *m = *(BYTE **)(it + 0x9c);
        if (m) r = (const int *)(m + 0x1c);
        wsprintfA(lab, "VEHICLE %s type %d hp %d/%d", an, (int)((*(DWORD *)it >> 16) & 0x3f), hp, mx);
    } else {
        wsprintfA(lab, "ITEM type %d %s hp %d", raw - 1, an, hp);
    }
    (void)n;
    if (!r || r[2] <= r[0] || r[3] <= r[1]) return 0;
    *wr = (RECT){ r[0], r[1], r[2], r[3] };
    return 1;
}

void debugview_draw(Surf *b)
{
    if (g_on < 0) g_on = GetPrivateProfileIntA("debug", "hitboxes", 0, mod_ini_path());
    static int was;
    int k = (GetAsyncKeyState(VK_CONTROL) < 0) && (GetAsyncKeyState(VK_SHIFT) < 0) && (GetAsyncKeyState('H') < 0);
    if (k && !was) {
        g_on = !g_on;
        WritePrivateProfileStringA("debug", "hitboxes", g_on ? "1" : "0", mod_ini_path());
        mlog("debug: hitbox view %s", g_on ? "on" : "off");
    }
    was = k;
    if (!g_on || !b || !b->bits || !layout_view_is_gameplay()) return;
    int px, py;
    if (!controls_pointer(&px, &py)) return;
    int wx = px + CAMERA_X, wy = py + CAMERA_Y;
    if (wx < 0 || wy < 0 || wx > 4095 || wy > 4095) return;
    BYTE *seen[12]; int ns = 0;
    short p[2] = { (short)wx, (short)wy };
    for (BYTE *it = OBJECTS_AT(p); it && ns < 6; it = *(BYTE **)(it + 0x60)) seen[ns++] = it;
    BYTE *cl[6]; int nc = roofs_cells_at(wx, wy, cl, 6);
    for (int i = 0; i < nc && ns < 12; i++) {
        BYTE *ow = *(BYTE **)(cl[i] + 0x0c);
        int dup = 0;
        for (int j = 0; j < ns; j++) dup |= seen[j] == ow;
        if (ow && !dup) seen[ns++] = ow;
    }
    BYTE yel = (BYTE)nearest(b, 255, 230, 40), grey = (BYTE)nearest(b, 170, 170, 170), dark = (BYTE)nearest(b, 0, 0, 0);
    int ty = py + 14;
    for (int i = 0; i < ns; i++) {
        RECT wr; char lab[96];
        int has = describe(seen[i], &wr, lab, sizeof lab);
        int target = controls_is_target(seen[i]);
        BYTE ink = target ? yel : grey;
        if (has) frame(b, (RECT){ wr.left - CAMERA_X, wr.top - CAMERA_Y, wr.right - CAMERA_X, wr.bottom - CAMERA_Y }, ink);
        text(b, px + 12, ty, lab, ink, dark);
        ty += 9;
    }
}
