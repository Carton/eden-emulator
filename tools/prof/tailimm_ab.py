#!/usr/bin/env python3
"""Back-to-back ETW A/B capture through one elevated helper (layer-3 method).

Two (or more) 40s CPU traces on the SAME binary/session (scene phase
comparable) captured by ONE elevated helper process (single UAC click) that
polls for phase command files and runs wpr on demand. Edit PHASES below for
your experiment (name, env overrides, etl output); the helper's watched
file list is generated from it. Historical default: the §28.6 token
TAIL_IMM pair (tailimm_off/on.etl).

Data (etl, wpr logs, handshake files) lands under EDEN_PROF_DATA
(default F:\\prof). Compare traces only per-draw-normalized (AGENTS
"三层法"); never compare absolute thread CPU across windows.
"""
import os
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DATA = os.environ.get("EDEN_PROF_DATA", r"F:\prof")
EDEN_DIR = os.environ.get("EDEN_DIR", os.path.join(REPO, "build-vs22", "bin"))
FOCUS = os.path.join(HERE, "focus_test.ps1")
NSP = os.environ.get("EDEN_NSP", os.path.join(DATA, "TOTK.nsp"))
TMP = os.path.join(DATA, "wpr_tmp")
os.makedirs(TMP, exist_ok=True)

HELPER = os.path.join(DATA, "wpr_helper.cmd")
PHASES = [
    ("tailimm_off", {}, os.path.join(DATA, "tailimm_off.etl")),
    ("tailimm_on", {"EDEN_DRAW_TOKEN": "1", "EDEN_TOKEN_TAIL_IMM": "1"},
     os.path.join(DATA, "tailimm_on.etl")),
]


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def run_phase(name, extra_env, etl):
    wprlog = os.path.join(DATA, f"wpr_{name}.log")
    go = os.path.join(DATA, f"wpr_go_{name}.cmd")
    cmd = (f"@echo off\n"
           f"wpr -cancel >nul 2>&1\n"
           f"wpr -start CPU -filemode -recordtempto {DATA} > {wprlog} 2>&1\n"
           f"echo STARTED >> {wprlog}\n"
           f"timeout /t 40 /nobreak >nul\n"
           f"wpr -stop {etl} >> {wprlog} 2>&1\n"
           f"echo WPR_DONE >> {wprlog}\n")
    open(go, "w").write(cmd)
    for f in (wprlog, etl):
        if os.path.exists(f):
            os.remove(f)

    subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)
    time.sleep(2)
    env = dict(os.environ)
    env.update(extra_env)
    p = subprocess.Popen([os.path.join(EDEN_DIR, "eden.exe"), NSP],
                         cwd=EDEN_DIR, env=env)
    log(f"[{name}] launched pid={p.pid} env={extra_env or 'clean'}")
    time.sleep(55)
    for i in range(3):
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                            "-File", FOCUS], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        log(f"[{name}] A-tap {i+1}/3 focus_ok={'focus_ok=True' in (r.stdout or '')}")
        time.sleep(8)
    time.sleep(25)
    log(f"[{name}] in-game; helper picks up {go} (40s capture)")

    t0 = time.time()
    done = False
    while time.time() - t0 < 180:
        if os.path.exists(wprlog):
            try:
                s = open(wprlog, "r", errors="replace").read()
            except OSError:
                s = ""
            if "WPR_DONE" in s:
                done = True
                break
        time.sleep(5)
    log(f"[{name}] wpr done={done} etl={'yes' if os.path.exists(etl) else 'MISSING'}")

    subprocess.run(["taskkill", "/IM", "eden.exe"], capture_output=True)
    for _ in range(60):
        if p.poll() is not None:
            break
        time.sleep(1)
    if p.poll() is None:
        subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)


helper = "@echo off\n:loop\n"
for name, _, _ in PHASES:
    go = os.path.join(DATA, f"wpr_go_{name}.cmd")
    helper += (f'if exist "{go}" (call "{go}" & del "{go}" >nul 2>&1)' + "\n")
helper += (f'if exist "{DATA}\\wpr_helper_stop" (del "{DATA}\\wpr_helper_stop" >nul 2>&1 & exit)' + "\n"
           "timeout /t 1 /nobreak >nul\ngoto loop\n")
open(HELPER, "w").write(helper)

log("launching elevated helper - CLICK UAC ONCE for all captures")
subprocess.Popen(["powershell.exe", "-NoProfile", "-Command",
                  f"Start-Process -Verb RunAs -WindowStyle Minimized -FilePath '{HELPER}'"])
time.sleep(5)

for name, extra_env, etl in PHASES:
    run_phase(name, extra_env, etl)

open(os.path.join(DATA, "wpr_helper_stop"), "w").write("stop")
log("A/B CAPTURE DONE")
