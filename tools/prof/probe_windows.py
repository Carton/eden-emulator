"""Probe visible windows of one PID using the same capture implementation as QA."""

import argparse
import uuid
from pathlib import Path

from PIL import ImageStat

from prof_common import data_dir, positive_int, writable_target
from shot_capture import capture_hwnd, windows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=positive_int)
    parser.add_argument("--outdir", type=Path)
    args = parser.parse_args(argv)
    target = writable_target(args.outdir or data_dir() / "shots" / f"probe-{uuid.uuid4().hex[:12]}")
    target.mkdir(parents=True, exist_ok=False)
    count = 0
    for hwnd, title in windows(args.pid, children=True):
        for flags in (2, 3):
            image = capture_hwnd(hwnd, flags)
            if image is None:
                print(f"{hwnd:x} {title!r}: capture failed")
                continue
            image.save(target / f"{hwnd:x}-{flags}.png")
            stats = ImageStat.Stat(image.convert("L"))
            print(f"{hwnd:x} {title!r}: mean={stats.mean[0]:.1f} std={stats.stddev[0]:.1f}")
            count += 1
    return 0 if count else 1


if __name__ == "__main__":
    raise SystemExit(main())
