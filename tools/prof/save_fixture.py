"""Temporarily load one intact TOTK slot into portable test slots, then restore."""

import hashlib
import shutil
from contextlib import contextmanager
from pathlib import Path

from prof_common import TITLE, require_no_eden, writable_target, write_json


@contextmanager
def temporary_slot(directory: Path, source: Path | None, backup: Path):
    if source is None:
        yield None
        return
    require_no_eden()
    source = source.resolve()
    files = sorted(source.glob("*.sav"))
    if not {"progress.sav", "caption.sav"}.issubset({path.name for path in files}):
        raise ValueError(f"Expected an intact TOTK slot directory: {source}")
    root = writable_target(directory / "user/nand/user/save")
    targets = sorted(root.glob(f"*/*/{TITLE}/slot_*"))
    if not targets:
        raise FileNotFoundError(f"No provisioned TOTK slots under {root}")
    if any(source == path.resolve() for path in targets):
        raise ValueError("Fixture source must be separate from active test slots")
    backup = writable_target(backup)
    backup.mkdir(parents=True, exist_ok=False)
    state = {
        "source": str(source),
        "targets": [str(path) for path in targets],
        "restored": False,
        "files": [
            {
                "name": path.name,
                "size": path.stat().st_size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
            for path in files
        ],
    }
    originals = []
    # Back up every destination before changing any slot.
    for slot in targets:
        for path in files:
            destination = writable_target(slot / path.name)
            saved = backup / destination.relative_to(root)
            if destination.is_file():
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(destination, saved)
            originals.append((destination, saved if destination.is_file() else None))
    write_json(backup / "manifest.json", state)
    try:
        for slot in targets:
            for path in files:
                shutil.copy2(path, writable_target(slot / path.name))
        yield state
    finally:
        require_no_eden()
        for destination, saved in originals:
            if saved is None:
                destination.unlink(missing_ok=True)
            else:
                shutil.copy2(saved, destination)
        state["restored"] = True
        write_json(backup / "manifest.json", state)
