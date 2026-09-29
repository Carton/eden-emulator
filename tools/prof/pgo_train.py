"""PGO training driver: dump pgort counters from an instrumented eden build.

Runs the game (default TOTK), posts the entry keys, rotates the camera and
dumps profile counters with pgosweep at fixed points. Eden never flushes .pgc
on exit by itself - even clean rc=0 teardown leaves nothing - so mid-run
pgosweep dumps are the only reliable way to collect training data.

Timings are calibrated for an instrumented build, which loads 2-5x slower
than the normal one; do not reuse them against a regular build.

    python pgo_train.py                       # defaults below
    python pgo_train.py --hold 300 --rotate-every 45
    EDEN_DIR=F:/.../build-pgo/bin python pgo_train.py
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

from prof_common import HERE, data_dir, eden_dir, positive_int

PGOSWEEP_DEFAULT = (
    r"D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"
    r"\14.44.35207\bin\Hostx64\x64\pgosweep.exe"
)

# vk / set-1 scancode pairs; SDL reads scancodes, so both must be correct.
KEY_A = (0x58, 0x2D)  # X = Switch A (matches patch_input.py binding)
KEY_CAM_LEFT = (0x4A, 0x24)  # J = right stick left
KEY_CAM_RIGHT = (0x4C, 0x26)  # L = right stick right


def ps_key(pid: int, vk: int, scan: int, milliseconds: int) -> bool:
    """Post one key press to the eden main window; False on any failure."""
    helpers = HERE / "window_input.ps1"
    command = (
        f". '{helpers}'; $w = Get-EdenWindow {pid}; "
        f"Send-EdenKey $w {vk} {scan} {milliseconds}; 'KEY_OK'"
    )
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
            capture_output=True,
            timeout=30,
        )
    except subprocess.TimeoutExpired:
        return False
    return b"KEY_OK" in (result.stdout or b"")


def sweep(pgosweep: Path, directory: Path, tag: str) -> bool:
    """Dump counters of every running eden.exe instance to pgo_<tag>.pgc."""
    result = subprocess.run(
        [str(pgosweep), "eden.exe", f"pgo_{tag}.pgc"],
        cwd=str(directory),
        capture_output=True,
        timeout=120,
    )
    wrote = (directory / f"pgo_{tag}.pgc").is_file()
    print(f"sweep {tag}: rc={result.returncode} wrote={wrote}", flush=True)
    return result.returncode == 0 and wrote


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--boot-sweep",
        type=positive_int,
        default=100,
        help="seconds before the boot/menu-phase sweep",
    )
    parser.add_argument(
        "--tap-at", type=positive_int, default=150, help="seconds before the first entry key"
    )
    parser.add_argument(
        "--taps",
        type=positive_int,
        default=6,
        help="entry key presses, spaced --tap-spacing seconds",
    )
    parser.add_argument("--tap-spacing", type=positive_int, default=12)
    parser.add_argument("--hold", type=positive_int, default=300, help="in-game training seconds")
    parser.add_argument(
        "--rotate-every",
        type=positive_int,
        default=45,
        help="camera rotation interval during the hold",
    )
    parser.add_argument(
        "--game-sweeps", type=positive_int, default=3, help="sweeps during the hold, evenly spaced"
    )
    parser.add_argument(
        "--pgosweep",
        type=Path,
        default=Path(PGOSWEEP_DEFAULT),
        help="pgosweep.exe path (or set EDEN_PGOSWEEP)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    directory = eden_dir()
    pgosweep = Path(os.environ.get("EDEN_PGOSWEEP", str(args.pgosweep)))
    exe = directory / "eden.exe"
    game = Path(os.environ.get("EDEN_NSP", data_dir() / "TOTK.nsp"))
    if not exe.is_file() or not game.is_file():
        print(f"missing executable or game: {exe}, {game}", file=sys.stderr)
        return 1
    if not pgosweep.is_file():
        print(f"pgosweep not found: {pgosweep} (set EDEN_PGOSWEEP)", file=sys.stderr)
        return 1
    if not (directory / "user" / "config" / "qt-config.ini").is_file():
        print("portable config required; refusing to touch the daily profile", file=sys.stderr)
        return 1

    process = subprocess.Popen([str(exe), str(game)], cwd=str(directory))
    print(f"launched pid={process.pid}", flush=True)
    origin = time.monotonic()

    def wait_until(offset: float) -> bool:
        while time.monotonic() - origin < offset:
            if process.poll() is not None:
                print(f"eden exited early rc={process.returncode}", file=sys.stderr)
                return False
            time.sleep(min(5.0, offset - (time.monotonic() - origin)))
        return True

    flips = 0
    if not wait_until(args.boot_sweep):
        return 1
    sweep(pgosweep, directory, "boot")
    if not wait_until(args.tap_at):
        return 1
    for index in range(args.taps):
        if process.poll() is not None:
            print("died during entry taps", file=sys.stderr)
            return 1
        ok = ps_key(process.pid, *KEY_A, 80)
        print(f"tap {index + 1}/{args.taps} ok={ok}", flush=True)
        time.sleep(args.tap_spacing)
    # Save load after the save list confirm; instrumented loads run minutes.
    if not wait_until(args.tap_at + args.taps * args.tap_spacing + 120):
        return 1

    hold_start = time.monotonic()
    last_rotate = hold_start
    while time.monotonic() - hold_start < args.hold:
        if process.poll() is not None:
            print("died during the hold", file=sys.stderr)
            return 1
        done = int((time.monotonic() - hold_start) * args.game_sweeps / args.hold)
        while flips < min(done, args.game_sweeps):
            flips += 1
            sweep(pgosweep, directory, f"game{flips}")
        if time.monotonic() - last_rotate > args.rotate_every:
            ps_key(process.pid, *(KEY_CAM_RIGHT if flips % 2 else KEY_CAM_LEFT), 1200)
            last_rotate = time.monotonic()
        time.sleep(5)
    sweep(pgosweep, directory, "final")

    # Close is best-effort: WM_CLOSE on a running game can hang in teardown,
    # and the hang path never reaches the CRT exit handlers anyway.
    subprocess.run(["taskkill", "/PID", str(process.pid)], capture_output=True, timeout=15)
    try:
        process.wait(timeout=120)
        print("closed gracefully", flush=True)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        print("had to kill (sweeps already captured the profile)", flush=True)
    for item in sorted(directory.glob("pgo_*.pgc")):
        print(f"PROFILE {item.name} {item.stat().st_size}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
