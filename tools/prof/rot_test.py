"""Rotation test with PID-targeted input and approximate CSV window alignment."""

import argparse
import subprocess
import time
import uuid

from eden_session import EdenSession, preflight, session_lock
from prof_common import (
    HERE,
    data_dir,
    eden_dir,
    frame_summary,
    fresh_csv,
    label_value,
    positive_int,
    read_frames,
    snapshot_csv,
    write_json,
)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", type=label_value)
    parser.add_argument("--win", type=positive_int, default=20)
    parser.add_argument("--reps", type=positive_int, default=2)
    args = parser.parse_args(argv)
    target = data_dir() / "runs" / f"{args.label}-{uuid.uuid4().hex[:12]}"
    windows = []
    with session_lock():
        preflight()
        before = snapshot_csv(eden_dir() / "user/log")
        with EdenSession() as game:
            game.enter_game()
            game.wait(30)
            for rep in range(args.reps):
                for direction in ("none", "right", "none", "left"):
                    start = time.monotonic()
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
                    if result.returncode or "input_ok=True" not in result.stdout:
                        raise RuntimeError(f"Rotation input failed: {result.stderr}")
                    windows.append((f"{direction}-{rep}", start, time.monotonic()))
            game.wait(5)
            close_time = time.monotonic()
            game.close()
            if game.forced or time.monotonic() - close_time > 5:
                raise RuntimeError("Cannot align CSV after forced/slow shutdown")
        path = fresh_csv(eden_dir() / "user/log", before)
        frames = read_frames(path)
        write_json(
            target / "windows.json",
            {
                "windows": windows,
                "close_monotonic": close_time,
                "binary": game.identity,
                "alignment": "approximate; CSV has no wall clock",
            },
        )
        (target / "frames.csv").write_bytes(path.read_bytes())
        cursor = close_time - sum(frames) / 1000
        spans = []
        for frame in frames:
            spans.append((cursor, frame))
            cursor += frame / 1000
        for name, start, end in windows:
            values = [frame for at, frame in spans if start <= at < end]
            print(name, frame_summary(values))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
