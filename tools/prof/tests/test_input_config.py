import pytest

from check_config import MUST_MATCH, validate
from patch_input import PATCH, patch_config, replace_keys


def config_text():
    return (
        "[Controls]\r\n"
        + "".join(f"{key}=old-{i}\r\n" for i, key in enumerate(PATCH))
        + "other=1\r\n"
    )


def test_patch_restore_preserves_other_settings_and_line_endings(tmp_path):
    path = tmp_path / "qt.ini"
    original = config_text()
    path.write_bytes(original.encode())
    patch_config(path)
    assert b"code:88" in path.read_bytes()
    patch_config(path)  # idempotent: must not replace original backup
    path.write_bytes(path.read_bytes().replace(b"other=1", b"other=2"))
    patch_config(path, restore=True)
    assert path.read_bytes() == original.replace("other=1", "other=2").encode()
    assert not path.with_name("qt.ini.autotest.json").exists()


def test_restore_missing_backup_does_not_patch(tmp_path):
    path = tmp_path / "qt.ini"
    path.write_bytes(config_text().encode())
    with pytest.raises(FileNotFoundError):
        patch_config(path, restore=True)
    assert path.read_bytes() == config_text().encode()


@pytest.mark.parametrize("text", ["no keys", config_text() + "keyboard_enabled=true\n"])
def test_missing_or_duplicate_key_cannot_partially_patch(text):
    with pytest.raises(ValueError):
        replace_keys(text, PATCH)


def test_key_match_does_not_modify_prefixed_name():
    text = config_text() + "other_keyboard_enabled=keep\r\n"
    patched, _ = replace_keys(text, PATCH)
    assert "other_keyboard_enabled=keep\r\n" in patched


def baseline():
    values = {f"Section/{key}": value for key, value in MUST_MATCH.items()}
    values.update(
        {f"Section/{key}\\default": "false" for key in MUST_MATCH if key != "record_frame_times"}
    )
    values.update(
        {
            f"Controls/{key}": value
            for key, value in PATCH.items()
            if key.split("\\")[0] not in MUST_MATCH
        }
    )
    return values


def test_default_flags_and_a_mapping():
    values = baseline()
    assert validate(values) == []
    values[r"Section/record_frame_times\default"] = "true"
    assert validate(values) == []  # This setting is deliberately read without the default flag.
    del values[r"Section/record_frame_times\default"]
    values["Controls/player_0_button_a"] = "engine:keyboard,code:99"
    assert any("button_a" in fail for fail in validate(values))


def test_missing_default_flag_means_compiled_default():
    values = baseline()
    del values[r"Section/cpu_accuracy\default"]
    assert any("cpu_accuracy" in fail for fail in validate(values))


def test_ambiguous_setting_rejected():
    values = baseline()
    values["Other/cpu_accuracy"] = "2"
    assert any("Ambiguous" in fail for fail in validate(values))
