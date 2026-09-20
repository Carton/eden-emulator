"""Compare two INI files using section-qualified, case-sensitive keys."""

import argparse
import configparser
from pathlib import Path


def parse(path: Path) -> dict[str, str]:
    config = configparser.ConfigParser(interpolation=None, strict=True)
    config.optionxform = str
    with path.open(encoding="utf-8-sig") as stream:
        config.read_file(stream)
    return {
        f"{section}/{key}": value
        for section in config.sections()
        for key, value in config.items(section)
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    args = parser.parse_args(argv)
    a, b = parse(args.a), parse(args.b)
    for key in sorted(a.keys() | b.keys()):
        if a.get(key) != b.get(key):
            print(f"{key}: A={a.get(key)!r} B={b.get(key)!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
