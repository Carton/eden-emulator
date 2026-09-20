# Example sequence file for quiet_watch.py -- copy, edit RUNS, run:
#   python tools/prof/quiet_watch.py my_seq.py
# Entries: {"label", "env"} = one bench_run.py run with env overrides;
#          {"label", "cmd"} = arbitrary quiet-window command (e.g. a build).
# KILL_LS / PAIRS are optional; see quiet_watch.py docstring.

RUNS = [
    # warmup after reboot/cold start: data discarded per AGENTS rule
    {"label": "warmup", "env": {}},

    # ABBA interleaved pair (macro verdicts only ever come from pairs)
    {"label": "golden-p1a", "env": {}},
    {"label": "token-p1b", "env": {"EDEN_DRAW_TOKEN": "1"}},
    {"label": "token-p2b", "env": {"EDEN_DRAW_TOKEN": "1"}},
    {"label": "golden-p2a", "env": {}},
]

KILL_LS = True

PAIRS = [("golden", "token")]
