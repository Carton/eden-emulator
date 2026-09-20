"""Interleaved AB/BA pairs; retry a whole pair when either arm is invalid."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import subprocess
import sys

from prof_common import (
    HERE,
    experiment_env,
    label_value,
    parse_overrides,
    positive_float,
    positive_int,
)


def parse_result(output: str, returncode: int, label: str) -> dict | None:
    if returncode:
        return None
    lines = [
        line.removeprefix("RESULT_JSON ")
        for line in output.splitlines()
        if line.startswith("RESULT_JSON ")
    ]
    if len(lines) != 1:
        return None
    try:
        result = json.loads(lines[0])
        fps = result["fps"]
        if (
            result.get("valid") is not True
            or result.get("label") != label
            or not isinstance(fps, (int, float))
            or not math.isfinite(fps)
            or fps <= 0
        ):
            return None
        return result
    except (ValueError, KeyError, TypeError, AttributeError):
        return None


def run_one(label: str, overrides: dict[str, str], measure: float) -> dict | None:
    result = subprocess.run(
        [sys.executable, str(HERE / "bench_run.py"), label, "--measure", str(measure)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=experiment_env(overrides),
    )
    print(result.stdout, end="", flush=True)
    print(result.stderr, end="", file=sys.stderr, flush=True)
    return parse_result(result.stdout, result.returncode, label)


def pair_ratio(a: dict, b: dict) -> float:
    # Brightness is a covariate, not a license to infer scene equality from FPS.
    if a.get("luma") is None or b.get("luma") is None or abs(a["luma"] - b["luma"]) > 1:
        raise ValueError("Missing/mismatched luma: pair needs scene review")
    if a.get("luma_std", 999) > 1 or b.get("luma_std", 999) > 1:
        raise ValueError("Unstable luma: pair needs scene review")
    return b["fps"] / a["fps"]


def collect_pairs(a, b, aenv, benv, count, retries, measure, runner=run_one):
    ratios = []
    for index in range(1, count + 1):
        order = [("a", a, aenv), ("b", b, benv)]
        if index % 2 == 0:
            order.reverse()
        for attempt in range(retries + 1):
            results = {}
            for arm, label, overrides in order:
                value = runner(f"{label}-p{index}{arm}r{attempt}", overrides, measure)
                if value is None:
                    break
                results[arm] = value
            if len(results) != 2:
                continue
            try:
                ratio = pair_ratio(results["a"], results["b"])
            except ValueError as exc:
                print(f"pair {index} VOID: {exc}")
                continue
            ratios.append(ratio)
            print(f"pair {index}: B/A={ratio:.4f}")
            break
    return ratios


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--a", required=True, type=label_value)
    parser.add_argument("--b", required=True, type=label_value)
    parser.add_argument("--aenv", type=parse_overrides, default={})
    parser.add_argument("--benv", type=parse_overrides, default={})
    parser.add_argument("--pairs", type=positive_int, default=3)
    parser.add_argument("--measure", type=positive_float, default=90)
    parser.add_argument("--retries", type=int, default=1)
    args = parser.parse_args(argv)
    if args.retries < 0:
        parser.error("retries must be non-negative")
    ratios = collect_pairs(
        args.a, args.b, args.aenv, args.benv, args.pairs, args.retries, args.measure
    )
    if ratios:
        print(f"SUMMARY {len(ratios)}/{args.pairs}: B/A median={statistics.median(ratios):.4f}")
    else:
        print("SUMMARY: no valid pairs")
    return 0 if len(ratios) == args.pairs else 1


if __name__ == "__main__":
    raise SystemExit(main())
