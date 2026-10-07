"""Summarize and plot a completed, validated rotation memory run."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from datetime import datetime
from pathlib import Path

from prof_common import writable_target, write_json


def segment(points: list[dict], start: str, end: str) -> dict:
    selected = [point for point in points if start <= point["utc"] < end]
    if len(selected) < 2:
        raise ValueError("At least two samples are required in each measurement segment")
    times = [float(point["elapsed_seconds"]) for point in selected]
    values = [int(point["private_bytes"]) / 1048576 for point in selected]
    mean_t, mean_v = statistics.mean(times), statistics.mean(values)
    duration = times[-1] - times[0]
    if duration <= 0:
        raise ValueError("Memory sample timestamps must advance")
    slope = sum((t - mean_t) * (v - mean_v) for t, v in zip(times, values, strict=True)) / sum(
        (t - mean_t) ** 2 for t in times
    )
    return {
        "samples": len(selected),
        "minutes": duration / 60,
        "first_private_mib": values[0],
        "last_private_mib": values[-1],
        "min_private_mib": min(values),
        "max_private_mib": max(values),
        "delta_mib": values[-1] - values[0],
        "ols_slope_mib_per_min": slope * 60,
    }


def summarize(record: dict, points: list[dict]) -> dict:
    if not record.get("valid"):
        raise ValueError("Run is invalid; inspect result.json and failure evidence first")
    if record.get("scene_review") == "pending":
        raise ValueError("Scene review is pending; inspect shots and record the reviewed scene")
    phases = record["phases"]
    by_name = {phase["phase"]: phase["utc"] for phase in phases}
    rotations = [phase for phase in phases if phase["phase"].startswith("rotation_")]
    if len(rotations) != record["args"]["rot_count"]:
        raise ValueError("Rotation phase count does not match the requested run")
    start, tail, end = rotations[0]["utc"], by_name["tail_idle"], by_name["measurement_done"]
    return {
        "pid": record["pid"],
        "binary": record["binary"],
        "scene_review": record["scene_review"],
        "rotations": len(rotations),
        "baseline": segment(points, by_name["baseline"], start),
        "rotation": segment(points, start, tail),
        "rotation_last_half": segment(points, rotations[len(rotations) // 2]["utc"], tail),
        "tail_idle": segment(points, tail, end),
        "minimum_system_commit_headroom_gib": min(
            int(point["system_commit_available_bytes"]) / 1073741824
            for point in points
            if by_name["baseline"] <= point["utc"] < end
        ),
        "interpretation_limit": (
            "A finite same-scene private-commit curve does not prove absence of leaks. "
            "Retained memory can be reachable caches or allocator high watermarks; "
            "growth requires allocation ownership evidence before attributing a leak."
        ),
    }


def plot(record: dict, points: list[dict], output: Path) -> None:
    try:
        import matplotlib
    except ImportError as error:
        raise RuntimeError(
            "Install the report extra: uv sync --project tools/prof --extra report"
        ) from error
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    by_name = {phase["phase"]: phase["utc"] for phase in record["phases"]}
    measured = [point for point in points if point["utc"] < by_name["measurement_done"]]
    times = [float(point["elapsed_seconds"]) / 60 for point in measured]
    private = [int(point["private_bytes"]) / 1073741824 for point in measured]
    working = [int(point["working_set_bytes"]) / 1073741824 for point in measured]
    headroom = [int(point["system_commit_available_bytes"]) / 1073741824 for point in measured]
    steady = [i for i, point in enumerate(measured) if point["utc"] >= by_name["baseline"]]
    fig, axes = plt.subplots(3, 1, figsize=(11, 9), layout="constrained", sharex=True)
    axes[0].plot(times, private, label="Private commit", color="#215ea8")
    axes[0].plot(times, working, label="Working set", color="#79828e", alpha=0.8)
    axes[0].set_ylabel("Process memory (GiB)")
    axes[0].legend(loc="lower right")
    axes[1].plot([times[i] for i in steady], [private[i] for i in steady], color="#215ea8")
    axes[1].set_ylabel("Private commit (GiB)\nAfter warmup, zoomed scale")
    axes[2].plot(times, headroom, color="#4a865a")
    axes[2].set_ylabel("System commit headroom (GiB)")
    axes[2].set_xlabel("Minutes since memory sampling started")
    bands = [
        ("baseline", "rotation_000_right", "Baseline", "#dbe3ee"),
        ("rotation_000_right", "tail_idle", "Rotation", "#e9dfc2"),
        ("tail_idle", "measurement_done", "Idle tail", "#d7e9df"),
    ]

    def phase_time(name):
        return (
            times[0]
            + (
                datetime.fromisoformat(by_name[name]) - datetime.fromisoformat(measured[0]["utc"])
            ).total_seconds()
            / 60
        )

    for start, end, label, color in bands:
        left, right = phase_time(start), phase_time(end)
        for axis in axes:
            axis.axvspan(left, right, color=color, alpha=0.35)
        axes[1].text(
            (left + right) / 2,
            0.98,
            label,
            transform=axes[1].get_xaxis_transform(),
            ha="center",
            va="top",
        )
    for axis in axes:
        axis.grid(axis="y", alpha=0.2)
    fig.suptitle(
        f"TOTK pond rotation memory test - PID {record['pid']} ({record['args']['rot_count']} windows)"
    )
    fig.savefig(output, dpi=160)
    fig.savefig(output.with_suffix(".svg"))
    plt.close(fig)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--plot", action="store_true", help="Requires the optional report extra")
    args = parser.parse_args(argv)
    run = writable_target(args.run)
    record = json.loads((run / "result.json").read_text(encoding="utf-8"))
    with (run / "memory/memory.csv").open(encoding="utf-8") as stream:
        points = list(csv.DictReader(stream))
    report = summarize(record, points)
    write_json(run / "memory-report.json", report)
    if args.plot:
        plot(record, points, run / "memory-trend.png")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
