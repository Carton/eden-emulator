#!/usr/bin/env python3
"""Whole-day frame CSV timeline: when did rotation-grade degradation start?

For each session CSV, report whole-session and tail-180s stats.
Tail-180s approximates the measurement period (stabilize+windows+close).
Usage: python tools/prof/csv_timeline.py [logdir] [date-prefix ...]
       logdir defaults to the current build's user/log; date prefixes
       (e.g. 2026-09-12) filter sessions, none = all sessions.
"""
import csv, glob, os, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGDIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    REPO, "build-vs22", "bin", "user", "log")
PREFIXES = sys.argv[2:]
paths = sorted(glob.glob(os.path.join(LOGDIR, "*_0100F2C0115B6000.csv")))
print(f"{'session':17s} {'frames':>6s} {'span_s':>7s} | tail180: {'fps':>5s} {'>30':>4s} {'>40':>4s} {'>66':>4s} {'max':>7s}")
for p in paths:
    tag = os.path.basename(p)[:16]
    if PREFIXES and not any(tag.startswith(x) for x in PREFIXES):
        continue
    ms = [float(r[0]) for r in csv.reader(open(p)) if r]
    total = sum(ms) / 1000.0
    tail = []
    acc = 0.0
    for m in ms:
        acc += m
    # walk from end collecting last 180s
    s = 0.0
    for m in reversed(ms):
        s += m
        tail.append(m)
        if s >= 180000.0:
            break
    fps = len(tail) / (sum(tail) / 1000.0)
    c30 = sum(1 for m in tail if m > 30)
    c40 = sum(1 for m in tail if m > 40)
    c66 = sum(1 for m in tail if m > 66)
    mx = max(tail)
    print(f"{tag:17s} {len(ms):6d} {total:7.0f} | {fps:5.1f} {c30:4d} {c40:4d} {c66:4d} {mx:7.1f}")
