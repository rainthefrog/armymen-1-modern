#!/usr/bin/env python3
"""
rec2test.py - Inspect recorded Army Men Modern sessions (.jsonl) and convert them
into repeatable automated tests for test/run.sh.

Examples:
  # Summarize a recorded run (shows every mission segment, outcome, and event counts)
  python scripts/rec2test.py --info "<game-folder>/recordings/run_*.jsonl"

  # Split a 3-mission play session into 3 separate automated test cases + a suite runner
  python scripts/rec2test.py recordings/run_20261124_160000.jsonl --split-missions --out desert_run

  # Extract a single mission segment by 1-based index (from --info) or mission ID
  python scripts/rec2test.py recordings/run_20261124_160000.jsonl --out desert2 --mission-idx 2
  python scripts/rec2test.py recordings/run_20261124_160000.jsonl --out desert1_smoke --mission 1

  # Convert UI/menu clicks into a CLICKS test
  python scripts/rec2test.py recordings/run_20261124_160000.jsonl --out ui_flow --mode clicks
"""
import argparse
import glob
import json
import math
import sys
from collections import Counter
from pathlib import Path


def mission_label(mid):
    if mid is None or mid < 0:
        return "unknown"
    if 1 <= mid <= 9:
        return f"Desert {mid}"
    if 10 <= mid <= 18:
        return f"Alpine {mid - 9}"
    if 19 <= mid <= 28:
        return f"Bayou {mid - 18}"
    if mid == 50:
        return "Boot Camp"
    return f"Mission {mid}"


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


def split_missions(events):
    """Split a full session into individual mission segments using mission_start / mission_end."""
    segments = []
    header = []
    cur = None
    seq = 0

    for ev in events:
        e = ev.get("e")
        if e == "session_start":
            header = [ev]
            continue
        if e == "setting":
            header.append(ev)
            if cur is not None:
                cur["events"].append(ev)
            continue

        if e == "mission_start":
            if cur is not None:
                segments.append(cur)
            seq += 1
            cur = {
                "idx": ev.get("idx", seq),
                "run": ev.get("run"),
                "scenario": ev.get("scenario"),
                "mission": ev.get("mission"),
                "start_t": ev.get("t", 0),
                "end_t": ev.get("t", 0),
                "reason": "eof",
                "won": False,
                "events": list(header) + [ev],
            }
            continue

        if cur is not None:
            cur["events"].append(ev)
            cur["end_t"] = ev.get("t", cur["end_t"])
            if e == "mission_won":
                cur["won"] = True
            elif e == "mission_end":
                cur["reason"] = ev.get("reason", "end")
                cur["won"] = bool(ev.get("won", cur["won"]))
                segments.append(cur)
                cur = None

    if cur is not None:
        segments.append(cur)
    return segments


def format_action(ev):
    e = ev.get("e", "")
    mod = ev.get("mod", 0)
    feat = ev.get("mod_feature")
    tag = f"[MOD-ONLY: {feat}]" if mod else "[VANILLA]"

    if e == "weapon_select":
        desc = f"weapon_select {ev.get('name')} (slot {ev.get('slot')}, ammo {ev.get('ammo')})"
    elif e == "weapon_fire":
        desc = f"weapon_fire {ev.get('name')} -> target ({ev.get('tx')},{ev.get('ty')})"
    elif e == "weapon_pickup":
        desc = f"weapon_pickup {ev.get('name')} (box {ev.get('box')}, ammo {ev.get('ammo_before')}->{ev.get('ammo_after')})"
    elif e == "weapon_drop":
        desc = f"weapon_drop {ev.get('name')}"
    elif e == "mine_lay":
        desc = f"mine_lay {ev.get('name')} (ammo left: {ev.get('ammo')})"
    elif e == "enemy_kill":
        desc = f"enemy_kill {ev.get('kind')} {ev.get('name')} at ({ev.get('wx')},{ev.get('wy')}) via {ev.get('via')}"
    elif e in ("vehicle_enter", "vehicle_capture", "vehicle_exit", "vehicle_bailout"):
        desc = f"{e} uid={ev.get('uid', '')} (army {ev.get('army', 0)})"
    elif e == "order":
        desc = f"order {ev.get('mode')} via {ev.get('via')}"
    elif e == "medkit":
        desc = f"medkit {ev.get('action')} (kits: {ev.get('kits')})"
    elif e == "cheat":
        desc = f"cheat {ev.get('name')} {ev.get('detail', '')}"
    elif e == "menu_click":
        desc = f"menu_click '{ev.get('item')}' (label: '{ev.get('label')}') on {ev.get('screen_name')}"
    else:
        desc = f"{e}"
    return tag, desc


def extract_stops_and_actions(events):
    stops = []
    cur_stop = None
    action_types = {
        "weapon_select", "weapon_fire", "weapon_pickup", "weapon_drop",
        "mine_lay", "enemy_kill", "vehicle_enter", "vehicle_capture",
        "vehicle_exit", "vehicle_bailout", "order", "medkit", "cheat",
    }
    for ev in events:
        e = ev.get("e")
        if e in ("sarge_stop", "stop"):
            cur_stop = {
                "idx": ev.get("stop_idx", len(stops) + 1),
                "wx": ev.get("wx", 0),
                "wy": ev.get("wy", 0),
                "facing": ev.get("facing", 0),
                "hp": ev.get("hp", 0),
                "veh": ev.get("veh"),
                "t": ev.get("t", 0),
                "actions": [],
            }
            stops.append(cur_stop)
        elif e in action_types:
            if cur_stop is None:
                cur_stop = {
                    "idx": 1,
                    "wx": ev.get("wx", ev.get("sx", 0)),
                    "wy": ev.get("wy", ev.get("sy", 0)),
                    "facing": ev.get("facing", 0),
                    "hp": 0,
                    "veh": None,
                    "t": ev.get("t", 0),
                    "actions": [],
                }
                stops.append(cur_stop)
            cur_stop["actions"].append(ev)
    return stops


def summarize(path: Path, events):
    counts = Counter(ev.get("e", "?") for ev in events)
    sess = next((ev for ev in events if ev.get("e") == "session_start"), {})
    segments = split_missions(events)
    bookmarks = [ev for ev in events if ev.get("e") == "bookmark"]
    exceptions = [ev for ev in events if ev.get("e") == "exception"]
    menu_clicks = [ev for ev in events if ev.get("e") == "menu_click"]
    dur_ms = events[-1].get("t", 0) if events else 0

    action_types = {
        "weapon_select", "weapon_fire", "weapon_pickup", "weapon_drop",
        "mine_lay", "enemy_kill", "vehicle_enter", "vehicle_capture",
        "vehicle_exit", "vehicle_bailout", "order", "medkit", "cheat",
    }

    print(f"=== {path} ===")
    print(f"  Build:     {sess.get('build', '?')} (portable={sess.get('portable', '?')})")
    print(f"  Duration:  {dur_ms / 1000.0:.1f} s  ({len(events)} events)")
    print("  Counts:    " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))

    if menu_clicks:
        print(f"  Menu Navigation ({len(menu_clicks)} item(s) clicked):")
        for i, mc in enumerate(menu_clicks, 1):
            tag = f"[MOD-ONLY: {mc.get('mod_feature')}]" if mc.get("mod") else "[VANILLA]"
            lbl = f" (label: \"{mc.get('label')}\")" if mc.get("label") else ""
            print(f"    {i}. [{mc.get('screen_name')}] -> {mc.get('item')}{lbl} {tag}")

    if segments:
        print(f"  Missions ({len(segments)}):")
        for seg in segments:
            sc = Counter(ev.get("e", "?") for ev in seg["events"])
            dur_s = max(0.0, (seg["end_t"] - seg["start_t"]) / 1000.0)
            outcome = "won" if seg["won"] else seg["reason"]
            n_stops = sum(1 for ev in seg["events"] if ev.get("e") in ("sarge_stop", "stop"))
            print(
                f"    #{seg['idx']}: mission {seg['mission']} ({mission_label(seg['mission'])}, run {seg['run']}) "
                f"[{seg['start_t']/1000:.1f}s..{seg['end_t']/1000:.1f}s, {dur_s:.1f}s, {outcome}] "
                f"stops={n_stops} moves={sc['move']+sc['walk_target']} pickups={sc['weapon_pickup']} fires={sc['weapon_fire']} kills={sc['enemy_kill']}"
            )

            stops = extract_stops_and_actions(seg["events"])
            if stops:
                print(f"      Stops & Actions ({len(stops)} stops):")
                for st in stops:
                    veh_s = f" [veh: {st['veh']}]" if st.get("veh") else ""
                    print(f"        Stop #{st['idx']} at ({st['wx']},{st['wy']}) facing={st['facing']}{veh_s}: {len(st['actions'])} action(s)")
                    for act in st["actions"]:
                        tag, desc = format_action(act)
                        print(f"          - {tag:<22} {desc}")

            all_actions = [ev for ev in seg["events"] if ev.get("e") in action_types]
            if all_actions:
                mod_acts = [ev for ev in all_actions if ev.get("mod") == 1]
                vanilla_acts = [ev for ev in all_actions if ev.get("mod", 0) == 0]
                v_pct = len(vanilla_acts) * 100 // len(all_actions)
                m_pct = 100 - v_pct
                feats = Counter(ev.get("mod_feature", "other") for ev in mod_acts)
                f_str = f" ({', '.join(f'{k}={v}' for k, v in feats.items())})" if feats else ""
                print(f"      Action breakdown: {len(all_actions)} total | {len(vanilla_acts)} vanilla ({v_pct}%) | {len(mod_acts)} mod-only ({m_pct}%){f_str}")
    else:
        print("  Missions:  none (menu-only session)")
    if bookmarks:
        for b in bookmarks:
            print(f"  Bookmark #{b.get('n')}: t={b.get('t',0)/1000:.1f}s mission={b.get('mission')} pos=({b.get('wx')},{b.get('wy')}) hp={b.get('hp')}")
    if exceptions:
        for ex in exceptions:
            print(f"  EXCEPTION: code={ex.get('code')} at {ex.get('mod')}+{ex.get('off')} mission={ex.get('mission')} pos=({ex.get('wx')},{ex.get('wy')})")


def slice_events(events, mission=None, mission_idx=None, run_num=None, bookmark=None, window_s=15.0, from_ms=None, to_ms=None):
    out = events
    if mission_idx is not None or mission is not None or run_num is not None:
        segments = split_missions(out)
        chosen = None
        for seg in segments:
            if mission_idx is not None and seg["idx"] == mission_idx:
                chosen = seg
                break
            if run_num is not None and seg["run"] == run_num:
                chosen = seg
                break
            if mission is not None and seg["mission"] == mission:
                chosen = seg
                break
        if chosen is None:
            raise SystemExit(f"No matching mission segment found (mission_idx={mission_idx}, mission={mission}, run={run_num})")
        out = chosen["events"]

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


def generate_replay_case(events, out_dir: Path, name: str, autokill=1, teleport=0, invuln=1, stops_only=False, strip_mod=False):
    out_dir.mkdir(parents=True, exist_ok=True)
    case_jsonl = out_dir / f"{name}.jsonl"
    case_sh = out_dir / f"{name}.sh"

    replay_types = {
        "session_start", "mission_start", "mission_won", "mission_end", "sarge_stop", "stop",
        "move", "walk_target", "weapon_pickup", "weapon_select", "weapon_fire", "mine_lay",
        "enemy_kill", "vehicle_enter", "vehicle_capture", "vehicle_exit", "bookmark",
    }
    filtered = [ev for ev in events if ev.get("e") in replay_types]

    if strip_mod:
        stripped = [ev for ev in filtered if ev.get("mod") == 1]
        filtered = [ev for ev in filtered if ev.get("mod", 0) == 0]
        if stripped:
            feat_cnt = Counter(ev.get("mod_feature", "unknown") for ev in stripped)
            print(f"  Stripped {len(stripped)} mod-only action(s) for vanilla compatibility: {', '.join(f'{k}={v}' for k, v in feat_cnt.items())}")

    if stops_only:
        filtered = [ev for ev in filtered if ev.get("e") not in ("move", "walk_target")]
        teleport = 1
    else:
        filtered = thin_moves(filtered)

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
# Auto-generated semantic replay test ({mission_label(mission)}, mission {mission}).
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
    stops_s = " [stops-only teleport]" if stops_only else ""
    print(f"Wrote replay case ({mission_label(mission)}){stops_s}:")
    print(f"  {case_jsonl} ({len(filtered)} events)")
    print(f"  {case_sh}")
    return case_sh


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

        if e == "menu_click":
            item = ev.get("item", "")
            sname = ev.get("screen_name", "")
            box = ev.get("box", [0, 0, 0, 0])
            gx = box[0] + box[2] // 2 if len(box) >= 4 and box[2] > 0 else 320
            gy = box[1] + box[3] // 2 if len(box) >= 4 and box[3] > 0 else 240
            steps.append(f"# Menu: {item} on {sname}")
            steps.append(f"g:{gx},{gy}")
            last_t = t
        elif e == "ui_click":
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
    return case_sh


def generate_split_missions(events, repo_root: Path, prefix: str, mode: str, autokill=1, teleport=0, invuln=1, stops_only=False, strip_mod=False, min_actions=1):
    segments = split_missions(events)
    if not segments:
        raise SystemExit("No mission_start segments found in recording.")

    action_types = {"sarge_stop", "stop", "move", "walk_target", "weapon_pickup", "weapon_select", "weapon_fire", "mine_lay", "enemy_kill", "vehicle_enter", "vehicle_capture", "vehicle_exit", "ui_click", "menu_click", "bookmark"}
    scripts = []
    for seg in segments:
        actions = sum(1 for ev in seg["events"] if ev.get("e") in action_types)
        if actions < min_actions:
            print(f"Skipping mission segment #{seg['idx']} (mission {seg['mission']}, only {actions} action(s) < --min-actions {min_actions})")
            continue
        case_name = f"{prefix}_{seg['idx']:02d}_m{seg['mission']}"
        out_dir = repo_root / "test" / "cases" / case_name
        if mode in ("replay", "both"):
            scripts.append(generate_replay_case(seg["events"], out_dir, case_name, autokill=autokill, teleport=teleport, invuln=invuln, stops_only=stops_only, strip_mod=strip_mod))
        if mode in ("clicks", "both"):
            scripts.append(generate_clicks_case(seg["events"], out_dir, case_name))

    if not scripts:
        raise SystemExit("All mission segments were filtered out (try --min-actions 0).")

    suite_path = repo_root / "test" / "cases" / f"{prefix}_suite.sh"
    lines = [
        "#!/bin/bash",
        f"# Auto-generated multi-mission test suite ({len(scripts)} case(s)) from {prefix}",
        f"# Run from repo root: bash test/cases/{prefix}_suite.sh",
        "set -u",
        "PASS=0; FAIL=0",
    ]
    for sc in scripts:
        rel = sc.relative_to(repo_root).as_posix()
        lines.append(f'echo "=== Running {rel} ==="')
        lines.append(f'if bash "{rel}"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); fi')
    lines.append('echo "=== Suite summary: $PASS passed, $FAIL failed ==="')
    lines.append('[ "$FAIL" -eq 0 ]')
    suite_path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"\nWrote multi-mission suite runner ({len(scripts)} test cases):")
    print(f"  {suite_path}")


def main():
    ap = argparse.ArgumentParser(description="Inspect and convert Army Men Modern .jsonl recordings into automated tests.")
    ap.add_argument("input", help="Path or glob to recorded .jsonl file(s)")
    ap.add_argument("--info", action="store_true", help="Print a summary of the recording(s) and exit")
    ap.add_argument("--out", help="Output test case name (or prefix with --split-missions), written under test/cases/")
    ap.add_argument("--split-missions", action="store_true", help="Split each mission segment in the recording into its own test case + suite runner")
    ap.add_argument("--min-actions", type=int, default=1, help="Minimum gameplay actions required to keep a segment with --split-missions (default: 1)")
    ap.add_argument("--mode", choices=("replay", "clicks", "both"), default="replay", help="Test style to generate (default: replay)")
    ap.add_argument("--stops-only", action="store_true", help="Keep only stationary stop waypoints and actions (teleports between stops, drops intermediate walking breadcrumbs)")
    ap.add_argument("--strip-mod-actions", action="store_true", help="Strip out actions only available via mods to target the unmodded vanilla game")
    ap.add_argument("--mission", type=int, help="Extract first segment matching this mission ID")
    ap.add_argument("--mission-idx", type=int, help="Extract the Nth (1-based) mission segment from --info")
    ap.add_argument("--run", type=int, help="Extract segment matching this mission run number")
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
    repo_root = Path(__file__).resolve().parent.parent

    if args.split_missions:
        generate_split_missions(
            events, repo_root, args.out, args.mode,
            autokill=0 if args.no_autokill else 1,
            teleport=1 if (args.teleport or args.stops_only) else 0,
            invuln=0 if args.no_invuln else 1,
            stops_only=args.stops_only,
            strip_mod=args.strip_mod_actions,
            min_actions=args.min_actions,
        )
        return

    sliced = slice_events(
        events, mission=args.mission, mission_idx=args.mission_idx, run_num=args.run,
        bookmark=args.bookmark, window_s=args.window,
        from_ms=args.from_ms, to_ms=args.to_ms,
    )

    out_dir = repo_root / "test" / "cases" / args.out
    if args.mode in ("replay", "both"):
        generate_replay_case(
            sliced, out_dir, args.out,
            autokill=0 if args.no_autokill else 1,
            teleport=1 if (args.teleport or args.stops_only) else 0,
            invuln=0 if args.no_invuln else 1,
            stops_only=args.stops_only,
            strip_mod=args.strip_mod_actions,
        )
    if args.mode in ("clicks", "both"):
        generate_clicks_case(sliced, out_dir, args.out)


if __name__ == "__main__":
    main()
