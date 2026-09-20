#!/usr/bin/env python3
"""Capture the boot + loading screens (t+8..t+70) for image QA.

Usage: python tools/prof/load_capture.py <label>       (token off)
       set EDEN_DRAW_TOKEN=1 for token mode
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shot_capture

REPO = os.path.dirname(os.path.dirname(HERE))
DATA = os.environ.get("EDEN_PROF_DATA", r"F:\prof")
EDEN_DIR = os.environ.get("EDEN_DIR", os.path.join(REPO, "build-vs22", "bin"))
EDEN = os.path.join(EDEN_DIR, "eden.exe")

label = sys.argv[1] if len(sys.argv) > 1 else "load"
shot_dir = os.path.join(DATA, "shots", label)
os.makedirs(shot_dir, exist_ok=True)

nsp = os.environ.get("EDEN_NSP", os.path.join(DATA, "TOTK.nsp"))
assert os.path.exists(nsp), "TOTK nsp not found"

subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)
time.sleep(2)
env = dict(os.environ)
p = subprocess.Popen([EDEN, nsp], cwd=EDEN_DIR, env=env)
print(f"launched pid={p.pid} label={label}")
t0 = time.time()
resized = False
while time.time() - t0 < 150:
    time.sleep(2)
    if not resized:
        form = shot_capture.find_render_window(p.pid)
        if form:
            # bigger window -> higher internal render resolution
            import ctypes
            ctypes.windll.user32.SetWindowPos(form, 0, 50, 50, 1280, 720, 0x0004)
            resized = True
            print("resized Form to 1280x720")
    name = f"t+{int(time.time() - t0)}s.png"
    if shot_capture.save_shot(p.pid, os.path.join(shot_dir, name)):
        print(f"shot {name}")
subprocess.run(["taskkill", "/IM", "eden.exe", "/F"], capture_output=True)
print("done")
