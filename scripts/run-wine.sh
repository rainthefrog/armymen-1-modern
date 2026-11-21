#!/bin/bash
# Run Army Men under Wine in a virtual X display and capture screenshots.
# Usage: run-wine.sh <exe-to-test> <out-dir> [seconds-before-first-shot] [shots] [interval] [-- extra game args]
# env (see test/README.md): CLICKS steps, SHOTS=step|marked (screenshot before every step = default | only at
# `shot` / `shot:name` steps + the final shot), INI_SET="section.key=value ..." (edits the copied armymen-mod.ini),
# DESKTOP, DPI, EXTRA, EXTRADIR, NOCLICK, WINEDEBUG_GAME. Steps `g:x,y` / `gmove:x,y` take GAME pixels of what is
# on screen now (menu: 640x480 design coordinates; battlefield: frame pixels) - needs [debug] rig_coords=1.
# The game folder is mounted read-only at /game; it is copied to a scratch dir so
# the game can write configs/saves without touching the original.
set -u
SHOTMODE="${SHOTS:-step}"   # env SHOTS=step|marked (the positional "shots" below is the final shot count)
T0=$(date +%s)
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
# INI_SET="display.hd_world=0 clock.speed=2 ...": set keys in the copied armymen-mod.ini (the mounted ini stays the
# base). A key is replaced inside its section (the FIRST section of that name: GetPrivateProfileInt ignores a
# second one), added at the end of that section if missing; a missing section is appended.
ini_set() {   # ini_set <file> <section> <key> <value>
  awk -v sec="$2" -v key="$3" -v val="$4" '
    BEGIN { insec = 0; done = 0; seen = 0 }
    { l = tolower($0); sub(/[ \t\r]+$/, "", l) }
    /^\[/ { if (insec && !done) { print key "=" val; done = 1 }
            insec = (!seen && l == "[" tolower(sec) "]"); if (insec) seen = 1; print; next }
    insec && !done && l ~ "^" tolower(key) "[ \t]*=" { print key "=" val; done = 1; next }
    { print }
    END { if (!done) { if (!seen) print "[" sec "]"; print key "=" val } }' "$1" > "$1.tmp" && mv "$1.tmp" "$1"
}
if [ -n "${INI_SET:-}" ]; then
  [ -f "$GAMEDIR/armymen-mod.ini" ] || : > "$GAMEDIR/armymen-mod.ini"
  for kv in $INI_SET; do
    k="${kv%%=*}"; ini_set "$GAMEDIR/armymen-mod.ini" "${k%%.*}" "${k#*.}" "${kv#*=}"
  done
  cp "$GAMEDIR/armymen-mod.ini" "$OUT/armymen-mod.used.ini"
fi

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
# g:x,y -> desktop x y: game pixels of the view shown NOW (last "rig: view L,T,R,B screen X,Y WxH" line the dll logs
# with [debug] rig_coords=1): menus = the 640x480 design picture, battlefield = the frame (e.g. 1024x540).
# Works at any window size / scale / position (FAST=1 1x window, FAST=ui / full 2x window).
g2s() {
  awk -v p="$1" '/rig: view /{ l = $0 } END {
      if (l == "") { print "NOVIEW"; exit }
      sub(/.*rig: view /, "", l); gsub(/[,x]/, " ", l); sub(/screen /, "", l); split(l, v, " ")
      split(p, q, ","); sx = (v[7] + 0.0) / (v[3] - v[1]); sy = (v[8] + 0.0) / (v[4] - v[2])
      printf "%d %d\n", v[5] + (q[1] - v[1] + 0.5) * sx, v[6] + (q[2] - v[2] + 0.5) * sy }' "$GAMEDIR/armymen-mod.log" 2>/dev/null
}
gmove() { local c; c=$(g2s "$1"); if [ "$c" = NOVIEW ] || [ -z "$c" ]; then echo "g:$1: no 'rig: view' line in the log (needs [debug] rig_coords=1, log>=1)" >&2; return 1; fi; xdotool mousemove $c; }
# waitlog:REGEX[:seconds] waits until the mod log has a NEW line matching REGEX (default timeout 60 s)
LOGSEEN=0
waitlog() {
  local re="$1" to=60
  if [[ "$1" =~ ^(.*):([0-9]+)$ ]]; then re="${BASH_REMATCH[1]}"; to="${BASH_REMATCH[2]}"; fi
  local t=0; while [ $t -lt $((to*4)) ]; do
    if tail -n +$((LOGSEEN+1)) "$GAMEDIR/armymen-mod.log" 2>/dev/null | grep -qE "$re"; then LOGSEEN=$(wc -l < "$GAMEDIR/armymen-mod.log"); return 0; fi
    sleep 0.25; t=$((t+1)); done
  echo "waitlog: '$re' not seen in ${to}s" >&2; return 1
}
T1=$(date +%s)
# Optional scripted input: CLICKS="x,y;x,y;key:Return;..." – one action per step. SHOTS=step (default): a screenshot
# BEFORE every step; SHOTS=marked: only at `shot` / `shot:name` steps (+ the final shot).
if [ -n "${CLICKS:-}" ]; then
  n=0
  IFS=';' read -ra STEPS <<< "$CLICKS"
  for s in "${STEPS[@]}"; do
    n=$((n+1))
    [ "$SHOTMODE" = step ] && import -window root "$OUT/step_$(printf %02d $n)_before.png" 2>/dev/null
    case "$s" in
      shot) import -window root "$OUT/shot_$(printf %02d $n).png" 2>/dev/null; continue ;;     # screenshot now (no step pause)
      shot:*) import -window root "$OUT/shot_${s#shot:}.png" 2>/dev/null; continue ;;
      waitlog:*) waitlog "${s#waitlog:}"; continue ;;                                          # wait for a mod log line
      g:*) gmove "${s#g:}" && { sleep 0.3; xdotool click 1; } ;;                               # click at game pixels
      gmove:*) gmove "${s#gmove:}" ;;                                                          # pointer to game pixels
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

T2=$(date +%s)
echo "timing: setup+start wait $((T1-T0)) s, steps + final shots $((T2-T1)) s" | tee "$OUT/timing.txt"
wineserver -k 2>/dev/null
if [ -n "${DPI:-}" ]; then wine reg delete "HKCU\Control Panel\Desktop" /v LogPixels /f >/dev/null 2>&1; wineserver -w; fi
kill $XPID 2>/dev/null
echo "done; game pid was $GPID"
cp "$GAMEDIR"/armymen-mod.log "$OUT/" 2>/dev/null
cp "$GAMEDIR"/armymen-mod.ini "$OUT/armymen-mod.after.ini" 2>/dev/null   # what the run wrote (bindings, switches)
ls -la "$OUT"
