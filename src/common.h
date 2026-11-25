/* Army Men (1998) replacement ddraw.dll.
 *
 * The game asks DirectDraw for an 8-bit palettized screen and draws every pixel in software.
 * We keep all surfaces as 8-bit buffers in normal memory and only convert to 32-bit colour when
 * a frame is shown, through Direct3D 11. Windows is never asked for a 256-colour display mode.
 *
 * The DLL is also the mod loader: at load time it patches the game exe in memory (patches.c),
 * driven by armymen-mod.ini next to the DLL.
 */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <stddef.h>
#include <stdint.h>
#include <intrin.h>                    /* __rdtsc (PX_BEGIN / PX_END) */

typedef struct DDraw DDraw;
typedef struct Surf Surf;
typedef struct Pal Pal;
typedef struct Clip Clip;


struct Pal {
    const void *const *vtbl;
    LONG ref;
    DDraw *dd;
    PALETTEENTRY e[256];
    uint32_t bgra[256];      /* e[] converted for presenting */
};

struct Clip {
    const void *const *vtbl;
    LONG ref;
    HWND hwnd;
};

struct Surf {
    const void *const *vtbl;  /* one vtable serves IDirectDrawSurface, 2 and 3 */
    LONG ref;
    DDraw *dd;
    DWORD caps;
    int w, h, pitch;
    BYTE *bits;               /* top-down 8-bit pixels, owned by dib */
    HBITMAP dib;
    HDC dc;
    HGDIOBJ old_bmp;
    Pal *pal;
    Clip *clip;
    int has_ck;
    DWORD ck_lo, ck_hi;
    Surf *back;               /* primary only: attached back buffer */
    int is_primary;
    int hd_state, hd_id;      /* hdsprites.c: 0 = not identified yet; hd_id = catalogue entry or -1 */
    void *hd_ref;             /* hdworld.c: per pixel, which upscaled sprite/tile pixel is there (or NULL) */
    int *hd_tiles;            /* hdworld.c: tile atlas surfaces: pack entry per 16x16 tile (+1, 0 = unknown) */
    unsigned serial;          /* surface.c: creation number in this process (savestate.c save files) */
};

struct DDraw {
    const void *const *vtbl1; /* IDirectDraw  */
    const void *const *vtbl2; /* IDirectDraw2 (QueryInterface returns &vtbl2) */
    LONG ref;
    HWND hwnd;
    DWORD coop;
    int mode_w, mode_h;       /* 0 = no mode set, use desktop */
    int changed_mode;         /* we called ChangeDisplaySettings */
    Surf *primary;
    Pal *pal;                 /* palette attached to the primary */
    LONG saved_style, saved_exstyle;
    int restyled;
};

struct Config {
    int width, height;        /* game render size; 0 = desktop */
    int vsync;
    int fps_limit;                  /* [display] fps_limit: with vsync=0, frames per second (0 = uncapped) */
    int log;
    int scale;                /* windowed: window = render size x scale */
    int windowed;             /* start in a window instead of fullscreen */
    int mouse_fix;            /* replace the game's cursor update (mouse.c) */
    float mouse_sens;
    int mouse_winspeed;       /* follow the Windows pointer speed setting */
    int mouse_pointer;        /* game cursor = Windows pointer (1:1, not locked); mouse.c */
    float clock_speed;        /* game clock rate (clock.c); 1.0 = real time */
    int controls_modern;      /* controls.c: mouse aim/fire/move + WASD */
    int controls_tap_ms;      /* right click shorter than this = walk to the spot */
    int controls_fire_interval_ms; /* auto-fire interval while the left button is held */
    int controls_confine;     /* keep the pointer inside the window while playing */
    float controls_run_speed; /* how much faster running is (1.6 = 60 % faster) */
    float controls_camera_lead;     /* camera leads toward the pointer by this x its offset (0 = centred) */
    float controls_camera_smooth_ms;/* camera easing time constant (0 = no easing) */
    int six_weapons;                /* [controls] six_weapons: a spare per weapon slot (weapons.c) */
    int cheat_keys;                 /* [debug] cheat_keys: Ctrl+T teleport, Ctrl+I invulnerable (controls.c) */
    int health_regen;               /* [gameplay] health_regen: Sarge heals slowly (gameplay.c) */
    /* shooting rules (bullets.c), one switch per group - Mod Settings, COMBAT tab */
    int shoot_over;                 /* [gameplay] shoot_over: barricade rules - low cover stops a bullet only for a prone soldier */
    int cover_enemies;              /* [gameplay] cover_enemies: enemy bullets follow the barricade rules too (off: the game's) */
    int corner_shots;               /* [gameplay] corner_shots: buildings stop the player's bullets by footprint depth / aimed target */
    int leg_hits;                   /* [gameplay] leg_hits: a shot through an enemy's lower body counts */
    int menu_text;                  /* [display] menu_text: menu labels drawn by the mod at window resolution (menutext.c) */
    int radio_overlay;              /* [display] radio_overlay: radio messages over the battlefield, text redrawn (radio.c) */
    int see_interior;               /* [display] see_interior: Sarge in a building -> its floor shown open (roofs.c, gpu.c) */
    int fronts_order;               /* [display] fronts_order: figures in front of a building front drawn over it (roofs.c) */
    int air_layer;                  /* [display] air_layer: the game's air layer (helicopter, planes, shells) above the ground in every mod path (roofs.c) */
    int fix_squash;                 /* [gameplay] fix_squash: no squash death for walking onto a blocked tile (gameplay.c) */
    int longer_range;               /* [gameplay] longer_range: direct-fire weapons reach weapon_range_pct % (gameplay.c) */
    int weapon_range_pct;           /* [gameplay] weapon_range_pct (default 120) */
    int height_rules;               /* [gameplay] height_rules: HEIGHTS_STANCE / _AM2 / _ORIGINAL (/ _MODDED, ini only) (bullets.c) */
    int drive_enemy_vehicles;       /* [gameplay] drive_enemy_vehicles: Sarge may drive empty enemy vehicles (capture.c) */
    int vehicle_bailout;            /* [gameplay] vehicle_bailout: Sarge leaves a destroyed vehicle alive (capture.c) */
    int vehicle_reverse;            /* [controls] vehicle_reverse: click-to-drive plans with reversing (vehicles.c) */
    int turret_smooth;              /* [controls] turret_smooth: turret holds its aim while the hull turns, eased (vehicles.c) */
    int vehicle_unstick;            /* [controls] vehicle_unstick: a wedged vehicle may back out; no creeping into objects */
    int sarge_unstick;              /* [controls] sarge_unstick: Sarge moved out when stuck on / inside a barricade (barricades.c) */
    int break_fences;               /* [gameplay] break_fences: Sarge's routes cross wooden fences and he breaks them (fences.c) */
    int enemy_awareness;            /* [modern] enemy_awareness: could-see marks, exposure meter, delayed spotting, alerts (aware.c, modern.c) */
    int squad_sprint;               /* [gameplay] squad_sprint: troopers following Sarge sprint with him / to catch up (squad.c) */
    int patrol_marches;             /* [gameplay] patrol_marches: enemy units marching to a far point at deploy patrol between their post and that point (patrol.c) */
    int ally_regen;                 /* [gameplay] ally_regen: the player's soldiers heal slowly like Sarge (allies.c) */
    int ally_heal;                  /* [gameplay] ally_heal: medkits carried at full health, used on soldiers by clicking them (allies.c) */
    int ally_health_x;              /* [gameplay] ally_health_x: max health of the player's soldiers in percent of the game's (allies.c, default 200) */
    int ally_health_bars;           /* [hud] ally_health_bars: health bars over the player's hurt soldiers (modern.c) */
    int am2_sounds;                 /* [audio] am2_sounds: Army Men II sound effects from <game>\am2_sounds (audio.c) */
    int am2_music;                  /* [audio] am2_music: Army Men II music from <game>\am2_music (music.c) */
    int quicksave;                  /* [savestate] enabled: the quicksave / quickload keys work (savestate.c; the heap part is chosen at start-up) */
    int map_edge_fog;               /* [display] map_edge_fog: darkness outside the visible map (fog.c) */
    int hud_corners;                /* [hud] corners: message box / minimap in the bottom corners (layout.c) */
    float hud_corner_scale;         /* [hud] corner_scale: 1 .. 3 in quarter steps (default 1.5) */
    float hud_minimap_scale;        /* [hud] minimap_scale: the minimap corner alone, 1 .. 3 (default 2) */
    float hud_centre_scale;         /* [hud] centre_scale: the middle part (logo, weapons, health, squad), default 1 */
    float hud_air_scale;            /* [hud] air_scale: air support, default 1 */
    int pin_objective;              /* [hud] pin_objective: mission objective pinned on screen (objectives.c) */
    int target_sector;              /* [hud] target_sector: blink the target sector on the minimap */
    int target_area;                /* [hud] target_area: faint highlight of the target sector on the battlefield (objectives.c, gpu.c) */
    int mission_path;               /* [hud] mission_path: route from Sarge's start through the mission's waypoints, painted on the ground (missionpath.c) */
    int mission_path_variation;     /* [hud] mission_path_variation: each leg varied a little (seeded per mission entry, missionpath.c) */
    int mission_path_all;           /* [hud] mission_path_all: all legs drawn at once (0 = only the current one) */
    int crt;                        /* [display] crt: CRT filter (gpu.c); F9 or Gameplay menu */
    int see_through;                /* [display] see_through: Sarge visible through buildings (roofs.c) */
    int see_enemies;                /* [display] see_enemies: enemies under tall cover show while they fire (roofs.c) */
    int button_press;               /* [display] button_press: menu buttons drawn pressed before they act (screenbtn.c) */
    int smooth_scroll;              /* [display] smooth_scroll: sub-pixel battlefield scrolling (controls.c, gpu.c) */
    int clock_lock;                 /* [clock] lock_to_display: game time advances per shown refresh (clock.c) */
    int hd_menus;                   /* [display] hd_menus: widescreen HD menu backgrounds (hdart.c) */
    int hd_sprites;                 /* [display] hd_sprites: upscaled pictures from hd (hdsprites.c) */
    int hd_world;                   /* [display] hd_world: upscaled battlefield from hdworld.pak (hdworld.c) */
    int hd_videos;                  /* [display] hd_videos: upscaled videos from hdAVI (hdvideo.c) */
    int fs_extends_view;            /* [display] fullscreen_extends_view: fullscreen = more battlefield, same HUD size */
    int regen_delay_ms;             /* [gameplay] time without damage before healing */
    float regen_full_s;             /* [gameplay] seconds to heal from empty to full */
    int clock_keys;           /* hotkeys to change the clock rate in game */
    int video_fit;            /* [display] video_fit: videos fill the window */
    int borderless_fs;        /* [display] borderless_fullscreen: start covering the whole monitor */
    float zoom;               /* [display] zoom: battlefield zoom, interface unchanged (zoom.c); 1 = off */
    int mm_blips;             /* [hud] minimap_blips: bigger outlined blips (minimap.c) */
    int mm_sarge;             /* [hud] minimap_sarge: Sarge highlighted on the minimap */
    int mm_contrast;          /* [hud] minimap_contrast: darker, higher-contrast map picture */
    int mm_hover;             /* [hud] minimap_hover: details of the blip under the pointer */
    int classic_gameplay;     /* [preset] classic_gameplay: original rules/controls; modern renderer + optional HUD layout remain */
    int modern_hud_layout;    /* [hud] modern_layout: sliced/scaled HUD; 0 = original centred 1x HUD */
    int record_runs;          /* [debug] record_runs: automatic .jsonl run recording (off in portable/release by default) */
    int update_check;         /* [update] check: check GitHub Releases for a newer ddraw.dll at start */
};

/* view.c: where the shown part of the screen (the view, in game pixels) goes in the window. */
typedef struct {
    RECT view;                /* part of the screen surface shown, game pixels */
    int whole;                /* exact whole-number enlargement (CPU) */
    int fit;                  /* 1 = fill the window keeping the shape (battlefield, videos) */
    int video;                /* the view is a video frame (smooth filter even for original pixels) */
    RECT dest;                /* where it ends up in the client area (may extend past it: cropped) */
} ViewLayout;
RECT view_current(void);
void view_note_whole_frame(void);          /* call with present_lock held */
void view_note_picture(const RECT *r);     /* call with present_lock held */
void view_note_video(const RECT *r);       /* a video frame; call with present_lock held */
ViewLayout view_layout(int client_w, int client_h);
void view_client_to_game(POINT *p);        /* window client pixels -> game pixels */
void view_game_to_client(POINT *p);
int view_pixel_scale(void);                /* window pixels per game pixel */
int view_game_client_linear(double *ax, double *ay, double *kx, double *ky);   /* client = a + k x game (0: zoomed) */

/* What the display thread should show (filled under present_lock). */
typedef struct {
    HWND hwnd;
    const BYTE *bits;
    int pitch;
    const uint32_t *lut;
} FrameSource;

extern DDraw *g_dd;
int dd_frame_source(DDraw *dd, FrameSource *out);
int dd_is_windowed(void);

void present_init(void);
void present_lock(void);
void present_unlock(void);
void present_request(DDraw *dd, int wait_until_shown);
void present_shutdown(void);

void clock_install(void);
void clock_trace_start(int seconds);                               /* clock.c: start the per-frame timing trace */
DWORD clock_game_ms(void);                 /* game time (speed / pause applied) */
void clock_set_paused(int paused);            /* clock.c */
int clock_is_paused(void);
double clock_speed_now(void);
void clock_set_speed(double s);
/* Replace an imported function in Armymen.exe's import table. Returns the old pointer. */
void *iat_hook(const char *dll, const char *func, void *replacement);
extern struct Config g_cfg;
extern HMODULE g_self;

void mlog(const char *fmt, ...);
void config_load(void);
void patches_apply(void);
/* Redirect the game function at va to target with a 5-byte jmp, after checking its first bytes. */
int hook_jmp(const char *name, uintptr_t va, const BYTE *orig, size_t n, void *target);
int hook_call(const char *name, uintptr_t va, uintptr_t expect, void *target);
void __fastcall mouse_update(int dx, int dy);

HRESULT pal_create(DDraw *dd, DWORD flags, const PALETTEENTRY *e, Pal **out);
void pal_lut(Pal *p);
HRESULT clip_create(Clip **out);
HRESULT surf_create(DDraw *dd, DDSURFACEDESC *desc, Surf **out);
ULONG surf_release(Surf *s);

/* The screen surface changed. frame_end = a whole new frame (Flip or full-window Blt): the game
 * waits until it has been shown. Call with present_lock held for the write itself. */
void dd_present(DDraw *dd, int frame_end);

void desktop_size(int *w, int *h);
struct GameWindow;
RECT *__attribute__((thiscall)) game_window_rect(struct GameWindow *win, RECT *out);
void window_fit_client(HWND h);
void window_sync_game_rects(HWND h);
BOOL WINAPI game_GetClientRect(HWND h, RECT *r);
void __cdecl mouse_release(void);
void __cdecl mouse_reacquire(void);
void mouse_real_cursor(int *x, int *y);
BOOL WINAPI mouse_PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp);
int WINAPI mouse_ShowCursor(BOOL show);
int mouse_filter_message(HWND h, UINT msg, WPARAM *wp, LPARAM lp, LRESULT *result);
void mouse_poll_pointer(void);
void mouse_nudge_cursor(int dx, int dy);   /* raw mode: move the game cursor (controls.c throw camera) */
/* layout.c: the 640x480 interface design on the real screen */
#define LAYOUT_DESIGN_W     640
#define LAYOUT_DESIGN_H     480
#define LAYOUT_HUD_TOP      360   /* design rows at or below this belong to the HUD */
#define LAYOUT_HUD_PANEL_Y  365   /* the HUD panel starts here (its top rows overlap the map) */
#define LAYOUT_HUD_SPLIT_X  316   /* the HUD panel splits right of its weapon column (spare weapons) */
#define WEAPON_COL_SRC_X    243   /* weapons.c COL_SRC_X: the weapon column the spare column copies */
#define LAYOUT_PANEL_MIN_W  200   /* smaller pictures are not clickable panels */
#define LAYOUT_PANEL_MIN_H  100
#define LAYOUT_TARGET_ROWS  540   /* presets aim for about this many battlefield rows */
typedef enum { ANCHOR_HUD, ANCHOR_CENTER } Anchor;
int layout_active(void);
Anchor layout_anchor_at(int design_y);
void layout_offset(Anchor a, int *dx, int *dy);
Anchor layout_place(int *x, int *y);
void layout_note_panel(Anchor a, const RECT *on_screen);
void layout_note_click(const RECT *on_screen, int dx, int dy);           /* layout.c: a moved picture: clicks on it map back */
void layout_pointer_to_game(int x, int y, int *game_x, int *game_y);
void layout_patch_limits(void);
int layout_resolve_preset(const char *name);
typedef void (__cdecl *VoidFnPtr)(void);
int layout_view_is_gameplay(void);
extern VoidFnPtr layout_draw_battlefield_original;
void __cdecl layout_draw_battlefield(void); /* FUN_0042d610 wrap */
int controls_map_bottom_room(void);        /* controls.c: rows the camera may show past boundrect.bottom (wide screens) */
/* controls.c */
extern VoidFnPtr controls_dispatch_original;
void __cdecl controls_dispatch(void);
extern VoidFnPtr controls_aim_point_original;
void __cdecl controls_aim_point(void);
typedef void (__fastcall *UnitFn)(BYTE *unit);
extern UnitFn controls_shot_direction_original;
void __fastcall controls_shot_direction(BYTE *unit);
extern UnitFn controls_fire_shot_original;
void __fastcall controls_fire_shot(BYTE *unit);
void controls_release_pointer(void);
void controls_sizing(int on);              /* window is being resized or moved: pointer free */
typedef void (__fastcall *MoveFn)(BYTE *obj, int flag, int arg);
extern MoveFn controls_move_original;
void __fastcall controls_move(BYTE *obj, int flag, int arg);   /* FUN_00414ad0 wrap: running */
void controls_reset(void);                 /* modern controls switched off/on (menu) */
void weapons_select_box(int b);             /* weapons.c: keys 1-6 */
int controls_wheel(int delta);              /* WM_MOUSEWHEEL: weapons (modern controls); 1 = used */
int controls_pointer_on_ui(void);           /* pointer over the HUD in gameplay (show an arrow) */
HCURSOR controls_cursor(void);              /* one-button mouse: walk / enter pointer over the battlefield, or NULL */
int controls_target_in_range(void);         /* target under the pointer: 1 in range, 0 beyond, -1 none */
int controls_is_target(BYTE *item);         /* controls.c: counts as a target for the one-button mouse */
void debugview_draw(Surf *b);              /* debugview.c: testing aid, hitboxes and names */
void __fastcall controls_draw_gunsight(void *pic, int x, int y);   /* reticle draw call sites */
void gameplay_frame(void);                 /* gameplay.c: once per gameplay frame */
void weapons_frame(void);                  /* weapons.c: per gameplay frame */
void weapons_wheel(int notches);           /* weapons.c: + = wheel up */
void weapons_quick_swap(void);             /* weapons.c: Q = back to the previous weapon */
void weapons_note_text(int x, int y, int font, int colour); /* weapons.c: ammo text position (hud.c) */
void weapons_draw_markers(Surf *back);     /* weapons.c: HUD spare markers, finished frame */
void controls_draw_sprint(Surf *back);     /* controls.c: "SPRINT" on the HUD while running */
int hud_panel_art(void **image, RECT *src); /* hud.c: the HUD panel picture (surface, src rect) */
void hud_draw_picture_raw(void *picture, int x, int y);   /* hud.c: at a final screen position */
int layout_hud_dx(int design_x);           /* layout.c: HUD x offset (split panel) */
int layout_hud_gap(void);                  /* layout.c: width of the spare-weapon gap in the HUD */
int layout_on_hud_panel(int x, int y);     /* layout.c: game point on the (split) HUD panel */
int layout_hud_top(void);                    /* layout.c: topmost row of any HUD pixel (cheap pre-test) */
int weapons_hud_gap(void);                 /* weapons.c: the gap it needs (0 = none) */
int weapons_ui_at(int x, int y);            /* weapons.c: game point is on the spare-weapon column */
int weapons_kit_at(int x, int y);           /* weapons.c: game point is on the medkit box (tips.c) */
void weapons_click_at(int x, int y);       /* weapons.c: left click on a HUD weapon box uses it */
void weapons_drop_at(int x, int y);        /* weapons.c: right click on a HUD weapon box drops it */
typedef void (__fastcall *TouchFn)(BYTE *unit, void *item, int arg);
extern TouchFn weapons_touch_original;
void __fastcall weapons_touch(BYTE *unit, void *item, int arg);   /* FUN_004495c0 wrap */
typedef int (__fastcall *CollideFn)(void *point, void **hit);
typedef int (__fastcall *HitObjFn)(BYTE *obj, int height);
extern CollideFn bullets_collide_original;
extern HitObjFn bullets_hit_object_original;
int __fastcall bullets_collide(void *point, void **hit);            /* FUN_00435a10 wrap */
int __fastcall bullets_hit_object(BYTE *obj, int height);          /* FUN_0040e600 wrap */
typedef int (__fastcall *StopsFn)(BYTE *item, BYTE *proj, int height, int distance);
extern StopsFn bullets_stops_original;
int __fastcall bullets_stops(BYTE *item, BYTE *proj, int height, int distance);   /* FUN_00435c80 wrap */
void fog_draw(Surf *b);                                             /* fog.c: after the battlefield is drawn */
void hud_finish_frame(Surf *s);                                    /* hud.c: finished frame (weapons, sprint, objectives, corners) */
void layout_scale_corners(Surf *b);                                /* layout.c: enlarge the HUD corner zones */
int layout_hud_corner_scale(void);                                 /* layout.c: corner scale in quarters */
int layout_resize_click(int x, int y);                        /* layout.c: click on a HUD group's resize icon */
int layout_resize_icon_at(int x, int y);                      /* layout.c: group whose resize icon is there, or -1 */
int layout_resize_repeat_at(int x, int y);                    /* layout.c: inside the repeat window of the last resize */
int layout_art_margin(int d, int ry);                          /* layout.c: HUD art see-through margin (battlefield) */
void layout_draw_resize_icon(Surf *b);
void layout_unscale(int *x, int *y);                          /* layout.c: enlarged HUD point -> its 1x point */
void layout_hud_mask_row(int y, BYTE *row, int w);            /* layout.c: exact interface mask row (zoom) */
unsigned layout_hud_signature(void);
unsigned layout_hud_mask_signature(void);                       /* layout.c: changes when any mask row would (modern.c keeps rows between frames) */
int weapons_ui_rect(RECT *r);                                 /* weapons.c: the spare-weapon column (1x) */
void controls_block_left(void);                               /* controls.c: this left press is used up */
const char *minimap_vehicle_name(DWORD uid);                         /* minimap.c: "Jeep" / "Tank" / "Half-track" / "Convoy truck" from a vehicle UID */
void layout_minimap_cycle(void);                                   /* layout.c: minimap 1.5 -> 2 -> 2.5x (animated) */
float layout_group_scale(int g);                              /* layout.c: HUD group size (0 msg, 1 centre, 2 air, 3 minimap) */
void layout_group_cycle(int g);
void objectives_draw(Surf *b);                                     /* objectives.c: target sector, pinned objective */
void menu_toggle_crt(void);                                         /* menu.c: CRT filter on/off (F9) */
void menu_toggle_hd(void);                                          /* menu.c: all upscaled art on/off */
int objectives_font(int *font, int *colour);                       /* objectives.c: font for mod text */
void tips_draw(Surf *b);                                           /* tips.c: HUD tooltips, finished frame */
int controls_pointer(int *x, int *y);                              /* controls.c: pointer, game-screen coords */
void controls_frame_shown(void);                                   /* controls.c: frame flipped: keep its sub-pixel offset */
int controls_shown_subpx(float *x, float *y);                      /* controls.c: sub-pixel offset of the frame on screen */
void clock_frame_shown(int gameplay);                              /* clock.c: a frame is on screen (lock to display) */
int clock_ahead_ms(int *paused, DWORD *handed);                                     /* clock.c: last game time handed out minus the game clock (freeze report) */
double clock_frame_period(void);                                  /* clock.c: ms per frame (window's monitor, or fps_limit) */
int clock_tracing(void);                                           /* clock.c: Ctrl+Shift+T timing trace running */
void clock_trace_frame(const char *line);                          /* clock.c: one trace line */
void controls_trace_info(char *buf, int n);                        /* controls.c: unit / camera for the trace */
int panel_rect(RECT *r);                                           /* panel.c: open panel on screen */
int tips_rect(RECT *r);                                            /* tips.c: tooltip on screen (recent) */
void tips_note_rect(RECT r);
int objectives_box(RECT *r);                                       /* objectives.c: pinned box on screen */
int overlay_at(int x, int y);                                      /* panel.c: game point on a mod box */
int overlay_message(HWND h, UINT msg, WPARAM wp, LPARAM lp);       /* panel.c: window hook (F9, F10, clicks) */
int panel_is_open(void);                                           /* panel.c */
void panel_draw(Surf *b);                                          /* panel.c: finished frame */
typedef void (__fastcall *DrawCellFn)(BYTE *cell, void *clip);
extern DrawCellFn roofs_draw_cell_original;
void __fastcall roofs_draw_cell(BYTE *cell, void *clip);           /* FUN_0042f120 wrap (roofs.c) */
int roofs_cells_at(int wx, int wy, BYTE **out, int max);           /* roofs.c: drawn world sprites at a point */
void roofs_forget_cells(void);
int roofs_cell_hit_ok(BYTE *cell);                  /* roofs.c: the cell + its sprite record are readable for 0x42faf0 */
int roofs_cell_hit(BYTE *cell, int wx, int wy);     /* roofs.c: the game's sprite-mask test 0x42faf0, guarded */
void roofs_debug_near(int wx, int wy);
int roofs_cells_list(BYTE **out, int max);
void roofs_new_frame(void);                                        /* roofs.c: before the battlefield */
void roofs_draw_ghost(Surf *b);                                     /* roofs.c: after the battlefield */
void __fastcall roofs_layer_0(void *rect, void *list);            /* roofs.c: FUN_0042e950 per map layer (FUN_0042e3e0) */
void __fastcall roofs_layer_1(void *rect, void *list);
void __fastcall roofs_layer_2(void *rect, void *list);
void __fastcall roofs_air_cell(BYTE *cell, void *clip);             /* roofs.c: FUN_0042f120 per air-layer cell (call 0x42ebab) */
void roofs_air_pass(void);                                           /* roofs.c: call 0x42d6bf (FUN_0042e460, air layer): ghost redraw first */
int roofs_aircraft_focus(int *x, int *y, DWORD ms);                 /* roofs.c: a moving map object (helicopter) moved within ms: its middle */
void hdworld_cell_air(int air);   /* roofs.c: the cell being drawn is in the air layer (hdworld.c FL_AIR) */
int hdart_prepare(uint32_t *pix, int w, int h, int menu);         /* hdart.c: HD menu backgrounds */
const uint32_t *hdart_picture(int *w, int *h, int *serial);
int hdart_active(void);                                             /* hdart.c: menu shows an HD background */
HCURSOR hdart_menu_cursor(int scale);                              /* hdart.c: the game cursor as a Windows cursor */
void hdart_game_path(char *out, int n, const char *rel);             /* hdart.c: path next to the game exe */
uint32_t *hdart_load_image(const char *path, int *w, int *h);      /* hdart.c: PNG/BMP/... via WIC, BGRA */
uint32_t *hdart_load_image_alpha(const char *path, int *w, int *h);   /* hdart.c: PNG with its transparency, premultiplied */
typedef struct { int x, y, w, h; float u0, v0, u1, v1; int id; const uint32_t *pix; int pw, ph; unsigned serial; } HdOp;
void hdsprites_note_blit(Surf *d, RECT dr, Surf *s, RECT sr, int keyed, DWORD lo, DWORD hi);   /* hdsprites.c */
int hdsprites_masked_source(Surf *s, DWORD key, Surf *tmp);   /* hdsprites.c: recall pictures - skip pixels the HD art has no ink over */
void hdsprites_frame_shown(Surf *frame);
void hdsprites_dirty(Surf *s);
void hdsprites_forget(Surf *s);                                    /* hdsprites.c: surface freed */
void hdsprites_start(void);                                         /* hdsprites.c: start the catalogue thread */
int hdsprites_showing(const char *name);   /* hdsprites.c: the last whole-screen picture's file name contains name (lower case) */
#define ATLAS_SIZE   4096                                            /* hdworld: atlas slices of 4096^2 */
#define ATLAS_SLICES 4
void hdworld_start(void);                                           /* hdworld.c: open hdworld.pak */
void hdvideo_install(void);                                         /* hdvideo.c: upscaled videos */
const uint32_t *hdvideo_frame(int *w, int *h);
int hdvideo_playing(void);                                         /* hdvideo.c: a Smacker video is open */
void hdworld_note_copy(Surf *d, RECT dr, Surf *s, RECT sr, int keyed, DWORD lo, DWORD hi);
void hdworld_note_fill(Surf *d, RECT r);
void hdworld_clear_run(Surf *d, int y, int x0, int x1);
void hdworld_under_run(Surf *d, int y, int x0, int x1);   /* hdworld.c: ground-level overlay (route line): behind objects */
int hdworld_under_fill(Surf *d, int y, int x0, int x1, BYTE c);   /* hdworld.c: the same for a run in colour c (0 = no mask: draw per pixel) */
void hdworld_cell_low(int low);   /* roofs.c: the cell the game is drawing is a low object (rock, bush) */
void hdworld_cell_top(int top);   /* hdworld.c: the cell being drawn is a vehicle turret (above its hull) */
void hdworld_cell_over(int over);   /* roofs.c: drawn in the pass over every figure (roof part) or a figure drawn again over it */
void hdworld_cell_sort(int y);   /* roofs.c: the drawn cell's sort row inside its image (world_pos.y - wrect.top) */
void hdworld_cell_begin(const BYTE *cell);   /* roofs.c: the cell the game draws now (NULL after) */
void hdworld_skip_shadows(int on);   /* roofs.c: a figure drawn again over a building front - its shadow not again */
int hdworld_cell_image(const BYTE *cell, int idx, int *hx, int *hy, int *slice, int *wl, int *wt);
int hdworld_entry_at(int x, int y);                                  /* hdworld.c: image entry drawn at a back-buffer pixel */
int hdworld_pixel_upscaled(const Surf *b, int x, int y);              /* hdworld.c: the GPU shows the upscaled picture at this back-buffer pixel */
int hdworld_cell_has_entry(const BYTE *cell, int e);   /* objectives.c: an image drawn through it */
BYTE *hdworld_over_mask(Surf *b);
void present_perf_refs(LONGLONG ticks);
typedef void (*ParFn)(int lo, int hi, void *ctx);
void par_for(int n, ParFn fn, void *ctx);          /* par.c: split work over the cores */
int par_threads(void);
int controls_sarge_blocked_at(int x, int y);       /* controls.c: the game's own Sarge test (object masks) */
int controls_blocked_at(int x, int y);             /* controls.c: Sarge cannot stand here */
int path_find(int sx, int sy, int tx, int ty, POINT *out, int max);   /* pathfind.c */
int path_reachable(int sx, int sy, int tx, int ty, int budget_ms);   /* pathfind.c: 1 yes, 0 no, -1 undecided */
void path_regions_step(void);           /* pathfind.c: one slice of the whole-map walkable areas (per gameplay frame) */
int path_find_vehicle(int type, int sx, int sy, int tx, int ty, POINT *out, int max);   /* pathfind.c: route by the vehicle footprint */
int path_find_vehicle_begin(int type, int sx, int sy, int tx, int ty, int max);   /* the same search in slices (2026-11-21): */
int path_find_vehicle_step(double budget_ms, POINT *out, int max);                /* -1 = not done yet (next frame), else as path_find_vehicle */
void path_vehicle_crush(int on);                    /* pathfind.c: 1 (default) fence / bush tiles are driven over; 0 for walking routes */
/* vehicles.c / pathfind.c (2026-11-20): vehicle routes with reversing, smooth turret, getting unstuck */
typedef struct { short x, y, facing; signed char dir; } VehPose;   /* dir 1 forward, -1 backward, 0 turn on the spot */
int path_vehicle_plan_begin(int type, int sx, int sy, int facing, int tx, int ty, int can_spin, int step);
int path_vehicle_plan_step(double budget_ms);       /* 0 searching, 1 found, 2 closest pose, -1 none */
int path_vehicle_plan_result(VehPose *out, int max);
int path_vehicle_overlap(int type, double x, double y, int facing, int *objects);   /* blocking tiles under the footprint (objects: of them blocking objects) */
void vehicles_drive_start(BYTE *v, int tx, int ty, DWORD now);  /* controls.c: click to drive (vehicle_reverse) */
int vehicles_drive(BYTE *v, DWORD now);             /* per frame while driving there: 0 = arrived / gave up */
int vehicles_route(POINT *pts, int max);            /* the rest of the route (modern.c route line) */
int vehicles_steer(BYTE *v, int heading, int drive, int slow);   /* hull toward heading; drive 1 / -1 / 0 */
int vehicles_turn_room(BYTE *v, int heading);       /* vehicles.c: room to turn round to heading: 0 none, 1 on the spot / K-turn, +-2 jeep forward U-turn to that side */
void vehicles_aim_turret(BYTE *v, double wx, double wy, DWORD now, int snap);   /* turret_smooth */
void vehicles_aim_report(BYTE *v, int wx, int wy, int firing, const char *tag);   /* vehicles.c: aim diagnostic (log >= 2) */
int vehicles_muzzle(BYTE *v, int *x, int *y);       /* vehicles.c: where VehicleFireWeapon starts the shot */
int vehicles_aim_reachable(BYTE *v, int wx, int wy); /* vehicles.c: 0 = the gun cannot point there (halftrack arc) */
void __fastcall vehicles_update(BYTE *v);           /* call 0x419c52 -> VehicleUpdate 0x4176f0 */
void __fastcall vehicles_turret_lock(BYTE *v, int spin);   /* call 0x417ce8 -> VehicleLockTurret 0x417eb0 */
void hdworld_restore_field(Surf *b, int y, int x0, int x1);   /* hdworld.c: battlefield bytes back from the map (layout.c) */
void hdworld_forget(Surf *s);                          /* surface.c: before freeing its references */
void hdworld_frame_shown(Surf *b);
int hdworld_take(const BYTE *bits, int pitch, RECT view);   /* under the present lock */
/* hdworld.c -> gpu.c: one frame's inputs for the GPU reference resolve */
typedef struct {
    RECT view;
    const void *screen_refs; int screen_pitch;          /* screen references (whole surface) */
    const BYTE *over; RECT over_box;                    /* override mask (view size), where it is set */
    const BYTE *bytes;                                  /* the shown bytes (view size) */
    int nlinks, link[4][6];                             /* screen rect l t r b + offset into the map */
    int map_w, map_h;                                   /* map surface size (0 = none) */
    int ndirty; RECT dirty[64]; const void *dirty_refs[64];   /* changed map rects + their references */
    int nmoves; RECT move_src[32], move_dst[32];        /* map scrolls (copy within the map), applied before the rects */
    const uint32_t *ents; int nents;                    /* entry table when it changed, else NULL */
} HdWorldIn;
int hdworld_gpu_world(HdWorldIn *in);
void hdworld_force_map(void);
void hdworld_force_entries(void);
void hdworld_upload(void *ctx);
void hdworld_ramps(float out[64][4]);
int gpu_world_put(void *ctx, int slice, int x, int y, int w, int h, const uint32_t *pix, int pitch);   /* gpu.c */
typedef void (__fastcall *Blit5Fn)(int x, int y, const BYTE *data, int x0, int y0, int x1, int y1);
typedef void (__fastcall *Blit6Fn)(int x, int y, const BYTE *data, int x0, int y0, int x1, int y1, const BYTE *remap);
extern Blit5Fn hdworld_wrle_original, hdworld_drle_original, hdworld_shadow_original;
extern Blit6Fn hdworld_remap_original;
void __fastcall hdworld_wrle(int x, int y, const BYTE *d, int x0, int y0, int x1, int y1);
void __fastcall hdworld_drle(int x, int y, const BYTE *d, int x0, int y0, int x1, int y1);
void __fastcall hdworld_remap(int x, int y, const BYTE *d, int x0, int y0, int x1, int y1, const BYTE *remap);
void __fastcall hdworld_shadow(int x, int y, const BYTE *d, int x0, int y0, int x1, int y1);
void window_keep_shape(HWND h, WPARAM edge, RECT *r);              /* window.c: WM_SIZING keeps the picture shape */
int hdsprites_prepare(const BYTE *bits, int pitch, RECT view, const HdOp **ops, const BYTE **idmap);
void hdsprites_scale_back(RECT src, int dx, int dy, int q);   /* hdsprites.c: notes follow an enlarged HUD group */
void hdsprites_new_frame(void);
void objectives_note_text(int x, int y, int font, int colour);      /* objectives.c: message box font (hud.c) */
typedef void (__fastcall *DashMsgFn)(const char *msg, void *unit);
extern DashMsgFn objectives_dash_message_original;
void __fastcall objectives_dash_message(const char *msg, void *unit);   /* FUN_00441300 wrap */
extern VoidFnPtr controls_camera_original;
void __cdecl controls_camera(void);         /* FUN_0040dcd0 wrap: smooth camera (modern controls) */
void patches_set_modern_controls(int on);  /* patches.c */
unsigned view_frame_count(void);
/* hud.c */
struct UiPicture;
void __cdecl hud_draw_cursor(void);
typedef void (__fastcall *DrawStringFn)(void *surface, int x, int y, const char *text, int font, int *clip, int colour);
extern DrawStringFn hud_draw_string_original;
void __fastcall hud_draw_string(void *surface, int x, int y, const char *text, int font, int *clip, int colour);
void *hook_wrap(const char *name, uintptr_t va, const BYTE *orig, size_t n, void *target);
typedef void (__fastcall *MinimapRectFn)(int x1, int y1, int x2, int y2);
typedef void (__fastcall *MinimapPlotFn)(int x, int y);
extern MinimapRectFn hud_minimap_rect_original;
extern MinimapPlotFn hud_minimap_plot_original;
void __fastcall hud_minimap_rect(int x1, int y1, int x2, int y2);
void __fastcall hud_minimap_plot(int x, int y);
void __fastcall hud_draw_picture(struct UiPicture *pic, int x, int y);
void __fastcall hud_draw_picture_on_screen(struct UiPicture *pic, int x, int y);
void __fastcall hud_fill_on_screen(RECT *r, DWORD colour);
HFONT WINAPI font_CreateFontA(int height, int width, int escapement, int orientation, int weight, DWORD italic,
                              DWORD underline, DWORD strikeout, DWORD charset, DWORD out_precision,
                              DWORD clip_precision, DWORD quality, DWORD pitch_family, LPCSTR face);
typedef int (__fastcall *GdiTextFn)(void *surface, int x, int y, const char *text, int font, int *size_out);
extern GdiTextFn hud_draw_gdi_text_original;
int __fastcall hud_draw_gdi_text(void *surface, int x, int y, const char *text, int font, int *size_out);

/* present.c: how a picture is scaled to the window */
enum { SMOOTH_OFF, SMOOTH_FULL };          /* square pixels (the game) / bilinear (videos) */

/* window.c: the window can be resized to any size */
void window_minmax(HWND h, MINMAXINFO *mm);
void window_allow_resize(HWND h);
int window_fullscreen_message(HWND h, UINT msg, WPARAM wp, LPARAM lp);   /* maximise, Alt+Enter, F11 */
void window_set_fullscreen(HWND h, int on);
int window_is_fullscreen(void);

/* menu.c: Gameplay menu in the game window's menu bar */
void menu_install(HWND h);
int menu_command(HWND h, int id);
void menu_popup(HMENU m);
const char *mod_ini_path(void);            /* ddraw.c */

/* resize.c: change the render size while running (fullscreen = more battlefield, same HUD size) */
void resize_request(HWND h);
void resize_poll(void);                    /* resize.c: deferred resize, once gameplay runs */
int resize_message(HWND h, UINT msg);
int resize_in_progress(void);
/* zoom.c: battlefield zoom (the interface is not zoomed) */
int layout_any_panel_open(void);           /* layout.c */
float zoom_target(void);
int zoom_active(void);
float zoom_factor(void);                   /* eased current factor; display thread, once per frame */
void zoom_centre(float *x, float *y);      /* game pixels */
void zoom_to_game(POINT *p);               /* shown (unzoomed) game point -> battlefield point */
void zoom_from_game(POINT *p);
void zoom_step(int dir);
void zoom_cycle(void);
int zoom_message(UINT msg, WPARAM wp);
void window_reapply_fullscreen(HWND h);    /* window.c: borderless style again after resize.c */

/* modern.c: modernization features, each switchable ([modern] in the ini, F10 panel "MODERN" page) */
enum { MOD_AIM_ASSIST, MOD_ROLL, MOD_STAMINA, MOD_SIDE_BUTTONS, MOD_SPRINT_TOGGLE, MOD_HIT_MARKERS, MOD_DAMAGE_DIR,
       MOD_ARROWS, MOD_AMMO, MOD_RANGE_RING, MOD_ROUTE, MOD_PEEK, MOD_VEHICLE_ZOOM, MOD_NO_SHAKE, MOD_SKIP_VIDEO,
       MOD_HEALTH_BARS, MOD_STATS, MOD_RESTART,
       MOD_GAMEPAD, MOD_THROW_POINTER, MOD_COLOURBLIND, MOD_SQUAD_MOUSE, MOD_RADIAL, MOD_PING, MOD_HALF_DAMAGE,
       MOD_THROW_HOLD, MOD_STANCE_HINTS, MOD_ENEMY_LABELS,
       MOD_COUNT };
enum { MK_ROLL, MK_PEEK, MK_STATS, MK_RADIAL, MK_PING, MK_RESTART, MK_COUNT };   /* the mod's own keys ([keys], KEYS page) */
typedef struct { int connected; float lx, ly, rx, ry, lt, rt; WORD held, pressed; } ModPad;   /* XInput buttons */
int modern_on(int feature);
int modern_count(void);
const char *modern_name(int feature);
char modern_size(int feature);
void modern_toggle(int feature);
/* modmenu.c: the mod settings as a screen of the game's own menus (pause menu, main menu) */
void modmenu_install(void);
void framerate_install(void);                                  /* framerate.c: 120 Hz+ fixes in the exe */
int framerate_gravity(void);
int modmenu_open(void);
int game_in_play(void);                                   /* modmenu.c: the game is in its play state (a mission) */
int modmenu_panel_rect(RECT *r);                          /* modmenu.c: settings panel on screen (play) */
int modmenu_f10(void);                                     /* modmenu.c: F10 = Mod Settings */
int layout_open_panels(RECT *out, int max);                   /* layout.c: open pop-ups / signs (screen rects) */
int game_menu_up(void);                                     /* modmenu.c: an option screen over a paused mission */
int layout_menu_scale(void);                               /* layout.c: in-game menu size factor in 64ths, half steps (0 = no menu up) */
void layout_menu_place(int k, int *x, int *y);
void modmenu_tick(void);
int write_mem(uintptr_t va, const void *src, size_t n);       /* patches.c */
/* panel.c: the switches, shared with modmenu.c */
int panel_row_count(void);
int panel_row_value(int i);
void panel_row_set(int i, int on);
void panel_speed_cycle(void);
void panel_zoom_cycle(void);
int modern_key(int k);                     /* modern.c: the keyboard key (VK) of an MK_* key - now from bindings.c */

/* bindings.c (2026-11-20): every modern-controls action with a keyboard and a mouse binding ([bindings] in the
 * ini, F10 panel BINDS page). Order = the BINDS page; append-only is not needed (ini names are the keys). */
enum { ACT_UP, ACT_LEFT, ACT_DOWN, ACT_RIGHT, ACT_SPRINT, ACT_SLOW,
       ACT_KNEEL,          /* one stance down: stand -> kneel -> prone (tap C / click Sarge) */
       ACT_PRONE,          /* straight to prone (hold C / double-click Sarge) */
       ACT_STAND,          /* stand up (double-tap C / click prone Sarge) */
       ACT_DIVE, ACT_ROLL,
       ACT_WEAPON1, ACT_WEAPON2, ACT_WEAPON3, ACT_WEAPON4, ACT_WEAPON5, ACT_WEAPON6,
       ACT_NEXT_WEAPON, ACT_PREV_WEAPON, ACT_QUICK_SWAP, ACT_DROP, ACT_SPECIAL, ACT_ENTER, ACT_MAP, ACT_OBJECTIVE,
       ACT_RADIAL, ACT_PING, ACT_STATS, ACT_PEEK, ACT_RESTART,
       ACT_HEAL,           /* allies.c: medkit selected (heal a soldier by clicking him) */
       ACT_COUNT };
/* Input-aware hints (for any hint / tip code):
 *   controls_last_device()  0 = the mouse, 1 = the keyboard was used last for an action (gameplay)
 *   controls_hint_input(a)  how to do action a on that device with the CURRENT bindings, e.g.
 *                           ACT_PRONE -> "double-click Sarge" / "hold C", ACT_DIVE -> "click mouse button 4" /
 *                           "press Space"; falls back to the other device when there is no way on this one;
 *                           "" for an unknown action. Static text, valid until the 8th next call. */
int controls_last_device(void);
const char *controls_hint_input(int action);
void bindings_note_device(int keyboard);                  /* controls.c: a mouse button / key was used */
void bindings_poll(DWORD now, int mouse_ok, int paused);  /* controls.c: once per gameplay frame */
void bindings_reset_state(void);                          /* controls.c: gameplay (re)started - held inputs ignored */
int bindings_pressed(int action);                         /* one-shot action this frame */
int bindings_held(int action);                            /* continuous action held now */
int bindings_from_mouse(int action);                      /* ... it came from the mouse binding */
int bindings_source_vk(int action);                       /* ... the input (VK) that gave it */
int bindings_vk(int action, int slot);                    /* active binding VK (slot 0 keyboard, 1 mouse), 0 = none */
int bindings_direct(int action);                          /* game key-table action the game reads directly */
int bindings_key_diks(int action, BYTE out[2]);           /* DIK scan code(s) of the keyboard binding */
int bindings_vk_diks(int vk, BYTE out[2]);
int bindings_reserved_dik(int dik);                       /* any active keyboard binding uses this scan code */
void bindings_hide(BYTE *keys);                           /* kb_get_state: our keys out of the game's state */
int bindings_mk_vk(int mk);
void bindings_text(int action, char *s, int n);           /* "C", "C:hold", "-" */
void bindings_reset_defaults(void);
int bindings_panel_rows(int max_rows);                    /* panel.c BINDS page */
void bindings_panel_draw(Surf *b, int x, int y, int w, int font, int colour);
int bindings_panel_key(int vk);
int bindings_panel_click(int x, int y);
void bindings_panel_wheel(int delta);
int bindings_panel_capturing(void);
void controls_bindings_changed(void);                     /* controls.c: bindings edited - the game's keys again */
/* controls.c: stance commands (stance radial, hints): 0 stand, 1 kneel, 2 prone, 3 dive, 4 roll left, 5 roll right */
enum { STANCE_CMD_STAND, STANCE_CMD_KNEEL, STANCE_CMD_PRONE, STANCE_CMD_DIVE, STANCE_CMD_ROLL_L, STANCE_CMD_ROLL_R, STANCE_CMD_COUNT };
void controls_stance_command(int cmd);
int controls_radial_on_sarge(void);                       /* controls.c: the radial being opened started on Sarge */
/* controls.c: the drag stance picker (press on Sarge + drag): Sarge's world feet, selected 0 stand / 1 kneel /
 * 2 prone, current pose the same way; 0 = not shown */
int controls_stance_picker(int *wx, int *wy, int *sel, int *cur);   /* sel 3 = DIVE */
#define STANCE_PICKER_BASE 34   /* px above Sarge's feet: centre of the lowest box (DIVE); PRONE, KNEEL, STAND above it */
#define STANCE_PICKER_BOXES 4   /* 0 STAND, 1 KNEEL, 2 PRONE, 3 DIVE (2026-11-21, user: "add dive to the bottom") */
#define STANCE_PICKER_STEP 15   /* px between the box centres */
#define STANCE_PICKER_W    50   /* box width */
#define STANCE_PICKER_DRAG 12   /* px of vertical drag from the press on Sarge that opens the picker */
int panel_wheel(int delta);                               /* panel.c: wheel while the F10 panel is open (1 = used) */
void modern_note_hit(BYTE *item);           /* bullets.c: Sarge's projectile stopped at item */
void modern_lane(float out[4]);              /* modern.c: mission path band colours (palette indices) + opacity, for gpu.c */
void modern_note_shot(void);                /* controls.c: Sarge fired */
void modern_note_damage(int from_x, int from_y);   /* bullets.c: an enemy projectile hit Sarge */
/* aware.c: enemy awareness of Sarge ([modern] enemy_awareness) */
#define AW_SEES      1                      /* aware_info: sees Sarge now (game test + stance-aware line) */
#define AW_TARGETING 2                      /* aware_info: his victim is Sarge / the vehicle he drives */
void aware_frame(void);                     /* modern_frame, every gameplay frame */
void aware_note_shot(void);                 /* modern_note_shot */
void aware_note_hit(BYTE *item);            /* modern_note_hit */
int aware_info(BYTE *t, float *level);      /* flags or -1 (unknown / off); *level 0..1 */
int aware_exposure(int *targeting, int *could);   /* 0..10 halves; noticed-and-targeting count, could-see count */
int __fastcall aware_sight_tick(int *weapon, BYTE *shooter, int cell);                         /* call 0x43ccc3 */
void __fastcall aware_fire_step(BYTE *t, BYTE *victim, int p1, int p2, int p3, int p4);        /* call 0x43d05b */
void aware_note_damage(BYTE *item, DWORD from);                    /* bullets.c ItemDamage wrap: Sarge (or his vehicle) did damage */
int aware_filter_seen(BYTE *viewer, BYTE *target, int seen);       /* bullets.c bullets_can_see: Sarge seen only by units that noticed him */
void __fastcall aware_unit_target(BYTE *target, BYTE *entry);      /* call 0x43aa03 (FUN_0043aa10) */
void __fastcall aware_script_attack(DWORD who, DWORD target, int a, int b, int c, int d);   /* call 0x4526ab (log only) */
void modern_ping(int wx, int wy);
void modern_frame(void);                    /* controls.c dispatcher, every gameplay frame */
int modern_no_zoom_now(void);               /* zoom.c */
void __cdecl modern_shake(void);            /* the game's shake call (patches.c) */
int modern_message(HWND h, UINT msg, WPARAM wp, LPARAM lp);   /* window hook (ddraw.c) */
void modern_palette(Pal *p, DWORD start, DWORD n);             /* ddraw.c after SetEntries */
const ModPad *modern_pad(void);
void modern_radial(int open, int px, int py);       /* the radial menu; opened while controls_radial_on_sarge() = the stance ring */
int modern_radial_open(void);
void modern_draw(Surf *b);                  /* hud.c finished frame */
int controls_hover_range(int *range);       /* controls.c: an enemy is hovered: weapon range (world px) */
int controls_reticle_visible(void);         /* controls.c */
void controls_game_keys(int modern);   /* controls.c: modern keys in the game's own key table */
BYTE *controls_hover_target(void);   /* controls.c: target item under the pointer or NULL */
int controls_throw_preview(int *wx, int *wy, int *radius, int *type, int *clamped, int *dist);   /* controls.c: grenade / mortar landing spot + blast radius */
int controls_reload(double *frac);   /* controls.c: the weapon is reloading (long reloads only), 0..1 done */
int controls_route(POINT *pts, int max);    /* controls.c: Sarge + the rest of his click-to-walk route */
int controls_reticle(int *x, int *y);       /* controls.c: reticle, game screen (before the map-bottom shift) */
float controls_stamina(void);               /* controls.c: 0..1 */
int objectives_target(int *wx, int *wy);    /* objectives.c: centre of the objective's sector (world) */
/* objectives.c -> present.c -> gpu.c: what the target paint needs, in game-picture pixels of one frame.
 * r[0 .. nsec) target sectors, r[nsec .. nspot) target spots (pads), r[nspot .. n) boxes of the objects that sit
 * on a sector border, key[] = what tells such an object's pixels on the GPU: (atlas xy of its image - its box corner)
 * modulo 2048 and the atlas slice, slice -1 = not known (hdworld_cell_atlas). */
#define ZONES_MAX 32
/* fade / glow: sector line strength and glow 0..1 (objectives.c timing); dim: the pinned objective box, darkened
 * on the upscaled battlefield (dim_on 0 = none) */
typedef struct { RECT r[ZONES_MAX]; int key[ZONES_MAX][3]; int nsec, nspot, n; float fade, glow; RECT dim; int dim_on; int menu_over; RECT panel[4]; int npanel;
                 /* building interior (roofs.c): picture px of cell (0,0) of a 64x64 window of 8 px cells, strength 0..1,
                  * one bit per floor cell (row y = in_mask[2y] bits 0..31, in_mask[2y+1] 32..63), the building's image keys */
                 int in_ox, in_oy, in_nkey, in_sx, in_sy, in_cr, in_nen, in_en[4][3]; float in_s; unsigned in_mask[128], in_door[128]; int in_key[8][3]; } TargetZones;
void roofs_interior_note(TargetZones *z, int cx, int cy);
int menutext_capture(void *surface, int x, int y, const char *text, int font, int *size_out);   /* menutext.c (hud.c) */
void menutext_note_blit(const RECT *r, int desktop);                  /* menutext.c (surface.c): a picture drawn on the screen */
void menutext_clear(void);
void squash_stub(void);                                              /* gameplay.c: replaces the call at 0x43c840 */
void crush_stub(void);                                              /* gameplay.c: replaces the ItemDamage call at 0x40cff3 (crush) */
int barricade_category(int category);                               /* barricades.c: ROCK, FALLEN_TREE, FENCE, WALL, GATE, WALL_GATE */
void barricades_frame(void);                                        /* barricades.c: Sarge never stays stuck on a barricade */
int fences_only_at(int x, int y, int strict);                       /* fences.c: blocked for Sarge only by breakable wooden fences (strict = planner rule) */
void fences_walk(BYTE *u, int facing, int route);                   /* fences.c: controls walk Sarge toward facing (route = click route) */
void fences_frame(void);                                            /* fences.c: per gameplay frame (push rule for other input) */
extern int g_fences_breaking;                                       /* fences.c: a fence is being broken (gameplay.c crush_allowed) */
int path_cell_class(int x0, int y0, int centre_only);               /* pathfind.c: 8 px cell 1 free, 2 blocked, 3 breakable fence only */
int path_route_fence_near(int x, int y, int r);                     /* pathfind.c: a fence cell of the last click route within r px */
int menutext_overlay(int dw, int dh, int pw, int ph, const uint32_t **pix, unsigned *ver, RECT *dirty);   /* menutext.c (gpu.c) */
int gpu_overlay_ready(void);
#define MT_SLIDERS 8
typedef struct { RECT r; int steps, pos, active; float fk; char value[16]; } MtSlider;   /* menutext.c: a slider (design px) */
void menutext_set_sliders(const MtSlider *s, int n, int state);       /* modmenu.c: shown while that screen state is up */
int modmenu_hover(char *text, int n, RECT *row);                     /* modmenu.c (menutext.c, display thread): hover info of the row under the pointer */
/* radio.c: radio messages drawn as an overlay over the battlefield ([display] radio_overlay) */
typedef int (__fastcall *LoadSpriteFn)(void *sprite, const char *name, int flags);
extern LoadSpriteFn radio_load_sprite_original;
int __fastcall radio_load_sprite(void *sprite, const char *name, int flags);   /* wrap of LoadSprite FUN_00412840 */
int radio_take(void *pic, int kind, const RECT *r);                    /* hud.c: 1 = this radio picture is drawn by the mod */
void radio_note_screen_picture(const RECT *r);                         /* hud.c: another picture on the SCREEN (cut out) */
int radio_overlay(int dw, int dh, int pw, int ph, const uint32_t **pix, unsigned *ver, RECT *dirty);   /* radio.c (gpu.c) */
void modmenu_slider_mouse(UINT msg, LPARAM lp);                        /* modmenu.c (ddraw.c wndproc): drag a slider */                                          /* gpu.c: the label layer can be drawn */            /* roofs.c: Sarge inside a building -> floor plan */
void objectives_zones(TargetZones *out);
typedef struct { int x, y, kind, army; } ObjMark;   /* army: 0 green 1 tan 2 blue 3 grey, -1 none (pads, objects) */   /* kind: 0 destroy, 1 pick up, 2 look after, 3 go to */
int objectives_marks(ObjMark *out, int max);  /* objectives.c: mission targets to point at (world) */
int view_is_video(void);                    /* view.c */

/* ---- game-thread profile (present.c prof_add; printed per frame by the timing trace) */
/* PR_H_*: parts of PR_HUD (2026-11-20, stutter hunt): weapons, modern (+ mission path), objectives (+ zones, interior), corners, minimap, rest
 * 2026-11-21 (stutter hunt 2): parts of PR_CONTROLS (c.keys = the game's own keyboard dispatcher, c.apply = modern
 * controls, c.gp = gameplay_frame, c.mod = modern_frame incl. aware_frame), parts of PR_OVERLAY (o.bars = health bars,
 * o.mpath = mission path build, o.mdraw = its drawing), path = our path searches (nested wherever they run), and
 * the gameplay hooks the game calls from its own update (aware / patrol / bullets / veh / mines / allies): those
 * are EXCLUSIVE - the game's original functions they call (PX_GAME) are not counted, so they say what OUR code
 * costs. Top-level time (a stage not inside another one) is summed so the trace can print "rest" = work minus
 * all of ours = the game's own code. */
enum { PR_CONTROLS, PR_REGIONS, PR_CAMERA, PR_FIELD, PR_CELLS, PR_BLIT, PR_GHOST, PR_HUD, PR_OVERLAY, PR_SHOWN,
       PR_H_WEAP, PR_H_MODERN, PR_H_OBJ, PR_H_CORNERS, PR_H_MINIMAP, PR_H_REST,
       PR_C_KEYS, PR_C_APPLY, PR_C_GAMEPLAY, PR_C_MODERN, PR_O_BARS, PR_O_MPATH, PR_O_MDRAW, PR_PATH,
       PR_AWARE, PR_PATROL, PR_BULLETS, PR_VEH, PR_MINES, PR_ALLIES, PR_CHOOKS, PR_UI, PR_DD, PR_N };
int present_thread_priority(void);               /* present.c: [display] thread_priority (1 = game / display threads raised) */
void prof_add(int k, LONGLONG ticks);
void prof_add_top(LONGLONG ticks);                 /* QPC ticks of a top-level stage (present.c) */
extern int g_prof_depth;                           /* game thread: open PROF stages */
extern unsigned long long g_px_child;              /* cycles spent in children of the open PX stage (below) */
/* (a PROF stage inside a PX stage is a child of it: its cycles are added to g_px_child) */
#define PROF_BEGIN(v) LARGE_INTEGER v; QueryPerformanceCounter(&v); unsigned long long v##_c = __rdtsc(); g_prof_depth++
#define PROF_END(v, k) do { LARGE_INTEGER pr_b_; QueryPerformanceCounter(&pr_b_); prof_add(k, pr_b_.QuadPart - v.QuadPart); \
                            g_px_child += __rdtsc() - v##_c; if (--g_prof_depth == 0) prof_add_top(pr_b_.QuadPart - v.QuadPart); } while (0)
/* exclusive hook stages (CPU cycles, __rdtsc: a few ns per call; present.c converts them with a QPC calibration):
 *   PX_BEGIN(t); ... PX_GAME(original(...)); / r = PX_CALL(original(...)); ... PX_END(t, PR_X);
 * every return path between BEGIN and END must pass PX_END (the hooks: body renamed NAME_px, NAME = timed wrapper) */
void prof_add_cyc(int k, unsigned long long cyc, int top);
#define PX_BEGIN(v) unsigned long long v##_t0 = __rdtsc(), v##_sv = g_px_child; g_px_child = 0
#define PX_END(v, k) do { unsigned long long px_el_ = __rdtsc() - v##_t0; prof_add_cyc(k, px_el_ - g_px_child, g_prof_depth == 0); g_px_child = v##_sv + px_el_; } while (0)
#define PX_GAME(code) do { unsigned long long px_g0_ = __rdtsc(), px_gs_ = g_px_child; code; g_px_child = px_gs_ + (__rdtsc() - px_g0_); } while (0)
#define PX_CALL(expr) ({ unsigned long long px_g0_ = __rdtsc(), px_gs_ = g_px_child; __typeof__(expr) px_r_ = (expr); g_px_child = px_gs_ + (__rdtsc() - px_g0_); px_r_; })

/* screenbtn.c */
typedef int (__fastcall *SBtnStateFn)(BYTE *btn, int state);
extern SBtnStateFn screenbtn_set_state_original;
int __fastcall screenbtn_set_state(BYTE *btn, int state);
void screenbtn_lbuttondown(HWND h, int inside);
int screenbtn_message(HWND h, UINT msg, WPARAM wp);
int bullets_fired_within(DWORD uid, DWORD ms);
int bullets_level_at(int x, int y);                 /* bullets.c: ground level (elevation / 4), -1 off the map */
/* bullets.c height_rules ([gameplay] height_rules = stance | am2 | original; modded: ini only; off = stance) */
enum { HEIGHTS_STANCE, HEIGHTS_MODDED, HEIGHTS_ORIGINAL, HEIGHTS_AM2, HEIGHTS_COUNT };
extern const char *const HEIGHT_RULE_NAMES[HEIGHTS_COUNT];  /* "stance", "modded", "original", "am2" (ini values) */
extern const char *const HEIGHT_RULE_TEXT[HEIGHTS_COUNT];   /* F10 panel: name + what it means */
int height_rules_parse(const char *s);                      /* bullets.c: ini value -> mode ("off" = stance) */
void height_rules_set(int mode);                            /* bullets.c: set + write the ini */
void height_rules_next(void);                               /* bullets.c: stance -> am2 -> original -> stance (modded -> stance) */
int __fastcall bullets_game_double(int damage, int dir, BYTE *item);   /* call 0x435b10 (the game's direction x2, 0x432050) */
/* the reticle cues for Sarge's shot at world point (tx, ty) with weapon type `weapon`, by the active height_rules
 * (bullets.c, the same rules the shots follow). block: 0 none, 1 CLIFF (go up), 2 RIDGE (move), 3 his own low cover
 * (needs `raise` = 0xb kneel / 0xc stand to fire over it); bx, by = the stop point. levels: show UP n / DOWN n.
 * over: the round passes over the unit under the pointer (original rules). mg_low: the test line lies that many
 * px below the reticle (original rules, vehicle machine gun from a hill), at (mx, my). */
typedef struct { int block, bx, by, raise, levels, over, mg_low, mx, my; } ShotCue;
int bullets_shot_cue(int tx, int ty, int weapon, ShotCue *c);   /* 1 = c filled */
/* defensive hint while Sarge is under fire (bullets.c): the text for the stance that helps, or 0. down / up = the
 * key names for one stance down / one stance up. */
int bullets_stance_hint(char *buf, int n, const char *down, const char *up);
typedef void (__fastcall *ItemDamageWrapFn)(BYTE *item, int damage, int type, void *at, DWORD from);
extern ItemDamageWrapFn bullets_item_damage_original;
void __fastcall bullets_item_damage(BYTE *item, int damage, int type, void *at, DWORD from);   /* ItemDamage 0x431f60 wrap */
int __fastcall bullets_damage_pose(void *avatar);           /* calls 0x4480b8 / 0x43c6c4 (GetPose in the prone rule) */
void bullets_sight_stub(void);                              /* call 0x44a47d (CanSeeFromItem in ItemUpdateSeen) */
void controls_stance_keys(char *down, char *up, int n);     /* controls.c: key names: one stance down / one up */

/* guarded calls into game code (ddraw.c): see the comment there */
typedef struct Guard { void *jb[5]; void *seh; const char *what; struct Guard *prev; } Guard;
void guard_enter(Guard *g, const char *what);
void guard_leave(Guard *g);
#define GUARD_TRY(g, what) (guard_enter(&(g), (what)), __builtin_setjmp((g).jb) == 0)
int controls_pointer_world(int *x, int *y);
void controls_fire_state(int *lock_ms_left, int *cancelled);
int controls_click_hint(void);                                  /* controls.c: rifle held after its shot */   /* controls.c: fire lock / cancel indicator */        /* controls.c: world point under the pointer */

/* missionpath.c (2026-11-20): the mission route painted on the ground */
int objectives_need(const char *need);                             /* objectives.c: briefing contains / lacks a word */
int objectives_item_ground(const char *name, int *x, int *y, int *raw, BYTE **item);   /* objectives.c: named item on the ground */
typedef struct { short x, y; BYTE dashed, wp; } MissionPt;   /* dashed: the leg INTO this point was not found (straight); wp: 1.. waypoint number, 0 = bend */
int path_line_free(int x0, int y0, int x1, int y1);                 /* pathfind.c */
void missionpath_install(void);                                    /* missionpath.c: [debug] start_mission (state 2 update entry) */
void missionpath_frame(void);                                      /* missionpath.c: per gameplay frame (game thread): build once per mission */
int missionpath_points(const MissionPt **pts);                     /* missionpath.c: the route (world px), 0 = none / not built yet */
int missionpath_points_key(void);                                  /* missionpath.c: changes when another leg is shown (lane draw-in restart) */
void missionpath_screen_note(void);                                /* missionpath.c: every frame (dd_present, game thread): menu seen / play left */
/* capture.c: Sarge drives empty enemy vehicles ([gameplay] drive_enemy_vehicles) */
typedef void (__fastcall *CaptureTwoFn)(BYTE *a, BYTE *b);
typedef void (__fastcall *CaptureOneFn)(BYTE *a);
extern CaptureTwoFn capture_sarge_in_original, capture_unit_enter_original;
extern CaptureOneFn capture_sarge_out_original;
void __fastcall capture_sarge_in(BYTE *sarge, BYTE *vehicle);     /* wraps SargeInVehicle 0x448ae0 */
void __fastcall capture_sarge_out(BYTE *sarge);                   /* wraps SargeOutVehicle 0x448b50 */
void __fastcall capture_unit_enter(BYTE *unit, BYTE *vehicle);    /* wraps UnitEnterVehicle 0x41d800 */
void __fastcall capture_handle_vehicle(BYTE *vehicle);            /* call 0x4296b6 (RunAI -> HandleVehicle) */
void __cdecl capture_enter_key(void);                             /* rewrite of FUN_00418910 (Enter Vehicle key) */
void capture_frame(void);                                         /* once per gameplay frame (gameplay.c) */
int capture_allowed(void);                                        /* option on and a one-player game */
int capture_can_enter(BYTE *vehicle);                             /* Sarge may climb in (own: the game's rule) */
BYTE *__fastcall capture_vehicle_find(DWORD uid);                  /* capture.c: VehicleFind 0x4188c0 rewrite (NULL-safe) */
int capture_hover_ok(BYTE *vehicle);                              /* another army's vehicle: enter pointer near Sarge */
int capture_vehicle_crewed(BYTE *vehicle);                        /* capture.c: another army's vehicle has a crew (THE crew rule) */
const char *capture_crew_reason(BYTE *vehicle);                   /* capture.c: "Occupied ..." why it counts as crewed, NULL = empty */
const char *capture_hover_note(int wx, int wy, int *vx, int *vy);   /* capture.c: why an enemy vehicle cannot be taken */
BYTE *capture_vehicle(void);
int capture_driver_seat(int *wx, int *wy, int *facing);           /* capture.c: captured vehicle position + hull facing */
int __fastcall deploy_force_records(DWORD *rec, int count, int full);  /* deploy.c: FUN_004196e0 rewrite (skips a record whose item is gone) */
void __fastcall deploy_vehicle_weapons(DWORD *rec, int count, void *unused);   /* deploy.c: FUN_0041ab30 rewrite (NULL-safe) */
typedef void (__fastcall *ArmySaveFn)(void *file);
extern ArmySaveFn capture_army_save_original;
void __fastcall capture_army_save(void *file);                   /* capture.c: wraps the army file writer FUN_0041a3e0 */
void controls_facing_vector(int facing, double *dx, double *dy);   /* controls.c: unit vector of a facing */
int modern_text(Surf *b, int x, int y, const char *s, BYTE c);       /* modern.c: the mod's own 5x7 text */                                      /* the vehicle Sarge took from another army, or NULL */
/* minimap.c (2026-11-20): minimap blips, contrast, hover details */
int layout_hud_point(int design_x, int design_y, int *sx, int *sy);   /* layout.c: HUD design point -> finished frame (enlarged), returns scale in 64ths */
void minimap_note_frame(int x1, int y1, int x2, int y2);              /* hud.c hud_minimap_rect: the game drew the minimap - copy its pins */
void minimap_picture_drawn(void *back, const void *pic, int px, int py, int w, int h);   /* hud.c hud_draw_picture: picture drawn (contrast) */
void minimap_draw(Surf *b);                                           /* hud.c finished frame: blips over the enlarged minimap */
void minimap_draw_tip(Surf *b);                                       /* hud.c finished frame: tooltip of the blip under the pointer */
int minimap_hovering(void);                                           /* tips.c: a blip is under the pointer */
int minimap_world_at(int x, int y, int *wx, int *wy);                 /* minimap.c: game-screen point on the minimap -> world px */
/* orders.c (2026-11-21): ATTACK / DEFEND pick their target with the command cursor; command targets via the minimap */
typedef void (__fastcall *CommandClickFn)(int confirm);
extern CommandClickFn orders_command_click_original;                  /* FUN_00441000 trampoline */
void __fastcall orders_command_click(int confirm);                    /* FUN_00441000 wrap */
void __cdecl orders_attack_key(void);                                 /* rewrite of 0x460dd0 (F2, HUD, radial) */
void __cdecl orders_defend_key(void);                                 /* rewrite of 0x460e00 (F3, HUD, radial) */
void orders_set_pick(int mode, int on, const char *why);              /* picking state for mode 2 / 3 on / off */
int orders_picking(void);                                             /* the command cursor picks a spot now */
int orders_minimap_click(int x, int y);                               /* controls.c: left press on the minimap while picking */
int orders_message(HWND h, UINT msg, WPARAM wp, LPARAM lp);           /* window hook: Esc cancels picking */
int orders_take_click(void);                                           /* controls.c: a command click was confirmed - swallow that press */
const char *orders_pick_label(void);                                  /* tips.c: the pointer label while picking */
int panel_row_of(const int *value);                                   /* panel.c: row switching that value (modmenu.c) */
void bindings_short(int action, char *s, int n);   /* bindings.c: "C", "hold C", "2x C", "Mouse4", "" */
void squad_frame(void);                                               /* squad.c: per gameplay frame, who of the squad sprints */
void patrol_frame(void);                                              /* patrol.c: per gameplay frame, enemy march patrols turn round */
void __fastcall patrol_order(BYTE *u, int order, DWORD goal, BYTE *target, int a3, int a4, int a5, int a6, int a7);   /* patrol.c: UnitOrders 0x41b990 wrap */
typedef void (__fastcall *UnitOrderFn)(BYTE *unit, int order, DWORD goal, BYTE *target, int a3, int a4, int a5, int a6, int a7);
extern UnitOrderFn patrol_order_original;                                 /* patrol.c: the original UnitOrders (trampoline) */
int aware_unit_noticed(BYTE *unit);                                    /* aware.c: a trooper of the unit fully noticed Sarge */
float squad_sprint_speed(BYTE *anim);                                 /* squad.c (controls.c controls_move): speed factor of this ANIM, 0 = none */
void audio_find_wave_stub(void);                                      /* audio.c: call 0x40f42d (FUN_004107b0) - Army Men II sound swap */
size_t __cdecl audio_fread(void *buf, size_t size, size_t count, void *file);   /* audio.c: call 0x40f515 (the effect data read) */
int __fastcall audio_source_volume(BYTE *src);                           /* audio.c: FUN_004031e0 rewrite (stale-safe) */
void music_stub_create(void), music_stub_play(void), music_stub_fill(void), music_stub_refill(void), music_stub_rewind(void);   /* music.c: FUN_004107b0 calls of the stream code */
void music_frame(void);                     /* music.c: dd_present, game thread - restarts the music when [audio] am2_music is switched */
void music_option_switched(void);           /* music.c: panel.c click_row - the same at once (game thread only) */
int controls_sarge_running(void);                                     /* controls.c: Sarge sprints (modern controls) */
void squad_debug_follow(void);                                        /* squad.c: testing aid, every player unit follows Sarge */
/* mines.c (2026-11-21): mine placement with the mouse - a ring round Sarge, left click inside it lays the mine there */
enum { MINE_OK, MINE_OUT, MINE_BLOCKED, MINE_NO_WAY, MINE_CLOSE, MINE_EMPTY, MINE_FAR };   /* MINE_FAR: beyond the ring, a tap walks there + lays */
int mines_pickup_hover(int *x, int *y, int *ok);   /* modern.c: Sarge's own mine under the pointer (no sweeper): a click picks it up */
   /* mines_preview state */
typedef struct { int sarge_x, sarge_y, reach, x, y, state, left, busy; } MinePreview;   /* world px; x, y = tile middle */
int mines_preview(MinePreview *p);          /* modern.c: mines selected (modern controls, on foot): ring + marker */
int mines_pointer_in_reach(void);           /* controls.c: the pointer is inside the ring (mine pointer, no walk ring) */
int mines_click(int wx, int wy, DWORD now, int *walk_x, int *walk_y);   /* controls.c: a left tap at the world point; 0 = normal, 1 = taken, 2 = walk to *walk_x, *walk_y (a mine to disarm) */
#define MF_FACE      1                      /* mines_frame: laying - face *face_x, *face_y */
#define MF_STOP_WALK 2                      /* mines_frame: stop the walk (laying, or the mine to disarm reached) */
int mines_frame(DWORD now, int walking, int *face_x, int *face_y);   /* controls.c after the on-foot controls, every frame */
enum { SWEEP_COLLECT, SWEEP_FULL, SWEEP_NO_PACK, SWEEP_NO_ROOM };   /* what disarming a mine would do */
typedef struct { int sarge_x, sarge_y, radius, selected, count, nearest, plate_x, plate_y, walking_to, hover, hover_x, hover_y, hover_plan, hover_own; } SweepInfo;
typedef struct { short x, y; char own, in_range, safe; } SweepMark;   /* safe: deactivated (revealed by the detector) */
int mines_sweep_info(SweepInfo *si);       /* modern.c: the sweeper is carried (detector state, the mine under the pointer) */
int mines_sweep_marks(SweepMark *m, int max, int x0, int y0, int x1, int y1);   /* modern.c: visible laid mines in a world box */
int mines_sweep_hud(int *count, int *nearest);   /* weapons.c: the sweeper box lamp */
int mines_collect_in_range(void);                /* controls.c: selected sweeper + right click collects every mine in its detector radius */
int mines_free_charges(BYTE *weapon);       /* weapons.c: real free charges of a mine weapon (ammo cap for the top-up) */
float mines_sweeper_walk_mul(BYTE *sarge, int speed);   /* controls.c controls_move: speed factor for Sarge walking with the sweeper selected (1 = none) */
void __fastcall mines_step(BYTE *mine, DWORD *who);    /* mines.c: FUN_004398d0 rewrite (something moved onto a laid mine) */
int mines_transfer(BYTE *to, BYTE *from, int n);   /* weapons.c: move free charges of a pack into used-up slots */
int mines_set_charges(BYTE *w, const short *objs, int n);   /* weapons.c: carry-over - the charge array recorded at the win */
BYTE *weapons_carried(int type, void **item); /* weapons.c: record of a carried weapon of this type (any slot / box) */
int weapons_room_for_special(void);         /* weapons.c: a special weapon picked up now would be taken */
void __fastcall mines_lay(BYTE *weapon, int army, DWORD point);   /* call 0x445f44 -> MineLay 0x446600 */
/* allies.c (2026-11-21): the player's soldiers - regeneration, more health, medkits used on them (AM2 style) */
void allies_frame(void);                    /* gameplay.c gameplay_frame: regeneration, mission change */
void drops_frame(void);                     /* drops.c: enemy soldiers drop their weapon where they die (gameplay_frame) */
void drops_install(void);                   /* drops.c: spare weapon pickups made at map load */
int drops_hover_label(const char **name, int *wx, int *wy);   /* drops.c: kind of the enemy soldier under the fire pointer */
unsigned gameplay_run(int *scenario, int *mission);   /* gameplay.c: changes on every mission (re)start (also Play Again / same mission again) */
void __fastcall allies_trooper_join(BYTE *trooper, BYTE *unit);   /* call 0x43b4e5 -> FUN_0043b5f0 (trooper joins his unit) */
int __fastcall allies_pickup_test(BYTE *item, BYTE *sarge);       /* call 0x439855 -> FUN_00439700 (may Sarge pick this up) */
int allies_kits(void);                      /* medkits carried (weapons.c HUD medkit box) */
int allies_select_heal(int on, const char *why);   /* weapons.c: the medkit box chosen (1) / put away (0); 1 = selected now */
void allies_debug_kit(int hurt);             /* controls.c test aids (cheat_keys): Ctrl+L a kit, Ctrl+Shift+L hurt the nearest soldier */
int present_game_to_screen(int *x, int *y);   /* present.c: view game pixels -> desktop (rig_coords; test aids) */
int allies_heal_mode(void);                 /* the medkit is selected */
void allies_toggle_heal(void);              /* controls.c: the heal key (ACT_HEAL) */
int allies_cancel(const char *why);         /* put the medkit away (right click, weapon chosen ...); 1 = it was selected */
int allies_message(HWND h, UINT msg, WPARAM wp, LPARAM lp);   /* window hook: Esc puts the selected medkit away */
int allies_take_esc(int down);               /* controls.c kb_get_state: Esc down now; 1 = it put the medkit away - hide it from the game */
enum { HEAL_OK, HEAL_FULL, HEAL_NO_KITS, HEAL_NOBODY, HEAL_NO_WAY };
int allies_hover(int wx, int wy, int unreachable);   /* controls.c / modern.c: what a heal click at the world point would do (HEAL_*) */
int allies_click(int wx, int wy, DWORD now, int *walk_x, int *walk_y);   /* controls.c: a left tap; 0 = normal, 1 = taken, 2 = walk to *walk_x, *walk_y first */
int allies_heal_self(void);                 /* controls.c: a click on Sarge in heal mode; 1 = a kit used (no stance change) */
int allies_walk_frame(DWORD now, int walking);   /* controls.c after the on-foot controls: 1 = stop the walk (soldier reached and healed) */
typedef struct { int on, kits, state, pct, x, y, hp, max; } HealInfo;   /* x, y: world point under the pointer */
int allies_heal_info(HealInfo *hi);         /* modern.c: heal mode on: the label at the pointer */
typedef void (*SoldierFn)(BYTE *trooper, int cx, int cy, int top, int is_vehicle);
void allies_each(SoldierFn fn);                /* modern.c: every living soldier of the player's army on the map (health bars) */
/* savestate.c (2026-11-21): quicksave / quickload of the running mission (memory snapshot) */
void savestate_install(void);               /* patches.c, first: the exe's CRT heap -> our arena, the idle-loop safe point */
int savestate_message(HWND h, UINT msg, WPARAM wp, LPARAM lp);   /* window hook: the save / load / slot keys */
const char *savestate_banner(void);         /* modern.c: the confirmation text, or NULL */
void savestate_region(const char *name, void *p, size_t n);      /* mod state saved with a quicksave, put back on a load */
void savestate_note_patch(uintptr_t va, size_t n);   /* patches.c write_mem: patched exe data keeps the value of this process on a load */
void missionpath_request_scenario(int scenario);    /* missionpath.c: start that scenario's first mission at the campaign screen */
int missionpath_screen_state(void);                  /* missionpath.c: the menu screen state (2 = campaign select), -1 none */
void hdworld_surface_replaced(Surf *s);              /* hdworld.c: a load put other pixels into s (its references are stale) */
#define SAVESTATE(v) savestate_region(#v, &(v), sizeof(v))
void weapons_savestate(void); void allies_savestate(void); void aware_savestate(void); void capture_savestate(void);
void weapons_army_saved(void); void weapons_debug_dump(const char *why); void weapons_debug_pickup(int charges_only); void weapons_debug_die(void);
void patrol_savestate(void); void mines_savestate(void); void bullets_savestate(void); void objectives_savestate(void);
void vehicles_savestate(void); void modern_savestate(void); void fences_savestate(void); void missionpath_savestate(void);
void controls_savestate(void);
void clock_restore(DWORD tick);             /* clock.c: the game clock (our GetTickCount) set to tick */
void gameplay_run_resync(void);             /* gameplay.c: after a load - the game clock went back, NOT a new mission run */
extern unsigned surf_count_made, surf_count_gone;   /* surface.c: surfaces created / destroyed (savestate.c log) */
typedef struct { Surf *s; LONG ref; } SurfRef;
int surf_snapshot(SurfRef *out, int max);   /* surface.c: surfaces alive + reference counts (savestate.c) */
int surf_restore_refs(const SurfRef *list, int n);   /* surface.c: a load: counts of the save back, zombies alive again */
void surf_keep_released(int on);            /* surface.c: released surfaces kept as zombies while a quicksave exists */
int surf_purge(int (*needed)(Surf *s));     /* surface.c: destroy zombies no quicksave needs */
int surf_count_not_in(const SurfRef *list, int n);   /* surface.c: alive surfaces made after that save */
Surf *surf_new(DDraw *dd, int w, int h, DWORD caps);   /* surface.c: a surface for a load from a save file */
int surf_list_all(Surf **out, int max);       /* surface.c: every surface held (incl. zombies) */

/* record.c: automatic manual-test session recorder & semantic replayer */
void record_init(void);
void record_shutdown(void);
int  record_active(void);
void record_frame(void);
void record_screen_note(void);
void record_note_mission_won(void);
void record_message(HWND h, UINT msg, WPARAM wp, LPARAM lp);
void record_note_walk_target(int tx, int ty, int sprint, DWORD veh_uid);
void record_note_weapon_pickup(int type, int ammo_before, int ammo_after, int box, int wx, int wy, DWORD uid, int merge);
void record_note_weapon_select(int type, int ammo, int slot, int box);
void record_note_weapon_drop(int type, int wx, int wy);
void record_note_weapon_fire(int type, int ammo, int wx, int wy, int facing, int tx, int ty, DWORD target_uid);
void record_note_mine_lay(int type, int wx, int wy, int ammo);
void record_note_cheat_kill(DWORD uid);
void record_note_vehicle(const char *action, DWORD uid, int army, int cls, int wx, int wy, int hp);
void record_note_order(const char *mode, int wx, int wy, DWORD unit_uid, const char *via);
void record_note_medkit(const char *action, int wx, int wy, int kits, int hp_before, int hp_after);
void record_note_setting(const char *section, const char *key, const char *val);
void record_note_cheat(const char *name, int wx, int wy, const char *detail);
void record_note_button_click(void *btn);
void record_note_savestate(const char *action, int slot, int ok);
void record_note_exception(DWORD code, void *at, const char *module, DWORD offset);
void controls_replay_walk(int wx, int wy, int sprint);
void controls_replay_fire(int tx, int ty);
int  weapons_select_type(int type);

/* update.c: built-in GitHub Releases updater */
int  layout_menu_scale_value(void);         /* layout.c: in-game menu scale (64ths) even before SCRN_FULL is published */
void update_cleanup(void);
void update_check_async(int manual);
int  update_message(HWND h, UINT msg, WPARAM wp, LPARAM lp);
const char *update_version(void);
