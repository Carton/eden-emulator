"""Compare naturally ordered QA shots; calibrate thresholds with golden vs golden."""

import argparse
from pathlib import Path

from PIL import Image

from prof_common import natural_key

SMALL = (96, 54)
BAD_PIXEL_THRESHOLD = 12
PAIR_FAIL_PCT = 15.0
PAIR_WARN_PCT = 5.0


def load_small(path):
    with Image.open(path) as image:
        return list(image.convert("RGB").resize(SMALL, Image.Resampling.LANCZOS).getdata())


def pair_stats(a, b):
    pa, pb = load_small(a), load_small(b)
    deltas = [
        sum(abs(x - y) for x, y in zip(ca, cb, strict=True)) / 3
        for ca, cb in zip(pa, pb, strict=True)
    ]
    return (
        sum(deltas) / len(deltas),
        100 * sum(v > BAD_PIXEL_THRESHOLD for v in deltas) / len(deltas),
        max(deltas),
    )


def compare(a: Path, b: Path) -> str:
    sets = [sorted(path.glob("*.png"), key=natural_key) for path in (a, b)]
    if not sets[0] or len(sets[0]) != len(sets[1]):
        print("FAIL: empty or mismatched capture counts")
        return "FAIL"
    verdict = "PASS"
    for index, (left, right) in enumerate(zip(*sets, strict=True)):
        mean, bad, worst = pair_stats(left, right)
        if bad > PAIR_FAIL_PCT:
            verdict = "FAIL"
        elif bad > PAIR_WARN_PCT and verdict != "FAIL":
            verdict = "WARN"
        print(f"{index}: mean={mean:.2f} bad%={bad:.2f} worst={worst:.2f}")
    print("OVERALL:", verdict)
    return verdict


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    args = parser.parse_args(argv)
    return int(compare(args.a, args.b) == "FAIL")


if __name__ == "__main__":
    raise SystemExit(main())
