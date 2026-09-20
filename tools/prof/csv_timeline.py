"""Whole-session and tail-180s frame statistics, without running the game."""

import argparse
from pathlib import Path

from prof_common import TITLE, eden_dir, read_frames, tail_window


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logdir", nargs="?", type=Path, default=eden_dir() / "user/log")
    parser.add_argument("prefixes", nargs="*")
    args = parser.parse_args(argv)
    failed = False
    for path in sorted(args.logdir.glob(f"*_{TITLE}.csv")):
        if args.prefixes and not any(path.name.startswith(p) for p in args.prefixes):
            continue
        try:
            values = read_frames(path)
            tail = tail_window(values, 180) if sum(values) >= 180000 else values
            print(
                f"{path.name}: frames={len(values)} span={sum(values) / 1000:.1f}s "
                f"tail_fps={1000 * len(tail) / sum(tail):.2f} "
                f">30ms={sum(v > 30 for v in tail)} max={max(tail):.2f}ms"
            )
        except ValueError as exc:
            print(f"{path.name}: INVALID {exc}")
            failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
