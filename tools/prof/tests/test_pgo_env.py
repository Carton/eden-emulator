"""Regression tests for the PGO-related env switches in shared helpers."""

from __future__ import annotations

import importlib
from pathlib import Path

import prof_common


def test_experiment_env_keeps_pgo_switches(monkeypatch) -> None:
    monkeypatch.setenv("EDEN_IGNORE_INPUT", "1")
    monkeypatch.setenv("EDEN_PROF_REPO", "F:/somewhere/eden")
    monkeypatch.setenv("EDEN_TOKEN_TAIL_PIPELINE", "1")
    env = prof_common.experiment_env({})
    assert env["EDEN_IGNORE_INPUT"] == "1"
    assert env["EDEN_PROF_REPO"] == "F:/somewhere/eden"
    assert "EDEN_TOKEN_TAIL_PIPELINE" not in env


def test_experiment_env_override_wins(monkeypatch) -> None:
    monkeypatch.setenv("EDEN_IGNORE_INPUT", "1")
    env = prof_common.experiment_env({"EDEN_IGNORE_INPUT": ""})
    assert env["EDEN_IGNORE_INPUT"] == ""


def test_repo_env_override(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("EDEN_PROF_REPO", str(tmp_path))
    try:
        module = importlib.reload(prof_common)
        # No resolve(): the override is used verbatim, and Windows temp paths
        # may hold a short-name component that resolve() would expand.
        assert tmp_path == module.REPO
    finally:
        monkeypatch.delenv("EDEN_PROF_REPO")
        importlib.reload(prof_common)
    default_root = Path(prof_common.__file__).resolve().parents[2]
    assert default_root == prof_common.REPO


def test_input_tick_bypass(monkeypatch) -> None:
    import eden_session

    monkeypatch.setenv("EDEN_IGNORE_INPUT", "1")
    assert eden_session.input_tick() == 0
    monkeypatch.delenv("EDEN_IGNORE_INPUT")
    assert isinstance(eden_session.input_tick(), int)
