"""Capture the actual game render surface (not the perf overlay).

The render child is the largest 'eden'-titled visible window of the eden pid;
the perf overlay ('Form') and title-bar strips are skipped by size. Use for
graphics-path QA where the 225x250 overlay capture proves nothing.
"""

from __future__ import annotations

import argparse
import uuid

from eden_session import EdenSession, preflight, session_lock
from prof_common import data_dir, label_value, positive_float, positive_int, writable_target
from shot_capture import capture_hwnd, find_render_window


def render_hwnd(pid: int) -> int:
    return find_render_window(pid) or 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", type=label_value)
    parser.add_argument("--count", type=positive_int, default=3)
    parser.add_argument("--gap", type=positive_float, default=5.0)
    args = parser.parse_args(argv)
    target = writable_target(data_dir() / "shots" / f"{args.label}-{uuid.uuid4().hex[:12]}")
    target.mkdir(parents=True, exist_ok=False)
    saved = 0
    with session_lock():
        preflight()
        with EdenSession() as game:
            game.enter_game()
            for index in range(args.count):
                hwnd = render_hwnd(game.pid)
                image = capture_hwnd(hwnd) if hwnd else None
                if image is not None:
                    image.save(target / f"game{index:02d}.png", "PNG")
                    saved += 1
                game.wait(args.gap)
    print(f"RESULT {args.label}: saved {saved}/{args.count} -> {target}")
    return 0 if saved == args.count and not game.forced else 1


if __name__ == "__main__":
    raise SystemExit(main())
