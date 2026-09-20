"""Ownership of one GUI Eden process; never terminate by image name."""

from __future__ import annotations

import ctypes
import hashlib
import os
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path

from prof_common import HERE, data_dir, eden_dir, require_no_eden, writable_target


def input_tick() -> int:
    class LastInput(ctypes.Structure):
        _fields_ = [("size", ctypes.c_uint32), ("tick", ctypes.c_uint32)]

    info = LastInput(ctypes.sizeof(LastInput), 0)
    if not ctypes.windll.user32.GetLastInputInfo(ctypes.byref(info)):
        raise ctypes.WinError()
    return info.tick


def idle_seconds() -> float:
    # LASTINPUTINFO is a 32-bit tick even on 64-bit Windows.
    return ((ctypes.windll.kernel32.GetTickCount() - input_tick()) & 0xFFFFFFFF) / 1000


def preflight() -> None:
    subprocess.run([sys.executable, str(HERE / "check_config.py")], check=True, timeout=30)


@contextmanager
def session_lock():
    """One automation session per archive; stale locks require explicit inspection."""
    directory = writable_target(data_dir())
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "automation.lock"
    try:
        stream = path.open("x", encoding="utf-8")
    except FileExistsError as exc:
        raise RuntimeError(f"Automation already active or stale lock: {path}") from exc
    try:
        with stream:
            stream.write(str(os.getpid()))
        yield
    finally:
        path.unlink(missing_ok=True)


class EdenSession:
    def __init__(self, env: dict[str, str] | None = None):
        self.directory = writable_target(eden_dir())
        self.env = env
        self.process: subprocess.Popen | None = None
        self.forced = False
        self.identity: dict[str, str | int] = {}

    @property
    def pid(self) -> int:
        if self.process is None:
            raise RuntimeError("Session has not started")
        return self.process.pid

    def __enter__(self):
        require_no_eden()
        exe = self.directory / "eden.exe"
        game = Path(os.environ.get("EDEN_NSP", data_dir() / "TOTK.nsp"))
        if not exe.is_file() or not game.is_file():
            raise FileNotFoundError(f"Missing executable or game: {exe}, {game}")
        if not (self.directory / "user" / "config" / "qt-config.ini").is_file():
            raise RuntimeError("Portable config required; refusing to use the daily profile")
        with exe.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        self.identity = {"exe": str(exe), "sha256": digest, "mtime_ns": exe.stat().st_mtime_ns}
        self.process = subprocess.Popen([str(exe), str(game)], cwd=self.directory, env=self.env)
        return self

    def wait(self, seconds: float, *, quiet: bool = True) -> None:
        if self.process is None:
            raise RuntimeError("Session has not started")
        mark = input_tick() if quiet else None
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(f"Eden exited unexpectedly ({self.process.returncode})")
            if quiet and input_tick() != mark:
                raise RuntimeError("User input detected: this run is VOID")
            time.sleep(min(0.25, max(0, deadline - time.monotonic())))

    def tap_a(self) -> None:
        result = subprocess.run(
            [
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(HERE / "focus_test.ps1"),
                "-ProcessId",
                str(self.pid),
            ],
            capture_output=True,
            text=True,
            timeout=15,
        )
        if result.returncode or "input_ok=True" not in result.stdout:
            raise RuntimeError(
                f"Failed to post A to PID {self.pid}: {result.stdout} {result.stderr}"
            )

    def enter_game(self, *, no_tap: bool = False) -> None:
        self.wait(50)
        if not no_tap:
            for _ in range(3):
                self.tap_a()
                self.wait(8)
        self.wait(30)

    def close(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        try:
            subprocess.run(["taskkill", "/PID", str(self.pid)], capture_output=True, timeout=15)
            self.process.wait(timeout=60)
        except (OSError, subprocess.SubprocessError):
            self.forced = True
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=15)

    def __exit__(self, exc_type, exc, traceback):
        self.close()
