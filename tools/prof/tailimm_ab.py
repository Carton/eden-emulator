"""Back-to-back ETW A/B via one elevated helper and atomic JSON requests."""

import argparse
import json
import subprocess
import sys
import time
import uuid

from eden_session import EdenSession, preflight, session_lock
from prof_common import HERE, atomic_text, data_dir, experiment_env, positive_float, write_json


def ps_quote(value):
    return "'" + value.replace("'", "''") + "'"


def await_file(path, timeout, game=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            return json.loads(path.read_text(encoding="utf-8"))
        if game is not None:
            game.wait(0.5)
        else:
            time.sleep(0.5)
    raise TimeoutError(f"Timed out waiting for {path}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=positive_float, default=40)
    args = parser.parse_args(argv)
    if args.seconds > 3600:
        parser.error("seconds must be <=3600")
    with session_lock():
        preflight()
        directory = data_dir().resolve() / "traces" / uuid.uuid4().hex
        directory.mkdir(parents=True, exist_ok=False)
        arguments = subprocess.list2cmdline(
            [str(HERE / "wpr_capture.py"), str(directory), "--serve"]
        )
        command = (
            f"Start-Process -Verb RunAs -WindowStyle Hidden -FilePath {ps_quote(sys.executable)} "
            f"-ArgumentList {ps_quote(arguments)} -ErrorAction Stop"
        )
        subprocess.run(["powershell", "-NoProfile", "-Command", command], check=True, timeout=120)
        try:
            await_file(directory / "ready.json", 120)
            phases = [
                ("tailimm_off", {}),
                ("tailimm_on", {"EDEN_DRAW_TOKEN": "1", "EDEN_TOKEN_TAIL_IMM": "1"}),
            ]
            for index, (name, overrides) in enumerate(phases):
                with EdenSession(experiment_env(overrides)) as game:
                    game.enter_game()
                    # Publish only after startup; a unique directory excludes stale requests.
                    write_json(
                        directory / f"request-{index}.json",
                        {
                            "name": name,
                            "seconds": args.seconds,
                        },
                    )
                    result = await_file(
                        directory / f"response-{index}.json", args.seconds + 400, game
                    )
                    if not result.get("ok"):
                        raise RuntimeError(result.get("error", "Capture failed"))
                    write_json(directory / f"{name}-binary.json", game.identity)
                if game.forced:
                    raise RuntimeError("Eden shutdown required force")
            print(f"Traces: {directory}; normalize ETW by draw count before comparison")
        finally:
            atomic_text(directory / "stop", "stop\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
