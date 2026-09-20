"""Patch only test input keys; restore without reverting unrelated settings."""

import argparse
import json
import re
from pathlib import Path

from eden_session import session_lock
from prof_common import atomic_text, eden_dir, require_no_eden, writable_target, write_json

PATCH = {
    r"keyboard_enabled\default": "false",
    "keyboard_enabled": "true",
    r"player_0_button_a\default": "false",
    "player_0_button_a": '"engine:keyboard,code:88,toggle:0"',
    r"player_0_rstick\default": "false",
    "player_0_rstick": (
        '"engine:analog_from_button,up:engine$0keyboard$1code$00$1toggle$00,'
        "down:engine$0keyboard$1code$00$1toggle$00,left:engine$0keyboard$1code$074$1toggle$00,"
        "right:engine$0keyboard$1code$076$1toggle$00,modifier:engine$0keyboard$1code$00$1toggle$00,"
        'modifier_scale:0.500000"'
    ),
}


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


def patch_config(path: Path, *, restore: bool = False) -> None:
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
        if set(saved) != set(PATCH) or not all(isinstance(v, str) for v in saved.values()):
            raise ValueError("Invalid input backup")
        restored, _ = replace_keys(text, saved)
        atomic_text(path, restored)
        backup.unlink()
        return
    patched, original = replace_keys(text, PATCH)
    if not backup.exists():
        if all(original[key] == value for key, value in PATCH.items()):
            raise ValueError("Already patched without a key backup; inspect legacy backup first")
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
