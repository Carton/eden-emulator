#!/usr/bin/env python3
"""Visual verification run: same flow as bench_run.py --hold but skips the
preflight process checks (for screenshot-only sessions where overlay noise
doesn't matter). Game stays up ~150s after entering for external screenshot."""
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
FOCUS = os.path.join(HERE, "focus_test.ps1")


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def tap_a():
    r = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", FOCUS],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    return "focus_ok=True" in ((r.stdout or "") + (r.stderr or ""))


def main():
    hold = 150
    if "--hold" in sys.argv:
        hold = int(sys.argv[sys.argv.index("--hold") + 1])
    # Preflight: same guard as bench_run, but tolerates the unkillable NVIDIA
    # Overlay (visual/stat runs don't measure frame pacing). Everything else
    # (user-launchable overlays like LosslessScaling) still hard-fails.
    r = subprocess.run([sys.executable, os.path.join(HERE, "check_config.py"),
                        "--tolerate-overlay"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    print(r.stdout, end="")
    if r.returncode != 0:
        print("PREFLIGHT FAIL: fix the environment before running")
        sys.exit(2)
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
    log(f"assumed in-game (t+{time.time() - t0:.0f}s), holding {hold}s for screenshot")
    time.sleep(hold)
    subprocess.run(["taskkill", "/IM", "eden.exe"], capture_output=True)
    for _ in range(60):
        if p.poll() is not None:
            break
        time.sleep(1)
    if p.poll() is None:
        subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)
    print("VISUAL RUN DONE")


if __name__ == "__main__":
    main()
