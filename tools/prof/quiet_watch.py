"""Wait for quiet time, then run a trusted local Python sequence.

RUNS contains label/env benchmarks or label/cmd argument lists.
PAIRS contains exact labels, never independently filtered prefix lists.
Failed builds and benchmarks abort the sequence. NVIDIA Overlay is untouched.
"""

import argparse
import importlib.util
import subprocess
import time
from pathlib import Path

from bench_ab import pair_ratio, run_one
from eden_session import idle_seconds
from prof_common import label_value, positive_float, process_names


def wait_idle(threshold, minutes):
    deadline = time.monotonic() + minutes * 60
    while time.monotonic() < deadline:
        if idle_seconds() >= threshold and "eden.exe" not in process_names():
            return True
        time.sleep(min(15, max(0, deadline - time.monotonic())))
    return False


def execute(runs, pairs, resume_idle, deadline, measure):
    results = {}
    for entry in runs:
        if not wait_idle(resume_idle, deadline):
            print("No quiet window before deadline")
            return 1
        label = entry["label"]
        if "cmd" in entry:
            if subprocess.run(entry["cmd"], check=False).returncode:
                print(f"Command failed: {label}; aborting dependent work")
                return 1
        else:
            result = run_one(label, entry.get("env", {}), measure)
            if result is None:
                return 1
            results[label] = result
    for a, b in pairs:
        if a not in results or b not in results:
            raise ValueError(f"PAIRS must name exact successful benchmark labels: {a}, {b}")
        print(f"pair {a}/{b}: B/A={pair_ratio(results[a], results[b]):.4f}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sequence", type=Path)
    parser.add_argument("--idle", type=positive_float, default=180)
    parser.add_argument("--deadline", type=positive_float, default=150, help="Minutes")
    parser.add_argument("--resume-idle", type=positive_float, default=60)
    parser.add_argument("--measure", type=positive_float, default=90)
    args = parser.parse_args(argv)
    spec = importlib.util.spec_from_file_location("quiet_sequence", args.sequence)
    if spec is None or spec.loader is None:
        raise ValueError("Cannot load trusted sequence")
    seq = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(seq)
    labels = [label_value(entry["label"]) for entry in seq.RUNS]
    if len(labels) != len(set(labels)):
        raise ValueError("Sequence labels must be unique")
    for entry in seq.RUNS:
        if "cmd" in entry and (
            not isinstance(entry["cmd"], list)
            or not entry["cmd"]
            or not all(isinstance(v, str) for v in entry["cmd"])
        ):
            raise ValueError("cmd must be a nonempty argument list")
    if not wait_idle(args.idle, args.deadline):
        print("No quiet window before deadline")
        return 1
    return execute(
        seq.RUNS, getattr(seq, "PAIRS", []), args.resume_idle, args.deadline, args.measure
    )


if __name__ == "__main__":
    raise SystemExit(main())
