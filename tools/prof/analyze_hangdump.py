#!/usr/bin/env python3
"""Analyze an eden.exe minidump taken during a hang (no exception record):
per-thread RIP + eden return addresses found by scanning each thread's stack
(raw stack scan, no unwind), symbolized through sym.exe when available.

Usage: python tools/prof/analyze_hangdump.py <dump.dmp> [eden.exe path]
Dump capture (no tools needed, same-user process):
  rundll32.exe C:\\Windows\\System32\\comsvcs.dll,MiniDump <pid> out.dmp full
"""
import os
import struct
import subprocess
import sys

from minidump.minidumpfile import MinidumpFile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DEFAULT_EXE = os.path.join(REPO, "build-vs22", "bin", "eden.exe")
SYM = r"F:\prof\sym.exe"  # built from tools/prof/sym.cpp; rebuild if stale


def sym_line(exe, rva):
    if not os.path.exists(SYM):
        return ""
    try:
        r = subprocess.run([SYM, exe, f"{rva:x}"], capture_output=True,
                           text=True, timeout=20)
        out = (r.stdout or "").strip().replace("\n", " | ")
        return "  " + out if out else ""
    except Exception:
        return ""


def main():
    path = sys.argv[1]
    exe = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_EXE
    mf = MinidumpFile.parse(path)
    mods = [(m.name, m.baseaddress, m.size) for m in mf.modules.modules]
    eden = next(((b, s) for n, b, s in mods if n.lower().endswith("eden.exe")), None)
    if not eden:
        print("eden.exe module not found in dump")
        return 1
    eb, es = eden
    print(f"eden.exe base=0x{eb:X} size=0x{es:X}  ({len(mods)} modules, "
          f"{len(mf.threads.threads)} threads)")
    reader = mf.get_reader()
    for th in mf.threads.threads:
        rip = 0
        try:
            ctx = th.ContextObject
            rip = getattr(ctx, "Rip", 0) or 0
            if not rip and ctx is not None:
                raw = getattr(ctx, "raw_data", None)
                if raw and len(raw) > 0x100:  # AMD64 CONTEXT: Rip at +0xF8
                    rip = struct.unpack_from("<Q", raw, 0xF8)[0]
        except Exception:
            pass
        st = th.Stack
        hits = []
        try:
            data = reader.read(st.StartOfMemoryRange, st.DataSize)
            for off in range(0, len(data) - 8, 8):
                v = struct.unpack_from("<Q", data, off)[0]
                if eb <= v < eb + es:
                    hits.append(v - eb)
        except Exception:
            pass
        loc = ""
        if rip:
            loc = (f" eden+0x{rip - eb:X}" if eb <= rip < eb + es
                   else f" rip=0x{rip:X}")
        print(f"--- tid {th.ThreadId}{loc} hits={len(hits)}")
        for r in hits[:10]:
            print(f"    eden+0x{r:X}{sym_line(exe, r)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
