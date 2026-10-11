"""Temporary Windows wake request and suspend detection for owned test sessions."""

from __future__ import annotations

import ctypes
import os
from ctypes import wintypes as wt


class PowerGuard:
    def __init__(self):
        self.kernel = None
        self.previous_state = None
        self.started = None

    def start(self):
        if os.name != "nt":
            return
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.SetThreadExecutionState.argtypes = [wt.DWORD]
        kernel.SetThreadExecutionState.restype = wt.DWORD
        kernel.GetTickCount64.argtypes = []
        kernel.GetTickCount64.restype = ctypes.c_ulonglong
        kernel.QueryUnbiasedInterruptTime.argtypes = [ctypes.POINTER(ctypes.c_ulonglong)]
        kernel.QueryUnbiasedInterruptTime.restype = wt.BOOL
        self.kernel = kernel
        self.started = self._clocks()
        self.previous_state = kernel.SetThreadExecutionState(0x80000003)
        if not self.previous_state:
            raise ctypes.WinError(ctypes.get_last_error())

    def _clocks(self):
        awake = ctypes.c_ulonglong()
        if not self.kernel.QueryUnbiasedInterruptTime(ctypes.byref(awake)):
            raise ctypes.WinError(ctypes.get_last_error())
        return self.kernel.GetTickCount64() / 1000, awake.value / 10_000_000

    def check(self):
        if self.started is None:
            return
        uptime, awake = self._clocks()
        suspended = (uptime - self.started[0]) - (awake - self.started[1])
        if suspended > 2:
            raise RuntimeError(f"System suspended for {suspended:.1f}s: this run is VOID")

    def close(self):
        if self.previous_state:
            # Restore this thread's prior requests, not persistent power settings.
            if not self.kernel.SetThreadExecutionState(self.previous_state | 0x80000000):
                raise ctypes.WinError(ctypes.get_last_error())
            self.previous_state = None
