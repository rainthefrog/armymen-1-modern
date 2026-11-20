/* In-memory patches to Armymen.exe, applied when the game loads ddraw.dll (before WinMain).
 * Each patch checks the original bytes first. If the bytes already hold the new value (an exe
 * patched on disk by tooling/patches/patch.py) the patch is skipped; anything else is logged and
 * left alone, so an unknown exe is never modified. */
#include "common.h"

typedef struct {
    const char *name;
    uintptr_t va;
    const BYTE *orig, *repl;
    size_t len;
} Patch;

int write_mem(uintptr_t va, const void *src, size_t n)
{
    DWORD old;
    if (!VirtualProtect((void *)va, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)va, src, n);
    VirtualProtect((void *)va, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)va, n);
    return 1;
}

static void apply(const Patch *p)
{
    const BYTE *cur = (const BYTE *)p->va;
    if (memcmp(cur, p->repl, p->len) == 0) { mlog("patch %-55s already in exe", p->name); return; }
    if (memcmp(cur, p->orig, p->len) != 0) { mlog("patch %-55s SKIPPED: unexpected bytes", p->name); return; }
    mlog("patch %-55s %s", p->name, write_mem(p->va, p->repl, p->len) ? "applied" : "FAILED");
}

/* ---------------------------------------------------------------- modern controls (switchable)
 * The original also steers Sarge (and vehicle turrets) with raw mouse movement, accumulated in
 * DAT_004fe45c / DAT_004fe460 by FUN_0041f4e0: with the mouse as a free cursor that turned Sarge
 * whenever the mouse moved, so the two accumulating stores are dropped. Sarge's per-frame update
 * FUN_004479b0 also applies the original mouse buttons after our code (left down = fire HELD every
 * frame, right down = walk forward); the new controls handle both buttons, so those two checks are
 * skipped. The Gameplay menu (menu.c) switches modern controls on and off while playing. */
static const BYTE ACCX_O[] = { 0x89,0x15,0x5c,0xe4,0x4f,0x00 }, ACCY_O[] = { 0x89,0x0d,0x60,0xe4,0x4f,0x00 };
static const BYTE NOP6[] = { 0x66,0x0f,0x1f,0x44,0x00,0x00 };
static const BYTE JE_L[] = { 0x74, 0x0c }, JMP_L[] = { 0xeb, 0x0c };
static const BYTE JE_R[] = { 0x74, 0x15 }, JMP_R[] = { 0xeb, 0x15 };
/* FUN_004411c0 = right click on the battlefield: with the original "right button walks" option off
 * it calls the "next weapon" handler (0x460d80). Modern controls walk with the right button and
 * change weapons with the wheel / HUD buttons, so the call is removed. */
static const BYTE CALL_NEXT_WEAPON[] = { 0xe8, 0x7c, 0xfb, 0x01, 0x00 }, NOP5[] = { 0x0f, 0x1f, 0x44, 0x00, 0x00 };
static const Patch MODERN[] = {
    { "controls: right click does not change weapon (0x4411ff)", 0x4411ff, CALL_NEXT_WEAPON, NOP5, 5 },
    { "controls: no mouse steering x (0x41f5a6)",           0x41f5a6, ACCX_O, NOP6, 6 },
    { "controls: no mouse steering y (0x41f5e0)",           0x41f5e0, ACCY_O, NOP6, 6 },
    { "controls: no original left-button fire (0x447a32)",  0x447a32, JE_L, JMP_L, 2 },
    { "controls: no original right-button walk (0x447a47)", 0x447a47, JE_R, JMP_R, 2 },
};

void patches_set_modern_controls(int on)
{
    for (size_t i = 0; i < sizeof MODERN / sizeof MODERN[0]; i++) {
        const Patch *p = &MODERN[i];
        const BYTE *want = on ? p->repl : p->orig, *other = on ? p->orig : p->repl;
        if (memcmp((const void *)p->va, want, p->len) == 0) continue;
        if (memcmp((const void *)p->va, other, p->len) != 0) { mlog("patch %-55s SKIPPED: unexpected bytes", p->name); continue; }
        mlog("patch %-55s %s", p->name, write_mem(p->va, want, p->len) ? (on ? "applied" : "undone") : "FAILED");
    }
}

void *iat_hook(const char *dll, const char *func, void *replacement)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((const char *)(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)n->Name, func) != 0) continue;
            void *old = (void *)iat->u1.Function;
            uintptr_t v = (uintptr_t)replacement;
            if (!write_mem((uintptr_t)&iat->u1.Function, &v, sizeof v)) return NULL;
            return old;
        }
    }
    return NULL;
}

/* Like hook_jmp, but returns a pointer that runs the ORIGINAL function, so a C wrapper can adjust the
 * arguments and then call the game's own code. orig must be whole instructions with no
 * position-dependent code (they are copied and followed by a jump back to va + n). */
void *hook_wrap(const char *name, uintptr_t va, const BYTE *orig, size_t n, void *target)
{
    if (n < 5 || memcmp((const void *)va, orig, n) != 0) {
        mlog("wrap  %-55s SKIPPED: unexpected bytes", name);
        return NULL;
    }
    BYTE *tramp = VirtualAlloc(NULL, n + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;
    memcpy(tramp, orig, n);
    tramp[n] = 0xE9;
    int32_t rel = (int32_t)((va + n) - ((uintptr_t)tramp + n + 5));
    memcpy(tramp + n + 1, &rel, 4);
    if (!hook_jmp(name, va, orig, n, target)) { VirtualFree(tramp, 0, MEM_RELEASE); return NULL; }
    return tramp;
}

/* Point the call (e8 rel32) at va, which must currently call expect, at target instead. */
int hook_call(const char *name, uintptr_t va, uintptr_t expect, void *target)
{
    const BYTE *p = (const BYTE *)va;
    if (p[0] != 0xE8 || va + 5 + *(const int32_t *)(p + 1) != expect) {
        mlog("call  %-55s SKIPPED: unexpected bytes", name);
        return 0;
    }
    int32_t rel = (int32_t)((uintptr_t)target - (va + 5));
    int ok = write_mem(va + 1, &rel, 4);
    mlog("call  %-55s %s", name, ok ? "redirected" : "FAILED");
    return ok;
}

int hook_jmp(const char *name, uintptr_t va, const BYTE *orig, size_t n, void *target)
{
    if (n < 5 || memcmp((const void *)va, orig, n) != 0) {
        mlog("hook  %-55s SKIPPED: unexpected bytes", name);
        return 0;
    }
    BYTE code[16];
    memset(code, 0x90, sizeof code);
    code[0] = 0xE9;                                   /* jmp rel32 */
    int32_t rel = (int32_t)((uintptr_t)target - (va + 5));
    memcpy(code + 1, &rel, 4);
    int ok = write_mem(va, code, n < sizeof code ? n : sizeof code);
    mlog("hook  %-55s %s", name, ok ? "installed" : "FAILED");
    return ok;
}

void patches_apply(void)
{
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000) { mlog("exe not at 0x400000; no patches"); return; }

    /* FUN_00403880: read the game's own palette instead of GetSystemPaletteEntries(), which
     * only gives the right colours on a real 8-bit desktop. */
    static const BYTE pal_o[] = { 0x68,0xb4,0xb2,0x4a,0x00, 0x68,0x00,0x01,0x00,0x00, 0x6a,0x00, 0x56, 0xff,0x15,0x20,0xe9,0x6a,0x00 };
    static const BYTE pal_n[] = { 0xbe,0xbc,0xb6,0x4a,0x00, 0xbf,0xb4,0xb2,0x4a,0x00, 0xb9,0x00,0x01,0x00,0x00, 0xf3,0xa5, 0x90,0x90 };
    /* FUN_004084a0: skip the DirectPlayLobby probe that makes Windows offer to install DirectPlay. */
    static const BYTE dp_o[] = { 0xe8,0xb8,0xe7,0x01,0x00 };
    static const BYTE dp_n[] = { 0x0f,0x1f,0x44,0x00,0x00 };
    const Patch fixed[] = {
        { "palette: use game palette, not GetSystemPaletteEntries", 0x403915, pal_o, pal_n, sizeof pal_o },
        { "startup: skip DirectPlayLobby probe", 0x4084b3, dp_o, dp_n, sizeof dp_o },
    };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) apply(&fixed[i]);

    if (g_cfg.mouse_fix) {
        /* FUN_0041f380: mov eax,[0x4fe448]; push esi; test eax,eax */
        static const BYTE mouse_o[] = { 0xa1,0x48,0xe4,0x4f,0x00, 0x56, 0x85,0xc0 };
        hook_jmp("mouse: cursor update (FUN_0041f380) -> mouse.c", 0x41f380, mouse_o, sizeof mouse_o,
                 (void *)mouse_update);
        /* FUN_0041f330 / FUN_0041f480 both start: mov eax,[0x4fe478]; test eax,eax */
        static const BYTE rel_o[] = { 0xa1,0x78,0xe4,0x4f,0x00, 0x85,0xc0, 0x74,0x41 };
        static const BYTE acq_o[] = { 0xa1,0x78,0xe4,0x4f,0x00, 0x85,0xc0, 0x74,0x49 };
        hook_jmp("mouse: release to Windows (FUN_0041f330)", 0x41f330, rel_o, sizeof rel_o, (void *)mouse_release);
        hook_jmp("mouse: take back from Windows (FUN_0041f480)", 0x41f480, acq_o, sizeof acq_o, (void *)mouse_reacquire);

        if (g_cfg.mouse_pointer) {
            /* DirectInput mouse: exclusive (5) -> shared (6), see mouse.c "Pointer mode plumbing" */
            static const BYTE coop_o[] = { 0x6a, 0x05 }, coop_n[] = { 0x6a, 0x06 };
            const Patch c = { "mouse: shared DirectInput mouse (0x420283)", 0x420283, coop_o, coop_n, 2 };
            apply(&c);
            mlog("hook  PostMessageA -> mouse.c %s", iat_hook("user32.dll", "PostMessageA", (void *)mouse_PostMessageA) ? "installed" : "NOT installed");
            mlog("hook  ShowCursor -> mouse.c %s", iat_hook("user32.dll", "ShowCursor", (void *)mouse_ShowCursor) ? "installed" : "NOT installed");
        }
    }

    /* FUN_00407ae0 -> window.c (sub esp,0x20; push ebx; push ebp; push esi; mov esi,ecx) */
    static const BYTE win_o[] = { 0x83,0xec,0x20, 0x53, 0x55, 0x56, 0x8b,0xf1 };
    hook_jmp("window: rectangle (FUN_00407ae0) -> window.c", 0x407ae0, win_o, sizeof win_o, (void *)game_window_rect);

    /* HUD at the bottom of larger screens (hud.c) */
    static const BYTE pic_o[] = { 0x83,0xec,0x6c, 0x53, 0x55, 0x56, 0x8b,0xf1 };   /* FUN_00412f70 */
    hook_jmp("hud: draw interface picture (FUN_00412f70) -> hud.c", 0x412f70, pic_o, sizeof pic_o, (void *)hud_draw_picture);
    static const BYTE cur_o[] = { 0xa1,0x48,0xe4,0x4f,0x00, 0x85,0xc0 };          /* FUN_004206a0 */
    hook_jmp("hud: draw mouse cursor (FUN_004206a0) -> hud.c", 0x4206a0, cur_o, sizeof cur_o, (void *)hud_draw_cursor);

    /* 0x4ab180: start fullscreen (1) or windowed (0). The -f / -w switches still override it. */
    {
        DWORD fs_o = 1, fs_n = g_cfg.windowed ? 0 : 1;
        if (fs_n == 0) {
            const Patch p = { "display: start in a window", 0x4ab180, (BYTE *)&fs_o, (BYTE *)&fs_n, 4 };
            apply(&p);

            /* FUN_004094b0 (start a mission): "if (DAT_004d8100 == 0) FUN_00408840();" switches to
             * fullscreen for every mission. DAT_004d8100 is read nowhere else (probably a developer
             * switch). Windowed: skip the call so missions stay in the window. */
            static const BYTE mission_o[] = { 0xe8,0x7b,0xf3,0xff,0xff };   /* call 0x408840 */
            static const BYTE mission_n[] = { 0x0f,0x1f,0x44,0x00,0x00 };   /* 5-byte nop */
            const Patch m = { "display: missions stay windowed (0x4094c0)", 0x4094c0, mission_o, mission_n, 5 };
            apply(&m);
        }
    }

    /* Minimap viewport box and unit dots (hud.c) */
    static const BYTE rect_o[] = { 0x83,0xec,0x6c, 0x53, 0x55 };          /* FUN_00405980: sub esp,0x6c; push ebx; push ebp */
    static const BYTE plot_o[] = { 0xa1,0xc8,0x2d,0x4e,0x00 };            /* FUN_004057b0: mov eax,[0x4e2dc8] */
    hud_minimap_rect_original = hook_wrap("hud: minimap box (FUN_00405980) -> hud.c", 0x405980, rect_o, sizeof rect_o,
                                          (void *)hud_minimap_rect);
    hud_minimap_plot_original = hook_wrap("hud: minimap dot (FUN_004057b0) -> hud.c", 0x4057b0, plot_o, sizeof plot_o,
                                          (void *)hud_minimap_plot);

    /* Pictures straight to the screen (pause menu, menu backgrounds, some HUD parts): hud.c */
    static const BYTE scr_o[] = { 0x83,0xec,0x10, 0x85,0xc9, 0x53, 0x56 };   /* FUN_00413050 */
    hook_jmp("ui: draw picture on screen (FUN_00413050) -> hud.c", 0x413050, scr_o, sizeof scr_o,
             (void *)hud_draw_picture_on_screen);
    /* Menu font "ArialNarrow" -> Arial Narrow, or narrowed Arial: fonts.c */
    mlog("hook  CreateFontA -> fonts.c %s", iat_hook("gdi32.dll", "CreateFontA", (void *)font_CreateFontA) ? "installed" : "NOT installed");
    /* Colour fills on the screen (menu label boxes, volume bars): hud.c */
    static const BYTE fill_o[] = { 0x83,0xec,0x74, 0x56, 0x8b,0xf1 };        /* sub esp,0x74; push esi; mov esi,ecx */
    hook_jmp("ui: colour fill on screen (FUN_00406390) -> hud.c", 0x406390, fill_o, sizeof fill_o, (void *)hud_fill_on_screen);
    hook_jmp("ui: colour fill on screen (FUN_00406410) -> hud.c", 0x406410, fill_o, sizeof fill_o, (void *)hud_fill_on_screen);
    /* GDI menu text (pause menu labels): hud.c */
    static const BYTE gdi_o[] = { 0x83,0xec,0x48, 0x53, 0x55 };              /* FUN_00413a30: sub esp,0x48; push ebx; push ebp */
    hud_draw_gdi_text_original = hook_wrap("ui: GDI text (FUN_00413a30) -> hud.c", 0x413a30, gdi_o, sizeof gdi_o,
                                           (void *)hud_draw_gdi_text);
    /* radio pictures (Screens\Briefs): which sprite holds which message - radio.c */
    static const BYTE lspr_o[] = { 0x83,0xec,0x40, 0x53, 0x55, 0x56 };     /* FUN_00412840 LoadSprite: sub esp,0x40; push ebx/ebp/esi */
    radio_load_sprite_original = (LoadSpriteFn)hook_wrap("radio: LoadSprite (FUN_00412840) -> radio.c", 0x412840, lspr_o, sizeof lspr_o,
                                                         (void *)radio_load_sprite);
    /* 640x480-based clamps (aim point, keyboard cursor): layout.c */
    layout_patch_limits();

    /* In-game text: FUN_0044b760 draws strings with the game's cached font at 640x480 positions (hud.c). */
    static const BYTE str_o[] = { 0x81,0xec,0x94,0x00,0x00,0x00 };   /* sub esp,0x94 */
    hud_draw_string_original = hook_wrap("hud: draw text (FUN_0044b760) -> hud.c", 0x44b760, str_o, sizeof str_o,
                                         (void *)hud_draw_string);

    /* The game asks Windows for its client size; windowed, answer in game pixels (window.c). */
    if (iat_hook("user32.dll", "GetClientRect", (void *)game_GetClientRect))
        mlog("hook  GetClientRect -> window.c (game-sized client area)");
    else
        mlog("hook  GetClientRect NOT installed");

    /* Modern controls: wrap the per-frame keyboard dispatcher (controls.c) */
    static const BYTE kbd_o[] = { 0xa1,0x7c,0xe4,0x4f,0x00 };                /* FUN_0041fca0: mov eax,[0x4fe47c] */
    controls_dispatch_original = (VoidFnPtr)hook_wrap("controls: keyboard dispatcher (FUN_0041fca0) -> controls.c",
                                                      0x41fca0, kbd_o, sizeof kbd_o, (void *)controls_dispatch);
    if (g_cfg.controls_modern) patches_set_modern_controls(1);
    static const BYTE move_o[] = { 0x83,0xec,0x0c, 0xa1,0x70,0x3f,0x4b,0x00 };  /* FUN_00414ad0: sub esp,0Ch; mov eax,[4b3f70] */
    static const BYTE touch_o[] = { 0x53, 0x56, 0x57, 0x8b,0xfa, 0x8b,0xf1, 0x8b,0xcf };   /* FUN_004495c0: push ebx/esi/edi; mov edi,edx; mov esi,ecx; mov ecx,edi */
    weapons_touch_original = (TouchFn)hook_wrap("weapons: pick up item (FUN_004495c0) -> weapons.c (spares)",
                                                0x4495c0, touch_o, sizeof touch_o, (void *)weapons_touch);
    static const BYTE coll_o[] = { 0x83,0xec,0x0c, 0x53, 0x55, 0x56, 0xc7,0x02,0x00,0x00,0x00,0x00 };   /* FUN_00435a10 */
    bullets_collide_original = (CollideFn)hook_wrap("bullets: collision point (FUN_00435a10) -> bullets.c",
                                                    0x435a10, coll_o, sizeof coll_o, (void *)bullets_collide);
    static const BYTE hitobj_o[] = { 0x56, 0x8b,0xf1, 0x57, 0x66,0x8b,0xfa, 0x8b,0x06 };             /* FUN_0040e600 */
    bullets_hit_object_original = (HitObjFn)hook_wrap("bullets: object hit (FUN_0040e600) -> bullets.c (shoot over)",
                                                      0x40e600, hitobj_o, sizeof hitobj_o, (void *)bullets_hit_object);
    static const BYTE sbtn_o[] = { 0x51, 0x53, 0x8b,0xd9, 0x55 };                                  /* SBtnSetState 0x45e370 */
    screenbtn_set_state_original = (SBtnStateFn)hook_wrap("screenbtn: SBtnSetState (0x45e370) -> screenbtn.c (pressed look)",
                                                          0x45e370, sbtn_o, sizeof sbtn_o, (void *)screenbtn_set_state);
    static const BYTE stops_o[] = { 0x56, 0x8b,0xf1, 0x8b,0x4a,0x78, 0x8b,0x06, 0x3b,0xc1 };   /* FUN_00435c80 (next: je rel32) */
    bullets_stops_original = (StopsFn)hook_wrap("bullets: stops missile (FUN_00435c80) -> bullets.c (downhill)",
                                                0x435c80, stops_o, sizeof stops_o, (void *)bullets_stops);
    static const BYTE dash_o[] = { 0x81,0xec,0x20,0x01,0x00,0x00, 0xa1,0xac,0x6e,0x69,0x00 };   /* FUN_00441300 */
    objectives_dash_message_original = (DashMsgFn)hook_wrap("objectives: dashboard message (FUN_00441300) -> objectives.c",
                                                            0x441300, dash_o, sizeof dash_o, (void *)objectives_dash_message);
    static const BYTE cell_o[] = { 0x83,0xec,0x20, 0x8d,0x44,0x24,0x10, 0x53, 0x55, 0x8b,0xe9 };   /* FUN_0042eef0 */
    roofs_draw_cell_original = (DrawCellFn)hook_wrap("roofs: draw sprite (FUN_0042eef0) -> roofs.c (see through)",
                                                     0x42eef0, cell_o, sizeof cell_o, (void *)roofs_draw_cell);
    static const BYTE blit_o[] = { 0x55, 0x8b,0xec, 0x83,0xec,0x0c, 0x53, 0x56, 0x57 };   /* FUN_004040c0 / 4041b0 / 4042a0 */
    static const BYTE shad_o[] = { 0x55, 0x8b,0xec, 0x83,0xec,0x14, 0x53, 0x56, 0x57 };   /* FUN_00404390 */
    hdworld_wrle_original = (Blit5Fn)hook_wrap("hdworld: sprite WRLE (FUN_004041b0)", 0x4041b0, blit_o, sizeof blit_o, (void *)hdworld_wrle);
    hdworld_drle_original = (Blit5Fn)hook_wrap("hdworld: sprite DRLE (FUN_004040c0)", 0x4040c0, blit_o, sizeof blit_o, (void *)hdworld_drle);
    hdworld_remap_original = (Blit6Fn)hook_wrap("hdworld: sprite army colour (FUN_004042a0)", 0x4042a0, blit_o, sizeof blit_o, (void *)hdworld_remap);
    hdworld_shadow_original = (Blit5Fn)hook_wrap("hdworld: shadow (FUN_00404390)", 0x404390, shad_o, sizeof shad_o, (void *)hdworld_shadow);
    static const BYTE cam_o[] = { 0x53, 0x55, 0x56, 0x8b,0x1d,0x50,0x30,0x4e,0x00 };  /* FUN_0040dcd0: push ebx/ebp/esi; mov ebx,[4e3050] */
    controls_camera_original = (VoidFnPtr)hook_wrap("controls: camera (FUN_0040dcd0) -> controls.c (smooth camera)",
                                                    0x40dcd0, cam_o, sizeof cam_o, (void *)controls_camera);
    static const BYTE field_o[] = { 0x83,0xec,0x10, 0x53, 0x56, 0x57 };       /* FUN_0042d610: sub esp,10h; push ebx/esi/edi */
    layout_draw_battlefield_original = (VoidFnPtr)hook_wrap("layout: draw battlefield (FUN_0042d610) -> layout.c (map bottom)",
                                                            0x42d610, field_o, sizeof field_o, (void *)layout_draw_battlefield);
    controls_move_original = (MoveFn)hook_wrap("controls: object movement (FUN_00414ad0) -> controls.c (run)",
                                               0x414ad0, move_o, sizeof move_o, (void *)controls_move);
    static const BYTE shot_o[] = { 0x56, 0x8b,0xf1, 0x0f,0xbf,0x8e,0xc6,0x02,0x00,0x00 };  /* FUN_00447d80 */
    controls_shot_direction_original = (UnitFn)hook_wrap("controls: shot direction (FUN_00447d80) -> controls.c",
                                                         0x447d80, shot_o, sizeof shot_o, (void *)controls_shot_direction);
    static const BYTE fire_o[] = { 0x51, 0x56, 0x8b,0xf1, 0x57, 0x8b,0x0e };     /* FUN_00447c30: push ecx/esi; mov esi,ecx; push edi; mov ecx,[esi] */
    controls_fire_shot_original = (UnitFn)hook_wrap("controls: fire shot (FUN_00447c30) -> controls.c",
                                                    0x447c30, fire_o, sizeof fire_o, (void *)controls_fire_shot);
    static const BYTE aim_o[] = { 0xa1,0xb0,0x91,0x4f,0x00 };                /* FUN_0043eff0: mov eax,[0x4f91b0] */
    controls_aim_point_original = (VoidFnPtr)hook_wrap("controls: aim reticle (FUN_0043eff0) -> controls.c",
                                                       0x43eff0, aim_o, sizeof aim_o, (void *)controls_aim_point);
    /* the reticle's two draw calls (FUN_0043ed90: in a vehicle / on foot) -> controls_draw_gunsight, which
     * skips the draw while the one-button mouse shows its walk / enter pointer */
    hook_call("controls: reticle draw in a vehicle (0x43eeb6)", 0x43eeb6, 0x4130e0, (void *)controls_draw_gunsight);
    hook_call("controls: reticle draw on foot (0x43eee4)", 0x43eee4, 0x4130e0, (void *)controls_draw_gunsight);
    hook_call("modern: screen shake (0x42d616)", 0x42d616, 0x40df00, (void *)modern_shake);
    hook_call("gameplay: trooper squash on a blocked tile (0x43c840)", 0x43c840, 0x42a230, (void *)squash_stub);
    /* Sarge drives empty enemy vehicles (capture.c, [gameplay] drive_enemy_vehicles; inactive while off) */
    static const BYTE vkey_o[] = { 0x83,0xec,0x14, 0x53, 0x55, 0x56, 0x57 };       /* FUN_00418910: sub esp,14h; push ebx/ebp/esi/edi */
    hook_jmp("capture: Enter Vehicle key (FUN_00418910) -> capture.c", 0x418910, vkey_o, sizeof vkey_o, (void *)capture_enter_key);
    static const BYTE sin_o[] = { 0x56, 0x8b,0xf1, 0x57, 0x8b,0xfa };              /* SargeInVehicle 0x448ae0 */
    capture_sarge_in_original = (CaptureTwoFn)hook_wrap("capture: SargeInVehicle (0x448ae0) -> capture.c", 0x448ae0, sin_o, sizeof sin_o,
                                                        (void *)capture_sarge_in);
    static const BYTE sout_o[] = { 0x51, 0x53, 0x56, 0x8b,0xf1, 0x57 };            /* SargeOutVehicle 0x448b50 */
    capture_sarge_out_original = (CaptureOneFn)hook_wrap("capture: SargeOutVehicle (0x448b50) -> capture.c", 0x448b50, sout_o, sizeof sout_o,
                                                         (void *)capture_sarge_out);
    static const BYTE uin_o[] = { 0x53, 0x8b,0xda, 0x55, 0x56 };                   /* UnitEnterVehicle 0x41d800 */
    capture_unit_enter_original = (CaptureTwoFn)hook_wrap("capture: UnitEnterVehicle (0x41d800) -> capture.c", 0x41d800, uin_o, sizeof uin_o,
                                                          (void *)capture_unit_enter);
    hook_call("capture: RunAI -> HandleVehicle (0x4296b6)", 0x4296b6, 0x4296f0, (void *)capture_handle_vehicle);

    clock_install();
    hdvideo_install();

    /* Render size. FUN_00407820 sets the graphics object's size; 0x4abac0 is the default clip rect. */
    if (g_cfg.width != 640 || g_cfg.height != 480) {
        DWORD w = g_cfg.width, h = g_cfg.height, w0 = 640, h0 = 480;
        DWORD clip_o[4] = { 0, 0, 640, 480 }, clip_n[4] = { 0, 0, w, h };
        const Patch res[] = {
            { "res: graphics object width",  0x407849, (BYTE *)&w0, (BYTE *)&w, 4 },
            { "res: graphics object height", 0x407853, (BYTE *)&h0, (BYTE *)&h, 4 },
            { "res: default clip rect",      0x4abac0, (BYTE *)clip_o, (BYTE *)clip_n, 16 },
        };
        for (size_t i = 0; i < sizeof res / sizeof res[0]; i++) apply(&res[i]);
    }
    modmenu_install();                                   /* modmenu.c: Mod Settings in the game's menus */
    framerate_install();                                 /* framerate.c: gravity / animation exact at any frame rate */
}
