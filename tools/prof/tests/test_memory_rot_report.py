from datetime import UTC, datetime, timedelta

import pytest

from memory_rot_report import segment, summarize


def fixture_run():
    origin = datetime(2026, 10, 7, tzinfo=UTC)

    def utc(seconds):
        return (origin + timedelta(seconds=seconds)).isoformat()

    rows = [
        {
            "utc": utc(i),
            "elapsed_seconds": i,
            "private_bytes": (1000 + i * 100 if i < 20 else 2000) * 1048576,
            "system_commit_available_bytes": 10 * 1073741824,
        }
        for i in range(0, 101, 5)
    ]
    record = {
        "valid": True,
        "scene_review": {"scene": "verified pond"},
        "pid": 1,
        "binary": {},
        "args": {"rot_count": 2},
        "phases": [
            {"phase": name, "utc": utc(seconds)}
            for name, seconds in (
                ("boot", 0),
                ("baseline", 20),
                ("rotation_000_right", 40),
                ("rotation_001_left", 60),
                ("tail_idle", 80),
                ("measurement_done", 100),
            )
        ],
    }
    return record, rows


def test_loading_growth_does_not_enter_rotation_conclusion():
    record, rows = fixture_run()
    report = summarize(record, rows)
    assert report["rotation"]["delta_mib"] == 0
    assert report["rotation"]["ols_slope_mib_per_min"] == 0
    assert report["tail_idle"]["samples"] == 4
    assert report["minimum_system_commit_headroom_gib"] == 10


@pytest.mark.parametrize("field,value", [("valid", False), ("scene_review", "pending")])
def test_failed_or_unreviewed_runs_cannot_produce_a_report(field, value):
    record, rows = fixture_run()
    record[field] = value
    with pytest.raises(ValueError):
        summarize(record, rows)


def test_constant_timestamps_do_not_produce_a_slope():
    record, rows = fixture_run()
    for row in rows:
        row["elapsed_seconds"] = 0
    with pytest.raises(ValueError, match="advance"):
        segment(rows, record["phases"][1]["utc"], record["phases"][2]["utc"])
