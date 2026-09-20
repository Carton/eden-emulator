#!/usr/bin/env python3
"""Compare two bench shot directories (image QA).

Pairs PNGs by sorted order (timestamps drift slightly between runs), resizes
to a small grid so per-frame animation phase differences average out, and
reports per-pair diff stats with a PASS/WARN/FAIL verdict. The threshold is
calibrated against baseline-vs-baseline noise; run one golden-vs-golden
comparison first when in doubt.
"""
import os
import sys

from PIL import Image

SMALL = (96, 54)
BAD_PIXEL_THRESHOLD = 12   # mean per-channel delta (0-255) per pixel
PAIR_FAIL_PCT = 15.0
PAIR_WARN_PCT = 5.0


def load_small(path):
    return Image.open(path).convert("RGB").resize(SMALL, Image.LANCZOS)


def pair_stats(a, b):
    pa, pb = load_small(a), load_small(b)
    data_a, data_b = pa.getdata(), pb.getdata()
    total = 0
    bad = 0
    worst = 0
    sum_diff = 0
    for ca, cb in zip(data_a, data_b):
        d = (abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2])) // 3
        sum_diff += d
        total += 1
        if d > BAD_PIXEL_THRESHOLD:
            bad += 1
        worst = max(worst, d)
    return sum_diff / total, 100.0 * bad / total, worst


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    dirs = [d for d in (sys.argv[1], sys.argv[2]) if os.path.isdir(d)]
    if len(dirs) != 2:
        print("need two existing shot dirs")
        sys.exit(2)
    sets = []
    for d in dirs:
        files = sorted(f for f in os.listdir(d) if f.endswith(".png"))
        sets.append((d, [os.path.join(d, f) for f in files]))
    n = min(len(sets[0][1]), len(sets[1][1]))
    if n == 0:
        print("no shots to compare")
        sys.exit(2)
    print(f"{sets[0][0]} ({len(sets[0][1])}) vs {sets[1][0]} ({len(sets[1][1])}), {n} pairs")
    verdicts = []
    for i in range(n):
        mean, bad_pct, worst = pair_stats(sets[0][1][i], sets[1][1][i])
        v = "PASS"
        if bad_pct > PAIR_WARN_PCT:
            v = "WARN"
        if bad_pct > PAIR_FAIL_PCT:
            v = "FAIL"
        verdicts.append(v)
        print(f"  pair {i}: mean={mean:6.2f} bad%={bad_pct:6.2f} worst={worst:3d}  {v}")
    fails = verdicts.count("FAIL")
    warns = verdicts.count("WARN")
    overall = "FAIL" if fails else ("WARN" if warns else "PASS")
    print(f"OVERALL: {overall} ({fails} fail / {warns} warn / {n} pairs)")
    sys.exit(1 if overall == "FAIL" else 0)


if __name__ == "__main__":
    main()
