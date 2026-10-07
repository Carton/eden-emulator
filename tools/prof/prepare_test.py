"""Prepare and verify portable test paths and keyboard-backed virtual controller."""

import argparse
from pathlib import Path

from config_diff import parse
from patch_input import PATCH, patch_config
from prof_common import TITLE, eden_dir, game_path, require_no_eden, writable_target


def prepare(directory: Path, *, restore: bool = False) -> list[str]:
    directory = writable_target(directory)
    require_no_eden()
    ini = directory / "user/config/qt-config.ini"
    custom = directory / f"user/config/custom/{TITLE}.ini"
    if restore:
        for path in (ini, custom):
            if path.with_name(path.name + ".autotest.json").exists():
                patch_config(path, restore=True)
        return ["Restored test configuration"]
    if not (directory / "eden.exe").is_file() or not ini.is_file() or not game_path().is_file():
        raise FileNotFoundError(
            f"Need eden.exe, portable qt-config.ini and game: {directory}, {game_path()}"
        )
    settings = {"Controls/" + key: value for key, value in PATCH.items()}
    for key, folder in (
        ("nand_directory", "nand"),
        ("sdmc_directory", "sdmc"),
        ("load_directory", "load"),
        ("dump_directory", "dump"),
    ):
        target = writable_target(directory / "user" / folder)
        if folder == "nand" and not target.is_dir():
            raise FileNotFoundError(f"Portable NAND missing: {target}; provision test data first")
        settings[f"Data%20Storage/{key}"] = target.as_posix()
        settings[f"Data%20Storage/{key}\\default"] = "false"
    settings.update(
        {
            "Debugging/record_frame_times": "true",
            "UI/confirmStop": "2",
            r"UI/confirmStop\default": "false",
        }
    )
    patch_config(ini, settings=settings)
    if custom.is_file():
        patch_config(
            custom,
            settings={
                "Controls/player_0_profile_name": "",
                r"Controls/player_0_profile_name\default": "false",
            },
        )
    values = parse(ini)
    failures = [key for key, expected in settings.items() if values.get(key) != expected]
    if failures:
        raise RuntimeError(f"Preparation readback failed: {failures}")
    return [f"Prepared {ini}", "Virtual controller: A=X, B=C, movement=WASD, camera=IJKL"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--restore", action="store_true")
    args = parser.parse_args(argv)
    from eden_session import session_lock

    with session_lock():
        for message in prepare(eden_dir(), restore=args.restore):
            print(message)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
