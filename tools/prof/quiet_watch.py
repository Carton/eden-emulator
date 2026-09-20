#!/usr/bin/env python3
"""Generic quiet-gated bench/watch driver (the *_watch.py pattern, distilled).

Waits until the machine has been idle (no keyboard/mouse for --idle seconds)
and no eden.exe is running, then executes a sequence of bench runs (and
optional arbitrary commands, e.g. an in-window build) without supervision.
Between entries it pauses while the user is active (< --resume-idle s) and
resumes after they leave again. Replaces the per-experiment watcher scripts
(PROFILE §28.12-28.16: band/epoch/async/round*_watch.py).

The sequence is a Python file defining:

    RUNS = [
        {"label": "warmup", "env": {}},                     # bench run
        {"label": "build", "cmd": ["bash", "-lc", "..."]},  # arbitrary command
        {"label": "token-ep1", "env": {"EDEN_DRAW_TOKEN": "1"}},
    ]
    KILL_LS = True        # taskkill LosslessScaling once, after quiet (default)
    PAIRS = [("golden", "token")]   # optional: report B/A fps ratios + median
                                    # for runs whose labels start with these
                                    # prefixes, zipped in execution order

Usage (foreground):
    python tools/prof/quiet_watch.py myseq.py [--idle 180] [--deadline 150]
Detached (survives this shell, log to file):
    powershell -NoProfile -Command "Start-Process -WindowStyle Hidden -FilePath \
      python -ArgumentList 'tools/prof/quiet_watch.py','myseq.py' \
      -RedirectStandardOutput 'F:/prof/myseq.log' -RedirectStandardError 'F:/prof/myseq.err'"

Notes:
  - 'cmd' entries must be quiet themselves: they run inside the quiet window
    (that is the point -- builds/benches never overlap user activity).
  - bash in cmd entries: use the FULL path (C:/Program Files/Git/bin/bash.exe)
    -- a bare "bash" resolves to WSL's bash and /f/... paths break (bit us in
    §28.16).
  - Data (bench_results.csv rows, shots, diag) goes where bench_run puts it:
    EDEN_PROF_DATA (default F:\\prof).
"""
import ctypes
import datetime
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.join(HERE, "bench_run.py")


class LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]


user32 = ctypes.windll.user32


def idle_seconds():
    li = LASTINPUTINFO()
    li.cbSize = ctypes.sizeof(li)
    user32.GetLastInputInfo(ctypes.byref(li))
    return (ctypes.windll.kernel32.GetTickCount64() - li.dwTime) / 1000.0


def log(msg):
    print(f"[{datetime.datetime.now():%H:%M:%S}] {msg}", flush=True)


def eden_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq eden.exe"],
                         capture_output=True, text=True,
                         encoding="utf-8", errors="replace").stdout
    return "eden.exe" in out


def wait_idle(threshold, deadline_s):
    started = time.time()
    while time.time() - started < deadline_s * 60:
        if idle_seconds() >= threshold and not eden_running():
            return True
        time.sleep(15)
    return False


def run_bench(label, env_over):
    env = dict(os.environ)
    for k, v in (env_over or {}).items():
        env[k] = v
    r = subprocess.run([sys.executable, BENCH, label],
                       capture_output=True, text=True, env=env,
                       encoding="utf-8", errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    m = re.search(r"^RESULT (\S+):.*?fps=([\d.]+).*?med=([\d.]+)ms.*?luma=(\S+)",
                  out, re.M)
    for line in out.splitlines():
        if "mismatch" in line.lower() or "diag:" in line.lower():
            log("  " + line.strip()[-170:])
    if not m or "VOID" in m.group(1):
        log(f"{label}: VOID/failed")
        return None
    log(f"{label}: fps={m.group(2)} med={m.group(3)} luma={m.group(4)}")
    return float(m.group(2))


def run_cmd(label, cmd):
    log(f"{label}: running {cmd}")
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace") as p:
        for line in p.stdout:
            log("  " + line.rstrip()[-170:])
        rc = p.wait()
    log(f"{label}: rc={rc}")
    return rc


def main():
    args = sys.argv[1:]
    seq_path = args[0] if args else "quiet_seq.py"
    idle_min = int(args[args.index("--idle") + 1]) if "--idle" in args else 180
    deadline = int(args[args.index("--deadline") + 1]) if "--deadline" in args else 150
    resume_idle = (int(args[args.index("--resume-idle") + 1])
                   if "--resume-idle" in args else 60)

    import importlib.util
    spec = importlib.util.spec_from_file_location("quiet_seq", seq_path)
    seq = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(seq)
    runs = seq.RUNS
    pairs = getattr(seq, "PAIRS", [])

    if not wait_idle(idle_min, deadline):
        log("give up: no quiet window within deadline")
        return
    log(f"quiet -- sequence start ({os.path.basename(seq_path)}, {len(runs)} entries)")
    if getattr(seq, "KILL_LS", True):
        subprocess.run(["taskkill", "/IM", "LosslessScaling.exe", "/F"],
                       capture_output=True)

    fps = {}
    for entry in runs:
        label = entry["label"]
        if "cmd" in entry:
            run_cmd(label, entry["cmd"])
            value = None
        else:
            value = run_bench(label, entry.get("env"))
        fps[label] = value
        # pause while the user is active so their session stays usable
        if idle_seconds() < resume_idle and not eden_running():
            log("user active -- pausing")
            wait_idle(resume_idle, 60)

    import statistics
    for a_pref, b_pref in pairs:
        ra = [v for k, v in fps.items() if k.startswith(a_pref) and v]
        rb = [v for k, v in fps.items() if k.startswith(b_pref) and v]
        if ra and rb:
            ratios = [b / a for a, b in zip(ra, rb)]
            med = statistics.median(ratios) if len(ratios) > 1 else ratios[0]
            for a, b in zip(ra, rb):
                log(f"pair ratio {b_pref}/{a_pref}: {b / a:.4f}")
            log(f"pair ratio {b_pref}/{a_pref} median: {med:.4f}")
    log("QUIET_WATCH_DONE")


if __name__ == "__main__":
    main()
