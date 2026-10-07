import pytest

import save_fixture
from prof_common import TITLE
from save_fixture import temporary_slot


def test_fixture_restores_exact_files_on_failure(tmp_path, monkeypatch):
    monkeypatch.setattr(save_fixture, "require_no_eden", lambda: None)
    directory = tmp_path / "eden"
    slot = directory / f"user/nand/user/save/0000/user/{TITLE}/slot_00"
    slot.mkdir(parents=True)
    (slot / "progress.sav").write_bytes(b"original progress")
    (slot / "unrelated").write_bytes(b"keep")
    source = tmp_path / "snapshot"
    source.mkdir()
    (source / "progress.sav").write_bytes(b"fixture progress")
    (source / "caption.sav").write_bytes(b"fixture caption")
    with (
        pytest.raises(RuntimeError, match="failed"),
        temporary_slot(directory, source, tmp_path / "backup") as state,
    ):
        assert (slot / "progress.sav").read_bytes() == b"fixture progress"
        raise RuntimeError("failed")
    assert state["restored"]
    assert (slot / "progress.sav").read_bytes() == b"original progress"
    assert not (slot / "caption.sav").exists()
    assert (slot / "unrelated").read_bytes() == b"keep"


def test_missing_fixture_is_rejected_before_any_slot_change(tmp_path, monkeypatch):
    monkeypatch.setattr(save_fixture, "require_no_eden", lambda: None)
    with (
        pytest.raises(ValueError, match="intact"),
        temporary_slot(tmp_path, tmp_path / "missing", tmp_path / "backup"),
    ):
        pytest.fail("Invalid fixture accepted")
