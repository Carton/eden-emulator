import json
import subprocess
from datetime import UTC, datetime, timedelta
from types import SimpleNamespace

import pytest
from PIL import Image

import memory_rot_run as runner
from memory_rot_run import gameplay_hud, phase_summary
from patch_input import patch_config
from prof_common import read_local_config


def test_new_controller_keys_restore_absence_and_legacy_backup(tmp_path):
    path = tmp_path / "qt.ini"
    path.write_text('[Controls]\nplayer_0_button_a="old"\nother=keep\n')
    backup = path.with_name(path.name + ".autotest.json")
    backup.write_text(json.dumps({"player_0_button_a": '"before-original-patch"'}))
    patch_config(path)
    assert "player_0_lstick=" in path.read_text()
    patch_config(path)
    patch_config(path, restore=True)
    assert path.read_text() == '[Controls]\nplayer_0_button_a="before-original-patch"\nother=keep\n'


def test_profile_json_paths_do_not_depend_on_working_directory(tmp_path, monkeypatch):
    config = tmp_path / "prof.local.json"
    config.write_text(json.dumps({"eden_dir": "build-other/bin", "data_dir": "data"}))
    monkeypatch.setenv("EDEN_PROF_CONFIG", str(config))
    monkeypatch.chdir(tmp_path.parent)
    base, values = read_local_config()
    assert base == tmp_path.resolve()
    assert (base / values["eden_dir"]).resolve() == tmp_path / "build-other/bin"


def test_growth_is_measured_only_in_requested_phase():
    start = datetime(2026, 10, 7, tzinfo=UTC)
    rows = [
        {
            "utc": (start + timedelta(seconds=i * 10)).isoformat(),
            "elapsed_seconds": i * 10,
            "private_bytes": (100 + i * 10) * 1048576,
        }
        for i in range(10)
    ]
    phases = [
        {"phase": "rotation", "utc": (start + timedelta(seconds=30)).isoformat()},
        {"phase": "tail", "utc": (start + timedelta(seconds=90)).isoformat()},
    ]
    result = phase_summary(rows, phases)[0]
    assert result["samples"] == 6
    assert result["delta_mib"] == 50
    assert result["slope_mib_per_min"] == 60


def test_menu_animation_and_black_screen_are_not_gameplay(tmp_path):
    path = tmp_path / "screen.png"
    for color in ("black", (80, 70, 40), "white"):
        Image.new("RGB", (640, 360), color).save(path)
        assert not gameplay_hud(path)


def test_failed_entry_cleans_owned_sampler_and_keeps_phase_record(tmp_path, monkeypatch):
    closed = []

    class Game:
        pid = 123
        forced = False

        def __init__(self):
            self.identity = {"exe": "test.exe"}

        def __enter__(self):
            return self

        def wait(self, _seconds):
            raise RuntimeError("entry failed")

        def __exit__(self, *args):
            closed.append("game")

    class Watcher:
        def poll(self):
            return None

        def wait(self, timeout):
            if "sampler" not in closed:
                raise subprocess.TimeoutExpired("watcher", timeout)

        def terminate(self):
            closed.append("sampler")

    monkeypatch.setattr(runner, "preflight", lambda **kw: None)
    monkeypatch.setattr(runner, "EdenSession", Game)
    monkeypatch.setattr(runner.subprocess, "run", lambda *a, **kw: SimpleNamespace(stdout="head"))
    monkeypatch.setattr(runner.subprocess, "Popen", lambda *a, **kw: Watcher())
    output = tmp_path / "failed-run"
    with pytest.raises(RuntimeError, match="entry failed"):
        runner.main(["--output", str(output)])
    result = json.loads((output / "result.json").read_text())
    assert closed == ["game", "sampler"]
    assert not result["valid"]
    assert result["phases"][0]["phase"] == "boot"
    assert result["sampler_forced_cleanup"]


def test_short_sampler_duration_fails_before_launch():
    with pytest.raises(SystemExit):
        runner.main(["--watch-seconds", "1"])
