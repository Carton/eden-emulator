"""Owned WPR sessions and a bounded JSON-based elevated capture worker."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import time
from pathlib import Path

from prof_common import label_value, positive_float, writable_target, write_json


class Wpr:
    def __init__(self, directory: Path, profiles: list[str], *, filemode: bool = False):
        self.directory = writable_target(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.env = dict(os.environ, TMP=str(self.directory), TEMP=str(self.directory))
        self.profiles = profiles
        self.filemode = filemode
        self.active = False
        self.started_epoch: float | None = None

    def _run(self, *args: str):
        result = subprocess.run(
            ["wpr.exe", *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=self.env,
            timeout=180,
        )
        with (self.directory / "wpr.log").open("a", encoding="utf-8") as stream:
            stream.write(f"{args!r}: rc={result.returncode}\n{result.stdout}\n{result.stderr}\n")
        return result

    def start(self) -> None:
        if self.active:
            raise RuntimeError("This WPR owner is already active")
        args = [value for profile in self.profiles for value in ("-start", profile)]
        if self.filemode:
            args.extend(["-filemode", "-recordtempto", str(self.directory)])
        result = self._run(*args)
        if result.returncode:
            raise RuntimeError(
                f"WPR start failed; existing sessions were left untouched: {result.stderr}"
            )
        self.active = True
        self.started_epoch = time.time()

    def stop(self, path: Path) -> None:
        if not self.active:
            raise RuntimeError("No WPR session owned by this process")
        path = writable_target(path)
        if path.exists():
            raise FileExistsError(path)
        result = self._run("-stop", str(path))
        if result.returncode:
            raise RuntimeError(f"WPR stop failed: {result.stderr}")
        self.active = False
        if not path.exists() or not path.stat().st_size:
            raise RuntimeError(f"WPR returned success without trace data: {path}")

    def cancel(self) -> None:
        if self.active:
            result = self._run("-cancel")
            if result.returncode:
                raise RuntimeError(f"Failed to cancel owned WPR session: {result.stderr}")
            self.active = False


def capture(directory: Path, name: str, seconds: float, profiles: list[str]) -> None:
    label_value(name)
    if not 0 < seconds <= 3600:
        raise ValueError("Capture duration must be within (0, 3600]")
    wpr = Wpr(directory / "tmp", profiles, filemode=True)
    try:
        wpr.start()
        time.sleep(seconds)
        wpr.stop(directory / f"{name}.etl")
    finally:
        wpr.cancel()


def serve(directory: Path) -> int:
    directory = writable_target(directory)
    # This session directory is newly allocated by the parent. No .cmd files run.
    write_json(directory / "ready.json", {"pid": os.getpid()})
    seen = set()
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline and not (directory / "stop").exists():
        for request in sorted(directory.glob("request-*.json")):
            if request.name in seen:
                continue
            seen.add(request.name)
            response = directory / request.name.replace("request-", "response-", 1)
            try:
                value = json.loads(request.read_text(encoding="utf-8"))
                if set(value) != {"name", "seconds"}:
                    raise ValueError("Unexpected request fields")
                capture(directory, value["name"], value["seconds"], ["CPU"])
                write_json(response, {"ok": True, "etl": f"{value['name']}.etl"})
            except (
                OSError,
                ValueError,
                RuntimeError,
                TypeError,
                subprocess.SubprocessError,
            ) as exc:
                write_json(response, {"ok": False, "error": str(exc)})
                return 1
            deadline = time.monotonic() + 600
        time.sleep(0.25)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--name", type=label_value, default="capture")
    parser.add_argument("--seconds", type=positive_float, default=100)
    parser.add_argument("--with-gpu", action="store_true")
    args = parser.parse_args(argv)
    if args.serve:
        return serve(args.directory)
    capture(args.directory, args.name, args.seconds, ["CPU", "GPU"] if args.with_gpu else ["CPU"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
