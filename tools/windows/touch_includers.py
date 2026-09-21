"""Touch every .cpp TU that (transitively) includes any given header.

build-vs22's ninja has no header dependency tracking (no msvc_deps_prefix;
cl emits GBK-localized /showIncludes ninja cannot parse), so editing a header
alone yields "no work to do" while includers keep the stale layout -> silent
ODR/ABI mismatch. Run this after any header edit, then rebuild and verify
eden.exe's mtime actually moved.

Why a python tool and not `while read f; do touch ...`: a bash loop over a
CRLF-terminated list leaves a trailing \\r on each path, and MSYS touch then
creates garbage files named like the absolute path with private-use
characters (U+F03A for ':' etc.) instead of touching the real file - exactly
the 20-file litter cleaned up on 2026-09-21.

Usage: python tools/windows/touch_includers.py HEADER [HEADER...]
       (paths relative to repo root or absolute; basenames are matched)
"""

import argparse
import collections
import os
import re
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
INC_RE = re.compile(r'#\s*include\s+[<"]([^">]+)[">]')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("headers", nargs="+")
    args = parser.parse_args(argv)
    targets = {os.path.basename(h) for h in args.headers}

    includers = collections.defaultdict(set)
    all_files = []
    for sub in ("src", "externals"):
        root = os.path.join(REPO, sub)
        if not os.path.isdir(root):
            continue
        for dirpath, _, files in os.walk(root):
            for f in files:
                if not f.endswith((".h", ".cpp", ".hpp")):
                    continue
                p = os.path.join(dirpath, f)
                all_files.append(p)
                try:
                    text = open(p, encoding="utf-8", errors="ignore").read()
                except OSError:
                    continue
                for m in INC_RE.finditer(text):
                    includers[os.path.basename(m.group(1))].add(p)

    seen, queue, cpps = set(), collections.deque(targets), []
    while queue:
        hdr = queue.popleft()
        for p in includers.get(hdr, ()):
            if p in seen:
                continue
            seen.add(p)
            if p.endswith(".cpp"):
                cpps.append(p)
            else:
                queue.append(os.path.basename(p))

    for p in sorted(set(cpps)):
        os.utime(p)
    print(f"touched {len(set(cpps))} TUs (closure of {sorted(targets)})")
    for p in sorted(set(cpps)):
        print(" ", os.path.relpath(p, REPO))
    return 0


if __name__ == "__main__":
    sys.exit(main())
