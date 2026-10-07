"""Patch only test input keys; restore without reverting unrelated settings."""

import argparse
import json
import re
from pathlib import Path

from eden_session import session_lock
from prof_common import atomic_text, eden_dir, require_no_eden, writable_target, write_json

PATCH = {
    r"player_0_connected\default": "false",
    "player_0_connected": "true",
    r"player_0_type\default": "false",
    "player_0_type": "0",
    r"player_0_profile_name\default": "false",
    "player_0_profile_name": "",
    r"tas_enable\default": "false",
    "tas_enable": "false",
    r"keyboard_enabled\default": "false",
    "keyboard_enabled": "true",
    r"player_0_button_a\default": "false",
    "player_0_button_a": '"engine:keyboard,code:88,toggle:0"',
    r"player_0_button_b\default": "false",
    "player_0_button_b": '"engine:keyboard,code:67,toggle:0"',
    r"player_0_button_plus\default": "false",
    "player_0_button_plus": '"engine:keyboard,code:77,toggle:0"',
    r"player_0_button_r\default": "false",
    "player_0_button_r": '"engine:keyboard,code:69,toggle:0"',
    r"player_0_button_l\default": "false",
    "player_0_button_l": '"engine:keyboard,code:81,toggle:0"',
    r"player_0_lstick\default": "false",
    r"player_0_rstick\default": "false",
    "player_0_rstick": (
        '"engine:analog_from_button,up:engine$0keyboard$1code$00$1toggle$00,'
        "down:engine$0keyboard$1code$00$1toggle$00,left:engine$0keyboard$1code$074$1toggle$00,"
        "right:engine$0keyboard$1code$076$1toggle$00,modifier:engine$0keyboard$1code$00$1toggle$00,"
        'modifier_scale:0.500000"'
    ),
}


def analog(up: int, down: int, left: int, right: int) -> str:
    parts = [
        f"{key}:engine$0keyboard$1code$0{code}$1toggle$00"
        for key, code in zip(("up", "down", "left", "right"), (up, down, left, right), strict=True)
    ]
    return '"engine:analog_from_button,' + ",".join(parts) + ',modifier_scale:0.500000"'


PATCH["player_0_lstick"] = analog(87, 83, 65, 68)
PATCH["player_0_rstick"] = analog(73, 75, 74, 76)


def edit_settings(text: str, values: dict[str, str | None]) -> tuple[str, dict[str, str | None]]:
    """Edit section-qualified keys, preserving other bytes and missing-key state."""
    lines = text.splitlines(keepends=True)
    newline = "\r\n" if "\r\n" in text else "\n"
    original: dict[str, str | None] = dict.fromkeys(values)
    seen = set()
    section = ""
    output = []

    def append_missing():
        for full, value in values.items():
            if full.split("/", 1)[0] == section and full not in seen and value is not None:
                output.append(f"{full.split('/', 1)[1]}={value}{newline}")
                seen.add(full)

    for line in lines:
        match = re.fullmatch(r"\[([^\]]+)\]\s*", line.strip())
        if match:
            append_missing()
            section = match[1]
        key, sep, value = line.rstrip("\r\n").partition("=")
        full = section + "/" + key
        if sep and full in values:
            if full in seen:
                raise ValueError(f"Duplicate setting: {full}")
            seen.add(full)
            original[full] = value
            if values[full] is not None:
                output.append(f"{key}={values[full]}{newline}")
        else:
            output.append(line)
    append_missing()
    for wanted in dict.fromkeys(key.split("/", 1)[0] for key in values):
        missing = {
            key: value
            for key, value in values.items()
            if key.split("/", 1)[0] == wanted and key not in seen and value is not None
        }
        if missing:
            output.append(f"{newline}[{wanted}]{newline}")
            for key, value in missing.items():
                output.append(f"{key.split('/', 1)[1]}={value}{newline}")
    return "".join(output), original


def replace_keys(text: str, values: dict[str, str]) -> tuple[str, dict[str, str]]:
    original = {}
    for key, value in values.items():
        pattern = re.compile(rf"^{re.escape(key)}=([^\r\n]*)", re.MULTILINE)
        matches = list(pattern.finditer(text))
        if len(matches) != 1:
            raise ValueError(f"Expected exactly one {key}, found {len(matches)}")
        original[key] = matches[0][1]
        text = pattern.sub(lambda _match, k=key, v=value: f"{k}={v}", text)
    return text, original


def patch_config(
    path: Path, *, restore: bool = False, settings: dict[str, str] | None = None
) -> None:
    path = writable_target(path)
    backup = path.with_name(path.name + ".autotest.json")
    with path.open(encoding="utf-8", newline="") as stream:
        text = stream.read()
    if restore:
        if not backup.is_file():
            raise FileNotFoundError(
                f"No key backup at {backup}; legacy .bak-autotest needs manual restore"
            )
        saved = json.loads(backup.read_text(encoding="utf-8"))
        if not isinstance(saved, dict) or not all(
            isinstance(v, str) or v is None for v in saved.values()
        ):
            raise ValueError("Invalid input backup")
        # Backward compatible with the original six-key unqualified backup.
        saved = {key if "/" in key else "Controls/" + key: value for key, value in saved.items()}
        restored, _ = edit_settings(text, saved)
        atomic_text(path, restored)
        backup.unlink()
        return
    settings = (
        settings
        if settings is not None
        else {"Controls/" + key: value for key, value in PATCH.items()}
    )
    patched, original = edit_settings(text, settings)
    if backup.exists():
        saved = json.loads(backup.read_text(encoding="utf-8"))
        saved = {key if "/" in key else "Controls/" + key: value for key, value in saved.items()}
        original.update(saved)
    write_json(backup, original)
    atomic_text(path, patched)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "ini", nargs="?", type=Path, default=eden_dir() / "user/config/qt-config.ini"
    )
    parser.add_argument("--restore", action="store_true")
    args = parser.parse_args(argv)
    with session_lock():
        require_no_eden()
        patch_config(args.ini, restore=args.restore)
    print("restored" if args.restore else "patched", args.ini)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
