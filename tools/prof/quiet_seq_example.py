"""Trusted local sequence. Use bench_ab.py for interleaved performance conclusions."""

RUNS = [
    {"label": "warmup", "env": {}},
    {"label": "golden-p1", "env": {}},
    {"label": "token-p1", "env": {"EDEN_DRAW_TOKEN": "1"}},
]
PAIRS = [("golden-p1", "token-p1")]
