"""Hold a preflighted GUI session for visual inspection; no performance result."""

import argparse

from eden_session import EdenSession, preflight, session_lock
from prof_common import positive_float


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hold", type=positive_float, default=150)
    args = parser.parse_args(argv)
    with session_lock():
        preflight()
        with EdenSession() as game:
            game.enter_game()
            game.wait(args.hold)
    return int(game.forced)


if __name__ == "__main__":
    raise SystemExit(main())
