"""Capture loading screens into a unique run directory (no FPS conclusion)."""

import argparse
import time
import uuid

from eden_session import EdenSession, preflight, session_lock
from prof_common import data_dir, label_value, positive_float, writable_target
from shot_capture import api, find_render_window, save_shot


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", type=label_value)
    parser.add_argument("--duration", type=positive_float, default=150)
    args = parser.parse_args(argv)
    target = writable_target(data_dir() / "shots" / f"{args.label}-{uuid.uuid4().hex[:12]}")
    target.mkdir(parents=True, exist_ok=False)
    count = 0
    with session_lock():
        preflight()
        with EdenSession() as game:
            start = time.monotonic()
            resized = False
            while time.monotonic() - start < args.duration:
                game.wait(min(2, max(0, start + args.duration - time.monotonic())))
                hwnd = find_render_window(game.pid)
                if hwnd and not resized:
                    resized = bool(api()[0].SetWindowPos(hwnd, None, 50, 50, 1280, 720, 4))
                if save_shot(game.pid, target / f"t+{int(time.monotonic() - start):04d}s.png"):
                    count += 1
    print(f"{count} captures: {target}")
    return 0 if count and not game.forced else 1


if __name__ == "__main__":
    raise SystemExit(main())
