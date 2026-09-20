"""Unit tests must never launch games, access hardware APIs or touch user data."""

import subprocess

import pytest


@pytest.fixture(autouse=True)
def isolated_environment(tmp_path, monkeypatch):
    monkeypatch.setenv("EDEN_PROF_DATA", str(tmp_path / "archive"))
    monkeypatch.setenv("EDEN_DIR", str(tmp_path / "eden"))
    monkeypatch.setenv("EDEN_NSP", str(tmp_path / "game.nsp"))

    def forbidden(*args, **kwargs):
        raise AssertionError("Unit test attempted a real subprocess; supply an explicit fake")

    monkeypatch.setattr(subprocess, "Popen", forbidden)
    monkeypatch.setattr(subprocess, "run", forbidden)
