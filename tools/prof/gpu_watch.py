"""Record NVIDIA GPU counters; one CSV row per GPU and sample."""

import argparse
import csv
import subprocess
import time
import uuid
from pathlib import Path

from prof_common import data_dir, positive_float, writable_target


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("seconds", nargs="?", type=positive_float, default=75)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    output = writable_target(args.output or data_dir() / f"gpu-{uuid.uuid4().hex[:12]}.csv")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["seconds", "gpu", "util", "sm_clk", "mem_clk", "temp", "mem_used_mib"])
        start = time.monotonic()
        while time.monotonic() - start < args.seconds:
            result = subprocess.run(
                [
                    "nvidia-smi",
                    "--query-gpu=index,utilization.gpu,clocks.sm,clocks.mem,temperature.gpu,memory.used",
                    "--format=csv,noheader,nounits",
                ],
                capture_output=True,
                text=True,
                check=True,
                timeout=10,
            )
            rows = list(csv.reader(result.stdout.splitlines()))
            if not rows or any(len(row) != 6 for row in rows):
                raise ValueError("Unexpected nvidia-smi output")
            for row in rows:
                writer.writerow([round(time.monotonic() - start, 2), *(v.strip() for v in row)])
            stream.flush()
            time.sleep(min(1, max(0, args.seconds - (time.monotonic() - start))))
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
