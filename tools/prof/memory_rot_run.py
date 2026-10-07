"""Bounded TOTK save entry, verified camera rotation and same-PID memory sampling."""

from __future__ import annotations

import argparse
import csv
import json
import shutil
import statistics
import subprocess
import sys
import time
import uuid
from datetime import UTC, datetime, timedelta
from itertools import pairwise
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

from eden_session import EdenSession, preflight, session_lock
from prof_common import (
    HERE,
    REPO,
    data_dir,
    eden_dir,
    game_path,
    positive_int,
    save_slot,
    writable_target,
    write_json,
)
from save_fixture import temporary_slot
from shot_capture import save_shot


def log(message: str) -> None:
    print(f"[{datetime.now(UTC).strftime('%H:%M:%S')}] {message}", flush=True)


def gameplay_hud(path: Path) -> bool:
    """TOTK red-heart HUD heuristic; evidence still needs human scene review."""
    with Image.open(path) as image:
        thumb = image.convert("RGB").resize((640, 360))
        pixels = thumb.crop((20, 12, 250, 55)).get_flattened_data()
        red = sum(r > 140 and r > g * 1.6 and r > b * 1.6 for r, g, b in pixels)
    return red >= 25


def scene_diff(a: Path, b: Path) -> float:
    thumbs = []
    for path in (a, b):
        with Image.open(path) as image:
            thumbs.append(image.convert("RGB").resize((256, 144)).crop((45, 35, 230, 120)))
    return sum(ImageStat.Stat(ImageChops.difference(*thumbs)).mean) / (3 * 255)


def rot_window(pid: int, direction: str, seconds: int) -> None:
    result = subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(HERE / "rot_hold.ps1"),
            "-ProcessId",
            str(pid),
            "-Dir",
            direction,
            "-HoldSec",
            str(seconds),
        ],
        capture_output=True,
        text=True,
        errors="replace",
        timeout=seconds + 30,
    )
    if result.returncode or "input_ok=True" not in result.stdout:
        raise RuntimeError(f"Rotation input failed: {result.stdout} {result.stderr}")


def phase_summary(rows: list[dict], phases: list[dict]) -> list[dict]:
    summary = []
    for start, end in pairwise(phases):
        points = [row for row in rows if start["utc"] <= row["utc"] < end["utc"]]
        if len(points) < 2:
            continue
        times = [float(point["elapsed_seconds"]) for point in points]
        values = [int(point["private_bytes"]) / 1048576 for point in points]
        mean_t, mean_v = statistics.mean(times), statistics.mean(values)
        denominator = sum((t - mean_t) ** 2 for t in times)
        slope = (
            sum((t - mean_t) * (v - mean_v) for t, v in zip(times, values, strict=True))
            / denominator
        )
        summary.append(
            {
                "phase": start["phase"],
                "samples": len(points),
                "first_private_mib": values[0],
                "last_private_mib": values[-1],
                "delta_mib": values[-1] - values[0],
                "slope_mib_per_min": slope * 60,
            }
        )
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output", type=Path, help="New run directory; defaults to data_dir/memory-rot-ID"
    )
    parser.add_argument("--shots", type=Path, help="Optional separate new screenshot directory")
    parser.add_argument("--baseline-sec", type=positive_int, default=120)
    parser.add_argument("--rot-win", type=positive_int, default=20)
    parser.add_argument("--rot-count", type=positive_int, default=60)
    parser.add_argument("--tail-sec", type=positive_int, default=300)
    parser.add_argument("--entry-timeout", "--load-wait-sec", type=positive_int, default=240)
    parser.add_argument("--interval", type=positive_int, default=5)
    parser.add_argument(
        "--watch-seconds", type=positive_int, help="Must cover entry and all phases"
    )
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument(
        "--await-scene",
        action="store_true",
        help="Allow agent positioning before probe; wait for run/scene-ready.json",
    )
    parser.add_argument("--scene-timeout", type=positive_int, default=300)
    args = parser.parse_args(argv)
    required = (
        args.entry_timeout
        + 90
        + args.baseline_sec
        + args.rot_count * (args.rot_win + 5)
        + args.tail_sec
        + 120
        + (args.scene_timeout if args.await_scene else 0)
    )
    if args.watch_seconds and args.watch_seconds < required:
        parser.error(f"--watch-seconds must be >= {required}; omit to compute automatically")
    target = writable_target(args.output or data_dir() / f"memory-rot-{uuid.uuid4().hex[:12]}")
    shots = writable_target(args.shots or target / "shots")
    phases: list[dict] = []
    record = {
        "valid": False,
        "phases": phases,
        "args": vars(args) | {"output": str(target), "shots": str(shots)},
        "paths": {"repo": str(REPO), "eden_dir": str(eden_dir()), "game": str(game_path())},
        "scene_validation": "TOTK HUD heuristic plus camera response; review shots for pond save",
        "scene_review": "pending",
    }
    record["args"] = {
        key: str(value) if isinstance(value, Path) else value
        for key, value in record["args"].items()
    }
    watcher = None
    game = None

    def persist():
        write_json(target / "result.json", record)

    def mark(name):
        phases.append(
            {"phase": name, "utc": datetime.now(UTC).isoformat(), "monotonic": time.monotonic()}
        )
        persist()
        log(name)

    def check_sampler():
        if watcher is not None and watcher.poll() is not None:
            raise RuntimeError(
                f"Memory sampler ended before test finished: rc={watcher.returncode}; see watcher.log"
            )
        csv_path = target / "memory/memory.csv"
        if csv_path.is_file():
            with csv_path.open(encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            if rows:
                row = rows[-1]
                age = (datetime.now(UTC) - datetime.fromisoformat(row["utc"])).total_seconds()
                if age > max(45, 3 * args.interval):
                    raise RuntimeError(f"Memory samples are stale ({age:.1f}s); see watcher.log")
                headroom = int(row["system_commit_available_bytes"]) / 1073741824
                log(
                    f"private={int(row['private_bytes']) / 1073741824:.3f} GiB; system commit headroom={headroom:.2f} GiB"
                )
                if headroom < 2:
                    raise RuntimeError(
                        "System commit headroom <2 GiB; stopping owned test before exhaustion"
                    )

    def shot(name):
        path = shots / f"{name}.png"
        if not save_shot(game.pid, path):
            raise RuntimeError(f"Required render capture failed: {path}")
        return path

    with session_lock():
        preflight(input_only=True)
        if args.prepare_only:
            print(json.dumps(record["paths"], indent=2), flush=True)
            return 0
        target.mkdir(parents=True, exist_ok=False)
        shots.mkdir(parents=True, exist_ok=False)
        try:
            config_dir = eden_dir() / "user/config"
            shutil.copy2(config_dir / "qt-config.ini", target / "qt-config.start.ini")
            custom = config_dir / "custom/0100F2C0115B6000.ini"
            if custom.is_file():
                shutil.copy2(custom, target / "totk-config.start.ini")
            record["source_head"] = subprocess.run(
                ["git", "-C", str(REPO), "rev-parse", "HEAD"],
                capture_output=True,
                text=True,
                check=True,
                timeout=10,
            ).stdout.strip()
            with (target / "watcher.log").open("x", encoding="utf-8") as watcher_log:  # noqa: SIM117
                with (
                    temporary_slot(eden_dir(), save_slot(), target / "save-backup") as fixture,
                    EdenSession() as game,
                ):
                    record["save_fixture"] = fixture
                    record["binary"] = game.identity
                    record["pid"] = game.pid
                    mark("boot")
                    watcher = subprocess.Popen(
                        [
                            sys.executable,
                            str(HERE / "memory_watch.py"),
                            "--pid",
                            str(game.pid),
                            "--seconds",
                            str(args.watch_seconds or required),
                            "--interval",
                            str(args.interval),
                            "--output",
                            str(target / "memory"),
                        ],
                        stdout=watcher_log,
                        stderr=subprocess.STDOUT,
                    )
                    deadline = time.monotonic() + args.entry_timeout
                    game.wait(30)
                    attempt = 0
                    while time.monotonic() < deadline:
                        check_sampler()
                        current = shot(f"entry_{attempt:03d}")
                        if gameplay_hud(current):
                            break
                        game.tap_a()
                        game.wait(8)
                        attempt += 1
                        log(f"entry attempt {attempt}; waiting for gameplay HUD")
                    else:
                        raise RuntimeError(
                            f"Gameplay HUD absent after {args.entry_timeout}s; inspect entry shots"
                        )
                    if args.await_scene:
                        mark("scene_setup")
                        log(
                            f"SCENE_SETUP PID={game.pid}; waiting for {target / 'scene-ready.json'}"
                        )
                        scene_deadline = time.monotonic() + args.scene_timeout
                        ready = target / "scene-ready.json"
                        while not ready.is_file():
                            if time.monotonic() >= scene_deadline:
                                raise RuntimeError(f"Scene setup exceeded {args.scene_timeout}s")
                            game.wait(1)
                        record["scene_review"] = json.loads(ready.read_text(encoding="utf-8"))
                    mark("camera_probe")
                    before = shot("probe_before")
                    rot_window(game.pid, "right", 2)
                    after = shot("probe_after")
                    response = scene_diff(before, after)
                    record["camera_response"] = response
                    if response < 0.025 or not gameplay_hud(after):
                        raise RuntimeError(
                            f"Camera probe did not confirm gameplay: diff={response:.4f}"
                        )
                    mark("warmup")
                    for direction in ("right", "left"):
                        rot_window(game.pid, direction, args.rot_win)
                        check_sampler()
                    shot("warmup_done")
                    mark("baseline")
                    game.wait(args.baseline_sec)
                    check_sampler()
                    shot("baseline_done")
                    for index in range(args.rot_count):
                        direction = "right" if index % 2 == 0 else "left"
                        mark(f"rotation_{index:03d}_{direction}")
                        rot_window(game.pid, direction, args.rot_win)
                        check_sampler()
                        if index % 5 == 0 or index == args.rot_count - 1:
                            shot(f"rotation_{index:03d}_{direction}")
                    mark("tail_idle")
                    remaining = args.tail_sec
                    while remaining:
                        chunk = min(30, remaining)
                        game.wait(chunk)
                        remaining -= chunk
                        check_sampler()
                        shot(f"tail_{args.tail_sec - remaining:03d}")
                    mark("measurement_done")
                    game.close()
                    record["forced_close"] = game.forced
                    watcher.wait(timeout=20)
                    if watcher.returncode:
                        raise RuntimeError(
                            f"Memory sampler failed ({watcher.returncode}); see watcher.log"
                        )
                    metadata = json.loads(
                        (target / "memory/metadata.json").read_text(encoding="utf-8")
                    )
                    if metadata["status"] != "target_exited":
                        raise RuntimeError(f"Sampler coverage invalid: {metadata['status']}")
                    with (target / "memory/memory.csv").open(encoding="utf-8") as stream:
                        points = list(csv.DictReader(stream))
                    if len(points) < 2:
                        raise RuntimeError("Sampler produced insufficient memory data")
                    covered_start = datetime.fromisoformat(points[0]["utc"])
                    covered_end = datetime.fromisoformat(points[-1]["utc"])
                    if covered_start > datetime.fromisoformat(phases[0]["utc"]) + timedelta(
                        seconds=args.interval * 2
                    ):
                        raise RuntimeError("Sampler missed the beginning of the run")
                    if covered_end < datetime.fromisoformat(phases[-1]["utc"]) - timedelta(
                        seconds=args.interval * 2
                    ):
                        raise RuntimeError("Sampler missed the end of measurement")
                    record["valid"] = True
        except BaseException as error:
            record["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            if watcher is not None and watcher.poll() is None:
                try:
                    watcher.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    watcher.terminate()
                    watcher.wait(timeout=10)
                    record["sampler_forced_cleanup"] = True
            if game is not None:
                record["forced_close"] = game.forced
            path = target / "memory/memory.csv"
            if path.is_file():
                with path.open(encoding="utf-8") as stream:
                    record["memory_phases"] = phase_summary(list(csv.DictReader(stream)), phases)
            persist()
    print("RESULT_JSON " + str(target / "result.json"), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
