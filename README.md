# Army Men Modern

Army Men Modern updates the original 1998 PC game for current systems while preserving its campaign, missions and
art style.

Highlights include true widescreen and ultrawide play, modern mouse/WASD controls, a scalable HUD, improved minimap,
mission-route guidance, expanded weapons and equipment, squad and vehicle improvements, enemy awareness, persistent
quicksaves, optional Army Men II audio and extensive stability fixes.

## Install

The release is one file: `ddraw.dll` (download from [Releases](../../releases)).

1. Use the GOG release of Army Men (1998).
2. Place `ddraw.dll` beside `Armymen.exe`.
3. Start the game normally.

The original executable is not modified. The DLL applies the mod in memory when the game starts. An INI, manifest
and HD-art pack are not required.

For Proton/Bazzite, add this launch option:

```text
WINEDLLOVERRIDES="ddraw=n,b" %command%
```

## Options

Use **Mod Settings** in the main/pause menu or the `F10` interface (`Shift+F10` for the compact overlay and binding
editor). Settings changed in game are saved to `armymen-mod.ini`; advanced users may also copy the documented example
[src/armymen-mod.ini](src/armymen-mod.ini) beside the DLL.

- **Classic gameplay** turns off gameplay, control and tactical additions while retaining the modern renderer.
- **Modern HUD layout** controls the sliced/scaled widescreen HUD. Turn it off for the original centered 1× HUD.
- **Logging** writes `armymen-mod.log` beside the DLL for tester reports. The previous run is retained as
  `armymen-mod.prev.log`.
- **Session recording** is off by default in release builds and can be turned on in `armymen-mod.ini` with
  `[debug] record_runs=1` (`Ctrl+Shift+B` drops a bookmark; see [scripts/rec2test.py](scripts/rec2test.py)).
- **Built-in updater** checks GitHub Releases on startup (can be toggled in **Gameplay > Check for updates at start**
  or `[update] check=0` in `armymen-mod.ini`), verifies `ddraw.dll.sha256`, swaps `ddraw.dll` in place, and restarts
  the game.

## More information

- [Full mod description](docs/MOD_DESCRIPTION.md)
- [Screenshot gallery](docs/SCREENSHOTS.md)
- [Release notes](docs/release-notes/v0.1.0.md)

## License

Code: [MIT](LICENSE).

Not affiliated with or endorsed by The 3DO Company, 2K, or GOG. Army Men is a trademark of its respective owner.
