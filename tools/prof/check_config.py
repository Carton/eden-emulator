"""Validate the pond profiling configuration. NVIDIA Overlay only warns."""

import argparse
import subprocess
import time
from pathlib import Path

from config_diff import parse
from prof_common import eden_dir, process_names

MUST_MATCH = {
    "cpu_accuracy": "2",
    "accelerate_astc": "2",
    "astc_recompression": "2",
    "use_asynchronous_shaders": "true",
    "keyboard_enabled": "true",
    "record_frame_times": "true",
    "confirmStop": "2",
    "scaling_filter": "6",
    "anti_aliasing": "1",
    "gpu_accuracy": "0",
}
MUST_DEFAULT = {
    "resolution_setup",
    "max_anisotropy",
    "use_vsync",
    "async_presentation",
    "use_reactive_flushing",
    "vram_usage_mode",
    "cpu_backend",
    "shader_backend",
}


def validate(values: dict[str, str], *, input_only: bool = False) -> list[str]:
    failures = []

    def get(key):
        matches = [value for full, value in values.items() if full.split("/", 1)[-1] == key]
        if len(matches) > 1:
            failures.append(f"Ambiguous setting across sections: {key}")
        return matches[0] if len(matches) == 1 else None

    for key, expected in MUST_MATCH.items():
        if input_only and key not in {"keyboard_enabled", "confirmStop", "record_frame_times"}:
            continue
        value, flag = get(key), get(key + r"\default")
        if value != expected:
            failures.append(f"{key}={value!r}; want {expected!r}")
        # ReadSettingGeneric defaults a missing flag to true. record_frame_times
        # is explicitly read outside that mechanism in ReadDebuggingValues.
        if key != "record_frame_times" and flag != "false":
            failures.append(f"{key}: default flag ignores required value")
    for key in MUST_DEFAULT:
        if input_only:
            continue
        if get(key) is not None and get(key + r"\default") != "true":
            failures.append(f"{key}: must use compiled default")
    from patch_input import PATCH

    for key, expected in PATCH.items():
        if get(key) != expected:
            failures.append(f"{key}: test controller setting is missing or ignored")
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ini", type=Path, default=eden_dir() / "user/config/qt-config.ini")
    parser.add_argument("--tolerate-overlay", action="store_true", help="Compatibility no-op")
    parser.add_argument(
        "--input-only", action="store_true", help="Skip historical performance preset"
    )
    args = parser.parse_args(argv)
    failures = validate(parse(args.ini), input_only=args.input_only)
    custom = args.ini.parent / "custom/0100F2C0115B6000.ini"
    if custom.is_file():
        values = parse(custom)
        if (
            values.get("Controls/player_0_profile_name")
            and values.get(r"Controls/player_0_profile_name\default") == "false"
        ):
            failures.append("TOTK custom input profile overrides test controller")
    processes = process_names()
    if "losslessscaling.exe" in processes:
        # User rule (2026-09-20): terminate on sight during profiling preflight,
        # no confirmation. It composites the presented frames and corrupts
        # timing/frame capture; auto-killing restores the old watchdog behavior.
        subprocess.run(
            ["taskkill", "/F", "/IM", "LosslessScaling.exe"],
            capture_output=True,
            text=True,
            timeout=20,
        )
        time.sleep(2)
        if "losslessscaling.exe" in process_names():
            failures.append("LosslessScaling survived taskkill")
        else:
            print("WARN LosslessScaling was terminated by preflight (user rule 2026-09-20)")
    if "nvidia overlay.exe" in processes:
        print("WARN NVIDIA Overlay is running (allowed)")
    for failure in failures:
        print("FAIL", failure)
    if not failures:
        print("ALL PASS")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
