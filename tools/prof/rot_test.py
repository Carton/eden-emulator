#!/usr/bin/env python3
"""Eden TOTK camera-rotation stutter test.

Flow: launch eden -> auto A-taps into game -> stabilize -> alternating
idle / rotate-left / rotate-right windows (rot_hold.ps1 injects held arrow
keys) -> graceful close -> window-wise frame analysis from the built-in
record_frame_times CSV.

Usage: python rot_test.py LABEL [--win 20] [--reps 2]
Output: per-window fps / spike counts appended to stdout (not to
bench_results.csv; separate concern).
"""
import csv
import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DATA = os.environ.get("EDEN_PROF_DATA", r"F:\prof")
EDEN_DIR = os.environ.get("EDEN_DIR", os.path.join(REPO, "build-vs22", "bin"))
EDEN = os.path.join(EDEN_DIR, "eden.exe")
NSP = os.environ.get("EDEN_NSP", os.path.join(DATA, "TOTK.nsp"))
LOGDIR = os.path.join(EDEN_DIR, "user", "log")
TITLE = "0100F2C0115B6000"
FOCUS = os.path.join(HERE, "focus_test.ps1")
ROTHOLD = os.path.join(HERE, "rot_hold.ps1")


def preflight():
    """Abort before touching the game if config drifted off the profile."""
    r = subprocess.run([sys.executable, os.path.join(HERE, "check_config.py")],
                       capture_output=True, text=True)
    print(r.stdout, end="")
    if r.returncode != 0:
        print("PREFLIGHT FAIL: fix qt-config.ini before measuring (see FAIL lines above)")
        sys.exit(2)


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def tap_a():
    r = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", FOCUS],
        capture_output=True, text=True)
    return "focus_ok=True" in ((r.stdout or "") + (r.stderr or ""))


def hold_window(direction, secs):
    r = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ROTHOLD,
         "-Dir", direction, "-HoldSec", str(secs)],
        capture_output=True, text=True)
    return "focus_ok=True" in ((r.stdout or "") + (r.stderr or ""))


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "rot"
    win = 20
    if "--win" in sys.argv:
        win = int(sys.argv[sys.argv.index("--win") + 1])
    reps = 2
    if "--reps" in sys.argv:
        reps = int(sys.argv[sys.argv.index("--reps") + 1])

    preflight()
    subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)
    time.sleep(2)

    t0 = time.time()
    p = subprocess.Popen([EDEN, NSP], cwd=EDEN_DIR)
    log(f"launched pid={p.pid}")

    time.sleep(50)
    for i in range(3):
        ok = tap_a()
        log(f"A-tap {i + 1}/3 focus_ok={ok}")
        time.sleep(8)
    time.sleep(30)
    log(f"assumed in-game (t+{time.time() - t0:.0f}s); stabilizing 30s")
    time.sleep(30)

    # window schedule: [idle, rotR, idle, rotL] x reps
    windows = []  # (name, t_start_wall, t_end_wall)
    import json
    for rep in range(reps):
        for name in ("idle1", "rotR", "idle2", "rotL"):
            log(f"window {name} #{rep + 1} start")
            t_start = time.time()
            ok = hold_window("none" if name.startswith("idle") else
                             ("right" if name == "rotR" else "left"), win)
            t_end = time.time()
            if not ok:
                log(f"WARN focus failed in {name}")
            windows.append((f"{name}#{rep + 1}", t_start, t_end))
            log(f"window {name} #{rep + 1} end (focus_ok={ok})")

    time.sleep(5)  # let the last window fully land in the CSV
    log("closing gracefully")
    t_close = time.time()
    with open(os.path.join(DATA, "rot_windows.json"), "w") as fh:
        json.dump({"label": label, "windows": windows, "t_close": t_close}, fh)
    subprocess.run(["taskkill", "/IM", "eden.exe"], capture_output=True)  # WM_CLOSE
    for _ in range(60):
        if p.poll() is not None:
            break
        time.sleep(1)
    if p.poll() is None:
        subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)
        print("RESULT no data (forced close)")
        sys.exit(1)
    time.sleep(2)

    csvs = sorted(glob.glob(os.path.join(LOGDIR, f"*_{TITLE}.csv")), key=os.path.getmtime)
    if not csvs:
        print("RESULT no data (no CSV)")
        sys.exit(1)
    ms = [float(r[0]) for r in csv.reader(open(csvs[-1])) if r]

    # map frames to wall time: last frame ends ~ t_close
    cums = []
    acc = 0.0
    for m in ms:
        cums.append(acc)
        acc += m
    total = acc
    def frame_wall(i):
        return t_close - (total - cums[i]) / 1000.0

    print(f"RESULT {label}: csv={os.path.basename(csvs[-1])} frames={len(ms)}")
    print(f"{'window':10s} {'frames':>6s} {'fps':>6s} {'>30ms':>6s} {'>40ms':>6s} {'>66ms':>6s} {'maxms':>7s}")
    for name, ws, we in windows:
        idx = [i for i in range(len(ms)) if ws <= frame_wall(i) < we]
        if not idx:
            print(f"{name:10s} {'-':>6s}")
            continue
        sub = [ms[i] for i in idx]
        dur = sum(sub)
        spikes30 = sum(1 for m in sub if m > 30)
        spikes40 = sum(1 for m in sub if m > 40)
        spikes66 = sum(1 for m in sub if m > 66)
        print(f"{name:10s} {len(sub):6d} {1000.0 * len(sub) / dur:6.2f} "
              f"{spikes30:6d} {spikes40:6d} {spikes66:6d} {max(sub):7.1f}")


if __name__ == "__main__":
    main()
