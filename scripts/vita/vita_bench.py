#!/usr/bin/env python3
"""
Repeatable Arx Libertatis benchmark runs on the Vita (see
src/platform/vita/VitaBench.h for the in-game side).

Record a route once, by playing:
  scripts/vita/vita_bench.py --record [--route crypt.txt]
  then launch Arx on the Vita, load a save and play the route; quit the game
  to end it. Recording starts when the save has loaded (loading another save
  starts it over from that one). The route and a copy of the save land in
  ux0:data/arx/bench/.

Replay it on every build, with no one at the console:
  scripts/vita/vita_bench.py [--deploy] --label NAME [--route crypt.txt]
  1. (--deploy) upload build-vita/eboot.bin
  2. write bench.txt and logserver.txt on the device
  3. launch Arx through vagentd (port 1339)
  4. follow the live log (port 1340) until "[Bench] done", a crash, or --timeout
  5. remove bench.txt again, so a manual launch plays normally
  6. print the result and append it to ~/Dev/vita/device-backups/arx-bench-ledger.tsv

  scripts/vita/vita_bench.py --results [N]   the last N runs, side by side

Needs an FTP server on the Vita (VITA_FTP_HOST, default the last one found).
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.expanduser("~/Dev/vita-agent/host"))

import vita_device_mcp as dev  # noqa: E402

RUNS = os.path.expanduser("~/Dev/vita/device-backups/runs")
LEDGER = os.path.expanduser("~/Dev/vita/device-backups/arx-bench-ledger.tsv")
TITLE = dev.TITLE
EBOOT = os.path.join(dev.REPO, "build-vita", "eboot.bin")


def say(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def write_toggle(name, lines):
    dev.put_bytes(("\n".join(lines) + "\n").encode(), dev.norm(name), keep_backup=False)


def clear_toggle(name):
    dev.delete(dev.norm(name), keep_backup=False)


def agent():
    from vita_client import Vita, SHELL_PORT

    v = Vita(dev.HOST, port=SHELL_PORT, timeout=30)
    v.connect()
    return v


def close_arx():
    """Close Arx if it is in front (a running app keeps its eboot locked against uploads)."""
    v = agent()
    try:
        _pid, front = v.front_app()
        if front == TITLE:
            say("Arx is already running: closing it first")
            v.close_app()
            time.sleep(3)
    finally:
        v.close()


def launch():
    close_arx()
    v = agent()
    try:
        say(v.start_app(TITLE))
        time.sleep(2)
        return v.front_app()[1]
    finally:
        v.close()


def take_screenshots(log, times, run_dir, tail):
    """Screenshots through vagentd, `times` seconds after the log says the route started."""
    start = None
    while start is None and tail.poll() is None:
        if os.path.exists(log) and "[Bench] route started" in open(log, errors="replace").read():
            start = time.time()
        else:
            time.sleep(0.25)
    if start is None:
        return
    from vita_client import Vita
    for t in sorted(times):
        delay = start + t - time.time()
        if delay > 0:
            time.sleep(delay)
        if tail.poll() is not None:
            return
        try:
            v = agent()
            try:
                w, h, _fmt, rgba = v.screenshot()
            finally:
                v.close()
            path = os.path.join(run_dir, f"shot-{t:g}s.png")
            with open(path, "wb") as f:
                f.write(Vita.png(w, h, rgba))
            say(f"screenshot: {path}")
        except Exception as e:  # noqa: BLE001 -- a missed screenshot must not end the run
            say(f"screenshot at {t:g}s failed: {e}")


def results_lines():
    try:
        ftp = dev.connect()
        try:
            return dev.fetch(ftp, dev.norm("bench/results.txt")).decode("utf-8", "replace").strip().splitlines()
        finally:
            ftp.quit()
    except Exception:  # noqa: BLE001 -- absent before the first run
        return []


def git_state():
    try:
        return subprocess.run(["git", "-C", dev.REPO, "describe", "--always", "--dirty"],
                              capture_output=True, text=True).stdout.strip()
    except OSError:
        return "?"


def eboot_hash():
    try:
        with open(EBOOT, "rb") as f:
            return hashlib.sha1(f.read()).hexdigest()[:10]
    except OSError:
        return "?"


def parse_result(result):
    """'avg=33.1ms median=33.3 ... cpu avg=20.1 median=19.8 p90=25.0 ...' -> {'avg': '33.1', 'cpu.avg': ...}.
    A bare 'cpu' or 'swap' word prefixes the avg/median/p90 that follow it."""
    values = {}
    section = ""
    for token in result.split():
        if "=" not in token:
            section = token if token in ("cpu", "swap") else ""
            continue
        key, value = token.split("=", 1)
        if section and key not in ("avg", "median", "p90"):
            section = ""
        values[f"{section}.{key}" if section else key] = re.sub(r"(ms|MB)$", "", value)
    return values


def show_zones(rows):
    """Zone breakdown of each run, one column per run, costliest zones first."""
    runs = []
    for row in rows:
        when, label, _outcome = (row + [""] * 3)[:3]
        zones = row[7] if len(row) > 7 else ""
        values = dict(tok.split("=", 1) for tok in zones.split() if "=" in tok)
        if values:
            runs.append((label or when, values))
    if not runs:
        print("no zone breakdowns in these runs")
        return
    names = sorted({k for _, v in runs for k in v}, key=lambda k: -float(runs[-1][1].get(k, 0)))
    width = max(len(n) for n in names)
    print(" " * width + "  " + "  ".join(f"{r[0][:12]:>12}" for r in runs))
    for name in names:
        print(f"{name:<{width}}  " + "  ".join(f"{r[1].get(name, '-'):>12}" for r in runs))


def show_results(count, zones=False):
    if not os.path.exists(LEDGER):
        print("no runs yet")
        return 0
    rows = [line.rstrip("\n").split("\t") for line in open(LEDGER)][1:][-count:]
    if zones:
        show_zones(rows)
        return 0
    cols = [("run", None), ("outcome", None), ("avg", "avg"), ("median", "median"), ("p90", "p90"),
            ("p99", "p99"), ("max", "max"), (">50ms", "hitches>50ms"), ("cpu", "cpu.avg"),
            ("upd", "upd"), ("rend", "rend"), ("swap", "swap.avg"), ("draws", "draws"),
            ("saveload", "save_load_ms"), ("git", None)]
    table = []
    for row in rows:
        when, label, outcome, git, _eboot, result = (row + [""] * 6)[:6]
        values = parse_result(result)
        fixed = {"run": label or when, "outcome": outcome, "git": git}
        table.append([fixed[name] if key is None else values.get(key, "-") for name, key in cols])
    widths = [max(len(name), *(len(t[i]) for t in table)) for i, (name, _) in enumerate(cols)]
    print("  ".join(name.ljust(w) for (name, _), w in zip(cols, widths)))
    for t in table:
        print("  ".join(v.ljust(w) for v, w in zip(t, widths)))
    print("frame times in ms; cpu = frame minus limiter sleep and swap (GPU/vsync wait)")
    return 0


def record(route):
    clear_toggle("bench.txt")
    write_toggle("benchrec.txt", [f"route={route}"])
    write_toggle("logserver.txt", ["on"])
    say(f"recording armed for bench/{route}: launch Arx on the Vita, load a save and play the route, "
        "then quit the game. Recording starts once the save has loaded (loading another save starts "
        "over from it); the next launch plays normally.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--deploy", action="store_true", help="upload build-vita/eboot.bin first")
    ap.add_argument("--record", action="store_true", help="arm route recording instead of replaying")
    ap.add_argument("--results", nargs="?", type=int, const=10, help="show the last N ledger runs and exit")
    ap.add_argument("--zones", action="store_true", help="with --results: the per-system breakdown instead")
    ap.add_argument("--route", default="route.txt", help="route file in ux0:data/arx/bench/")
    ap.add_argument("--label", default="")
    ap.add_argument("--settle", type=float, help="seconds at the route start before timing (default 2)")
    ap.add_argument("--fps", type=int, help="frame limit for this run only (0 = uncapped)")
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--on", action="append", default=[], metavar="FILE",
                    help="create toggle FILE in ux0:data/arx for this run only (e.g. noworldgpu.txt)")
    ap.add_argument("--set", action="append", default=[], metavar="FILE=VALUE",
                    help="write VALUE into toggle FILE in ux0:data/arx for this run only (e.g. worldcell.txt=16)")
    ap.add_argument("--shot", help="screenshots this many seconds after the route starts, e.g. 5,20")
    args = ap.parse_args()

    if args.results is not None:
        return show_results(args.results, args.zones)
    if args.record:
        return record(args.route)

    ftp = dev.connect()
    try:
        have_route = dev.remote_size(ftp, dev.norm(f"bench/{args.route}")) is not None
    finally:
        ftp.quit()
    if not have_route:
        say(f"no route bench/{args.route} on the device: record one first (--record)")
        return 1

    if args.deploy:
        close_arx()
        say(dev.tool_deploy({}))

    options = ["exit", f"route={args.route}"]
    if args.settle is not None:
        options.append(f"settle={args.settle:g}")
    if args.fps is not None:
        options.append(f"fps={args.fps}")
    clear_toggle("benchrec.txt")
    write_toggle("logserver.txt", ["on"])
    write_toggle("bench.txt", options)
    for name in args.on:
        write_toggle(name, ["on"])
    for spec in args.set:
        name, value = spec.split("=", 1)
        write_toggle(name, [value])

    stamp = time.strftime("%Y%m%d-%H%M%S")
    run_dir = os.path.join(RUNS, f"arx-bench-{stamp}" + (f"-{args.label}" if args.label else ""))
    os.makedirs(run_dir, exist_ok=True)
    log = os.path.join(run_dir, "arx.log")
    before = results_lines()

    try:
        say(f"front app after launch: {launch()}")
        tail = subprocess.Popen([sys.executable, os.path.join(HERE, "vita_logtail.py"), "--host", dev.HOST,
                                 "--out", log, "--until", r"\[Bench\] done", "--timeout", str(args.timeout)])
        if args.shot:
            take_screenshots(log, [float(t) for t in args.shot.split(",")], run_dir, tail)
        rc = tail.wait()
        outcome = {0: "done", 1: "crash-or-disconnect", 2: "timeout", 3: "no-logserver"}.get(rc, f"rc={rc}")
        say(f"outcome: {outcome}")
        time.sleep(4)  # "exit" quits the game
    finally:
        # bench.txt left behind would make every manual launch replay and quit
        try:
            clear_toggle("bench.txt")
            temporary = args.on + [spec.split("=", 1)[0] for spec in args.set]
            for name in temporary:
                clear_toggle(name)
            say("cleared bench.txt" + "".join(f", {n}" for n in temporary))
        except Exception as e:  # noqa: BLE001 -- a cleanup failure must not lose the result
            say(f"could not clear bench.txt: {e}")

    text = open(log, errors="replace").read() if os.path.exists(log) else ""
    m = re.search(r"\[Bench\] done (.*)", text)
    result = m.group(1).strip() if m else ""
    m = re.search(r"\[Bench\] zones (.*)", text)
    zones = m.group(1).strip() if m else ""
    for m in re.finditer(r"\[Load\] (.*)", text):
        say(f"load: {m.group(1).strip()}")
    if not result:
        # The game also appends every finished route to bench/results.txt
        new_lines = results_lines()[len(before):]
        done = [l for l in new_lines if "frames=" in l]
        if done:
            result = done[-1]
            outcome = "done"
            zones = next((l[len("zones "):] for l in reversed(new_lines) if l.startswith("zones ")), zones)
    say(f"result: {result or '(no result line)'}")
    if zones:
        say(f"zones (ms per frame): {zones}")

    new = not os.path.exists(LEDGER)
    os.makedirs(os.path.dirname(LEDGER), exist_ok=True)
    with open(LEDGER, "a") as f:
        if new:
            f.write("when\tlabel\toutcome\tgit\teboot\tresult\trun_dir\tzones\n")
        f.write(f"{stamp}\t{args.label}\t{outcome}\t{git_state()}\t{eboot_hash()}\t{result}\t{run_dir}\t{zones}\n")
    say(f"log: {log}")
    say(f"ledger: {LEDGER} (scripts/vita/vita_bench.py --results)")
    return 0 if outcome == "done" else 1


if __name__ == "__main__":
    sys.exit(main())
