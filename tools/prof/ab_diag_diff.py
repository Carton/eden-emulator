"""Compare serial-diag segment averages between two bench arm label groups.

Reads every ``<label-prefix>-p*`` run under the prof archive, keeps the last
``--tail`` diag lines of each family (steady state; shader-compile storms live
in the early windows), averages the numeric fields per run across those lines,
then reports the per-field A-vs-B ratio of run means.
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from pathlib import Path

from prof_common import data_dir, label_value

FIELD = re.compile(r"([a-z_]+)=([0-9]+)")


def parse_line(line: str) -> dict[str, int]:
    return {k: int(v) for k, v in FIELD.findall(line.split("diag:", 1)[1])}


def collect(prefix: str, family: str, tail: int) -> list[dict[str, float]]:
    rows = []
    for directory in sorted((data_dir() / "runs").glob(f"{prefix}-p*")):
        diag = directory / "diag.txt"
        if not diag.is_file():
            continue
        lines = [
            line
            for line in diag.read_text(encoding="utf-8", errors="replace").splitlines()
            if f"{family} diag:" in line
        ]
        if not lines:
            continue
        parsed = [parse_line(line) for line in lines[-tail:]]
        keys = set().union(*parsed)
        rows.append({k: statistics.mean(p.get(k, 0) for p in parsed) for k in keys})
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--a", required=True, type=label_value)
    parser.add_argument("--b", required=True, type=label_value)
    parser.add_argument("--family", default="SerialDraw phases")
    parser.add_argument("--tail", type=int, default=20)
    args = parser.parse_args(argv)
    a_rows = collect(args.a, args.family, args.tail)
    b_rows = collect(args.b, args.family, args.tail)
    if not a_rows or not b_rows:
        print(f"no diag rows: a={len(a_rows)} b={len(b_rows)}")
        return 1
    a_avg = {
        k: statistics.mean(r.get(k, 0.0) for r in a_rows) for k in set().union(*a_rows)
    }
    b_avg = {
        k: statistics.mean(r.get(k, 0.0) for r in b_rows) for k in set().union(*b_rows)
    }
    print(f"family={args.family!r} runs: a={len(a_rows)} b={len(b_rows)} tail={args.tail}")
    print(f"{'field':28} {'A_mean':>12} {'B_mean':>12} {'B/A':>7}")
    for key in sorted(set(a_avg) | set(b_avg)):
        av, bv = a_avg.get(key, 0.0), b_avg.get(key, 0.0)
        ratio = f"{bv / av:7.3f}" if av else "      -"
        print(f"{key:28} {av:12.1f} {bv:12.1f} {ratio}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
