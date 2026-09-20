import importlib

import pytest

SCRIPTS = [
    "bench_run",
    "bench_ab",
    "check_config",
    "config_diff",
    "csv_timeline",
    "gpu_watch",
    "load_capture",
    "maximize_eden",
    "patch_input",
    "probe_windows",
    "quiet_watch",
    "rot_test",
    "shot_compare",
    "stutter_watch",
    "tailimm_ab",
    "visual_run",
    "wpr_capture",
]


@pytest.mark.parametrize("name", SCRIPTS)
def test_help_has_no_process_or_file_side_effects(name, tmp_path):
    module = importlib.import_module(name)
    with pytest.raises(SystemExit) as exit_info:
        module.main(["--help"])
    assert exit_info.value.code == 0
    assert list(tmp_path.iterdir()) == []
