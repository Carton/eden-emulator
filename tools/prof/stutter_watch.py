"""Monitor PresentMon frames and freeze an owned WPR ring on severe stutters.

Run elevated. Existing WPR/PresentMon sessions are never cancelled at startup.
CPU-only ring is the default; all WPR temporary files stay in the output directory.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import queue
import statistics
import subprocess
import threading
import time
import uuid
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from prof_common import data_dir, positive_float, positive_int, writable_target, write_json
from wpr_capture import Wpr


def find_col(header, *candidates, fuzzy=()):
    names = [name.strip().lower() for name in header]
    for candidate in candidates:
        if candidate.lower() in names:
            return names.index(candidate.lower())
    return next((i for part in fuzzy for i, name in enumerate(names) if part.lower() in name), -1)


class Detector:
    """Pure trigger state. A reserved capture consumes quota before background work starts."""

    def __init__(self, severe, ratio, cooldown, minimum, maximum):
        self.severe, self.ratio, self.cooldown = severe, ratio, cooldown
        self.minimum, self.maximum = minimum, maximum
        self.history = deque(maxlen=max(120, minimum))
        self.last = -math.inf
        self.captures = 0

    def observe(self, ms, now, *, armed=True, busy=False):
        if not math.isfinite(ms) or ms <= 0:
            return None
        baseline = statistics.median(self.history) if self.history else ms
        ready = len(self.history) >= self.minimum
        self.history.append(ms)
        if (
            not armed
            or busy
            or not ready
            or self.captures >= self.maximum
            or now - self.last < self.cooldown
            or ms < self.severe
            or ms < self.ratio * baseline
        ):
            return None
        self.last = now
        self.captures += 1
        return {"frame_ms": ms, "median_ms": baseline, "recent_ms": list(self.history)[-30:]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--process-name", default="eden")
    parser.add_argument("--severe-ms", type=positive_float, default=50)
    parser.add_argument("--ratio", type=positive_float, default=1.6)
    parser.add_argument("--cooldown", type=float, default=5)
    parser.add_argument("--max-captures", type=positive_int, default=4)
    parser.add_argument("--min-history", type=positive_int, default=60)
    parser.add_argument("--start-delay", type=float, default=15)
    parser.add_argument("--duration", type=float, default=0)
    parser.add_argument("--outdir", type=Path, default=data_dir() / "stutters")
    parser.add_argument("--with-gpu", action="store_true")
    parser.add_argument("--no-manual-flush", action="store_true")
    args = parser.parse_args(argv)
    if any(not math.isfinite(v) or v < 0 for v in (args.cooldown, args.start_delay, args.duration)):
        parser.error("cooldown/start-delay/duration must be finite and non-negative")
    if args.min_history > 100000:
        parser.error("min-history must be <=100000")
    directory = writable_target(args.outdir / uuid.uuid4().hex)
    directory.mkdir(parents=True, exist_ok=False)
    pm_exe = os.environ.get("PM_EXE", r"G:\Tools\PresentMon\PresentMon-2.5.1-x64.exe")
    wpr = Wpr(directory / "tmp", ["CPU", "GPU"] if args.with_gpu else ["CPU"])
    detector = Detector(
        args.severe_ms, args.ratio, args.cooldown, args.min_history, args.max_captures
    )
    lines = queue.Queue(maxsize=100000)
    stopping = threading.Event()
    overflow = threading.Event()
    future = None
    pm = reader = None
    started = time.monotonic()
    column = -1
    frames = 0

    def read_pipe():
        for line in pm.stdout:
            while not stopping.is_set():
                try:
                    lines.put(line, timeout=0.25)
                    break
                except queue.Full:
                    overflow.set()
            if stopping.is_set():
                break

    def save_capture(context, index):
        path = directory / f"stutter-{index}.etl"
        wpr.stop(path)
        context["etl"] = str(path)
        write_json(path.with_suffix(".json"), context)
        # Never restart here: only the main thread decides whether to re-arm.

    with (
        (directory / "pm_console.log").open("w", encoding="utf-8") as errors,
        (directory / "pm_live.csv").open("w", encoding="utf-8", newline="") as output,
        ThreadPoolExecutor(max_workers=1) as executor,
    ):
        try:
            wpr.start()
            pm = subprocess.Popen(
                [
                    pm_exe,
                    "--process_name",
                    args.process_name,
                    "--output_stdout",
                    "--qpc_time",
                    "--no_console_stats",
                    "--session_name",
                    f"EdenProf-{uuid.uuid4().hex}",
                ],
                stdout=subprocess.PIPE,
                stderr=errors,
                text=True,
                encoding="utf-8",
                errors="replace",
                creationflags=0x08000000 if os.name == "nt" else 0,
            )
            reader = threading.Thread(target=read_pipe)
            reader.start()
            while not args.duration or time.monotonic() - started < args.duration:
                if overflow.is_set():
                    raise RuntimeError("PresentMon queue overflow: timing analysis is invalid")
                if future is not None and future.done():
                    future.result()
                    future = None
                    if detector.captures < args.max_captures:
                        wpr.start()
                try:
                    line = lines.get(timeout=0.25)
                except queue.Empty:
                    if pm.poll() is not None:
                        raise RuntimeError(
                            f"PresentMon exited ({pm.returncode}); see {directory}"
                        ) from None
                    continue
                output.write(line)
                parts = next(csv.reader([line]))
                if column < 0:
                    column = find_col(parts, "MsBetweenPresents")
                    continue
                try:
                    ms = float(parts[column])
                except (ValueError, IndexError):
                    continue
                if math.isfinite(ms) and ms > 0:
                    frames += 1
                context = detector.observe(
                    ms,
                    time.monotonic(),
                    armed=time.monotonic() - started >= args.start_delay and wpr.active,
                    busy=future is not None,
                )
                if context is not None:
                    context.update(
                        {
                            "detected_epoch": time.time(),
                            "wpr_start_epoch": wpr.started_epoch,
                            "row": parts,
                        }
                    )
                    future = executor.submit(save_capture, context, detector.captures)
                    print(
                        f"Capture {detector.captures}/{args.max_captures}: {ms:.1f}ms", flush=True
                    )
        except KeyboardInterrupt:
            print("Stopping capture")
        finally:
            stopping.set()
            if pm is not None:
                if pm.poll() is None:
                    pm.terminate()
                    try:
                        pm.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        pm.kill()
                        pm.wait(timeout=5)
                if reader is not None:
                    reader.join()
                if pm.stdout is not None:
                    pm.stdout.close()
            # Serialize with the in-flight stop. WPR subprocesses have bounded timeouts.
            try:
                if future is not None:
                    future.result()
                if wpr.active and not args.no_manual_flush:
                    wpr.stop(directory / "manual.etl")
            finally:
                wpr.cancel()
    if frames == 0:
        raise RuntimeError("No valid PresentMon frames captured")
    print(f"{frames} frames, {detector.captures} captures: {directory}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
