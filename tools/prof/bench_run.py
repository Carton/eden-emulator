"""Run one GUI benchmark; invalid runs never produce a successful RESULT_JSON."""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import time
import uuid

from PIL import Image, ImageStat

from eden_session import EdenSession, input_tick, preflight, session_lock
from prof_common import (
    REPO,
    data_dir,
    eden_dir,
    frame_summary,
    fresh_csv,
    label_value,
    positive_float,
    read_frames,
    snapshot_csv,
    tail_window,
    writable_target,
    write_json,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", type=label_value)
    parser.add_argument("--measure", type=positive_float, default=90.0)
    parser.add_argument("--hold", type=positive_float)
    parser.add_argument("--no-tap", action="store_true")
    parser.add_argument("--no-shots", action="store_true")
    parser.add_argument("--tolerate-overlay", action="store_true", help="Compatibility no-op")
    parser.add_argument("--affinity", type=lambda value: int(value, 0))
    args = parser.parse_args(argv)
    duration = args.hold or args.measure
    run_id = f"{args.label}-{uuid.uuid4().hex[:12]}"
    directory = writable_target(data_dir() / "runs" / run_id)
    directory.mkdir(parents=True, exist_ok=False)
    record = {"label": args.label, "run_id": run_id, "valid": False}
    try:
        with session_lock():
            preflight()
            before = snapshot_csv(eden_dir() / "user/log")
            with EdenSession() as game:
                record["binary"] = game.identity
                record["source_head"] = subprocess.run(
                    ["git", "-C", str(REPO), "rev-parse", "HEAD"],
                    capture_output=True,
                    text=True,
                    check=True,
                    timeout=10,
                ).stdout.strip()
                # Source checkout and actual executable identity are deliberately separate.
                if args.affinity is not None:
                    if args.affinity <= 0:
                        raise ValueError("Affinity must be a positive bitmask")
                    subprocess.run(
                        [
                            "powershell",
                            "-NoProfile",
                            "-Command",
                            f"(Get-Process -Id {game.pid}).ProcessorAffinity = {args.affinity}",
                        ],
                        check=True,
                        timeout=15,
                    )
                game.enter_game(no_tap=args.no_tap)
                mark = input_tick()
                started = time.monotonic()
                shot_times = []
                if not args.no_shots and not args.hold:
                    shot_at = min(5.0, duration / 2)
                    while shot_at < duration:
                        game.wait(max(0, started + shot_at - time.monotonic()))
                        from shot_capture import save_shot

                        target = directory / "shots" / f"{len(shot_times):03d}.png"
                        if not save_shot(game.pid, target):
                            raise RuntimeError("Required QA capture failed; run is VOID")
                        shot_times.append(time.monotonic() - started)
                        shot_at += 15
                game.wait(max(0, started + duration - time.monotonic()))
                if input_tick() != mark:
                    raise RuntimeError("User input during measurement: run is VOID")
                record["shot_times"] = shot_times
                record["measurement_seconds"] = time.monotonic() - started
                close_start = time.monotonic()
                game.close()
                record["close_seconds"] = time.monotonic() - close_start
                if game.forced:
                    raise RuntimeError("Forced shutdown: frame CSV is unreliable")
            if args.hold:
                record["mode"] = "hold"
                print("HOLD DONE")
                return 0
            path = fresh_csv(eden_dir() / "user/log", before)
            # Frame CSV has no absolute timestamps. These remain tail estimates;
            # reject long closes which could shift the measured window materially.
            # 25s bar (2026-09-20): TOTK teardown of a ~16GB session measured
            # 15.9-16.5s on the fix lineage (runs fix-check-c654/096e), so 5s
            # voided 100% of real runs; 25s still catches the pathological
            # 60s-close known issue. Close seconds are recorded per-run.
            if record["close_seconds"] > 25:
                raise RuntimeError("Close took >25s; cannot align untimestamped frame CSV")
            window = tail_window(read_frames(path), duration)
            summary = frame_summary(window)
            if summary["fps"] > 52 or summary["median"] < 20:
                raise ValueError("Menu/loading signature, not pond gameplay")
            tail = tail_window(window, max(duration - 15, min(duration, 1.0)))
            summary["tail_fps"] = 1000 * len(tail) / sum(tail)
            means = []
            for shot in sorted((directory / "shots").glob("*.png")):
                with Image.open(shot) as image:
                    means.append(ImageStat.Stat(image.convert("L").resize((96, 54))).mean[0])
            record.update(summary)
            record["luma"] = statistics.mean(means) if means else None
            record["luma_std"] = statistics.pstdev(means) if means else None
            record["csv"] = str(path)
            # Archive the exact source CSV before the next run can overwrite it.
            (directory / "frames.csv").write_bytes(path.read_bytes())
            log_path = eden_dir() / "user/log/eden_log.txt"
            if log_path.exists():
                lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
                (directory / "diag.txt").write_text(
                    "\n".join(line for line in lines if "diag" in line.lower()) + "\n",
                    encoding="utf-8",
                )
            record["valid"] = True
            write_json(directory / "result.json", record)
            print(f"RESULT {args.label}: fps={summary['fps']:.2f} med={summary['median']:.2f}ms")
            print("RESULT_JSON " + json.dumps(record))
            return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        record["error"] = str(exc)
        print(f"RESULT VOID {args.label}: {exc}")
        return 1
    finally:
        write_json(directory / "result.json", record)


if __name__ == "__main__":
    raise SystemExit(main())
