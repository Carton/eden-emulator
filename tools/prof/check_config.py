#!/usr/bin/env python3
"""Pre-launch config guard for the TOTK profiling workflow.

Verifies build/bin/user/config/qt-config.ini matches the optimized profile
(ASTC CpuAsync + Bc3 + async shaders + CPU Unsafe + input automation) and
that no stray GPU enhancements (FSR/FXAA/...) silently crept in via GUI.

Usage:
    python check_config.py                    # print PASS/FAIL table, exit 1 on FAIL

NVIDIA Overlay is never killed and never fails the check (user rule 2026-09-19):
med is measured unaffected (PROFILE §24.5) and it respawns from nvcontainer
anyway -> WARN only. --tolerate-overlay is accepted for compat, no-op.
LosslessScaling remains a hard FAIL (frame pacer).
Exit codes: 0 = all pass, 1 = at least one FAIL (WARN does not fail).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))  # repo root (tools/prof/)

INI = os.path.join(os.environ.get(
    "EDEN_DIR", os.path.join(REPO, "build-vs22", "bin")),
    "user", "config", "qt-config.ini")

# key -> (required value, required \default flag [None = don't care])
MUST_MATCH = {
    "cpu_accuracy":              ("2", False),   # Unsafe
    "accelerate_astc":           ("2", False),   # CpuAsynchronous
    "astc_recompression":        ("2", False),   # Bc3
    "use_asynchronous_shaders":  ("true", False),
    "keyboard_enabled":          ("true", False),
    "record_frame_times":        ("true", None),
    "confirmStop":               ("2", False),
    # user baseline (official install has the same values; part of every
    # historical measurement — do NOT "fix" to compiled defaults)
    "scaling_filter":            ("6", False),   # FSR
    "anti_aliasing":             ("1", False),   # FXAA
    "gpu_accuracy":              ("0", False),   # Low
}

# keys that must stay at compiled default (value line ignored when \default=true)
MUST_DEFAULT = [
    "resolution_setup",
    "max_anisotropy",
    "use_vsync",
    "async_presentation",
    "use_reactive_flushing",
    "vram_usage_mode",
    "cpu_backend",
    "shader_backend",
]

# processes that must NOT be running (they pollute frame pacing / capture tax)
BANNED_PROCESSES = [
    "LosslessScaling.exe",
]

# never killed, never blocks (user rule 2026-09-19: overlay stays, med unaffected)
WARN_PROCESSES = [
    "NVIDIA Overlay.exe",
]

def parse(path):
    kv = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^(.+?)\\default=(.+)$", line.strip())
        if m:
            kv.setdefault(m.group(1), {})["default"] = m.group(2)
            continue
        m = re.match(r"^(.+?)=(.+)$", line.strip())
        if m:
            kv.setdefault(m.group(1), {})["value"] = m.group(2)
    return kv

def main():
    kv = parse(INI)
    fails, warns = [], []

    def get(k):
        e = kv.get(k, {})
        return e.get("value"), e.get("default")

    for k, (want, dflt) in MUST_MATCH.items():
        val, d = get(k)
        if val is None:
            fails.append(f"{k}: missing (want {want})")
        elif val != want:
            fails.append(f"{k}={val} (want {want})")
        elif dflt is not None and d is not None and d.lower() != str(dflt).lower():
            fails.append(f"{k}\\default={d} must be {str(dflt).lower()} or the value is ignored")

    for k in MUST_DEFAULT:
        val, d = get(k)
        if val is None:
            continue  # not present = compiled default, fine
        if d is None or d.lower() != "true":
            fails.append(f"{k}={val} \\default={d} — must be \\default=true (compiled default)")

    # input automation: rstick via J/L
    val, _ = get("player_0_rstick")
    if not (val and "code$074" in val and "code$076" in val):
        fails.append("player_0_rstick: J(74)/L(76) mapping missing — rot automation broken")

    # speed limit: mode report only
    val, d = get("use_speed_limit")
    mode = "limited" if (d and d.lower() == "true") or val == "true" else "unlocked"
    warns.append(f"use_speed_limit -> {mode} mode (choose per experiment)")

    # banned / tolerated processes (frame pacing interferers vs overlay rule)
    import subprocess
    out = subprocess.run(["tasklist", "/FO", "CSV"],
                         capture_output=True).stdout.decode("gbk", errors="replace")
    for proc in BANNED_PROCESSES:
        if proc.lower() in out.lower():
            fails.append(f"process {proc} is running — kill it (taskkill /IM \"{proc}\" /F)")
    for proc in WARN_PROCESSES:
        if proc.lower() in out.lower():
            warns.append(f"process {proc} is running (user rule 2026-09-19: "
                         f"never killed, med measured unaffected)")

    print(f"config check: {INI}")
    for f in fails:
        print(f"  FAIL  {f}")
    for w in warns:
        print(f"  WARN  {w}")
    if not fails:
        print("  ALL PASS")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
