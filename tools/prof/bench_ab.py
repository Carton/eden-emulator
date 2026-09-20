#!/usr/bin/env python3
"""Interleaved A/B benchmark driver (stable macro numbers).

Runs N back-to-back pairs through bench_run.py, alternating the order each
pair (AB, BA, AB, ...) so slow drift (thermals, background, scene state)
cancels out. The reported number is the per-pair fps ratio fps(B)/fps(A)
and its median across pairs -- NOT absolute fps, which has a +/-2-4% band
even on a quiet machine.

Usage:
  python tools/prof/bench_ab.py --pairs 3 --a golden --b token-tailimm \
      --benv "EDEN_DRAW_TOKEN=1,EDEN_TOKEN_TAIL_IMM=1"
Options:
  --aenv "K=V,K=V"    env overrides for arm A (default: none)
  --measure 90        per-run measure window seconds
  --retries 1         re-runs of a VOIDed slot before the pair is dropped
"""
import os
import re
import statistics
import subprocess
import sys
import time

BENCH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench_run.py")


def run_one(label, env_over, measure):
    env = dict(os.environ)
    for kv in [k for k in env_over.split(",") if k.strip()]:
        k, _, v = kv.partition("=")
        env[k.strip()] = v
    r = subprocess.run([sys.executable, BENCH, label, "--measure", str(measure)],
                       capture_output=True, text=True, env=env,
                       encoding="utf-8", errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    print(out, flush=True)
    m = re.search(r"^RESULT (\S+):.*?fps=([\d.]+)", out, re.M)
    if m and "VOID" not in m.group(1):
        return float(m.group(2))
    return None  # VOID / no data


def main():
    args = sys.argv[1:]
    def opt(name, default=None):
        return args[args.index(name) + 1] if name in args else default

    pairs = int(opt("--pairs", "1"))
    label_a = opt("--a", "golden")
    label_b = opt("--b", "token")
    env_a = opt("--aenv", "")
    env_b = opt("--benv", "")
    measure = opt("--measure", "90")
    retries = int(opt("--retries", "1"))

    ratios = []
    for i in range(1, pairs + 1):
        a_first = (i % 2 == 1)
        order = ((label_a, env_a, "a"), (label_b, env_b, "b")) if a_first \
            else ((label_b, env_b, "b"), (label_a, env_a, "a"))
        fps = {}
        for base, env_over, arm in order:
            for attempt in range(retries + 1):
                suffix = f"-p{i}{arm}" + (f"r{attempt}" if attempt else "")
                print(f"\n===== pair {i} arm {arm} "
                      f"({base}{suffix}) {time.strftime('%H:%M:%S')} =====", flush=True)
                v = run_one(base + suffix, env_over, measure)
                if v is not None:
                    fps[arm] = v
                    break
                print(f"pair {i} arm {arm} VOID (attempt {attempt + 1})", flush=True)
        if "a" in fps and "b" in fps:
            r = fps["b"] / fps["a"]
            ratios.append(r)
            print(f"----- pair {i}: A={fps['a']:.2f} B={fps['b']:.2f} "
                  f"ratio B/A={r:.4f}", flush=True)
        else:
            print(f"----- pair {i}: INVALID (voided arm)", flush=True)

    if ratios:
        med = statistics.median(ratios)
        print(f"\nSUMMARY: {len(ratios)}/{pairs} valid pairs, "
              f"ratio B/A median={med:.4f} range={min(ratios):.4f}-{max(ratios):.4f} "
              f"(delta={100 * (med - 1):+.1f}%)")
    else:
        print("\nSUMMARY: no valid pairs (machine busy? check RESULT lines)")
        sys.exit(1)


if __name__ == "__main__":
    main()
