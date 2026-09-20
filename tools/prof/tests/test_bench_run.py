import contextlib
import json
from types import SimpleNamespace
from typing import ClassVar

import pytest
from PIL import Image

import bench_run
import shot_capture
from prof_common import TITLE, data_dir, eden_dir


@pytest.fixture
def fake_game(monkeypatch):
    clock = [0.0]
    closed = []
    options = {"csv": True, "shot": True, "input": False, "preflight": True}
    directory = eden_dir() / "user/log"
    directory.mkdir(parents=True)

    class Game:
        identity: ClassVar = {"sha256": "actual-exe", "mtime_ns": 123}
        forced = False
        pid = 1234

        def __enter__(self):
            return self

        def __exit__(self, *args):
            self.close()

        def enter_game(self, **kwargs):
            pass

        def wait(self, seconds):
            clock[0] += seconds

        def close(self):
            closed.append(True)
            if options["csv"]:
                (directory / f"now_{TITLE}.csv").write_text("25\n" * 800)

    def shot(pid, path):
        if not options["shot"]:
            return False
        path.parent.mkdir(parents=True, exist_ok=True)
        Image.new("RGB", (10, 10), (60, 60, 60)).save(path)
        return True

    def preflight():
        if not options["preflight"]:
            raise RuntimeError("bad configuration")

    monkeypatch.setattr(bench_run, "EdenSession", Game)
    monkeypatch.setattr(bench_run, "session_lock", contextlib.nullcontext)
    monkeypatch.setattr(bench_run, "preflight", preflight)
    monkeypatch.setattr(bench_run.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(bench_run, "input_tick", lambda: int(options["input"] and clock[0] > 0))
    monkeypatch.setattr(shot_capture, "save_shot", shot)
    monkeypatch.setattr(
        bench_run.subprocess, "run", lambda *args, **kwargs: SimpleNamespace(stdout="source-head\n")
    )
    return options, closed, directory


def manifests():
    return [json.loads(p.read_text()) for p in (data_dir() / "runs").glob("*/result.json")]


def test_success_has_actual_binary_and_chronological_stats(fake_game, capsys):
    assert bench_run.main(["test", "--measure", "10"]) == 0
    value = manifests()[0]
    assert value["valid"]
    assert value["fps"] == value["tail_fps"] == 40
    assert value["binary"]["sha256"] == "actual-exe"
    assert value["source_head"] == "source-head"
    assert "RESULT_JSON" in capsys.readouterr().out


@pytest.mark.parametrize("reason", ["shot", "input", "preflight", "stale"])
def test_invalid_runs_never_publish_success(fake_game, capsys, reason):
    options, closed, directory = fake_game
    if reason == "input":
        options["input"] = True
    elif reason == "stale":
        options["csv"] = False
        (directory / f"old_{TITLE}.csv").write_text("25\n" * 800)
    else:
        options[reason] = False
    assert bench_run.main(["test", "--measure", "10"]) == 1
    assert manifests()[0]["valid"] is False
    assert "RESULT_JSON" not in capsys.readouterr().out
    if reason != "preflight":
        assert closed


def test_repeated_label_uses_new_archive(fake_game):
    assert bench_run.main(["same", "--measure", "10"]) == 0
    assert bench_run.main(["same", "--measure", "10"]) == 0
    assert len(manifests()) == 2
