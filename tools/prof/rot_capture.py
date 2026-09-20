"""Rotation driver for sentinel rounds: wall-clock window map, no eden CSV dependency.

Drives the same none/right/none/left rotation windows as rot_test.py, but records
each window's wall-clock bounds to JSON immediately (survives a forced/slow eden
shutdown) and does not require eden's frame CSV: external sentinels (stutter_watch
PresentMon + WPR) own the timing data. A slow close at the end is reported as a
warning, not an error.
"""

import argparse
import subprocess
import time
import uuid

from eden_session import EdenSession, preflight, session_lock
from prof_common import HERE, data_dir, label_value, positive_int, write_json


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", type=label_value)
    parser.add_argument("--win", type=positive_int, default=20)
    parser.add_argument("--reps", type=positive_int, default=2)
    parser.add_argument("--settle", type=positive_int, default=30)
    args = parser.parse_args(argv)
    target = data_dir() / "runs" / f"{args.label}-{uuid.uuid4().hex[:12]}"
    target.mkdir(parents=True, exist_ok=False)
    markers = target / "windows.json"
    windows = []
    slow_close = False
    with session_lock():
        preflight()
        with EdenSession() as game:
            game.enter_game()
            game.wait(args.settle)
            for rep in range(args.reps):
                for direction in ("none", "right", "none", "left"):
                    start = time.time()
                    write_json(
                        markers,
                        {
                            "binary": game.identity,
                            "win_seconds": args.win,
                            "windows": windows + [
                                {
                                    "name": f"{direction}-{rep}",
                                    "start_epoch": start,
                                    "state": "running",
                                }
                            ],
                        },
                    )
                    result = subprocess.run(
                        [
                            "powershell",
                            "-NoProfile",
                            "-ExecutionPolicy",
                            "Bypass",
                            "-File",
                            str(HERE / "rot_hold.ps1"),
                            "-ProcessId",
                            str(game.pid),
                            "-Dir",
                            direction,
                            "-HoldSec",
                            str(args.win),
                        ],
                        capture_output=True,
                        text=True,
                        timeout=args.win + 15,
                    )
                    end = time.time()
                    if result.returncode or "input_ok=True" not in result.stdout:
                        raise RuntimeError(f"Rotation input failed: {result.stderr}")
                    entry = {
                        "name": f"{direction}-{rep}",
                        "start_epoch": start,
                        "end_epoch": end,
                    }
                    windows.append(entry)
                    write_json(
                        markers,
                        {
                            "binary": game.identity,
                            "win_seconds": args.win,
                            "windows": windows,
                        },
                    )
                    print(entry, flush=True)
            game.wait(5)
            close_time = time.monotonic()
            game.close()
            if game.forced or time.monotonic() - close_time > 5:
                slow_close = True
    print(f"windows={len(windows)} markers={markers}")
    if slow_close:
        print("WARNING: forced/slow shutdown; sentinel data is unaffected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
