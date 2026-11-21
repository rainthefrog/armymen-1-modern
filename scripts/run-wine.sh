#!/bin/bash
# Run Army Men under Wine in a virtual X display and capture screenshots.
# Usage: run-wine.sh <exe-to-test> <out-dir> [seconds-before-first-shot] [shots] [interval] [-- extra game args]
# The game folder is mounted read-only at /game; it is copied to a scratch dir so
# the game can write configs/saves without touching the original.
set -u
EXE="$1"; OUT="$2"; WAIT="${3:-25}"; SHOTS="${4:-4}"; EVERY="${5:-5}"
shift 5 2>/dev/null || shift $#
[ "${1:-}" = "--" ] && shift
ARGS=("$@")

mkdir -p "$OUT"
GAMEDIR=/tmp/am
rm -rf "$GAMEDIR"; cp -r /game "$GAMEDIR"
cp "$EXE" "$GAMEDIR/Armymen.exe"
# EXTRA="a.dll b.ini": extra files dropped into the game folder (e.g. our ddraw.dll). Use with
# WINEDLLOVERRIDES="ddraw=n,b" so Wine loads ours instead of its builtin.
for f in ${EXTRA:-}; do cp "$f" "$GAMEDIR/"; done
# EXTRADIR="dir ...": folders copied into the game folder.
for d in ${EXTRADIR:-}; do cp -r "$d" "$GAMEDIR/"; done

export DISPLAY=:99
DESKTOP="${DESKTOP:-1920x1080}"   # env DESKTOP=WxH: size of the virtual screen
Xvfb :99 -screen 0 ${DESKTOP}x24 -nolisten tcp >/dev/null 2>&1 &
XPID=$!
sleep 2

if [ ! -f "$WINEPREFIX/system.reg" ]; then
  wineboot -i >/dev/null 2>&1
  wineserver -w
fi

# env DPI=144|192: pretend Windows display scaling is 150%|200% (LogPixels), reset afterwards
if [ -n "${DPI:-}" ]; then wine reg add "HKCU\Control Panel\Desktop" /v LogPixels /t REG_DWORD /d "$DPI" /f >/dev/null 2>&1; fi
cd "$GAMEDIR"
# Virtual desktop lets the game "change display mode" like Proton/gamescope does.
WINEDEBUG="${WINEDEBUG_GAME:--all}" wine explorer /desktop=AM,${DESKTOP} Armymen.exe "${ARGS[@]}" > "$OUT/wine.log" 2>&1 &
GPID=$!

sleep "$WAIT"
# Optional scripted input: CLICKS="x,y;x,y;key:Return;..." – one action per step, screenshot after each
if [ -n "${CLICKS:-}" ]; then
  n=0
  IFS=';' read -ra STEPS <<< "$CLICKS"
  for s in "${STEPS[@]}"; do
    n=$((n+1))
    import -window root "$OUT/step_$(printf %02d $n)_before.png" 2>/dev/null
    case "$s" in
      key:*) xdotool key "${s#key:}" ;;
      press:*) xdotool keydown "${s#press:}"; sleep 0.15; xdotool keyup "${s#press:}" ;;   # held long enough for per-frame polling
      keydown:*) xdotool keydown "${s#keydown:}" ;;   # keep a key held (screenshots are taken before each step) ...
      keyup:*) xdotool keyup "${s#keyup:}" ;;         # ... until keyup
      wait:*) sleep "${s#wait:}" ;;
      move:*) m="${s#move:}"; xdotool mousemove "${m%,*}" "${m#*,}" ;;
      rel:*) m="${s#rel:}"; xdotool mousemove_relative -- "${m%,*}" "${m#*,}" ;;
      click) xdotool click 1 ;;
      dblclick) xdotool click --repeat 2 --delay 120 1 ;;                          # double-click left (sprint to a spot)
      rclick) xdotool click 3 ;;                                                    # right click (tap)
      hold:*) h="${s#hold:}"; xdotool keydown "${h%:*}"; sleep "${h#*:}"; xdotool keyup "${h%:*}" ;;   # hold:KEY:seconds
      lhold:*) xdotool mousedown 1; sleep "${s#lhold:}"; xdotool mouseup 1 ;;      # hold left button N seconds
      rhold:*) xdotool mousedown 3; sleep "${s#rhold:}"; xdotool mouseup 3 ;;      # hold right button N seconds
      rdblhold:*) xdotool click 3; sleep 0.12; xdotool mousedown 3; sleep "${s#rdblhold:}"; xdotool mouseup 3 ;;   # double-click right, keep the 2nd held N seconds (run)
      wheel:*) [ "${s#wheel:}" = up ] && xdotool click 4 || xdotool click 5 ;;   # one mouse wheel notch: wheel:up / wheel:down
      rdbldown) xdotool click 3; sleep 0.12; xdotool mousedown 3 ;;   # double-click right and KEEP holding (until rup)
      rdown) xdotool mousedown 3 ;;   # press right and KEEP holding (until rup)
      rup) xdotool mouseup 3 ;;
      ldown) xdotool mousedown 1 ;;   # press left and KEEP holding (until lup; e.g. the stance drag on Sarge)
      lup) xdotool mouseup 1 ;;
      mdown) xdotool mousedown 2 ;;   # middle button down / up (radial)
      mup) xdotool mouseup 2 ;;
      mclick) xdotool click 2 ;;
      dtap:*) k="${s#dtap:}"; xdotool keydown "$k"; sleep 0.1; xdotool keyup "$k"; sleep 0.08; xdotool keydown "$k"; sleep 0.1; xdotool keyup "$k" ;;   # double-tap a key
      xclick:*) xdotool click "${s#xclick:}" ;;   # side buttons: xclick:8 = Mouse4 (X1), xclick:9 = Mouse5 (X2)
      drag:*) d=(${s#drag:}); d=(${d//,/ }); xdotool mousemove ${d[0]} ${d[1]}; sleep 0.3; xdotool mousedown 1; sleep 0.3
              for i in 1 2 3 4 5 6 7 8; do xdotool mousemove $(( d[0] + (d[2]-d[0])*i/8 )) $(( d[1] + (d[3]-d[1])*i/8 )); sleep 0.1; done
              xdotool mouseup 1 ;;   # drag:x1,y1,x2,y2 with the left button (e.g. a window corner)
      *) xdotool mousemove "${s%,*}" "${s#*,}"; sleep 0.3; xdotool click 1 ;;
    esac
    sleep "$EVERY"
  done
  SHOTS=1
  NOCLICK=1
fi
for i in $(seq 1 "$SHOTS"); do
  import -window root "$OUT/shot_$i.png" 2>/dev/null
  # click to advance through splash / menus between shots
  [ -z "${NOCLICK:-}" ] && xdotool mousemove 960 540 click 1 2>/dev/null
  sleep "$EVERY"
done

wineserver -k 2>/dev/null
if [ -n "${DPI:-}" ]; then wine reg delete "HKCU\Control Panel\Desktop" /v LogPixels /f >/dev/null 2>&1; wineserver -w; fi
kill $XPID 2>/dev/null
echo "done; game pid was $GPID"
cp "$GAMEDIR"/armymen-mod.log "$OUT/" 2>/dev/null
cp "$GAMEDIR"/armymen-mod.ini "$OUT/armymen-mod.after.ini" 2>/dev/null   # what the run wrote (bindings, switches)
ls -la "$OUT"
