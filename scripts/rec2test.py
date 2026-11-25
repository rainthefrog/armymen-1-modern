#!/usr/bin/env python3
"""
rec2test.py - Inspect recorded Army Men Modern sessions (.jsonl) and convert them
into repeatable automated tests for test/run.sh.

Examples:
  # Summarize a recorded run (missions, moves, pickups, shots, kills, UI clicks, bookmarks)
  python scripts/rec2test.py --info "<game-folder>/recordings/run_*.jsonl"

  # Convert mission 1 of a recording into a semantic replay test under test/cases/desert1_smoke/
  python scripts/rec2test.py recordings/run_20261124_160000.jsonl --out desert1_smoke --mission 1

  # Convert UI/menu clicks (e.g. around a bookmark or full session) into a CLICKS test
  python scripts/rec2test.py recordings/run_20261124_160000.jsonl --out ui_flow --mode clicks
"""
import argparse
import glob
import json
import math
import os
import sys
from collections import Counter
from pathlib import Path


def load_events(path: Path):
    events = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as exc:
                print(f"warning: skipping malformed line {lineno} in {path}: {exc}", file=sys.stderr)
    return events


def summarize(path: Path, events):
    counts = Counter(ev.get("e", "?") for ev in events)
    sess = next((ev for ev in events if ev.get("e") == "session_start"), {})
    missions = [ev for ev in events if ev.get("e") == "mission_start"]
    bookmarks = [ev for ev in events if ev.get("e") == "bookmark"]
    exceptions = [ev for ev in events if ev.get("e") == "exception"]
    dur_ms = events[-1].get("t", 0) if events else 0

    print(f"=== {path} ===")
    print(f"  Build:     {sess.get('build', '?')} (portable={sess.get('portable', '?')})")
    print(f"  Duration:  {dur_ms / 1000.0:.1f} s  ({len(events)} events)")
    if missions:
        ms = ", ".join(f"run {m.get('run')} -> mission {m.get('mission')} (scen {m.get('scenario')}) @ {m.get('t',0)/1000:.1f}s" for m in missions)
        print(f"  Missions:  {ms}")
    else:
        print("  Missions:  none (menu-only session)")
    print("  Counts:    " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    if bookmarks:
        for b in bookmarks:
            print(f"  Bookmark #{b.get('n')}: t={b.get('t',0)/1000:.1f}s mission={b.get('mission')} pos=({b.get('wx')},{b.get('wy')}) hp={b.get('hp')}")
    if exceptions:
        for ex in exceptions:
            print(f"  EXCEPTION: code={ex.get('code')} at {ex.get('mod')}+{ex.get('off')} mission={ex.get('mission')} pos=({ex.get('wx')},{ex.get('wy')})")


def slice_events(events, mission=None, run_num=None, bookmark=None, window_s=15.0, from_ms=None, to_ms=None):
    out = events
    if mission is not None or run_num is not None:
        # Find the matching mission_start and take until next mission_start
        starts = [i for i, ev in enumerate(out) if ev.get("e") == "mission_start"]
        chosen = None
        for idx in starts:
            ev = out[idx]
            if run_num is not None and ev.get("run") == run_num:
                chosen = idx
                break
            if mission is not None and ev.get("mission") == mission:
                chosen = idx
                break
        if chosen is None:
            raise SystemExit(f"No matching mission_start found (mission={mission}, run={run_num})")
        next_starts = [i for i in starts if i > chosen]
        end = next_starts[0] if next_starts else len(out)
        # Keep session_start + chosen mission slice
        header = [ev for ev in out[:chosen] if ev.get("e") in ("session_start", "setting")]
        out = header + out[chosen:end]

    if bookmark is not None:
        bm = next((ev for ev in out if ev.get("e") == "bookmark" and ev.get("n") == bookmark), None)
        if not bm:
            raise SystemExit(f"Bookmark #{bookmark} not found in selected events")
        bt = bm.get("t", 0)
        lo = max(0, bt - int(window_s * 1000))
        hi = bt + int(3 * 1000)
        header = [ev for ev in out if ev.get("e") in ("session_start", "mission_start", "setting") and ev.get("t", 0) <= lo]
        body = [ev for ev in out if lo <= ev.get("t", 0) <= hi]
        out = header + body

    if from_ms is not None or to_ms is not None:
        lo = from_ms if from_ms is not None else 0
        hi = to_ms if to_ms is not None else 10**12
        header = [ev for ev in out if ev.get("e") in ("session_start", "mission_start") and ev.get("t", 0) < lo]
        out = header + [ev for ev in out if lo <= ev.get("t", 0) <= hi]

    return out


def thin_moves(events, min_step_px=40):
    """Collapse redundant move breadcrumbs while keeping every semantic action intact."""
    kept = []
    last_xy = None
    for ev in events:
        e = ev.get("e")
        if e == "move":
            xy = (ev.get("wx", 0), ev.get("wy", 0))
            if last_xy is not None:
                if math.hypot(xy[0] - last_xy[0], xy[1] - last_xy[1]) < min_step_px:
                    continue
            last_xy = xy
            kept.append(ev)
        else:
            if e in ("walk_target", "weapon_pickup", "weapon_fire", "enemy_kill", "vehicle_enter", "vehicle_capture", "vehicle_exit"):
                last_xy = (ev.get("wx", ev.get("tx", 0)), ev.get("wy", ev.get("ty", 0)))
            kept.append(ev)
    return kept


def build_ini_overrides(events):
    sess = next((ev for ev in events if ev.get("e") == "session_start"), {})
    cfg = sess.get("cfg", {})
    overrides = ["debug.record_runs=0"]
    mapping = [
        ("classic_gameplay", "preset.classic_gameplay"),
        ("modern_hud", "hud.modern_layout"),
        ("six_weapons", "controls.six_weapons"),
        ("health_regen", "gameplay.health_regen"),
        ("drive_enemy_vehicles", "gameplay.drive_enemy_vehicles"),
        ("vehicle_bailout", "gameplay.vehicle_bailout"),
        ("break_fences", "gameplay.break_fences"),
        ("enemy_awareness", "modern.enemy_awareness"),
        ("height_rules", "gameplay.height_rules"),
    ]
    for k, ini_k in mapping:
        if k in cfg:
            overrides.append(f"{ini_k}={cfg[k]}")
    if "modern" in cfg:
        overrides.append(f"controls.mode={'modern' if cfg['modern'] else 'classic'}")
    for ev in events:
        if ev.get("e") == "setting":
            overrides.append(f"{ev['section']}.{ev['key']}={ev['val']}")
    return overrides


def generate_replay_case(events, out_dir: Path, name: str, autokill=1, teleport=0, invuln=1):
    out_dir.mkdir(parents=True, exist_ok=True)
    case_jsonl = out_dir / f"{name}.jsonl"
    case_sh = out_dir / f"{name}.sh"

    replay_types = {
        "session_start", "mission_start", "move", "walk_target",
        "weapon_pickup", "weapon_select", "weapon_fire", "mine_lay",
        "enemy_kill", "vehicle_enter", "vehicle_capture", "vehicle_exit", "bookmark",
    }
    filtered = thin_moves([ev for ev in events if ev.get("e") in replay_types])

    with case_jsonl.open("w", encoding="utf-8") as f:
        for ev in filtered:
            f.write(json.dumps(ev, separators=(",", ":")) + "\n")

    ms = next((ev for ev in filtered if ev.get("e") == "mission_start"), None)
    mission = ms.get("mission", 1) if ms else 1
    overrides = build_ini_overrides(events)
    overrides.extend([
        f"debug.replay=Z:/mod/test/cases/{name}/{name}.jsonl",
        f"debug.replay_autokill={autokill}",
        f"debug.replay_teleport={teleport}",
        f"debug.replay_invuln={invuln}",
    ])
    ini_set = " ".join(overrides)

    sh = f"""#!/bin/bash
# Auto-generated semantic replay test from recorded manual session.
# Run from repo root: bash test/cases/{name}/{name}.sh
set -euo pipefail
OUT_NAME="case-{name}"
FAST=1 START={mission} \\
  INI_SET="{ini_set}" \\
  CLICKS="@start;waitlog:replay: (PASS|FAIL):120;shot:end" \\
  bash test/run.sh "$OUT_NAME"

LOG="test/out/$OUT_NAME/armymen-mod.log"
echo "--- Checking assertions in $LOG ---"
if grep -q "EXCEPTION" "$LOG"; then
  echo "FAIL: EXCEPTION detected in $LOG"
  grep -n "EXCEPTION" "$LOG" | head -n 5
  exit 1
fi
if grep -q "replay: FAIL" "$LOG"; then
  echo "FAIL: replay reported failure:"
  grep -n "replay: FAIL" "$LOG"
  exit 1
fi
if ! grep -q "replay: PASS" "$LOG"; then
  echo "FAIL: replay did not reach PASS"
  grep -n "replay:" "$LOG" | tail -n 10
  exit 1
fi
grep "replay: PASS" "$LOG"
echo "PASS: {name}"
"""
    case_sh.write_text(sh, encoding="utf-8", newline="\n")
    print(f"Wrote replay case:")
    print(f"  {case_jsonl} ({len(filtered)} events)")
    print(f"  {case_sh}")


def generate_clicks_case(events, out_dir: Path, name: str):
    out_dir.mkdir(parents=True, exist_ok=True)
    case_sh = out_dir / f"{name}_clicks.sh"

    ms = next((ev for ev in events if ev.get("e") == "mission_start"), None)
    mission = ms.get("mission") if ms else None

    steps = []
    if mission is not None:
        steps.append("@start")

    last_t = None
    in_mission = mission is None
    for ev in events:
        e = ev.get("e")
        if not in_mission:
            if e == "mission_start" and ev.get("mission") == mission:
                in_mission = True
                last_t = ev.get("t", 0)
            continue

        t = ev.get("t", 0)
        if last_t is not None:
            dt_s = (t - last_t) / 1000.0
            if dt_s >= 0.4:
                steps.append(f"wait:{min(dt_s, 10.0):.1f}")

        if e == "ui_click":
            btn = ev.get("btn", "L")
            gx, gy = ev.get("gx", 0), ev.get("gy", 0)
            if btn == "L":
                steps.append(f"g:{gx},{gy}")
            elif btn == "R":
                steps.append(f"gmove:{gx},{gy}")
                steps.append("rclick")
            elif btn == "M":
                steps.append(f"gmove:{gx},{gy}")
                steps.append("mclick")
            elif btn == "wheel":
                steps.append("wheel:up" if ev.get("delta", 1) > 0 else "wheel:down")
            last_t = t
        elif e == "key":
            kname = ev.get("name", "")
            if kname and not kname.startswith("vk_"):
                steps.append(f"press:{kname}")
                last_t = t
        elif e == "bookmark":
            steps.append(f"shot:bm_{ev.get('n', 1)}")
            last_t = t

    steps.append("shot:end")
    overrides = build_ini_overrides(events)
    ini_set = " ".join(overrides)
    start_env = f"START={mission} " if mission is not None else ""
    clicks_str = ";".join(steps)

    sh = f"""#!/bin/bash
# Auto-generated UI/input click test from recorded manual session.
# Run from repo root: bash test/cases/{name}/{name}_clicks.sh
set -euo pipefail
OUT_NAME="case-{name}-ui"
FAST=ui STEP_WAIT=0 {start_env}\\
  INI_SET="{ini_set}" \\
  CLICKS="{clicks_str}" \\
  bash test/run.sh "$OUT_NAME"

LOG="test/out/$OUT_NAME/armymen-mod.log"
if grep -q "EXCEPTION" "$LOG"; then
  echo "FAIL: EXCEPTION detected in $LOG"
  grep -n "EXCEPTION" "$LOG" | head -n 5
  exit 1
fi
echo "PASS: {name} (UI clicks)"
"""
    case_sh.write_text(sh, encoding="utf-8", newline="\n")
    print(f"Wrote UI clicks case:")
    print(f"  {case_sh} ({len(steps)} steps)")


def main():
    ap = argparse.ArgumentParser(description="Inspect and convert Army Men Modern .jsonl recordings into automated tests.")
    ap.add_argument("input", help="Path or glob to recorded .jsonl file(s)")
    ap.add_argument("--info", action="store_true", help="Print a summary of the recording(s) and exit")
    ap.add_argument("--out", help="Output test case name (written under test/cases/<name>/)")
    ap.add_argument("--mode", choices=("replay", "clicks", "both"), default="replay", help="Test style to generate (default: replay)")
    ap.add_argument("--mission", type=int, help="Extract only this mission ID")
    ap.add_argument("--run", type=int, help="Extract only this mission run number")
    ap.add_argument("--bookmark", type=int, help="Slice a window ending at bookmark #N")
    ap.add_argument("--window", type=float, default=15.0, help="Seconds before --bookmark to include (default: 15)")
    ap.add_argument("--from-ms", type=int, help="Start timestamp in ms")
    ap.add_argument("--to-ms", type=int, help="End timestamp in ms")
    ap.add_argument("--no-autokill", action="store_true", help="Disable replay_autokill in generated replay test")
    ap.add_argument("--teleport", action="store_true", help="Use replay_teleport=1 instead of walking in generated replay test")
    ap.add_argument("--no-invuln", action="store_true", help="Disable replay_invuln in generated replay test")
    args = ap.parse_args()

    matches = sorted(glob.glob(args.input))
    if not matches:
        raise SystemExit(f"No files matched: {args.input}")

    if args.info:
        for m in matches:
            p = Path(m)
            summarize(p, load_events(p))
        return

    if not args.out:
        raise SystemExit("Specify --info or --out <case-name>")

    src = Path(matches[-1])
    events = load_events(src)
    sliced = slice_events(events, mission=args.mission, run_num=args.run,
                          bookmark=args.bookmark, window_s=args.window,
                          from_ms=args.from_ms, to_ms=args.to_ms)

    repo_root = Path(__file__).resolve().parent.parent
    out_dir = repo_root / "test" / "cases" / args.out
    if args.mode in ("replay", "both"):
        generate_replay_case(sliced, out_dir, args.out,
                             autokill=0 if args.no_autokill else 1,
                             teleport=1 if args.teleport else 0,
                             invuln=0 if args.no_invuln else 1)
    if args.mode in ("clicks", "both"):
        generate_clicks_case(sliced, out_dir, args.out)


if __name__ == "__main__":
    main()
