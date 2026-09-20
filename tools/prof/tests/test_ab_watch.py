import json
from types import SimpleNamespace

import pytest

import bench_ab
import quiet_watch


def result(fps=40):
    return {"fps": fps, "luma": 60.5, "luma_std": 0.2}


def test_whole_pair_retry_and_ba_order():
    calls = []
    answers = iter([result(), None, result(), result(44), result(44), result()])

    def run(label, env, measure):
        calls.append(label)
        return next(answers)

    assert bench_ab.collect_pairs("a", "b", {}, {}, 2, 1, 90, run) == [1.1, 1.1]
    assert calls == ["a-p1ar0", "b-p1br0", "a-p1ar1", "b-p1br1", "b-p2br0", "a-p2ar0"]


@pytest.mark.parametrize(
    "rc,label,valid,fps",
    [(1, "a", True, 40), (0, "b", True, 40), (0, "a", False, 40), (0, "a", True, float("nan"))],
)
def test_result_requires_exit_status_matching_label_and_valid_number(rc, label, valid, fps):
    output = "RESULT_JSON " + json.dumps({"label": label, "valid": valid, "fps": fps})
    assert bench_ab.parse_result(output, rc, "a") is None


def test_luma_invalidates_pair():
    with pytest.raises(ValueError):
        bench_ab.pair_ratio(result(), dict(result(), luma=65))


def test_failed_build_stops_dependent_bench(monkeypatch):
    monkeypatch.setattr(quiet_watch, "wait_idle", lambda *args: True)
    monkeypatch.setattr(
        quiet_watch.subprocess, "run", lambda *args, **kwargs: SimpleNamespace(returncode=1)
    )
    monkeypatch.setattr(quiet_watch, "run_one", lambda *args: pytest.fail("Ran after failed build"))
    assert (
        quiet_watch.execute(
            [{"label": "build", "cmd": ["fake"]}, {"label": "bench"}], [], 60, 1, 90
        )
        == 1
    )


def test_idle_deadline_never_runs_command(monkeypatch):
    monkeypatch.setattr(quiet_watch, "wait_idle", lambda *args: False)
    assert quiet_watch.execute([{"label": "build", "cmd": ["fake"]}], [], 60, 1, 90) == 1
