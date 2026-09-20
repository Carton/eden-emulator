"""Shared paths, validation and data handling. Importing this module has no side effects."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
TITLE = "0100F2C0115B6000"


def data_dir() -> Path:
    return Path(os.environ.get("EDEN_PROF_DATA", r"F:\prof"))


def eden_dir() -> Path:
    return Path(os.environ.get("EDEN_DIR", REPO / "build-vs22" / "bin"))


def writable_target(path: Path) -> Path:
    """Also protects the daily installation when paths contain '..' or junctions."""
    resolved = path.resolve()
    protected = Path(r"F:\Switch\Yuzu").resolve()
    if resolved == protected or protected in resolved.parents:
        raise ValueError(f"Daily installation is read-only: {path}")
    return resolved


def label_value(value: str) -> str:
    if (
        not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,99}", value)
        or value.endswith(".")
        or value.split(".")[0].upper()
        in {
            "CON",
            "PRN",
            "AUX",
            "NUL",
            *(f"COM{i}" for i in range(1, 10)),
            *(f"LPT{i}" for i in range(1, 10)),
        }
    ):
        raise argparse.ArgumentTypeError(
            "Label must be a safe Windows filename (1-100 ASCII chars)"
        )
    return value


def positive_float(value: str) -> float:
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError("Expected a finite positive number")
    return result


def positive_int(value: str) -> int:
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("Expected a positive integer")
    return result


def experiment_env(overrides: dict[str, str]) -> dict[str, str]:
    """A/B arms must not inherit another experiment's EDEN switches."""
    paths = {"EDEN_DIR", "EDEN_NSP", "EDEN_PROF_DATA", "EDEN_PYTHON"}
    result = {k: v for k, v in os.environ.items() if not k.startswith("EDEN_") or k in paths}
    result.update(overrides)
    return result


def parse_overrides(value: str) -> dict[str, str]:
    result = {}
    for item in value.split(","):
        if not item.strip():
            continue
        key, sep, val = item.partition("=")
        key = key.strip()
        if not sep or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            raise argparse.ArgumentTypeError(f"Expected KEY=VALUE, got {item!r}")
        result[key] = val
    return result


def atomic_text(path: Path, text: str) -> None:
    path = writable_target(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="", dir=path.parent, delete=False
        ) as stream:
            temporary = Path(stream.name)
            stream.write(text)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def write_json(path: Path, value: object) -> None:
    atomic_text(path, json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def process_names() -> set[str]:
    # Capture BYTES and decode with replacement: on zh-CN Windows tasklist
    # emits GBK (a non-ASCII process/window title makes the stream non-UTF-8),
    # and a text=True reader thread that hits a decode error silently leaves
    # stdout=None (seen live: killed two quiet_watch sequences mid-poll).
    # The CSV first column (image name) is ASCII, so replacement chars in
    # later columns never affect the result.
    proc = subprocess.run(
        ["tasklist", "/FO", "CSV", "/NH"],
        capture_output=True,
        check=True,
        timeout=20,
    )
    stdout = proc.stdout.decode("utf-8", errors="replace")
    return {row[0].casefold() for row in csv.reader(stdout.splitlines()) if row}


def require_no_eden() -> None:
    if "eden.exe" in process_names():
        raise RuntimeError("An Eden instance is already running; leave it untouched and stop")


def read_frames(path: Path) -> list[float]:
    values = [float(v) for v in path.read_text(encoding="utf-8-sig").split()]
    if not values or any(not math.isfinite(v) or v <= 0 for v in values):
        raise ValueError(f"Empty, non-finite or non-positive frame data: {path}")
    return values


def tail_window(values: list[float], seconds: float) -> list[float]:
    """Return the last complete duration in chronological order."""
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("Window duration must be positive and finite")
    total = 0.0
    for index in range(len(values) - 1, -1, -1):
        total += values[index]
        if total >= seconds * 1000:
            return values[index:]
    raise ValueError(f"Insufficient frame coverage ({total / 1000:.2f}s < {seconds}s)")


def frame_summary(values: list[float]) -> dict[str, float]:
    if len(values) < 30 or any(not math.isfinite(v) or v <= 0 for v in values):
        raise ValueError("Need at least 30 valid frames")
    ordered = sorted(values)
    count = len(values)
    slow = ordered[-max(1, count // 100) :]
    return {
        "frames": count,
        "fps": 1000 * count / sum(values),
        "low1": 1000 * len(slow) / sum(slow),
        "median": ordered[count // 2],
        "p95": ordered[min(count - 1, int(0.95 * count))],
        "p99": ordered[min(count - 1, int(0.99 * count))],
    }


def snapshot_csv(directory: Path) -> dict[Path, tuple[int, int]]:
    return {p: (p.stat().st_mtime_ns, p.stat().st_size) for p in directory.glob(f"*_{TITLE}.csv")}


def fresh_csv(directory: Path, before: dict[Path, tuple[int, int]]) -> Path:
    after = snapshot_csv(directory)
    changed = [p for p, fingerprint in after.items() if before.get(p) != fingerprint]
    if len(changed) != 1:
        raise ValueError(f"Expected exactly one new/changed frame CSV, got {len(changed)}")
    return changed[0]


def natural_key(path: Path) -> list[int | str]:
    return [int(part) if part.isdigit() else part for part in re.split(r"(\d+)", path.name)]
