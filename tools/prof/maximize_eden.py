"""Maximize the render window of an explicit PID, never an unrelated Qt app."""

import argparse
import time

from prof_common import positive_float, positive_int
from shot_capture import api, find_render_window


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=positive_int)
    parser.add_argument("--timeout", type=positive_float, default=120)
    args = parser.parse_args(argv)
    end = time.monotonic() + args.timeout
    while time.monotonic() < end:
        hwnd = find_render_window(args.pid)
        if hwnd:
            api()[0].ShowWindow(hwnd, 3)
            return 0
        time.sleep(1)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
