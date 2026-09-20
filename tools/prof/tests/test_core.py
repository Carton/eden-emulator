import argparse
import math
from pathlib import Path

import pytest

from prof_common import (
    TITLE,
    atomic_text,
    eden_dir,
    experiment_env,
    frame_summary,
    fresh_csv,
    label_value,
    natural_key,
    parse_overrides,
    positive_float,
    read_frames,
    snapshot_csv,
    tail_window,
    writable_target,
)


@pytest.mark.parametrize(
    "label", ["..", ".", "../x", "a/b", "a\\b", "NUL", "con.txt", "x.", "", "a:b"]
)
def test_invalid_label(label):
    with pytest.raises(argparse.ArgumentTypeError):
        label_value(label)


def test_label_and_natural_order():
    assert label_value("golden-p1.a") == "golden-p1.a"
    assert sorted(map(Path, ["t+100s.png", "t+90s.png"]), key=natural_key)[0].name == "t+90s.png"


@pytest.mark.parametrize("value", ["nan", "inf", "-1", "0"])
def test_invalid_duration(value):
    with pytest.raises(argparse.ArgumentTypeError):
        positive_float(value)


def test_tail_is_chronological_and_latest():
    values = [20.0] * 100 + [40.0] * 100
    window = tail_window(values, 5)
    assert window == [20.0] * 50 + [40.0] * 100
    assert frame_summary(tail_window(window, 2))["fps"] == 25
    with pytest.raises(ValueError, match="coverage"):
        tail_window(values, 7)


@pytest.mark.parametrize("bad", ["", "nan", "inf", "0", "-1", "20 nope"])
def test_bad_csv(tmp_path, bad):
    path = tmp_path / "bad.csv"
    path.write_text(bad)
    with pytest.raises(ValueError):
        read_frames(path)


def test_fresh_csv_rejects_stale_and_ambiguous(tmp_path):
    first = tmp_path / f"a_{TITLE}.csv"
    first.write_text("25\n")
    before = snapshot_csv(tmp_path)
    with pytest.raises(ValueError):
        fresh_csv(tmp_path, before)
    first.write_text("25\n25\n")
    assert fresh_csv(tmp_path, before) == first
    (tmp_path / f"b_{TITLE}.csv").write_text("25\n")
    with pytest.raises(ValueError):
        fresh_csv(tmp_path, before)


def test_environment_isolates_experiments(monkeypatch):
    monkeypatch.setenv("EDEN_TOKEN_TAIL_IMM", "1")
    monkeypatch.setenv("EDEN_DRAW_TOKEN", "1")
    result = experiment_env({"EDEN_DRAW_TOKEN": "0"})
    assert "EDEN_TOKEN_TAIL_IMM" not in result
    assert result["EDEN_DRAW_TOKEN"] == "0"
    assert result["EDEN_DIR"] == str(eden_dir())
    assert parse_overrides(" K=x=y, A=0 ") == {"K": "x=y", "A": "0 "}
    with pytest.raises(argparse.ArgumentTypeError):
        parse_overrides("EDEN_TOKEN")


def test_atomic_write_preserves_bytes_and_cleans_temp(tmp_path):
    target = tmp_path / "a.ini"
    atomic_text(target, "x=1\r\n")
    assert target.read_bytes() == b"x=1\r\n"
    assert list(tmp_path.iterdir()) == [target]


def test_daily_install_is_protected():
    with pytest.raises(ValueError, match="read-only"):
        writable_target(Path(r"F:\Switch\Yuzu") / "user/config/qt-config.ini")


def test_statistics_reject_nonfinite():
    with pytest.raises(ValueError):
        frame_summary([math.inf] * 100)
