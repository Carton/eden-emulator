#!/usr/bin/env python3
r"""Eden TOTK FPS benchmark (uses eden's built-in record_frame_times CSV).

Flow: kill stray eden -> purge old TOTK CSV -> launch eden.exe TOTK.nsp ->
auto-tap A to enter game (focus_test.ps1) -> idle measure window ->
graceful close (taskkill WM_CLOSE, triggers PerfStats dtor -> CSV dump) ->
parse last N seconds of frame times -> append row to bench_results.csv.

Usage:  python tools/prof/bench_run.py LABEL [--measure 90]      (from repo root)
Prereq: qt-config.ini has record_frame_times=true and confirmStop=2 (Ask_Never);
        user not touching keyboard during the run (foreground lock).

Paths: script-relative (same-dir helpers, REPO inferred from tools/prof/).
Data (bench_results.csv, shots/, diag/) lives under EDEN_PROF_DATA
(default F:\prof, the historical archive); game NSP under EDEN_NSP.
"""
import glob
import datetime
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))          # repo root (tools/prof/)
DATA = os.environ.get("EDEN_PROF_DATA", r"F:\prof")    # data archive
EDEN_DIR = os.environ.get("EDEN_DIR", os.path.join(REPO, "build-vs22", "bin"))
EDEN = os.path.join(EDEN_DIR, "eden.exe")
NSP = os.environ.get("EDEN_NSP", os.path.join(DATA, "TOTK.nsp"))
LOGDIR = os.path.join(EDEN_DIR, "user", "log")
TITLE = "0100F2C0115B6000"
RESULTS = os.path.join(DATA, "bench_results.csv")
FOCUS = os.path.join(HERE, "focus_test.ps1")


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def tap_a():
    r = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", FOCUS],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    return "focus_ok=True" in ((r.stdout or "") + (r.stderr or ""))


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "unlabeled"
    measure = 90
    if "--measure" in sys.argv:
        measure = int(sys.argv[sys.argv.index("--measure") + 1])
    hold = "--hold" in sys.argv
    notap = "--no-tap" in sys.argv
    if hold:
        measure = int(sys.argv[sys.argv.index("--hold") + 1])

    cmd = [sys.executable, os.path.join(HERE, "check_config.py")]
    if "--tolerate-overlay" in sys.argv:
        cmd.append("--tolerate-overlay")
    r = subprocess.run(cmd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    print(r.stdout, end="")
    if r.returncode != 0:
        print("PREFLIGHT FAIL: fix qt-config.ini before measuring")
        sys.exit(2)
    subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)
    time.sleep(2)

    affinity = None
    if "--affinity" in sys.argv:
        affinity = sys.argv[sys.argv.index("--affinity") + 1]
    t0 = time.time()
    p = subprocess.Popen([EDEN, NSP], cwd=EDEN_DIR)
    log(f"launched pid={p.pid}")
    if affinity:
        time.sleep(2)
        rc = subprocess.run(["powershell", "-NoProfile", "-Command",
                            f"(Get-Process -Id {p.pid}).ProcessorAffinity = {affinity}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
        log(f"affinity {affinity}: {'ok' if rc == 0 else 'FAILED'}")

    # Verified enter-game flow (HANDOFF 2.2): boot~45s -> 3 A-taps 8s apart -> ~26s load
    time.sleep(50)
    if notap:
        log("no-tap mode: staying at menu, no input")
        time.sleep(24)
    else:
        ok_any = False
        for i in range(8):
            ok = tap_a()
            ok_any = ok_any or ok
            log(f"A-tap {i + 1}/8 focus_ok={ok}")
            time.sleep(6)
            if i >= 2 and ok_any:
                break
    time.sleep(30)
    log(f"assumed in-game (t+{time.time() - t0:.0f}s), {'holding' if hold else 'measuring'} "
        f"{measure}s - DO NOT TOUCH KEYBOARD")

    # Measure window; image QA shots are taken inside it (default on,
    # --no-shots disables) without changing the total duration.
    shot_dir = None
    start = time.time()
    if hold or "--no-shots" in sys.argv:
        time.sleep(measure)
    else:
        next_shot = start + 5
        n_shots = 0
        shot_dir = os.path.join(DATA, "shots", label)
        try:
            import shot_capture
            while True:
                now = time.time()
                remaining = measure - (now - start)
                if remaining <= 0:
                    break
                time.sleep(min(max(next_shot - now, 0), remaining))
                if time.time() - start >= measure:
                    break
                if time.time() >= next_shot - 0.05:
                    name = f"t+{int(time.time() - t0)}s.png"
                    if shot_capture.save_shot(p.pid, os.path.join(shot_dir, name)):
                        n_shots += 1
                    next_shot += 15
            if n_shots:
                log(f"shots: {n_shots} -> {shot_dir}")
            else:
                log("WARN no shots captured")
        except Exception as e:  # noqa: BLE001 - QA shots must never kill a bench
            log(f"WARN shot capture failed: {e}")
        time.sleep(max(0, measure - (time.time() - start)))

    if hold:
        subprocess.run(["taskkill", "/IM", "eden.exe"], capture_output=True)
        for _ in range(60):
            if p.poll() is not None:
                break
            time.sleep(1)
        if p.poll() is None:
            subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)
        print("HOLD DONE")
        return

    subprocess.run(["taskkill", "/IM", "eden.exe"], capture_output=True)  # graceful WM_CLOSE
    for _ in range(60):
        if p.poll() is not None:
            break
        time.sleep(1)
    forced = p.poll() is None
    if forced:
        log("WARN graceful close timed out, forcing kill (CSV lost)")
        subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)
        print("RESULT no data (forced close)")
        sys.exit(1)
    time.sleep(2)

    csvs = sorted(glob.glob(os.path.join(LOGDIR, f"*_{TITLE}.csv")), key=os.path.getmtime)
    if not csvs:
        print("RESULT no data (no CSV found)")
        sys.exit(1)
    path = csvs[-1]
    if os.path.getmtime(path) < t0:
        # No CSV newer than this run's launch => the game never wrote frame
        # data (stayed at the title menu).  The file above is a PREVIOUS
        # run's; parsing it silently poisoned bench_results.csv (three
        # bit-identical rows on 2026-09-19 21:03-21:10).
        print(f"RESULT VOID {label}: stale CSV ({os.path.basename(path)}) -- "
              "game never entered gameplay this run (user at keyboard?)")
        sys.exit(1)
    times = [float(x) for x in open(path).read().split() if x.strip()]

    win, acc = [], 0.0
    for ms in reversed(times):
        acc += ms
        win.append(ms)
        if acc >= measure * 1000:
            break
    win = win[5:]  # drop boundary frames
    n = len(win)
    if n < 30:
        print(f"RESULT unreliable: only {n} frames in window")
        sys.exit(1)
    mean_ms = sum(win) / n
    fps = 1000.0 / mean_ms
    s = sorted(win)

    def pct(q):
        return s[min(n - 1, int(q * n))]

    med = s[n // 2]
    if fps > 52 or med < 20.0:
        # Title menu / loading runs at a locked 60fps (frame raster 16.67ms);
        # every in-game mode of this scene has med >= 21.67. The 60fps raster
        # survives hitch-heavy boots as an fps average below 52 (golden-p4a:
        # 48.02 with med 16.67), so both signatures must be checked.
        sig = f"fps={fps:.2f}" if fps > 52 else f"med={med:.2f} (60fps raster)"
        print(f"RESULT VOID {label}: {sig} -- gameplay never started this run")
        sys.exit(1)
    k = max(1, int(0.01 * n))
    low1 = 1000.0 / (sum(s[-k:]) / k)  # slowest 1% of frames
    commit = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True).stdout.strip()

    # Scene-brightness covariate from this run's QA shots. Clean runs of the
    # pond scene all land at luma 60-61 with in-run std ~0.2; a far-off luma
    # (or high std) flags a contaminated/foreign capture for review.
    luma_str = ""
    if shot_dir:
        try:
            import glob as _g
            import numpy as np
            from PIL import Image
            pngs = sorted(_g.glob(os.path.join(shot_dir, "*.png")))
            if pngs:
                means = [float(np.asarray(
                    Image.open(f).convert("L").resize((96, 54)),
                    dtype=np.float32).mean()) for f in pngs]
                luma_str = f"{sum(means) / len(means):.1f}"
        except Exception as e:  # noqa: BLE001 - covariate must not kill a bench
            log(f"WARN luma computation failed: {e}")

    header = "time,label,commit,frames,fps_mean,fps_1p_low,ms_median,ms_p95,ms_p99,csv,forced,luma"
    if not os.path.exists(RESULTS):
        with open(RESULTS, "w") as fh:
            fh.write(header + "\n")
    else:
        with open(RESULTS) as fh:
            lines = [l for l in fh.read().splitlines() if l.strip()]
        if lines and lines[0] != header:
            with open(RESULTS, "w") as fh:  # migrate schema, pad old rows
                fh.write(header + "\n")
                for l in lines[1:]:
                    fh.write(l + ",\n")
    with open(RESULTS, "a") as fh:
        fh.write(f"{datetime.datetime.now():%Y-%m-%d %H:%M:%S},{label},{commit},{n},"
                 f"{fps:.2f},{low1:.2f},{med:.2f},{pct(0.95):.2f},{pct(0.99):.2f},"
                 f"{os.path.basename(path)},{forced},{luma_str}\n")
    # Tail metric: drop the first 15s of the window (post-load catch-up presents)
    tail, tacc = [], 0.0
    for ms in reversed(win):
        tail.append(ms)
        tacc += ms
        if tacc >= (measure - 15) * 1000:
            break
    tail_fps = 1000.0 / (tacc / len(tail)) if tail else fps
    print(f"RESULT {label}: frames={n} fps={fps:.2f} 1%low={low1:.2f} "
          f"med={med:.2f}ms p95={pct(0.95):.2f}ms p99={pct(0.99):.2f}ms "
          f"tail_fps={tail_fps:.2f} luma={luma_str or 'n/a'}")

    # Preserve this run's diag lines (SerialDraw/DrawToken/Resolver counters;
    # eden_log.txt is truncated on next boot) for per-draw A/B attribution.
    try:
        logtxt = open(os.path.join(LOGDIR, "eden_log.txt"),
                      encoding="utf-8", errors="replace").read()
        dlines = [l for l in logtxt.splitlines() if "diag" in l.lower()][-16:]
        if dlines:
            os.makedirs(os.path.join(DATA, "diag"), exist_ok=True)
            with open(os.path.join(DATA, "diag", f"{label}.txt"), "a") as fh:
                fh.write(f"=== {datetime.datetime.now():%Y-%m-%d %H:%M:%S} "
                         f"{label} commit={commit}\n")
                fh.write("\n".join(dlines) + "\n")
            for l in dlines[-2:]:
                print("DIAG " + l.strip())
    except Exception as e:  # noqa: BLE001 - diag archival must not kill a bench
        log(f"WARN diag archival failed: {e}")


if __name__ == "__main__":
    main()
