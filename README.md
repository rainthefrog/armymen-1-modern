# armymen-mod (ddraw.dll)

Drop-in `ddraw.dll` for the GOG release of Army Men (1998). Put `ddraw.dll`, `Armymen.exe.manifest` and
`armymen-mod.ini` next to the **unmodified** `Armymen.exe`. Settings and their meaning are in
`armymen-mod.ini`; what happened at start-up is written to `armymen-mod.log`.

On Proton/Bazzite add `WINEDLLOVERRIDES="ddraw=n,b" %command%` to the Steam launch options.

## Build

```
docker run --rm -v "<repo>:/mod" -w /mod/src armymen-re:2 make
```

The build image: `docker build -t armymen-re:2 docker/` (Debian, mingw-w64 i686, Wine, Ghidra for the decomp).

## Layout

| Folder | What |
|---|---|
| `src/` | the mod's C sources, `Makefile`, `armymen-mod.ini` (defaults), `Armymen.exe.manifest` |
| `scripts/` | `run-wine.sh` (the Wine test runner, inside the container), `deploy-test.ps1` (copy a build to a Windows test folder), `clear-compat-markers.ps1` (user-run, registry) |
| `docker/` | `Dockerfile` of the build / test image `armymen-re:2` |
| `docs/` | research notes: menus, targeting, mission targets, UI inventory, menu renderer, modernization plan |
| `test/` | `run.sh` (Desert 1 in Wine with an ini), `run-menu.sh` (menu screens), test inis; output goes to `test/out/<name>` (ignored by git) |

## How it works

The game only ever talks to DirectDraw, and Windows always loads `ddraw.dll` from the game
folder first. So this DLL is two things:

1. **A DirectDraw replacement** (the game keeps drawing 8-bit pictures; we show them with
   Direct3D 11). Nothing asks Windows for 256-colour display modes.
2. **A mod loader.** When the DLL loads (before the game's own code runs) it patches the game in
   memory: small byte fixes, and *rewritten game functions*: a game function is decompiled,
   rewritten as readable C here, and the original is redirected to it with a `jmp`.

## Files

| File | What it is |
|---|---|
| `common.h` | Shared types (`DDraw`, `Surf`, `Pal`, `Config`) and every cross-file function. |
| `ddraw.c` | DLL entry point, ini loading, log, `IDirectDraw`, palette and clipper objects, the game-window hook, and the window/game coordinate mapping. |
| `surface.c` | `IDirectDrawSurface`: 8-bit surfaces, the software blitter, and the translation of windowed-mode screen coordinates into game pixels (`screen_to_game`). |
| `present.c` | Display thread: once per monitor refresh it converts the view to 32-bit colour, scaled to the window (any size; `[display] smoothing` off / light / full), and shows it with Direct3D 11. |
| `menu.c` | The **Gameplay** menu in the game window's menu bar: one toggle, Modern controls (switches the controls patches and hooks at once, saved to the ini). |
| `modmenu.c` | **Mod Settings** inside the game's own menus (pause menu, main menu, F10): extra buttons in the game's button lists, screen state 14 taken over, screen built from copies of the game's Audio Options definitions (see `armymen-1-modern/docs/game-menus.md`). |
| `menutext.c` | **Menu labels** of the full-screen menus drawn by the mod at window resolution (bigger, antialiased, the game's state colours) instead of into the 640x480 picture; layer drawn by gpu.c `draw_overlay` (menu renderer step 1, `armymen-1-modern/docs/menu-render-evaluation.md`). |
| `gpu.c` | Scaling on the graphics card (Direct3D 11 shaders compiled at start-up with d3dcompiler_47): square pixels for the game, smooth for videos, blurred fill around pictures that do not fill the window (menus). present.c falls back to CPU scaling if shaders are unavailable. |
| `gameplay.c` | Game rule changes: health regeneration (delay after the last hit, then slow healing on the game clock); Gameplay menu toggle. |
| `weapons.c` | Six weapons: a spare per slot (wrap of the pickup `FUN_004495c0`), mouse-wheel weapon change through slots and spares, spare moves up after a drop; the spare-weapon column drawn in a gap of the split HUD panel (layout.c `layout_hud_dx`), clickable. |
| `view.c` | Which part of the screen is shown (whole frame in gameplay, the 640x480 picture in menus, the video) and where it goes in the window; also the window/game mouse coordinate conversion. |
| `patches.c` | **The list of everything changed in the game**: byte patches, `hook_jmp` (redirect a game function to C) and `iat_hook` (replace an imported Windows function for the exe only). Start here. |
| `window.c` | Rewrite of `FUN_00407ae0` (game window rectangle), the window size correction, resizing (any size) and borderless fullscreen (maximise / Alt+Enter / F11). |
| `mouse.c` | Rewrites of `FUN_0041f380` (cursor movement), `FUN_0041f330` / `FUN_0041f480` (hand the mouse to/from Windows). Pointer mode (default): the game cursor is the Windows pointer, 1:1 and not locked; shared DirectInput mouse, duplicate mouse messages filtered, pointer hidden only over the game picture. |
| `Armymen.exe.manifest` | Ships next to the exe: tells Windows the game is Windows 10 aware and DPI aware, so it stops applying automatic compatibility fixes (reduced colour mode etc.). |
| `layout.c` | **The 640x480 interface layout on any screen.** Anchors (HUD = bottom centre, everything else = centre), offsets, clickable panels (pointer -> design position), the 640x480-based limits it patches, and the resolution presets. Start here for anything about positions. |
| `hud.c` | Every interface drawing routine of the game, made resolution independent through layout.c: pictures (`FUN_00412f70`, `FUN_00413050`), text (`FUN_0044b760`, `FUN_00413a30`), minimap box/dots (`FUN_00405980`, `FUN_004057b0`), cursor (`FUN_004206a0`); HUD panel at its original width with the battlefield showing beside it (layout.c). |
| `minimap.c` | **Minimap**: bigger outlined blips in army colours, Sarge marked, map picture contrast, tooltip with the details of the blip under the pointer (army, kind, men, health, distance). Works from the game's pin table (curstratmap 0x691960, copied in the `FUN_00405980` wrap) and layout.c `layout_hud_point` (same mapping as the enlarged minimap). `[hud] minimap_*`. |
| `controls.c` | Modern controls (`[controls] mode=modern`): mouse aim, left = fire, hold right = walk toward the pointer, tap right = walk to the spot, double-click right and hold = run (`FUN_00414ad0` speed), W A S D = screen directions, smooth camera (`FUN_0040dcd0`). Wraps the keyboard dispatcher `FUN_0041fca0`, sets bits in the player input word `0x6a547c` and Sarge's facing; hides W A S D from the game's own key bindings. |
| `fonts.c` | Font substitution: the game asks for "ArialNarrow"; uses Arial Narrow if installed, otherwise Arial narrowed to its width. |
| `clock.c` | The game clock: replaces the exe's `GetTickCount`, adjustable speed, pause, hotkeys. |
| `missionpath.c` | **Mission path** (`[hud] mission_path`): once per mission, a route from Sarge's start through the mission's waypoints in order (WAYS table from the mission scripts), built with pathfind.c over a few frames (driven legs: the vehicle hybrid planner, resumable; fences / bushes are driven over); drawn on the ground by modern.c. No game patch. Testing aid `[debug] skip_to_mission=N` (wins earlier campaign missions with the game's Triumph cheat handler). |
| `vehicles.c` | **The vehicle Sarge drives** (2026-11-20): click-to-drive routes with reversing / multi-point turns (`[controls] vehicle_reverse`, hybrid A* in pathfind.c), a steady turret (`turret_smooth`: the hull no longer drags it, eased aim), getting unstuck (`vehicle_unstick`: no creeping into sandbags / rocks, moves that lead out of an overlap are let through). Wraps the calls to VehicleUpdate (0x419c52) and VehicleLockTurret (0x417ce8). |
| `barricades.c` | **Sarge never stays stuck on a barricade** (2026-11-20, `[controls] sarge_unstick`): inside a barricade's mask -> moved out to the nearest roomy point; walking refused although the way ahead is free (mask holes) -> moved along it; never through a barricade (segment test with the game's own Sarge rule). Per frame from gameplay_frame, no game patch. Testing aid `[debug] barricade_test` / `barricade_crush` (cheat_keys). The no-squash rules for soldiers at barricades are in `gameplay.c` (`fix_squash`). |
| `fences.c` | **Sarge breaks wooden fences** (2026-11-20, `[gameplay] break_fences`, F10 "Sarge breaks fences"): cells blocked only by breakable FENCE objects (category 5, strength > 0, no script) are passable at a cost for the click route, the reachability test, the walkable regions (pathfind.c `path_cell_class`) and the mission path (missionpath.c); Sarge breaks the piece with the game's ItemDamage (DMGTYPE_RUNOVER, strength + armor + 1) when his click route reaches it, or after pushing against it ~350 ms. Never walls, sandbags, gates, bushes (indestructible in the game), buildings. No crush of Sarge / soldiers by the changing fence (gameplay.c crush_allowed). Testing aid `[debug] fence_route=tx,ty[,delay_ms[,sx,sy[,walk_ms,run,follow]]]` (cheat_keys; run/follow: squad.c test); log=2 lists the map's fences. |
| `capture.c` | **Enemy vehicles and bail-out** (2026-11-20): Sarge drives empty enemy vehicles (`[gameplay] drive_enemy_vehicles`: UID moved to a spare player legion slot 49, which then always holds a blank stand-in so stale UIDs find nothing instead of crashing VehicleFind); a destroyed vehicle throws Sarge out alive (`[gameplay] vehicle_bailout`: after the game's own SargeOutVehicle from VehicleDie, a little hurt + short invulnerability). Wraps SargeInVehicle, SargeOutVehicle, UnitEnterVehicle; rewrites the Enter Vehicle key. |
| `squad.c` | **The squad sprints with Sarge** (2026-11-20, `[gameplay] squad_sprint`, F10 "Squad sprints with Sarge"): troopers of the player's units with the FOLLOW order on Sarge (UNIT +0x74 = 3, +0x90 = Sarge) get the same run_speed step as Sarge in the AnimMove wrap (controls.c controls_move) while he sprints and moves, or while they are more than 120 px from him; every pixel of the longer step checked with the NPC tile rule, else the normal step. Enemy / allied AI unchanged. |

## Conventions for rewritten game functions

- One file per game subsystem (`window.c`, `mouse.c`, ...). The file starts with a header
  comment that lists, for each rewritten function: the original address, who calls it and its
  calling convention, what the original did (kept), and what we changed.
- Game memory is never used as a bare number in code. Give it a named `#define` (or a struct
  field for objects, see `GameWindow` in `window.c`) with a comment saying what it holds.
- Each rewrite is registered in `patches.c` with the first original bytes, so it refuses to
  install on an unexpected exe (and says so in the log).
- Calling conventions: `__fastcall` = arguments in ecx/edx; `__attribute__((thiscall))` = object
  in ecx, rest on the stack; `__cdecl` for functions with no arguments. Check the caller in the
  disassembly before choosing.

## Adding a rewrite

1. Find the function in `tooling/work/decomp/functions/0x<address>.c` (Ghidra pseudocode) and
   check the real instructions with `objdump -d -M intel --start-address=...` in the Docker image.
2. Write the C version in the right subsystem file, with the header comment.
3. Add a `hook_jmp(...)` line in `patches_apply()` with the function's first bytes.
4. Build, test in Wine (`armymen-1-modern/scripts/run-wine.sh`, see the repo's CLAUDE.md), check the log.

## Useful log levels (`[debug] log=`)

- `1` start-up, patches, display mode, window size
- `2` also mouse movement
- `3` also the first 40 blits onto the screen and which game code made them
- `4` also wide pictures (>= 400 px) drawn into the game picture; `5` also everything drawn in the HUD area
