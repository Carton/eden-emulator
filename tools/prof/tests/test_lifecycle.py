import subprocess
from types import SimpleNamespace

import pytest

import eden_session
from eden_session import EdenSession, session_lock
from stutter_watch import Detector
from wpr_capture import Wpr


def test_session_cleanup_targets_only_owned_pid(monkeypatch):
    calls = []

    class Process:
        pid = 1234

        def poll(self):
            return None

        def wait(self, timeout):
            calls.append(("wait", timeout))
            if timeout == 60:
                raise subprocess.TimeoutExpired("eden", timeout)

        def kill(self):
            calls.append(("kill", self.pid))

    monkeypatch.setattr(eden_session.subprocess, "run", lambda command, **kw: calls.append(command))
    game = EdenSession()
    game.process = Process()
    game.close()
    assert calls == [["taskkill", "/PID", "1234"], ("wait", 60), ("kill", 1234), ("wait", 15)]
    assert game.forced


def test_lock_is_exclusive_and_released_on_exception():
    with pytest.raises(RuntimeError, match="test error"), session_lock():
        with pytest.raises(RuntimeError, match="Automation"), session_lock():
            pytest.fail("Acquired duplicate lock")
        raise RuntimeError("test error")
    with session_lock():
        pass


def test_input_during_wait_invalidates_run(monkeypatch):
    marks = iter([100, 101])
    monkeypatch.setattr(eden_session, "input_tick", lambda: next(marks))
    game = EdenSession()
    game.process = SimpleNamespace(poll=lambda: None)
    with pytest.raises(RuntimeError, match="VOID"):
        game.wait(10)


def test_wpr_start_failure_does_not_cancel_foreign_session(tmp_path, monkeypatch):
    wpr = Wpr(tmp_path, ["CPU"])
    calls = []

    def run(*args):
        calls.append(args)
        return SimpleNamespace(returncode=1, stderr="already running")

    monkeypatch.setattr(wpr, "_run", run)
    with pytest.raises(RuntimeError, match="existing sessions"):
        wpr.start()
    wpr.cancel()
    assert calls == [("-start", "CPU")]


def test_wpr_stop_requires_actual_trace(tmp_path, monkeypatch):
    wpr = Wpr(tmp_path, ["CPU"])
    monkeypatch.setattr(wpr, "_run", lambda *args: SimpleNamespace(returncode=0, stderr=""))
    wpr.start()
    with pytest.raises(RuntimeError, match="without trace"):
        wpr.stop(tmp_path / "missing.etl")
    assert not wpr.active


def test_wpr_failed_stop_preserves_ownership_for_cleanup(tmp_path, monkeypatch):
    wpr = Wpr(tmp_path, ["CPU"])
    wpr.active = True
    calls = []

    def run(*args):
        calls.append(args)
        return SimpleNamespace(returncode=int(args[0] == "-stop"), stderr="failure")

    monkeypatch.setattr(wpr, "_run", run)
    with pytest.raises(RuntimeError, match="stop failed"):
        wpr.stop(tmp_path / "trace.etl")
    wpr.cancel()
    assert calls[-1] == ("-cancel",)
    assert not wpr.active


def test_detector_quota_busy_and_nonfinite():
    detector = Detector(50, 1.6, 0, 3, 1)
    for at in range(3):
        assert detector.observe(25, at) is None
    assert detector.observe(float("inf"), 4) is None
    assert detector.observe(100, 5, busy=True) is None
    context = detector.observe(100, 6)
    assert context is not None
    snapshot = context["recent_ms"].copy()
    assert detector.observe(300, 7) is None
    assert context["recent_ms"] == snapshot
    assert detector.captures == 1


def test_detector_history_and_cooldown():
    detector = Detector(50, 1.6, 5, 200, 3)
    for at in range(200):
        assert detector.observe(25, at) is None
    assert detector.observe(100, 201) is not None
    assert detector.observe(100, 202) is None
    assert detector.observe(100, 207) is not None
