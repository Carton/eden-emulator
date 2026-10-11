from types import SimpleNamespace

import pytest

from power_guard import PowerGuard


def test_suspend_is_detected_even_when_measurement_deadline_passed(monkeypatch):
    guard = PowerGuard()
    guard.started = (100, 80)
    monkeypatch.setattr(guard, "_clocks", lambda: (1000, 90))
    with pytest.raises(RuntimeError, match="suspended.*VOID"):
        guard.check()


def test_normal_runtime_and_clock_resolution_are_allowed(monkeypatch):
    guard = PowerGuard()
    guard.started = (100, 80)
    monkeypatch.setattr(guard, "_clocks", lambda: (130, 109.98))
    guard.check()


def test_wake_request_restores_prior_thread_state():
    calls = []
    guard = PowerGuard()
    guard.previous_state = 0x80000001
    guard.kernel = SimpleNamespace(SetThreadExecutionState=lambda state: calls.append(state) or 1)
    guard.close()
    guard.close()
    assert calls == [0x80000001]
